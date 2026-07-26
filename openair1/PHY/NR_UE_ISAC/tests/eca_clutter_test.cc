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

/*! \file openair1/PHY/NR_UE_ISAC/tests/eca_clutter_test.cc
 * \brief Offline self-test for the ECA/ECA+ CFR-domain clutter canceller (ECA_CLUTTER_HANDOVER.md
 *        rollout step 2/4). Exercises eca_clutter (and range_doppler with clutter_removal="eca+")
 *        directly on hand-built synthetic CFR grids — same offline-first pattern as isac_sync_test.cc.
 *
 * Cases:
 *   1. MeanSubtractionEquivalence — with delay_max=full (P_Φ=I) and doppler_max=0 (DC-only Ψ), ECA
 *      must reproduce the legacy per-subcarrier slow-time mean subtraction bit-for-bit (float tol).
 *   2. StaticSuppressedMovingPreserved — a strong near-zero-Doppler clutter tone is suppressed while
 *      a target whose Doppler is outside the removal band is preserved (the ECA blind-speed contract).
 *   3. MirrorGhostSuppressed — end-to-end through range_doppler: clutter with a near-DC (but not exact
 *      DC) slow-time envelope leaves a conjugate mirror ghost under mean subtraction; ECA+ removes the
 *      clutter band in the complex domain so the ghost collapses, while a genuine moving target
 *      (Doppler outside the band) survives both. This is the motivating bug + fix.
 *
 * Carrier matches the live testbed (CLAUDE.md): n78, 51 PRB, 30 kHz SCS, fc=3414.99 MHz.
 */

#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "defs_nr_UE_ISAC.h"
#include "eca_clutter.h"
#include "range_doppler.h"

extern "C" {
#include "common/utils/LOG/log.h"
#include "common/config/config_userapi.h"
}

// LOG/CONFIG_LIB need these outside a full softmodem executable (same convention as isac_sync_test.cc).
extern "C" configmodule_interface_t* uniqCfg = nullptr;
extern "C" void exit_function(const char* file, const char* function, const int line, const char* s, const int assert)
{
  (void)file;
  (void)function;
  (void)line;
  (void)s;
  (void)assert;
  abort();
}

using namespace nr_isac;

namespace {

constexpr double   SPEED_OF_LIGHT = 299792458.0;
constexpr uint32_t NOF_PRB        = 51;
constexpr uint32_t NOF_SUBC       = NOF_PRB * 12; // 612 (fused grid)
constexpr double   SCS_HZ         = 30000.0;
constexpr double   DF_COMB        = SCS_HZ; // grid comb = 1
constexpr double   FC_HZ          = 3414990000.0;
constexpr double   SLOT_DUR_S     = 0.5e-3;                  // numerology 1
constexpr uint32_t PERIOD_SLOTS   = 20;                      // slow-time sampling period
constexpr double   T_SLOW_S       = PERIOD_SLOTS * SLOT_DUR_S; // 10 ms

// Physical model shared with range_doppler / the sync self-test: H[n,m] for a scatterer at delay
// bin r (=> tau) and (fftshift-relative) Doppler bin d over an N-row, M-column CFR grid.
void add_scatterer(std::vector<icf_t>& H,
                   uint32_t            N,
                   uint32_t            M,
                   double              amp,
                   int                 range_bin,
                   int                 doppler_bin)
{
  const double tau = (double)range_bin / ((double)M * DF_COMB);
  const double fd  = (double)doppler_bin / ((double)N * T_SLOW_S);
  for (uint32_t n = 0; n < N; n++) {
    for (uint32_t m = 0; m < M; m++) {
      const double ph = -2.0 * M_PI * ((double)m * DF_COMB) * tau + 2.0 * M_PI * fd * ((double)n * T_SLOW_S);
      H[(size_t)n * M + m] += icf_t((float)(amp * std::cos(ph)), (float)(amp * std::sin(ph)));
    }
  }
}

nr_isac_carrier_t make_carrier()
{
  nr_isac_carrier_t c{};
  c.nof_prb         = NOF_PRB;
  c.scs_hz          = (uint32_t)SCS_HZ;
  c.dl_center_hz    = (uint64_t)FC_HZ;
  c.pci             = 2;
  c.slots_per_frame = 20;
  return c;
}

// Max |power| over the whole Doppler axis of one range row of an RVM.
float row_peak(const sensing_rvm_t& rvm, uint32_t r)
{
  float best = 0.0f;
  for (uint32_t d = 0; d < rvm.nof_doppler_bins; d++) {
    best = std::max(best, rvm.power[(size_t)r * rvm.nof_doppler_bins + d]);
  }
  return best;
}

} // namespace

