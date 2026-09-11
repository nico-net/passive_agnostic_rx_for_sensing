/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#pragma once

#include "nr_isac.h"
#include "small_matrix.h"

#include <array>
#include <complex>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace nr_isac {

constexpr double C_MPS = 299792458.0;
constexpr double PI = 3.141592653589793238462643383279502884;
// Keep this definition identical to gpu_pipeline.py.  The diagnostic DL view is not inferred
// from an already-fused row mask because a mixed row cannot be separated after averaging.
constexpr uint32_t DL_SOURCE_BITS = (1u << NR_ISAC_SRC_CSI_RS)
                                    | (1u << NR_ISAC_SRC_PDSCH_DMRS)
                                    | (1u << NR_ISAC_SRC_PDSCH_DATA)
                                    | (1u << NR_ISAC_SRC_PDSCH_DMRS_BLIND);
constexpr uint32_t UL_SOURCE_BITS = (1u << NR_ISAC_SRC_PUSCH_DMRS) | (1u << NR_ISAC_SRC_PUSCH_DATA);

struct CfrWindow {
  uint32_t antennas = 0;
  uint32_t rows = 0;
  uint32_t subcarriers = 0;
  double scs_hz = 0.0;
  double fc_hz = 0.0;
  uint16_t pci = 0;
  int64_t start_utc_ns = 0;
  std::vector<std::complex<float>> values; // antenna-major [a][row][subcarrier]
  std::vector<uint8_t> observed;           // [row][subcarrier]
  std::vector<double> row_time_slots;
  std::vector<uint32_t> row_slot_idx;
  std::vector<double> row_slot_frac;
  std::vector<uint32_t> row_source_mask;
  // P10a: bit b set when a row of this window carried branch identity b; 0 = no row was tagged.
  // Window-level, not per-row: this slice only makes the identity visible in the report, and a
  // per-row axis would be the first half of the branch-routed windowing that belongs to P13.
  uint32_t branch_mask = 0;
  std::array<uint64_t, NR_ISAC_SRC_COUNT> source_occurrences{};

  size_t cell(uint32_t row, uint32_t subcarrier) const
  { return (size_t)row * subcarriers + subcarrier; }
  size_t sample(uint32_t antenna, uint32_t row, uint32_t subcarrier) const
  { return ((size_t)antenna * rows + row) * subcarriers + subcarrier; }
  bool valid() const
  {
    const size_t cells = (size_t)rows * subcarriers;
    return antennas > 0 && rows > 0 && subcarriers > 0 && values.size() == cells * antennas
           && observed.size() == cells && row_time_slots.size() == rows
           && row_slot_idx.size() == rows && row_slot_frac.size() == rows
           && row_source_mask.size() == rows && scs_hz > 0.0 && fc_hz > 0.0;
  }
};

struct SyncEstimate {
  uint32_t rows = 0;
  uint32_t admitted_rows = 0;
  int anchor_bin = 0;
  int anchor_halfwidth_bins = 0;
  double los_bins = 0.0;
  double sto_bins = 0.0;
  double sto_standard_error_bins = std::numeric_limits<double>::infinity();
  double sfo_ppm = 0.0;
  double sfo_standard_error_ppm = std::numeric_limits<double>::infinity();
  double sfo_bic_constant = std::numeric_limits<double>::infinity();
  double sfo_bic_linear = std::numeric_limits<double>::infinity();
  double cfo_hz = 0.0;
  double cfo_resolution_hz = std::numeric_limits<double>::infinity();
  double cfo_bic_zero = std::numeric_limits<double>::infinity();
  double cfo_bic_tone = std::numeric_limits<double>::infinity();
  bool sto_applied = false;
  bool sfo_applied = false;
  bool cfo_applied = false;
  std::string reject_reason;
};

struct LocalStatistic {
  bool valid = false;
  bool fallback_global = false;
  double cut_score = 0.0;
  double z = -std::numeric_limits<double>::infinity();
  double log_background_median = 0.0;
  double log_background_mad = 0.0;
  double log_background_sigma = 0.0;
  uint32_t training_cells = 0;
};

