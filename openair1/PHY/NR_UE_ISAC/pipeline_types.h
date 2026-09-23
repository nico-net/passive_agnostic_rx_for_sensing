/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#pragma once

#include "coherent_types.h"
#include "nr_isac.h"
#include "small_matrix.h"

#include <array>
#include <complex>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace nr_isac {

constexpr double C_MPS = 299792458.0;
constexpr double PI = 3.141592653589793238462643383279502884;
constexpr double SPATIAL_CPI_DURATION_S = 0.075;
// Keep this definition identical to gpu_pipeline.py.  The diagnostic DL view is not inferred
// from an already-fused row mask because a mixed row cannot be separated after averaging.
constexpr uint32_t DL_SOURCE_BITS = (1u << NR_ISAC_SRC_CSI_RS)
                                    | (1u << NR_ISAC_SRC_PDSCH_DMRS)
                                    | (1u << NR_ISAC_SRC_PDSCH_DATA)
                                    | (1u << NR_ISAC_SRC_PDSCH_DMRS_BLIND);
constexpr uint32_t UL_SOURCE_BITS = (1u << NR_ISAC_SRC_PUSCH_DMRS) | (1u << NR_ISAC_SRC_PUSCH_DATA);

struct CfrWindow {
  // Zero for non-session-specific sources. UL windows carry the PUSCH protocol session and must
  // never contain rows from more than one transmitter.
  uint64_t session_id = 0;
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
  double column_peak = 0.0;   // (2026-09-22) strongest cell in the CUT's range column (skirt rule)
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
  // OUR ADAPTATION: -1 means this component is independent (a bulk/rigid-body component, or a
  // component with no other component sharing its range neighborhood). A non-negative value is
  // the `iteration` of the bulk component this one shares range support with but not Doppler
  // support with -- i.e. a candidate micro-Doppler sideband of that bulk component, not merged
  // into it (unlike collapse_unresolved's existing range+Doppler blend) so its own range/Doppler/
  // amplitude stay exactly as CLEAN found them. Set by tag_micro_doppler_families(), after
  // collapse_unresolved. Never used to change the underlying CLEAN detection/subtraction.
  int32_t bulk_component_iteration = -1;
  // (2026-09-20) true when the proposal failed the range-column (greatest-of) CFAR test: a Doppler
  // skirt of a stronger component.  Subtracted by CLEAN like any component, but excluded from
  // the objects/detections the report emits.
  bool skirt = false;
  double amplitude_abs = 0.0;
  double amplitude_phase_rad = 0.0;
  // Explicit fitted coefficient.  amplitude_abs/phase are retained for report compatibility;
  // this value avoids a lossy reconstruction in downstream path-origin processing.
  std::complex<double> complex_coefficient;
  double weighted_energy_removed = 0.0;
  double fitted_weighted_energy = 0.0;
  LocalStatistic local;
  double local_threshold = std::numeric_limits<double>::infinity();
  // (2026-09-22) range-column (greatest-of) test that decides `skirt`, exported for diagnosis
  double column_z = 0.0;
  double column_threshold = std::numeric_limits<double>::infinity();
  uint32_t column_training_cells = 0;
  Localization localization;
  std::vector<std::complex<double>> array_response;
  uint32_t object_component_count = 1;
  // One-based serial CLEAN iterations contributing to this component/object.  This is provenance
  // only: it is populated after the unchanged CLEAN decisions have been made.
  std::vector<uint32_t> component_lineage;
  bool split_validated = false;
  double split_minimum_z = -std::numeric_limits<double>::infinity();
  double split_threshold = std::numeric_limits<double>::infinity();
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
  bool split_validated = false;
  double split_minimum_z = -std::numeric_limits<double>::infinity();
  double split_threshold = std::numeric_limits<double>::infinity();
  bool covariance_valid = false;
  Matrix range_rate_covariance{2, 2};
  uint32_t source_component_iteration = 0;
  int32_t bulk_component_iteration = -1;
  uint32_t object_component_count = 1;
  std::complex<double> complex_coefficient;
  std::vector<uint32_t> component_lineage;
  AoaEstimate aoa;
  double dwell_s = 0.0;
  bool ul_confirmation_supported = false;
  bool ul_confirmation_candidate_specific = false;
  std::string ul_confirmation_status;
};

