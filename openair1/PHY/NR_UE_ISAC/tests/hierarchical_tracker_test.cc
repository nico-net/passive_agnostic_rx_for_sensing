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

/*! \file openair1/PHY/NR_UE_ISAC/tests/hierarchical_tracker_test.cc
 * \brief Unit tests for Hierarchical Global 3D Tracker and Closed-Loop Feedback.
 */

#include <gtest/gtest.h>
#include <cmath>
#include <vector>

#include "hierarchical_tracker.h"

using namespace nr_isac;

TEST(HierarchicalTracker, ApertureSidelobeRejection)
{
  const float range_res = 3.0f;
  const float vel_res   = 0.5f;

  float trk_range = 100.0f;
  float trk_vel   = 10.0f;
  float trk_score = 100.0f;

  // Candidate right in the primary mainlobe -> not a sidelobe
  EXPECT_FALSE(is_aperture_sidelobe(100.2f, 10.1f, 90.0f, trk_range, trk_vel, trk_score, range_res, vel_res));

  // Candidate along the same range but 5 velocity bins away, with attenuated score consistent with sinc envelope -> rejected!
  float cand_vel = 10.0f + 5.0f * vel_res;
  float cand_score = 0.5f; // low power sidelobe
  EXPECT_TRUE(is_aperture_sidelobe(100.0f, cand_vel, cand_score, trk_range, trk_vel, trk_score, range_res, vel_res));

  // Candidate at same offset but very high score (real target) -> NOT rejected
  EXPECT_FALSE(is_aperture_sidelobe(100.0f, cand_vel, 80.0f, trk_range, trk_vel, trk_score, range_res, vel_res));
}

TEST(HierarchicalTracker, MultipathShadowRejection)
{
  const float range_res = 3.0f;
  const float vel_res   = 0.5f;

  float trk_range = 60.0f;
  float trk_vel   = -5.0f;
  float trk_score = 120.0f;

  // Trailing delayed return (1 bin delayed = +3m), matching velocity, attenuated -> rejected
  EXPECT_TRUE(is_multipath_shadow(63.0f, -5.0f, 40.0f, trk_range, trk_vel, trk_score, range_res, vel_res));

  // Leading return (-3m) -> NOT a multipath shadow
  EXPECT_FALSE(is_multipath_shadow(57.0f, -5.0f, 40.0f, trk_range, trk_vel, trk_score, range_res, vel_res));
}

TEST(HierarchicalTracker, AdaptiveClutterMap)
{
  adaptive_clutter_map cmap(0.1f, 3.0f);
  const float range_res = 3.0f;
  const float vel_res   = 0.5f;

  // Direct path leakage at 1.0m (within 1.5 cells) -> rejected
  EXPECT_TRUE(cmap.is_static_clutter(1.0f, 0.0f, 50.0f, range_res, vel_res));

  // Unmapped cell at 80m, zero velocity, initial low observation
  cmap.update(80.0f, 0.0f, 20.0f, range_res, vel_res);

  // Subsequent detection at same cell below learned floor + margin -> rejected as static clutter
  EXPECT_TRUE(cmap.is_static_clutter(80.0f, 0.0f, 25.0f, range_res, vel_res));

  // Moving target at same range (vel = 8.0 m/s) -> NOT rejected
  EXPECT_FALSE(cmap.is_static_clutter(80.0f, 8.0f, 25.0f, range_res, vel_res));
}

TEST(HierarchicalTracker, RayEllipsoidBirth3D)
{
  // Surveyed Tx and Rx
  vec3_t tx(300.0, 200.0, 25.0);
  vec3_t rx(0.0, 0.0, 10.0);
  bistatic_geometry_3d_t geom(tx, rx);

  // Ground truth target
  vec3_t target(120.0, 160.0, 15.0);
  vec3_t r_tx = target - tx;
  vec3_t r_rx = target - rx;

  double total_range = r_tx.norm() + r_rx.norm();
  double excess_range = total_range - geom.baseline;
  double az_deg = std::atan2(r_rx.y, r_rx.x) * 180.0 / M_PI;
  double el_deg = std::atan2(r_rx.z, std::hypot(r_rx.x, r_rx.y)) * 180.0 / M_PI;

  vec3_t grad = (r_tx / r_tx.norm()) + (r_rx / r_rx.norm());
  vec3_t target_vel(10.0, -5.0, 0.0);
  double range_rate = grad.dot(target_vel);

  // Construct 3D EKF track from measurement
  global_3d_track trk(1, 0.0, geom, excess_range, range_rate, az_deg, el_deg, 3.0, 0.5, 1.0, 2.0, 0);

  // Position recovered from ray-ellipsoid closed form must match ground truth target position within millimeters!
  EXPECT_NEAR(trk.x[0], target.x, 1e-2);
  EXPECT_NEAR(trk.x[1], target.y, 1e-2);
  EXPECT_NEAR(trk.x[2], target.z, 1e-2);
}

