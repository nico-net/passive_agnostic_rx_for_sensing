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

/*! \file openair1/SIMULATION/TOOLS/tests/test_sensing_channel.cpp
 * \brief Unit test for the synthetic moving-target sensing channel's tap-synthesis math.
 *
 * Validates, WITHOUT the full radio loop, that sensing_channel_update() places each object's energy
 * at the tap bin implied by its closed-form bistatic differential range, and that the object tap's
 * phase advances between blocks at the closed-form bistatic Doppler. This isolates the channel-model
 * DSP from the OAI protocol stack (the plan's verification step 1), so an end-to-end failure can be
 * localised to "stack" vs "channel math."
 */

#include <cmath>
#include <complex>
#include <cstdlib>
#include <cstring>

#include <gtest/gtest.h>

extern "C" {
#include "common/config/config_userapi.h"
#include "common/utils/LOG/log.h"
#include "openair1/SIMULATION/TOOLS/sim.h"
#include "openair1/SIMULATION/TOOLS/sensing_channel.h"
}

// LOG/CONFIG_LIB stubs needed outside a full softmodem executable (matches the convention in
// common/utils/tests/test_bits.c and openair1/PHY/NR_UE_ISAC/tests/isac_sync_test.cc).
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

namespace {

constexpr double C_LIGHT = 299792458.0;

// Minimal SISO channel_desc_t with ch[] allocated; sensing_channel_make() grows it as needed.
channel_desc_t *make_bare_desc(uint64_t center_freq, double fs, int init_taps)
{
  channel_desc_t *cd = (channel_desc_t *)calloc(1, sizeof(channel_desc_t));
  cd->nb_tx          = 1;
  cd->nb_rx          = 1;
  cd->center_freq    = center_freq;
  cd->sampling_rate  = fs;
  cd->channel_length = init_taps;
  cd->ch             = (struct complexd **)calloc(1, sizeof(struct complexd *));
  cd->ch[0]          = (struct complexd *)calloc(init_taps, sizeof(struct complexd));
  return cd;
}

void free_bare_desc(channel_desc_t *cd)
{
  free(cd->ch[0]);
  free(cd->ch);
  free(cd);
}

// Index of the max-|.| tap in [1, channel_length) (skip bin 0, which holds the LOS).
int peak_object_bin(const channel_desc_t *cd)
{
  int    best     = 1;
  double best_pow = -1.0;
  for (int l = 1; l < (int)cd->channel_length; l++) {
    const double p = cd->ch[0][l].r * cd->ch[0][l].r + cd->ch[0][l].i * cd->ch[0][l].i;
    if (p > best_pow) {
      best_pow = p;
      best     = l;
    }
  }
  return best;
}

// Complex sum of the object taps (everything except the LOS bin 0) — the object's aggregate phasor,
// robust to the fractional-delay kernel spreading energy across a few bins.
std::complex<double> object_phasor(const channel_desc_t *cd)
{
  std::complex<double> acc(0.0, 0.0);
  for (int l = 1; l < (int)cd->channel_length; l++) {
    acc += std::complex<double>(cd->ch[0][l].r, cd->ch[0][l].i);
  }
  return acc;
}

} // namespace

