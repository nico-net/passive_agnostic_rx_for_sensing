/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#pragma once

#include "nr_isac.h"
#include "small_matrix.h"

#include <array>
#include <complex>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace nr_isac {

constexpr double C_MPS = 299792458.0;
constexpr double PI = 3.141592653589793238462643383279502884;
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
};

struct DetectorResult {
  Axes axes;
  std::vector<CleanComponent> components;
  std::vector<CleanComponent> objects;
  std::vector<double> initial_likelihood; // range-major [range][Doppler], raw score
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

struct PipelineConfig {
  uint32_t num_ues = 1; // supported runtime range: 1..4; decoding stays UE-agnostic here
  uint32_t sources_mask = 1u << NR_ISAC_SRC_CSI_RS;
  std::vector<double> duration_bank_s{0.008, 0.016, 0.024, 0.032};
  uint32_t bootstrap_duration_index = 3;
  double minimum_dwell_s = 0.006;
  double maximum_dwell_s = 0.032;
  uint32_t minimum_rows = 16;
  uint32_t maximum_rows = 512;
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
  Vec3 tx_position;
  Vec3 rx_position;
  std::string rx_id = "rx1";
  std::string illuminator_id = "gnb1";
  std::string out_path = "/tmp/oaiue_sensing";
  std::string report_path;
  std::string report_endpoint;
  bool capture_rvm = false;
  uint32_t subslot_symbols = 0;
  uint32_t subslot_min_re = 600;
  float subslot_min_snr_db = 0.0f;
};

inline double slot_duration_s(double scs_hz)
{ return 1e-3 / std::max(1.0, scs_hz / 15000.0); }

inline bool source_is_ul(uint32_t source_mask) { return (source_mask & UL_SOURCE_BITS) != 0; }

} // namespace nr_isac
