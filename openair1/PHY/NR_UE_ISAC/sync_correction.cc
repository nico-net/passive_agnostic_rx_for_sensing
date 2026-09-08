/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#include "sync_correction.h"

#include "cuda_support.h"
#include "detector_cuda.h"
#include "fft.h"
#ifdef NR_ISAC_CUDA_DETECTOR
#include "family_processing_cuda.h"
#endif
#include "robust_stats.h"
#ifdef NR_ISAC_CUDA_ACCELERATION
#include "sync_correction_cuda.h"
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <numeric>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace nr_isac {
namespace {

std::vector<double> row_times(const CfrWindow& window)
{
  std::vector<double> result(window.rows);
  const double slot_s = slot_duration_s(window.scs_hz);
  for (uint32_t r = 0; r < window.rows; ++r)
    result[r] = (window.row_time_slots[r] - window.row_time_slots[0]) * slot_s;
  return result;
}

double bic(double rss, uint32_t samples, uint32_t parameters)
{
  const double safe = std::max(rss / std::max(1u, samples), std::numeric_limits<double>::min());
  return samples * std::log(safe) + parameters * std::log(std::max(2u, samples));
}

struct LineFit {
  double intercept = 0.0;
  double slope = 0.0;
  double rss_constant = 0.0;
  double rss_linear = 0.0;
  double intercept_se = std::numeric_limits<double>::infinity();
  double slope_se = std::numeric_limits<double>::infinity();
};

LineFit weighted_line(const std::vector<double>& x,
                      const std::vector<double>& y,
                      const std::vector<double>& w)
{
  LineFit out;
  const double total = std::accumulate(w.begin(), w.end(), 0.0);
  double sx = 0.0, sy = 0.0;
  for (size_t i = 0; i < x.size(); ++i) {
    sx += w[i] * x[i]; sy += w[i] * y[i];
  }
  const double mx = sx / total, my = sy / total;
  double denominator = 0.0, numerator = 0.0;
  for (size_t i = 0; i < x.size(); ++i) {
    const double dx = x[i] - mx;
    denominator += w[i] * dx * dx;
    numerator += w[i] * dx * (y[i] - my);
  }
  if (denominator <= std::numeric_limits<double>::min()) {
    out.intercept = my;
    return out;
  }
  out.slope = numerator / denominator;
  out.intercept = my - out.slope * mx;
  for (size_t i = 0; i < x.size(); ++i) {
    const double c = y[i] - my;
    const double e = y[i] - (out.intercept + out.slope * x[i]);
    out.rss_constant += w[i] * c * c;
    out.rss_linear += w[i] * e * e;
  }
  const double sigma2 = out.rss_linear / std::max<size_t>(1, x.size() - 2);
  out.slope_se = std::sqrt(std::max(0.0, sigma2 / denominator));
  out.intercept_se = std::sqrt(std::max(0.0, sigma2 * (1.0 / total + mx * mx / denominator)));
  return out;
}

int profile_halfwidth(const std::vector<double>& power, int anchor)
{
  const int n = static_cast<int>(power.size());
  const double background = median(power);
  const double peak = power[(anchor % n + n) % n];
  const double crossing = std::sqrt(std::max(background, std::numeric_limits<double>::min())
                                    * std::max(peak, std::numeric_limits<double>::min()));
  int result = 1;
  for (int direction : {-1, 1}) {
    int distance = 1;
    while (distance < n / 2 && power[(anchor + direction * distance + n * 2) % n] > crossing)
      ++distance;
    result = std::max(result, distance);
  }
  return std::min(result, std::max(1, n / 2 - 1));
}

struct FamilyKey {
  uint32_t source = 0;
  int fraction_28 = 0;
  std::string occupancy;
  bool operator<(const FamilyKey& other) const
  { return std::tie(source, fraction_28, occupancy) < std::tie(other.source, other.fraction_28, other.occupancy); }
};

std::map<FamilyKey, std::vector<uint32_t>> families(const CfrWindow& window)
{
  std::map<FamilyKey, std::vector<uint32_t>> result;
  for (uint32_t r = 0; r < window.rows; ++r) {
    FamilyKey key;
    key.source = window.row_source_mask[r];
    key.fraction_28 = static_cast<int>(std::llround(window.row_slot_frac[r] * 28.0));
    key.occupancy.assign(reinterpret_cast<const char*>(window.observed.data() + (size_t)r * window.subcarriers),
                         window.subcarriers);
    result[std::move(key)].push_back(r);
  }
  return result;
}

double tone_rss(const std::vector<std::complex<double>>& samples,
                const std::vector<double>& times,
                const std::vector<double>& weights,
                double frequency)
{
  std::complex<double> numerator(0.0, 0.0);
  double denominator = 0.0;
  for (size_t i = 0; i < samples.size(); ++i) {
    const double phase = 2.0 * PI * frequency * times[i];
    const std::complex<double> basis(std::cos(phase), std::sin(phase));
    numerator += weights[i] * samples[i] * std::conj(basis);
    denominator += weights[i];
  }
  const std::complex<double> amplitude = numerator / denominator;
  double rss = 0.0;
  for (size_t i = 0; i < samples.size(); ++i) {
    const double phase = 2.0 * PI * frequency * times[i];
    const std::complex<double> basis(std::cos(phase), std::sin(phase));
    rss += weights[i] * std::norm(samples[i] - amplitude * basis);
  }
  return rss;
}

void compute_cir_power_cpu(const CfrWindow& window,
                           uint32_t fft_n,
                           std::vector<std::vector<double>>& cir_power,
                           std::vector<double>& mean_profile)
{
  cir_power.assign(window.rows, std::vector<double>(fft_n));
  mean_profile.assign(fft_n, 0.0);
  for (uint32_t r = 0; r < window.rows; ++r) {
    std::vector<std::complex<double>> row(fft_n);
    for (uint32_t k = 0; k < window.subcarriers; ++k)
      if (window.observed[window.cell(r, k)])
        row[k] = window.values[window.sample(0, r, k)];
    fft_inplace(row, true);
    double energy = 0.0;
    for (uint32_t i = 0; i < fft_n; ++i) {
      cir_power[r][i] = std::norm(row[i]);
      energy += cir_power[r][i];
    }
    energy = std::max(energy, static_cast<double>(std::numeric_limits<float>::min()));
    for (uint32_t i = 0; i < fft_n; ++i)
      mean_profile[i] += cir_power[r][i] / energy / window.rows;
  }
}

#ifdef NR_ISAC_CUDA_ACCELERATION
bool force_cpu_sync()
{
  return environment_flag_enabled("NR_ISAC_DISABLE_CUDA_SYNC");
}
#endif

} // namespace

SyncEstimate estimate_sync(const CfrWindow& window)
{
  if (!window.valid() || window.rows < 3 || window.subcarriers < 3)
    throw std::invalid_argument("sync estimation needs a valid >=3x3 CFR window");
  const uint32_t rows = window.rows, subcarriers = window.subcarriers;
  const int exponent = std::min(4, std::max(1, static_cast<int>(std::ceil(std::log2(std::sqrt(rows))))));
  const uint32_t oversample = 1u << exponent;
  const uint32_t fft_n = subcarriers * oversample;
  uint32_t anchor_unsigned = 0;
  int anchor = 0, halfwidth = 0;
  std::vector<double> delays, contrasts, peak_powers;
  bool used_cuda = false;
#ifdef NR_ISAC_CUDA_ACCELERATION
  static std::atomic<bool> cuda_failed{false};
  if (cuda_required() && force_cpu_sync())
    throw std::runtime_error("NR_ISAC_REQUIRE_CUDA=1 conflicts with NR_ISAC_DISABLE_CUDA_SYNC=1");
  if (!force_cpu_sync() && !cuda_failed.load(std::memory_order_relaxed)) {
    std::string error;
    CudaSyncFrontEnd front_end;
    used_cuda = compute_sync_frontend_cuda(window, oversample, front_end, &error);
    if (!used_cuda) {
      cuda_failed.store(true, std::memory_order_relaxed);
      if (cuda_required())
        throw std::runtime_error("required CUDA sync front end failed: " + error);
      std::fprintf(stderr, "SENSING: CUDA sync front end failed (%s); using the CPU fallback\n",
                   error.c_str());
    } else {
      static std::atomic<bool> cuda_logged{false};
      if (!cuda_logged.exchange(true, std::memory_order_relaxed))
        std::fprintf(stderr,
                     "SENSING: CUDA sync front end active (complex64 cuFFT/CUB, rows=%u, subcarriers=%u)\n",
                     rows, subcarriers);
      anchor_unsigned = front_end.anchor_unsigned;
      anchor = front_end.anchor;
      halfwidth = front_end.halfwidth;
      delays = std::move(front_end.delays);
      contrasts = std::move(front_end.contrasts);
      peak_powers = std::move(front_end.peak_powers);
    }
  }
#else
  if (cuda_required())
    throw std::runtime_error("NR_ISAC_REQUIRE_CUDA=1 but CUDA sync support was not built");
#endif
  if (!used_cuda) {
    std::vector<std::vector<double>> cir_power;
    std::vector<double> mean_profile;
    compute_cir_power_cpu(window, fft_n, cir_power, mean_profile);
    std::vector<double> coarse(subcarriers, 0.0);
    for (uint32_t k = 0; k < subcarriers; ++k)
      for (uint32_t j = 0; j < oversample; ++j)
        coarse[k] += mean_profile[(size_t)k * oversample + j];
    anchor_unsigned = static_cast<uint32_t>(std::max_element(coarse.begin(), coarse.end()) - coarse.begin());
    anchor = anchor_unsigned <= subcarriers / 2 ? static_cast<int>(anchor_unsigned)
                                                : static_cast<int>(anchor_unsigned) - static_cast<int>(subcarriers);
    halfwidth = profile_halfwidth(coarse, anchor_unsigned);
    const int64_t fine_center = static_cast<int64_t>(anchor_unsigned) * oversample;
    delays.resize(rows); contrasts.resize(rows); peak_powers.resize(rows);
    for (uint32_t r = 0; r < rows; ++r) {
      int64_t best_index = fine_center;
      double best_power = -1.0;
      for (int64_t offset = -static_cast<int64_t>(halfwidth * oversample);
           offset <= static_cast<int64_t>(halfwidth * oversample); ++offset) {
        int64_t candidate = (fine_center + offset) % fft_n;
        if (candidate < 0) candidate += fft_n;
        if (cir_power[r][candidate] > best_power) {
          best_power = cir_power[r][candidate]; best_index = candidate;
        }
      }
      const double left = cir_power[r][(best_index + fft_n - 1) % fft_n];
      const double centre = cir_power[r][best_index];
      const double right = cir_power[r][(best_index + 1) % fft_n];
      const double den = left - 2.0 * centre + right;
      const double delta = std::abs(den) > std::numeric_limits<float>::min()
                               ? std::clamp(0.5 * (left - right) / den, -0.5, 0.5) : 0.0;
      const int64_t signed_peak = best_index <= fft_n / 2 ? best_index : best_index - fft_n;
      delays[r] = (signed_peak + delta) / oversample;
      const double floor = median(cir_power[r]);
      contrasts[r] = std::log(std::max(centre, static_cast<double>(std::numeric_limits<float>::min())))
                     - std::log(std::max(floor, static_cast<double>(std::numeric_limits<float>::min())));
      peak_powers[r] = centre;
    }
  }
  const double med_contrast = median(contrasts);
  const double mad_contrast = median_absolute_deviation(contrasts, med_contrast);
  const double robust_sigma = 1.4826 * mad_contrast;
  const double tail = std::sqrt(2.0 * std::log(std::max(2u, rows)));
  const double threshold = med_contrast - tail * std::max(robust_sigma, std::numeric_limits<double>::epsilon());
  std::vector<uint32_t> admitted;
  for (uint32_t r = 0; r < rows; ++r)
    if (std::isfinite(delays[r]) && std::isfinite(contrasts[r]) && contrasts[r] >= threshold)
      admitted.push_back(r);

  SyncEstimate out;
  out.rows = rows; out.admitted_rows = admitted.size(); out.anchor_bin = anchor;
  out.anchor_halfwidth_bins = halfwidth;
  const uint32_t minimum_identifiable = std::max(3, static_cast<int>(std::ceil(std::log2(std::max(2u, rows)))));
  if (admitted.size() < minimum_identifiable) {
    out.reject_reason = "insufficient_statistically_admitted_rows";
    return out;
  }
  const auto all_times = row_times(window);
  std::vector<double> times, selected_delay, weights;
  for (uint32_t r : admitted) {
    times.push_back(all_times[r]); selected_delay.push_back(delays[r]); weights.push_back(peak_powers[r]);
  }
  const double weight_scale = std::max(median(weights), std::numeric_limits<double>::min());
  for (double& w : weights) w /= weight_scale;
  const double weight_median = median(weights);
  const double weight_mad = median_absolute_deviation(weights, weight_median);
  const double weight_cap = weight_median + tail * std::max(weight_mad, std::numeric_limits<double>::epsilon());
  for (double& w : weights) w = std::min(w, weight_cap);
  const LineFit fit = weighted_line(times, selected_delay, weights);
  out.sfo_bic_constant = bic(fit.rss_constant, times.size(), 1);
  out.sfo_bic_linear = bic(fit.rss_linear, times.size(), 2);
  out.sfo_applied = out.sfo_bic_linear < out.sfo_bic_constant;
  const double bins_per_second_per_ppm = 1e-6 * subcarriers * window.scs_hz;
  out.sfo_ppm = fit.slope / bins_per_second_per_ppm;
  out.sfo_standard_error_ppm = fit.slope_se / bins_per_second_per_ppm;
  out.los_bins = fit.intercept;
  out.sto_bins = out.los_bins - std::round(out.los_bins);
  out.sto_standard_error_bins = std::max(fit.intercept_se, 1.0 / (std::sqrt(12.0) * oversample));
  out.sto_applied = std::abs(out.sto_bins) > tail * out.sto_standard_error_bins;

  std::vector<std::complex<double>> tap_samples;
  tap_samples.reserve(admitted.size());
  for (size_t i = 0; i < admitted.size(); ++i) {
    const uint32_t r = admitted[i];
    std::complex<double> sum(0.0, 0.0);
    uint32_t count = 0;
    for (uint32_t k = 0; k < subcarriers; ++k) {
      if (!window.observed[window.cell(r, k)]) continue;
      const double centered = k - 0.5 * (subcarriers - 1.0);
      const double phase = 2.0 * PI * centered * selected_delay[i] / subcarriers;
      sum += static_cast<std::complex<double>>(window.values[window.sample(0, r, k)])
             * std::complex<double>(std::cos(phase), std::sin(phase));
      ++count;
    }
    tap_samples.push_back(sum / static_cast<double>(std::max(1u, count)));
  }
  std::vector<double> deltas;
  for (size_t i = 1; i < times.size(); ++i)
    if (times[i] - times[i - 1] > 0.0)
      deltas.push_back(times[i] - times[i - 1]);
  const double span = times.back() - times.front();
  if (!deltas.empty() && span > 0.0) {
    const double nyquist = 0.5 / *std::min_element(deltas.begin(), deltas.end());
    out.cfo_resolution_hz = 1.0 / (span * std::max(2.0, std::sqrt(static_cast<double>(times.size()))));
    const uint32_t bins = std::max(3, static_cast<int>(std::ceil(2.0 * nyquist / out.cfo_resolution_hz)) + 1);
    std::vector<double> spectrum(bins);
    for (uint32_t b = 0; b < bins; ++b) {
      const double f = -nyquist + 2.0 * nyquist * b / (bins - 1.0);
      std::complex<double> sum(0.0, 0.0);
      for (size_t i = 0; i < times.size(); ++i) {
        const double phase = -2.0 * PI * f * times[i];
        sum += tap_samples[i] * weights[i] * std::complex<double>(std::cos(phase), std::sin(phase));
      }
      spectrum[b] = std::abs(sum);
    }
    uint32_t best = static_cast<uint32_t>(std::max_element(spectrum.begin(), spectrum.end()) - spectrum.begin());
    out.cfo_hz = -nyquist + 2.0 * nyquist * best / (bins - 1.0);
    if (best > 0 && best + 1 < bins) {
      const double den = spectrum[best - 1] - 2.0 * spectrum[best] + spectrum[best + 1];
      if (std::abs(den) > std::numeric_limits<double>::min())
        out.cfo_hz += std::clamp(0.5 * (spectrum[best - 1] - spectrum[best + 1]) / den, -0.5, 0.5)
                      * (2.0 * nyquist / (bins - 1.0));
    }
    out.cfo_bic_zero = bic(tone_rss(tap_samples, times, weights, 0.0), times.size(), 2);
    out.cfo_bic_tone = bic(tone_rss(tap_samples, times, weights, out.cfo_hz), times.size(), 3);
    out.cfo_applied = out.cfo_bic_tone < out.cfo_bic_zero;
  }
  std::vector<std::string> reasons;
  if (!out.sto_applied) reasons.emplace_back("sto_null_preferred");
  if (!out.sfo_applied) reasons.emplace_back("sfo_constant_model_preferred");
  if (!out.cfo_applied) reasons.emplace_back("cfo_zero_model_preferred");
  for (size_t i = 0; i < reasons.size(); ++i) {
    if (i) out.reject_reason += ',';
    out.reject_reason += reasons[i];
  }
  return out;
}

static void apply_sync_correction_cpu(
    CfrWindow& window,
    const SyncEstimate& estimate,
    double delay_reference_bin,
    const std::optional<std::array<std::complex<double>, 4>>& los_spatial)
{
  if (!window.valid() || !std::isfinite(delay_reference_bin))
    throw std::invalid_argument("invalid sync-correction input");
  const auto times = row_times(window);
  std::vector<double> delay(window.rows, 0.0);
  const double constant = (estimate.sto_applied || estimate.sfo_applied)
                              ? estimate.los_bins - delay_reference_bin : 0.0;
  for (uint32_t r = 0; r < window.rows; ++r) {
    delay[r] = constant;
    if (estimate.sfo_applied)
      delay[r] += estimate.sfo_ppm * 1e-6 * window.subcarriers * window.scs_hz * times[r];
  }

  // Python requests CPE, not a direct CFO rotation. Each usable row is phase-referenced to its
  // measured LOS; missing rows use nearest admitted phase plus the BIC-selected CFO prediction.
  std::vector<double> measured_phase(window.rows, 0.0), applied_phase(window.rows, 0.0);
  std::vector<uint8_t> admitted(window.rows, 0);
  const uint32_t selected_antennas = los_spatial ? window.antennas : 1;
  for (uint32_t r = 0; r < window.rows; ++r) {
    double row_delay = estimate.los_bins;
    if (estimate.sfo_applied)
      row_delay += estimate.sfo_ppm * 1e-6 * window.subcarriers * window.scs_hz * times[r];
    std::complex<double> coherent(0.0, 0.0);
    double energy = 0.0;
    uint64_t complex_count = 0;
    for (uint32_t a = 0; a < selected_antennas; ++a) {
      const std::complex<double> spatial = los_spatial ? (*los_spatial)[a] / std::abs((*los_spatial)[a])
                                                       : std::complex<double>(1.0, 0.0);
      for (uint32_t k = 0; k < window.subcarriers; ++k) {
        if (!window.observed[window.cell(r, k)]) continue;
        const auto value = static_cast<std::complex<double>>(window.values[window.sample(a, r, k)]);
        const double centered = k - 0.5 * (window.subcarriers - 1.0);
        const double angle = 2.0 * PI * row_delay * centered / window.subcarriers;
        coherent += value * std::conj(spatial) * std::complex<double>(std::cos(angle), std::sin(angle));
        energy += std::norm(value); ++complex_count;
      }
    }
    if (!complex_count || !(energy > 0.0)) continue;
    const double explained = std::norm(coherent) / complex_count;
    const double residual = std::max(energy - explained, std::numeric_limits<double>::min());
    const uint32_t real_samples = 2 * complex_count;
    if (bic(residual, real_samples, 2) < bic(energy, real_samples, 0)) {
      admitted[r] = 1; measured_phase[r] = std::arg(coherent); applied_phase[r] = measured_phase[r];
    }
  }
  std::vector<uint32_t> usable;
  for (uint32_t r = 0; r < window.rows; ++r) if (admitted[r]) usable.push_back(r);
  if (!usable.empty()) {
    for (uint32_t r = 0; r < window.rows; ++r) {
      if (admitted[r]) continue;
      const uint32_t nearest = *std::min_element(usable.begin(), usable.end(), [&](uint32_t a, uint32_t b) {
        return std::abs(times[a] - times[r]) < std::abs(times[b] - times[r]);
      });
      applied_phase[r] = measured_phase[nearest];
      if (estimate.cfo_applied)
        applied_phase[r] += 2.0 * PI * estimate.cfo_hz * (times[r] - times[nearest]);
    }
  }
  for (uint32_t a = 0; a < window.antennas; ++a)
    for (uint32_t r = 0; r < window.rows; ++r)
      for (uint32_t k = 0; k < window.subcarriers; ++k) {
        const size_t index = window.sample(a, r, k);
        if (!window.observed[window.cell(r, k)]) {
          window.values[index] = {};
          continue;
        }
        const double centered = k - 0.5 * (window.subcarriers - 1.0);
        const double angle = 2.0 * PI * delay[r] * centered / window.subcarriers
                             - (usable.empty() ? 0.0 : applied_phase[r]);
        window.values[index] *= std::complex<float>(std::cos(angle), std::sin(angle));
      }
}

void apply_sync_correction(CfrWindow& window,
                           const SyncEstimate& estimate,
                           double delay_reference_bin,
                           const std::optional<std::array<std::complex<double>, 4>>& los_spatial)
{
#ifdef NR_ISAC_CUDA_ACCELERATION
  if (cuda_required() && force_cpu_sync())
    throw std::runtime_error("NR_ISAC_REQUIRE_CUDA=1 conflicts with NR_ISAC_DISABLE_CUDA_SYNC=1");
  static std::atomic<bool> cuda_failed{false};
  if (!force_cpu_sync() && !cuda_failed.load(std::memory_order_relaxed)) {
    std::string error;
    if (apply_sync_correction_cuda(window, estimate, delay_reference_bin, los_spatial, &error)) {
      static std::atomic<bool> cuda_logged{false};
      if (!cuda_logged.exchange(true, std::memory_order_relaxed))
        std::fprintf(stderr,
                     "SENSING: CUDA common-mode sync correction active "
                     "(complex64 output, double LOS evidence)\n");
      return;
    }
    cuda_failed.store(true, std::memory_order_relaxed);
    if (cuda_required())
      throw std::runtime_error("required CUDA sync correction failed: " + error);
    std::fprintf(stderr, "SENSING: CUDA sync correction failed (%s); using the CPU fallback\n",
                 error.c_str());
  }
#else
  if (cuda_required())
    throw std::runtime_error("NR_ISAC_REQUIRE_CUDA=1 but CUDA sync support was not built");
#endif
  apply_sync_correction_cpu(window, estimate, delay_reference_bin, los_spatial);
}

SyncEstimate IndependentClockTracker::update(const SyncEstimate& measurement,
                                              double represented_time_s,
                                              uint32_t subcarriers,
                                              double)
{
  if (!std::isfinite(represented_time_s) || (last_time_s_ && represented_time_s < *last_time_s_))
    throw std::invalid_argument("clock tracker time is nonfinite/nonmonotone");
  last_time_s_ = represented_time_s;
  const uint32_t minimum = std::max(3, static_cast<int>(std::ceil(std::log2(std::max(2u, measurement.rows)))));
  const bool selected = measurement.admitted_rows >= minimum && std::isfinite(measurement.los_bins)
                        && std::isfinite(measurement.sto_standard_error_bins)
                        && measurement.sto_standard_error_bins > 0.0;
  SyncEstimate out = measurement;
  if (selected) {
    double observed = measurement.los_bins;
    if (have_delay_)
      observed += std::round((delay_ - observed) / subcarriers) * subcarriers;
    delay_ = observed;
    delay_variance_ = measurement.sto_standard_error_bins * measurement.sto_standard_error_bins;
    ++delay_updates_; have_delay_ = true;
    out.los_bins = delay_;
    out.sto_bins = delay_ - std::round(delay_);
    out.sto_standard_error_bins = std::sqrt(delay_variance_);
    out.sto_applied = true;
  }
  return out;
}

void IndependentClockTracker::reset()
{
  have_delay_ = false; delay_ = delay_variance_ = 0.0; delay_updates_ = 0; last_time_s_.reset();
}

FamilyAlignmentStats align_allocation_families(CfrWindow& window, bool subtract_static)
{
  if (!window.valid())
    throw std::invalid_argument("invalid allocation-family window");
  FamilyAlignmentStats stats;
  const auto grouped = families(window);
#ifdef NR_ISAC_CUDA_DETECTOR
  if (detector_cuda_available()) {
    std::vector<std::vector<uint32_t>> rows;
    rows.reserve(grouped.size());
    for (const auto& item : grouped) rows.push_back(item.second);
    try {
      return align_allocation_families_cuda(window, subtract_static, rows);
    } catch (const std::exception& error) {
      if (cuda_required()) throw;
      std::fprintf(stderr, "NR_ISAC: CUDA family alignment failed; using CPU fallback: %s\n",
                   error.what());
    }
  } else if (cuda_required()) {
    throw std::runtime_error("NR_ISAC_REQUIRE_CUDA=1 but CUDA family alignment is unavailable");
  }
#endif
  stats.families = grouped.size();
  for (const auto& item : grouped) {
    const auto& rows = item.second;
    if (rows.size() < 2) {
      ++stats.singleton_rows;
      if (subtract_static)
        for (uint32_t a = 0; a < window.antennas; ++a)
          for (uint32_t k = 0; k < window.subcarriers; ++k)
            window.values[window.sample(a, rows.front(), k)] = {};
      continue;
    }
    ++stats.repeated_families;
    const uint32_t reference_row = rows.front();
    std::vector<uint32_t> columns;
    for (uint32_t k = 0; k < window.subcarriers; ++k)
      if (window.observed[window.cell(reference_row, k)]) columns.push_back(k);
    if (columns.empty()) continue;
    for (size_t ri = 1; ri < rows.size(); ++ri) {
      const uint32_t r = rows[ri];
      std::vector<double> phase(columns.size()), weight(columns.size());
      double unwrap = 0.0, previous = 0.0;
      for (size_t i = 0; i < columns.size(); ++i) {
        const uint32_t k = columns[i];
        const std::complex<double> ref = window.values[window.sample(0, reference_row, k)];
        const std::complex<double> value = window.values[window.sample(0, r, k)];
        const auto cross_value = value * std::conj(ref);
        const double raw = std::arg(cross_value);
        if (i) {
          const double step = raw - previous;
          if (step > PI) unwrap -= 2.0 * PI;
          else if (step < -PI) unwrap += 2.0 * PI;
        }
        previous = raw; phase[i] = raw + unwrap; weight[i] = std::abs(cross_value);
      }
      const double wsum = std::accumulate(weight.begin(), weight.end(), 0.0);
      if (!(wsum > std::numeric_limits<double>::min())) continue;
      double mean_k = 0.0, mean_phase = 0.0;
      for (size_t i = 0; i < columns.size(); ++i) {
        mean_k += weight[i] * columns[i]; mean_phase += weight[i] * phase[i];
      }
      mean_k /= wsum; mean_phase /= wsum;
      double num = 0.0, den = 0.0;
      for (size_t i = 0; i < columns.size(); ++i) {
        const double x = columns[i] - mean_k;
        num += weight[i] * x * (phase[i] - mean_phase); den += weight[i] * x * x;
      }
      if (!(den > std::numeric_limits<double>::min())) continue;
      const double delay_bins = -(num / den) * window.subcarriers / (2.0 * PI);
      for (uint32_t a = 0; a < window.antennas; ++a)
        for (uint32_t k : columns) {
          const double angle = 2.0 * PI * delay_bins * k / window.subcarriers;
          window.values[window.sample(a, r, k)] *= std::complex<float>(std::cos(angle), std::sin(angle));
        }
      std::complex<double> gain_num(0.0, 0.0);
      double gain_den = 0.0;
      for (uint32_t k : columns) {
        const auto ref = static_cast<std::complex<double>>(window.values[window.sample(0, reference_row, k)]);
        const auto value = static_cast<std::complex<double>>(window.values[window.sample(0, r, k)]);
        gain_num += std::conj(ref) * value; gain_den += std::norm(ref);
      }
      const auto gain = gain_den > std::numeric_limits<float>::min() ? gain_num / gain_den
                                                                     : std::complex<double>(1.0, 0.0);
      if (std::abs(gain) > std::numeric_limits<float>::min() && std::isfinite(gain.real()) && std::isfinite(gain.imag()))
        for (uint32_t a = 0; a < window.antennas; ++a)
          for (uint32_t k : columns)
            window.values[window.sample(a, r, k)] /= static_cast<std::complex<float>>(gain);
      ++stats.aligned_rows;
    }
    if (subtract_static) {
      for (uint32_t a = 0; a < window.antennas; ++a)
        for (uint32_t k : columns) {
          std::complex<double> mean(0.0, 0.0);
          for (uint32_t r : rows) mean += window.values[window.sample(a, r, k)];
          mean /= static_cast<double>(rows.size());
          for (uint32_t r : rows) window.values[window.sample(a, r, k)] -= static_cast<std::complex<float>>(mean);
        }
    }
  }
  return stats;
}

void subtract_allocation_family_static(CfrWindow& window)
{
  if (!window.valid()) throw std::invalid_argument("invalid family-static window");
  for (const auto& item : families(window)) {
    const auto& rows = item.second;
    if (rows.size() < 2) {
      for (uint32_t a = 0; a < window.antennas; ++a)
        for (uint32_t k = 0; k < window.subcarriers; ++k)
          window.values[window.sample(a, rows.front(), k)] = {};
      continue;
    }
    for (uint32_t a = 0; a < window.antennas; ++a)
      for (uint32_t k = 0; k < window.subcarriers; ++k) {
        if (!window.observed[window.cell(rows.front(), k)]) continue;
        std::complex<double> mean(0.0, 0.0);
        for (uint32_t r : rows) mean += window.values[window.sample(a, r, k)];
        mean /= static_cast<double>(rows.size());
        for (uint32_t r : rows) window.values[window.sample(a, r, k)] -= static_cast<std::complex<float>>(mean);
      }
  }
}

double estimate_current_cpi_variance(const CfrWindow& window, uint32_t* family_count,
                                     uint64_t* differenced_samples)
{
  if (!window.valid()) throw std::invalid_argument("invalid covariance window");
  const auto grouped = families(window);
  if (family_count) *family_count = grouped.size();
#ifdef NR_ISAC_CUDA_DETECTOR
  if (detector_cuda_available()) {
    std::vector<std::vector<uint32_t>> rows;
    rows.reserve(grouped.size());
    for (const auto& item : grouped) rows.push_back(item.second);
    try {
      return estimate_current_cpi_variance_cuda(window, rows, differenced_samples);
    } catch (const std::exception& error) {
      if (cuda_required()) throw;
      std::fprintf(stderr, "NR_ISAC: CUDA current-CPI variance failed; using CPU fallback: %s\n",
                   error.what());
    }
  } else if (cuda_required()) {
    throw std::runtime_error("NR_ISAC_REQUIRE_CUDA=1 but CUDA current-CPI variance is unavailable");
  }
#endif
  uint64_t count = 0;
  std::vector<double> powers;
  for (const auto& item : grouped) {
    if (item.second.size() < 2) continue;
    for (size_t i = 1; i < item.second.size(); ++i) {
      const uint32_t left = item.second[i - 1], right = item.second[i];
      for (uint32_t a = 0; a < window.antennas; ++a)
        for (uint32_t k = 0; k < window.subcarriers; ++k) {
          if (!window.observed[window.cell(left, k)] || !window.observed[window.cell(right, k)]) continue;
          const double power = std::norm(static_cast<std::complex<double>>(window.values[window.sample(a, right, k)])
                                         - static_cast<std::complex<double>>(window.values[window.sample(a, left, k)]));
          ++count;
          if (std::isfinite(power) && power > 0.0) powers.push_back(power);
        }
    }
  }
  if (differenced_samples) *differenced_samples = count;
  if (!powers.empty()) return median(std::move(powers)) / (2.0 * std::log(2.0));
  for (uint32_t a = 0; a < window.antennas; ++a)
    for (uint32_t r = 0; r < window.rows; ++r)
      for (uint32_t k = 0; k < window.subcarriers; ++k)
        if (window.observed[window.cell(r, k)]) {
          const double power = std::norm(window.values[window.sample(a, r, k)]);
          if (std::isfinite(power) && power > 0.0) powers.push_back(power);
        }
  if (powers.empty()) throw std::invalid_argument("CPI has no positive covariance samples");
  return median(std::move(powers)) / std::log(2.0);
}

std::vector<uint8_t> aoa_observed_mask(const CfrWindow& window, bool enable, bool ul_enable)
{
  std::vector<uint8_t> result = window.observed;
  if (!enable) {
    std::fill(result.begin(), result.end(), 0); return result;
  }
  if (ul_enable) return result;
  for (uint32_t r = 0; r < window.rows; ++r)
    if (source_is_ul(window.row_source_mask[r]))
      std::fill(result.begin() + (size_t)r * window.subcarriers,
                result.begin() + (size_t)(r + 1) * window.subcarriers, 0);
  return result;
}

} // namespace nr_isac
