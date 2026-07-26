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

/*! \file openair1/PHY/NR_UE_ISAC/tests/multi_target_tracker_test.cc
 * \brief Offline tests for multi-object tracking (multi_target_tracker.{h,cc}), Phase 2 Part 1.
 *
 * Two LINEARLY MOVING, CONSTANT-VELOCITY targets on the 100 MHz / 273 PRB grid (same quantization as
 * the real pipeline and the single-track tests). Each test isolates one MOT behaviour the handover
 * (PHASE2_MOT_MULTIUE_HANDOVER.md Part 1) calls for: two-target association under gating, track-ID
 * stability across a range crossing, and spurious-track rejection via the M-of-N initiation policy.
 */

#include <cmath>
#include <vector>

#include <gtest/gtest.h>

#include "defs_nr_UE_ISAC.h"
#include "multi_target_tracker.h"

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

nr_isac_args_t make_args()
{
  nr_isac_args_t a;
  a.track_enable            = true;
  a.track_r_var_m2          = 2.6f;
  a.track_q_accel           = 0.0025f;
  a.track_gate_sigma        = 3.0f;
  a.track_init_vel_var_m2s2 = 25.0f;
  a.track_max_coast         = 5;
  a.track_max_tracks        = 16;
  a.track_confirm_m         = 3;
  a.track_confirm_n         = 5;
  a.track_assoc_gate_sigma  = 5.0f;
  return a;
}

/// A detection at a quantized range, as range_doppler would report it.
sensing_detection_t det(double range_m, double vel_mps, double snr_db)
{
  sensing_detection_t d;
  d.range_bin = (uint32_t)std::lround(range_m / RANGE_RES_M);
  d.range_m   = (float)(d.range_bin * RANGE_RES_M);
  d.vel_mps   = (float)vel_mps;
  d.snr_db    = (float)snr_db;
  return d;
}

/// A constant-velocity target: differential bistatic range r0 + v*t.
struct cv_target {
  double r0, v;
  double at(int k) const { return r0 + v * DT_S * k; }
};

/// Find the confirmed track nearest a given range in a tracker output snapshot; nullptr if none.
const sensing_track_t* nearest_track(const std::vector<sensing_track_t>& ts, double range_m)
{
  const sensing_track_t* best = nullptr;
  double bd = 1e18;
  for (const auto& t : ts) {
    const double d = std::abs((double)t.range_m - range_m);
    if (d < bd) { bd = d; best = &t; }
  }
  return best;
}

} // namespace

// Two well-separated CV targets (different range AND velocity throughout) must each get their own
// confirmed track, tracked to sub-bin accuracy -- the basic "does the plumbing work" MOT case.
TEST(multi_target_tracker, two_separated_targets_each_get_a_track)
{
  multi_target_tracker trk(make_args());
  cv_target A{80.0, 6.0};    // opening
  cv_target B{200.0, -8.0};  // closing, far away

  std::vector<sensing_track_t> ts;
  for (int k = 0; k < 40; k++) {
    ts = trk.update({det(A.at(k), A.v, 20.0), det(B.at(k), B.v, 20.0)}, k == 0 ? 0.0 : DT_S);
  }
  ASSERT_EQ(ts.size(), 2u) << "expected exactly two confirmed tracks";

  const sensing_track_t* tA = nearest_track(ts, A.at(39));
  const sensing_track_t* tB = nearest_track(ts, B.at(39));
  ASSERT_NE(tA, nullptr);
  ASSERT_NE(tB, nullptr);
  EXPECT_NE(tA->track_id, tB->track_id) << "two targets collapsed into one id";
  EXPECT_NEAR((double)tA->range_m, A.at(39), 1.5 * RANGE_RES_M);
  EXPECT_NEAR((double)tB->range_m, B.at(39), 1.5 * RANGE_RES_M);
}

