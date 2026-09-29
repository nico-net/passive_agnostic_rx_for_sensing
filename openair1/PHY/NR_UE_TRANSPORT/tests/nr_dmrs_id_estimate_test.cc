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
#include <cmath>
#include <random>
#include <vector>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <gtest/gtest.h>
extern "C" {
#include "nr_dmrs_id_estimate.h"
#include "common/utils/LOG/log.h"
/* The OAI PHY headers declaring these are not C++-clean (VLA parameters). ABI-equivalent
 * prototypes: nr_prefix_type_t is an int-sized enum, NR_NORMAL == 0, NFAPI_NR_DMRS_TYPE1 == 0. */
uint32_t *nr_gold_pdsch(int N_RB_DL, int symbols_per_slot, int nid, int nscid, int slot, int symbol);
int nr_pdsch_dmrs_rx(int Ncp, const unsigned int *gold, c16_t *output, unsigned short p, unsigned char lp,
                     unsigned short nb_pdsch_rb, uint8_t config_type, int16_t dmrs_scaling);
#include "common/config/config_userapi.h"
configmodule_interface_t *uniqCfg = nullptr;
void exit_function(const char *,const char *,int,const char *,int) { std::abort(); }
}

/* Geometry of the cell every saved capture comes from: 273 PRB, mu=1, 4096-point grid. */
static constexpr int N_RB = 273, SYMS = 14, FFT = 4096, FIRST_CARRIER = FFT - (N_RB * 12) / 2;

/* NFAPI_NR_DMRS_TYPE1/2 == 0/1, duplicated (not included) for the same "no heavy PHY headers"
 * reason as this file's other ABI-equivalent declarations above. */
static constexpr int kDmrsType1 = 0, kDmrsType2 = 1;

/* TS 38.211 6.4.1.1.3-1/-2: absolute RE offset of pilot index m = 2n+k' from the reference point,
 * CDM group 0 (delta=0). Mirrors get_dmrs_freq_idx_ul() (dmrs_nr.h) exactly -- duplicated here
 * rather than linked, same reasoning as kDmrsType1/2 above. Type 1: 4n+2k' (comb-2, 6 REs/RB).
 * Type 2: 6n+k' (2 adjacent REs every 6, 4 REs/RB). */
static int dmrs_re_offset(int m, int dmrs_type)
{
  const int n = m / 2, kp = m % 2;
  return dmrs_type == kDmrsType2 ? (6 * n + kp) : (4 * n + 2 * kp);
}

/* Transmit one port-0 DM-RS symbol (type 1 by default) with identity `nid` through a channel with
 * delay `tau_samples` and per-component noise sigma; returns the full OFDM symbol in the
 * estimator's own rxdataF layout (first_carrier_offset applied, wrap-around at FFT). */
static std::vector<c16_t> synth(int nid, int nscid, int slot, int sym, int rb_offset, int nb_rb,
                                double tau_samples, double noise, std::mt19937 &rng, int dmrs_type = kDmrsType1)
{
  std::vector<c16_t> rx(FFT, c16_t{0, 0});
  std::normal_distribution<double> n(0.0, noise);
  for (auto &v : rx) v = c16_t{(int16_t)n(rng), (int16_t)n(rng)};
  const uint32_t *gold = nr_gold_pdsch(N_RB, SYMS, nid, nscid, slot, sym);
  const int nb_dmrs_per_rb = dmrs_type == kDmrsType2 ? 4 : 6;
  std::vector<c16_t> pil(nb_dmrs_per_rb * (nb_rb + rb_offset));
  nr_pdsch_dmrs_rx(0, gold, pil.data(), 1000, 0, (unsigned short)(nb_rb + rb_offset), dmrs_type, 16384);
  const int npil = nb_dmrs_per_rb * nb_rb;
  for (int m = 0; m < npil; ++m) {
    const int re = (FIRST_CARRIER + rb_offset * 12 + dmrs_re_offset(m, dmrs_type)) % FFT;
    const c16_t p = pil[nb_dmrs_per_rb * rb_offset + m]; // the receiver's pilot is conj-form: p * tx = |.|^2
    const double ph = -2.0 * M_PI * tau_samples * re / FFT; // linear phase across subcarriers
    // tx symbol = conj(p)/|p| * 256 rotated by the channel: then p * tx ~ 256*|p| * e^{j ph}
    const double pr = p.r, pi = p.i, mag = std::hypot(pr, pi);
    const double txr = (pr * std::cos(ph) + pi * std::sin(ph)) / mag * 256.0;  // conj(p)*e^{j ph}
    const double txi = (pr * std::sin(ph) - pi * std::cos(ph)) / mag * 256.0;
    rx[re].r = (int16_t)std::lround(txr + n(rng));
    rx[re].i = (int16_t)std::lround(txi + n(rng));
  }
  return rx;
}

