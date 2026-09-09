/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#include "variable_cpi.h"

#include "detector.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace nr_isac {

double range_resolution_m(uint32_t n, double scs)
{
  if (!n || !(scs > 0.0)) throw std::invalid_argument("range resolution needs positive support");
  return C_MPS / (n * scs);
}

double rate_resolution_mps(double fc, double dwell)
{
  if (!(fc > 0.0) || !(dwell > 0.0)) throw std::invalid_argument("rate resolution needs positive support");
  return C_MPS / (fc * dwell);
}

double migration_limited_dwell_s(double v, double a, double eta, double dr)
{
  if (v < 0.0 || a < 0.0 || !(eta * dr > 0.0))
    throw std::invalid_argument("invalid migration bound");
  const double budget = eta * dr;
  if (a <= 1e-12) return v <= 1e-12 ? std::numeric_limits<double>::infinity() : budget / v;
  return (-v + std::sqrt(v * v + 2.0 * a * budget)) / a;
}

double phase_limited_dwell_s(double a, double phi, double fc)
{
  if (a < 0.0 || !(phi > 0.0) || !(fc > 0.0)) throw std::invalid_argument("invalid phase bound");
  return a <= 1e-12 ? std::numeric_limits<double>::infinity()
                    : std::sqrt(phi * C_MPS / (PI * fc * a));
}

double geometric_accel_envelope_mps2(double rate, double excess_range, double baseline)
{
  if (rate < 0.0 || !(baseline > 0.0)) throw std::invalid_argument("invalid geometric envelope");
  if (!(excess_range > 0.0) || !std::isfinite(excess_range))
    return std::numeric_limits<double>::infinity();
  const double near_leg = 0.5 * excess_range;
  const double far_leg = baseline + near_leg;
  return rate * rate * (1.0 / near_leg + 1.0 / far_leg);
}

RateGate finalize_search_gate(const CpiPlan& p, uint32_t rows, double dwell, double fc,
                              const PipelineConfig& c)
{
  RateGate out;
  if (!p.rate_search_center_mps || !p.rate_search_half_width_mps || rows < 2 || !(dwell > 0.0))
    return out;
  const double realized = C_MPS * (rows - 1.0) / (fc * rows * dwell);
  const double half = std::max({c.k_sigma * p.sigma_rate_mps.value_or(0.0),
                                c.search_minimum_bins * realized,
                                0.5 * p.a_q_mps2.value_or(0.0) * dwell});
  if (half < 2.0 * c.maximum_target_speed_mps) {
    out.center_mps = p.rate_search_center_mps;
    out.half_width_mps = half;
  }
  return out;
}

CpiPlanner::CpiPlanner(PipelineConfig config) : config_(std::move(config))
{
  if (config_.duration_bank_s.empty()
      || !std::is_sorted(config_.duration_bank_s.begin(), config_.duration_bank_s.end())
      || std::adjacent_find(config_.duration_bank_s.begin(), config_.duration_bank_s.end())
           != config_.duration_bank_s.end()
      || std::any_of(config_.duration_bank_s.begin(), config_.duration_bank_s.end(),
                     [](double x) { return !(x > 0.0); }))
    throw std::invalid_argument("duration bank must be positive, ascending, and unique");
  config_.bootstrap_duration_index = std::min<uint32_t>(
      config_.bootstrap_duration_index, config_.duration_bank_s.size() - 1);
  if (!(config_.minimum_dwell_s > 0.0)
      || config_.minimum_dwell_s > config_.maximum_dwell_s
      || config_.minimum_rows < 2 || config_.maximum_rows < config_.minimum_rows
      || !(config_.k_sigma > 0.0) || !(config_.migration_eta_bins > 0.0)
      || !(config_.phase_error_max_rad > 0.0 && config_.phase_error_max_rad <= PI)
      || !config_.confirmed_updates_before_lengthen || !config_.lengthen_agreement
      || !(config_.plan_max_age_s > 0.0) || !(config_.search_minimum_bins > 0.0))
    throw std::invalid_argument("invalid variable-CPI configuration");
  index_ = config_.bootstrap_duration_index;
}

void CpiPlanner::reset()
{
  plan_id_ = 0; index_ = config_.bootstrap_duration_index; longer_agreement_ = 0;
  last_plan_.reset();
}