/** Immutable, receiver-local first-pass likelihood surface supplied to fusion.
 *
 * Values are the detector's unthresholded current-CPI GLRT map.  They are never modified by the
 * tracker and are explicitly distinguished from CLEAN residual iterations.  A missing or
 * incomplete surface is unavailable evidence, not a negative detection.
 */
struct SoftRangeRateEvidence {
  Axes axes;
  std::vector<double> values; // range-major [range][Doppler], raw detector score
  double null_scale = 0.0; // exponential mean power, NOT its median
  uint32_t psf_range_halfwidth_bins = 0;
  uint32_t psf_doppler_halfwidth_bins = 0;
  uint64_t effective_hypotheses = 0;
  bool search_complete = false;

  bool valid() const
  {
    return axes.range_bins > 0 && axes.rate_bins > 0
           && values.size() == static_cast<size_t>(axes.range_bins) * axes.rate_bins
           && axes.range_res_m > 0.0 && axes.rate_res_mps > 0.0
           && std::isfinite(null_scale) && null_scale > 0.0
           && effective_hypotheses > 0;
  }
};

struct DetectorResult {
  Axes axes;
  std::vector<CleanComponent> components;
  std::vector<CleanComponent> objects;
  std::vector<double> initial_likelihood; // range-major [range][Doppler], raw score
  std::vector<double> final_likelihood;   // same layout: residual map after the last CLEAN subtraction (export only)
  // Exact DL-only diagnostic view, accumulated before DL/UL PendingRow fusion.
  std::vector<double> initial_dl_likelihood;
  uint64_t dl_observed_re_count = 0;
  double initial_weighted_energy = 0.0;
  double final_weighted_energy = 0.0;
  double initial_residual_scale = 0.0;
  double adaptive_threshold = std::numeric_limits<double>::infinity();
  uint32_t psf_range_halfwidth_bins = 0;
  uint32_t psf_doppler_halfwidth_bins = 0;
  uint64_t searched_cells = 0;
  uint64_t resolution_cells = 0;
  uint64_t effective_hypotheses = 0;
  uint64_t split_validation_rejections = 0;
  uint64_t identifiability_guard = 0;
  uint32_t skirt_components = 0;   // (2026-09-20) proposals rejected by the range-column CFAR test
  double null_scale = 0.0; // exponential mean power; shared contract with soft-map consumers
  std::string null_scale_source;
  std::string stop_reason;
};

struct TrackSnapshot {
  double existence_score = 0.0;
  double existence_threshold = 0.0;
  uint32_t existence_receiver_mask = 0;
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
  bool imm_valid = false;
  std::array<double, 3> imm_model_probabilities{}; // CV, CA, coordinated turn
  uint32_t receiver_update_count = 0;
  bool has_geometry_condition = false;
  double geometry_condition = 0.0;
  std::string ul_validation_status = "unavailable";
  double ul_validation_log_bayes_factor = 0.0;
  double direct_predictive_bic = std::numeric_limits<double>::infinity();
  double reflected_predictive_bic = std::numeric_limits<double>::infinity();
  // A reportable target must win a causal direct-vs-clutter predictive comparison in more than
  // one CPI.  These counters expose that decision rather than hiding it in track confirmation.
  uint64_t direct_validation_epochs = 0;
  uint64_t direct_support_epochs = 0;
  uint32_t direct_supported_receivers = 0;
  double direct_validation_mean_log_bayes_factor = 0.0;
  bool direct_emission_validated = false;
  uint64_t shared_reflector_id = 0;
  uint64_t physical_parent_track_id = 0;
  // Diagnostic first stage of measurement-origin management. It is intentionally separate from
  // track status until independent simulation/OTA validation demonstrates safe suppression.
  std::string origin_hypothesis = "clutter_or_new";
  bool origin_evidence_ready = false;
  std::array<double, 3> origin_log_evidence{}; // target, static/slow multipath, clutter/new
  double origin_temporal_structure_log_bayes_factor = 0.0;
  double origin_cross_rx_structure_log_bayes_factor = 0.0;
  double origin_stationary_log_bayes_factor = 0.0;
  double origin_ul_direct_log_bayes_factor = 0.0;
  double origin_exclusivity_log_bayes_factor = 0.0;
  uint64_t origin_exclusivity_competitor_track_id = 0;
  uint64_t origin_observed_epochs = 0;
  uint64_t origin_cross_rx_epochs = 0;
};

