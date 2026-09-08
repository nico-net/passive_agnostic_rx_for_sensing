/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#include "detector.h"

#include "adaptive_threshold.h"
#include "cuda_support.h"
#include "detector_cuda.h"
#include "fft.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace nr_isac {
namespace {

struct Plan {
  Axes axes;
  std::vector<double> times;
  std::vector<double> weights;
  std::vector<uint8_t> rate_allowed;
  double denominator = 0.0;
};

Plan prepare(const CfrWindow& window, const PipelineConfig& config, const RateGate& gate)
{
  if (!window.valid() || window.rows < 2)
    throw std::invalid_argument("detector needs a valid coherent aperture");
  if (gate.center_mps.has_value() != gate.half_width_mps.has_value()
      || (gate.half_width_mps && !(*gate.half_width_mps > 0.0)))
    throw std::invalid_argument("invalid signed range-rate gate");
  Plan plan;
  plan.times.resize(window.rows);
  const double slot_s = slot_duration_s(window.scs_hz);
  for (uint32_t r = 0; r < window.rows; ++r)
    plan.times[r] = (window.row_time_slots[r] - window.row_time_slots[0]) * slot_s;
  plan.axes.dwell_s = plan.times.back();
  if (!(plan.axes.dwell_s > 0.0))
    throw std::invalid_argument("nonviable coherent aperture");
  plan.axes.range_res_m = C_MPS / (window.subcarriers * window.scs_hz);
  plan.axes.range_bins = std::min(window.subcarriers,
      static_cast<uint32_t>(std::floor(config.maximum_range_m / plan.axes.range_res_m)) + 1);
  plan.axes.rate_res_mps = C_MPS * (window.rows - 1.0)
                           / (window.fc_hz * window.rows * plan.axes.dwell_s);
  plan.axes.rate_bins = window.rows;
  plan.axes.rate_axis_mps.resize(window.rows);
  plan.rate_allowed.resize(window.rows, 1);
  for (uint32_t d = 0; d < window.rows; ++d) {
    const double rate = -(static_cast<double>(d) - static_cast<int>(window.rows / 2))
                        * plan.axes.rate_res_mps;
    plan.axes.rate_axis_mps[d] = rate;
    bool allowed = std::abs(rate) <= 2.0 * config.maximum_target_speed_mps;
    if (gate.half_width_mps)
      allowed = allowed && std::abs(rate - *gate.center_mps) <= *gate.half_width_mps;
    plan.rate_allowed[d] = allowed;
  }
  if (std::none_of(plan.rate_allowed.begin(), plan.rate_allowed.end(), [](uint8_t x) { return x != 0; }))
    throw std::invalid_argument("range-rate gate excludes every Doppler bin");
  plan.weights.resize((size_t)window.rows * window.subcarriers);
  for (size_t i = 0; i < plan.weights.size(); ++i) {
    plan.weights[i] = window.observed[i] ? 1.0 : 0.0;
    plan.denominator += plan.weights[i];
  }
  plan.axes.observed_re_count = static_cast<uint64_t>(plan.denominator);
  return plan;
}

double residual_energy(const std::vector<std::complex<double>>& residual,
                       uint32_t antennas, const Plan& plan)
{
  const size_t cells = plan.weights.size();
  double result = 0.0;
  for (uint32_t a = 0; a < antennas; ++a)
    for (size_t i = 0; i < cells; ++i)
      result += plan.weights[i] * std::norm(residual[(size_t)a * cells + i]);
  return result;
}

// Variant with pre-scaled slow-time coordinates. Kept explicit to make the physical sign identical
// to optimized_pipeline.py's steering_conjugate matrix.
std::vector<double> likelihood_map_scaled(const std::vector<std::complex<double>>& residual,
                                          uint32_t antennas,
                                          uint32_t rows,
                                          uint32_t subcarriers,
                                          double fc_hz,
                                          const Plan& plan,
                                          uint32_t minimum_range_bin)
{
  const uint32_t range_bins = plan.axes.range_bins;
  std::vector<std::complex<double>> projected((size_t)antennas * rows * range_bins);
  std::vector<std::complex<double>> buffer(subcarriers);
  const size_t cells = (size_t)rows * subcarriers;
  for (uint32_t a = 0; a < antennas; ++a)
    for (uint32_t r = 0; r < rows; ++r) {
      for (uint32_t k = 0; k < subcarriers; ++k) {
        const size_t cell = (size_t)r * subcarriers + k;
        buffer[k] = plan.weights[cell] * residual[(size_t)a * cells + cell];
      }
      fft_inplace(buffer, true);
      for (uint32_t q = 0; q < range_bins; ++q)
        projected[((size_t)a * rows + r) * range_bins + q] = buffer[q] * static_cast<double>(subcarriers);
    }
  const double denominator = plan.denominator * antennas;
  std::vector<double> result((size_t)range_bins * rows, -std::numeric_limits<double>::infinity());
  if (!(denominator > 0.0)) return result;
  /* The nonuniform slow-time steering depends only on Doppler and row, not on range or antenna.
   * Precompute it once per map exactly as NumPy materializes steering_conjugate; recomputing the
   * same sin/cos pair inside q*a*d*r dominated every live CLEAN iteration. */
  std::vector<std::complex<double>> slow_steering((size_t)rows * rows);
  for (uint32_t d = 0; d < rows; ++d) {
    if (!plan.rate_allowed[d]) continue;
    const double rate = plan.axes.rate_axis_mps[d];
    for (uint32_t r = 0; r < rows; ++r) {
      const double phase = 2.0 * PI * rate * fc_hz * plan.times[r] / C_MPS;
      slow_steering[(size_t)d * rows + r] =
          std::complex<double>(std::cos(phase), std::sin(phase));
    }
  }
  for (uint32_t q = minimum_range_bin; q < range_bins; ++q)
    for (uint32_t d = 0; d < rows; ++d) {
      if (!plan.rate_allowed[d]) continue;
      double power = 0.0;
      for (uint32_t a = 0; a < antennas; ++a) {
        std::complex<double> coherent(0.0, 0.0);
        for (uint32_t r = 0; r < rows; ++r)
          coherent += projected[((size_t)a * rows + r) * range_bins + q]
                      * slow_steering[(size_t)d * rows + r];
        power += std::norm(coherent);
      }
      const double score = power / denominator;
      result[(size_t)q * rows + d] = std::isfinite(score) && score > 0.0
                                        ? score : -std::numeric_limits<double>::infinity();
    }
  return result;
}

struct Peak { uint32_t r = 0, d = 0; double score = 0.0; bool valid = false; };

Peak strongest(const std::vector<double>& map, uint32_t rows)
{
  Peak result;
  for (size_t i = 0; i < map.size(); ++i)
    if (std::isfinite(map[i]) && map[i] > 0.0 && (!result.valid || map[i] > result.score)) {
      result.valid = true; result.score = map[i]; result.r = i / rows; result.d = i % rows;
    }
  return result;
}

struct Refined {
  double range_bin = 0.0, doppler_bin = 0.0, score = 0.0;
  std::vector<std::complex<double>> steering;
  std::vector<std::complex<double>> coherent;
  Localization localization;
};

Refined refine(const std::vector<std::complex<double>>& residual,
               const CfrWindow& window,
               const Plan& plan,
               Peak coarse,
               uint32_t minimum_range_bin,
               const RateGate& gate,
               double maximum_abs_rate_mps,
               CudaDetectorBackend*& cuda_backend)
{
  const uint32_t antennas = window.antennas, rows = window.rows, subcarriers = window.subcarriers;
  const size_t cells = (size_t)rows * subcarriers;
  std::array<double, 2> lower{std::max<double>(minimum_range_bin, coarse.r - 0.5),
                              std::max(0.0, coarse.d - 0.5)};
  std::array<double, 2> upper{std::min<double>(plan.axes.range_bins - 1, coarse.r + 0.5),
                              std::min<double>(rows - 1, coarse.d + 0.5)};
  double rate_lower = -maximum_abs_rate_mps, rate_upper = maximum_abs_rate_mps;
  if (gate.half_width_mps) {
    rate_lower = std::max(rate_lower, *gate.center_mps - *gate.half_width_mps);
    rate_upper = std::min(rate_upper, *gate.center_mps + *gate.half_width_mps);
  }
  if (std::isfinite(rate_upper))
    lower[1] = std::max(lower[1], rows / 2.0 - rate_upper / plan.axes.rate_res_mps);
  if (std::isfinite(rate_lower))
    upper[1] = std::min(upper[1], rows / 2.0 - rate_lower / plan.axes.rate_res_mps);
  if (lower[1] > upper[1]) lower[1] = upper[1] = coarse.d;

  struct Eval {
    double objective = 0.0;
    std::array<double, 2> gradient{};
    std::array<double, 4> hessian{};
    std::vector<std::complex<double>> steering;
    std::vector<std::complex<double>> coherent;
  };
  auto evaluate = [&](const std::array<double, 2>& point) {
    if (cuda_backend) {
      const auto accelerated = cuda_backend->evaluate(point[0], point[1]);
      Eval e;
      e.objective = accelerated.objective;
      e.gradient = accelerated.gradient;
      e.hessian = accelerated.hessian;
      e.coherent = accelerated.coherent;
      return e;
    }
    Eval e;
    e.steering.resize(cells);
    e.coherent.assign(antennas, {});
    std::vector<std::complex<double>> first_r(antennas), first_d(antennas),
                                      second_r(antennas), second_d(antennas), second_cross(antennas);
    for (uint32_t r = 0; r < rows; ++r)
      for (uint32_t k = 0; k < subcarriers; ++k) {
        const size_t cell = (size_t)r * subcarriers + k;
        const double sr = -2.0 * PI * k / subcarriers;
        const double sd = 2.0 * PI * plan.axes.rate_res_mps * window.fc_hz * plan.times[r] / C_MPS;
        const double phase = sr * point[0] + sd * (point[1] - static_cast<int>(rows / 2));
        const std::complex<double> steering(std::cos(phase), std::sin(phase));
        e.steering[cell] = steering;
        for (uint32_t a = 0; a < antennas; ++a) {
          const auto projected = plan.weights[cell] * residual[(size_t)a * cells + cell] * std::conj(steering);
          e.coherent[a] += projected;
          first_r[a] += std::complex<double>(0.0, -1.0) * projected * sr;
          first_d[a] += std::complex<double>(0.0, -1.0) * projected * sd;
          second_r[a] -= projected * sr * sr;
          second_d[a] -= projected * sd * sd;
          second_cross[a] -= projected * sr * sd;
        }
      }
    for (uint32_t a = 0; a < antennas; ++a) {
      e.objective += std::norm(e.coherent[a]);
      e.gradient[0] += 2.0 * std::real(std::conj(e.coherent[a]) * first_r[a]);
      e.gradient[1] += 2.0 * std::real(std::conj(e.coherent[a]) * first_d[a]);
      e.hessian[0] += 2.0 * std::real(std::conj(first_r[a]) * first_r[a]
                                      + std::conj(e.coherent[a]) * second_r[a]);
      const double cross_value = 2.0 * std::real(std::conj(first_d[a]) * first_r[a]
                                                 + std::conj(e.coherent[a]) * second_cross[a]);
      e.hessian[1] += cross_value; e.hessian[2] += cross_value;
      e.hessian[3] += 2.0 * std::real(std::conj(first_d[a]) * first_d[a]
                                      + std::conj(e.coherent[a]) * second_d[a]);
    }
    return e;
  };

  std::array<double, 2> point{std::clamp<double>(coarse.r, lower[0], upper[0]),
                              std::clamp<double>(coarse.d, lower[1], upper[1])};
  Eval current = evaluate(point);
  std::string convergence = "coarse_stationary";
  uint32_t iterations = 0;
  for (; iterations < 23; ++iterations) { // float32 mantissa bits, exactly np.finfo(float32).nmant
    const double info_a = -current.hessian[0], info_b = -0.5 * (current.hessian[1] + current.hessian[2]);
    const double info_d = -current.hessian[3];
    const auto eigen = eigenvalues_symmetric_2x2(info_a, info_b, info_d);
    const double max_point = std::max({1.0, std::abs(point[0]), std::abs(point[1])});
    const float fp = static_cast<float>(max_point);
    const double tolerance = std::nextafter(fp, std::numeric_limits<float>::infinity()) - fp;
    std::array<double, 2> step{};
    if (std::isfinite(eigen[0]) && eigen[0] > 0.0) {
      const double det = current.hessian[0] * current.hessian[3]
                         - current.hessian[1] * current.hessian[2];
      if (std::abs(det) <= std::numeric_limits<double>::min()) { convergence = "singular_local_likelihood"; break; }
      step[0] = (-current.gradient[0] * current.hessian[3]
                 + current.hessian[1] * current.gradient[1]) / det;
      step[1] = (current.hessian[2] * current.gradient[0]
                 - current.hessian[0] * current.gradient[1]) / det;
    } else {
      const double gn = std::hypot(current.gradient[0], current.gradient[1]);
      if (!std::isfinite(gn) || gn <= tolerance) { convergence = "nonconcave_stationary_likelihood"; break; }
      std::array<double, 2> direction{current.gradient[0] / gn, current.gradient[1] / gn};
      double feasible = std::numeric_limits<double>::infinity();
      for (size_t axis = 0; axis < 2; ++axis) {
        double value = std::numeric_limits<double>::infinity();
        if (direction[axis] > 0.0) value = (upper[axis] - point[axis]) / direction[axis];
        else if (direction[axis] < 0.0) value = (lower[axis] - point[axis]) / direction[axis];
        if (value > 0.0) feasible = std::min(feasible, value);
      }
      if (!std::isfinite(feasible)) { convergence = "nonconcave_box_stationary"; break; }
      step = {direction[0] * feasible, direction[1] * feasible};
    }
    if (!std::isfinite(step[0]) || !std::isfinite(step[1]) || std::hypot(step[0], step[1]) <= tolerance) {
      convergence = "likelihood_stationary"; break;
    }
    bool accepted = false;
    for (double scale = 1.0; scale >= std::numeric_limits<float>::epsilon(); scale *= 0.5) {
      const std::array<double, 2> candidate{
          std::clamp(point[0] + scale * step[0], lower[0], upper[0]),
          std::clamp(point[1] + scale * step[1], lower[1], upper[1])};
      if (candidate == point) break;
      Eval trial = evaluate(candidate);
      if (trial.objective > current.objective) {
        point = candidate; current = std::move(trial); accepted = true; break;
      }
    }
    if (!accepted) { convergence = "no_representable_likelihood_improvement"; ++iterations; break; }
  }
  if (iterations == 23) convergence = "floating_point_iteration_guard";

  Refined out;
  out.range_bin = point[0]; out.doppler_bin = point[1];
  out.steering = std::move(current.steering); out.coherent = std::move(current.coherent);
  out.score = current.objective / std::max(plan.denominator * antennas, std::numeric_limits<double>::min());
  out.localization.iterations = iterations;
  out.localization.convergence = convergence;

  const uint64_t observed_cells = std::count_if(plan.weights.begin(), plan.weights.end(), [](double w){ return w > 0.0; });
  double fit_energy = 0.0, signal_scale = 0.0;
  if (cuda_backend && out.steering.empty()) {
    // Weighted least squares with unit-magnitude steering removes ||coherent||^2/sum(weights).
    // Keep the unweighted residual scale separately because unobserved cells can hold fitted
    // components after prior CLEAN iterations, exactly as in the Python CUDA implementation.
    fit_energy = std::max(0.0, cuda_backend->weighted_energy()
                                  - current.objective
                                        / std::max(plan.denominator,
                                                   std::numeric_limits<double>::min()));
    signal_scale = cuda_backend->unweighted_energy()
                   / std::max<uint64_t>(1, (uint64_t)antennas * observed_cells);
  } else {
    for (uint32_t a = 0; a < antennas; ++a) {
      const auto alpha = out.coherent[a] / std::max(plan.denominator, std::numeric_limits<double>::min());
      for (size_t i = 0; i < cells; ++i) {
        fit_energy += plan.weights[i] * std::norm(residual[(size_t)a * cells + i] - alpha * out.steering[i]);
        signal_scale += std::norm(residual[(size_t)a * cells + i]);
      }
    }
    signal_scale /= std::max<uint64_t>(1, (uint64_t)antennas * observed_cells);
  }
  const uint64_t dof = std::max<uint64_t>(1, (uint64_t)antennas * observed_cells - antennas - 2);
  double variance = fit_energy / dof;
  variance = std::max(variance, std::numeric_limits<float>::epsilon() * std::numeric_limits<float>::epsilon()
                                * std::max(signal_scale, std::numeric_limits<double>::min()));
  const double scale = std::max(plan.denominator * variance, std::numeric_limits<double>::min());
  const double ia = -current.hessian[0] / scale;
  const double ib = -0.5 * (current.hessian[1] + current.hessian[2]) / scale;
  const double id = -current.hessian[3] / scale;
  const auto ie = eigenvalues_symmetric_2x2(ia, ib, id);
  if (std::isfinite(ie[0]) && ie[0] > 0.0) {
    Matrix info(2, 2); info(0,0)=ia; info(0,1)=info(1,0)=ib; info(1,1)=id;
    out.localization.covariance_bins = inverse(info);
    Matrix jac(2,2); jac(0,0)=plan.axes.range_res_m; jac(1,1)=-plan.axes.rate_res_mps;
    out.localization.covariance_range_rate = jac * out.localization.covariance_bins * jac.transposed();
    out.localization.covariance_valid = true;
  }
  return out;
}

std::vector<CleanComponent> collapse(std::vector<CleanComponent> remaining,
                                     const Plan& plan,
                                     const CfrWindow& window,
                                     const PipelineConfig& config)
{
  std::stable_sort(remaining.begin(), remaining.end(), [](const auto& a, const auto& b) { return a.score > b.score; });
  std::vector<CleanComponent> objects;
  const double doppler_hz_per_bin = plan.axes.rate_res_mps * window.fc_hz / C_MPS;
  const double max_doppler = config.maximum_path_doppler_hz > 0.0
                                 ? config.maximum_path_doppler_hz
                                 : 2.0 * config.maximum_target_speed_mps * window.fc_hz / C_MPS;
  const double floor_ratio = std::pow(10.0, config.leading_significance_db / 10.0);
  while (!remaining.empty()) {
    std::vector<uint8_t> grouped(remaining.size(), 0); grouped[0] = 1;
    std::vector<size_t> frontier{0};
    while (!frontier.empty()) {
      const size_t member = frontier.back(); frontier.pop_back();
      for (size_t i = 0; i < remaining.size(); ++i) {
        if (grouped[i]) continue;
        const double delay = std::abs(remaining[i].range_bin - remaining[member].range_bin) * plan.axes.range_res_m;
        const double doppler = std::abs(remaining[i].doppler_bin - remaining[member].doppler_bin) * doppler_hz_per_bin;
        if (delay <= config.maximum_path_delay_m && doppler <= max_doppler) {
          grouped[i] = 1; frontier.push_back(i); // active contract: transitive
        }
      }
    }
    std::vector<size_t> group;
    for (size_t i = 0; i < grouped.size(); ++i) if (grouped[i]) group.push_back(i);
    const size_t strongest = *std::max_element(group.begin(), group.end(), [&](size_t a, size_t b) {
      return remaining[a].score < remaining[b].score;
    });
    std::vector<size_t> significant;
    for (size_t i : group) if (remaining[i].score >= remaining[strongest].score * floor_ratio) significant.push_back(i);
    const size_t leading = *std::min_element(significant.begin(), significant.end(), [&](size_t a, size_t b) {
      if (remaining[a].range_bin != remaining[b].range_bin) return remaining[a].range_bin < remaining[b].range_bin;
      return remaining[a].score > remaining[b].score;
    });
    CleanComponent object = remaining[leading];
    std::vector<size_t> centroid;
    for (size_t i : significant) {
      const double dr = (remaining[i].range_bin - remaining[strongest].range_bin) / 2.0;
      const double dd = (remaining[i].doppler_bin - remaining[strongest].doppler_bin) / 2.0;
      if (dr * dr + dd * dd <= 1.0) centroid.push_back(i);
    }
    double energy = 0.0;
    for (size_t i : centroid) energy += remaining[i].score;
    if (energy > 0.0) {
      object.range_bin = object.doppler_bin = 0.0;
      for (size_t i : centroid) {
        object.range_bin += remaining[i].score * remaining[i].range_bin / energy;
        object.doppler_bin += remaining[i].score * remaining[i].doppler_bin / energy;
      }
      bool covariance = !centroid.empty();
      for (size_t i : centroid) covariance = covariance && remaining[i].localization.covariance_valid;
      if (covariance) {
        Matrix mixture(2,2);
        for (size_t i : centroid) {
          const double p = remaining[i].score / energy;
          const double dr = remaining[i].range_bin - object.range_bin;
          const double dd = remaining[i].doppler_bin - object.doppler_bin;
          mixture(0,0) += p * (remaining[i].localization.covariance_bins(0,0) + dr*dr);
          mixture(0,1) += p * (remaining[i].localization.covariance_bins(0,1) + dr*dd);
          mixture(1,0) += p * (remaining[i].localization.covariance_bins(1,0) + dd*dr);
          mixture(1,1) += p * (remaining[i].localization.covariance_bins(1,1) + dd*dd);
        }
        object.localization.covariance_bins = mixture;
        Matrix jac(2,2); jac(0,0)=plan.axes.range_res_m; jac(1,1)=-plan.axes.rate_res_mps;
        object.localization.covariance_range_rate = jac * mixture * jac.transposed();
        object.localization.covariance_valid = true;
      }
    }
    object.score = remaining[strongest].score;
    object.raw_score = remaining[strongest].raw_score;
    object.local = remaining[strongest].local;
    object.local_threshold = remaining[strongest].local_threshold;
    object.array_response = remaining[strongest].array_response;
    object.iteration = remaining[strongest].iteration;
    object.coarse_range_bin = remaining[strongest].coarse_range_bin;
    object.coarse_doppler_bin = remaining[strongest].coarse_doppler_bin;
    object.object_component_count = group.size();
    objects.push_back(std::move(object));
    std::vector<CleanComponent> next;
    for (size_t i = 0; i < remaining.size(); ++i) if (!grouped[i]) next.push_back(std::move(remaining[i]));
    remaining = std::move(next);
  }
  return objects;
}

} // namespace