struct Axes {
  double range_res_m = 0.0;
  double rate_res_mps = 0.0;
  double dwell_s = 0.0;
  uint32_t range_bins = 0;
  uint32_t rate_bins = 0;
  uint64_t observed_re_count = 0;
  std::vector<double> rate_axis_mps;
};

struct Localization {
  bool covariance_valid = false;
  Matrix covariance_bins{2, 2};
  Matrix covariance_range_rate{2, 2};
  uint32_t iterations = 0;
  std::string convergence;
};

struct CleanComponent {
  double range_bin = 0.0;
  double doppler_bin = 0.0;
  uint32_t coarse_range_bin = 0;
  uint32_t coarse_doppler_bin = 0;
  double score = 0.0;
  double raw_score = 0.0;
  uint32_t iteration = 0;
  double amplitude_abs = 0.0;
  double amplitude_phase_rad = 0.0;
  double weighted_energy_removed = 0.0;
  double fitted_weighted_energy = 0.0;
  LocalStatistic local;
  double local_threshold = std::numeric_limits<double>::infinity();
  Localization localization;
  std::vector<std::complex<double>> array_response;
  uint32_t object_component_count = 1;
};

struct AoaEstimate {
  bool valid = false;
  bool covariance_valid = false;
  std::string reason;
  double azimuth_deg = 0.0;
  double elevation_deg = 0.0;
  Vec3 direction;
  Matrix covariance_rad2{2, 2};
  Matrix direction_covariance{3, 3};
  double phase_fit_residual_rms_rad = 0.0;
  double relative_manifold_residual_energy = 0.0;
  uint32_t phase_wraps_tested = 0;
  bool visible_region_clipped = false;
  uint32_t component_aoa_count = 0;
  double component_direction_coherence = 1.0;
  double component_direction_rms_deg = 0.0;
  double component_direction_max_deg = 0.0;
  std::string covariance_status;
};

struct Detection {
  double range_m = 0.0;
  double range_rate_mps = 0.0;
  double score = 0.0;
  double decision_statistic = -std::numeric_limits<double>::infinity();
  double decision_threshold = std::numeric_limits<double>::infinity();
  double effective_decision_threshold = std::numeric_limits<double>::infinity();
  bool covariance_valid = false;
  Matrix range_rate_covariance{2, 2};
  uint32_t source_component_iteration = 0;
  uint32_t object_component_count = 1;
  AoaEstimate aoa;
  double dwell_s = 0.0;
  bool ul_confirmation_supported = false;
  bool ul_confirmation_candidate_specific = false;
  std::string ul_confirmation_status;
};

struct DetectorResult {
  Axes axes;
  std::vector<CleanComponent> components;
  std::vector<CleanComponent> objects;
  std::vector<double> initial_likelihood; // range-major [range][Doppler], raw score
  // Exact DL-only diagnostic view, accumulated before DL/UL PendingRow fusion.
  std::vector<double> initial_dl_likelihood;
  uint64_t dl_observed_re_count = 0;
  double initial_weighted_energy = 0.0;
  double final_weighted_energy = 0.0;
  double initial_residual_scale = 0.0;
  double adaptive_threshold = std::numeric_limits<double>::infinity();
};

struct TrackSnapshot {
  uint64_t track_id = 0;
  std::string status = "uninitialized";
  double air_time_s = 0.0;
  bool has_time = false;
  bool updated = false;
  double range_m = 0.0;
  double range_rate_mps = 0.0;
  double range_accel_mps2 = 0.0;
  double sigma_range_m = 0.0;
  double sigma_rate_mps = 0.0;
  double sigma_accel_mps2 = 0.0;
  double nis = 0.0;
  bool has_nis = false;
  double nis_ewma = 0.0;
  bool has_nis_ewma = false;
  uint32_t coast_count = 0;
  uint32_t confirmed_update_count = 0;
  uint32_t total_update_count = 0;
  bool last_update_used_angles = false;
  uint32_t temporal_aoa_rejections = 0;
  uint32_t auxiliary_aoa_updates = 0;
  uint64_t source_cpi_sequence = 0;
  double source_cpi_midpoint_s = 0.0;
  double propagation_age_s = 0.0;
  bool propagated = false;
  bool position_valid = false;
  Vec3 position_enu_m;
  Vec3 velocity_enu_mps;
  Matrix position_covariance{3, 3};
  Matrix velocity_covariance{3, 3};
  double azimuth_deg = 0.0;
  double elevation_deg = 0.0;
};

