/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#pragma once

#include "variable_cpi.h"
#include "sync_correction.h"

#include <array>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace nr_isac {

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
  uint32_t covariance_family_count = 0;
  uint64_t covariance_difference_count = 0;
  double current_cpi_variance = 0.0;
  FamilyAlignmentStats detector_alignment;
  SyncEstimate sync;
  CpiPlan plan;
  DetectorResult detector;
  std::vector<Detection> detections;
  std::vector<TrackSnapshot> tracks;
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