DiagnosticLikelihoodMap diagnostic_likelihood_map(const CfrWindow& window,
                                                  const PipelineConfig& config,
                                                  const RateGate& rate_gate,
                                                  uint32_t minimum_range_bin)
{
  const Plan plan = prepare(window, config, rate_gate);
  if (minimum_range_bin >= plan.axes.range_bins)
    throw std::invalid_argument("diagnostic minimum range bin outside detector support");
  if (!(plan.denominator > 0.0))
    throw std::runtime_error("DL-only diagnostic RDM has no observed resource elements");
  const size_t cells = (size_t)window.rows * window.subcarriers;
  std::vector<std::complex<double>> residual((size_t)window.antennas * cells);
  for (uint32_t a = 0; a < window.antennas; ++a)
    for (size_t i = 0; i < cells; ++i)
      residual[(size_t)a * cells + i] = window.observed[i]
          ? window.values[(size_t)a * cells + i] : std::complex<float>();

  const bool require_cuda = cuda_required();
  const bool have_cuda = detector_cuda_available();
  if (require_cuda && !have_cuda)
    throw std::runtime_error("NR_ISAC_REQUIRE_CUDA=1 but no usable CUDA detector is available");
  if (have_cuda) {
    // Do not reuse the fused CLEAN backend: resetting it for the diagnostic map would overwrite
    // its device residual and change fused detection/tracker results.
    struct CachedDiagnosticBackend {
      uint32_t antennas = 0, rows = 0, subcarriers = 0, range_bins = 0;
      std::unique_ptr<CudaDetectorBackend> backend;
    };
    static thread_local CachedDiagnosticBackend cached;
    const bool shape_matches = cached.backend && cached.antennas == window.antennas
        && cached.rows == window.rows && cached.subcarriers == window.subcarriers
        && cached.range_bins == plan.axes.range_bins;
    try {
      if (!shape_matches) {
        cached = {};
        cached.antennas = window.antennas; cached.rows = window.rows;
        cached.subcarriers = window.subcarriers; cached.range_bins = plan.axes.range_bins;
        cached.backend = std::make_unique<CudaDetectorBackend>(
            window.antennas, window.rows, window.subcarriers, plan.axes.range_bins,
            window.fc_hz, plan.denominator, plan.weights, plan.times, plan.axes.rate_axis_mps,
            plan.rate_allowed, plan.axes.rate_res_mps);
      } else {
        cached.backend->reset_cpi(window.fc_hz, plan.denominator, plan.weights, plan.times,
                                  plan.axes.rate_axis_mps, plan.rate_allowed,
                                  plan.axes.rate_res_mps);
      }
      return {plan.axes, cached.backend->likelihood_map(residual, minimum_range_bin)};
    } catch (...) {
      if (require_cuda) throw;
      std::fprintf(stderr, "NR_ISAC: DL-only diagnostic CUDA likelihood failed; using CPU fallback\n");
    }
  }
  return {plan.axes, likelihood_map_scaled(residual, window.antennas, window.rows,
                                            window.subcarriers, window.fc_hz, plan,
                                            minimum_range_bin)};
}

