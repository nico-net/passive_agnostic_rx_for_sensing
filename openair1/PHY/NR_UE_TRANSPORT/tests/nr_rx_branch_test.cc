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

#include <cstring>
#include <gtest/gtest.h>
extern "C" {
#include "nr_rx_branch.h"
#include "common/utils/LOG/log.h"
#include "common/config/config_userapi.h"
}
extern "C" {
configmodule_interface_t *uniqCfg = nullptr;
void exit_function(const char *, const char *, int, const char *, int) { std::abort(); }
}

TEST(RxBranchParse, DefaultOneBranchOnPhysicalZero) {
  nr_rx_branch_set_t set;
  ASSERT_EQ(nr_rx_branch_set_parse(&set, "0", "0:0", nullptr), 0);
  EXPECT_EQ(set.n_active, 1);
  EXPECT_EQ(set.b[0].physical_channel, 0);
  EXPECT_EQ(set.b[0].state, NR_RXB_ACQUIRING);
  EXPECT_STREQ(set.b[0].rx_id, "rx0");
  EXPECT_EQ(set.b[0].acq_epoch, 0u);
  EXPECT_EQ(set.b[0].lock_epoch, 0u);
  for (int i = 1; i < NR_RX_BRANCH_MAX; i++) {
    EXPECT_EQ(set.b[i].state, NR_RXB_DISABLED);
    EXPECT_EQ(set.b[i].physical_channel, -1);
  }
}

TEST(RxBranchParse, FourBranchesIdentityMap) {
  nr_rx_branch_set_t set;
  ASSERT_EQ(nr_rx_branch_set_parse(&set, "0,1,2,3", "0:0,1:1,2:2,3:3", "rx"), 0);
  EXPECT_EQ(set.n_active, 4);
  for (int i = 0; i < 4; i++) {
    EXPECT_EQ(set.b[i].physical_channel, i);
    EXPECT_EQ(set.b[i].state, NR_RXB_ACQUIRING);
    char expected[NR_RX_BRANCH_ID_LEN];
    snprintf(expected, sizeof(expected), "rx%d", i);
    EXPECT_STREQ(set.b[i].rx_id, expected);
  }
}

TEST(RxBranchParse, RejectsDuplicateBranch) {
  nr_rx_branch_set_t set;
  EXPECT_EQ(nr_rx_branch_set_parse(&set, "0,1,0", "0:0,1:1", nullptr), -1);
}

TEST(RxBranchParse, RejectsDuplicatePhysicalChannel) {
  nr_rx_branch_set_t set;
  EXPECT_EQ(nr_rx_branch_set_parse(&set, "0,1", "0:0,1:0", nullptr), -1);
}

TEST(RxBranchParse, RejectsOutOfRangeBranch) {
  nr_rx_branch_set_t set;
  EXPECT_EQ(nr_rx_branch_set_parse(&set, "0,4", "0:0,4:1", nullptr), -1);
}

TEST(RxBranchParse, RejectsOutOfRangePhysical) {
  nr_rx_branch_set_t set;
  EXPECT_EQ(nr_rx_branch_set_parse(&set, "0", "0:4", nullptr), -1);
}

TEST(RxBranchParse, RejectsMissingMappingForActiveBranch) {
  nr_rx_branch_set_t set;
  EXPECT_EQ(nr_rx_branch_set_parse(&set, "0,1", "0:0", nullptr), -1);
}

TEST(RxBranchParse, RejectsEmptyOrNullInputs) {
  nr_rx_branch_set_t set;
  EXPECT_EQ(nr_rx_branch_set_parse(&set, nullptr, "0:0", nullptr), -1);
  EXPECT_EQ(nr_rx_branch_set_parse(&set, "", "0:0", nullptr), -1);
  EXPECT_EQ(nr_rx_branch_set_parse(&set, "0", nullptr, nullptr), -1);
  EXPECT_EQ(nr_rx_branch_set_parse(nullptr, "0", "0:0", nullptr), -1);
}

TEST(RxBranchCheckAntennas, MoreBranchesThanAntennasRejected) {
  nr_rx_branch_set_t set;
  ASSERT_EQ(nr_rx_branch_set_parse(&set, "0,1,2,3", "0:0,1:1,2:2,3:3", nullptr), 0);
  EXPECT_EQ(nr_rx_branch_set_check_antennas(&set, 4), 0);
  EXPECT_EQ(nr_rx_branch_set_check_antennas(&set, 2), -1);
  EXPECT_EQ(nr_rx_branch_set_check_antennas(&set, 0), -1);
}

// ---- G1 test 2 in pure form: permuting the physical-channel map changes only the permuted
// branches' physical_channel; identity (branch_id, rx_id) and epochs are unaffected. ----
TEST(RxBranchPermute, OnlyPermutedBranchesChangePhysicalChannel) {
  nr_rx_branch_set_t before, after;
  ASSERT_EQ(nr_rx_branch_set_parse(&before, "0,1,2,3", "0:0,1:1,2:2,3:3", "rx"), 0);
  // Swap branch 0 and 1's physical channels; leave 2 and 3 untouched.
  ASSERT_EQ(nr_rx_branch_set_parse(&after, "0,1,2,3", "0:1,1:0,2:2,3:3", "rx"), 0);

  EXPECT_NE(before.b[0].physical_channel, after.b[0].physical_channel);
  EXPECT_NE(before.b[1].physical_channel, after.b[1].physical_channel);
  EXPECT_EQ(after.b[0].physical_channel, 1);
  EXPECT_EQ(after.b[1].physical_channel, 0);
  for (int i = 2; i < 4; i++)
    EXPECT_EQ(before.b[i].physical_channel, after.b[i].physical_channel);

  for (int i = 0; i < 4; i++) {
    EXPECT_EQ(before.b[i].branch_id, after.b[i].branch_id);
    EXPECT_STREQ(before.b[i].rx_id, after.b[i].rx_id);
    EXPECT_EQ(before.b[i].acq_epoch, after.b[i].acq_epoch);
    EXPECT_EQ(before.b[i].lock_epoch, after.b[i].lock_epoch);
  }
}

