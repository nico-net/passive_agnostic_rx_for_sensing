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
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

/*! \file openair1/PHY/NR_UE_ISAC/tests/target_tracker_test.cc
 * \brief Offline tests for the 2-state constant-velocity Kalman track (target_tracker.{h,cc}).
 *
 * Scenario mirrors tests/sensing_sim's real one so the numbers are meaningful rather than abstract:
 * a target at ~89 m opening at ~6.4 m/s, CPIs ~0.116 s apart, detections quantized to a 3.05 m range
 * bin (the 100 MHz / 273 PRB grid). Each test isolates one behaviour the file comment claims.
 */

#include <cmath>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "defs_nr_UE_ISAC.h"
#include "target_tracker.h"

extern "C" {
#include "common/utils/LOG/log.h"
#include "common/config/config_userapi.h"
}

extern "C" configmodule_interface_t* uniqCfg = nullptr;
extern "C" void exit_function(const char* file, const char* function, const int line, const char* s, const int assert)
{
  (void)file; (void)function; (void)line; (void)s; (void)assert;
  abort();
}

using namespace nr_isac;

namespace {

constexpr double RANGE_RES_M = 3.052; // 100 MHz / 273 PRB grid
constexpr double DT_S        = 0.116; // observed CPI cadence
constexpr double R0_M        = 88.7;  // trajectory start (differential bistatic range)
constexpr double VEL_MPS     = 6.4;

nr_isac_args_t make_args()
{
  nr_isac_args_t a;
  a.track_enable          = true;
  a.track_r_var_m2        = 2.6f;
  a.track_q_accel         = 0.0025f;
  a.track_gate_sigma      = 3.0f;
  a.track_init_vel_var_m2s2 = 25.0f;
  a.track_max_coast       = 5;
  return a;
}

/// A detection at a quantized range, as range_doppler would report it.
sensing_detection_t det(double range_m, double vel_mps, double snr_db)
{
  sensing_detection_t d;
  d.range_bin = (uint32_t)std::lround(range_m / RANGE_RES_M);
  d.range_m   = (float)(d.range_bin * RANGE_RES_M); // quantized, exactly like the real pipeline
  d.vel_mps   = (float)vel_mps;
  d.snr_db    = (float)snr_db;
  return d;
}

double truth_at(int k) { return R0_M + VEL_MPS * DT_S * k; }

} // namespace

// The headline claim: filtering a QUANTIZED measurement stream recovers sub-bin range accuracy,
// beating the raw detections it is fed.
TEST(target_tracker, beats_raw_quantized_detections)
{
  target_tracker trk(make_args());
  double err_raw = 0.0, err_trk = 0.0;
  int    n = 0;
  for (int k = 0; k < 60; k++) {
    const double t = truth_at(k);
    std::vector<sensing_detection_t> d{det(t, VEL_MPS, 20.0)};
    const sensing_track_t& s = trk.update(d, k == 0 ? 0.0 : DT_S);
    if (k < 10) {
      continue; // let the filter converge before scoring
    }
    err_raw += std::abs((double)d[0].range_m - t);
    err_trk += std::abs((double)s.range_m - t);
    n++;
  }
  err_raw /= n;
  err_trk /= n;
  EXPECT_LT(err_trk, err_raw) << "tracked=" << err_trk << " m vs raw=" << err_raw << " m";
  // Quantization alone gives a mean |error| of Delta/4 = 0.76 m for a uniformly-swept target; the
  // filter should land well inside that.
  EXPECT_LT(err_trk, 0.5 * RANGE_RES_M / 2.0);
}

// The track must survive CPIs with no detection at all by coasting on its prediction, and pick the
// target back up when it returns -- not drop or jump.
TEST(target_tracker, coasts_through_missed_cpis_and_reacquires)
{
  target_tracker trk(make_args());
  for (int k = 0; k < 15; k++) {
    trk.update({det(truth_at(k), VEL_MPS, 20.0)}, k == 0 ? 0.0 : DT_S);
  }
  // Three CPIs with nothing detected.
  for (int k = 15; k < 18; k++) {
    const sensing_track_t& s = trk.update({}, DT_S);
    EXPECT_TRUE(s.active) << "track dropped while coasting at k=" << k;
    EXPECT_FALSE(s.updated);
    EXPECT_EQ(s.coast_count, (uint32_t)(k - 14));
    // Coasting must keep extrapolating, not freeze.
    EXPECT_NEAR((double)s.range_m, truth_at(k), 3.0 * RANGE_RES_M) << "k=" << k;
  }
  const sensing_track_t& s = trk.update({det(truth_at(18), VEL_MPS, 20.0)}, DT_S);
  EXPECT_TRUE(s.updated) << "failed to re-acquire after the gap";
  EXPECT_EQ(s.coast_count, 0u);
  EXPECT_NEAR((double)s.range_m, truth_at(18), 2.0 * RANGE_RES_M);
}

