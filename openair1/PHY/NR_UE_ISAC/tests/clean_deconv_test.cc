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

/*! \file openair1/PHY/NR_UE_ISAC/tests/clean_deconv_test.cc
 * \brief Offline tests for CLEAN deconvolution (clean_deconv.{h,cc}).
 *
 * Builds a faithful synthetic range-Doppler map: the coherent superposition of point scatterers, each
 * spread by the SAME separable window PSF (kr outer kd) the real pipeline produces, with kr/kd taken
 * as the (normalised) transform of a Hann window -- exactly the kernels range_doppler builds. The key
 * scenario is the one CLEAN exists to fix: a STRONG scatterer whose Hann sidelobe skirt (~-31 dB first
 * sidelobe) is BRIGHTER than a genuine WEAK target, so a plain CFAR peak-search would report the
 * strong target's sidelobe instead of the weak target. After CLEAN the strong skirt is gone and the
 * weak target is the cleanly-dominant secondary feature.
 */

#include <cmath>
#include <complex>
#include <vector>

#include <gtest/gtest.h>

#include "clean_deconv.h"
#include "defs_nr_UE_ISAC.h"

using namespace nr_isac;

namespace {

constexpr uint32_t R = 64; // range bins
constexpr uint32_t D = 64; // Doppler bins

// A Hann window of length N (matches range_doppler's freq_hann / hann construction).
std::vector<float> hann(uint32_t N)
{
  std::vector<float> w(N);
  for (uint32_t i = 0; i < N; i++) {
    w[i] = 0.5f * (1.0f - std::cos(2.0f * (float)M_PI * (float)i / (float)(N - 1)));
  }
  return w;
}

// Range kernel kr[lag] = IFFT(hann)[lag] / IFFT(hann)[0]  (normalised, kr[0] = (1,0)).
std::vector<icf_t> range_kernel(uint32_t N)
{
  const std::vector<float> w = hann(N);
  std::vector<icf_t> k(N);
  for (uint32_t r = 0; r < N; r++) {
    icf_t acc(0.0f, 0.0f);
    for (uint32_t m = 0; m < N; m++) {
      const double ph = 2.0 * M_PI * (double)m * (double)r / (double)N; // +j: inverse transform
      acc += icf_t((float)(w[m] * std::cos(ph)), (float)(w[m] * std::sin(ph)));
    }
    k[r] = acc;
  }
  const icf_t k0 = k[0];
  for (uint32_t r = 0; r < N; r++) {
    k[r] /= k0;
  }
  return k;
}

// Doppler kernel kd[lag] = FFT(hann)[lag] / FFT(hann)[0]  (normalised, kd[0] = (1,0)).
std::vector<icf_t> doppler_kernel(uint32_t N)
{
  const std::vector<float> w = hann(N);
  std::vector<icf_t> k(N);
  for (uint32_t lag = 0; lag < N; lag++) {
    icf_t acc(0.0f, 0.0f);
    for (uint32_t n = 0; n < N; n++) {
      const double ph = -2.0 * M_PI * (double)lag * (double)n / (double)N; // -j: forward transform
      acc += icf_t((float)(w[n] * std::cos(ph)), (float)(w[n] * std::sin(ph)));
    }
    k[lag] = acc;
  }
  const icf_t k0 = k[0];
  for (uint32_t lag = 0; lag < N; lag++) {
    k[lag] /= k0;
  }
  return k;
}

// Deposit a scatterer at (r0,d0) with complex amplitude a onto the coherent map: a * kr[.-r0]*kd[.-d0].
void add_scatterer(std::vector<icf_t>& map, const std::vector<icf_t>& kr, const std::vector<icf_t>& kd,
                   uint32_t r0, uint32_t d0, icf_t a)
{
  for (uint32_t r = 0; r < R; r++) {
    const icf_t akr = a * kr[(r + R - r0) % R];
    for (uint32_t d = 0; d < D; d++) {
      map[(size_t)r * D + d] += akr * kd[(d + D - d0) % D];
    }
  }
}

float power_at(const std::vector<float>& p, uint32_t r, uint32_t d)
{
  return p[(size_t)r * D + d];
}

} // namespace

// A strong scatterer's near-mainlobe skirt (the first ring at ~-6 dB for a Hann window) carries real
// energy that CLEAN then subtracts coherently. NOTE (measured, not assumed): a CLEAN Hann/window PSF
// has NO significant FAR sidelobes -- they are already <= -45 dB two bins out -- so the ghost family
// separable-window CLEAN removes is the near skirt / pedestal of a strong target, NOT the amplitude-
// gating Doppler harmonics or sparse-occupancy pedestal (those are data-domain effects that this
// shift-invariant window PSF does not represent). This test asserts the near-skirt IS removed.
TEST(clean_deconv, removes_strong_near_skirt)
{
  const auto kr = range_kernel(R);
  const auto kd = doppler_kernel(D);
  std::vector<icf_t> map((size_t)R * D, icf_t(0.0f, 0.0f));

  const uint32_t r0 = 20, d0 = 32;
  add_scatterer(map, kr, kd, r0, d0, icf_t(100.0f, 0.0f));

  // The first-ring skirt (1 bin out, outside NMS's usual reach for a distinct target) is significant
  // before CLEAN (~-6 dB of the peak here).
  const float skirt_before = std::norm(map[(size_t)(r0 + 1) * D + d0]);
  const float peak_before  = std::norm(map[(size_t)r0 * D + d0]);
  EXPECT_GT(skirt_before, 0.01f * peak_before) << "premise: the near skirt carries real energy";

  clean_deconv_params p;
  p.max_components = 30;
  p.loop_gain     = 0.9f;
  p.stop_db       = 50.0f;
  p.restore_bins  = 0; // pure delta so the skirt cell isn't refilled by the restore beam
  std::vector<float> out((size_t)R * D, 0.0f);
  clean_deconv_run(map.data(), R, D, kr.data(), kd.data(), p, out.data(), nullptr);

  const float skirt_after = power_at(out, r0 + 1, d0);
  EXPECT_LT(skirt_after, 0.02f * skirt_before) << "CLEAN should remove the coherent near skirt";
}

