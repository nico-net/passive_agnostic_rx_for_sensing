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
#include <set>
#include <gtest/gtest.h>
extern "C" {
#include "nr_rx_branch.h"
#include "nr_passive_harq_tag.h"
#include "nr_passive_harq_tag_gnb_pin.h" /* the C shim: PHY_VARS_gNB is not C++-includable */
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

// P13a: the CONVERSE of the case above, and the one that was accepted. A phys_map entry for a
// branch rx_branches does not name left physical_channel >= 0 on a DISABLED slot, splitting the
// two "is this branch active" predicates the receiver uses. Measured consequence before the fix:
// the sensing layer built TWO engines (physical_channel >= 0) while deriving ONE unsuffixed set of
// output paths for them (n_active == 1), so two receivers appended into one JSONL under one rx_id
// and the second ZeroMQ bind was lost -- silently.
TEST(RxBranchParse, RejectsAPhysicalMappingForAnUnnamedBranch)
{
  nr_rx_branch_set_t set;
  EXPECT_EQ(nr_rx_branch_set_parse(&set, "0", "0:0,1:1", nullptr), -1);
  EXPECT_EQ(nr_rx_branch_set_parse(&set, "0,2", "0:0,1:1,2:2", nullptr), -1);
}

// The invariant the rejection above buys, stated as a test so a future parser change cannot
// weaken it unnoticed: on SUCCESS, physical_channel >= 0 and state != NR_RXB_DISABLED name
// exactly the same set of branches, and n_active counts exactly that many.
TEST(RxBranchParse, ActivePredicatesAreEquivalentOnSuccess)
{
  const char* lists[][2] = {{"0", "0:0"}, {"2", "2:3"}, {"0,2", "0:0,2:2"},
                            {"0,1,2,3", "0:1,1:0,2:2,3:3"}};
  for (const auto& c : lists) {
    nr_rx_branch_set_t set;
    ASSERT_EQ(nr_rx_branch_set_parse(&set, c[0], c[1], nullptr), 0) << c[0];
    int mapped = 0, enabled = 0;
    for (int i = 0; i < NR_RX_BRANCH_MAX; i++) {
      mapped += set.b[i].physical_channel >= 0 ? 1 : 0;
      enabled += set.b[i].state != NR_RXB_DISABLED ? 1 : 0;
      EXPECT_EQ(set.b[i].physical_channel >= 0, set.b[i].state != NR_RXB_DISABLED) << c[0] << " b" << i;
    }
    EXPECT_EQ(mapped, enabled) << c[0];
    EXPECT_EQ(mapped, (int)set.n_active) << c[0];
  }
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
   * two branches decoding DIFFERENT grants concurrently is just as common as the same one. The
   * harq-process loop runs to STRIDE-1 = 31, i.e. the 5-bit DCI field's full range. */
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
   * 2000 + harq_process_nbr -- this is what the P02 replay's byte-identical result rests on.
   * Looped to 31, not 15: the DCI HARQ-process-number field is 5 bits wide when the cell sets
   * harq-ProcessNumberSizeDCI-1-1 (nr_pdcch_blind_monitor.h:263), and the old formula had no
   * modulo, so a stride narrower than 32 would silently diverge from it above hpn 15. */
  for (int h = 0; h < 32; h++)
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
  EXPECT_EQ(highest, 2127u); // 2000 + 3*32 + 31, with NR_RX_BRANCH_MAX == 4
  EXPECT_LT(highest, NR_PDSCH_PASSIVE_HARQ_TAG_BASE + NR_PASSIVE_HARQ_NAMESPACE_SPAN);
  EXPECT_EQ(highest, nr_pdsch_passive_harq_tag(NR_RX_BRANCH_MAX - 1,
                                               NR_PDSCH_PASSIVE_HARQ_BRANCH_STRIDE - 1));
}

/* ---------------------------------------------------------------------------------------------
 * P08a: the passive UPLINK decode's harq_unique_pid.
 *
 * There is no legacy value worth pinning here: the pre-P08a id was ULSCH_id, which the passive path
 * pins at 0 in EVERY decode context, so "preserving" it would be preserving the collision. The
 * property that matters is that the contexts are pairwise distinct, and that the range does not
 * reach any other submitter's.
 *
 * The tests compose base + ULSCH_id exactly as nr_ulsch_decoding.c:133 does
 * (harq_unique_pid = phy_vars_gNB->harq_unique_pid_base + ULSCH_id); the helper deliberately
 * returns only the base, because ULSCH_id is an ARRAY INDEX into gNB->ulsch[]/pusch_vars[] and
 * cannot itself be offset.
 * --------------------------------------------------------------------------------------------- */

static uint32_t ul_tag(int ctx, uint32_t ulsch_id)
{
  return nr_pusch_passive_harq_tag_base(ctx) + ulsch_id;
}

TEST(PassiveUlHarqTag, EveryDecodeContextGetsADistinctTag) {
  /* THE property this fix exists for. Before P08a all six of these were 0. */
  std::set<uint32_t> seen;
  for (int ctx = 0; ctx < (int)NR_PUSCH_PASSIVE_HARQ_MAX_CTX; ctx++)
    for (uint32_t u = 0; u < NR_PUSCH_PASSIVE_ULSCH_PER_CTX; u++)
      EXPECT_TRUE(seen.insert(ul_tag(ctx, u)).second) << "collision at ctx=" << ctx << " ulsch=" << u;
  EXPECT_EQ(seen.size(), NR_PUSCH_PASSIVE_HARQ_MAX_CTX * NR_PUSCH_PASSIVE_ULSCH_PER_CTX);
  EXPECT_EQ(*seen.begin(), NR_PUSCH_PASSIVE_HARQ_TAG_BASE);
}

TEST(PassiveUlHarqTag, NoContextEmitsTheOldCollidingZero) {
  /* The old id was also what nr_dlsch_decoding.c:76 emits for attached harq_pid=0/cw=0, i.e. the
   * collision crossed the direction axis as well as the context axis. */
  for (int ctx = 0; ctx < (int)NR_PUSCH_PASSIVE_HARQ_MAX_CTX; ctx++) {
    for (uint32_t u = 0; u < NR_PUSCH_PASSIVE_ULSCH_PER_CTX; u++) {
      const uint32_t tag = ul_tag(ctx, u);
      EXPECT_NE(tag, 0u) << "ctx=" << ctx;
      EXPECT_GT(tag, 31u) << "ctx=" << ctx << " still inside the attached DL decode range 0..31";
    }
  }
}

TEST(PassiveUlHarqTag, RangeIsDisjointFromEveryOtherSubmitterType) {
  const uint32_t lo = NR_PUSCH_PASSIVE_HARQ_TAG_BASE;
  const uint32_t hi = NR_PUSCH_PASSIVE_HARQ_TAG_BASE + NR_PASSIVE_HARQ_NAMESPACE_SPAN;
  /* 4000 = the UL RE-ENCODE base (nr_pusch_data_aided.h:23), which is what the removed dead
   * PASSIVE_UL_HARQ_TAG_BASE 4000 would have aliased. 2000/3000 = the DL decode/re-encode. */
  for (uint32_t other : {0u, 31u, 1000u, 2000u, 2127u, 3000u, 3255u, 4000u, 4005u}) {
    EXPECT_TRUE(other < lo || other >= hi) << other << " falls inside the passive UL decode range";
  }
  for (int ctx = -3; ctx < 64; ctx++) {
    const uint32_t tag = ul_tag(ctx, NR_PUSCH_PASSIVE_ULSCH_PER_CTX - 1);
    EXPECT_GE(tag, lo) << "ctx=" << ctx;
    EXPECT_LT(tag, hi) << "ctx=" << ctx << " escaped the namespace";
  }
  /* Out-of-range folds onto a live context rather than escaping -- documented behaviour, asserted
   * rather than assumed. */
  EXPECT_EQ(nr_pusch_passive_harq_tag_base(NR_PUSCH_PASSIVE_HARQ_MAX_CTX),
            nr_pusch_passive_harq_tag_base(0));
}

TEST(PassiveUlHarqTag, StrideBoundHoldsForTheConfiguredContextCount) {
  /* The header's static_assert arithmetic, restated so the suite reports it too. */
  const uint32_t highest = NR_PUSCH_PASSIVE_HARQ_TAG_BASE
                           + (NR_PUSCH_PASSIVE_HARQ_MAX_CTX - 1) * NR_PUSCH_PASSIVE_ULSCH_PER_CTX
                           + (NR_PUSCH_PASSIVE_ULSCH_PER_CTX - 1);
  EXPECT_EQ(highest, 5005u); // 5000 + 5*1 + 0, with MAX_CTX == 6 and one ULSCH per context
  EXPECT_LT(highest, NR_PUSCH_PASSIVE_HARQ_TAG_BASE + NR_PASSIVE_HARQ_NAMESPACE_SPAN);
  EXPECT_EQ(highest, ul_tag((int)NR_PUSCH_PASSIVE_HARQ_MAX_CTX - 1,
                            NR_PUSCH_PASSIVE_ULSCH_PER_CTX - 1));
}

TEST(PassiveUlHarqTag, AZeroInitialisedGnbReproducesTheUpstreamTag) {
  /* The fix is upstream-neutral only because a PHY_VARS_gNB is zero-initialised at every
   * allocation site in this tree (calloc / calloc_or_fail / malloc16_clear / malloc+memset). That
   * is the load-bearing property and it lives in the STRUCT, not in the arithmetic above, so pin it
   * on the real type rather than restating the argument in prose: zero-init must leave
   * nr_ulsch_decoding.c:133 computing exactly the pre-P08a `= ULSCH_id`. */
  ASSERT_GT(nr_passive_harq_tag_gnb_size(), 0u) << "the shim did not see the real struct";
  const uint32_t base = nr_passive_harq_tag_gnb_zero_default();
  ASSERT_NE(base, UINT32_MAX) << "allocation failed in the shim; the pin proved nothing";
  EXPECT_EQ(base, 0u);
  for (uint32_t ulsch_id = 0; ulsch_id < 8; ulsch_id++)
    EXPECT_EQ(base + ulsch_id, ulsch_id) << "upstream tag moved at ULSCH_id=" << ulsch_id;
  /* And once a passive context stamps its base, the same expression lands in the 5000 range. */
  EXPECT_EQ(nr_pusch_passive_harq_tag_base(3) + 0u, 5003u);
}

int main(int argc, char **argv)
{
  logInit();
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