// A false alarm far from the prediction must be rejected by the gate rather than dragging the track.
TEST(target_tracker, gate_rejects_far_false_alarm)
{
  target_tracker trk(make_args());
  for (int k = 0; k < 20; k++) {
    trk.update({det(truth_at(k), VEL_MPS, 20.0)}, k == 0 ? 0.0 : DT_S);
  }
  const double before = trk.last().range_m;
  // A much STRONGER detection 600 m away -- SNR alone would pick it; the gate must not.
  const sensing_track_t& s = trk.update({det(truth_at(20) + 600.0, -12.0, 40.0)}, DT_S);
  EXPECT_FALSE(s.updated) << "gate accepted a 600 m outlier";
  EXPECT_NEAR((double)s.range_m, before + VEL_MPS * DT_S, 1.0) << "track was dragged by the outlier";
}

// With the correct target present alongside a stronger false alarm, association must follow the
// PREDICTION, not the SNR ranking -- the thing raw "top detection" consumption gets wrong.
TEST(target_tracker, associates_by_prediction_not_by_snr)
{
  target_tracker trk(make_args());
  for (int k = 0; k < 20; k++) {
    trk.update({det(truth_at(k), VEL_MPS, 20.0)}, k == 0 ? 0.0 : DT_S);
  }
  std::vector<sensing_detection_t> d{det(truth_at(20) + 900.0, 3.0, 45.0),  // stronger, far
                                     det(truth_at(20), VEL_MPS, 12.0)};      // weaker, correct
  const sensing_track_t& s = trk.update(d, DT_S);
  EXPECT_TRUE(s.updated);
  EXPECT_NEAR((double)s.range_m, truth_at(20), 2.0 * RANGE_RES_M);
}

// After max_coast consecutive misses the track is dropped rather than left wandering, and the next
// detection starts a fresh one.
TEST(target_tracker, drops_after_max_coast_then_reinitialises)
{
  nr_isac_args_t a = make_args();
  a.track_max_coast = 3;
  target_tracker trk(a);
  for (int k = 0; k < 10; k++) {
    trk.update({det(truth_at(k), VEL_MPS, 20.0)}, k == 0 ? 0.0 : DT_S);
  }
  for (uint32_t i = 0; i < a.track_max_coast; i++) {
    EXPECT_TRUE(trk.update({}, DT_S).active) << "dropped too early at coast " << i;
  }
  EXPECT_FALSE(trk.update({}, DT_S).active) << "track should be dropped past max_coast";
  const sensing_track_t& s = trk.update({det(500.0, 2.0, 20.0)}, DT_S);
  EXPECT_TRUE(s.active);
  EXPECT_NEAR((double)s.range_m, 500.0, RANGE_RES_M) << "did not re-initialise on the new detection";
}

// Filter consistency: with R and q matching the data that generated it, the normalized innovation
// squared should average ~1 (chi-square, 1 DoF). This is the tuning criterion the header documents,
// so it is asserted rather than left as folklore.
TEST(target_tracker, nis_is_consistent_when_r_matches_measurement_noise)
{
  nr_isac_args_t a = make_args();
  const double   sigma = 1.6; // matches the measured live residual std
  a.track_r_var_m2 = (float)(sigma * sigma + RANGE_RES_M * RANGE_RES_M / 12.0);

  target_tracker trk(a);
  std::mt19937 rng(4242);
  std::normal_distribution<double> noise(0.0, sigma);
  double nis_sum = 0.0;
  int    n = 0;
  for (int k = 0; k < 200; k++) {
    const double z = truth_at(k) + noise(rng);
    const sensing_track_t& s = trk.update({det(z, VEL_MPS, 20.0)}, k == 0 ? 0.0 : DT_S);
    if (k > 20 && s.updated) {
      nis_sum += s.nis;
      n++;
    }
  }
  const double mean_nis = nis_sum / n;
  // Wide bounds: this asserts the filter is not grossly over/under-confident, which is what the
  // diagnostic is for -- not that it hits exactly 1.0 on a finite sample.
  EXPECT_GT(mean_nis, 0.2) << "mean NIS=" << mean_nis << " (R likely too large)";
  EXPECT_LT(mean_nis, 3.0) << "mean NIS=" << mean_nis << " (R/q likely too small)";
}

