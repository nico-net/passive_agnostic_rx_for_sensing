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

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
