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

/*! \file openair1/PHY/NR_UE_ISAC/tests/occ_clean_test.cc
 * \brief Integration test for occupancy-aware forward-model CLEAN (clean_occ_aware).
 *
 * Builds a synthetic CPI with ONE moving target present on every subcarrier, then imposes a bursty
 * per-row OCCUPANCY mask (some slow-time rows fully occupied, some sparse) -- exactly the 5G-scheduling
 * amplitude gating that turns a target's slow-time tone into a harmonic-rich signal (replicas at
 * multiples of the target Doppler). Passing the SAME occupancy mask through the range-Doppler
 * processor with occupancy masking reproduces those replicas in the RVM. The test checks that
 * occupancy-aware CLEAN (which forward-models each component THROUGH the occupancy mask) suppresses the
 * 2x-Doppler harmonic replica relative to leaving it in place, while preserving the true fundamental.
 */

#include <cmath>
#include <complex>
#include <vector>

#include <gtest/gtest.h>

#include "defs_nr_UE_ISAC.h"
#include "range_doppler.h"

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

constexpr uint32_t NSLOW = 48; // slow-time rows (CPI length)
constexpr uint32_t NSUBC = 96; // reference subcarriers

nr_isac_carrier_t carrier()
{
  nr_isac_carrier_t c;
  c.scs_hz        = 30000.0f;
  c.dl_center_hz  = 3.75e9f;
  return c;
}

// Peak RVM power over a small range band around a target range bin, at a given Doppler bin.
float band_power(const sensing_rvm_t& rvm, uint32_t range_bin, uint32_t dopp_bin, int rwin = 2)
{
  float best = 0.0f;
  for (int dr = -rwin; dr <= rwin; dr++) {
    const int r = (int)range_bin + dr;
    if (r < 0 || r >= (int)rvm.nof_range_bins) continue;
    best = std::max(best, rvm.power[(size_t)r * rvm.nof_doppler_bins + dopp_bin]);
  }
  return best;
}

// Strongest SPURIOUS power in the target's range band: the max over all Doppler bins EXCEPT a window
// around the fundamental and the zero-Doppler notch. This is the dominant gating replica, wherever the
// gating spectrum places it (fd +/- k*f_gate), without assuming it sits at exactly 2x the velocity.
float max_spurious(const sensing_rvm_t& rvm, uint32_t range_bin, uint32_t d_fund, int fund_guard, int zdg)
{
  const uint32_t half = rvm.nof_doppler_bins / 2;
  float best = 0.0f;
  for (uint32_t d = 0; d < rvm.nof_doppler_bins; d++) {
    if (std::abs((int)d - (int)d_fund) <= fund_guard) continue; // skip the fundamental
    if (std::abs((int)d - (int)half) <= zdg) continue;          // skip the zero-Doppler notch
    best = std::max(best, band_power(rvm, range_bin, d));
  }
  return best;
}

} // namespace

// Occupancy-aware CLEAN suppresses the amplitude-gating 2x-Doppler harmonic while keeping the
// fundamental, versus the matched-filter path (same occupancy masking, but no deconvolution) which
// leaves the harmonic in the map.
TEST(occ_clean, suppresses_gated_doppler_harmonic)
{
  const uint32_t half = NSLOW / 2;
  const uint32_t r0   = 24;          // target range bin
  const int      d0u  = 6;           // target Doppler (unshifted); fundamental at ds = half + d0u
  const uint32_t d_fund = half + d0u;

  // Bursty per-row occupancy with a period-3 gating pattern (fully occupied 1 row in 3, sparse
  // otherwise). The target tone is present on ALL subcarriers; the mask decides what the processor
  // sees each row, so the per-row energy modulation -- hence the gating replicas in Doppler -- is
  // entirely in the occupancy. A period-3 pattern places clear replicas at fd +/- NSLOW/3.
  std::vector<icf_t>   grid((size_t)NSLOW * NSUBC, icf_t(0.0f, 0.0f));
  std::vector<uint8_t> occ((size_t)NSLOW * NSUBC, 0);
  std::vector<uint32_t> row_comb(NSLOW, 1);
  std::vector<double>   row_time(NSLOW, 0.0);
  for (uint32_t n = 0; n < NSLOW; n++) {
    row_time[n] = (double)n; // uniform slow-time spacing (1 slot/row) -> replicas are pure gating, not jitter
    const bool full = (n % 3) == 0;
    for (uint32_t m = 0; m < NSUBC; m++) {
      const double ph = -2.0 * M_PI * (double)m * (double)r0 / (double)NSUBC +
                         2.0 * M_PI * (double)d0u * (double)n / (double)NSLOW;
      grid[(size_t)n * NSUBC + m] = icf_t((float)std::cos(ph), (float)std::sin(ph));
      occ[(size_t)n * NSUBC + m]  = (full || (m % 4 == 0)) ? 1 : 0; // sparse rows keep every 4th subcarrier
    }
  }

  auto run = [&](bool occ_aware) {
    nr_isac_args_t a;
    a.zero_doppler_guard = 1;
    a.zero_range_guard   = 1;
    a.max_detections     = 16;
    if (occ_aware) {
      a.clean_deconv     = true;
      a.clean_occ_aware  = true;
      a.clean_loop_gain  = 0.9f;
      a.clean_stop_db    = 30.0f;
      a.clean_restore_bins = 1;
      a.clean_max_components = 4;
    } else {
      a.detector         = "matched_filter"; // same occ masking, NO deconvolution -> harmonic remains
      a.mf_per_row_norm  = false;            // keep the gating (norm would flatten it)
    }
    range_doppler rd(a);
    sensing_rvm_t rvm;
    std::vector<sensing_detection_t> dets;
    rd.process(grid.data(), NSLOW, NSUBC, 1, carrier(), 1.0f, row_comb.data(), rvm, dets,
               row_time.data(), occ.data());
    return rvm;
  };

  const sensing_rvm_t base  = run(false); // harmonic present
  const sensing_rvm_t clean = run(true);  // occ-aware CLEAN

  // Fundamental must survive CLEAN (not erased).
  const float fund_base  = band_power(base, r0, d_fund);
  const float fund_clean = band_power(clean, r0, d_fund);
  ASSERT_GT(fund_base, 0.0f);
  EXPECT_GT(fund_clean, 0.2f * fund_base) << "occ-aware CLEAN erased the fundamental";

  // The dominant gating replica (relative to the fundamental) must drop substantially under CLEAN.
  const float spur_ratio_base  = max_spurious(base, r0, d_fund, 2, 1) / fund_base;
  const float spur_ratio_clean = max_spurious(clean, r0, d_fund, 2, 1) / std::max(fund_clean, 1e-9f);
  EXPECT_GT(spur_ratio_base, 0.02f) << "test premise: gating should produce a visible Doppler replica";
  EXPECT_LT(spur_ratio_clean, 0.5f * spur_ratio_base)
      << "occ-aware CLEAN should suppress the gated Doppler replica (base ratio=" << spur_ratio_base
      << ", clean ratio=" << spur_ratio_clean << ")";
}