// The real MOT correctness test: two trajectories whose bistatic RANGES cross once mid-capture, but
// with OPPOSITE range-rate. Naive nearest-in-range association would swap identities at the crossing;
// predicting each track forward by its own velocity must keep the two track_ids attached to the same
// physical target throughout.
TEST(multi_target_tracker, track_ids_survive_a_range_crossing)
{
  multi_target_tracker trk(make_args());
  // A opens from 90 m at +7 m/s; B closes from 150 m at -7 m/s. Ranges are equal (120 m) at k=37.
  cv_target A{90.0, 7.0};
  cv_target B{150.0, -7.0};
  const int K = 74;

  // Let both tracks confirm before the crossing (crossing is ~k=37, well after M-of-N).
  std::vector<sensing_track_t> ts;
  uint32_t id_lower = 0, id_upper = 0; // ids of the initially-lower / initially-upper target
  for (int k = 0; k < K; k++) {
    ts = trk.update({det(A.at(k), A.v, 20.0), det(B.at(k), B.v, 20.0)}, k == 0 ? 0.0 : DT_S);
    if (k == 15) { // well before the crossing, capture the identity assignment
      ASSERT_EQ(ts.size(), 2u);
      const sensing_track_t* tA = nearest_track(ts, A.at(k));
      const sensing_track_t* tB = nearest_track(ts, B.at(k));
      id_lower = tA->track_id; // A is the lower-range target early on
      id_upper = tB->track_id;
      ASSERT_NE(id_lower, id_upper);
    }
  }

  // After the crossing, A is the HIGHER-range target (it kept opening, B kept closing). The id that
  // was on A early must still be on A now -- i.e. it must have followed the physical target THROUGH
  // the crossing, not stuck to "whichever detection is lower in range".
  ASSERT_EQ(ts.size(), 2u) << "lost a track through the crossing";
  const sensing_track_t* tA_end = nearest_track(ts, A.at(K - 1));
  const sensing_track_t* tB_end = nearest_track(ts, B.at(K - 1));
  ASSERT_NE(tA_end, nullptr);
  ASSERT_NE(tB_end, nullptr);
  EXPECT_EQ(tA_end->track_id, id_lower) << "identity of target A was swapped at the crossing";
  EXPECT_EQ(tB_end->track_id, id_upper) << "identity of target B was swapped at the crossing";
}

// M-of-N initiation: a burst of ungated, one-CPI-only false detections (CFAR alarms / mirror ghosts)
// scattered at random ranges must NOT produce confirmed tracks -- each appears in too few CPIs to
// clear the M-of-N gate. A steady real target alongside them still confirms.
TEST(multi_target_tracker, spurious_single_cpi_detections_do_not_confirm)
{
  multi_target_tracker trk(make_args()); // confirm 3-of-5
  cv_target A{110.0, 5.0};

  std::vector<sensing_track_t> ts;
  for (int k = 0; k < 30; k++) {
    std::vector<sensing_detection_t> d{det(A.at(k), A.v, 20.0)};
    // One transient false alarm at a fresh random-ish range each CPI (never repeats near itself).
    const double ghost = 300.0 + 40.0 * (double)((k * 37) % 11);
    d.push_back(det(ghost, 1.0, 30.0));
    ts = trk.update(d, k == 0 ? 0.0 : DT_S);
  }
  ASSERT_EQ(ts.size(), 1u) << "a transient false alarm was confirmed as a track";
  EXPECT_NEAR((double)ts[0].range_m, A.at(29), 1.5 * RANGE_RES_M);
}

// A single detection is not enough to report a track: with confirm_m=3 the track stays UNconfirmed
// (and hence unreported) for the first two CPIs, then appears on the third.
TEST(multi_target_tracker, m_of_n_delays_confirmation)
{
  multi_target_tracker trk(make_args()); // 3-of-5
  cv_target A{95.0, 4.0};

  EXPECT_EQ(trk.update({det(A.at(0), A.v, 20.0)}, 0.0).size(), 0u)   << "confirmed on CPI 1";
  EXPECT_EQ(trk.update({det(A.at(1), A.v, 20.0)}, DT_S).size(), 0u)  << "confirmed on CPI 2";
  EXPECT_EQ(trk.update({det(A.at(2), A.v, 20.0)}, DT_S).size(), 1u)  << "not confirmed by CPI 3 (3-of-5)";
}

// confirm_m=1 reproduces the legacy single-track "confirm on first hit" behaviour for one target.
TEST(multi_target_tracker, confirm_m_1_is_immediate)
{
  nr_isac_args_t a = make_args();
  a.track_confirm_m = 1;
  multi_target_tracker trk(a);
  cv_target A{100.0, 3.0};
  auto ts = trk.update({det(A.at(0), A.v, 20.0)}, 0.0);
  ASSERT_EQ(ts.size(), 1u) << "confirm_m=1 should report on the first hit";
  EXPECT_NEAR((double)ts[0].range_m, A.at(0), RANGE_RES_M);
}