DetectorResult detect_clean(const CfrWindow& window,
                            const PipelineConfig& config,
                            const RateGate& rate_gate,
                            uint32_t minimum_range_bin)
{
  using DetectorClock = std::chrono::steady_clock;
  const bool timing_enabled = std::getenv("NR_ISAC_DETECTOR_TIMING") != nullptr;
  const auto detector_started = DetectorClock::now();
  const Plan plan = prepare(window, config, rate_gate);
  if (minimum_range_bin >= plan.axes.range_bins)
    throw std::invalid_argument("minimum range bin outside detector support");
  const size_t cells = (size_t)window.rows * window.subcarriers;
  std::vector<std::complex<double>> residual((size_t)window.antennas * cells);
  for (uint32_t a = 0; a < window.antennas; ++a)
    for (size_t i = 0; i < cells; ++i)
      residual[(size_t)a * cells + i] = window.observed[i] ? window.values[(size_t)a * cells + i]
                                                           : std::complex<float>();

  CudaDetectorBackend* cuda_backend = nullptr;
  const bool require_cuda = cuda_required();
  const bool have_cuda = detector_cuda_available();
  if (require_cuda && !have_cuda)
    throw std::runtime_error("NR_ISAC_REQUIRE_CUDA=1 but no usable CUDA detector is available");
  if (have_cuda) {
    try {
      struct CachedBackend {
        uint32_t antennas = 0, rows = 0, subcarriers = 0, range_bins = 0;
        uint64_t last_used = 0;
        std::unique_ptr<CudaDetectorBackend> backend;
      };
      static thread_local std::vector<CachedBackend> cache;
      static thread_local uint64_t use_sequence = 0;
      ++use_sequence;
      auto found = std::find_if(cache.begin(), cache.end(), [&](const CachedBackend& entry) {
        return entry.antennas == window.antennas && entry.rows == window.rows
               && entry.subcarriers == window.subcarriers
               && entry.range_bins == plan.axes.range_bins;
      });
      if (found == cache.end()) {
        // Live scheduler row counts vary slightly around each duration bank. Retain a bounded LRU
        // of plans/workspaces so common shapes reuse all cudaMalloc and cuFFT planning state.
        constexpr size_t maximum_cached_shapes = 8;
        if (cache.size() == maximum_cached_shapes) {
          found = std::min_element(cache.begin(), cache.end(), [](const auto& left, const auto& right) {
            return left.last_used < right.last_used;
          });
          cache.erase(found);
        }
        CachedBackend entry;
        entry.antennas = window.antennas;
        entry.rows = window.rows;
        entry.subcarriers = window.subcarriers;
        entry.range_bins = plan.axes.range_bins;
        entry.last_used = use_sequence;
        entry.backend = std::make_unique<CudaDetectorBackend>(
            window.antennas, window.rows, window.subcarriers, plan.axes.range_bins,
            window.fc_hz, plan.denominator, plan.weights, plan.times, plan.axes.rate_axis_mps,
            plan.rate_allowed, plan.axes.rate_res_mps);
        cache.push_back(std::move(entry));
        cuda_backend = cache.back().backend.get();
      } else {
        found->last_used = use_sequence;
        found->backend->reset_cpi(window.fc_hz, plan.denominator, plan.weights, plan.times,
                                  plan.axes.rate_axis_mps, plan.rate_allowed,
                                  plan.axes.rate_res_mps);
        cuda_backend = found->backend.get();
      }
    } catch (const std::exception& error) {
      if (require_cuda) throw;
      std::fprintf(stderr, "NR_ISAC: CUDA detector initialization failed; using CPU fallback: %s\n",
                   error.what());
    }
  }
  const auto initialized_at = DetectorClock::now();
  DetectorResult result;
  result.axes = plan.axes;
  const double detector_denominator = plan.denominator * window.antennas;
  if (!cuda_backend) {
    result.initial_weighted_energy = residual_energy(residual, window.antennas, plan);
    result.initial_residual_scale = result.initial_weighted_energy / detector_denominator;
  }
  const double budget = false_object_budget(config.false_object_intensity_per_s, plan.axes.dwell_s);
  result.adaptive_threshold = adaptive_z_threshold(budget, config.maximum_components);

  for (uint32_t iteration = 0; iteration < config.maximum_components; ++iteration) {
    const auto map_started = DetectorClock::now();
    std::vector<double> map;
    if (cuda_backend) {
      try {
        map = cuda_backend->likelihood_map(residual, minimum_range_bin);
      } catch (const std::exception& error) {
        if (require_cuda || iteration != 0)
          throw; // Host residual is stale after a device-side CLEAN subtraction: never mix states.
        std::fprintf(stderr, "NR_ISAC: initial CUDA likelihood failed; using CPU fallback: %s\n",
                     error.what());
        cuda_backend = nullptr;
      }
    }
    if (!cuda_backend)
      map = likelihood_map_scaled(residual, window.antennas, window.rows, window.subcarriers,
                                  window.fc_hz, plan, minimum_range_bin);
    const auto map_finished = DetectorClock::now();
    if (iteration == 0) {
      result.initial_likelihood = map;
      if (cuda_backend) {
        result.initial_weighted_energy = cuda_backend->weighted_energy();
        result.initial_residual_scale = result.initial_weighted_energy / detector_denominator;
      }
    }
    const Peak peak = strongest(map, window.rows);
    if (!peak.valid) break;
    LocalStatistic local = cut_excluded_local_statistic(
        map, plan.axes.range_bins, window.rows, peak.r, peak.d,
        config.adaptive_training_range_bins, config.adaptive_training_doppler_bins,
        config.adaptive_guard_range_bins, config.adaptive_guard_doppler_bins);
    Refined refined = refine(residual, window, plan, peak, minimum_range_bin, rate_gate,
                             2.0 * config.maximum_target_speed_mps, cuda_backend);
    const auto refine_finished = DetectorClock::now();
    CleanComponent component;
    component.range_bin = refined.range_bin; component.doppler_bin = refined.doppler_bin;
    component.coarse_range_bin = peak.r; component.coarse_doppler_bin = peak.d;
    component.raw_score = refined.score;
    component.score = refined.score / std::max(result.initial_residual_scale, std::numeric_limits<double>::min());
    component.iteration = iteration + 1; component.local = local;
    component.local_threshold = result.adaptive_threshold;
    component.localization = refined.localization;
    component.array_response.resize(window.antennas);
    double alpha_power = 0.0;
    for (uint32_t a = 0; a < window.antennas; ++a) {
      const auto alpha = refined.coherent[a] / std::max(plan.denominator, std::numeric_limits<double>::min());
      component.array_response[a] = alpha;
      alpha_power += std::norm(alpha);
    }
    double before = 0.0, after = 0.0;
    if (cuda_backend) {
      const auto energy = cuda_backend->subtract_component(
          refined.range_bin, refined.doppler_bin, component.array_response);
      before = energy[0];
      after = energy[1];
    } else {
      before = residual_energy(residual, window.antennas, plan);
      for (uint32_t a = 0; a < window.antennas; ++a)
        for (size_t i = 0; i < cells; ++i)
          residual[(size_t)a * cells + i] -= component.array_response[a] * refined.steering[i];
      after = residual_energy(residual, window.antennas, plan);
    }
    const auto alpha0 = component.array_response.front();
    component.amplitude_abs = std::sqrt(alpha_power / window.antennas);
    component.amplitude_phase_rad = std::arg(alpha0);
    component.fitted_weighted_energy = alpha_power * plan.denominator;
    const auto update_finished = DetectorClock::now();
    component.weighted_energy_removed = std::max(0.0, before - after);
    result.components.push_back(std::move(component));
    if (timing_enabled)
      std::fprintf(stderr, "NR_ISAC detector iteration %u: map=%.3f ms refine=%.3f ms update=%.3f ms\n",
                   iteration + 1,
                   std::chrono::duration<double, std::milli>(map_finished - map_started).count(),
                   std::chrono::duration<double, std::milli>(refine_finished - map_finished).count(),
                   std::chrono::duration<double, std::milli>(update_finished - refine_finished).count());
  }
  result.final_weighted_energy = cuda_backend
                                     ? cuda_backend->weighted_energy()
                                     : residual_energy(residual, window.antennas, plan);
  result.objects = collapse(result.components, plan, window, config);
  if (result.objects.size() > config.maximum_objects) result.objects.resize(config.maximum_objects);
  if (timing_enabled)
    std::fprintf(stderr, "NR_ISAC detector CPI: initialize=%.3f ms total=%.3f ms\n",
                 std::chrono::duration<double, std::milli>(initialized_at - detector_started).count(),
                 std::chrono::duration<double, std::milli>(DetectorClock::now() - detector_started).count());
  return result;
}

} // namespace nr_isac
