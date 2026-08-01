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

/*! \file openair1/PHY/NR_UE_ISAC/tests/det_quality_test.cc
 * \brief Offline self-test for the adaptive per-detection quality gate (det_quality.{h,cc}).
 *
 * The property that matters is ADAPTATION: the same algorithm, with no reconfiguration, must find the
 * right operating point on populations that sit at completely different absolute SNRs. That is the
 * whole reason it exists instead of a dB threshold, so it is the first thing asserted here.
 */

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include "gtest/gtest.h"

#include "detection_report.h"

extern "C" {
#include "common/config/config_userapi.h"
#include "common/utils/LOG/log.h"
}
extern "C" configmodule_interface_t* uniqCfg = nullptr;
// Same stubs the other NR_UE_ISAC offline tests provide: the module links against OAI's config/log
// libraries, which reference these, but nothing here ever triggers them.
extern "C" void exit_function(const char* file, const char* function, const int line, const char* s,
                              const int a)
{
  (void)file;
  (void)function;
  (void)line;
  (void)s;
  (void)a;
  abort();
}

#include "det_quality.h"

using namespace nr_isac;

namespace {

/// One synthetic CPI: `n_fa` false alarms scattered in range/Doppler at `fa_mean` dB, plus `n_real`
/// targets at `real_mean` dB that RECUR in the same cells CPI after CPI (which is what makes them
/// persistent, exactly as a real target is).
struct scene_gen {
  std::mt19937                     rng{12345};
  std::normal_distribution<double> nfa{0.0, 1.0};
  std::uniform_int_distribution<int> cell{0, 500};

  std::vector<sensing_detection_t> cpi(double fa_mean, double real_mean, int n_fa, int n_real,
                                       int drift)
  {
    std::vector<sensing_detection_t> out;
    for (int i = 0; i < n_fa; i++) {
      sensing_detection_t d;
      d.snr_db      = (float)(fa_mean + nfa(rng) * 1.5);
      d.range_bin   = (uint32_t)cell(rng);
      d.doppler_bin = (uint32_t)cell(rng);
      out.push_back(d);
    }
    for (int t = 0; t < n_real; t++) {
      sensing_detection_t d;
      d.snr_db = (float)(real_mean + nfa(rng) * 1.5);
      // Stable cell, drifting slowly -- a real target reappears where it was, a false alarm does not.
      d.range_bin   = (uint32_t)(100 + 40 * t + drift);
      d.doppler_bin = (uint32_t)(60 + 20 * t);
      out.push_back(d);
    }
    return out;
  }
};

/// Fraction of admitted detections that are real, and fraction of real detections admitted, after a
/// warm-up during which the estimator is still learning.
struct outcome {
  double precision, recall;
};

outcome run(double fa_mean, double real_mean, int n_cpi = 40, float cost_ratio = 1.0f)
{
  det_quality dq(cost_ratio);
  scene_gen   g;
  int         tp = 0, fp = 0, fn = 0;
  const int   n_real = 2, n_fa = 8;
  for (int c = 0; c < n_cpi; c++) {
    auto dets = g.cpi(fa_mean, real_mean, n_fa, n_real, c);
    std::vector<float> p;
    dq.score(dets, 3.05f, 0.06, p);
    if (c < 10) {
      continue; // warm-up: the online estimates are still converging
    }
    for (size_t i = 0; i < dets.size(); i++) {
      const bool is_real = (i >= (size_t)n_fa);
      const bool admit   = p[i] >= dq.boundary();
      if (admit && is_real) tp++;
      else if (admit && !is_real) fp++;
      else if (!admit && is_real) fn++;
    }
  }
  return {(double)tp / std::max(tp + fp, 1), (double)tp / std::max(tp + fn, 1)};
}

} // namespace

/// THE test: identical algorithm, populations 12 dB apart in absolute level, comparable outcome.
/// A fixed dB threshold tuned to either one of these would fail badly on the other -- which is the
/// documented reason this is adaptive (the two real validation captures learn 12.7 and 18.1 dB nulls).
TEST(DetQuality, AdaptsToPopulationsAtDifferentAbsoluteSnr)
{
  const outcome low  = run(/*fa_mean=*/8.0, /*real_mean=*/16.0);
  const outcome high = run(/*fa_mean=*/20.0, /*real_mean=*/28.0);
  for (const auto& o : {low, high}) {
    EXPECT_GT(o.precision, 0.85) << "precision " << o.precision;
    EXPECT_GT(o.recall, 0.70) << "recall " << o.recall;
  }
  EXPECT_NEAR(low.precision, high.precision, 0.15) << "the operating point must not depend on the "
                                                      "absolute level of the population";
}

/// The learned null must track the actual false-alarm population, not a constant.
TEST(DetQuality, LearnsTheNullOfWhateverPopulationItSees)
{
  det_quality dq_lo(1.0f), dq_hi(1.0f);
  scene_gen   g;
  for (int c = 0; c < 30; c++) {
    std::vector<float> p;
    auto a = g.cpi(8.0, 16.0, 8, 2, c);
    dq_lo.score(a, 3.05f, 0.06, p);
    auto b = g.cpi(20.0, 28.0, 8, 2, c);
    dq_hi.score(b, 3.05f, 0.06, p);
  }
  EXPECT_TRUE(dq_lo.warmed_up());
  EXPECT_NEAR(dq_lo.null_median_db(), 8.0, 3.0);
  EXPECT_NEAR(dq_hi.null_median_db(), 20.0, 3.0);
  EXPECT_GT(dq_hi.null_median_db() - dq_lo.null_median_db(), 8.0)
      << "the two nulls must be learned ~12 dB apart, not clamped together";
}

