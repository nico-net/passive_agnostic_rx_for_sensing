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
#include <gtest/gtest.h>
extern "C" {
#include "nr_scrambling_id_sweep.h"
}

TEST(ScramblingIdSweep, OrderIsPciThenDmrsIdThenRest) {
  nr_scrambling_id_sweep_t s;
  nr_scrambling_id_sweep_init(&s, 64, 700);
  ASSERT_EQ(s.n, 1024);
  EXPECT_EQ(s.order[0], 64);
  EXPECT_EQ(s.order[1], 700);
  // no duplicates: every id 0..1023 appears exactly once
  bool seen[1024] = {false};
  for (int i = 0; i < s.n; ++i) {
    ASSERT_FALSE(seen[s.order[i]]) << "duplicate at " << i << ": " << s.order[i];
    seen[s.order[i]] = true;
  }
  for (int id = 0; id < 1024; ++id) EXPECT_TRUE(seen[id]) << "missing id " << id;
}

TEST(ScramblingIdSweep, LatchesOnFirstCrcPass) {
  nr_scrambling_id_sweep_t s;
  nr_scrambling_id_sweep_init(&s, 64, 700);
  nr_scrambling_id_sweep_feed(&s, 0);
  nr_scrambling_id_sweep_feed(&s, 0);
  nr_scrambling_id_sweep_feed(&s, 0);
  nr_scrambling_id_sweep_feed(&s, 1);
  EXPECT_EQ(s.latched, s.order[3]);
  EXPECT_EQ(nr_scrambling_id_sweep_current(&s), s.order[3]);
  // stays there: further feeds are no-ops
  nr_scrambling_id_sweep_feed(&s, 0);
  EXPECT_EQ(nr_scrambling_id_sweep_current(&s), s.order[3]);
}

TEST(ScramblingIdSweep, LatchedIdSurvivesFurtherPassesAndFailures) {
  // Review fix round 1, finding 1: the bug was in the CALLER wrappers
  // (nr_pdsch_passive_data_id_current / nr_pusch_passive_data_id_current), which re-derived
  // eligibility from a lifetime "stalled" counter that the sweep's own first CRC pass permanently
  // broke, discarding the just-latched correct id and reverting to PCI forever. That fix makes the
  // wrappers ALWAYS return latched (never re-consult eligibility) once nr_scrambling_id_sweep_t
  // itself has latched -- this pins the invariant the fix depends on: once latched, current() is
  // stable under ANY sequence of further feed() calls, pass or fail.
  nr_scrambling_id_sweep_t s;
  nr_scrambling_id_sweep_init(&s, 64, 700);
  nr_scrambling_id_sweep_feed(&s, 0);
  nr_scrambling_id_sweep_feed(&s, 0);
  nr_scrambling_id_sweep_feed(&s, 1); // latches on the first pass
  const int latched_id = s.latched;
  ASSERT_GE(latched_id, 0);
  EXPECT_EQ(nr_scrambling_id_sweep_current(&s), latched_id);
  // Further PASSES must not perturb it (a real deployment keeps decoding successfully forever).
  for (int i = 0; i < 5; ++i) {
    nr_scrambling_id_sweep_feed(&s, 1);
    EXPECT_EQ(s.latched, latched_id);
    EXPECT_EQ(nr_scrambling_id_sweep_current(&s), latched_id);
  }
  // Later FAILURES (an unrelated bad grant, noise, etc.) must not perturb it either.
  for (int i = 0; i < 5; ++i) {
    nr_scrambling_id_sweep_feed(&s, 0);
    EXPECT_EQ(s.latched, latched_id);
    EXPECT_EQ(nr_scrambling_id_sweep_current(&s), latched_id);
  }
}

TEST(ScramblingIdSweep, DmrsIdAboveDataRangeIsSkipped) {
  nr_scrambling_id_sweep_t s;
  nr_scrambling_id_sweep_init(&s, 64, 40000); // 40000 is outside the 0..1023 data-id space
  ASSERT_EQ(s.n, 1024);
  EXPECT_EQ(s.order[0], 64);
  EXPECT_EQ(s.order[1], 0);
}

TEST(ScramblingIdSweep, NoDmrsDecisionYet) {
  nr_scrambling_id_sweep_t s;
  nr_scrambling_id_sweep_init(&s, 64, -1);
  ASSERT_EQ(s.n, 1024);
  EXPECT_EQ(s.order[0], 64);
  EXPECT_EQ(s.order[1], 0);
}

int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
