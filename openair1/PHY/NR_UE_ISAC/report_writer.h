/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#pragma once

#include "variable_cpi.h"
#include "sync_correction.h"
#include "causal_clutter_filter.h"
#include "multistatic_imm_tracker.h"

#include <array>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace nr_isac {

struct SpatialReceiverReport {
  std::string receiver_id;
  Vec3 receiver_position;
  SyncEstimate sync;
  uint32_t covariance_family_count = 0;
  uint64_t covariance_difference_count = 0;
  double current_cpi_variance = 0.0;
  FamilyAlignmentStats detector_alignment;
  CausalClutterStats causal_clutter;
  DetectorResult detector;
  std::vector<Detection> detections;
  // (2026-09-22) nested long dwells: detections from the merged rows of the last 2 / 4 CPIs of this
  // receiver (150 / 300 ms), reported at the closing CPI with the merged window's dwell and midpoint.
  struct LongDwell { double dwell_s = 0.0; double mid_time_slots = 0.0; uint32_t rows = 0; std::vector<Detection> detections; };
  std::vector<LongDwell> long_dwells;
  std::vector<LongDwell> uplink_long_dwells;
  bool uplink_present = false;
  uint64_t uplink_session_id = 0;
  bool uplink_valid = false;
  bool uplink_search_complete = false;
  std::string uplink_error;
  SyncEstimate uplink_sync;
  bool uplink_direct_path_rate_valid = false;
  double uplink_direct_path_range_rate_mps = 0.0;
  double uplink_direct_path_rate_variance_mps2 =
      std::numeric_limits<double>::infinity();
  uint32_t uplink_covariance_family_count = 0;
  uint64_t uplink_covariance_difference_count = 0;
  double uplink_current_cpi_variance = 0.0;
  FamilyAlignmentStats uplink_detector_alignment;
  CausalClutterStats uplink_causal_clutter;
  DetectorResult uplink_detector;
  std::vector<Detection> uplink_detections;
};

struct PipelineReport {
  uint64_t cpi_sequence = 0;
  int64_t start_utc_ns = 0;
  int64_t cpi_duration_ns = 0;
  int64_t first_row_time_ns = 0;
  int64_t last_row_time_ns = 0;
  double midpoint_air_time_s = 0.0;
  uint32_t sources_mask = 0;
  std::array<uint64_t, NR_ISAC_SRC_COUNT> source_occurrences{};
  uint64_t dropped_submissions = 0;
  uint64_t dropped_cpis = 0;
  uint64_t discarded_pending_rows = 0;
  uint64_t discarded_pending_intervals = 0;
  uint64_t stale_submissions = 0;
  uint32_t spatial_frontend_jobs = 0;
  uint32_t spatial_frontend_worker_count = 0;
  uint32_t spatial_frontend_peak_concurrency = 0;
  double spatial_frontend_wall_s = 0.0;
  uint32_t covariance_family_count = 0;
  uint64_t covariance_difference_count = 0;
  double current_cpi_variance = 0.0;
  FamilyAlignmentStats detector_alignment;
  SyncEstimate sync;
  CpiPlan plan;
  DetectorResult detector;
  std::vector<Detection> detections;
  bool uplink_present = false;
  uint64_t uplink_session_id = 0;
  std::vector<uint64_t> uplink_session_ids;
  bool uplink_valid = false;
  std::string uplink_error;
  SyncEstimate uplink_sync;
  uint32_t uplink_covariance_family_count = 0;
  uint64_t uplink_covariance_difference_count = 0;
  double uplink_current_cpi_variance = 0.0;
  FamilyAlignmentStats uplink_detector_alignment;
  DetectorResult uplink_detector;
  std::vector<Detection> uplink_detections;
  std::vector<TrackSnapshot> uplink_tracks;
  std::vector<TrackSnapshot> tracks;
  std::vector<SpatialReceiverReport> spatial_receivers;
  // Complete per-session UL telemetry. spatial_receivers keeps only the first session in its
  // legacy uplink_dtd_dfs member; this collection prevents additional RNTIs from becoming
  // invisible to validation and operations.
  std::vector<std::vector<SpatialReceiverReport>> uplink_session_receivers;
  MultistaticTrackerProcessingStats multistatic_tracker_processing;
};

std::string source_name(nr_isac_source_t source);
std::string sources_name(uint32_t source_mask);
std::string build_report_json(const PipelineReport& report, const PipelineConfig& config);

class ReportWriter {
public:
  explicit ReportWriter(const PipelineConfig& config);
  ~ReportWriter();
  void emit(const PipelineReport& report);

private:
  PipelineConfig config_;
  std::ofstream file_;
#ifdef ENABLE_ZEROMQ
  void* zmq_context_ = nullptr;
  void* zmq_socket_ = nullptr;
#endif
};

} // namespace nr_isac
