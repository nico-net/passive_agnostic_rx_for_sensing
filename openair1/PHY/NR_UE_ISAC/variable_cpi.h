/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#pragma once

#include "detector.h"

#include <optional>
#include <string>

namespace nr_isac {

struct CpiPlan {
  uint64_t plan_id = 0;
  uint64_t source_cpi_sequence = 0;
  double created_air_time_s = 0.0;
  double valid_until_air_time_s = 0.0;
  double target_dwell_s = 0.032;
  uint32_t duration_bank_index = 0;
  uint32_t minimum_rows = 2;
  uint32_t maximum_rows = 512;
  std::string reason = "bootstrap";
  std::optional<double> predicted_range_m;
  std::optional<double> predicted_range_rate_mps;
  std::optional<double> predicted_range_accel_mps2;
  std::optional<double> v_q_mps;
  std::optional<double> a_q_mps2;
  std::optional<double> migration_limit_s;
  std::optional<double> phase_limit_s;
  std::string tracker_status = "uninitialized";
  double tracker_age_s = 0.0;
  uint32_t coast_count = 0;
  std::optional<double> sigma_rate_mps;
  std::optional<double> sigma_accel_mps2;
  std::optional<double> rate_search_center_mps;
  std::optional<double> rate_search_half_width_mps;

  bool full_search() const { return !rate_search_half_width_mps.has_value(); }
};

double range_resolution_m(uint32_t subcarriers, double scs_hz);
double rate_resolution_mps(double carrier_hz, double dwell_s);
double migration_limited_dwell_s(double v_q_mps, double a_q_mps2,
                                 double eta_bins, double range_resolution);
double phase_limited_dwell_s(double a_q_mps2, double phi_max_rad, double carrier_hz);
double geometric_accel_envelope_mps2(double rate_bound_mps, double excess_range_m,
                                     double baseline_m);
RateGate finalize_search_gate(const CpiPlan& plan, uint32_t realized_rows,
                              double realized_dwell_s, double carrier_hz,
                              const PipelineConfig& config);

class CpiPlanner {
public:
  explicit CpiPlanner(PipelineConfig config = {});
  CpiPlan plan(const TrackSnapshot& snapshot, double air_time_s, double carrier_hz,
               uint32_t subcarriers, double scs_hz, bool centered_search,
               std::optional<double> baseline_m);
  CpiPlan fixed_reference_plan(double air_time_s, const TrackSnapshot& snapshot,
                               const std::string& reason = "bootstrap");
  const std::optional<CpiPlan>& last_plan() const { return last_plan_; }
  uint32_t current_index() const { return index_; }
  void reset();

private:
  CpiPlan emit(uint32_t index, const std::string& reason, double air_time_s,
               const TrackSnapshot& snapshot, std::optional<double> center,
               std::optional<double> half_width, std::optional<double> v_q,
               std::optional<double> a_q, std::optional<double> migration_limit,
               std::optional<double> phase_limit, std::optional<uint32_t> minimum_rows);
  PipelineConfig config_;
  uint64_t plan_id_ = 0;
  uint32_t index_ = 0;
  uint32_t longer_agreement_ = 0;
  std::optional<CpiPlan> last_plan_;
};

} // namespace nr_isac
