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
#include "nr_passive_harq_tag.h"
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

TEST(RxBranchParse, RejectsOverlongRxIdPrefix) {
  nr_rx_branch_set_t set;
  // NR_RX_BRANCH_ID_LEN(16) - 2 = 14 is the longest prefix that still leaves room for the
  // 1-digit branch id and the NUL terminator; 15 must be rejected, not silently truncated into a
  // collision (every branch's rx_id would read the same truncated string).
  EXPECT_EQ(nr_rx_branch_set_parse(&set, "0", "0:0", "012345678901234"), -1);  // 15 chars
  ASSERT_EQ(nr_rx_branch_set_parse(&set, "0,1", "0:0,1:1", "01234567890123"), 0);  // 14 chars: OK
  EXPECT_STREQ(set.b[0].rx_id, "012345678901230");
  EXPECT_STREQ(set.b[1].rx_id, "012345678901231");
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


/* ---- adaptive_RX_pipeline.md P06a: fan-out dispatch + epoch staleness ------------------------ */

TEST(RxBranchDispatch, OneDescriptorPerActiveBranchWithCurrentEpochs) {
  nr_rx_branch_set_t set;
  ASSERT_EQ(nr_rx_branch_set_parse(&set, "0,1,2,3", "0:0,1:1,2:2,3:3", nullptr), 0);
  nr_rx_branch_lose_lock(&set.b[2]);          // b2 lock_epoch -> 1
  nr_rx_branch_set_rf_discontinuity(&set);    // every branch acq_epoch -> 1

  nr_rx_branch_dispatch_t d[NR_RX_BRANCH_MAX];
  ASSERT_EQ(nr_rx_branch_set_dispatch(&set, d, NR_RX_BRANCH_MAX), 4);
  for (int i = 0; i < 4; i++) {
    EXPECT_EQ(d[i].branch_id, i);
    EXPECT_EQ(d[i].physical_channel, i);
    EXPECT_EQ(d[i].acq_epoch, 1u);
    EXPECT_EQ(d[i].lock_epoch, (i == 2) ? 1u : 0u);
  }
}

TEST(RxBranchDispatch, SingleBranchGivesExactlyOneLegacyDescriptor) {
  nr_rx_branch_set_t set;
  ASSERT_EQ(nr_rx_branch_set_parse(&set, "0", "0:0", nullptr), 0);
  nr_rx_branch_dispatch_t d[NR_RX_BRANCH_MAX];
  ASSERT_EQ(nr_rx_branch_set_dispatch(&set, d, NR_RX_BRANCH_MAX), 1);
  EXPECT_EQ(d[0].branch_id, 0);
  EXPECT_EQ(d[0].physical_channel, 0);
  EXPECT_EQ(d[0].lock_epoch, 0u);
  EXPECT_EQ(d[0].acq_epoch, 0u);
}

TEST(RxBranchDispatch, RejectsRatherThanTruncatesWhenCapacityIsTooSmall) {
  nr_rx_branch_set_t set;
  ASSERT_EQ(nr_rx_branch_set_parse(&set, "0,1,2", "0:0,1:1,2:2", nullptr), 0);
  nr_rx_branch_dispatch_t d[NR_RX_BRANCH_MAX];
  EXPECT_EQ(nr_rx_branch_set_dispatch(&set, d, 2), -1); // silently dropping a branch would make
                                                        // per-branch coverage wrong invisibly
  EXPECT_EQ(nr_rx_branch_set_dispatch(nullptr, d, NR_RX_BRANCH_MAX), -1);
  EXPECT_EQ(nr_rx_branch_set_dispatch(&set, nullptr, NR_RX_BRANCH_MAX), -1);
}

TEST(RxBranchDispatch, StaleAfterLoseLockOrDiscontinuityAndFailsSafe) {
  nr_rx_branch_set_t set;
  ASSERT_EQ(nr_rx_branch_set_parse(&set, "0,1", "0:0,1:1", nullptr), 0);
  nr_rx_branch_dispatch_t d[NR_RX_BRANCH_MAX];
  ASSERT_EQ(nr_rx_branch_set_dispatch(&set, d, NR_RX_BRANCH_MAX), 2);
  EXPECT_EQ(nr_rx_branch_dispatch_is_stale(&set, &d[0]), 0);
  EXPECT_EQ(nr_rx_branch_dispatch_is_stale(&set, &d[1]), 0);

  nr_rx_branch_lose_lock(&set.b[1]);
  EXPECT_EQ(nr_rx_branch_dispatch_is_stale(&set, &d[0]), 0); // branch-local, not common-mode
  EXPECT_EQ(nr_rx_branch_dispatch_is_stale(&set, &d[1]), 1);

  nr_rx_branch_set_rf_discontinuity(&set);
  EXPECT_EQ(nr_rx_branch_dispatch_is_stale(&set, &d[0]), 1); // common-mode: now every branch

  nr_rx_branch_dispatch_t unknown;
  std::memset(&unknown, 0, sizeof(unknown));
  unknown.branch_id = 3;
  unknown.physical_channel = 3;
  EXPECT_EQ(nr_rx_branch_dispatch_is_stale(&set, &unknown), 1); // names no active branch
  EXPECT_EQ(nr_rx_branch_dispatch_is_stale(nullptr, &d[0]), 1);
  EXPECT_EQ(nr_rx_branch_dispatch_is_stale(&set, nullptr), 1);
}

/* ---- P09: per-branch harq_unique_pid namespacing (adaptive_RX_pipeline.md Stage 2) -------------
 * Lives in this target rather than its own because the identity being folded into the id IS the
 * branch identity this file already tests, and nr_passive_harq_tag.h is header-only. The property
 * under test is the one a hardware LDPC accelerator needs: two transport blocks that can be in
 * flight at the same moment never carry the same id. */

TEST(PassiveHarqTag, SameProcessDifferentBranchesDoNotAlias) {
  /* The exact P06a fan-out case: one occasion, one grant, N branches. Before P09 every one of
   * these was 2000 + 7. */
  uint32_t seen[NR_RX_BRANCH_MAX];
  for (int b = 0; b < NR_RX_BRANCH_MAX; b++) {
    seen[b] = nr_pdsch_passive_harq_tag((uint8_t)b, 7);
    for (int prev = 0; prev < b; prev++)
      EXPECT_NE(seen[b], seen[prev]) << "branch " << b << " aliases branch " << prev;
  }
}

TEST(PassiveHarqTag, AllBranchProcessPairsAreDistinct) {
  /* Stronger than the above: the whole (branch x harq process) product must be injective, since
   * two branches decoding DIFFERENT grants concurrently is just as common as the same one. */
  bool used[NR_PASSIVE_HARQ_NAMESPACE_SPAN] = {false};
  for (int b = 0; b < NR_RX_BRANCH_MAX; b++) {
    for (int h = 0; h < (int)NR_PDSCH_PASSIVE_HARQ_BRANCH_STRIDE; h++) {
      const uint32_t tag = nr_pdsch_passive_harq_tag((uint8_t)b, (uint8_t)h);
      const uint32_t off = tag - NR_PDSCH_PASSIVE_HARQ_TAG_BASE;
      ASSERT_LT(off, NR_PASSIVE_HARQ_NAMESPACE_SPAN);
      EXPECT_FALSE(used[off]) << "collision at branch=" << b << " harq=" << h;
      used[off] = true;
    }
  }
}

TEST(PassiveHarqTag, BranchZeroReproducesTheLegacyTag) {
  /* Regression pin. Legacy / single-branch mode must emit the literal pre-P09 expression,
   * 2000 + harq_process_nbr -- this is what the P02 replay's byte-identical result rests on. */
  for (int h = 0; h < 16; h++)
    EXPECT_EQ(nr_pdsch_passive_harq_tag(0, (uint8_t)h), 2000u + (uint32_t)h);
}

TEST(PassiveHarqTag, OutOfRangeInputsStayInsideThisTypesNamespace) {
  /* The guard, not the happy path. branch_id comes from a producer-filled job and
   * harq_process_nbr from a BLINDLY decoded DCI, so neither is trusted: a malformed value must
   * stay below the next submitter type's base (3000, the passive DL re-encode) rather than
   * aliasing a different submitter's transport block. */
  const uint32_t limit = NR_PDSCH_PASSIVE_HARQ_TAG_BASE + NR_PASSIVE_HARQ_NAMESPACE_SPAN;
  for (int b = 0; b < 256; b++) {
    for (int h = 0; h < 256; h++) {
      const uint32_t tag = nr_pdsch_passive_harq_tag((uint8_t)b, (uint8_t)h);
      EXPECT_GE(tag, NR_PDSCH_PASSIVE_HARQ_TAG_BASE);
      EXPECT_LT(tag, limit) << "branch=" << b << " harq=" << h << " escaped the namespace";
    }
  }
  /* And an out-of-range branch must not be silently mapped onto branch 0's live ids in a way that
   * looks legal: it folds, which is the documented behaviour, so assert the fold rather than
   * pretend it cannot happen. */
  EXPECT_EQ(nr_pdsch_passive_harq_tag(NR_RX_BRANCH_MAX, 3), nr_pdsch_passive_harq_tag(0, 3));
}

TEST(PassiveHarqTag, StrideBoundHoldsForTheConfiguredBranchCount) {
  /* The same arithmetic the header's static_assert makes at compile time, restated at runtime so a
   * future NR_RX_BRANCH_MAX/stride change is reported by the test suite and not only by a build
   * failure somebody might "fix" by widening the constant. */
  const uint32_t highest =
      NR_PDSCH_PASSIVE_HARQ_TAG_BASE
      + (NR_RX_BRANCH_MAX - 1) * NR_PDSCH_PASSIVE_HARQ_BRANCH_STRIDE
      + (NR_PDSCH_PASSIVE_HARQ_BRANCH_STRIDE - 1);
  EXPECT_EQ(highest, 2063u); // 2000 + 3*16 + 15, with NR_RX_BRANCH_MAX == 4
  EXPECT_LT(highest, NR_PDSCH_PASSIVE_HARQ_TAG_BASE + NR_PASSIVE_HARQ_NAMESPACE_SPAN);
  EXPECT_EQ(highest, nr_pdsch_passive_harq_tag(NR_RX_BRANCH_MAX - 1,
                                               NR_PDSCH_PASSIVE_HARQ_BRANCH_STRIDE - 1));
}

int main(int argc, char **argv)
{
  logInit();
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
