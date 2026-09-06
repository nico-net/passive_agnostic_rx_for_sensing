/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this
 * file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.openairinterface.org/?page_id=698
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*! \file openair1/PHY/NR_UE_ISAC/hierarchical_tracker.h
 * \brief Hierarchical Global 3D Tracker (physical ENU coordinates) and Closed-Loop Feedback
 *
 * Architecture:
 * - Stage 1 (Inner Loop): Causal, truth-blind MotionTracker in bistatic (range, range-rate, accel)
 *   coordinates. Provides continuous tracklet formation, Mahalanobis association gating, CFAR prior
 *   weighting, and CPI search predictions.
 * - Stage 2 (Global 3D Loop): Physical 3D Cartesian EKF (position, velocity) in ENU coordinates.
 *   Binds deterministically to Stage-1 confirmed tracks. Ingests receiver array AoA (azimuth,
 *   elevation) with full covariance. Performs closed-form 3D ray-ellipsoid birth, 4D EKF updates,
 *   graceful fallback to 2D range-rate updates during angular fades, and persistent global target IDs.
 * - Track-to-Detector Closed-Loop Feedback:
 *   1. Prior-weighted CFAR thresholding (boosts sensitivity near predicted tracks, suppresses clutter elsewhere).
 *   2. Analytic PSF aperture sidelobe masking (rejects theoretical sinc leakage along range/Doppler axes).
 *   3. Multipath shadow masking (rejects delayed multipath echoes behind confirmed targets).
 *   4. Online adaptive clutter map (EWMA background estimation to reject static clutter).
 */

#ifndef NR_ISAC_HIERARCHICAL_TRACKER_H
#define NR_ISAC_HIERARCHICAL_TRACKER_H

#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "defs_nr_UE_ISAC.h"
#include "target_tracker.h"

namespace nr_isac {

// Standard Chi-square gating thresholds
constexpr double CHI2_4DOF_99 = 13.276704135987622;
constexpr double CHI2_2DOF_99 = 9.21034037197618;
constexpr double CHI2_1DOF_99 = 6.6348966010212145;

/// 3D Vector helper
struct vec3_t {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;

  vec3_t() = default;
  vec3_t(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}

  double norm() const { return std::sqrt(x * x + y * y + z * z); }
  double dot(const vec3_t& o) const { return x * o.x + y * o.y + z * o.z; }

  vec3_t operator+(const vec3_t& o) const { return {x + o.x, y + o.y, z + o.z}; }
  vec3_t operator-(const vec3_t& o) const { return {x - o.x, y - o.y, z - o.z}; }
  vec3_t operator*(double s) const { return {x * s, y * s, z * s}; }
  vec3_t operator/(double s) const { return (s != 0.0) ? vec3_t{x / s, y / s, z / s} : vec3_t{0, 0, 0}; }
};

/// 3D Bistatic Geometry definition
struct bistatic_geometry_3d_t {
  vec3_t tx;
  vec3_t rx;
  double baseline = 0.0;

  bistatic_geometry_3d_t() = default;
  bistatic_geometry_3d_t(const vec3_t& tx_, const vec3_t& rx_)
      : tx(tx_), rx(rx_), baseline((rx_ - tx_).norm()) {}
};

/// Closed-loop analytic PSF & multipath masking helpers
bool is_aperture_sidelobe(float range_m, float vel_mps, float score,
                          float trk_range_m, float trk_vel_mps, float trk_score,
                          float range_res_m, float vel_res_mps, float margin_db = 3.0f);

bool is_multipath_shadow(float range_m, float vel_mps, float score,
                         float trk_range_m, float trk_vel_mps, float trk_score,
                         float range_res_m, float vel_res_mps);

/// Online adaptive clutter map (EWMA background estimation)
class adaptive_clutter_map {
public:
  adaptive_clutter_map(float alpha = 0.05f, float threshold_offset_db = 3.0f)
      : alpha_(alpha), threshold_offset_db_(threshold_offset_db) {}

  void update(float range_m, float vel_mps, float score, float range_res_m, float vel_res_mps);
  bool is_static_clutter(float range_m, float vel_mps, float score, float range_res_m, float vel_res_mps) const;
  void reset();

private:
  float alpha_;
  float threshold_offset_db_;
  std::map<std::pair<int, int>, float> power_map_;
};

/// Stage-1 Bistatic Tracklet (Range, Range-Rate, Acceleration)
class bistatic_tracklet {
public:
  uint32_t track_id = 0;
  double   x[3]     = {0.0, 0.0, 0.0}; // [range_m, range_rate_mps, accel_m_s2]
  double   p[3][3]  = {{0}};
  double   time_s   = 0.0;
  bool     active   = false;
  bool     updated_this_cpi = false;
  int      associated_idx   = -1;
  uint32_t confirmed_updates = 0;
  uint32_t total_updates    = 0;
  uint32_t coast_count      = 0;
  float    last_score       = 0.0f;
  std::string status        = "tentative";