// A static object: its tap bin must match round(dR/c*fs), and re-running gives an identical phasor
// (no drift for a stationary target).
TEST(sensing_channel, static_object_lands_at_expected_bin)
{
  const uint64_t fc = 3414990000ULL;
  const double   fs = 61.44e6;
  channel_desc_t *cd = make_bare_desc(fc, fs, 4);

  // TX at origin, RX 100 m along x; one static object off to the side.
  const double tx_x = 0, tx_y = 0, rx_x = 100, rx_y = 0;
  const double ox = 50, oy = 40;
  char spec[128];
  snprintf(spec, sizeof(spec), "1.0;0,%.1f,%.1f", ox, oy);
  cd->sensing_traj = sensing_channel_make(cd, tx_x, tx_y, rx_x, rx_y, /*los_db=*/0.0, /*los_delay=*/0.0,
                                          /*frac_taps=*/8, /*chanlen=*/128, spec);
  ASSERT_NE(cd->sensing_traj, nullptr);

  sensing_channel_update(cd, /*nbSamples=*/0, /*TS=*/0);

  const double R_tx  = std::hypot(tx_x - ox, tx_y - oy);
  const double R_rx  = std::hypot(ox - rx_x, oy - rx_y);
  const double R_los = std::hypot(tx_x - rx_x, tx_y - rx_y);
  const double dR    = (R_tx + R_rx) - R_los;
  const int    expected_bin = (int)std::lround(dR / C_LIGHT * fs);

  EXPECT_EQ(peak_object_bin(cd), expected_bin);

  // Re-run at a later timestamp: a static object's aggregate phasor must be unchanged (no Doppler).
  const std::complex<double> ph0 = object_phasor(cd);
  sensing_channel_update(cd, 0, (uint64_t)(fs * 0.5)); // +0.5 s
  const std::complex<double> ph1 = object_phasor(cd);
  EXPECT_NEAR(std::arg(ph1 / ph0), 0.0, 1e-6);

  sensing_channel_free(cd->sensing_traj);
  free_bare_desc(cd);
}

// A moving object on a straight line: the object tap's phase must advance between two blocks at the
// closed-form bistatic Doppler f_d = -(1/lambda) dR/dt, i.e. dphase = -2*pi/lambda * (R(t1)-R(t0)).
TEST(sensing_channel, moving_object_phase_advances_at_bistatic_doppler)
{
  const uint64_t fc     = 3414990000ULL;
  const double   fs     = 61.44e6;
  const double   lambda = C_LIGHT / (double)fc;
  channel_desc_t *cd    = make_bare_desc(fc, fs, 4);

  const double tx_x = 0, tx_y = 0, rx_x = 100, rx_y = 0;
  // Object moves from (50,80) to (150,80) over 10 s -> +10 m/s along x.
  const char *spec = "1.0;0,50,80;10,150,80";
  cd->sensing_traj = sensing_channel_make(cd, tx_x, tx_y, rx_x, rx_y, 0.0, 0.0, 8, 200, spec);
  ASSERT_NE(cd->sensing_traj, nullptr);

  auto bistatic_R = [&](double ox, double oy) {
    return std::hypot(tx_x - ox, tx_y - oy) + std::hypot(ox - rx_x, oy - rx_y);
  };
  const double R_los = std::hypot(tx_x - rx_x, tx_y - rx_y);
  // Trajectory position at absolute trajectory time t (waypoints at t=0 and t=10).
  auto pos_at = [&](double t) {
    const double a = t / 10.0;
    return std::pair<double, double>(50 + a * 100.0, 80.0);
  };

  // NOTE: the FIRST update() call anchors the trajectory clock (start_TS := its TS), so trajectory
  // time is measured RELATIVE to the first call. Passing TS=0 first, then TS=fs*dt, samples the
  // trajectory at absolute times 0 and dt.
  const double dt = 1.0;
  sensing_channel_update(cd, 0, /*TS=*/0);
  const std::complex<double> ph0 = object_phasor(cd);
  sensing_channel_update(cd, 0, (uint64_t)(fs * dt));
  const std::complex<double> ph1 = object_phasor(cd);

  const auto   p0 = pos_at(0.0);
  const auto   p1 = pos_at(dt);
  const double R0 = bistatic_R(p0.first, p0.second);
  const double R1 = bistatic_R(p1.first, p1.second);
  const double expected_dphase = -2.0 * M_PI / lambda * (R1 - R0);

  const double measured_dphase = std::arg(ph1 / ph0);
  // Compare modulo 2*pi (phase can wrap over a 1 s step at these Dopplers).
  double diff = measured_dphase - expected_dphase;
  diff -= 2.0 * M_PI * std::round(diff / (2.0 * M_PI));
  EXPECT_NEAR(diff, 0.0, 0.05) << "measured=" << measured_dphase << " expected=" << expected_dphase;

  // The object bin must track the differential range (range migration): at t=dt the peak sits at
  // round((R1 - R_los)/c*fs). ph1 was computed at that same time, so no extra update is needed.
  const int expected_bin1 = (int)std::lround((R1 - R_los) / C_LIGHT * fs);
  EXPECT_LE(std::abs(peak_object_bin(cd) - expected_bin1), 1) << "expected_bin1=" << expected_bin1;

  sensing_channel_free(cd->sensing_traj);
  free_bare_desc(cd);
}

