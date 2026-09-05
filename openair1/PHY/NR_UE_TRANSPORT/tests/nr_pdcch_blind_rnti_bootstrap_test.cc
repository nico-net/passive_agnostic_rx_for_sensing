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

/*! \file openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_blind_rnti_bootstrap_test.cc
 * \brief Phase 3 Technique B (roadmap artifact): the C-RNTI bootstrap accessor, tested against
 * the recorder function directly rather than through a live RT capture -- this state machine has
 * no hardware dependency, so it gets a real unit test instead of only a live smoke check.
 */
#include <gtest/gtest.h>

extern "C" {
#include "nr_pdcch_blind_monitor.h"
}

class RntiBootstrapTest : public ::testing::Test {
 protected:
  void SetUp() override { nr_pdcch_blind_rnti_bootstrap_reset_for_test(); }
};

TEST_F(RntiBootstrapTest, NoConfirmedRntiBeforeAnySighting) {
  uint16_t rnti; uint8_t cls; uint32_t age;
  EXPECT_FALSE(nr_pdcch_blind_monitor_confirmed_rnti(/*now_abs_slot=*/1000, &rnti, &cls, &age));
}

TEST_F(RntiBootstrapTest, OneSightingIsNotEnough) {
  // A single accept must not confirm anything -- persistence is the whole point (a noise accept
  // is almost always a one-off, per rnti_persistence_check()'s own established rationale).
  nr_pdcch_blind_rnti_bootstrap_record(0x4601, NR_BLIND_RNTI_CLASS_C, /*abs_slot=*/1000);
  uint16_t rnti; uint8_t cls; uint32_t age;
  EXPECT_FALSE(nr_pdcch_blind_monitor_confirmed_rnti(1000, &rnti, &cls, &age));
}

TEST_F(RntiBootstrapTest, RepeatedSightingsWithinTheWindowConfirm) {
  nr_pdcch_blind_rnti_bootstrap_record(0x4601, NR_BLIND_RNTI_CLASS_C, 1000);
  nr_pdcch_blind_rnti_bootstrap_record(0x4601, NR_BLIND_RNTI_CLASS_C, 1010);
  uint16_t rnti = 0; uint8_t cls = 0; uint32_t age = 0;
  ASSERT_TRUE(nr_pdcch_blind_monitor_confirmed_rnti(1010, &rnti, &cls, &age));
  EXPECT_EQ(rnti, 0x4601);
  EXPECT_EQ(cls, NR_BLIND_RNTI_CLASS_C);
}

TEST_F(RntiBootstrapTest, ADifferentRntiDoesNotAccumulateTowardTheFirstOnes) {
  // Two sightings of a WRONG rnti and one of the real one must not confirm the wrong one --
  // matches this project's own already-proven persistence semantics (a real UE's RNTI recurs;
  // a noise accept does not, and different noise accepts do not recur AS EACH OTHER either).
  nr_pdcch_blind_rnti_bootstrap_record(0x1234, NR_BLIND_RNTI_CLASS_C, 1000);
  nr_pdcch_blind_rnti_bootstrap_record(0x5678, NR_BLIND_RNTI_CLASS_C, 1010);
  uint16_t rnti; uint8_t cls; uint32_t age;
  EXPECT_FALSE(nr_pdcch_blind_monitor_confirmed_rnti(1010, &rnti, &cls, &age));
}

TEST_F(RntiBootstrapTest, SiRntiAndPagingRntiAreNeverAcceptedAsTheDedicatedRnti) {
  // SI-RNTI (0xFFFF) and P-RNTI (0xFFFE) are fixed, spec-known values already handled by Phase 1's
  // CSS0 path -- they carry no information about the DEDICATED C-RNTI this task exists to
  // bootstrap, and accepting one here would feed a nonsense "dedicated RNTI" into Technique C.
  nr_pdcch_blind_rnti_bootstrap_record(0xFFFF, NR_BLIND_RNTI_CLASS_SI, 1000);
  nr_pdcch_blind_rnti_bootstrap_record(0xFFFF, NR_BLIND_RNTI_CLASS_SI, 1010);
  uint16_t rnti; uint8_t cls; uint32_t age;
  EXPECT_FALSE(nr_pdcch_blind_monitor_confirmed_rnti(1010, &rnti, &cls, &age));
}

TEST_F(RntiBootstrapTest, PagingRntiIsNeverAcceptedAsTheDedicatedRnti) {
  // P-RNTI (0xFFFE) is a fixed, spec-known value already handled by Phase 1s
  // CSS0 path -- it carries no information about the DEDICATED C-RNTI this task exists to
  // bootstrap, and accepting one here would feed a nonsense "dedicated RNTI" into Technique C.
  nr_pdcch_blind_rnti_bootstrap_record(0xFFFE, NR_BLIND_RNTI_CLASS_P, 1000);
  nr_pdcch_blind_rnti_bootstrap_record(0xFFFE, NR_BLIND_RNTI_CLASS_P, 1010);
  uint16_t rnti; uint8_t cls; uint32_t age;
  EXPECT_FALSE(nr_pdcch_blind_monitor_confirmed_rnti(1010, &rnti, &cls, &age));
}

TEST_F(RntiBootstrapTest, AStaleConfirmationExpires) {
  nr_pdcch_blind_rnti_bootstrap_record(0x4601, NR_BLIND_RNTI_CLASS_C, 1000);
  nr_pdcch_blind_rnti_bootstrap_record(0x4601, NR_BLIND_RNTI_CLASS_C, 1010);
  uint16_t rnti; uint8_t cls; uint32_t age;
  ASSERT_TRUE(nr_pdcch_blind_monitor_confirmed_rnti(1010, &rnti, &cls, &age));
  // A UE that has gone silent for a long time (moved off this cell, went idle) should not keep
  // anchoring the search to a stale RNTI forever -- 10 seconds of absence at this deployment's
  // ~2000 slots/s is 20000 slots, so asking "now" 20001 slots after the last sighting must
  // report no confirmation at all (not merely a large age).
  EXPECT_FALSE(nr_pdcch_blind_monitor_confirmed_rnti(1010 + 20001, &rnti, &cls, &age));
}

int main(int argc, char** argv)
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
