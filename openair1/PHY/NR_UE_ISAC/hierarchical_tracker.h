/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#pragma once

#include "enu_tracker.h"
#include "motion_tracker.h"

#include <map>
#include <memory>
#include <vector>

namespace nr_isac {

struct HierarchicalTrackerConfig {
  double gate_chi2_4d = 13.276704135987622;
  double gate_chi2_2d = 9.21034037197618;
  uint32_t maximum_3d_coasts = 80;
  uint32_t confirm_updates = 3;
  uint32_t confirm_window = 5;
  double maximum_acceleration_variance = 100.0;
  double initial_maximum_velocity_variance = 100.0;
  double adaptation_alpha = 0.25;
};

/** Native transcription of hierarchical_tracker.py: Stage-1 range/rate plus persistent ENU EKFs. */
class HierarchicalEnuTracker {
public:
  HierarchicalEnuTracker(BistaticGeometry geometry,
                         HierarchicalTrackerConfig config = {},
                         MotionTrackerConfig inner_config = {});
  void reset();
  TrackSnapshot predict_to(double air_time_s) const;
  void update(double air_time_s, const std::vector<Detection>& detections,
              double range_resolution_m, double rate_resolution_mps,
              uint64_t cpi_sequence, double dwell_s);
  std::vector<TrackSnapshot> snapshots() const;
  TrackSnapshot snapshot() const;
  std::vector<ConfirmedTrackView> confirmed_tracks() const
  { return motion_tracker_.confirmed_tracks(); }
  AdaptiveClutterMap& clutter_map() { return motion_tracker_.clutter_map(); }

private:
  BistaticGeometry geometry_;
  HierarchicalTrackerConfig config_;
  MotionTrackerConfig inner_config_;
  MotionTracker motion_tracker_;
  std::map<uint64_t, std::unique_ptr<EnuTrack>> global_tracks_;
  std::map<uint64_t, uint64_t> stage1_to_global_;
  std::map<uint64_t, Detection> valid_aoa_cache_;
  uint64_t next_global_id_ = 1;
};

} // namespace nr_isac
