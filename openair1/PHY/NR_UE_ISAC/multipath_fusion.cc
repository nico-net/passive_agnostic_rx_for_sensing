/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#include "multipath_fusion.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <map>
#include <stdexcept>
#include <tuple>

namespace nr_isac {
namespace {

constexpr size_t PLANE_PARAMETER_COUNT = 3;

bool finite(Vec3 value)
{
  return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

double median(std::vector<double> values)
{
  std::sort(values.begin(), values.end());
  const size_t middle = values.size() / 2;
  return values.size() & 1U ? values[middle]
                            : 0.5 * (values[middle - 1] + values[middle]);
}

std::array<double, 3> plane_parameters(const ReflectorPlane& plane)
{
  const Vec3 n = normalized(plane.normal);
  return {std::atan2(n.y, n.x), std::asin(std::clamp(n.z, -1.0, 1.0)),
          plane.offset_m};
}

ReflectorPlane parameterized_plane(const std::array<double, 3>& value,
                                   ReflectionLeg leg)
{
  ReflectorPlane plane;
  const double horizontal = std::cos(value[1]);
  plane.normal = {horizontal * std::cos(value[0]), horizontal * std::sin(value[0]),
                  std::sin(value[1])};
  plane.offset_m = value[2];
  plane.leg = leg;
  // (n,d) and (-n,-d) are the same plane. A canonical sign makes merging deterministic.
  const double sign_key = std::abs(plane.normal.x) > std::numeric_limits<double>::epsilon()
      ? plane.normal.x
      : (std::abs(plane.normal.y) > std::numeric_limits<double>::epsilon()
             ? plane.normal.y : plane.normal.z);
  if (sign_key < 0.0) {
    plane.normal = -1.0 * plane.normal;
    plane.offset_m = -plane.offset_m;
  }
  return plane;
}

std::array<double, 2> direct_model(const std::array<double, 6>& state,
                                   const BistaticGeometry& geometry)
{
  const std::vector<double> x(state.begin(), state.end());
  const auto value = enu_measurement_model(x, geometry, false);
  return {value[0], value[1]};
}

double measurement_cost(const ReflectorPathMeasurement& measurement,
                        const std::array<double, 2>& predicted)
{
  if (!measurement.detection.covariance_valid
      || measurement.detection.range_rate_covariance.rows() != 2
      || measurement.detection.range_rate_covariance.cols() != 2)
    return std::numeric_limits<double>::infinity();
  try {
    const Matrix covariance = positive_semidefinite(
        symmetrized(measurement.detection.range_rate_covariance), 1e-12);
    return quadratic({measurement.detection.range_m - predicted[0],
                      measurement.detection.range_rate_mps - predicted[1]},
                     pseudoinverse_symmetric(covariance));
  } catch (...) {
    return std::numeric_limits<double>::infinity();
  }
}

double plane_cost(const std::vector<ReflectorPathMeasurement>& measurements,
                  const ReflectorPlane& plane)
{
  double cost = 0.0;
  for (const auto& measurement : measurements) {
    const double item = measurement_cost(
        measurement, reflected_dl_model(measurement.parent_state,
                                        measurement.geometry, plane));
    if (!std::isfinite(item)) return std::numeric_limits<double>::infinity();
    cost += item;
  }
  return cost;
}

double independent_cost(const std::vector<ReflectorPathMeasurement>& measurements)
{
  double cost = 0.0;
  for (const auto& measurement : measurements) {
    const double item = measurement_cost(
        measurement, direct_model(measurement.path_state, measurement.geometry));
    if (!std::isfinite(item)) return std::numeric_limits<double>::infinity();
    cost += item;
  }
  return cost;
}

struct PlaneFit {
  bool valid = false;
  ReflectorPlane plane;
  double cost = std::numeric_limits<double>::infinity();
  Matrix information{3, 3};
};

PlaneFit fit_plane(const std::vector<ReflectorPathMeasurement>& measurements,
                   ReflectorPlane initial)
{
  PlaneFit best;
  if (measurements.size() * 2 <= PLANE_PARAMETER_COUNT) return best;
  std::array<double, 3> state = plane_parameters(initial);
  double damping = std::numeric_limits<double>::epsilon();
  const double finite_difference = std::cbrt(std::numeric_limits<double>::epsilon());
  // The limit scales only with the three-dimensional numerical problem; it is not a scene gate.
  for (size_t iteration = 0; iteration < 16 * PLANE_PARAMETER_COUNT; ++iteration) {
    const ReflectorPlane plane = parameterized_plane(state, initial.leg);
    Matrix information(PLANE_PARAMETER_COUNT, PLANE_PARAMETER_COUNT);
    std::vector<double> gradient(PLANE_PARAMETER_COUNT, 0.0);
    double cost = 0.0;
    bool usable = true;
    for (const auto& measurement : measurements) {
      std::array<double, 2> predicted;
      try { predicted = reflected_dl_model(measurement.parent_state,
                                           measurement.geometry, plane); }
      catch (...) { usable = false; break; }
      Matrix covariance;
      try {
        covariance = positive_semidefinite(
            symmetrized(measurement.detection.range_rate_covariance), 1e-12);
      } catch (...) { usable = false; break; }
      Matrix jacobian(2, PLANE_PARAMETER_COUNT);
      for (size_t parameter = 0; parameter < PLANE_PARAMETER_COUNT; ++parameter) {
        const double step = finite_difference * std::max(1.0, std::abs(state[parameter]));
        auto above = state, below = state;
        above[parameter] += step;
        below[parameter] -= step;
        try {
          const auto plus = reflected_dl_model(
              measurement.parent_state, measurement.geometry,
              parameterized_plane(above, initial.leg));
          const auto minus = reflected_dl_model(
              measurement.parent_state, measurement.geometry,
              parameterized_plane(below, initial.leg));
          for (size_t row = 0; row < 2; ++row)
            jacobian(row, parameter) = (plus[row] - minus[row]) / (2.0 * step);
        } catch (...) { usable = false; break; }
      }
      if (!usable) break;
      const std::vector<double> residual{
          measurement.detection.range_m - predicted[0],
          measurement.detection.range_rate_mps - predicted[1]};
      const Matrix weight = pseudoinverse_symmetric(covariance);
      cost += quadratic(residual, weight);
      information = information + jacobian.transposed() * weight * jacobian;
      const auto local = jacobian.transposed() * weight * residual;
      for (size_t parameter = 0; parameter < PLANE_PARAMETER_COUNT; ++parameter)
        gradient[parameter] += local[parameter];
    }
    if (!usable || !std::isfinite(cost)) break;
    if (cost < best.cost) {
      best.valid = true;
      best.cost = cost;
      best.plane = plane;
      best.information = information;
    }
    Matrix regularized = information;
    for (size_t axis = 0; axis < PLANE_PARAMETER_COUNT; ++axis)
      regularized(axis, axis) += damping * std::max(1.0, information(axis, axis));
    std::vector<double> step;
    try { step = inverse(regularized) * gradient; }
    catch (...) { break; }
    auto candidate = state;
    for (size_t axis = 0; axis < PLANE_PARAMETER_COUNT; ++axis)
      candidate[axis] += step[axis];
    const double candidate_cost = plane_cost(
        measurements, parameterized_plane(candidate, initial.leg));
    if (std::isfinite(candidate_cost) && candidate_cost < cost) {
      state = candidate;
      damping = std::max(std::numeric_limits<double>::epsilon(), damping * 0.25);
      const double step_norm = std::sqrt(std::inner_product(
          step.begin(), step.end(), step.begin(), 0.0));
      if (step_norm <= std::sqrt(std::numeric_limits<double>::epsilon())
                           * std::max(1.0, std::abs(state[2])))
        break;
    } else {
      damping = std::min(1.0 / std::numeric_limits<double>::epsilon(), damping * 10.0);
    }
  }
  if (best.valid) {
    try { best.plane.covariance = pseudoinverse_symmetric(best.information); }
    catch (...) { best.valid = false; }
  }
  return best;
}

PlaneFit fit_best_leg(const std::vector<ReflectorPathMeasurement>& measurements,
                      Vec3 parent_position,
                      Vec3 path_position)
{
  Vec3 normal;
  try { normal = normalized(path_position - parent_position); }
  catch (...) { return {}; }
  const Vec3 midpoint = 0.5 * (path_position + parent_position);
  PlaneFit best;
  for (ReflectionLeg leg : {ReflectionLeg::receive, ReflectionLeg::transmit}) {
    ReflectorPlane initial;
    initial.normal = normal;
    initial.offset_m = dot(normal, midpoint);
    initial.leg = leg;
    PlaneFit candidate = fit_plane(measurements, initial);
    if (candidate.valid && candidate.cost < best.cost) best = std::move(candidate);
  }
  return best;
}

double bic(double cost, uint64_t scalar_count, uint32_t parameters)
{
  return cost + parameters * std::log(static_cast<double>(std::max<uint64_t>(2, scalar_count)));
}

} // namespace

Vec3 mirror_across_plane(Vec3 point, const ReflectorPlane& plane)
{
  if (!finite(point) || !finite(plane.normal) || !std::isfinite(plane.offset_m))
    throw std::invalid_argument("reflection received nonfinite geometry");
  const Vec3 normal = normalized(plane.normal);
  return point + 2.0 * (plane.offset_m - dot(normal, point)) * normal;
}

std::array<double, 2> reflected_dl_model(const std::array<double, 6>& target,
                                         const BistaticGeometry& geometry,
                                         const ReflectorPlane& plane)
{
  const Vec3 x{target[0], target[1], target[2]};
  const Vec3 velocity{target[3], target[4], target[5]};
  const Vec3 tx = plane.leg == ReflectionLeg::transmit
                      ? mirror_across_plane(geometry.tx, plane) : geometry.tx;
  const Vec3 rx = plane.leg == ReflectionLeg::receive
                      ? mirror_across_plane(geometry.rx, plane) : geometry.rx;
  const Vec3 u_tx = normalized(x - tx);
  const Vec3 u_rx = normalized(x - rx);
  return {norm(x - tx) + norm(x - rx) - geometry.baseline_m(),
          dot(u_tx + u_rx, velocity)};
}

std::pair<BistaticGeometry, Detection> virtualize_reflected_detection(
    const BistaticGeometry& geometry, const Detection& detection,
    const ReflectorPlane& plane)
{
  BistaticGeometry virtual_geometry = geometry;
  if (plane.leg == ReflectionLeg::receive)
    virtual_geometry.rx = mirror_across_plane(geometry.rx, plane);
  else
    virtual_geometry.tx = mirror_across_plane(geometry.tx, plane);
  Detection adjusted = detection;
  adjusted.range_m -= virtual_geometry.baseline_m() - geometry.baseline_m();
  return {virtual_geometry, adjusted};
}

SoftEvidenceEvaluation evaluate_soft_evidence(
    const SoftRangeRateEvidence& surface,
    const std::array<double, 2>& predicted,
    const Matrix& input_covariance)
{
  SoftEvidenceEvaluation result;
  if (!surface.valid() || !surface.search_complete
      || input_covariance.rows() != 2 || input_covariance.cols() != 2
      || !std::isfinite(predicted[0]) || !std::isfinite(predicted[1]))
    return result;
  Matrix covariance = symmetrized(input_covariance);
  covariance(0, 0) = std::max(covariance(0, 0),
      surface.axes.range_res_m * surface.axes.range_res_m / 12.0);
  covariance(1, 1) = std::max(covariance(1, 1),
      surface.axes.rate_res_mps * surface.axes.rate_res_mps / 12.0);
  Matrix bins(2, 2);
  bins(0, 0) = covariance(0, 0)
               / (surface.axes.range_res_m * surface.axes.range_res_m);
  bins(0, 1) = -covariance(0, 1)
               / (surface.axes.range_res_m * surface.axes.rate_res_mps);
  bins(1, 0) = bins(0, 1);
  bins(1, 1) = covariance(1, 1)
               / (surface.axes.rate_res_mps * surface.axes.rate_res_mps);
  Matrix inverse_bins;
  try { inverse_bins = pseudoinverse_symmetric(positive_semidefinite(bins, 1e-12)); }
  catch (...) { return result; }
  const double centre_range = predicted[0] / surface.axes.range_res_m;
  const double centre_doppler = 0.5 * surface.axes.rate_bins
                                - predicted[1] / surface.axes.rate_res_mps;
  const double representable_nis = -2.0 * std::log(std::numeric_limits<double>::epsilon());
  const double half_range = std::sqrt(representable_nis * bins(0, 0));
  const double half_doppler = std::sqrt(representable_nis * bins(1, 1));
  const int64_t first_range = std::max<int64_t>(0, std::floor(centre_range - half_range));
  const int64_t last_range = std::min<int64_t>(surface.axes.range_bins - 1,
                                               std::ceil(centre_range + half_range));
  if (first_range > last_range) return result;

  std::vector<double> log_terms;
  std::vector<double> log_weights;
  double best_term = -std::numeric_limits<double>::infinity();
  uint32_t best_range = 0, best_doppler = 0;
  // Doppler is periodic. Iterating the complete axis when uncertainty spans it prevents a boundary
  // from being mistaken for missing evidence.
  const bool full_doppler = 2.0 * half_doppler + 1.0 >= surface.axes.rate_bins;
  const int64_t first_doppler = full_doppler ? 0 : std::floor(centre_doppler - half_doppler);
  const int64_t last_doppler = full_doppler ? surface.axes.rate_bins - 1
                                            : std::ceil(centre_doppler + half_doppler);
  for (int64_t range = first_range; range <= last_range; ++range)
    for (int64_t unwrapped = first_doppler; unwrapped <= last_doppler; ++unwrapped) {
      int64_t doppler = unwrapped % static_cast<int64_t>(surface.axes.rate_bins);
      if (doppler < 0) doppler += surface.axes.rate_bins;
      double dd = static_cast<double>(unwrapped) - centre_doppler;
      if (full_doppler) {
        dd = static_cast<double>(doppler) - centre_doppler;
        if (dd > surface.axes.rate_bins / 2.0) dd -= surface.axes.rate_bins;
        if (dd < -static_cast<double>(surface.axes.rate_bins) / 2.0) dd += surface.axes.rate_bins;
      }
      const std::vector<double> delta{static_cast<double>(range) - centre_range, dd};
      const double nis = quadratic(delta, inverse_bins);
      if (!(std::isfinite(nis) && nis <= representable_nis)) continue;
      const double value = surface.values[static_cast<size_t>(range)
                                          * surface.axes.rate_bins + doppler];
      if (!(std::isfinite(value) && value > 0.0)) continue;
      // The detector supplies the exponential MEAN power scale. Its robust median-to-mean
      // conversion has already happened upstream; applying log(2) here again loses evidence.
      const double tail_exponent = value / surface.null_scale;
      const double log_weight = -0.5 * nis;
      const double term = tail_exponent + log_weight;
      log_terms.push_back(term);
      log_weights.push_back(log_weight);
      if (term > best_term) {
        best_term = term;
        best_range = static_cast<uint32_t>(range);
        best_doppler = static_cast<uint32_t>(doppler);
      }
    }
  if (log_terms.empty()) return result;
  const auto log_sum_exp = [](const std::vector<double>& values) {
    const double maximum = *std::max_element(values.begin(), values.end());
    double total = 0.0;
    for (double value : values) total += std::exp(value - maximum);
    return maximum + std::log(total);
  };
  result.available = true;
  result.integrated_cells = log_terms.size();
  result.log_bayes_factor = log_sum_exp(log_terms) - log_sum_exp(log_weights)
                            - std::log(static_cast<double>(surface.effective_hypotheses));
  result.posterior_mode.range_m = best_range * surface.axes.range_res_m;
  result.posterior_mode.range_rate_mps =
      -(static_cast<double>(best_doppler) - surface.axes.rate_bins / 2.0)
      * surface.axes.rate_res_mps;
  result.posterior_mode.score = surface.values[
      static_cast<size_t>(best_range) * surface.axes.rate_bins + best_doppler]
      / surface.null_scale;
  result.posterior_mode.covariance_valid = true;
  result.posterior_mode.range_rate_covariance = covariance;
  result.posterior_mode.dwell_s = surface.axes.dwell_s;
  return result;
}

RotatingPredictiveScore predictive_bic_from_correlated_folds(
    const std::vector<std::optional<double>>& folds,
    uint32_t fitted_parameter_count,
    uint64_t scalar_observation_count)
{
  RotatingPredictiveScore result;
  std::vector<double> usable;
  for (const auto& fold : folds)
    if (fold && std::isfinite(*fold)) usable.push_back(*fold);
  result.evaluated_folds = usable.size();
  if (usable.empty()) return result;
  result.available = true;
  result.median_log_likelihood_ratio = median(std::move(usable));
  // Folds share three quarters of their data. The median is one robust predictive score, not a
  // product. BIC charges parameters against the actual scalar observations once.
  result.bic = -2.0 * result.median_log_likelihood_ratio
               + fitted_parameter_count
                     * std::log(static_cast<double>(std::max<uint64_t>(2,
                                                        scalar_observation_count)));
  return result;
}

struct PairHistory {
  std::vector<ReflectorPathMeasurement> measurements;
  std::set<uint64_t> cpis;
  ReflectorPairDecision decision;
};

struct SharedReflectorBank::Impl {
  std::map<std::pair<uint64_t, uint64_t>, PairHistory> histories;
  std::vector<ReflectorPlane> planes;
  std::map<uint64_t, std::vector<ReflectorPathMeasurement>> plane_measurements;
  uint64_t next_plane_id = 1;
};

SharedReflectorBank::SharedReflectorBank() : impl_(std::make_unique<Impl>()) {}
SharedReflectorBank::~SharedReflectorBank() = default;
SharedReflectorBank::SharedReflectorBank(SharedReflectorBank&&) noexcept = default;
SharedReflectorBank& SharedReflectorBank::operator=(SharedReflectorBank&&) noexcept = default;

const std::vector<ReflectorPlane>& SharedReflectorBank::planes() const
{
  return impl_->planes;
}

ReflectorPairDecision SharedReflectorBank::observe_pair(
    uint64_t cpi_sequence,
    uint64_t parent_track_id,
    uint64_t path_track_id,
    const std::array<double, 6>& parent_state,
    const std::array<double, 6>& path_state,
    const std::vector<ReflectorPathMeasurement>& input)
{
  if (!parent_track_id || !path_track_id || parent_track_id == path_track_id || input.empty())
    return {};
  auto& history = impl_->histories[{parent_track_id, path_track_id}];
  if (!history.cpis.insert(cpi_sequence).second) return history.decision;
  for (auto measurement : input) {
    measurement.cpi_sequence = cpi_sequence;
    measurement.parent_track_id = parent_track_id;
    measurement.path_track_id = path_track_id;
    measurement.parent_state = parent_state;
    measurement.path_state = path_state;
    history.measurements.push_back(std::move(measurement));
  }
  // A single CPI can always invent a convenient plane. It is retained as provisional history but
  // cannot affect tracking until a later independent CPI makes the model overdetermined.
  if (history.cpis.size() < 2
      || history.measurements.size() * 2 <= PLANE_PARAMETER_COUNT)
    return history.decision;
  const Vec3 parent_position{parent_state[0], parent_state[1], parent_state[2]};
  const Vec3 path_position{path_state[0], path_state[1], path_state[2]};
  const PlaneFit fit = fit_best_leg(history.measurements, parent_position, path_position);
  if (!fit.valid) return history.decision;
  const uint64_t scalar_count = 2 * history.measurements.size();
  const double reflector_bic = bic(fit.cost, scalar_count, PLANE_PARAMETER_COUNT);
  const double target_bic = bic(independent_cost(history.measurements), scalar_count, 6);
  history.decision.evidence_ready = true;
  history.decision.shared_reflector_bic = reflector_bic;
  history.decision.independent_target_bic = target_bic;
  history.decision.shared_reflector_preferred = reflector_bic < target_bic;
  if (!history.decision.shared_reflector_preferred) return history.decision;

  const auto same_pair = [&](const ReflectorPathMeasurement& measurement) {
    return measurement.parent_track_id == parent_track_id
           && measurement.path_track_id == path_track_id;
  };
  // A pair that already owns a provisional/shared plane updates its causal samples in place. It
  // cannot increment the number of supporting targets merely by surviving another CPI.
  for (size_t index = 0; index < impl_->planes.size(); ++index) {
    ReflectorPlane& existing = impl_->planes[index];
    auto& stored = impl_->plane_measurements.at(existing.id);
    if (!std::any_of(stored.begin(), stored.end(), same_pair)) continue;
    std::vector<ReflectorPathMeasurement> updated;
    std::copy_if(stored.begin(), stored.end(), std::back_inserter(updated),
                 [&](const auto& measurement) { return !same_pair(measurement); });
    updated.insert(updated.end(), history.measurements.begin(), history.measurements.end());
    const PlaneFit refreshed = fit_plane(updated, existing);
    if (refreshed.valid) {
      std::set<uint64_t> refreshed_cpis;
      std::set<uint64_t> refreshed_parents;
      std::set<std::pair<uint64_t, uint64_t>> refreshed_pairs;
      for (const auto& measurement : updated) {
        refreshed_cpis.insert(measurement.cpi_sequence);
        refreshed_parents.insert(measurement.parent_track_id);
        refreshed_pairs.insert({measurement.parent_track_id, measurement.path_track_id});
      }
      ReflectorPlane plane = refreshed.plane;
      plane.id = existing.id;
      plane.first_cpi_sequence = *refreshed_cpis.begin();
      plane.last_cpi_sequence = *refreshed_cpis.rbegin();
      plane.supporting_cpis = refreshed_cpis.size();
      plane.supporting_pairs = refreshed_pairs.size();
      plane.supporting_parent_tracks = refreshed_parents.size();
      plane.bic = bic(refreshed.cost, 2 * updated.size(), PLANE_PARAMETER_COUNT);
      plane.admitted = existing.admitted;
      existing = std::move(plane);
      stored = std::move(updated);
    }
    history.decision.shared_reflector_preferred = existing.admitted
                                                   && reflector_bic < target_bic;
    history.decision.reflector_id = existing.admitted ? existing.id : 0;
    return history.decision;
  }

  // A wall learned from only one target/path pair remains provisional. Reuse it only if a joint
  // fit with a different target beats keeping two separately fitted planes by BIC.
  size_t selected = impl_->planes.size();
  double selected_shared_bic = std::numeric_limits<double>::infinity();
  PlaneFit selected_fit;
  std::vector<ReflectorPathMeasurement> selected_measurements;
  for (size_t index = 0; index < impl_->planes.size(); ++index) {
    const ReflectorPlane& existing = impl_->planes[index];
    if (existing.leg != fit.plane.leg) continue;
    auto combined = impl_->plane_measurements.at(existing.id);
    combined.insert(combined.end(), history.measurements.begin(), history.measurements.end());
    const PlaneFit shared_fit = fit_plane(combined, existing);
    if (!shared_fit.valid) continue;
    const double shared_bic = bic(shared_fit.cost, 2 * combined.size(), PLANE_PARAMETER_COUNT);
    const double separate_bic = existing.bic + reflector_bic;
    if (shared_bic < separate_bic && shared_bic < selected_shared_bic) {
      selected = index;
      selected_shared_bic = shared_bic;
      selected_fit = shared_fit;
      selected_measurements = std::move(combined);
    }
  }
  if (selected != impl_->planes.size()) {
    const ReflectorPlane previous = impl_->planes[selected];
    std::set<uint64_t> parents;
    std::set<uint64_t> cpis;
    std::set<std::pair<uint64_t, uint64_t>> pairs;
    for (const auto& measurement : selected_measurements) {
      parents.insert(measurement.parent_track_id);
      cpis.insert(measurement.cpi_sequence);
      pairs.insert({measurement.parent_track_id, measurement.path_track_id});
    }
    ReflectorPlane merged = selected_fit.plane;
    merged.id = previous.id;
    merged.first_cpi_sequence = *cpis.begin();
    merged.last_cpi_sequence = *cpis.rbegin();
    merged.supporting_cpis = cpis.size();
    merged.supporting_pairs = pairs.size();
    merged.supporting_parent_tracks = parents.size();
    merged.bic = selected_shared_bic;
    // "Shared" means shared by distinct physical target parents as well as repeated CPIs. Two
    // ghosts of one target cannot promote their convenient plane into environmental state.
    merged.admitted = merged.supporting_pairs >= 2
                      && merged.supporting_parent_tracks >= 2
                      && merged.supporting_cpis >= 2;
    impl_->planes[selected] = std::move(merged);
    impl_->plane_measurements[previous.id] = std::move(selected_measurements);
  } else {
    ReflectorPlane plane = fit.plane;
    plane.id = impl_->next_plane_id++;
    plane.first_cpi_sequence = *history.cpis.begin();
    plane.last_cpi_sequence = *history.cpis.rbegin();
    plane.supporting_cpis = history.cpis.size();
    plane.supporting_pairs = 1;
    plane.supporting_parent_tracks = 1;
    plane.bic = reflector_bic;
    plane.admitted = false;
    impl_->plane_measurements[plane.id] = history.measurements;
    impl_->planes.push_back(std::move(plane));
    selected = impl_->planes.size() - 1;
  }
  history.decision.shared_reflector_preferred = impl_->planes[selected].admitted
                                                 && reflector_bic < target_bic;
  history.decision.reflector_id = impl_->planes[selected].admitted
                                      ? impl_->planes[selected].id : 0;
  return history.decision;
}

ReflectorPairDecision SharedReflectorBank::classify_pair(
    uint64_t parent_track_id, uint64_t path_track_id) const
{
  const auto found = impl_->histories.find({parent_track_id, path_track_id});
  return found == impl_->histories.end() ? ReflectorPairDecision{} : found->second.decision;
}

void SharedReflectorBank::retain_tracks(const std::set<uint64_t>& active)
{
  for (auto history = impl_->histories.begin(); history != impl_->histories.end();) {
    if (!active.count(history->first.first) || !active.count(history->first.second))
      history = impl_->histories.erase(history);
    else
      ++history;
  }
  // A provisional plane belongs to active causal track pairs and must not leak memory under an
  // OTA stream of short-lived clutter births. Admitted planes are environmental state and
  // intentionally survive their founding tracks.
  for (auto plane = impl_->planes.begin(); plane != impl_->planes.end();) {
    if (plane->admitted) {
      ++plane;
      continue;
    }
    const auto stored = impl_->plane_measurements.find(plane->id);
    const bool supported = stored != impl_->plane_measurements.end()
        && std::any_of(stored->second.begin(), stored->second.end(), [&](const auto& measurement) {
             return active.count(measurement.parent_track_id)
                    && active.count(measurement.path_track_id);
           });
    if (supported) {
      ++plane;
    } else {
      if (stored != impl_->plane_measurements.end()) impl_->plane_measurements.erase(stored);
      plane = impl_->planes.erase(plane);
    }
  }
}

void SharedReflectorBank::reset()
{
  impl_->histories.clear();
  impl_->planes.clear();
  impl_->plane_measurements.clear();
  impl_->next_plane_id = 1;
}

} // namespace nr_isac