// Sub-bin interpolation: a target placed deliberately OFF a bin centre must be reported at its true
// fractional range/velocity, not snapped to the bin. Without interpolation the error is bounded only
// by half a bin (1.5 m in range here); with it, well under that.
TEST(occ_clean, subbin_interpolation_recovers_an_off_bin_target)
{
  const uint32_t half = NSLOW / 2;
  const double   r_true = 24.35;  // 0.35 bin off centre
  const double   d_true = 6.30;   // 0.30 bin off centre

  std::vector<icf_t>    grid((size_t)NSLOW * NSUBC, icf_t(0.0f, 0.0f));
  std::vector<uint8_t>  occ((size_t)NSLOW * NSUBC, 1);
  std::vector<uint32_t> row_comb(NSLOW, 1);
  std::vector<double>   row_time(NSLOW, 0.0);
  for (uint32_t n = 0; n < NSLOW; n++) {
    row_time[n] = (double)n;
    for (uint32_t m = 0; m < NSUBC; m++) {
      const double ph = -2.0 * M_PI * (double)m * r_true / (double)NSUBC +
                         2.0 * M_PI * d_true * (double)n / (double)NSLOW;
      grid[(size_t)n * NSUBC + m] = icf_t((float)std::cos(ph), (float)std::sin(ph));
    }
  }

  auto run = [&](bool subbin) {
    nr_isac_args_t a;
    a.zero_doppler_guard = 1;
    a.zero_range_guard   = 1;
    a.max_detections     = 8;
    a.subbin_interp      = subbin;
    range_doppler rd(a);
    sensing_rvm_t rvm;
    std::vector<sensing_detection_t> dets;
    rd.process(grid.data(), NSLOW, NSUBC, 1, carrier(), 1.0f, row_comb.data(), rvm, dets,
               row_time.data(), occ.data());
    // strongest detection
    sensing_detection_t best{};
    float bs = -1e9f;
    for (const auto& x : dets) {
      if (x.snr_db > bs) { bs = x.snr_db; best = x; }
    }
    return std::make_pair(best, rvm);
  };

  auto off = run(false);
  auto on  = run(true);
  ASSERT_GT(off.second.range_res_m, 0.0f);

  const double truth_m  = r_true * off.second.range_res_m;
  const double err_off  = std::abs((double)off.first.range_m - truth_m);
  const double err_on   = std::abs((double)on.first.range_m - truth_m);
  EXPECT_LT(err_on, err_off) << "sub-bin did not improve range (off=" << err_off << " on=" << err_on << ")";
  EXPECT_LT(err_on, 0.5 * off.second.range_res_m)
      << "sub-bin range error should be well inside half a bin, got " << err_on;

  // Doppler: same target, fractional bin 0.30 off centre.
  const double vtruth = -((d_true + (double)half) - (double)half) * on.second.vel_res_mps;
  EXPECT_LT(std::abs((double)on.first.vel_mps - vtruth), std::abs((double)off.first.vel_mps - vtruth) + 1e-6)
      << "sub-bin should not worsen velocity";
}

int main(int argc, char** argv)
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
