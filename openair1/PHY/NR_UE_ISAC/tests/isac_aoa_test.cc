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

/*! \file openair1/PHY/NR_UE_ISAC/tests/isac_aoa_test.cc
 * \brief Offline self-test for the receive-array AoA estimator (isac_aoa.{h,cc}),
 *        PHASE3_AOA_MULTISTATIC_HANDOVER §5.4.
 *
 * Synthesises a per-antenna CPI grid directly from the physics — a target at a known delay, Doppler
 * and BEARING, plus a static direct path at its own known bearing — and checks the estimator recovers
 * the bearing it was given.
 *
 * The synthesis deliberately mirrors `openair1/SIMULATION/TOOLS/sensing_channel.c`'s `steer_gain()`
 * sign convention (element closer to the source by `d·û` leads in phase). That is the one place the
 * two halves of this feature have to agree, and getting it wrong yields a confidently MIRRORED bearing
 * rather than an obviously broken one — so it is asserted here against an independently written
 * expression rather than against the estimator's own steering function.
 */

#include <cmath>
#include <complex>
#include <cstdio>
#include <string>
#include <vector>

#include <gtest/gtest.h>

extern "C" {
#include "common/config/config_userapi.h"
#include "common/utils/LOG/log.h"
}

#include "defs_nr_UE_ISAC.h"
#include "isac_aoa.h"

// LOG/CONFIG_LIB stubs needed outside a full softmodem executable (same convention as
// isac_sync_test.cc).
extern "C" configmodule_interface_t *uniqCfg = nullptr;
extern "C" void exit_function(const char *file, const char *function, const int line, const char *s, const int a)
{
  (void)file;
  (void)function;
  (void)line;
  (void)s;
  (void)a;
  abort();
}

using namespace nr_isac;