struct ArrayGeometry {
  std::array<Vec3, 4> positions{};
  Vec3 broadside{0.0, 0.0, 1.0};
  bool configured = false;
};

struct SpatialReceiverGeometry {
  std::array<Vec3, 4> positions{};
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
  uint32_t evidence_mode = 0;
  uint32_t lifecycle_features = 0;
  double evidence_window_s = 0.150;
  double existence_threshold_scale = 1.0;
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
  // Operational requirements have no library defaults.  The OAI configuration parser and every
  // caller must declare them; zero deliberately fails closed before sensing starts.
  double maximum_target_speed_mps = 0.0;
  double maximum_range_m = 0.0;
  double false_object_intensity_per_s = 0.0;
  // EXPERIMENTAL, REVERTIBLE: see MultistaticImmTrackerConfig::ue_position_known's declaration in
  // multistatic_imm_tracker.h. Default false leaves every existing path unchanged.
  bool ue_position_known = false;
  Vec3 ue_position;
  // Wall-clock budget handed to the CLEAN detector. In a live system this MUST remain the CPI
  // cadence: work that cannot finish before the next causal CPI has to be shed, and the detector's
  // own comment is explicit that shedding is never a live-throughput claim. In OFFLINE REPLAY the
  // constraint is an artefact -- there is no next real CPI to race, but the 8 concurrent detector
  // invocations (4 receivers x DL+UL) still contend for the same hardware, so each is truncated by
  // machine load rather than by evidence. Measured cost on these captures: the deadline fires on
  // 45-75% of receiver-CPIs, halves emitted components (2.3-2.8 -> 1.2 per receiver) and costs
  // 15-17 points of target detection probability; because the cut-off point depends on wall-clock
  // timing it is also the reason two identical replay runs differ in 158/160 CPIs. Raising it for
  // offline scoring lets CLEAN stop on its own statistical criterion instead. Zero or negative
  // means unlimited. Default preserves the existing live-cadence behavior exactly.
  double spatial_detector_deadline_s = SPATIAL_CPI_DURATION_S;
  bool sync_enable = true;
  // DL excess-range origin.  true: the measured direct-path delay of the current CPI is placed at
  // zero (the only reference an OTA receiver without absolute timing has; a sub-cell LOS-peak bias
  // from close environment paths enters the range axis).  false: the surveyed gNB->RX baseline is
  // placed at zero, which is exact only when the CFR delay origin is absolute (ideal file replay)
  // and demonstrably fails under any STO/SFO (impairment A/B, 2026-09-19).
  bool dl_reference_measured_los = true;
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
  SpatialReceiverGeometry spatial_receivers;
  std::string rx_id = "rx1";
  std::string illuminator_id = "gnb1";
  std::string out_path = "/tmp/oaiue_sensing";
  std::string report_path;
  std::string report_endpoint;
  bool capture_rvm = false;
  // Report-side map cost controls (Task 12): capture_rvm alone made every CPI's report carry a
  // full range-Doppler blob per receiver, which is unaffordable for a live capture. These bound
  // that cost without touching what the detector itself computes.
  double rvm_period_s = 0.5;   // emit a map at most this often (steady clock); default 0.5 s
  double rvm_max_range_m = 0.0; // crop each map's range axis to this many metres; 0 = full axis
  coherent::CoherentConfig coherent;   // coherent fuser (coherent_enable); default off
  // The RF/CIR clock is allowed to warm up before sensing starts.  When enabled, snapshots outside
  // [admission_start_slot, admission_end_slot) are rejected before they enter the sensing FIFO.
  bool admission_window_enabled = false;
  uint64_t admission_start_slot = 0;
  uint64_t admission_end_slot = 0;
  uint32_t subslot_symbols = 0;
  uint32_t subslot_min_re = 600;
  float subslot_min_snr_db = 0.0f;
};

inline double slot_duration_s(double scs_hz)
{ return 1e-3 / std::max(1.0, scs_hz / 15000.0); }

inline bool source_is_ul(uint32_t source_mask) { return (source_mask & UL_SOURCE_BITS) != 0; }

} // namespace nr_isac
