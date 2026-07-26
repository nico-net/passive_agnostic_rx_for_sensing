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

/*! \file openair1/PHY/NR_UE_ISAC/tests/matrix_complete_test.cc
 * \brief Offline tests for low-rank CFR matrix completion (matrix_complete.{h,cc}).
 *
 * Builds a genuinely low-rank slow-time x subcarrier grid from a few "scatterers" (each a rank-1
 * Doppler-delay outer product, the physical model), punches an irregular occupancy mask into it, and
 * checks that Singular Value Projection recovers the unobserved entries -- and, the point of the
 * whole exercise, that recovering the consistent full aperture removes the amplitude-gating Doppler
 * harmonics a masked grid produces.
 */

#include <cmath>
#include <complex>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "defs_nr_UE_ISAC.h"
#include "matrix_complete.h"

using namespace nr_isac;

namespace {

constexpr uint32_t N_SLOW = 64;
constexpr uint32_t N_SUBC = 256;

// A rank-`n_tgt` grid: sum of scatterers, each H[n,k] = a * exp(j2pi(fd*n + tau*k)).
std::vector<icf_t> make_lowrank(uint32_t n_tgt, std::mt19937& rng)
{
  std::uniform_real_distribution<double> U(0.0, 1.0);
  std::vector<icf_t> H((size_t)N_SLOW * N_SUBC, icf_t(0.0f, 0.0f));
  for (uint32_t t = 0; t < n_tgt; t++) {
    const double fd  = U(rng) * 0.4 - 0.2; // cycles/slot
    const double tau = U(rng) * 0.4 - 0.2; // cycles/subcarrier
    const double amp = 0.5 + U(rng);
    const double ph0 = U(rng) * 2.0 * M_PI;
    for (uint32_t n = 0; n < N_SLOW; n++) {
      for (uint32_t k = 0; k < N_SUBC; k++) {
        const double ph = ph0 + 2.0 * M_PI * (fd * (double)n + tau * (double)k);
        H[(size_t)n * N_SUBC + k] += icf_t((float)(amp * std::cos(ph)), (float)(amp * std::sin(ph)));
      }
    }
  }
  return H;
}

double frob(const std::vector<icf_t>& a, const std::vector<icf_t>& b)
{
  double s = 0.0;
  for (size_t i = 0; i < a.size(); i++) {
    const icf_t d = a[i] - b[i];
    s += (double)d.real() * d.real() + (double)d.imag() * d.imag();
  }
  return std::sqrt(s);
}

double frob_norm(const std::vector<icf_t>& a)
{
  double s = 0.0;
  for (const icf_t& v : a) {
    s += (double)v.real() * v.real() + (double)v.imag() * v.imag();
  }
  return std::sqrt(s);
}

} // namespace

// Core claim: with ~50% of entries observed, SVP recovers a genuinely low-rank grid to small error.
TEST(matrix_complete, recovers_lowrank_grid_from_partial_observations)
{
  std::mt19937 rng(2026);
  const std::vector<icf_t> truth = make_lowrank(3, rng);

  std::vector<icf_t>   H = truth;
  std::vector<uint8_t> mask((size_t)N_SLOW * N_SUBC, 0);
  std::uniform_real_distribution<double> U(0.0, 1.0);
  for (size_t i = 0; i < mask.size(); i++) {
    mask[i] = (U(rng) < 0.5) ? 1 : 0;
    if (!mask[i]) {
      H[i] = icf_t(0.0f, 0.0f);
    }
  }

  complete_lowrank(H.data(), mask.data(), N_SLOW, N_SUBC, /*rank*/ 4, /*iters*/ 40, /*power*/ 1, nullptr);

  const double rel = frob(H, truth) / frob_norm(truth);
  EXPECT_LT(rel, 0.05) << "relative completion error " << rel << " too large";
}

// Observed entries must be preserved EXACTLY (data consistency).
TEST(matrix_complete, preserves_observed_entries)
{
  std::mt19937 rng(7);
  std::vector<icf_t> truth = make_lowrank(2, rng);
  std::vector<icf_t> H = truth;
  std::vector<uint8_t> mask((size_t)N_SLOW * N_SUBC, 0);
  std::uniform_real_distribution<double> U(0.0, 1.0);
  for (size_t i = 0; i < mask.size(); i++) {
    mask[i] = (U(rng) < 0.4) ? 1 : 0;
    if (!mask[i]) H[i] = icf_t(0.0f, 0.0f);
  }
  complete_lowrank(H.data(), mask.data(), N_SLOW, N_SUBC, 4, 20, 1, nullptr);
  for (size_t i = 0; i < mask.size(); i++) {
    if (mask[i]) {
      const icf_t d = H[i] - truth[i];
      EXPECT_LT(std::abs(d), 1e-3f) << "observed entry " << i << " was altered";
    }
  }
}