// Adaptive q: under a sustained acceleration the fixed-q filter of the earlier tests would coast and
// drop (verified live, var_report.html); with adaptation enabled q inflates via the NIS EWMA, the
// gate widens, and the track survives the maneuver AND keeps following. Then, once the target returns
// to constant velocity, q_mult must decay back toward 1 on its own -- no per-route tuning.
TEST(target_tracker, adaptive_q_survives_acceleration_then_relaxes)
{
  nr_isac_args_t a = make_args();
  a.track_q_adapt_enable = true;
  a.track_max_coast      = 5;
  target_tracker trk(a);

  // Phase A: constant velocity -- q_mult should sit at ~1.
  int k = 0;
  for (; k < 25; k++) {
    trk.update({det(truth_at(k), VEL_MPS, 20.0)}, k == 0 ? 0.0 : DT_S);
  }
  EXPECT_LT(trk.last().q_mult, 2.0f) << "q inflated with no maneuver present";

  // Phase B: a hard acceleration leg -- range now advances quadratically, well beyond the CV model.
  double r = truth_at(k - 1), v = VEL_MPS;
  const double accel = 6.0; // m/s^2, comparable to the live 8 m/s leg's onset
  bool dropped = false;
  double max_qmult = 1.0;
  for (int j = 0; j < 25; j++, k++) {
    v += accel * DT_S;
    r += v * DT_S;
    const sensing_track_t& s = trk.update({det(r, v, 20.0)}, DT_S);
    if (!s.active) {
      dropped = true;
    }
    max_qmult = std::max(max_qmult, (double)s.q_mult);
  }
  EXPECT_GT(max_qmult, 3.0) << "adaptation never engaged during the acceleration";
  // ROBUSTNESS, not tight tracking: a 2-state constant-velocity filter cannot follow a sustained hard
  // acceleration closely (velocity is frozen while it coasts, and even a CV update lags under constant
  // accel) -- that needs acceleration IN THE STATE (3-state / IMM, a later phase). What the adaptive-q
  // + manoeuvre/gone split guarantees is that the track is never DROPPED while the target is present
  // and keeps re-acquiring, so it is ready to snap back the moment the motion eases. Both are asserted;
  // sub-bin accuracy DURING the hard leg is deliberately NOT.
  EXPECT_FALSE(dropped) << "adaptive filter lost the track through the maneuver";
  EXPECT_TRUE(trk.last().active);

  // Phase C: back to constant velocity -- the filter must reconverge TIGHTLY (proving both that it
  // stayed locked on the real target and that q relaxes so the track tightens again), and q_mult must
  // decay back toward baseline on its own.
  v = 12.0; // whatever rate it ended the maneuver at, continue at constant velocity
  double rr = r;
  for (int j = 0; j < 60; j++, k++) {
    rr += v * DT_S;
    trk.update({det(rr, v, 20.0)}, DT_S);
  }
  EXPECT_LT(std::abs((double)trk.last().range_m - rr), 2.0 * RANGE_RES_M)
      << "did not reconverge after the maneuver ended";
  EXPECT_LT(trk.last().q_mult, 2.5f)
      << "q_mult stayed inflated after the maneuver ended (q_mult=" << trk.last().q_mult << ")";
}

// Constant-acceleration (3-state) model tracks a CURVING target far tighter than constant-velocity.
// Circular motion has continuous centripetal acceleration -- exactly the case a CV model can only lag
// (it reacts to curvature one step late) and a CA model predicts (acceleration is in the state). Same
// synthetic bistatic-range profile of a circle used offline; asserts CA's error is a large multiple
// smaller than CV's on the identical track.
TEST(target_tracker, ca_model_tracks_curve_tighter_than_cv)
{
  auto run = [](const std::string& model) {
    nr_isac_args_t a = make_args();
    a.track_model            = model;
    a.track_rv_var_m2s2      = 1.0f;
    a.track_q_accel          = (model == "ca") ? 0.5f : 0.0025f;
    a.track_q_adapt_enable   = true;
    a.track_q_mult_max       = 1000.0f;
    a.track_q_adapt_window_sigma = 12.0f;
    a.track_init_acc_var     = 100.0f;
    target_tracker trk(a);
    const double Cx = 50, Cy = 140, R = 35, w = 0.42;
    double err = 0.0; int n = 0;
    for (int k = 0; k < 70; k++) {
      const double t = k * DT_S, ang = -M_PI / 2 + w * t;
      const double x = Cx + R * std::cos(ang), y = Cy + R * std::sin(ang);
      const double vx = -R * w * std::sin(ang), vy = R * w * std::cos(ang);
      const double d1 = std::hypot(x, y), d2 = std::hypot(x - 100, y);
      const double dR = d1 + d2 - 100.0;
      const double rr = (x * vx + y * vy) / d1 + ((x - 100) * vx + y * vy) / d2;
      const sensing_track_t& st = trk.update({det(dR, rr, 20.0)}, k == 0 ? 0.0 : DT_S);
      if (t > 1.0 && st.active) { err += std::abs((double)st.range_m - dR); n++; }
    }
    return err / n;
  };
  const double cv = run("cv"), ca = run("ca");
  EXPECT_LT(ca, cv * 0.5) << "CA (" << ca << " m) should clearly beat CV (" << cv << " m) on a curve";
  EXPECT_LT(ca, 1.0) << "CA error on the curve should be sub-metre, got " << ca << " m";
}

int main(int argc, char** argv)
{
  logInit();
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
