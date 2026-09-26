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

// Standalone unit test for nr_arm_sweep.h -- the generic pick/latch core shared by the VRB-L
// (2-arm) and PRG (3-arm) per-RNTI sweeps in nr_pdsch_passive_decode.c. No PHY/UE dependencies.

#include <gtest/gtest.h>

extern "C" {
#include "nr_arm_sweep.h"
}

// Drive `n_arms` arms for `rounds` rounds, feeding tb_ok = per-arm success probability `p[arm]`
// deterministically (round-robin over a period-100 pattern, not RNG, so the test is reproducible).
static void run_sweep(nr_arm_sweep_t *s, int n_arms, const int p_pct[], int rounds)
{
  for (int r = 0; r < rounds && s->latched < 0; r++) {
    const int arm = nr_arm_sweep_pick(s, n_arms);
    ASSERT_GE(arm, 0);
    ASSERT_LT(arm, n_arms);
    const bool ok = (r % 100) < p_pct[arm];
    nr_arm_sweep_feed(s, n_arms, arm, ok);
  }
}

TEST(ArmSweep, StartsAtArmZeroUntried)
{
  nr_arm_sweep_t s = {};
  s.latched = -1;
  // All arms virgin: must return the default/wideband arm, never an untried higher index first.
  EXPECT_EQ(nr_arm_sweep_pick(&s, 3), 0);
}

TEST(ArmSweep, ExploresEveryArmAtLeastOnceBeforeLatching)
{
  // Arm 0 always succeeds; if the sweep never tried arms 1/2 it could not legitimately rule them
  // out. Run a handful of rounds (fewer than the min-ok latch floor) and check every arm has a
  // trial recorded.
  nr_arm_sweep_t s = {};
  s.latched = -1;
  for (int r = 0; r < 6; r++) {
    const int arm = nr_arm_sweep_pick(&s, 3);
    nr_arm_sweep_feed(&s, 3, arm, true);
  }
  EXPECT_GT(s.tr[0], 0u);
  EXPECT_GT(s.tr[1], 0u);
  EXPECT_GT(s.tr[2], 0u);
}

TEST(ArmSweep, LatchesOnAClearWinner)
{
  // 2-arm case (VRB-L shape): arm 0 always fails, arm 1 always succeeds.
  nr_arm_sweep_t s = {};
  s.latched = -1;
  const int p[2] = {0, 100};
  run_sweep(&s, 2, p, 200);
  ASSERT_EQ(s.latched, 1);
  // Latched: further feeds/picks are a no-op (guard-cost requirement -- one field read only).
  const uint32_t tr0_before = s.tr[0], tr1_before = s.tr[1];
  EXPECT_EQ(nr_arm_sweep_pick(&s, 2), 1);
  EXPECT_EQ(nr_arm_sweep_feed(&s, 2, 0, true), 1); // returns the latch, does not record a trial
  EXPECT_EQ(s.tr[0], tr0_before);
  EXPECT_EQ(s.tr[1], tr1_before);
}

TEST(ArmSweep, LatchesOnAClearWinnerThreeArms)
{
  // 3-arm case (PRG shape): wideband (0) is the real answer, 2 and 4 both fail.
  nr_arm_sweep_t s = {};
  s.latched = -1;
  const int p[3] = {100, 0, 0};
  run_sweep(&s, 3, p, 300);
  EXPECT_EQ(s.latched, 0);
}

TEST(ArmSweep, NeverLatchesOnNoise)
{
  // All arms genuinely equal (50/50): confidence intervals must never fully separate, so this must
  // run out the clock still unlatched, however long the clock runs.
  nr_arm_sweep_t s = {};
  s.latched = -1;
  const int p[3] = {50, 50, 50};
  run_sweep(&s, 3, p, 5000);
  EXPECT_EQ(s.latched, -1);
}

TEST(ArmSweep, SmallAdvantageDoesNotFalseLatchEarly)
{
  // A small, noisy edge (55% vs 45%) must not be mistaken for a settled winner before enough
  // evidence has accumulated -- i.e. it must not latch within the first NR_ARM_SWEEP_LATCH_MIN_OK
  // trials of the leader.
  nr_arm_sweep_t s = {};
  s.latched = -1;
  for (int r = 0; r < NR_ARM_SWEEP_LATCH_MIN_OK; r++) {
    const int arm = nr_arm_sweep_pick(&s, 2);
    nr_arm_sweep_feed(&s, 2, arm, (r % 100) < 55);
  }
  EXPECT_EQ(s.latched, -1);
}

TEST(ArmSweep, PickReturnsLatchWithoutRecordingFurtherTrials)
{
  nr_arm_sweep_t s = {};
  s.latched = 1;
  s.tr[1] = 42;
  EXPECT_EQ(nr_arm_sweep_pick(&s, 3), 1);
  EXPECT_EQ(nr_arm_sweep_feed(&s, 3, 0, false), 1);
  EXPECT_EQ(s.tr[1], 42u); // unchanged: a latched sweep costs one field read, nothing else
}

// ---- Evidence-triggered (incumbent) variant, final review C1 ----------------------------------

TEST(ArmSweepGated, HealthyIncumbentNeverExplores)
{
  nr_arm_sweep_gated_t g;
  nr_arm_sweep_gated_init(&g);
  for (int r = 0; r < 5000; r++) {
    const int arm = nr_arm_sweep_gated_pick(&g, 3);
    ASSERT_EQ(arm, 0) << "round " << r;
    nr_arm_sweep_gated_feed(&g, 3, arm, (r * 37 % 100) < 60, true); // 60 % CRC, interleaved: a normal link
  }
  EXPECT_FALSE(g.explore);
  EXPECT_EQ(g.s.tr[1] + g.s.tr[2], 0u); // the costly arms were never paid for
}

