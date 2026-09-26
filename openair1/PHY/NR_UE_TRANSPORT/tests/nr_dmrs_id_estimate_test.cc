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

/* Transmit one type-1, port-0 DM-RS symbol with identity `nid` through a channel with delay
 * `tau_samples` and per-component noise sigma; returns the full OFDM symbol in the estimator's
 * own rxdataF layout (first_carrier_offset applied, wrap-around at FFT). */
static std::vector<c16_t> synth(int nid, int nscid, int slot, int sym, int rb_offset, int nb_rb,
                                double tau_samples, double noise, std::mt19937 &rng)
{
  std::vector<c16_t> rx(FFT, c16_t{0, 0});
  std::normal_distribution<double> n(0.0, noise);
  for (auto &v : rx) v = c16_t{(int16_t)n(rng), (int16_t)n(rng)};
  const uint32_t *gold = nr_gold_pdsch(N_RB, SYMS, nid, nscid, slot, sym);
  std::vector<c16_t> pil(6 * (nb_rb + rb_offset));
  nr_pdsch_dmrs_rx(0, gold, pil.data(), 1000, 0, (unsigned short)(nb_rb + rb_offset), 0, 16384);
  int re = (FIRST_CARRIER + (rb_offset) * 12) % FFT;
  for (int m = 0; m < 6 * nb_rb; ++m) {
    const c16_t p = pil[6 * rb_offset + m];            // the receiver's pilot is conj-form: p * tx = |.|^2
    const double ph = -2.0 * M_PI * tau_samples * re / FFT; // linear phase across subcarriers
    // tx symbol = conj(p)/|p| * 256 rotated by the channel: then p * tx ~ 256*|p| * e^{j ph}
    const double pr = p.r, pi = p.i, mag = std::hypot(pr, pi);
    const double txr = (pr * std::cos(ph) + pi * std::sin(ph)) / mag * 256.0;  // conj(p)*e^{j ph}
    const double txi = (pr * std::sin(ph) - pi * std::cos(ph)) / mag * 256.0;
    rx[re].r = (int16_t)std::lround(txr + n(rng));
    rx[re].i = (int16_t)std::lround(txi + n(rng));
    re = (re + 2) % FFT;
  }
  return rx;
}

TEST(DmrsId, RecoversTheTrueIdentityWithLargeMargin) {
  std::mt19937 rng(7);
  for (int nid : {2, 517, 1023}) {
    nr_dmrs_id_state_t st; nr_dmrs_id_init(&st, "TEST", nid);
    auto rx = synth(nid, 0, 3, 2, /*rb_offset=*/5, /*nb_rb=*/50, /*tau=*/3.7, /*noise=*/40.0, rng);
    ASSERT_EQ(nr_dmrs_id_accumulate(&st, rx.data(), FFT, FIRST_CARRIER + 5 * 12, 5, 50, N_RB, SYMS, 3, 2, 0, 1),
              NR_DMRS_ID_CANDIDATES);
    ASSERT_TRUE(nr_dmrs_id_decide(&st, 1, 10.0)) << "nid=" << nid;
    EXPECT_EQ(st.best_id, nid);
    EXPECT_GT(st.margin_db, 10.0) << "nid=" << nid << " margin " << st.margin_db;
  }
}
TEST(DmrsId, WrongAssumptionIsReportedAsMismatchNotConfirmed) {
  std::mt19937 rng(11);
  nr_dmrs_id_state_t st; nr_dmrs_id_init(&st, "TEST", /*assumed=*/2);
  auto rx = synth(/*true=*/900, 0, 5, 2, 0, 30, 1.2, 40.0, rng);
  nr_dmrs_id_accumulate(&st, rx.data(), FFT, FIRST_CARRIER, 0, 30, N_RB, SYMS, 5, 2, 0, 1);
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
    nr_dmrs_id_accumulate(&st, rx.data(), FFT, FIRST_CARRIER, 0, 40, N_RB, SYMS, g, 2, 0, 1);
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
    nr_dmrs_id_accumulate(&st, rx.data(), FFT, FIRST_CARRIER + 10 * 12, 10, 4, N_RB, SYMS, slot, 2, 0, 1);
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
                                           SYMS, 3, 2, 0, 1);
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
  nr_dmrs_id_accumulate(&st, rx.data(), FFT, FIRST_CARRIER + 5 * 12, 5, 50, N_RB, SYMS, 3, 2, 0, 1);
  EXPECT_FALSE(nr_dmrs_id_decide(&st, 1, 10.0));
}
TEST(DmrsId, RejectsInvalidGeometry) {
  nr_dmrs_id_state_t st; nr_dmrs_id_init(&st, "TEST", 2);
  std::vector<c16_t> rx(FFT);
  EXPECT_EQ(nr_dmrs_id_accumulate(&st, rx.data(), FFT, 0, 270, 10, N_RB, SYMS, 0, 2, 0, 1), 0) << "beyond N_RB";
  EXPECT_EQ(nr_dmrs_id_accumulate(&st, nullptr, FFT, 0, 0, 10, N_RB, SYMS, 0, 2, 0, 1), 0);
  EXPECT_EQ(st.grants, 0u);
}
int main(int argc, char **argv) { logInit(); testing::InitGoogleTest(&argc, argv); return RUN_ALL_TESTS(); }
