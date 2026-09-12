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

/*! \file openair1/PHY/NR_UE_ISAC/tests/sparse_doppler_test.cc
 * \brief Offline tests for L1/FISTA sparse Doppler recovery (sparse_doppler.{h,cc}).
 *
 * Mirrors the Python numerical validation done before porting to C++ (see
 * PHASE2_MOT_MULTIUE_HANDOVER.md): a gappy (scheduler-like burst mask) single tone, comparing the
 * sparse recovery's peak-to-harmonic ratio against a plain dense NUDFT of the same data, and a
 * two-target case checking a genuine second, weaker target survives at roughly its correct relative
 * power instead of being erased by the L1 penalty.
 */

#include <algorithm> // std::nth_element (line ~151); was missing, broke the test_sparse_doppler build
#include <cmath>
#include <complex>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "defs_nr_UE_ISAC.h"
#include "sparse_doppler.h"

using namespace nr_isac;

namespace {

constexpr uint32_t N_SLOW = 64;
constexpr uint32_t N_DOPP = 64;

// Bursty occupancy mask matching the scheduler-gating model: on for 3 slots, off for 3, repeat.
std::vector<double> bursty_times()
{
  std::vector<double> t;
  for (uint32_t n = 0; n < N_SLOW; n++) {
    if ((n / 3) % 2 == 0) {
      t.push_back((double)n);
    }
  }
  return t;
}

// Dense NUDFT of the same gappy row (no sparsity) -- what the existing doppler_nudft path computes,
// used as the baseline this module must beat.
std::vector<icf_t> dense_nudft(const std::vector<icf_t>& y, const std::vector<double>& t, uint32_t N)
{
  std::vector<icf_t> x(N, icf_t(0.0f, 0.0f));
  const double norm = 1.0 / std::sqrt((double)N);
  for (uint32_t n = 0; n < N; n++) {
    icf_t acc(0.0f, 0.0f);
    for (size_t k = 0; k < t.size(); k++) {
      const double ph = -2.0 * M_PI * (double)n * t[k] / (double)N;
      acc += y[k] * icf_t((float)(std::cos(ph) * norm), (float)(std::sin(ph) * norm));
    }
    x[n] = acc;
  }
  return x;
}

double peak_to_harmonic_db(const std::vector<icf_t>& x)
{
  uint32_t peak = 0;
  float    peakp = 0.0f;
  for (uint32_t n = 0; n < x.size(); n++) {
    const float p = std::norm(x[n]);
    if (p > peakp) { peakp = p; peak = n; }
  }
  float runner = 0.0f;
  for (uint32_t n = 0; n < x.size(); n++) {
    if (std::abs((int)n - (int)peak) > 2) {
      runner = std::max(runner, std::norm(x[n]));
    }
  }
  return 10.0 * std::log10((double)peakp / std::max((double)runner, 1e-12));
}

} // namespace

// Core claim: on a gappy single tone, FISTA's peak-to-harmonic ratio should be dramatically better
// than the dense NUDFT's -- the whole point of using sparse recovery instead of a dense transform.
TEST(sparse_doppler, beats_dense_nudft_peak_to_harmonic_ratio_on_gappy_tone)
{
  const std::vector<double> t = bursty_times();
  const double fd = 6.0 / N_SLOW;
  std::vector<icf_t> y(t.size());
  for (size_t k = 0; k < t.size(); k++) {
    const double ph = 2.0 * M_PI * fd * t[k];
    y[k] = icf_t((float)std::cos(ph), (float)std::sin(ph));
  }

  const std::vector<icf_t> dense = dense_nudft(y, t, N_DOPP);
  const double dense_db = peak_to_harmonic_db(dense);

  sparse_doppler_ctx ctx;
  sparse_doppler_prepare(t.data(), (uint32_t)t.size(), 1.0f, N_DOPP, ctx);
  // Tiny lambda (near-noiseless synthetic signal): just enough to enforce sparsity, not to fight noise.
  std::vector<icf_t> x;
  sparse_doppler_solve(ctx, y.data(), 80, 0.05f, x);
  const double sparse_db = peak_to_harmonic_db(x);

  EXPECT_GT(sparse_db, dense_db + 20.0)
      << "sparse (" << sparse_db << " dB) should dramatically beat dense NUDFT (" << dense_db << " dB)";
  EXPECT_GT(sparse_db, 60.0) << "sparse recovery should nearly perfectly suppress the harmonic on a "
                                "clean synthetic tone, got " << sparse_db << " dB";
}

// A genuine second, weaker target must survive (not be erased by the L1 penalty) at roughly its
// correct relative power -- sparsity must not be so aggressive it treats a real weak target as noise.
TEST(sparse_doppler, preserves_a_second_weaker_genuine_target)
{
  const std::vector<double> t = bursty_times();
  const double fd1 = 6.0 / N_SLOW, fd2 = -10.0 / N_SLOW;
  const double amp2 = 0.6;
  std::vector<icf_t> y(t.size());
  std::mt19937 rng(1);
  std::normal_distribution<double> noise(0.0, 0.15 / std::sqrt(2.0));
  for (size_t k = 0; k < t.size(); k++) {
    const double ph1 = 2.0 * M_PI * fd1 * t[k];
    const double ph2 = 2.0 * M_PI * fd2 * t[k];
    const std::complex<double> s = std::polar(1.0, ph1) + amp2 * std::polar(1.0, ph2) +
                                    std::complex<double>(noise(rng), noise(rng));
    y[k] = icf_t((float)s.real(), (float)s.imag());
  }

  sparse_doppler_ctx ctx;
  sparse_doppler_prepare(t.data(), (uint32_t)t.size(), 1.0f, N_DOPP, ctx);

  // Noise-floor estimate from the dense profile's median (same estimator range_doppler.cc uses).
  const std::vector<icf_t> dense = dense_nudft(y, t, N_DOPP);
  std::vector<float> prof(N_DOPP);
  for (uint32_t n = 0; n < N_DOPP; n++) prof[n] = std::norm(dense[n]);
  std::vector<float> sorted = prof;
  std::nth_element(sorted.begin(), sorted.begin() + N_DOPP / 2, sorted.end());
  const float sigma  = std::sqrt(std::max(sorted[N_DOPP / 2], 1e-20f) / (float)M_LN2);
  const float lambda = sigma * std::sqrt(2.0f * std::log((float)N_DOPP)) * 0.5f;

  std::vector<icf_t> x;
  sparse_doppler_solve(ctx, y.data(), 80, lambda, x);

  uint32_t i1 = 0, i2 = 0;
  float p1 = 0.0f, p2 = 0.0f;
  for (uint32_t n = 0; n < N_DOPP; n++) {
    const float p = std::norm(x[n]);
    if (p > p1) { p2 = p1; i2 = i1; p1 = p; i1 = n; }
    else if (p > p2) { p2 = p; i2 = n; }
  }
  ASSERT_NE(i1, i2) << "did not find two distinct peaks";
  EXPECT_GT(p2, 0.0f) << "the weaker second target was fully erased";
  const double ratio = (double)p2 / (double)p1;
  // Expected power ratio ~= amp2^2 = 0.36; wide tolerance since this is a single noisy realisation.
  EXPECT_GT(ratio, 0.1) << "second target power ratio " << ratio << " far below expected ~0.36";
  EXPECT_LT(ratio, 0.9) << "second target power ratio " << ratio << " implausibly close to the first";
}

int main(int argc, char** argv)
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