  bistatic_tracklet() = default;
  bistatic_tracklet(uint32_t id, double t_s, double r_m, double rr_mps, float score,
                    double r_res_m, double rr_res_mps);

  void predict(double next_t_s, double jerk_psd = 1.0);
  bool update(double r_m, double rr_mps, float score, double r_res_m, double rr_res_mps,
              int det_idx, double gate_chi2 = CHI2_2DOF_99);
  void coast();
};

/// Stage-2 Global 3D Cartesian EKF Track (ENU coordinates)
class global_3d_track {
public:
  uint32_t track_id = 0;
  double   x[6]     = {0.0}; // [x, y, z, vx, vy, vz] in ENU
  double   p[6][6]  = {{0}};
  double   time_s   = 0.0;
  std::string status = "confirmed";
  uint32_t coast_count       = 0;
  uint32_t total_updates     = 0;
  uint32_t confirmed_updates = 0;
  bool     updated_this_cpi  = false;
  int      associated_idx    = -1;
  double   nis               = 0.0;
  double   nis_ewma          = 0.0;
  double   accel_var         = 10.0;
  double   stage1_range_m    = 0.0;
  double   stage1_range_rate_mps = 0.0;
  bistatic_geometry_3d_t geom;

  global_3d_track() = default;
  global_3d_track(uint32_t id, double t_s, const bistatic_geometry_3d_t& g,
                  double r_m, double rr_mps, double az_deg, double el_deg,
                  double r_res_m, double rr_res_mps, double az_std_deg = 2.0, double el_std_deg = 5.0,
                  int det_idx = -1);

  void predict(double next_t_s);
  bool update(double r_m, double rr_mps, bool with_angles, double az_deg, double el_deg,
              double r_res_m, double rr_res_mps, double az_std_deg, double el_std_deg,
              int det_idx, double s1_r, double s1_rr);
  void coast();
  sensing_track_t to_sensing_track() const;

  // Measurement model & Jacobian
  void evaluate_model(const double state[6], double out[4], bool with_angles) const;
  void evaluate_jacobian(const double state[6], double H[4][6], bool with_angles) const;
};

/// Complete Hierarchical 3D Global Tracker Configuration
struct hierarchical_tracker_config_t {
  double gate_chi2_4d = CHI2_4DOF_99;
  double gate_chi2_2d = CHI2_2DOF_99;
  uint32_t max_coasts_3d = 80;
  uint32_t confirm_updates = 3;
  uint32_t confirm_window = 5;
  double jerk_psd = 400.0;
  double max_accel_var = 100.0;
  float  clutter_map_alpha = 0.05f;
  float  clutter_threshold_offset_db = 3.0f;
  float  psf_sidelobe_margin_db = 3.0f;
};

/// Complete Hierarchical 3D Global Tracker Class
class hierarchical_tracker {
public:
  hierarchical_tracker(const bistatic_geometry_3d_t& geom,
                       const hierarchical_tracker_config_t& cfg = hierarchical_tracker_config_t());

  void reset();

  /// Feed detections and perform Stage 1 + Stage 2 hierarchical tracking
  const std::vector<sensing_track_t>& update(
      const std::vector<sensing_detection_t>& detections,
      double time_s,
      double range_res_m,
      double vel_res_mps,
      double dwell_s = 0.032);

  /// Get currently confirmed Stage-1 tracklets (for detector feedback)
  std::vector<bistatic_tracklet> get_prior_confirmed_tracklets() const;

  /// Access adaptive clutter map
  adaptive_clutter_map& clutter_map() { return clutter_map_; }
  const adaptive_clutter_map& clutter_map() const { return clutter_map_; }

  /// Access last emitted tracks
  const std::vector<sensing_track_t>& last_tracks() const { return last_sensing_tracks_; }

  /// Check if a candidate detection should be rejected by closed-loop feedback
  bool should_reject_detection(float range_m, float vel_mps, float score,
                               float range_res_m, float vel_res_mps,
                               bool& near_track) const;

private:
  bistatic_geometry_3d_t geom_;
  hierarchical_tracker_config_t cfg_;
  adaptive_clutter_map clutter_map_;

  std::vector<bistatic_tracklet> s1_tracks_;
  std::map<uint32_t, global_3d_track> g_tracks_;
  std::map<uint32_t, uint32_t> s1_to_global_;
  std::vector<sensing_track_t> last_sensing_tracks_;

  uint32_t next_s1_id_ = 1;
  uint32_t next_g_id_ = 1;
  double prev_time_s_ = -1.0;
};

} // namespace nr_isac

#endif // NR_ISAC_HIERARCHICAL_TRACKER_H