// The payoff: a single moving target whose slow-time signal is amplitude-GATED (only some slots
// observed) produces Doppler harmonics; completing the grid first collapses them. Compare the Doppler
// spectrum (at the target's range) of the masked-then-zero-filled grid vs the completed grid.
TEST(matrix_complete, removes_amplitude_gating_doppler_harmonics)
{
  // One target: pure rank-1 tone. Doppler fd = 6 cycles / 64 slots.
  const double fd = 6.0 / (double)N_SLOW, tau = 0.05;
  std::vector<icf_t> full((size_t)N_SLOW * N_SUBC);
  for (uint32_t n = 0; n < N_SLOW; n++)
    for (uint32_t k = 0; k < N_SUBC; k++) {
      const double ph = 2.0 * M_PI * (fd * (double)n + tau * (double)k);
      full[(size_t)n * N_SUBC + k] = icf_t((float)std::cos(ph), (float)std::sin(ph));
    }

  // Per-slot PARTIAL subcarrier occupancy that VARIES slot to slot -- the physical scheduler case
  // (every occupied slot allocates SOME PRBs, but a different, moving block each time). Every row has
  // observations (a completely empty row is unrecoverable and never happens in the real pipeline,
  // which only accumulates slots that carry a reference). Subcarrier 0 is therefore observed in some
  // slots and not others -> its slow-time series is amplitude-gated -> Doppler harmonics; completion
  // fills it in every slot -> clean tone.
  const uint32_t width = N_SUBC / 2; // half-band allocation, moving each slot
  std::vector<uint8_t> mask((size_t)N_SLOW * N_SUBC, 0);
  for (uint32_t n = 0; n < N_SLOW; n++) {
    const uint32_t start = (uint32_t)((uint64_t)n * 37u) % (N_SUBC - width + 1);
    for (uint32_t k = start; k < start + width; k++) mask[(size_t)n * N_SUBC + k] = 1;
  }

  std::vector<icf_t> gated = full, completed = full;
  for (size_t i = 0; i < mask.size(); i++) {
    if (!mask[i]) { gated[i] = icf_t(0.0f, 0.0f); completed[i] = icf_t(0.0f, 0.0f); }
  }
  complete_lowrank(completed.data(), mask.data(), N_SLOW, N_SUBC, /*rank*/ 1, /*iters*/ 80, /*power*/ 2, nullptr);

  // Doppler spectrum at subcarrier 0 (single-tone, so any subcarrier works): |DFT over slow time|.
  auto dopp_peaks = [](const std::vector<icf_t>& M) {
    std::vector<double> P(N_SLOW, 0.0);
    for (uint32_t d = 0; d < N_SLOW; d++) {
      icf_t acc(0.0f, 0.0f);
      for (uint32_t n = 0; n < N_SLOW; n++) {
        const double ph = -2.0 * M_PI * (double)d * (double)n / (double)N_SLOW;
        acc += M[(size_t)n * N_SUBC + 0] * icf_t((float)std::cos(ph), (float)std::sin(ph));
      }
      P[d] = (double)acc.real() * acc.real() + (double)acc.imag() * acc.imag();
    }
    return P;
  };
  const std::vector<double> pg = dopp_peaks(gated);
  const std::vector<double> pc = dopp_peaks(completed);

  const uint32_t fund = 6; // true Doppler bin
  // Harmonic energy (everything outside the fundamental +/-1 bin) relative to the fundamental.
  auto harm_ratio = [&](const std::vector<double>& P) {
    double fundp = 0.0, harm = 0.0;
    for (uint32_t d = 0; d < N_SLOW; d++) {
      if (std::abs((int)d - (int)fund) <= 1) fundp = std::max(fundp, P[d]);
    }
    for (uint32_t d = 2; d < N_SLOW; d++) {
      if (std::abs((int)d - (int)fund) > 2) harm = std::max(harm, P[d]);
    }
    return harm / (fundp + 1e-12);
  };
  const double rg = harm_ratio(pg), rc = harm_ratio(pc);
  EXPECT_LT(rc, rg * 0.5) << "completion should roughly halve (or better) the harmonic-to-fundamental "
                             "ratio: gated=" << rg << " completed=" << rc;
}

int main(int argc, char** argv)
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