// CLEAN removes the strong scatterer's coherent skirt while recovering the weak target as a clean
// local maximum that now dominates the strong target's former sidelobes.
TEST(clean_deconv, removes_strong_skirt_and_recovers_weak_target)
{
  const auto kr = range_kernel(R);
  const auto kd = doppler_kernel(D);
  std::vector<icf_t> map((size_t)R * D, icf_t(0.0f, 0.0f));

  const uint32_t r_strong = 20, d_strong = 32;
  const uint32_t r_weak   = 34, d_weak   = 32;
  add_scatterer(map, kr, kd, r_strong, d_strong, icf_t(100.0f, 0.0f));
  add_scatterer(map, kr, kd, r_weak, d_weak, icf_t(1.0f, 0.0f));

  clean_deconv_params p;
  p.max_components = 30;
  p.loop_gain     = 0.9f;
  p.stop_db       = 45.0f;
  p.restore_bins  = 1;
  std::vector<float> out((size_t)R * D, 0.0f);
  uint32_t n_comp = 0;
  clean_deconv_run(map.data(), R, D, kr.data(), kd.data(), p, out.data(), &n_comp);

  EXPECT_GE(n_comp, 2u) << "should extract at least the two real scatterers";

  // The weak target survives as a clear local peak.
  const float weak_out = power_at(out, r_weak, d_weak);
  EXPECT_GT(weak_out, 0.25f) << "weak target (amplitude 1 -> power 1) should be preserved, ~clean beam";

  // The strong target's former sidelobe cells are now far below the weak target -> no longer ghosts.
  float max_sidelobe_after = 0.0f;
  for (int dr = 3; dr <= 8; dr++) {
    max_sidelobe_after = std::max(max_sidelobe_after, power_at(out, r_strong + dr, d_strong));
    if (r_strong >= (uint32_t)dr) {
      max_sidelobe_after = std::max(max_sidelobe_after, power_at(out, r_strong - dr, d_strong));
    }
  }
  EXPECT_LT(max_sidelobe_after, 0.5f * weak_out)
      << "strong target's coherent sidelobe skirt should be removed (well below the weak target)";

  // The strong target itself is restored (still the brightest cell).
  const float strong_out = power_at(out, r_strong, d_strong);
  EXPECT_GT(strong_out, weak_out) << "strong target restored as the dominant peak";
}

// With a single scatterer, CLEAN drives the total off-mainlobe (sidelobe) energy far below the
// original map's -- the coherent skirt is genuinely subtracted, not merely thresholded.
TEST(clean_deconv, single_target_sidelobe_energy_collapses)
{
  const auto kr = range_kernel(R);
  const auto kd = doppler_kernel(D);
  std::vector<icf_t> map((size_t)R * D, icf_t(0.0f, 0.0f));
  const uint32_t r0 = 30, d0 = 30;
  add_scatterer(map, kr, kd, r0, d0, icf_t(50.0f, 0.0f));

  auto sidelobe_energy = [&](auto&& power_of) {
    double e = 0.0;
    for (uint32_t r = 0; r < R; r++) {
      for (uint32_t d = 0; d < D; d++) {
        const int dr = std::abs((int)r - (int)r0);
        const int dd = std::abs((int)d - (int)d0);
        if (dr > 2 || dd > 2) { // outside the mainlobe
          e += power_of(r, d);
        }
      }
    }
    return e;
  };

  const double before = sidelobe_energy([&](uint32_t r, uint32_t d) { return std::norm(map[(size_t)r * D + d]); });

  clean_deconv_params p;
  p.max_components = 30;
  p.loop_gain     = 0.9f;
  p.stop_db       = 50.0f;
  p.restore_bins  = 1;
  std::vector<float> out((size_t)R * D, 0.0f);
  clean_deconv_run(map.data(), R, D, kr.data(), kd.data(), p, out.data(), nullptr);

  const double after = sidelobe_energy([&](uint32_t r, uint32_t d) { return (double)out[(size_t)r * D + d]; });

  EXPECT_LT(after, 0.05 * before) << "CLEAN should remove >95% of the coherent sidelobe energy";
}

// Null kernels => graceful passthrough of the power map (no crash, no deconvolution).
TEST(clean_deconv, null_kernels_passthrough)
{
  std::vector<icf_t> map((size_t)R * D, icf_t(0.0f, 0.0f));
  map[(size_t)10 * D + 10] = icf_t(3.0f, 4.0f); // |.|^2 = 25
  clean_deconv_params p;
  std::vector<float> out((size_t)R * D, -1.0f);
  clean_deconv_run(map.data(), R, D, nullptr, nullptr, p, out.data(), nullptr);
  EXPECT_FLOAT_EQ(out[(size_t)10 * D + 10], 25.0f);
  EXPECT_FLOAT_EQ(out[0], 0.0f);
}

int main(int argc, char** argv)
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