namespace {

constexpr double C_LIGHT = 299792458.0;
constexpr double TWO_PI  = 6.283185307179586;

struct scene_t {
  uint32_t nof_ant  = 4;
  uint32_t nof_slow = 32;
  uint32_t nof_subc = 256;
  uint32_t scs_hz   = 30000;
  double   fc_hz    = 3414.99e6;
};

nr_isac_carrier_t carrier_of(const scene_t &sc)
{
  nr_isac_carrier_t c = {};
  c.nof_prb           = sc.nof_subc / 12;
  c.scs_hz            = sc.scs_hz;
  c.dl_center_hz      = (uint64_t)sc.fc_hz;
  c.pci               = 2;
  c.slots_per_frame   = 20;
  return c;
}

/// lambda/2 uniform linear array along ENU +x, as a spec string.
std::string ula_spec(uint32_t n, double lambda)
{
  std::string s;
  char        buf[64];
  for (uint32_t i = 0; i < n; i++) {
    std::snprintf(buf, sizeof(buf), "%s%.9f,0", i ? ";" : "", (double)i * lambda / 2.0);
    s += buf;
  }
  return s;
}

/// One scatterer to inject into the synthetic grid.
struct src_t {
  double range_bin;    ///< fractional range bin (== delay * nof_subc * scs)
  double dopp_cycles;  ///< Doppler in cycles per slot
  double bearing_rad;  ///< true ENU bearing from the receiver
  double amp = 1.0;
};

/// Build a per-antenna CPI grid containing the given sources, plus optional white noise.
/// Uses the SAME steering convention as sensing_channel.c, written out independently here.
void synth(const scene_t &sc, const aoa_array_t &arr, const std::vector<src_t> &srcs, double noise_amp,
           std::vector<icf_t> &h, std::vector<uint8_t> &occ, std::vector<double> &row_t,
           const std::vector<icf_t> *channel_err = nullptr)
{
  const size_t plane = (size_t)sc.nof_slow * sc.nof_subc;
  h.assign((size_t)sc.nof_ant * plane, icf_t(0.0f, 0.0f));
  occ.assign(plane, 1);
  row_t.assign(sc.nof_slow, 0.0);
  for (uint32_t n = 0; n < sc.nof_slow; n++) {
    row_t[n] = (double)n; // uniform, one row per slot
  }

  unsigned rng = 12345u;
  auto     urand = [&rng]() {
    rng = rng * 1103515245u + 12345u;
    return ((double)((rng >> 16) & 0x7fff) / 16383.5) - 1.0;
  };

  for (uint32_t a = 0; a < sc.nof_ant; a++) {
    for (uint32_t n = 0; n < sc.nof_slow; n++) {
      for (uint32_t c = 0; c < sc.nof_subc; c++) {
        icf_t acc(0.0f, 0.0f);
        for (const src_t &s : srcs) {
          // Delay -> a phase ramp across subcarriers with the sign the range IFFT inverts, Doppler ->
          // a phase ramp across slow time.
          const double ph = -TWO_PI * (double)c * s.range_bin / (double)sc.nof_subc
                            + TWO_PI * s.dopp_cycles * row_t[n];
          // Array steering: element a is closer to the source by d_a . u, hence a phase ADVANCE.
          const double proj = (double)arr.ex[a] * std::cos(s.bearing_rad)
                              + (double)arr.ey[a] * std::sin(s.bearing_rad);
          const double sp   = TWO_PI * proj / arr.lambda_m;
          acc += icf_t((float)(s.amp * std::cos(ph + sp)), (float)(s.amp * std::sin(ph + sp)));
        }
        if (noise_amp > 0.0) {
          acc += icf_t((float)(noise_amp * urand()), (float)(noise_amp * urand()));
        }
        if (channel_err != nullptr) {
          acc *= (*channel_err)[a]; // per-channel phase/gain error, the thing self-calibration removes
        }
        h[(size_t)a * plane + (size_t)n * sc.nof_subc + c] = acc;
      }
    }
  }
}

nr_isac_args_t base_args()
{
  nr_isac_args_t a;
  a.aoa_enable           = true;
  a.aoa_estimator        = "beamscan";
  a.aoa_scan_step_deg    = 0.5f;
  a.aoa_min_snr_db       = 0.0f;
  a.aoa_cell_search_bins = 2;
  a.nominal_los_range_m  = 0.0f;
  return a;
}

double wrap_deg(double d)
{
  while (d > 180.0) {
    d -= 360.0;
  }
  while (d < -180.0) {
    d += 360.0;
  }
  return d;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Array parsing
// ---------------------------------------------------------------------------------------------

TEST(isac_aoa_array, parses_a_ula_and_flags_its_properties)
{
  const double lambda = C_LIGHT / 3414.99e6;
  aoa_array_t  arr;
  ASSERT_TRUE(parse_rx_array(ula_spec(4, lambda), 0.0, 3414.99e6, arr));
  EXPECT_EQ(arr.size(), 4u);
  EXPECT_TRUE(arr.usable());
  EXPECT_TRUE(arr.collinear) << "a ULA must be flagged collinear (mirror ambiguity)";
  EXPECT_FALSE(arr.ambiguous) << "lambda/2 spacing must NOT be flagged as spatially aliasing";
  // Tolerance is set by the spec string's own precision (%.9f), not by the parser.
  EXPECT_NEAR(arr.max_spacing_m, 1.5 * lambda, 1e-8);
}

TEST(isac_aoa_array, flags_spacing_above_half_lambda_as_ambiguous)
{
  const double lambda = C_LIGHT / 3414.99e6;
  aoa_array_t  arr;
  char         spec[64];
  std::snprintf(spec, sizeof(spec), "0,0;%.9f,0", lambda); // full-wavelength spacing
  ASSERT_TRUE(parse_rx_array(spec, 0.0, 3414.99e6, arr));
  EXPECT_TRUE(arr.ambiguous);
}

TEST(isac_aoa_array, rejects_unusable_specs)
{
  aoa_array_t arr;
  EXPECT_FALSE(parse_rx_array("", 0.0, 3.5e9, arr));
  EXPECT_FALSE(parse_rx_array("0,0", 0.0, 3.5e9, arr)) << "a single element carries no bearing";
  EXPECT_FALSE(parse_rx_array("garbage;also,bad,extra", 0.0, 3.5e9, arr));
}

TEST(isac_aoa_array, boresight_rotates_the_element_frame_into_enu)
{
  const double lambda = C_LIGHT / 3.5e9;
  aoa_array_t  along_x, rotated;
  ASSERT_TRUE(parse_rx_array(ula_spec(2, lambda), 90.0, 3.5e9, rotated));
  ASSERT_TRUE(parse_rx_array(ula_spec(2, lambda), 0.0, 3.5e9, along_x));
  // Rotating a +x array by 90 deg must put element 1 on +y.
  EXPECT_NEAR(rotated.ex[1], 0.0, 1e-6);
  EXPECT_NEAR(rotated.ey[1], along_x.ex[1], 1e-6);
}

TEST(isac_aoa_array, a_two_dimensional_array_is_not_collinear)
{
  const double lambda = C_LIGHT / 3.5e9;
  char         spec[128];
  std::snprintf(spec, sizeof(spec), "0,0;%.9f,0;0,%.9f;%.9f,%.9f", lambda / 2, lambda / 2, lambda / 2, lambda / 2);
  aoa_array_t arr;
  ASSERT_TRUE(parse_rx_array(spec, 0.0, 3.5e9, arr));
  EXPECT_FALSE(arr.collinear) << "a square array resolves the mirror and must scan the full circle";
}

// ---------------------------------------------------------------------------------------------
// Bearing estimation
// ---------------------------------------------------------------------------------------------

/// The headline property: a target injected at a known bearing must come back at that bearing.
/// Swept across the array's usable field of view, since accuracy is strongly geometry-dependent.
TEST(isac_aoa, recovers_the_injected_bearing_across_the_field_of_view)
{
  scene_t      sc;
  const double lambda = C_LIGHT / sc.fc_hz;
  aoa_array_t  arr;
  ASSERT_TRUE(parse_rx_array(ula_spec(4, lambda), 0.0, sc.fc_hz, arr));

  // Array axis is ENU +x, so its broadside (and default scan centre) is +y = 90 deg.
  for (double truth_deg : {40.0, 60.0, 90.0, 120.0, 140.0}) {
    std::vector<icf_t>   h;
    std::vector<uint8_t> occ;
    std::vector<double>  row_t;
    synth(sc, arr, {{30.0, 0.12, truth_deg * M_PI / 180.0, 1.0}}, 0.0, h, occ, row_t);

    aoa_estimator est(base_args(), arr);
    std::vector<sensing_detection_t> dets(1);
    dets[0].range_bin   = 30;
    dets[0].doppler_bin = (uint32_t)std::lround(0.12 * sc.nof_slow) + sc.nof_slow / 2;
    dets[0].snr_db      = 20.0f;
    std::vector<aoa_estimate_t> out;
    est.process(h.data(), sc.nof_ant, sc.nof_slow, sc.nof_slow, sc.nof_subc, occ.data(), row_t.data(), 1.0, carrier_of(sc), dets,
                out);

    ASSERT_EQ(out.size(), 1u);
    ASSERT_TRUE(out[0].valid) << "no bearing produced at " << truth_deg << " deg";
    EXPECT_NEAR(wrap_deg((double)out[0].azimuth_deg - truth_deg), 0.0, 2.0)
        << "truth " << truth_deg << " deg, got " << out[0].azimuth_deg << " deg";
    EXPECT_GT(out[0].azimuth_std_deg, 0.0f);
    EXPECT_TRUE(out[0].mirror_ambiguous) << "a ULA must declare its mirror ambiguity";
  }
}

/// Two elements have a closed form rather than a scan; it must agree with the injected bearing and
/// with the beamscan estimator on the same data (they are the same estimator for one source).
TEST(isac_aoa, interferometry_agrees_with_beamscan_on_two_elements)
{
  scene_t sc;
  sc.nof_ant          = 2;
  const double lambda = C_LIGHT / sc.fc_hz;
  aoa_array_t  arr;
  ASSERT_TRUE(parse_rx_array(ula_spec(2, lambda), 0.0, sc.fc_hz, arr));

  const double truth = 70.0 * M_PI / 180.0;
  std::vector<icf_t>   h;
  std::vector<uint8_t> occ;
  std::vector<double>  row_t;
  synth(sc, arr, {{45.0, -0.2, truth, 1.0}}, 0.0, h, occ, row_t);

  std::vector<sensing_detection_t> dets(1);
  dets[0].range_bin   = 45;
  dets[0].doppler_bin = (uint32_t)std::lround(-0.2 * sc.nof_slow) + sc.nof_slow / 2;
  dets[0].snr_db      = 20.0f;

  auto run = [&](const std::string &which) {
    nr_isac_args_t a = base_args();
    a.aoa_estimator  = which;
    aoa_estimator               est(a, arr);
    std::vector<aoa_estimate_t> out;
    est.process(h.data(), sc.nof_ant, sc.nof_slow, sc.nof_slow, sc.nof_subc, occ.data(), row_t.data(), 1.0, carrier_of(sc), dets,
                out);
    EXPECT_TRUE(out[0].valid);
    return (double)out[0].azimuth_deg;
  };
  const double a_int = run("interferometry");
  const double a_bs  = run("beamscan");
  EXPECT_NEAR(wrap_deg(a_int - 70.0), 0.0, 2.0);
  EXPECT_NEAR(wrap_deg(a_int - a_bs), 0.0, 2.0);
}

/// MUSIC over the neighbourhood-snapshot covariance must also land on the truth. It is opt-in and
/// falls back to beamscan when the array is too small, so the fallback is checked too.
TEST(isac_aoa, music_recovers_the_bearing_and_falls_back_gracefully)
{
  scene_t      sc;
  const double lambda = C_LIGHT / sc.fc_hz;
  aoa_array_t  arr4, arr2;
  ASSERT_TRUE(parse_rx_array(ula_spec(4, lambda), 0.0, sc.fc_hz, arr4));
  ASSERT_TRUE(parse_rx_array(ula_spec(2, lambda), 0.0, sc.fc_hz, arr2));

  const double truth = 105.0;
  nr_isac_args_t a   = base_args();
  a.aoa_estimator    = "music";

  for (const aoa_array_t *arr : {&arr4, &arr2}) {
    scene_t s2 = sc;
    s2.nof_ant = arr->size();
    std::vector<icf_t>   h;
    std::vector<uint8_t> occ;
    std::vector<double>  row_t;
    synth(s2, *arr, {{60.0, 0.05, truth * M_PI / 180.0, 1.0}}, 0.02, h, occ, row_t);

    std::vector<sensing_detection_t> dets(1);
    dets[0].range_bin   = 60;
    dets[0].doppler_bin = (uint32_t)std::lround(0.05 * s2.nof_slow) + s2.nof_slow / 2;
    dets[0].snr_db      = 20.0f;

    aoa_estimator               est(a, *arr);
    std::vector<aoa_estimate_t> out;
    est.process(h.data(), s2.nof_ant, s2.nof_slow, s2.nof_slow, s2.nof_subc, occ.data(), row_t.data(), 1.0, carrier_of(s2), dets,
                out);
    ASSERT_TRUE(out[0].valid) << "no bearing with " << arr->size() << " elements";
    EXPECT_NEAR(wrap_deg((double)out[0].azimuth_deg - truth), 0.0, 3.0)
        << arr->size() << " elements, got " << out[0].azimuth_deg;
  }
}

/// A detection below `aoa_min_snr_db` must yield NO bearing rather than a noisy one — a missing
/// azimuth degrades cleanly to a 2-D measurement at the central node, a wrong one corrupts the fix.
TEST(isac_aoa, withholds_a_bearing_below_the_snr_gate)
{
  scene_t      sc;
  const double lambda = C_LIGHT / sc.fc_hz;
  aoa_array_t  arr;
  ASSERT_TRUE(parse_rx_array(ula_spec(4, lambda), 0.0, sc.fc_hz, arr));
  std::vector<icf_t>   h;
  std::vector<uint8_t> occ;
  std::vector<double>  row_t;
  synth(sc, arr, {{30.0, 0.1, 1.2, 1.0}}, 0.0, h, occ, row_t);

  nr_isac_args_t a = base_args();
  a.aoa_min_snr_db = 12.0f;
  aoa_estimator                    est(a, arr);
  std::vector<sensing_detection_t> dets(2);
  dets[0].range_bin = 30;
  dets[0].doppler_bin = (uint32_t)std::lround(0.1 * sc.nof_slow) + sc.nof_slow / 2;
  dets[0].snr_db      = 20.0f; // above the gate
  dets[1]             = dets[0];
  dets[1].snr_db      = 5.0f; // below it
  std::vector<aoa_estimate_t> out;
  est.process(h.data(), sc.nof_ant, sc.nof_slow, sc.nof_slow, sc.nof_subc, occ.data(), row_t.data(), 1.0, carrier_of(sc), dets, out);
  EXPECT_TRUE(out[0].valid);
  EXPECT_FALSE(out[1].valid);
}

/// The reported sigma is a CRB, so it must behave like one: shrink with SNR, and blow up towards
/// endfire where the array's phase stops varying with bearing.
TEST(isac_aoa, sigma_shrinks_with_snr_and_grows_towards_endfire)
{
  scene_t      sc;
  const double lambda = C_LIGHT / sc.fc_hz;
  aoa_array_t  arr;
  ASSERT_TRUE(parse_rx_array(ula_spec(4, lambda), 0.0, sc.fc_hz, arr));

  auto sigma_at = [&](double bearing_deg, float snr_db) {
    std::vector<icf_t>   h;
    std::vector<uint8_t> occ;
    std::vector<double>  row_t;
    synth(sc, arr, {{30.0, 0.1, bearing_deg * M_PI / 180.0, 1.0}}, 0.0, h, occ, row_t);
    aoa_estimator                    est(base_args(), arr);
    std::vector<sensing_detection_t> dets(1);
    dets[0].range_bin   = 30;
    dets[0].doppler_bin = (uint32_t)std::lround(0.1 * sc.nof_slow) + sc.nof_slow / 2;
    dets[0].snr_db      = snr_db;
    std::vector<aoa_estimate_t> out;
    est.process(h.data(), sc.nof_ant, sc.nof_slow, sc.nof_slow, sc.nof_subc, occ.data(), row_t.data(), 1.0, carrier_of(sc), dets,
                out);
    EXPECT_TRUE(out[0].valid);
    return (double)out[0].azimuth_std_deg;
  };

  EXPECT_LT(sigma_at(90.0, 30.0f), sigma_at(90.0, 10.0f)) << "sigma must fall as SNR rises";
  EXPECT_GT(sigma_at(25.0, 20.0f), sigma_at(90.0, 20.0f)) << "sigma must grow towards endfire";
}

/// Two targets at the same bearing but different range/Doppler cells must each get their own bearing
/// from their own cell — i.e. the per-cell evaluation really is per cell.
TEST(isac_aoa, separate_cells_get_their_own_bearings)
{
  scene_t      sc;
  const double lambda = C_LIGHT / sc.fc_hz;
  aoa_array_t  arr;
  ASSERT_TRUE(parse_rx_array(ula_spec(4, lambda), 0.0, sc.fc_hz, arr));

  const double b1 = 60.0, b2 = 125.0;
  std::vector<icf_t>   h;
  std::vector<uint8_t> occ;
  std::vector<double>  row_t;
  synth(sc, arr,
        {{25.0, 0.15, b1 * M_PI / 180.0, 1.0}, {90.0, -0.25, b2 * M_PI / 180.0, 1.0}}, 0.0, h, occ, row_t);

  aoa_estimator                    est(base_args(), arr);
  std::vector<sensing_detection_t> dets(2);
  dets[0].range_bin   = 25;
  dets[0].doppler_bin = (uint32_t)std::lround(0.15 * sc.nof_slow) + sc.nof_slow / 2;
  dets[0].snr_db      = 20.0f;
  dets[1].range_bin   = 90;
  dets[1].doppler_bin = (uint32_t)std::lround(-0.25 * sc.nof_slow) + sc.nof_slow / 2;
  dets[1].snr_db      = 20.0f;
  std::vector<aoa_estimate_t> out;
  est.process(h.data(), sc.nof_ant, sc.nof_slow, sc.nof_slow, sc.nof_subc, occ.data(), row_t.data(), 1.0, carrier_of(sc), dets, out);

  ASSERT_TRUE(out[0].valid);
  ASSERT_TRUE(out[1].valid);
  EXPECT_NEAR(wrap_deg((double)out[0].azimuth_deg - b1), 0.0, 3.0);
  EXPECT_NEAR(wrap_deg((double)out[1].azimuth_deg - b2), 0.0, 3.0);
}

/// Regression for the ONLY gross bearing errors the 2026-07-26 live rx1 run produced: 2 of 417
/// detections reported ~180 deg, i.e. the ILLUMINATOR's bearing rather than their target's. The
/// +/-`aoa_cell_search_bins` re-peak had wandered into the direct-path guard region and found a
/// leakage cell stronger than the target's own. Those cells are exactly the ones CFAR is forbidden to
/// detect in, so the re-peak must not prefer one either.
///
/// Constructed to fail loudly without the guard: the interferer sits 2 Doppler bins away (inside
/// zero_doppler_guard, hence reachable by the re-peak but not by CFAR) at 8x the amplitude, so
/// 64x the power — an unguarded search picks it every time.
TEST(isac_aoa, re_peak_never_wanders_into_the_direct_path_guard)
{
  scene_t      sc;
  const double lambda = C_LIGHT / sc.fc_hz;
  aoa_array_t  arr;
  ASSERT_TRUE(parse_rx_array(ula_spec(4, lambda), 0.0, sc.fc_hz, arr));

  nr_isac_args_t a       = base_args();
  a.zero_range_guard     = 2;
  a.zero_doppler_guard   = 3;
  a.aoa_cell_search_bins = 2;

  const int    half     = (int)(sc.nof_slow / 2);
  const int    tgt_db   = half + 4;  // |4| > zero_doppler_guard -> a legitimate CFAR cell
  const int    leak_db  = half + 2;  // |2| <= zero_doppler_guard -> inside the notch, reachable at dd=-2
  const double tgt_bear = 60.0, leak_bear = 180.0;

  std::vector<icf_t>   h;
  std::vector<uint8_t> occ;
  std::vector<double>  row_t;
  synth(sc, arr,
        {{40.0, (double)(tgt_db - half) / (double)sc.nof_slow, tgt_bear * M_PI / 180.0, 1.0},
         {40.0, (double)(leak_db - half) / (double)sc.nof_slow, leak_bear * M_PI / 180.0, 8.0}},
        0.0, h, occ, row_t);

  std::vector<sensing_detection_t> dets(1);
  dets[0].range_bin   = 40;
  dets[0].doppler_bin = (uint32_t)tgt_db;
  dets[0].snr_db      = 20.0f;

  std::vector<aoa_estimate_t> out;
  aoa_estimator               est(a, arr);
  est.process(h.data(), sc.nof_ant, sc.nof_slow, sc.nof_slow, sc.nof_subc, occ.data(), row_t.data(), 1.0,
              carrier_of(sc), dets, out);

  ASSERT_TRUE(out[0].valid);
  EXPECT_NEAR(wrap_deg((double)out[0].azimuth_deg - tgt_bear), 0.0, 3.0);
  // And specifically NOT the interferer's bearing, which is what the unguarded search returned.
  EXPECT_GT(std::abs(wrap_deg((double)out[0].azimuth_deg - leak_bear)), 30.0);
}

// ---------------------------------------------------------------------------------------------
// Direct-path self-calibration (§5.4 item 3)
// ---------------------------------------------------------------------------------------------

/// The elegant part of the design: uncalibrated per-channel phase offsets bias every bearing, and the
/// LOS tap — whose bearing is known from the surveyed illuminator position — removes them with no
/// injected tone and no anechoic chamber. Asserted as a strict improvement over the uncalibrated run.
TEST(isac_aoa_selfcal, direct_path_calibration_removes_per_channel_phase_error)
{
  scene_t      sc;
  const double lambda = C_LIGHT / sc.fc_hz;
  aoa_array_t  arr;
  ASSERT_TRUE(parse_rx_array(ula_spec(4, lambda), 0.0, sc.fc_hz, arr));

  // Arbitrary, deliberately large per-channel phase errors (cable-length / LO-distribution class).
  const double        errs[4] = {0.0, 0.9, -1.7, 2.4};
  std::vector<icf_t>  channel_err(4);
  for (int i = 0; i < 4; i++) {
    channel_err[i] = icf_t((float)std::cos(errs[i]), (float)std::sin(errs[i]));
  }

  const double target_deg   = 110.0;
  const double los_bearing  = 200.0 * M_PI / 180.0; // illuminator behind/left of the receiver

  std::vector<icf_t>   h;
  std::vector<uint8_t> occ;
  std::vector<double>  row_t;
  // The direct path is at range bin 0 and zero Doppler; the target elsewhere.
  synth(sc, arr,
        {{0.0, 0.0, los_bearing, 4.0}, {40.0, 0.18, target_deg * M_PI / 180.0, 1.0}}, 0.0, h, occ, row_t,
        &channel_err);

  std::vector<sensing_detection_t> dets(1);
  dets[0].range_bin   = 40;
  dets[0].doppler_bin = (uint32_t)std::lround(0.18 * sc.nof_slow) + sc.nof_slow / 2;
  dets[0].snr_db      = 20.0f;

  auto run = [&](bool selfcal) {
    nr_isac_args_t a = base_args();
    a.aoa_selfcal    = selfcal;
    aoa_estimator est(a, arr);
    if (selfcal) {
      EXPECT_TRUE(est.calibrate_from_los(h.data(), sc.nof_ant, sc.nof_slow, sc.nof_slow, sc.nof_subc, occ.data(), row_t.data(), 1.0,
                                         carrier_of(sc), los_bearing));
      EXPECT_EQ(est.calibration_updates(), 1u);
    }
    std::vector<aoa_estimate_t> out;
    est.process(h.data(), sc.nof_ant, sc.nof_slow, sc.nof_slow, sc.nof_subc, occ.data(), row_t.data(), 1.0, carrier_of(sc), dets,
                out);
    EXPECT_TRUE(out[0].valid);
    return std::fabs(wrap_deg((double)out[0].azimuth_deg - target_deg));
  };

  const double err_raw = run(false);
  const double err_cal = run(true);
  EXPECT_GT(err_raw, 5.0) << "the injected channel errors should visibly corrupt the raw bearing";
  EXPECT_LT(err_cal, 2.0) << "self-calibration should recover the true bearing, got " << err_cal << " deg";
}

// =================================================================================================
// Position-anchored harmonic rejection (GHOST_KINEMATIC_CONSISTENCY_HANDOVER.md Phase A)
//
// These exercise the geometry and the filter directly, on hand-built detection lists: the point of
// Phase A is the ANCHOR, and the anchor is decided entirely by (range, bearing) -> position, with no
// dependence on the CFR grid the bearings came from.
// =================================================================================================

namespace {

nr_isac_args_t harm_args()
{
  nr_isac_args_t a;
  a.tx_pos_x                = 0.0f;
  a.tx_pos_y                = 0.0f;
  a.rx_pos_x                = 100.0f;
  a.rx_pos_y                = 0.0f;
  a.harmonic_pos_reject     = true;
  a.harmonic_pos_chi2       = 9.0f; // 3 sigma per axis, matches repos/isac's convention
  a.harmonic_pos_range_tol_m = 8.0f;
  a.harmonic_max_k          = 4;
  a.harmonic_tol            = 0.15f;
  a.harmonic_snr_margin     = 6.0f;
  return a;
}

/// The differential range and ENU bearing of a target at (x, y), for the geometry in harm_args().
void true_meas(double x, double y, double& dr, double& az_deg)
{
  const double rt = std::hypot(x - 0.0, y - 0.0);
  const double rr = std::hypot(x - 100.0, y - 0.0);
  dr              = rt + rr - 100.0;
  az_deg          = std::atan2(y - 0.0, x - 100.0) * 180.0 / M_PI;
}

/// @param az_offset_deg  added to the TRUE bearing before it is reported -- simulates this
///        detection's own independent AoA estimation error, which is the whole point of the chi2
///        gate: two detections of the SAME reflection do not report exactly the same bearing.
/// @param az_std_deg     the reported 1-sigma CRB this detection claims for its own bearing.
sensing_detection_t det_at(double x, double y, double vel, double snr, bool with_az = true,
                           float az_std_deg = 2.0f, double az_offset_deg = 0.0)
{
  double dr = 0.0, az = 0.0;
  true_meas(x, y, dr, az);
  sensing_detection_t d;
  d.range_m         = (float)dr;
  d.vel_mps         = (float)vel;
  d.snr_db          = (float)snr;
  d.azimuth_valid   = with_az;
  d.azimuth_deg     = (float)(az + az_offset_deg);
  d.azimuth_std_deg = with_az ? az_std_deg : 0.0f;
  return d;
}

} // namespace

/// The forward/inverse pair must be exact: localising a self-consistent (range, bearing) has to
/// return the position it came from. Same closed form as isac-core's localize_with_bearing(), and it
/// is pinned here because a UE-side fix and a central-node fix disagreeing would be invisible.
TEST(HarmonicPos, LocalizeInvertsTheGeometryExactly)
{
  for (auto p : {std::pair<double, double>{70.0, 110.0},
                 {-140.0, 60.0},
                 {400.0, -300.0},
                 {250.0, 5.0}}) {
    double dr = 0.0, az = 0.0;
    true_meas(p.first, p.second, dr, az);
    double gx = 0.0, gy = 0.0;
    ASSERT_TRUE(nr_isac::aoa_localize(0.0, 0.0, 100.0, 0.0, dr, az, gx, gy));
    EXPECT_NEAR(gx, p.first, 1e-6);
    EXPECT_NEAR(gy, p.second, 1e-6);
  }
}

/// The core case: a ghost at the SAME position (same range, same bearing -- the measured signature of
/// a harmonic) with 2x the velocity is dropped, and its fundamental survives.
TEST(HarmonicPos, DropsASecondHarmonicAtTheSamePosition)
{
  std::vector<sensing_detection_t> dets = {
      det_at(70.0, 110.0, 6.0, 18.0),   // fundamental
      det_at(70.0, 110.0, 12.0, 17.0),  // its 2nd harmonic
  };
  EXPECT_EQ(nr_isac::harmonic_pos_reject(harm_args(), dets), 1u);
  ASSERT_EQ(dets.size(), 1u);
  EXPECT_NEAR(dets[0].vel_mps, 6.0f, 1e-6) << "the fundamental, not the harmonic, must be kept";
}

/// The reason Phase A exists at all. These two targets share a RANGE BIN -- so the existing
/// range-anchored harmonic_reject would pair them -- but sit at opposite bearings, i.e. at completely
/// different places in the world. A position anchor must spare them.
TEST(HarmonicPos, SparesTwoTargetsThatShareARangeButNotABearing)
{
  // Mirror image across the baseline: identical bistatic range, opposite bearing.
  auto a = det_at(70.0, 110.0, 6.0, 18.0);
  auto b = det_at(70.0, -110.0, 12.0, 17.0);
  ASSERT_NEAR(a.range_m, b.range_m, 1e-3) << "the test geometry must actually share a range";
  std::vector<sensing_detection_t> dets = {a, b};
  EXPECT_EQ(nr_isac::harmonic_pos_reject(harm_args(), dets), 0u);
  EXPECT_EQ(dets.size(), 2u);
}

/// Without bearings there are no positions, so the filter must be exactly inert -- the convention
/// every gate in this project follows, asserted rather than assumed.
TEST(HarmonicPos, IsInertWithoutBearings)
{
  std::vector<sensing_detection_t> dets = {
      det_at(70.0, 110.0, 6.0, 18.0, false),
      det_at(70.0, 110.0, 12.0, 17.0, false),
  };
  EXPECT_EQ(nr_isac::harmonic_pos_reject(harm_args(), dets), 0u);
  EXPECT_EQ(dets.size(), 2u);

  // ...and equally inert when the feature itself is off.
  nr_isac_args_t off  = harm_args();
  off.harmonic_pos_reject = false;
  std::vector<sensing_detection_t> dets2 = {
      det_at(70.0, 110.0, 6.0, 18.0),
      det_at(70.0, 110.0, 12.0, 17.0),
  };
  EXPECT_EQ(nr_isac::harmonic_pos_reject(off, dets2), 0u);
  EXPECT_EQ(dets2.size(), 2u);
}

/// A mixed CPI: only the detections that carry a bearing take part. A bearing-less detection at the
/// harmonic velocity must survive (it cannot be positioned, so nothing may be concluded about it),
/// which is what keeps this filter safe on a weak cell whose AoA failed its quality gate.
TEST(HarmonicPos, LeavesBearinglessDetectionsAlone)
{
  std::vector<sensing_detection_t> dets = {
      det_at(70.0, 110.0, 6.0, 18.0),          // fundamental, positioned
      det_at(70.0, 110.0, 12.0, 17.0, false),  // harmonic velocity, but no bearing
      det_at(70.0, 110.0, 18.0, 17.0),         // 3rd harmonic, positioned
  };
  EXPECT_EQ(nr_isac::harmonic_pos_reject(harm_args(), dets), 1u);
  ASSERT_EQ(dets.size(), 2u);
  EXPECT_NEAR(dets[1].vel_mps, 12.0f, 1e-6);
}

/// A non-integer velocity ratio is not a harmonic, and a fundamental far weaker than its supposed
/// harmonic may not veto it (the snr_margin guard, carried over from the range-anchored version).
TEST(HarmonicPos, RespectsTheIntegerRatioAndSnrMargin)
{
  std::vector<sensing_detection_t> ratio = {
      det_at(70.0, 110.0, 6.0, 18.0),
      det_at(70.0, 110.0, 15.0, 17.0), // 2.5x -- not an integer
  };
  EXPECT_EQ(nr_isac::harmonic_pos_reject(harm_args(), ratio), 0u);

  std::vector<sensing_detection_t> weak = {
      det_at(70.0, 110.0, 6.0, 2.0),   // 16 dB weaker than the "harmonic"
      det_at(70.0, 110.0, 12.0, 18.0),
  };
  EXPECT_EQ(nr_isac::harmonic_pos_reject(harm_args(), weak), 0u);
}

// ---------------------------------------------------------------------------------------------
// The chi2 rewrite (2026-07-30, GHOST_KINEMATIC_CONSISTENCY_HANDOVER.md 7.3). A flat metre gate
// on the raw fix-to-fix distance was measured to LOSE to the existing range anchor, because a real
// harmonic pair's two fixes are dominated by INDEPENDENT bearing noise (~1.9 deg CRB on the harness
// that measured it) -- a gate tight enough to reject unrelated targets was also too tight to hold
// real pairs together. These two tests are the reason the rewrite exists: they use REALISTIC,
// independently-perturbed bearings (not the noiseless det_at(...) calls above) and pin the direction
// of the fix -- a 15 m EUCLIDEAN gate would have gotten the first one wrong.
// ---------------------------------------------------------------------------------------------

/// Candidate and fundamental are the SAME true reflection, each reporting its OWN independent
/// bearing estimate (3 deg apart, both at a realistic ~2 deg CRB). Two independent estimates of one
/// point disagreeing by a couple of degrees is what real noise looks like, not evidence of two
/// objects. Hand-verified: the resulting fixes are ~16 m apart in raw Euclidean distance (a flat
/// 15 m gate would have SPARED this real harmonic), but the chi2-normalised tangential residual is
/// ~1.1 sigma^2 and the radial residual ~3 m -- both comfortably inside the default gate.
TEST(HarmonicPos, DropsAHarmonicDespiteRealisticIndependentBearingNoise)
{
  auto fundamental = det_at(70.0, 300.0, 6.0, 18.0, true, 2.0f, 0.0);
  auto candidate   = det_at(70.0, 300.0, 12.0, 17.0, true, 2.0f, 3.0); // 3 deg independent offset
  std::vector<sensing_detection_t> dets = {fundamental, candidate};
  EXPECT_EQ(nr_isac::harmonic_pos_reject(harm_args(), dets), 1u);
  ASSERT_EQ(dets.size(), 1u);
  EXPECT_NEAR(dets[0].vel_mps, 6.0f, 1e-6) << "the fundamental, not the harmonic, must be kept";
}

/// Same construction, but the bearing disagreement (20 deg) is far beyond anything a ~2 deg CRB
/// explains -- these are two different objects, not one noisy reflection, and must be spared. Chi2
/// alone is not what catches this case (the radial residual at this bearing separation is already
/// ~36 m, well past harmonic_pos_range_tol_m), which is by design: an implausible bearing
/// disagreement drags the localised fix off the true point entirely, and the radial gate is what
/// catches a fix that has moved that far.
TEST(HarmonicPos, SparesADistinctTargetWhoseBearingDisagreesFarBeyondNoise)
{
  auto fundamental = det_at(70.0, 300.0, 6.0, 18.0, true, 2.0f, 0.0);
  auto other       = det_at(70.0, 300.0, 12.0, 17.0, true, 2.0f, 20.0); // 20 deg: not noise
  std::vector<sensing_detection_t> dets = {fundamental, other};
  EXPECT_EQ(nr_isac::harmonic_pos_reject(harm_args(), dets), 0u);
  EXPECT_EQ(dets.size(), 2u);
}

int main(int argc, char **argv)
{
  logInit();
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