// ---- Auto-derived M-of-N confirmation (track_confirm_m==0), added 2026-07-24 after a live run on
// mot2.rfsim.conf showed a hand-picked 3-of-5 confirming a range=5146 m ghost (and several extra
// ids) on a 2-real-target scene once CFAR was running ~14-16 raw detections/CPI. solve_confirm_m()
// derives M from the MEASURED detection density instead of a fixed constant. ----

namespace {
/// A minimal, plausible grid matching the 100 MHz/273 PRB harness (3276 range bins, 128 Doppler
/// bins) -- only the fields the auto-M derivation reads need to be populated.
sensing_rvm_t make_rvm()
{
  sensing_rvm_t r;
  r.nof_range_bins   = 3276;
  r.nof_doppler_bins = 128;
  r.range_res_m      = 3.05f;
  r.vel_res_mps      = 0.755f;
  return r;
}
} // namespace

// Auto mode must not be a fixed number: a scene with a much denser false-alarm rate must require a
// STRICTLY higher M to hit the same target_pfa than a sparse scene -- this is the entire point of
// deriving M from measurement instead of hand-picking it.
TEST(multi_target_tracker, auto_confirm_m_adapts_to_measured_false_alarm_density)
{
  auto run_with_noise_count = [](int noise_dets_per_cpi) {
    nr_isac_args_t a = make_args();
    a.track_confirm_m          = 0; // auto
    a.track_confirm_target_pfa = 1e-3f;
    multi_target_tracker trk(a);
    sensing_rvm_t rvm = make_rvm();
    uint32_t m_last = 0;
    for (int k = 0; k < 20; k++) {
      std::vector<sensing_detection_t> d{det(100.0 + k, 5.0, 20.0)}; // one real-ish target
      for (int j = 0; j < noise_dets_per_cpi; j++) {
        d.push_back(det(500.0 + 37.0 * j + k, 20.0, 10.0)); // scattered noise, never repeats near itself
      }
      trk.update(d, k == 0 ? 0.0 : DT_S, &rvm);
      m_last = trk.last_confirm_m();
    }
    return m_last;
  };
  const uint32_t m_sparse = run_with_noise_count(1);
  const uint32_t m_dense  = run_with_noise_count(300);
  EXPECT_GT(m_dense, m_sparse) << "denser measured false-alarm rate should require a strictly higher"
                                  " auto M to hold the same target_pfa (sparse=" << m_sparse
                               << " dense=" << m_dense << ")";
}

// An explicit track_confirm_m > 0 must pin the value and skip auto-derivation entirely, even when a
// wall of noise is fed alongside a valid rvm -- manual override must not be silently overridden.
TEST(multi_target_tracker, manual_confirm_m_overrides_auto)
{
  nr_isac_args_t a = make_args();
  a.track_confirm_m = 2;
  multi_target_tracker trk(a);
  sensing_rvm_t rvm = make_rvm();
  for (int k = 0; k < 5; k++) {
    std::vector<sensing_detection_t> d{det(100.0, 5.0, 20.0)};
    for (int j = 0; j < 50; j++) {
      d.push_back(det(400.0 + 13.0 * j, 3.0, 8.0));
    }
    trk.update(d, k == 0 ? 0.0 : DT_S, &rvm);
    EXPECT_EQ(trk.last_confirm_m(), 2u) << "manual pin was overridden by auto-derivation at k=" << k;
    EXPECT_LT(trk.last_p_hit(), 0.0) << "manual pin should not compute p_hit (sentinel -1)";
  }
}

// With no rvm (offline callers that don't have grid geometry, e.g. legacy call sites), auto mode
// must fall back to a fixed, documented value rather than crashing or silently disabling MOT.
TEST(multi_target_tracker, auto_mode_without_rvm_uses_fixed_fallback)
{
  nr_isac_args_t a = make_args();
  a.track_confirm_m = 0; // auto, but no rvm will be passed
  multi_target_tracker trk(a);
  auto ts = trk.update({det(100.0, 5.0, 20.0)}, 0.0); // no rvm argument -> nullptr default
  EXPECT_EQ(trk.last_confirm_m(), std::min(a.track_confirm_n, 3u));
  EXPECT_LT(trk.last_p_hit(), 0.0) << "no rvm -> p_hit should be the not-computed sentinel";
}