TEST(DmrsId, RecoversTheTrueIdentityWithLargeMargin) {
  std::mt19937 rng(7);
  for (int nid : {2, 517, 1023}) {
    nr_dmrs_id_state_t st; nr_dmrs_id_init(&st, "TEST", nid);
    auto rx = synth(nid, 0, 3, 2, /*rb_offset=*/5, /*nb_rb=*/50, /*tau=*/3.7, /*noise=*/40.0, rng);
    ASSERT_EQ(nr_dmrs_id_accumulate(&st, rx.data(), FFT, FIRST_CARRIER + 5 * 12, 5, 50, N_RB, SYMS, 3, 2, 0, 1, kDmrsType1),
              NR_DMRS_ID_CANDIDATES);
    ASSERT_TRUE(nr_dmrs_id_decide(&st, 1, 10.0)) << "nid=" << nid;
    EXPECT_EQ(st.best_id, nid);
    EXPECT_GT(st.margin_db, 10.0) << "nid=" << nid << " margin " << st.margin_db;
  }
}
/* The gap this closes: nr_dmrs_id_accumulate() used to hardcode NFAPI_NR_DMRS_TYPE1 unconditionally
 * (both the reference-sequence generation AND the comb-2 RE-stepping), so a DM-RS-type-2 cell's
 * scrambling-ID estimate would silently correlate against the wrong REs -- not a crash, just a
 * receiver that reads noise and never converges (or worse, converges to a wrong answer by chance).
 * Type 2 CDM group 0 has only 4 pilot REs/RB (vs type 1's 6), so this exercises a genuinely
 * different, sparser RE pattern, not a relabelled copy of the type-1 test. */
TEST(DmrsId, RecoversTheTrueIdentityWithType2Dmrs) {
  std::mt19937 rng(13);
  for (int nid : {2, 517, 1023}) {
    nr_dmrs_id_state_t st; nr_dmrs_id_init(&st, "TEST", nid);
    auto rx = synth(nid, 0, 3, 2, /*rb_offset=*/5, /*nb_rb=*/50, /*tau=*/3.7, /*noise=*/40.0, rng, kDmrsType2);
    ASSERT_EQ(nr_dmrs_id_accumulate(&st, rx.data(), FFT, FIRST_CARRIER + 5 * 12, 5, 50, N_RB, SYMS, 3, 2, 0, 1, kDmrsType2),
              NR_DMRS_ID_CANDIDATES);
    ASSERT_TRUE(nr_dmrs_id_decide(&st, 1, 10.0)) << "nid=" << nid;
    EXPECT_EQ(st.best_id, nid);
    EXPECT_GT(st.margin_db, 10.0) << "nid=" << nid << " margin " << st.margin_db;
  }
}
/* If the RE-stepping formula degenerated to the same pattern for both types, a type-2 signal read
 * with dmrs_type forced to type 1 might still accumulate SOME coherent energy by accident and reach
 * a confident (possibly even correct, by luck) decision. Cross-feeding proves the two paths are
 * genuinely different: reading a type-2 signal as type-1 must score far worse than reading it as
 * what it actually is. */