// frac_delay_taps=0 (nearest-bin) puts ALL object energy in exactly one bin; >0 spreads it across
// several — the property that avoids mid-CPI range-walk snapping.
TEST(sensing_channel, fractional_delay_spreads_energy_across_taps)
{
  const uint64_t fc = 3414990000ULL;
  const double   fs = 61.44e6;

  // Choose geometry giving a clearly non-integer tau so nearest vs sinc differ.
  const double tx_x = 0, tx_y = 0, rx_x = 100, rx_y = 0, ox = 37, oy = 33;
  char spec[128];
  snprintf(spec, sizeof(spec), "1.0;0,%.1f,%.1f", ox, oy);

  channel_desc_t *cd_near = make_bare_desc(fc, fs, 4);
  cd_near->sensing_traj   = sensing_channel_make(cd_near, tx_x, tx_y, rx_x, rx_y, 0.0, 0.0, /*frac=*/0, 128, spec);
  sensing_channel_update(cd_near, 0, 0);

  channel_desc_t *cd_sinc = make_bare_desc(fc, fs, 4);
  cd_sinc->sensing_traj   = sensing_channel_make(cd_sinc, tx_x, tx_y, rx_x, rx_y, 0.0, 0.0, /*frac=*/8, 128, spec);
  sensing_channel_update(cd_sinc, 0, 0);

  auto occupied_bins = [](const channel_desc_t *cd) {
    int n = 0;
    for (int l = 1; l < (int)cd->channel_length; l++) {
      if (cd->ch[0][l].r * cd->ch[0][l].r + cd->ch[0][l].i * cd->ch[0][l].i > 1e-12) {
        n++;
      }
    }
    return n;
  };

  EXPECT_EQ(occupied_bins(cd_near), 1);
  EXPECT_GT(occupied_bins(cd_sinc), 1);

  sensing_channel_free(cd_near->sensing_traj);
  free_bare_desc(cd_near);
  sensing_channel_free(cd_sinc->sensing_traj);
  free_bare_desc(cd_sinc);
}

// ---------------------------------------------------------------------------------------------
// RX array steering (PHASE3_AOA_MULTISTATIC_HANDOVER §5.5)
// ---------------------------------------------------------------------------------------------

namespace {

// Multi-antenna descriptor: nb_tx=1, nb_rx=N, ch[] laid out [aarx + aatx*nb_rx].
channel_desc_t *make_array_desc(uint64_t center_freq, double fs, int init_taps, int nb_rx)
{
  channel_desc_t *cd = (channel_desc_t *)calloc(1, sizeof(channel_desc_t));
  cd->nb_tx          = 1;
  cd->nb_rx          = nb_rx;
  cd->center_freq    = center_freq;
  cd->sampling_rate  = fs;
  cd->channel_length = init_taps;
  cd->ch             = (struct complexd **)calloc(nb_rx, sizeof(struct complexd *));
  for (int a = 0; a < nb_rx; a++) {
    cd->ch[a] = (struct complexd *)calloc(init_taps, sizeof(struct complexd));
  }
  return cd;
}

void free_array_desc(channel_desc_t *cd)
{
  for (int a = 0; a < cd->nb_rx; a++) {
    free(cd->ch[a]);
  }
  free(cd->ch);
  free(cd);
}

// Aggregate object phasor (all taps except the LOS bin 0) on one rx antenna.
std::complex<double> object_phasor_ant(const channel_desc_t *cd, int aarx)
{
  std::complex<double> acc(0.0, 0.0);
  for (int l = 1; l < (int)cd->channel_length; l++) {
    acc += std::complex<double>(cd->ch[aarx][l].r, cd->ch[aarx][l].i);
  }
  return acc;
}

} // namespace