// ---- Association-gate ceiling (track_assoc_gate_max_sr_m/sv_mps), added 2026-07-24. A long-coasting
// track's sigma_range_m inflates via adaptive q; without a ceiling on the ASSOCIATION step's gate
// radius (separate from the per-track filter's own update gate), a stray detection far from the
// track can get routed to it anyway once its gate has grown wide enough -- live-confirmed as part of
// the same session's track-churn investigation. ----

// A heavily-coasted track's sigma_range_m inflates from plain CV covariance growth alone (a large
// initial velocity variance propagating unchecked through many empty-detection predict steps needs
// no maneuver/adaptive-q at all). Without a CEILING on the association step's gate radius, a stray
// detection far from the track can get absorbed by it once the gate has grown wide enough. With the
// ceiling small, the same stray must instead be free to seed its OWN new tentative track.
TEST(multi_target_tracker, assoc_gate_ceiling_stops_a_coasted_track_from_swallowing_a_stray_detection)
{
  auto run = [](float max_sr_m) {
    nr_isac_args_t a            = make_args();
    a.track_confirm_m           = 1;  // confirm immediately -- isolates the association-gate behaviour
    a.track_max_coast           = 50; // survive many empty CPIs without being dropped
    a.track_assoc_gate_max_sr_m = max_sr_m;
    multi_target_tracker trk(a);

    cv_target A{100.0, 5.0};
    double t = 0.0;
    trk.update({det(A.r0, A.v, 20.0)}, 0.0); // seed + confirm track A at CPI 0

    // Coast it empty for a long run so sigma_range_m inflates well past a small ceiling.
    int k = 1;
    for (; k < 30; k++) {
      trk.update({}, DT_S);
      t += DT_S;
    }

    // A stray detection 50 m beyond A's (coasted) prediction, same velocity (isolates the range
    // term): far enough that a small CEILING rejects it, but the naturally-inflated, UNCAPPED sigma
    // at this point is large enough to plausibly accept it.
    const double r_pred = A.r0 + A.v * t;
    return trk.update({det(r_pred + 50.0, A.v, 20.0)}, DT_S);
  };

  const auto ts_capped   = run(2.0f);    // tight ceiling
  const auto ts_uncapped = run(1000.0f); // effectively no ceiling

  EXPECT_EQ(ts_capped.size(), 2u) << "capped gate should have let the stray seed its own track "
                                     "instead of being absorbed by the coasted one";
  EXPECT_EQ(ts_uncapped.size(), 1u) << "uncapped gate should have absorbed the stray into the "
                                       "existing (heavily-coasted) track, reproducing the live bug";
}

// ---- Track-level Doppler-harmonic rejection (track_harmonic_reject), added 2026-07-24. A harmonic
// ghost that leaks past the per-CPI detection filter can confirm a track and then coast; a Kalman
// track follows it as smoothly as a real target, so the tracker must reject it structurally: a
// confirmed track at the same range and an integer-multiple velocity of another confirmed track is
// the fundamental's harmonic, and is dropped (the lower-|velocity| fundamental is kept). ----
TEST(multi_target_tracker, track_harmonic_reject_drops_the_integer_multiple_track)
{
  auto run = [](bool reject) {
    nr_isac_args_t a          = make_args();
    a.track_confirm_m         = 1;      // confirm immediately, isolate the harmonic logic
    a.track_harmonic_reject   = reject;
    a.track_harmonic_range_m  = 10.0f;
    a.harmonic_max_k          = 4;
    a.harmonic_tol            = 0.15f;
    multi_target_tracker trk(a);
    // A real target at 120 m / +6 m/s, plus its 2x Doppler harmonic ghost: SAME range trajectory as
    // the target (a harmonic sits at the target's range, only its reported velocity is 2x), reported
    // at +12 m/s. The two tracks are distinguished by velocity but stay co-located in range.
    cv_target real{120.0, 6.0};
    std::vector<sensing_track_t> ts;
    for (int k = 0; k < 20; k++) {
      const double rng = real.at(k);
      ts = trk.update({det(rng, real.v, 22.0), det(rng, 2.0 * real.v, 18.0)}, k == 0 ? 0.0 : DT_S);
    }
    return ts;
  };

  const auto without = run(false);
  const auto with    = run(true);
  EXPECT_EQ(without.size(), 2u) << "baseline: both the target and its harmonic track exist";
  ASSERT_EQ(with.size(), 1u) << "harmonic-reject should leave only the fundamental track";
  // The survivor must be the fundamental (lower |velocity|, ~+6 m/s), not the +12 m/s harmonic.
  EXPECT_LT(std::abs((double)with[0].range_rate_mps), 9.0)
      << "kept the harmonic instead of the fundamental (rate=" << with[0].range_rate_mps << ")";
}