TEST(DmrsId, Type1AndType2ReadingsOfATtype2SignalDiffer) {
  std::mt19937 rng(17);
  const int nid = 42;
  auto rx = synth(nid, 0, 3, 2, 5, 50, 3.7, 40.0, rng, kDmrsType2);
  nr_dmrs_id_state_t matched; nr_dmrs_id_init(&matched, "TEST", nid);
  nr_dmrs_id_accumulate(&matched, rx.data(), FFT, FIRST_CARRIER + 5 * 12, 5, 50, N_RB, SYMS, 3, 2, 0, 1, kDmrsType2);
  nr_dmrs_id_state_t mismatched; nr_dmrs_id_init(&mismatched, "TEST", nid);
  nr_dmrs_id_accumulate(&mismatched, rx.data(), FFT, FIRST_CARRIER + 5 * 12, 5, 50, N_RB, SYMS, 3, 2, 0, 1, kDmrsType1);
  const double matched_score = nr_dmrs_id_score(&matched, nid);
  const double mismatched_score = nr_dmrs_id_score(&mismatched, nid);
  EXPECT_GT(matched_score, 10.0 * mismatched_score)
      << "matched=" << matched_score << " mismatched=" << mismatched_score;
}
TEST(DmrsId, WrongAssumptionIsReportedAsMismatchNotConfirmed) {
  std::mt19937 rng(11);
  nr_dmrs_id_state_t st; nr_dmrs_id_init(&st, "TEST", /*assumed=*/2);
  auto rx = synth(/*true=*/900, 0, 5, 2, 0, 30, 1.2, 40.0, rng);
  nr_dmrs_id_accumulate(&st, rx.data(), FFT, FIRST_CARRIER, 0, 30, N_RB, SYMS, 5, 2, 0, 1, kDmrsType1);
  ASSERT_TRUE(nr_dmrs_id_decide(&st, 1, 10.0));
  EXPECT_EQ(st.best_id, 900);
  EXPECT_NE(st.best_id, st.assumed_id);
}
TEST(DmrsId, NoiseAloneNeverDecides) {
  std::mt19937 rng(3);
  nr_dmrs_id_state_t st; nr_dmrs_id_init(&st, "TEST", 2);
  std::normal_distribution<double> n(0.0, 200.0);
  for (int g = 0; g < 8; ++g) {
    std::vector<c16_t> rx(FFT);
    for (auto &v : rx) v = c16_t{(int16_t)n(rng), (int16_t)n(rng)};
    nr_dmrs_id_accumulate(&st, rx.data(), FFT, FIRST_CARRIER, 0, 40, N_RB, SYMS, g, 2, 0, 1, kDmrsType1);
  }
  double best = -1e9; for (int i = 0; i < NR_DMRS_ID_CANDIDATES; ++i) best = std::max(best, nr_dmrs_id_margin_db(&st, i));
  EXPECT_FALSE(nr_dmrs_id_decide(&st, 1, 10.0)) << "best margin on noise: " << best;
}
TEST(DmrsId, EvidenceAccumulatesAcrossGrantsAtLowSnr) {
  /* One noisy small grant is not enough; several are. This is the operating point on a real
   * cell: many small allocations, none of them individually decisive. */
  std::mt19937 rng(5);
  nr_dmrs_id_state_t st; nr_dmrs_id_init(&st, "TEST", 2);
  auto one = [&](int slot) {
    auto rx = synth(2, 0, slot, 2, 10, /*nb_rb=*/4, 0.4, /*noise=*/120.0, rng);
    nr_dmrs_id_accumulate(&st, rx.data(), FFT, FIRST_CARRIER + 10 * 12, 10, 4, N_RB, SYMS, slot, 2, 0, 1, kDmrsType1);
  };
  one(0);
  const double m1 = nr_dmrs_id_margin_db(&st, 2);
  for (int s = 1; s < 12; ++s) one(s);
  const double m12 = nr_dmrs_id_margin_db(&st, 2);
  EXPECT_GT(m12, m1) << "margin must grow with evidence";
  EXPECT_TRUE(nr_dmrs_id_decide(&st, 12, 10.0)) << "margin after 12 grants: " << m12;
  EXPECT_EQ(st.best_id, 2);
}
TEST(DmrsId, PortPairCoherenceSeparatesOneLayerFromTwo) {
  std::mt19937 rng(9);
  auto rx1 = synth(2, 0, 3, 2, 5, 50, 1.0, 20.0, rng);
  const double c1 = nr_dmrs_port_pair_coherence(rx1.data(), FFT, FIRST_CARRIER + 5 * 12, 5, 50, N_RB, SYMS, 3, 2, 0, 2, 1);
  EXPECT_GT(c1, 0.9) << "single layer: " << c1;
  /* Two layers: add port 1 = same pilots with w_f = [+1,-1] over each comb pair, through an
   * independent channel of similar strength (random phase per RB). */
  auto rx2 = rx1;
  std::uniform_real_distribution<double> ph(0, 2 * M_PI);
  int re = (FIRST_CARRIER + 5 * 12) % FFT;
  double a = 0;
  for (int m = 0; m < 6 * 50; ++m) {
    if (m % 6 == 0) a = ph(rng);
    const double sgn = (m & 1) ? -1.0 : 1.0;            // w_f for port 1
    // port-1 tx symbol = sgn * conj(p)/|p| * 256 * e^{j a}; reuse rx1's port-0 symbol as the pilot carrier
    const double pr = rx1[re].r, pi = rx1[re].i, mag = std::hypot(pr, pi) + 1e-9; // rx1 = conj(p)-ish * 256
    const double ur = pr / mag * 256.0, ui = pi / mag * 256.0;                  // unit-ish port-0 symbol
    rx2[re].r = (int16_t)std::lround(rx1[re].r + sgn * (ur * std::cos(a) - ui * std::sin(a)));
    rx2[re].i = (int16_t)std::lround(rx1[re].i + sgn * (ur * std::sin(a) + ui * std::cos(a)));
    re = (re + 2) % FFT;
  }
  const double c2 = nr_dmrs_port_pair_coherence(rx2.data(), FFT, FIRST_CARRIER + 5 * 12, 5, 50, N_RB, SYMS, 3, 2, 0, 2, 1);
  EXPECT_LT(c2, 0.5) << "two layers: " << c2;
  EXPECT_LT(nr_dmrs_port_pair_coherence(nullptr, FFT, 0, 0, 1, N_RB, SYMS, 0, 0, 0, 2, 1), 0.0);
}
TEST(DmrsId, FindsAnIdAboveTheOldRange) {
  /* Stage 2: N_ID = 40000, well above the old 0..1023 window. Accumulate WITHOUT any CRC input --
   * the estimator never needed one, that is the whole point of Task 13(a). */
  std::mt19937 rng(13);
  nr_dmrs_id_state_t st;
  nr_dmrs_id_init(&st, "TEST", 2);
  nr_dmrs_id_set_range(&st, NR_DMRS_ID_CANDIDATES, NR_DMRS_ID_SPACE - NR_DMRS_ID_CANDIDATES);
  auto rx = synth(40000, 0, 3, 2, /*rb_offset=*/5, /*nb_rb=*/50, /*tau=*/3.7, /*noise=*/40.0, rng);
  const auto t0 = std::chrono::steady_clock::now();
  const int scored = nr_dmrs_id_accumulate(&st, rx.data(), FFT, FIRST_CARRIER + 5 * 12, 5, 50, N_RB,
                                           SYMS, 3, 2, 0, 1, kDmrsType1);
  const auto t1 = std::chrono::steady_clock::now();
  ASSERT_EQ(scored, (int)(NR_DMRS_ID_SPACE - NR_DMRS_ID_CANDIDATES));
  const double us_per_accumulate =
      std::chrono::duration<double, std::micro>(t1 - t0).count();
  fprintf(stderr, "[ INFO     ] stage-2 accumulate() over %d candidates took %.0f us\n", scored,
          us_per_accumulate);
  ASSERT_TRUE(nr_dmrs_id_decide(&st, 1, 10.0));
  EXPECT_EQ(st.best_id, 40000);
  EXPECT_GT(st.margin_db, 10.0);
}
TEST(DmrsId, WrongRangeDoesNotDecide) {
  /* Same true identity (40000), but the state is still watching the default 0..1023 window: the
   * margin gate must not decide on a window that cannot contain the true id. */
  std::mt19937 rng(17);
  nr_dmrs_id_state_t st;
  nr_dmrs_id_init(&st, "TEST", 2); // default range 0..1023
  auto rx = synth(40000, 0, 3, 2, 5, 50, 3.7, 40.0, rng);
  nr_dmrs_id_accumulate(&st, rx.data(), FFT, FIRST_CARRIER + 5 * 12, 5, 50, N_RB, SYMS, 3, 2, 0, 1, kDmrsType1);
  EXPECT_FALSE(nr_dmrs_id_decide(&st, 1, 10.0));
}
TEST(DmrsId, RejectsInvalidGeometry) {
  nr_dmrs_id_state_t st; nr_dmrs_id_init(&st, "TEST", 2);
  std::vector<c16_t> rx(FFT);
  EXPECT_EQ(nr_dmrs_id_accumulate(&st, rx.data(), FFT, 0, 270, 10, N_RB, SYMS, 0, 2, 0, 1, kDmrsType1), 0) << "beyond N_RB";
  EXPECT_EQ(nr_dmrs_id_accumulate(&st, nullptr, FFT, 0, 0, 10, N_RB, SYMS, 0, 2, 0, 1, kDmrsType1), 0);
  EXPECT_EQ(st.grants, 0u);
}
// ---- final review I5: two-window driver ------------------------------------------------------------
static bool feed2(nr_dmrs_id_2stage_t *t, const std::vector<c16_t> &rx, int slot, int dmrs_type = kDmrsType1)
{
  return nr_dmrs_id_2stage_accumulate(t, rx.data(), FFT, FIRST_CARRIER + 5 * 12, 5, 50, N_RB, SYMS, slot, 2, 0, 1, dmrs_type);
}