struct ArrayGeometry {
  std::array<Vec3, 4> positions{};
  Vec3 broadside{0.0, 0.0, 1.0};
  bool configured = false;
};

/** Fixed receive-chain correction, indexed by physical array element.
 *
 * physical_to_observed[p] names the SDR channel connected to physical element p.  The complex
 * correction applied at baseband offset f is
 *
 *   gain[p] * exp(j * (phase_rad[p] + 2*pi*f*delay_s[p])).
 *
 * Values are relative to element zero; a common gain/phase/delay is immaterial to AoA.
 */
struct ArrayCalibration {
  bool configured = false;
  std::array<uint32_t, 4> physical_to_observed{0, 1, 2, 3};
  std::array<double, 4> gain{1.0, 1.0, 1.0, 1.0};
  std::array<double, 4> phase_rad{};
  std::array<double, 4> delay_s{};
};

/** Admission limits for using an otherwise finite AoA measurement in tracking. */
struct AoaQualityPolicy {
  double maximum_relative_manifold_residual_energy = 0.25;
  double maximum_phase_fit_residual_rms_rad = PI / 4.0;
  double maximum_azimuth_stddev_deg = 45.0;
  double maximum_elevation_stddev_deg = 45.0;
};

struct PipelineConfig {
  uint32_t num_ues = 1; // supported runtime range: 1..4; decoding stays UE-agnostic here
  uint32_t sources_mask = 1u << NR_ISAC_SRC_CSI_RS;
  std::vector<double> duration_bank_s{0.008, 0.016, 0.024, 0.032};
  uint32_t bootstrap_duration_index = 3;
  double minimum_dwell_s = 0.006;
  double maximum_dwell_s = 0.032;
  uint32_t minimum_rows = 16;
  uint32_t maximum_rows = 512;
  // Bounds raw, not-yet-planned rows while the causal detector/tracker worker is busy.
  uint64_t pending_row_budget_bytes = 2048ULL * 1024ULL * 1024ULL;
  double k_sigma = 3.0;
  double migration_eta_bins = 0.5;
  double phase_error_max_rad = PI / 4.0;
  uint32_t confirmed_updates_before_lengthen = 3;
  uint32_t lengthen_agreement = 2;
  double plan_max_age_s = 0.5;
  double search_minimum_bins = 4.0;
  double maximum_target_speed_mps = 50.0;
  double maximum_range_m = 312.283810417;
  uint32_t maximum_components = 8;
  uint32_t maximum_objects = 8;
  double maximum_path_delay_m = 312.283810417;
  double maximum_path_doppler_hz = 0.0; // 0 derives 2*vmax*fc/c
  double leading_significance_db = -10.0;
  uint32_t adaptive_training_range_bins = 12;
  uint32_t adaptive_training_doppler_bins = 12;
  uint32_t adaptive_guard_range_bins = 2;
  uint32_t adaptive_guard_doppler_bins = 2;
  double false_object_intensity_per_s = 0.01 / 0.0305;
  bool sync_enable = true;
  bool family_static = true;
  bool tracker_enable = true;
  bool hierarchical_tracker_enable = true;
  bool aoa_enable = false;
  bool aoa_ul_enable_requested = false;
  bool aoa_ul_enable = false;
  ArrayGeometry array;
  ArrayCalibration array_calibration;
  AoaQualityPolicy aoa_quality;
  Vec3 tx_position;
  Vec3 rx_position;
  std::string rx_id = "rx1";
  std::string illuminator_id = "gnb1";
  std::string out_path = "/tmp/oaiue_sensing";
  std::string report_path;
  std::string report_endpoint;
  bool capture_rvm = false;
  // The RF/CIR clock is allowed to warm up before sensing starts.  When enabled, snapshots outside
  // [admission_start_slot, admission_end_slot) are rejected before they enter the sensing FIFO.
  bool admission_window_enabled = false;
  uint64_t admission_start_slot = 0;
  uint64_t admission_end_slot = 0;
  uint32_t subslot_symbols = 0;
  uint32_t subslot_min_re = 600;
  float subslot_min_snr_db = 0.0f;
};