// Two genuinely distinct targets at the SAME velocity but well-separated range must BOTH survive
// harmonic rejection -- the co-location gate (track_harmonic_range_m) must protect them.
TEST(multi_target_tracker, track_harmonic_reject_spares_separated_same_velocity_targets)
{
  nr_isac_args_t a         = make_args();
  a.track_confirm_m        = 1;
  a.track_harmonic_reject  = true;
  a.track_harmonic_range_m = 10.0f;
  multi_target_tracker trk(a);
  cv_target A{100.0, 6.0};
  cv_target B{100.0, 12.0}; // 2x velocity BUT 100 m further in range -> not co-located, not a harmonic
  std::vector<sensing_track_t> ts;
  for (int k = 0; k < 20; k++) {
    ts = trk.update({det(A.at(k), A.v, 20.0), det(B.at(k) + 100.0, B.v, 20.0)}, k == 0 ? 0.0 : DT_S);
  }
  EXPECT_EQ(ts.size(), 2u) << "a same-velocity target at a different range was wrongly rejected as a harmonic";
}

// track_flicker_reject (power-continuity TBD gate): a track whose per-CPI SNR swings hard (a gated
// amplitude-harmonic ghost's signature) is withheld from the report, while a smooth-SNR real target
// is kept. Same scene for both, distinguished only by their SNR trajectories.
TEST(multi_target_tracker, flicker_reject_drops_the_bursty_snr_track_keeps_the_smooth_one)
{
  auto run = [](bool reject) {
    nr_isac_args_t a             = make_args();
    a.track_confirm_m            = 1;    // confirm immediately, isolate the flicker logic
    a.track_flicker_reject       = reject;
    a.track_flicker_max_db       = 6.0f;
    a.track_flicker_min_updates  = 3;
    a.track_flicker_ewma_alpha   = 0.4f;
    multi_target_tracker trk(a);
    // Two well-separated CV targets. A: smooth SNR (~22 dB, +/-1). B: bursty SNR alternating 24/8 dB
    // (16 dB swings) -- the flicker signature of a slot-gated harmonic ghost.
    cv_target A{120.0, 6.0};
    cv_target B{300.0, -5.0};
    std::vector<sensing_track_t> ts;
    for (int k = 0; k < 20; k++) {
      const double snr_a = 22.0 + ((k % 2) ? 1.0 : -1.0);   // smooth: +/-1 dB
      const double snr_b = (k % 2) ? 24.0 : 8.0;            // bursty: 16 dB swings
      ts = trk.update({det(A.at(k), A.v, snr_a), det(B.at(k), B.v, snr_b)}, k == 0 ? 0.0 : DT_S);
    }
    return ts;
  };

  const auto without = run(false);
  const auto with    = run(true);
  EXPECT_EQ(without.size(), 2u) << "baseline: both tracks are reported without the flicker gate";
  ASSERT_EQ(with.size(), 1u) << "flicker-reject should withhold the bursty-SNR ghost track";
  // The survivor is the smooth target near 120 m, not the bursty one near 300 m.
  EXPECT_LT(std::abs((double)with[0].range_m - 120.0), 30.0)
      << "kept the bursty ghost instead of the smooth target (range=" << with[0].range_m << ")";
}

// A smooth-SNR track is never withheld by the flicker gate, even with the gate on -- guards against
// the gate suppressing genuine targets.
TEST(multi_target_tracker, flicker_reject_spares_a_smooth_target)
{
  nr_isac_args_t a            = make_args();
  a.track_confirm_m           = 1;
  a.track_flicker_reject      = true;
  a.track_flicker_max_db      = 6.0f;
  a.track_flicker_min_updates = 3;
  multi_target_tracker trk(a);
  cv_target A{150.0, 4.0};
  std::vector<sensing_track_t> ts;
  for (int k = 0; k < 20; k++) {
    const double snr = 20.0 + 1.5 * std::sin(0.5 * k); // smooth slow variation, ~3 dB peak-to-peak
    ts = trk.update({det(A.at(k), A.v, snr)}, k == 0 ? 0.0 : DT_S);
  }
  ASSERT_EQ(ts.size(), 1u) << "a smooth-SNR real target was wrongly withheld by the flicker gate";
}

int main(int argc, char** argv)
{
  logInit();
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