TEST(DmrsId2Stage, PrematureEscalationDoesNotLoseAStage1Id) {
  // The first grants are garbage (acquisition / mis-lock / false accepts) so stage 2 is armed; the
  // true id 300 arrives afterwards. The old one-way switch made 300 undecidable from then on.
  std::mt19937 rng(21);
  nr_dmrs_id_2stage_t t;
  nr_dmrs_id_2stage_init(&t, "TEST", 2);
  t.s1_grants = 4;
  t.s2_throttle = 1000000; // stage 2 evaluated once (tick 0), then never again in this test
  std::normal_distribution<double> n(0.0, 200.0);
  for (int i = 0; i < 4; ++i) {
    std::vector<c16_t> junk(FFT);
    for (auto &v : junk) v = c16_t{(int16_t)n(rng), (int16_t)n(rng)};
    EXPECT_FALSE(feed2(&t, junk, 3 + i));
  }
  EXPECT_TRUE(t.s2_armed);
  EXPECT_EQ(nr_dmrs_id_2stage_decided(&t), -1);
  bool decided = false;
  for (int g = 0; g < 40 && !decided; ++g)
    decided = feed2(&t, synth(300, 0, g % 20, 2, 5, 50, 3.7, 40.0, rng), g % 20);
  ASSERT_TRUE(decided);
  EXPECT_EQ(nr_dmrs_id_2stage_decided(&t), 300);
  EXPECT_LE(t.s2_evals, 1u);
}