/// Persistence must be doing independent work: two detections at the SAME SNR, one recurring in a
/// stable cell and one not, must not receive the same posterior. This is the feature measured to
/// separate WITHIN every SNR quartile, so if it is inert the gate is just an SNR threshold.
TEST(DetQuality, PersistenceSeparatesAtEqualSnr)
{
  det_quality dq(1.0f);
  std::vector<float> p;
  float stable_p = 0.0f, drifting_p = 0.0f;
  for (int c = 0; c < 25; c++) {
    std::vector<sensing_detection_t> dets;
    // background false alarms, random cells
    scene_gen g;
    g.rng.seed(999 + c);
    for (int i = 0; i < 8; i++) {
      sensing_detection_t d;
      // Real spread, not a constant: a degenerate all-identical population has MAD 0 and would make
      // every z infinite, testing the clamp rather than the persistence feature.
      d.snr_db      = (float)(10.0 + g.nfa(g.rng) * 1.5);
      d.range_bin   = (uint32_t)(g.cell(g.rng));
      d.doppler_bin = (uint32_t)(g.cell(g.rng));
      dets.push_back(d);
    }
    // Two detections at IDENTICAL, MODESTLY elevated SNR. The elevation is deliberately small: at a
    // large SNR gap the posterior saturates at 1.0 for both and the test would pass or fail on
    // nothing. Here SNR alone leaves them ambiguous, so persistence is what must break the tie.
    sensing_detection_t stable;
    stable.snr_db = 13.0f;
    stable.range_bin = 200;
    stable.doppler_bin = 80;
    dets.push_back(stable);
    sensing_detection_t drifting = stable;
    drifting.range_bin = (uint32_t)(300 + 37 * c); // never in the same place twice
    drifting.doppler_bin = (uint32_t)(150 + 29 * c);
    dets.push_back(drifting);

    dq.score(dets, 3.05f, 0.06, p);
    stable_p   = p[8];
    drifting_p = p[9];
  }
  EXPECT_GT(stable_p, drifting_p)
      << "a persistent detection must score above a transient one at the same SNR (stable "
      << stable_p << " vs drifting " << drifting_p << ")";
}

/// The cost ratio must move the operating point monotonically, and in the documented direction:
/// higher cost of a false alarm => admit fewer.
TEST(DetQuality, CostRatioTradesRecallForPrecision)
{
  const outcome lenient = run(10.0, 18.0, 40, 0.33f);
  const outcome strict  = run(10.0, 18.0, 40, 3.0f);
  EXPECT_GE(lenient.recall, strict.recall);
  EXPECT_LE(lenient.precision, strict.precision + 1e-9);
}

/// An empty CPI must not disturb the learned state or crash -- CPIs with no detections are ordinary.
TEST(DetQuality, HandlesEmptyCpis)
{
  det_quality dq(1.0f);
  scene_gen   g;
  std::vector<float> p;
  for (int c = 0; c < 12; c++) {
    auto d = g.cpi(10.0, 18.0, 8, 2, c);
    dq.score(d, 3.05f, 0.06, p);
  }
  const double med = dq.null_median_db();
  std::vector<sensing_detection_t> none;
  dq.score(none, 3.05f, 0.06, p);
  EXPECT_TRUE(p.empty());
  EXPECT_DOUBLE_EQ(dq.null_median_db(), med) << "an empty CPI must not move the learned null";
}


// ---- Wire contract: p_real reaches the central node ------------------------------------------
//
// The gate makes a hard keep/drop call, so on the wire its survivors would otherwise look identical
// -- a 0.55-confidence detection and a 0.99 one would update a track with equal weight, and nothing
// downstream can recover the difference from range/rate/SNR (the null it is measured against is
// per-CPI and moves bodily with gain, traffic and scene). Hence the field; hence this test.

static std::string report_json_with(float p_real)
{
  sensing_rvm_t rvm;
  rvm.range_res_m = 3.05f;
  rvm.vel_res_mps = 1.45f;
  sensing_detection_t d;
  d.range_m = 120.0f;
  d.vel_mps = 6.0f;
  d.snr_db  = 15.0f;
  d.p_real  = p_real;
  std::vector<sensing_detection_t> dets{d};
  detection_report_t rep;
  rep.rx_id      = "rx1";
  rep.rvm        = &rvm;
  rep.detections = &dets;
  return build_detection_report_json(rep);
}

TEST(DetQuality, ReportCarriesPReal)
{
  EXPECT_NE(report_json_with(0.87f).find("\"p_real\":0.87"), std::string::npos)
      << report_json_with(0.87f);
}

/// An unscored detection must OMIT the field, never emit 0. A downstream consumer reads 0 as
/// "certainly a false alarm", which would suppress every track from a receiver that simply does not
/// run the gate -- the same absent-vs-zero distinction the azimuth fields already make.
TEST(DetQuality, ReportOmitsPRealWhenUnscored)
{
  EXPECT_EQ(report_json_with(-1.0f).find("p_real"), std::string::npos);
}

int main(int argc, char** argv)
{
  logInit();
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