/* adaptive_RX_pipeline.md P13: one independent SensingEngine per ACTIVE receive branch.
 * nr_rx_branch_set_t::b[] is itself branch_id-indexed, so the engine array is indexed by
 * branch_id too and there is no second mapping table that can fall out of step with it.
 *
 * These four helpers are the ONLY place the per-branch output identity and the routing decision
 * are derived. They are free functions rather than private detail of nr_isac.cc so the parity test
 * can pin them directly: the legacy-identity regression and the "never misroute" rule are the two
 * safety-critical properties of the array, and neither is reachable through nr_isac_init() from a
 * test (that path needs the live configuration subsystem). */

/* The ONE "is this branch active" predicate this layer uses. Both halves are tested deliberately:
 * nr_rx_branch_set_parse() now guarantees they are equivalent (it rejects a phys_map entry for a
 * branch that rx_branches does not name, and `state` is only ever moved between ACQUIRING/LOCKED/
 * LOST at runtime, never back to DISABLED), but P13a must not SILENTLY depend on that invariant --
 * if it is ever weakened again, the failure here has to be a refused row, not two engines sharing
 * one report file. */
inline bool branch_is_active(const nr_rx_branch_set_t& set, int branch)
{
  return branch >= 0 && branch < NR_RX_BRANCH_MAX && set.b[branch].physical_channel >= 0
         && set.b[branch].state != NR_RXB_DISABLED;
}

/* How many engines this set will build. Counted from branch_is_active(), NOT read from
 * set.n_active: the suffixing decision and the construction loop must agree by CONSTRUCTION, and
 * they used to be two independent predicates that a malformed phys_map could split. */
inline int branch_active_count(const nr_rx_branch_set_t& set)
{
  int n = 0;
  for (int b = 0; b < NR_RX_BRANCH_MAX; ++b) n += branch_is_active(set, b) ? 1 : 0;
  return n;
}

/* "/x/reports.jsonl" -> "/x/reports_b2.jsonl"; "/x/prefix" -> "/x/prefix_b2". The suffix goes
 * BEFORE the extension so a consumer globbing "*.jsonl" still finds every branch's stream. */
inline std::string branch_suffix_path(const std::string& base, uint8_t branch_id)
{
  if (base.empty()) return base;
  const std::string tag = "_b" + std::to_string(static_cast<unsigned>(branch_id));
  const size_t slash = base.find_last_of('/');
  const size_t dot = base.find_last_of('.');
  const bool extension = dot != std::string::npos && dot + 1 < base.size()
                         && (slash == std::string::npos ? dot > 0 : dot > slash + 1);
  return extension ? base.substr(0, dot) + tag + base.substr(dot) : base + tag;
}

/* A TCP bind endpoint ending in a numeric port gets that port plus branch_id (port 5555 becomes
 * 5557 for branch 2); anything without a trailing numeric port takes the same "_b<id>" suffix as
 * a path. Offsetting BY branch_id, not by an ordinal,
 * keeps the ports distinct for any rx_branches list and leaves branch 0 on the configured port. */
inline std::string branch_suffix_endpoint(const std::string& base, uint8_t branch_id)
{
  if (base.empty()) return base;
  size_t at = base.size();
  while (at > 0 && base[at - 1] >= '0' && base[at - 1] <= '9') --at;
  if (at < base.size() && at > 0 && base[at - 1] == ':') {
    const unsigned long port = std::strtoul(base.c_str() + at, nullptr, 10);
    return base.substr(0, at) + std::to_string(port + branch_id);
  }
  return base + "_b" + std::to_string(static_cast<unsigned>(branch_id));
}

/* The configuration one branch's engine is constructed with. With ONE active branch this returns
 * the base configuration UNCHANGED -- that is the legacy bit-identity requirement, and it is
 * deliberately keyed on set.n_active rather than on "branch_id == 0", because a single-branch
 * deployment may name any branch id and must still write exactly the configured
 * rx_id/out_path/report_path. With several, every field a ReportWriter can collide on (its JSONL
 * path, the out_path that path falls back to, the ZeroMQ bind endpoint, and the rx_id stamped into
 * every line) is made distinct. */