TEST(ArmSweepGated, DeadLinkNeverTriggers)
{
  // Every decode fails, but nothing else passes either (outage / CFO mis-lock): no trigger.
  nr_arm_sweep_gated_t g;
  nr_arm_sweep_gated_init(&g);
  for (int r = 0; r < 1000; r++)
    nr_arm_sweep_gated_feed(&g, 3, nr_arm_sweep_gated_pick(&g, 3), false, false);
  EXPECT_FALSE(g.explore);
  EXPECT_EQ(nr_arm_sweep_gated_pick(&g, 3), 0);
}

TEST(ArmSweepGated, TriggersAfterExactlyOnePoorHealthyWindow)
{
  nr_arm_sweep_gated_t g;
  nr_arm_sweep_gated_init(&g);
  for (int r = 0; r < NR_ARM_SWEEP_INCUMBENT_MIN_TRIALS - 1; r++)
    nr_arm_sweep_gated_feed(&g, 3, 0, r < 4, true); // 4 passes: <= 25 %
  EXPECT_FALSE(g.explore);
  nr_arm_sweep_gated_feed(&g, 3, 0, false, true);
  EXPECT_TRUE(g.explore);
}

TEST(ArmSweepGated, NonPoorWindowIsDiscarded)
{
  // 9/32 (just above the 25 % line) must not trigger, and must not carry into the next window.
  nr_arm_sweep_gated_t g;
  nr_arm_sweep_gated_init(&g);
  for (int r = 0; r < NR_ARM_SWEEP_INCUMBENT_MIN_TRIALS; r++)
    nr_arm_sweep_gated_feed(&g, 3, 0, r < 9, true);
  EXPECT_FALSE(g.explore);
  EXPECT_EQ(g.win_tr, 0u);
}

TEST(ArmSweepGated, FindsTheBetterArmOnceTriggered)
{
  nr_arm_sweep_gated_t g;
  nr_arm_sweep_gated_init(&g);
  const int p[3] = {5, 5, 80}; // prg=4 is the truth
  int latched = -1;
  for (int r = 0; r < 4000 && latched < 0; r++) {
    const int arm = nr_arm_sweep_gated_pick(&g, 3);
    latched = nr_arm_sweep_gated_feed(&g, 3, arm, (r % 100) < p[arm], true);
  }
  EXPECT_EQ(latched, 2);
}

TEST(ArmSweepGated, TieLatchesTheIncumbent)
{
  // Every arm decodes alike (SISO / wideband precoding): the sweep must still end, on arm 0.
  nr_arm_sweep_gated_t g;
  nr_arm_sweep_gated_init(&g);
  int latched = -1, r = 0;
  for (; r < 20000 && latched < 0; r++) {
    const int arm = nr_arm_sweep_gated_pick(&g, 3);
    latched = nr_arm_sweep_gated_feed(&g, 3, arm, (r * 37 % 100) < 10, true);
  }
  EXPECT_EQ(latched, 0) << "after " << r << " rounds";
  EXPECT_LE(g.explore_tr, 3u * NR_ARM_SWEEP_EXPLORE_MAX_TRIALS); // bounded exploration cost
  EXPECT_EQ(nr_arm_sweep_gated_pick(&g, 3), 0);
}

TEST(ArmSweepGated, TieBreakPrefersArmZero)
{
  nr_arm_sweep_gated_t g;
  nr_arm_sweep_gated_init(&g);
  g.explore = true;
  g.s.tr[0] = 4; g.s.ok[0] = 1;
  g.s.tr[1] = 4; g.s.ok[1] = 1;
  g.s.tr[2] = 4; g.s.ok[2] = 1;
  EXPECT_EQ(nr_arm_sweep_gated_pick(&g, 3), 0);
}

// Dual bandit on ONE TB-CRC stream (the review's missing test): the per-RNTI VRB-L sweep (plain) and
// the PRG sweep (gated) are fed the same outcome. A CRC pass needs BOTH to be right.
static int run_dual(int vrbl_truth, int prg_truth, int *vrbl_latch, bool *prg_explored)
{
  nr_arm_sweep_t v = {};
  v.latched = -1;
  nr_arm_sweep_gated_t g;
  nr_arm_sweep_gated_init(&g);
  int prg_latch = -1;
  for (int r = 0; r < 20000 && (v.latched < 0 || (g.explore && prg_latch < 0)); r++) {
    const int va = nr_arm_sweep_pick(&v, 2), pa = nr_arm_sweep_gated_pick(&g, 3);
    const bool ok = va == vrbl_truth && pa == prg_truth && (r % 10) < 9;
    nr_arm_sweep_feed(&v, 2, va, ok);
    prg_latch = nr_arm_sweep_gated_feed(&g, 3, pa, ok, true);
  }
  *vrbl_latch = v.latched;
  *prg_explored = g.explore;
  return g.explore ? prg_latch : 0;
}

TEST(ArmSweepGated, DualBanditVrblWrongDoesNotTriggerPrg)
{
  int vl;
  bool ex;
  run_dual(/*vrbl L4*/ 1, /*prg wideband*/ 0, &vl, &ex);
  EXPECT_EQ(vl, 1);
  EXPECT_FALSE(ex); // VRB-L's early failures stay under the PRG trigger window's poor line
}

TEST(ArmSweepGated, DualBanditBothConvergeWhenPrgIsWrong)
{
  int vl;
  bool ex;
  const int pl = run_dual(/*vrbl L2*/ 0, /*prg 4*/ 2, &vl, &ex);
  EXPECT_TRUE(ex);
  EXPECT_EQ(pl, 2);
  EXPECT_EQ(vl, 0);
}

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