// --- Case 1: ECA reduces to mean subtraction for the DC-only / full-delay configuration. ---
TEST(EcaClutter, MeanSubtractionEquivalence)
{
  const uint32_t N = 64, M = 96;
  std::vector<icf_t> H((size_t)N * M);
  uint32_t           lcg = 12345u; // deterministic pseudo-random fill
  auto               rnd = [&]() {
    lcg = lcg * 1664525u + 1013904223u;
    return ((float)(lcg >> 8) / (float)(1u << 24)) * 2.0f - 1.0f;
  };
  for (auto& v : H) {
    v = icf_t(rnd(), rnd());
  }

  // Reference: explicit per-subcarrier slow-time mean subtraction.
  std::vector<icf_t> ref = H;
  for (uint32_t m = 0; m < M; m++) {
    double ar = 0.0, ai = 0.0;
    for (uint32_t n = 0; n < N; n++) {
      ar += ref[(size_t)n * M + m].real();
      ai += ref[(size_t)n * M + m].imag();
    }
    const icf_t mean((float)(ar / N), (float)(ai / N));
    for (uint32_t n = 0; n < N; n++) {
      ref[(size_t)n * M + m] -= mean;
    }
  }

  nr_isac_args_t args;
  args.eca_delay_max_m     = 0.0f; // full range => P_Φ = I
  args.eca_doppler_max_mps = 0.0f; // DC-only => single all-ones slow-time atom
  eca_clutter        eca(args);
  std::vector<icf_t> got = H;
  eca.remove(got.data(), N, M, DF_COMB, T_SLOW_S, FC_HZ);

  EXPECT_TRUE(eca.freq_is_identity());
  EXPECT_EQ(eca.last_doppler_atoms(), 1u); // DC only

  float maxdiff = 0.0f;
  for (size_t i = 0; i < got.size(); i++) {
    maxdiff = std::max(maxdiff, std::abs(got[i] - ref[i]));
  }
  EXPECT_LT(maxdiff, 1e-4f) << "ECA(DC-only, full delay) must equal mean subtraction";
}

// --- Case 2: near-zero-Doppler clutter suppressed, out-of-band moving target preserved. ---
TEST(EcaClutter, StaticSuppressedMovingPreserved)
{
  const uint32_t N = 128, M = NOF_SUBC;
  std::vector<icf_t> H((size_t)N * M, icf_t(0.0f, 0.0f));

  const int   clutter_r = 8, clutter_d = 0;   // static clutter at zero Doppler
  const int   moving_r = 20, moving_d = 40;   // clearly out-of-band target
  const float clutter_amp = 50.0f, moving_amp = 1.0f;
  add_scatterer(H, N, M, clutter_amp, clutter_r, clutter_d);
  add_scatterer(H, N, M, moving_amp, moving_r, moving_d);

  // Project the (still-complex) grid onto a scatterer's own tone; |coeff| ~ that scatterer's amplitude.
  auto tone_amp = [&](const std::vector<icf_t>& G, int rbin, int dbin) {
    const double tau = (double)rbin / ((double)M * DF_COMB);
    const double fd  = (double)dbin / ((double)N * T_SLOW_S);
    std::complex<double> acc(0.0, 0.0);
    for (uint32_t n = 0; n < N; n++) {
      for (uint32_t m = 0; m < M; m++) {
        const double ph = -2.0 * M_PI * ((double)m * DF_COMB) * tau + 2.0 * M_PI * fd * ((double)n * T_SLOW_S);
        const std::complex<double> atom(std::cos(ph), std::sin(ph));
        acc += std::conj(atom) * std::complex<double>(G[(size_t)n * M + m].real(), G[(size_t)n * M + m].imag());
      }
    }
    return std::abs(acc) / ((double)N * M);
  };

  const double clutter_before = tone_amp(H, clutter_r, clutter_d);
  const double moving_before  = tone_amp(H, moving_r, moving_d);

  nr_isac_args_t args;
  args.eca_delay_max_m     = 0.0f; // full range
  args.eca_doppler_max_mps = 0.1f; // removes several Doppler bins around zero (Qh ~ 6 at this geometry)
  eca_clutter eca(args);
  std::vector<icf_t> cleaned = H;
  eca.remove(cleaned.data(), N, M, DF_COMB, T_SLOW_S, FC_HZ);
  EXPECT_GE(eca.last_doppler_atoms(), 3u); // band actually spans more than DC

  const double clutter_after = tone_amp(cleaned, clutter_r, clutter_d);
  const double moving_after   = tone_amp(cleaned, moving_r, moving_d);

  EXPECT_LT(clutter_after, 0.01 * clutter_before) << "static clutter must be strongly suppressed";
  EXPECT_GT(moving_after, 0.9 * moving_before) << "out-of-band moving target must be preserved";
}