// With no rx_array configured, every antenna must see the IDENTICAL CIR — i.e. the array support is
// strictly opt-in and cannot perturb any existing scene.
TEST(sensing_channel_array, unconfigured_array_leaves_antennas_identical)
{
  const uint64_t fc = 3414990000ULL;
  const double   fs = 61.44e6;
  channel_desc_t *cd = make_array_desc(fc, fs, 4, /*nb_rx=*/4);
  cd->sensing_traj = sensing_channel_make(cd, 0, 0, 100, 0, 0.0, 0.0, 8, 128, "1.0;0,50,80");
  ASSERT_NE(cd->sensing_traj, nullptr);
  sensing_channel_update(cd, 0, 0);

  for (int a = 1; a < cd->nb_rx; a++) {
    for (int l = 0; l < (int)cd->channel_length; l++) {
      EXPECT_NEAR(cd->ch[a][l].r, cd->ch[0][l].r, 1e-12) << "ant " << a << " tap " << l;
      EXPECT_NEAR(cd->ch[a][l].i, cd->ch[0][l].i, 1e-12) << "ant " << a << " tap " << l;
    }
  }
  sensing_channel_free(cd->sensing_traj);
  free_array_desc(cd);
}

// The core AoA property: with a lambda/2 two-element array along x, the inter-element phase
// difference of an object's tap must equal 2*pi*d*cos(theta)/lambda, where theta is the object's
// true ENU bearing from the receiver. This is exactly what the UE-side interferometric estimator
// inverts, so getting the sign wrong here would make every simulated AoA mirror-image correct.
TEST(sensing_channel_array, two_element_phase_difference_matches_true_bearing)
{
  const uint64_t fc     = 3414990000ULL;
  const double   fs     = 61.44e6;
  const double   lambda = C_LIGHT / (double)fc;
  const double   d      = lambda / 2.0;

  const double tx_x = 0, tx_y = 0, rx_x = 100, rx_y = 0;
  // Several bearings, including behind the array and near broadside.
  for (const auto &obj : {std::pair<double, double>{50, 80}, {160, 60}, {40, -70}, {100, 150}}) {
    channel_desc_t *cd = make_array_desc(fc, fs, 4, /*nb_rx=*/2);
    char spec[128];
    snprintf(spec, sizeof(spec), "1.0;0,%.1f,%.1f", obj.first, obj.second);
    cd->sensing_traj = sensing_channel_make(cd, tx_x, tx_y, rx_x, rx_y, 0.0, 0.0, 8, 128, spec);
    ASSERT_NE(cd->sensing_traj, nullptr);
    // Elements at x=0 and x=d, array frame aligned to ENU.
    char arr[64];
    snprintf(arr, sizeof(arr), "0,0;%.9f,0", d);
    ASSERT_EQ(sensing_channel_set_rx_array(cd->sensing_traj, arr, 0.0), 2);
    sensing_channel_update(cd, 0, 0);

    const std::complex<double> z0 = object_phasor_ant(cd, 0);
    const std::complex<double> z1 = object_phasor_ant(cd, 1);
    const double measured = std::arg(z1 * std::conj(z0));

    const double theta    = std::atan2(obj.second - rx_y, obj.first - rx_x);
    const double expected = 2.0 * M_PI * d * std::cos(theta) / lambda;
    double diff = measured - expected;
    diff -= 2.0 * M_PI * std::round(diff / (2.0 * M_PI));
    EXPECT_NEAR(diff, 0.0, 1e-3) << "obj=(" << obj.first << "," << obj.second << ") theta="
                                 << theta * 180.0 / M_PI << "deg";
    sensing_channel_free(cd->sensing_traj);
    free_array_desc(cd);
  }
}