CpiPlan CpiPlanner::emit(uint32_t index, const std::string& reason, double t,
                         const TrackSnapshot& s, std::optional<double> center,
                         std::optional<double> half, std::optional<double> vq,
                         std::optional<double> aq, std::optional<double> migration,
                         std::optional<double> phase, std::optional<uint32_t> minimum_rows)
{
  CpiPlan p;
  p.plan_id = ++plan_id_; p.source_cpi_sequence = s.source_cpi_sequence;
  p.created_air_time_s = t; p.valid_until_air_time_s = t + config_.plan_max_age_s;
  p.target_dwell_s = config_.duration_bank_s.at(index); p.duration_bank_index = index;
  p.minimum_rows = minimum_rows.value_or(config_.minimum_rows);
  p.maximum_rows = config_.maximum_rows; p.reason = reason; p.tracker_status = s.status;
  if (s.has_time && s.status != "uninitialized" && s.status != "lost" && s.status != "stale") {
    p.predicted_range_m = s.range_m; p.predicted_range_rate_mps = s.range_rate_mps;
    p.predicted_range_accel_mps2 = s.range_accel_mps2;
    p.sigma_rate_mps = s.sigma_rate_mps; p.sigma_accel_mps2 = s.sigma_accel_mps2;
  }
  p.v_q_mps = vq; p.a_q_mps2 = aq;
  if (migration && std::isfinite(*migration)) p.migration_limit_s = migration;
  if (phase && std::isfinite(*phase)) p.phase_limit_s = phase;
  p.tracker_age_s = s.propagation_age_s; p.coast_count = s.coast_count;
  p.rate_search_center_mps = center; p.rate_search_half_width_mps = half;
  index_ = index; last_plan_ = p;
  return p;
}

CpiPlan CpiPlanner::fixed_reference_plan(double t, const TrackSnapshot& s,
                                          const std::string& reason)
{
  longer_agreement_ = 0;
  return emit(config_.bootstrap_duration_index, reason, t, s, {}, {}, {}, {}, {}, {}, 2);
}

CpiPlan CpiPlanner::plan(const TrackSnapshot& s, double t, double fc, uint32_t subcarriers,
                         double scs, bool centered_search, std::optional<double> baseline)
{
  if (s.status == "uninitialized" || s.status == "lost")
    return fixed_reference_plan(t, s, s.status == "lost" ? "lost_fallback" : "bootstrap");
  if (s.status == "stale" || s.propagation_age_s > config_.plan_max_age_s)
    return fixed_reference_plan(t, s, "stale_fallback");
  if (s.status == "tentative" || s.status == "coasting")
    return fixed_reference_plan(t, s, s.status == "coasting" ? "coast" : "bootstrap");
  if (s.status != "confirmed") return fixed_reference_plan(t, s, "stale_fallback");

  const double vq = std::abs(s.range_rate_mps) + config_.k_sigma * s.sigma_rate_mps;
  double envelope = std::numeric_limits<double>::infinity();
  if (baseline)
    envelope = geometric_accel_envelope_mps2(vq, s.range_m, *baseline);
  const double aq = std::abs(s.range_accel_mps2)
                    + std::min(config_.k_sigma * s.sigma_accel_mps2, envelope);
  const double migration = migration_limited_dwell_s(
      vq, aq, config_.migration_eta_bins, range_resolution_m(subcarriers, scs));
  const double phase = phase_limited_dwell_s(aq, config_.phase_error_max_rad, fc);
  const double limit = std::min({migration, phase, config_.maximum_dwell_s});
  uint32_t target = 0; bool any = false;
  for (uint32_t i = 0; i < config_.duration_bank_s.size(); ++i)
    if (config_.duration_bank_s[i] <= limit && config_.duration_bank_s[i] >= config_.minimum_dwell_s)
      target = i, any = true;
  std::string reason = any ? (target < index_ ? "maneuver_shortening" : "confirmed_track")
                           : "sparse_nonviable";
  if (target > index_) {
    const bool eligible = s.confirmed_update_count >= config_.confirmed_updates_before_lengthen
                          && s.coast_count == 0;
    longer_agreement_ = eligible ? longer_agreement_ + 1 : 0;
    if (eligible && longer_agreement_ >= config_.lengthen_agreement) {
      target = index_ + 1; longer_agreement_ = 0;
    } else target = index_;
    reason = "confirmed_track";
  } else longer_agreement_ = 0;

  std::optional<double> center, half;
  if (centered_search) {
    const double dwell = config_.duration_bank_s[target];
    const double value = std::max({config_.k_sigma * s.sigma_rate_mps,
                                   config_.search_minimum_bins * rate_resolution_mps(fc, dwell),
                                   0.5 * aq * dwell});
    if (value < 2.0 * config_.maximum_target_speed_mps) {
      center = s.range_rate_mps; half = value;
    }
  }
  return emit(target, reason, t, s, center, half, vq, aq, migration, phase, {});
}

} // namespace nr_isac