// --- Case 3 (motivating bug): the conjugate mirror ghost collapses under ECA+, target survives. ---
TEST(EcaClutter, MirrorGhostSuppressed)
{
  const uint32_t N = 128, M = NOF_SUBC;
  const uint32_t R = M; // nof_range == nof_subc

  // Clutter engineered to reproduce the real, root-caused mirror-ghost fingerprint (see
  // tests/sensing_sim/README.md §"The mirror ghost"): a CFR that is REAL-valued in the subcarrier
  // (delay) axis — built from symmetric delay tones ±r_c so cos(2π·m·Δf·τ_c) — and SLOW but non-DC in
  // slow-time — symmetric Doppler tones ±d_c so cos(2π·d_c·n/N). The real-in-m structure makes the
  // range IFFT conjugate-symmetric → co-equal peaks at bin r_c AND its mirror ≈ R−r_c; the zero-mean
  // slow-time envelope means per-subcarrier mean subtraction removes nothing, so the ghost survives it.
  // A genuine moving target sits well outside the ECA Doppler band and is single-delay (complex in m),
  // so it has no strong mirror of its own.
  const int   clutter_r = 8, clutter_d = 2;
  const int   moving_r = 24, moving_d = 45;
  const float clutter_amp = 40.0f, moving_amp = 3.0f;

  std::vector<icf_t> H((size_t)N * M, icf_t(0.0f, 0.0f));
  for (int rs : {+clutter_r, -clutter_r}) {
    for (int ds : {+clutter_d, -clutter_d}) {
      add_scatterer(H, N, M, clutter_amp, rs, ds);
    }
  }
  add_scatterer(H, N, M, moving_amp, moving_r, moving_d);

  // The −r_c delay tone lands near row R−r_c (IFFT wrap), a bin or two off the nominal R−1−r_c; scan a
  // small band in the far/upper range region so the off-by-one from the transform convention is covered.
  auto ghost_peak = [&](const sensing_rvm_t& rvm) {
    float best = 0.0f;
    for (uint32_t r = R - (uint32_t)clutter_r - 3; r <= R - (uint32_t)clutter_r + 1 && r < R; r++) {
      best = std::max(best, row_peak(rvm, r));
    }
    return best;
  };

  auto run = [&](const std::string& mode, sensing_rvm_t& rvm) {
    nr_isac_args_t args;
    args.clutter_removal     = mode;
    args.eca_delay_max_m     = 0.0f;  // full range
    // Qh = 3 bins: covers the +/-2 clutter, excludes moving_d=45. Was 0.1f; doubled 2026-07-23 when
    // the velocity axis dropped its erroneous monostatic factor 2 (see range_doppler.cc), which
    // halved the bins any given m/s band maps to — 0.2f here reproduces the ORIGINAL Qh exactly, so
    // this test still exercises the same configuration it always did.
    args.eca_doppler_max_mps = 0.2f;
    args.zero_doppler_guard  = 0;     // don't notch — we read rvm.power directly
    args.zero_range_guard    = 0;
    range_doppler                    rd(args);
    std::vector<sensing_detection_t> dets;
    std::vector<icf_t>               cpi = H; // process() copies internally, but keep H pristine anyway
    rd.process(cpi.data(), N, M, /*comb=*/1, make_carrier(), (float)PERIOD_SLOTS, /*row_comb=*/nullptr, rvm, dets);
  };

  sensing_rvm_t rvm_mean, rvm_eca;
  run("mean", rvm_mean);
  run("eca+", rvm_eca);

  const float ghost_mean = ghost_peak(rvm_mean);
  const float ghost_eca  = ghost_peak(rvm_eca);
  const float tgt_mean   = row_peak(rvm_mean, (uint32_t)moving_r);
  const float tgt_eca    = row_peak(rvm_eca, (uint32_t)moving_r);

  // The mean-subtraction pipeline must actually exhibit the ghost (otherwise the test proves nothing).
  ASSERT_GT(ghost_mean, 0.1f * tgt_mean) << "mean-sub baseline should show a real mirror ghost";
  // ECA+ collapses the ghost by >= 20 dB relative to the mean-sub baseline.
  EXPECT_LT(ghost_eca, 0.01f * ghost_mean) << "ECA+ must suppress the conjugate mirror ghost";
  // ...while keeping the genuine moving target.
  EXPECT_GT(tgt_eca, 0.5f * tgt_mean) << "ECA+ must preserve the out-of-band moving target";
}

int main(int argc, char** argv)
{
  logInit();
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
