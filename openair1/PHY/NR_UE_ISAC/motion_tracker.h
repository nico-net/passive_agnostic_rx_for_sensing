/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#pragma once

#include "adaptive_clutter_map.h"
#include "pipeline_types.h"

#include <optional>
#include <vector>

namespace nr_isac {

struct MotionTrackerConfig {
  double range_floor_m = 0.0;
  double rate_floor_mps = 0.0;
  double jerk_psd_m2_s5 = 400.0;
  double initial_accel_sigma_mps2 = 25.0;
  double gate_chi2 = 9.21034037197618;
  uint32_t confirm_updates = 3;
  uint32_t confirm_window = 5;
  uint32_t maximum_coasts = 3;
  double maximum_propagation_s = 2.0;
  double nis_ewma_alpha = 0.25;
  bool adaptive_jerk = false;
  double adaptive_jerk_floor = 0.2;
  uint32_t notch_coast_extension = 3;
  double notch_guard_mps = 1.5;
  uint32_t maximum_tracks = 16;
  double birth_score_threshold = 100.0;
  double multipath_shadow_range_m = 12.0;
  double multipath_shadow_rate_mps = 1.2;
  double sidelobe_range_m = 2.0;
  double sidelobe_power_ratio = 0.08;
  double direct_leakage_range_m = 4.5;
  double static_clutter_guard_mps = 1.0;
  bool use_analytic_psf = true;
  double psf_margin_db = 3.0;
  bool use_adaptive_clutter_map = true;
  double clutter_alpha = 0.15;
  double clutter_cfar_margin = 1.5;
  std::optional<double> comb_spacing_mps;
};

struct ConfirmedTrackView {
  uint64_t id = 0;
  double range_m = 0.0;
  double rate_mps = 0.0;
  double sigma_range_m = 0.0;
  double sigma_rate_mps = 0.0;
  double last_score = 0.0;
};

struct Stage1TrackView {
  TrackSnapshot snapshot;
  std::optional<size_t> associated_index;
  double last_score = 0.0;
};

double range_psf_envelope(double delta_m, double resolution_m, double margin_db = 3.0);
double rate_psf_envelope(double delta_mps, double resolution_mps, double margin_db = 3.0);
bool is_aperture_sidelobe(const Detection& candidate, const ConfirmedTrackView& primary,
                          double range_res_m, double rate_res_mps, double margin_db = 3.0,
                          const std::optional<double>& comb_spacing_mps = std::nullopt);
bool is_multipath_shadow(const Detection& candidate, const ConfirmedTrackView& primary,
                         double range_res_m, double rate_res_mps,
                         double maximum_delay_m = 12.0);

class MotionTracker {
public:
  explicit MotionTracker(MotionTrackerConfig config = {});
  ~MotionTracker();
  void reset();
  TrackSnapshot predict_to(double air_time_s) const;
  void update(double air_time_s, const std::vector<Detection>& detections,
              double range_res_m, double rate_res_mps, uint64_t cpi_sequence);
  TrackSnapshot snapshot() const;
  std::vector<TrackSnapshot> snapshots() const;
  std::vector<ConfirmedTrackView> confirmed_tracks() const;
  std::vector<Stage1TrackView> active_tracks() const;
  AdaptiveClutterMap& clutter_map() { return clutter_map_; }
  const AdaptiveClutterMap& clutter_map() const { return clutter_map_; }

  struct Track;

private:
  MotionTrackerConfig config_;
  std::vector<Track> tracks_;
  uint64_t next_id_ = 1;
  std::optional<double> time_s_;
  bool last_lost_ = false;
  AdaptiveClutterMap clutter_map_;
};

} // namespace nr_isac