TEST(DmrsId2Stage, FindsAStage2IdThrottledAndCapped) {
  std::mt19937 rng(23);
  nr_dmrs_id_2stage_t t;
  nr_dmrs_id_2stage_init(&t, "TEST", 2);
  t.s1_grants = 2;
  t.s2_throttle = 3;
  t.s2_max_evals = 20;
  int calls = 0;
  bool decided = false;
  for (; calls < 200 && !decided; ++calls)
    decided = feed2(&t, synth(40000, 0, calls % 20, 2, 5, 50, 3.7, 40.0, rng), calls % 20);
  ASSERT_TRUE(decided) << "after " << calls << " calls, " << t.s2_evals << " stage-2 evaluations";
  EXPECT_EQ(nr_dmrs_id_2stage_decided(&t), 40000);
  EXPECT_LE(t.s2_evals, 20u);
  EXPECT_GE(t.s2_evals, 16u); // the decide floor is 16 stage-2 grants
  EXPECT_GE(calls, 2 + 3 * 15); // throttled: one evaluation per 3 calls after arming
  // decided: every later call is a no-op for both stages
  const uint32_t g1 = t.s1.grants, e2 = t.s2_evals;
  EXPECT_FALSE(feed2(&t, synth(40000, 0, 1, 2, 5, 50, 3.7, 40.0, rng), 1));
  EXPECT_EQ(t.s1.grants, g1);
  EXPECT_EQ(t.s2_evals, e2);
}