inline PipelineConfig branch_pipeline_config(const PipelineConfig& base,
                                             const nr_rx_branch_set_t& set, uint8_t branch_id)
{
  PipelineConfig out = base;
  if (branch_active_count(set) <= 1) return out;
  out.rx_id = base.rx_id + "_b" + std::to_string(static_cast<unsigned>(branch_id));
  out.out_path = branch_suffix_path(base.out_path, branch_id);
  out.report_path = branch_suffix_path(base.report_path, branch_id);
  out.report_endpoint = branch_suffix_endpoint(base.report_endpoint, branch_id);
  return out;
}

/* The routing decision, and the single most safety-critical rule in P13: a CFR row tagged with a
 * branch that has no engine must be DROPPED, never folded into another branch's CPI -- that would
 * corrupt the other branch's coherent window with samples it never measured, which is exactly the
 * hazard the whole per-branch plan exists to prevent. Returns the engine index (== branch_id), or
 * -1 meaning "drop".
 *
 * NR_ISAC_BRANCH_NONE resolves to the LOWEST active branch. That is the legacy path: it is where
 * the five still-unmigrated CFR producers land, and with one active branch it is the only engine
 * there is, so behaviour is exactly today's. With several active branches it is an ATTRIBUTION and
 * not a measurement -- nr_isac.cc says so loudly, once -- and it disappears as P10's continuation
 * tags the remaining producers. */
inline int branch_engine_index(const nr_rx_branch_set_t& set, uint8_t branch_id)
{
  if (branch_id == NR_ISAC_BRANCH_NONE) {
    for (int b = 0; b < NR_RX_BRANCH_MAX; ++b)
      if (branch_is_active(set, b)) return b;
    return -1;
  }
  if (branch_id >= NR_RX_BRANCH_MAX) return -1;
  return branch_is_active(set, branch_id) ? static_cast<int>(branch_id) : -1;
}

/* P10b: the submission-plan derivation, as a free function for exactly the reason the four helpers
 * above are free functions -- the legacy-identity regression is the safety-critical property and it
 * has to be pinnable from the parity test, which cannot reach nr_isac_init(). nr_isac.cc's
 * nr_isac_submit_plan() is a thin C wrapper over this that supplies the process-wide branch set and
 * counts the skips. See nr_isac.h for the contract. */
inline int build_submit_plan(const nr_rx_branch_set_t* set, nr_isac_submit_plan_t* out, int max,
                             uint32_t legacy_nof_ant, uint32_t available_antennas,
                             uint32_t* pack_antennas)
{
  if (pack_antennas) *pack_antennas = 0;
  if (!out || max < 1) return 0;
  const int active = set ? branch_active_count(*set) : 0;
  if (active <= 1) {
    // THE REGRESSION PIN. Legacy nof_ant verbatim -- the producer has already clamped it against
    // its own buffer and nb_antennas_rx, and re-clamping here would silently change the AoA path.
    out[0].first_ant = 0;
    out[0].nof_ant = legacy_nof_ant;
    out[0].branch_id = NR_ISAC_BRANCH_NONE;
    if (pack_antennas) *pack_antennas = legacy_nof_ant;
    return 1;
  }
  if (active > max) return -1;  // never fan out to a silent subset
  int n = 0;
  uint32_t pack = 0;
  for (int b = 0; b < NR_RX_BRANCH_MAX; ++b) {
    if (!branch_is_active(*set, b)) continue;
    const uint32_t physical = static_cast<uint32_t>(set->b[b].physical_channel);  // >= 0 by the predicate
    if (physical >= available_antennas) continue;  // the wrapper counts and logs this
    out[n].first_ant = physical;
    out[n].nof_ant = 1;
    out[n].branch_id = static_cast<uint8_t>(b);
    if (physical + 1u > pack) pack = physical + 1u;
    ++n;
  }
  if (pack_antennas) *pack_antennas = pack;
  return n;
}

inline double slot_duration_s(double scs_hz)
{ return 1e-3 / std::max(1.0, scs_hz / 15000.0); }

inline bool source_is_ul(uint32_t source_mask) { return (source_mask & UL_SOURCE_BITS) != 0; }

} // namespace nr_isac