TEST(HierarchicalTracker, PersistenceAndReacquisition)
{
  vec3_t tx(200.0, 0.0, 20.0);
  vec3_t rx(0.0, 0.0, 10.0);
  bistatic_geometry_3d_t geom(tx, rx);

  hierarchical_tracker_config_t cfg;
  hierarchical_tracker tracker(geom, cfg);

  double t = 0.0;
  double dt = 0.1;

  // Move target from (50, 50, 15) with velocity (5, 0, 0)
  vec3_t pos(50.0, 50.0, 15.0);
  vec3_t vel(5.0, 0.0, 0.0);

  uint32_t confirmed_track_id = 0;

  // 1. Run 5 CPIs of detections -> track confirms
  for (int cpi = 0; cpi < 5; cpi++) {
    pos = pos + vel * dt;
    t += dt;

    vec3_t r_tx = pos - geom.tx;
    vec3_t r_rx = pos - geom.rx;
    double dR = r_tx.norm() + r_rx.norm() - geom.baseline;
    vec3_t grad = (r_tx / r_tx.norm()) + (r_rx / r_rx.norm());
    double rr = grad.dot(vel);
    double az = std::atan2(r_rx.y, r_rx.x) * 180.0 / M_PI;
    double el = std::atan2(r_rx.z, std::hypot(r_rx.x, r_rx.y)) * 180.0 / M_PI;

    sensing_detection_t det;
    det.range_m = (float)dR;
    det.vel_mps = (float)rr;
    det.snr_db = 25.0f;
    det.azimuth_valid = true;
    det.azimuth_deg = (float)az;
    det.azimuth_std_deg = 1.0f;
    det.elevation_valid = true;
    det.elevation_deg = (float)el;
    det.elevation_std_deg = 2.0f;

    const auto& tracks = tracker.update({det}, t, 3.0, 0.5, 0.032);
    if (!tracks.empty()) {
      confirmed_track_id = tracks[0].track_id;
    }
  }

  EXPECT_GT(confirmed_track_id, 0u);

  // 2. Coast for 2 CPIs (no detections, simulated fade)
  for (int cpi = 0; cpi < 2; cpi++) {
    pos = pos + vel * dt;
    t += dt;
    const auto& tracks = tracker.update({}, t, 3.0, 0.5, 0.032);
    EXPECT_FALSE(tracks.empty());
    EXPECT_EQ(tracks[0].track_id, confirmed_track_id);
    EXPECT_FALSE(tracks[0].updated); // coasting
  }

  // 3. Re-acquire target: detection resumes
  pos = pos + vel * dt;
  t += dt;
  vec3_t r_tx = pos - geom.tx;
  vec3_t r_rx = pos - geom.rx;
  double dR = r_tx.norm() + r_rx.norm() - geom.baseline;
  vec3_t grad = (r_tx / r_tx.norm()) + (r_rx / r_rx.norm());
  double rr = grad.dot(vel);
  double az = std::atan2(r_rx.y, r_rx.x) * 180.0 / M_PI;
  double el = std::atan2(r_rx.z, std::hypot(r_rx.x, r_rx.y)) * 180.0 / M_PI;

  sensing_detection_t det;
  det.range_m = (float)dR;
  det.vel_mps = (float)rr;
  det.snr_db = 25.0f;
  det.azimuth_valid = true;
  det.azimuth_deg = (float)az;
  det.azimuth_std_deg = 1.0f;
  det.elevation_valid = true;
  det.elevation_deg = (float)el;
  det.elevation_std_deg = 2.0f;

  const auto& tracks = tracker.update({det}, t, 3.0, 0.5, 0.032);
  ASSERT_EQ(tracks.size(), 1u);
  // CRUCIAL: Global track ID is preserved across coast and re-acquisition! No fragmentation!
  EXPECT_EQ(tracks[0].track_id, confirmed_track_id);
  EXPECT_TRUE(tracks[0].updated);
  EXPECT_NEAR(tracks[0].pos_x, pos.x, 1.0);
  EXPECT_NEAR(tracks[0].pos_y, pos.y, 1.0);
}

int main(int argc, char** argv)
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