// ---- G1 test 4 in pure form: lose_lock -> lock_epoch+1 and LOST; rf_discontinuity ->
// acq_epoch+1 on all active (DISABLED untouched), lock_epoch untouched; reset never decrements
// an epoch. ----
TEST(RxBranchLifecycle, LoseLockIncrementsLockEpoch) {
  nr_rx_branch_set_t set;
  ASSERT_EQ(nr_rx_branch_set_parse(&set, "0", "0:0", nullptr), 0);
  nr_rx_branch_lock(&set.b[0], 100);
  EXPECT_EQ(set.b[0].state, NR_RXB_LOCKED);
  EXPECT_EQ(set.b[0].lock_absolute_slot, 100u);

  nr_rx_branch_lose_lock(&set.b[0]);
  EXPECT_EQ(set.b[0].state, NR_RXB_LOST);
  EXPECT_EQ(set.b[0].lock_epoch, 1u);
  EXPECT_EQ(set.b[0].acq_epoch, 0u);

  nr_rx_branch_lock(&set.b[0], 200);
  EXPECT_EQ(set.b[0].state, NR_RXB_LOCKED);
  EXPECT_EQ(set.b[0].counters.relocks, 1u);
  EXPECT_EQ(set.b[0].lock_epoch, 1u); // lock() itself never bumps lock_epoch
}

TEST(RxBranchLifecycle, RfDiscontinuityBumpsAcqEpochOnActiveOnlyAndLeavesLockEpoch) {
  nr_rx_branch_set_t set;
  ASSERT_EQ(nr_rx_branch_set_parse(&set, "0,2", "0:0,2:2", nullptr), 0);
  nr_rx_branch_lock(&set.b[0], 10);
  nr_rx_branch_lock(&set.b[2], 20);
  ASSERT_EQ(set.b[0].lock_epoch, 0u);

  nr_rx_branch_set_rf_discontinuity(&set);

  EXPECT_EQ(set.b[0].acq_epoch, 1u);
  EXPECT_EQ(set.b[2].acq_epoch, 1u);
  EXPECT_EQ(set.b[0].state, NR_RXB_LOST);
  EXPECT_EQ(set.b[2].state, NR_RXB_LOST);
  EXPECT_EQ(set.b[0].counters.discontinuities, 1u);
  // lock_epoch untouched by rf_discontinuity (only nr_rx_branch_lose_lock() bumps it)
  EXPECT_EQ(set.b[0].lock_epoch, 0u);
  EXPECT_EQ(set.b[2].lock_epoch, 0u);
  // DISABLED slots (1, 3) are not touched at all.
  EXPECT_EQ(set.b[1].acq_epoch, 0u);
  EXPECT_EQ(set.b[1].state, NR_RXB_DISABLED);
  EXPECT_EQ(set.b[3].acq_epoch, 0u);
  EXPECT_EQ(set.b[3].state, NR_RXB_DISABLED);
}

TEST(RxBranchLifecycle, ResetNeverDecrementsEpochsAndKeepsIdentity) {
  nr_rx_branch_set_t set;
  ASSERT_EQ(nr_rx_branch_set_parse(&set, "1", "1:3", "rx"), 0);
  nr_rx_branch_lock(&set.b[1], 5);
  nr_rx_branch_lose_lock(&set.b[1]);
  nr_rx_branch_set_rf_discontinuity(&set);
  const uint32_t acq_before = set.b[1].acq_epoch;
  const uint32_t lock_before = set.b[1].lock_epoch;
  ASSERT_GT(acq_before, 0u);
  ASSERT_GT(lock_before, 0u);

  nr_rx_branch_reset(&set.b[1]);

  EXPECT_GE(set.b[1].acq_epoch, acq_before);
  EXPECT_GE(set.b[1].lock_epoch, lock_before);
  EXPECT_EQ(set.b[1].acq_epoch, acq_before); // reset touches neither epoch at all
  EXPECT_EQ(set.b[1].lock_epoch, lock_before);
  EXPECT_EQ(set.b[1].branch_id, 1);
  EXPECT_EQ(set.b[1].physical_channel, 3);
  EXPECT_STREQ(set.b[1].rx_id, "rx1");
  EXPECT_EQ(set.b[1].state, NR_RXB_ACQUIRING);
  EXPECT_EQ(set.b[1].counters.discontinuities, 0u);
  EXPECT_EQ(set.b[1].counters.relocks, 0u);
  EXPECT_EQ(set.b[1].lock_absolute_slot, 0u);
}

TEST(RxBranchLifecycle, ResetOfDisabledBranchStaysDisabled) {
  nr_rx_branch_set_t set;
  ASSERT_EQ(nr_rx_branch_set_parse(&set, "0", "0:0", nullptr), 0);
  nr_rx_branch_reset(&set.b[2]);
  EXPECT_EQ(set.b[2].state, NR_RXB_DISABLED);
  EXPECT_EQ(set.b[2].physical_channel, -1);
}

int main(int argc, char **argv)
{
  logInit();
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