// The boresight rotation must be equivalent to rotating the element coordinates by hand: an array
// declared along its own +x with boresight 90 deg is the same physical array as one declared along
// ENU +y with boresight 0.
TEST(sensing_channel_array, boresight_rotation_matches_hand_rotated_elements)
{
  const uint64_t fc     = 3414990000ULL;
  const double   fs     = 61.44e6;
  const double   lambda = C_LIGHT / (double)fc;
  const double   d      = lambda / 2.0;
  const char    *spec   = "1.0;0,50,80";

  char along_x[64], along_y[64];
  snprintf(along_x, sizeof(along_x), "0,0;%.9f,0", d);
  snprintf(along_y, sizeof(along_y), "0,0;0,%.9f", d);

  channel_desc_t *rot = make_array_desc(fc, fs, 4, 2);
  rot->sensing_traj   = sensing_channel_make(rot, 0, 0, 100, 0, 0.0, 0.0, 8, 128, spec);
  ASSERT_EQ(sensing_channel_set_rx_array(rot->sensing_traj, along_x, 90.0), 2);
  sensing_channel_update(rot, 0, 0);

  channel_desc_t *hand = make_array_desc(fc, fs, 4, 2);
  hand->sensing_traj   = sensing_channel_make(hand, 0, 0, 100, 0, 0.0, 0.0, 8, 128, spec);
  ASSERT_EQ(sensing_channel_set_rx_array(hand->sensing_traj, along_y, 0.0), 2);
  sensing_channel_update(hand, 0, 0);

  const double a_rot  = std::arg(object_phasor_ant(rot, 1) * std::conj(object_phasor_ant(rot, 0)));
  const double a_hand = std::arg(object_phasor_ant(hand, 1) * std::conj(object_phasor_ant(hand, 0)));
  EXPECT_NEAR(a_rot, a_hand, 1e-6);

  sensing_channel_free(rot->sensing_traj);
  free_array_desc(rot);
  sensing_channel_free(hand->sensing_traj);
  free_array_desc(hand);
}

// The LOS/direct-path tap must be steered by the ILLUMINATOR's bearing, not left unsteered. This is
// the whole basis of the direct-path array self-calibration: the receiver knows where the gNB is, so
// the LOS tap's inter-element phase is a known reference it can null out.
TEST(sensing_channel_array, los_tap_carries_the_illuminator_bearing)
{
  const uint64_t fc     = 3414990000ULL;
  const double   fs     = 61.44e6;
  const double   lambda = C_LIGHT / (double)fc;
  const double   d      = lambda / 2.0;

  // TX at (0,0), RX at (100,60): the illuminator sits at bearing atan2(-60,-100) from the receiver.
  const double tx_x = 0, tx_y = 0, rx_x = 100, rx_y = 60;
  channel_desc_t *cd = make_array_desc(fc, fs, 8, /*nb_rx=*/2);
  // LOS only (no objects), so tap 0 is unambiguously the direct path.
  cd->sensing_traj = sensing_channel_make(cd, tx_x, tx_y, rx_x, rx_y, 0.0, 0.0, 8, 128, "");
  ASSERT_NE(cd->sensing_traj, nullptr);
  char arr[64];
  snprintf(arr, sizeof(arr), "0,0;%.9f,0", d);
  ASSERT_EQ(sensing_channel_set_rx_array(cd->sensing_traj, arr, 0.0), 2);
  sensing_channel_update(cd, 0, 0);

  const std::complex<double> z0(cd->ch[0][0].r, cd->ch[0][0].i);
  const std::complex<double> z1(cd->ch[1][0].r, cd->ch[1][0].i);
  const double measured = std::arg(z1 * std::conj(z0));
  const double theta    = std::atan2(tx_y - rx_y, tx_x - rx_x);
  const double expected = 2.0 * M_PI * d * std::cos(theta) / lambda;
  double diff = measured - expected;
  diff -= 2.0 * M_PI * std::round(diff / (2.0 * M_PI));
  EXPECT_NEAR(diff, 0.0, 1e-6);

  sensing_channel_free(cd->sensing_traj);
  free_array_desc(cd);
}

int main(int argc, char **argv)
{
  logInit();
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