TEST(DmrsId2Stage, Stage2BudgetIsAHardCap) {
  std::mt19937 rng(29);
  nr_dmrs_id_2stage_t t;
  nr_dmrs_id_2stage_init(&t, "TEST", 2);
  t.s1_grants = 1;
  t.s2_throttle = 1;
  t.s2_max_evals = 3;
  std::normal_distribution<double> n(0.0, 200.0);
  for (int i = 0; i < 12; ++i) {
    std::vector<c16_t> junk(FFT);
    for (auto &v : junk) v = c16_t{(int16_t)n(rng), (int16_t)n(rng)};
    feed2(&t, junk, i % 20);
  }
  EXPECT_EQ(t.s2_evals, 3u);
  EXPECT_EQ(t.s2.num_r, nullptr); // spent undecided: stage-2 arrays released
  EXPECT_EQ(t.s1.grants, 12u);     // stage 1 kept accumulating throughout
  EXPECT_EQ(nr_dmrs_id_2stage_decided(&t), -1);
}

TEST(DmrsId2Stage, WorksEndToEndWithType2Dmrs) {
  // The 2-stage driver is a thin wrapper around nr_dmrs_id_accumulate(); this confirms dmrs_type
  // actually reaches through both stage-1 calls inside it, not just the direct-call path the tests
  // above exercise.
  std::mt19937 rng(31);
  nr_dmrs_id_2stage_t t;
  nr_dmrs_id_2stage_init(&t, "TEST", 2);
  bool decided = false;
  for (int g = 0; g < 40 && !decided; ++g)
    decided = feed2(&t, synth(700, 0, g % 20, 2, 5, 50, 3.7, 40.0, rng, kDmrsType2), g % 20, kDmrsType2);
  ASSERT_TRUE(decided);
  EXPECT_EQ(nr_dmrs_id_2stage_decided(&t), 700);
}

TEST(DmrsId2Stage, ZeroInitialisedStateIsUndecided) {
  // The live states are static BSS read by the scan thread before any consumer init()s them: a zeroed
  // state must read as undecided, never as "decided identity 0".
  static nr_dmrs_id_2stage_t z;
  EXPECT_EQ(nr_dmrs_id_2stage_decided(&z), -1);
  EXPECT_EQ(nr_dmrs_id_2stage_decided(nullptr), -1);
}

int main(int argc, char **argv) { logInit(); testing::InitGoogleTest(&argc, argv); return RUN_ALL_TESTS(); }
