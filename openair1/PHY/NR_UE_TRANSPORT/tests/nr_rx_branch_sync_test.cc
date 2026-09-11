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
#include "nr_rx_branch_sync.h"
#include "common/utils/LOG/log.h"
#include "common/config/config_userapi.h"
}
extern "C" {
configmodule_interface_t *uniqCfg = nullptr;
void exit_function(const char *, const char *, int, const char *, int) { std::abort(); }
}

// ---- reset(): clears correction state but preserves identity/epochs and slot-continuity state.
TEST(RxBranchSyncReset, ClearsCorrectionPreservesEpochsAndContinuity) {
  nr_rx_branch_sync_t s = {};
  s.cfo_hz = 123.4;
  s.cfo_accum_hz = 999.9;
  s.timing_offset_samples = 42;
  s.shift_for_next_frame = -7;
  s.synchronized = 1;
  s.last_absolute_slot = 555;
  s.frame_wraps = 3;
  s.snap_lock_epoch = 2;
  s.snap_acq_epoch = 5;

  nr_rx_branch_sync_reset(&s);

  EXPECT_EQ(s.cfo_hz, 0.0);
  EXPECT_EQ(s.cfo_accum_hz, 0.0);
  EXPECT_EQ(s.timing_offset_samples, 0);
  EXPECT_EQ(s.shift_for_next_frame, 0);
  EXPECT_EQ(s.synchronized, 0);
  // NOT correction state -- untouched.
  EXPECT_EQ(s.last_absolute_slot, 555);
  EXPECT_EQ(s.frame_wraps, 3u);
  EXPECT_EQ(s.snap_lock_epoch, 2u);
  EXPECT_EQ(s.snap_acq_epoch, 5u);
}

TEST(RxBranchSyncReset, NullIsNoOp) {
  nr_rx_branch_sync_reset(nullptr); // must not crash
}

// ---- apply_cfo(): accumulates, digital-only by construction (no device handle reachable).
TEST(RxBranchSyncCfo, AccumulatesAndRecordsLatest) {
  nr_rx_branch_sync_t s = {};
  nr_rx_branch_sync_apply_cfo(&s, 10.0);
  EXPECT_EQ(s.cfo_hz, 10.0);
  EXPECT_EQ(s.cfo_accum_hz, 10.0);
  nr_rx_branch_sync_apply_cfo(&s, -3.5);
  EXPECT_EQ(s.cfo_hz, -3.5);
  EXPECT_EQ(s.cfo_accum_hz, 6.5);
  nr_rx_branch_sync_apply_cfo(nullptr, 1.0); // must not crash
}

// ---- on_slot(): normal forward advance, no wrap.
TEST(RxBranchSyncOnSlot, ForwardAdvanceNoWrap) {
  nr_rx_branch_sync_t s = {};
  EXPECT_EQ(nr_rx_branch_sync_on_slot(&s, 0, 20), 0);
  EXPECT_EQ(s.last_absolute_slot, 0);
  EXPECT_EQ(s.frame_wraps, 0u);
  EXPECT_EQ(nr_rx_branch_sync_on_slot(&s, 19, 20), 0); // still frame 0
  EXPECT_EQ(s.frame_wraps, 0u);
  EXPECT_EQ(nr_rx_branch_sync_on_slot(&s, 20, 20), 0); // frame 1, forward
  EXPECT_EQ(s.last_absolute_slot, 20);
  EXPECT_EQ(s.frame_wraps, 0u);
}

// ---- on_slot(): a small backward step that still crosses a frame-index boundary counts as a
// frame wrap (the class of event G1 test 4 injects), and the state DOES advance.
TEST(RxBranchSyncOnSlot, SmallBackwardFrameIndexDecreaseCountsAsWrap) {
  nr_rx_branch_sync_t s = {};
  ASSERT_EQ(nr_rx_branch_sync_on_slot(&s, 21, 20), 0); // frame 1
  EXPECT_EQ(nr_rx_branch_sync_on_slot(&s, 19, 20), 0); // frame 0: backward by 2, within 1 frame (20)
  EXPECT_EQ(s.last_absolute_slot, 19);
  EXPECT_EQ(s.frame_wraps, 1u);
}

// ---- on_slot(): a small backward step that does NOT cross a frame-index boundary is not a wrap.
TEST(RxBranchSyncOnSlot, SmallBackwardStepWithinSameFrameIsNotAWrap) {
  nr_rx_branch_sync_t s = {};
  ASSERT_EQ(nr_rx_branch_sync_on_slot(&s, 15, 20), 0); // frame 0
  EXPECT_EQ(nr_rx_branch_sync_on_slot(&s, 12, 20), 0); // frame 0 still
  EXPECT_EQ(s.last_absolute_slot, 12);
  EXPECT_EQ(s.frame_wraps, 0u);
}

// ---- on_slot(): backward by MORE than one frame is rejected, -1, and does NOT advance. This is
// the "unsigned slot delta breaks concurrent CPI" out-of-order-job defect class.
TEST(RxBranchSyncOnSlot, LargeBackwardStepRejectedAndStateUnchanged) {
  nr_rx_branch_sync_t s = {};
  ASSERT_EQ(nr_rx_branch_sync_on_slot(&s, 100, 20), 0);
  s.frame_wraps = 7; // sentinel to prove it is untouched on rejection
  EXPECT_EQ(nr_rx_branch_sync_on_slot(&s, 79, 20), -1); // backward by 21 > slots_per_frame=20
  EXPECT_EQ(s.last_absolute_slot, 100); // unchanged
  EXPECT_EQ(s.frame_wraps, 7u);         // unchanged
}

TEST(RxBranchSyncOnSlot, ExactlyOneFrameBackwardIsAcceptedNotRejected) {
  nr_rx_branch_sync_t s = {};
  ASSERT_EQ(nr_rx_branch_sync_on_slot(&s, 100, 20), 0);
  EXPECT_EQ(nr_rx_branch_sync_on_slot(&s, 80, 20), 0); // backward by exactly 20 == slots_per_frame
  EXPECT_EQ(s.last_absolute_slot, 80);
}

TEST(RxBranchSyncOnSlot, RejectsNullOrInvalidSlotsPerFrame) {
  nr_rx_branch_sync_t s = {};
  EXPECT_EQ(nr_rx_branch_sync_on_slot(nullptr, 5, 20), -1);
  EXPECT_EQ(nr_rx_branch_sync_on_slot(&s, 5, 0), -1);
  EXPECT_EQ(nr_rx_branch_sync_on_slot(&s, 5, -1), -1);
}

// ---- is_stale(): true after lose_lock() (lock_epoch bump) and after set_rf_discontinuity()
// (acq_epoch bump); false when neither has moved since the snapshot was taken.
TEST(RxBranchSyncStale, FalseWhenEpochsMatch) {
  nr_rx_branch_set_t set;
  ASSERT_EQ(nr_rx_branch_set_parse(&set, "0", "0:0", nullptr), 0);
  nr_rx_branch_sync_t s = {};
  s.snap_lock_epoch = set.b[0].lock_epoch;
  s.snap_acq_epoch = set.b[0].acq_epoch;
  EXPECT_EQ(nr_rx_branch_sync_is_stale(&s, &set.b[0]), 0);
}

TEST(RxBranchSyncStale, TrueAfterLoseLock) {
  nr_rx_branch_set_t set;
  ASSERT_EQ(nr_rx_branch_set_parse(&set, "0", "0:0", nullptr), 0);
  nr_rx_branch_sync_t s = {};
  s.snap_lock_epoch = set.b[0].lock_epoch;
  s.snap_acq_epoch = set.b[0].acq_epoch;
  nr_rx_branch_lose_lock(&set.b[0]);
  EXPECT_EQ(nr_rx_branch_sync_is_stale(&s, &set.b[0]), 1);
}

TEST(RxBranchSyncStale, TrueAfterRfDiscontinuity) {
  nr_rx_branch_set_t set;
  ASSERT_EQ(nr_rx_branch_set_parse(&set, "0", "0:0", nullptr), 0);
  nr_rx_branch_sync_t s = {};
  s.snap_lock_epoch = set.b[0].lock_epoch;
  s.snap_acq_epoch = set.b[0].acq_epoch;
  nr_rx_branch_set_rf_discontinuity(&set);
  EXPECT_EQ(nr_rx_branch_sync_is_stale(&s, &set.b[0]), 1);
}

TEST(RxBranchSyncStale, FailsSafeOnNull) {
  nr_rx_branch_set_t set;
  ASSERT_EQ(nr_rx_branch_set_parse(&set, "0", "0:0", nullptr), 0);
  nr_rx_branch_sync_t s = {};
  EXPECT_EQ(nr_rx_branch_sync_is_stale(nullptr, &set.b[0]), 1);
  EXPECT_EQ(nr_rx_branch_sync_is_stale(&s, nullptr), 1);
}

// ---- G1 test 4 in pure form: build a job snapshot under epoch E, bump the branch's epoch,
// assert the job is stale and would be discarded while a fresh job (re-snapshotted) is not.
// Mirrors nr_rx_span_pool_test.cc's SpanPoolEpoch.AcqEpochCarriesThroughWithoutOldNewMixing (P04),
// but at P03's epoch level rather than the span pool's -- this is the "no old/new mixing" story
// told with nr_rx_branch_sync_t/nr_rx_branch_t alone, no span pool involved.
TEST(RxBranchSyncG1Test4, StaleJobDiscardedFreshJobKept) {
  nr_rx_branch_set_t set;
  ASSERT_EQ(nr_rx_branch_set_parse(&set, "0", "0:0", nullptr), 0);

  // Job A is created (snapshotted) under the branch's initial epoch pair (0, 0).
  nr_rx_branch_sync_t job_a = {};
  job_a.snap_lock_epoch = set.b[0].lock_epoch;
  job_a.snap_acq_epoch = set.b[0].acq_epoch;
  ASSERT_EQ(nr_rx_branch_sync_is_stale(&job_a, &set.b[0]), 0) << "fresh at creation";

  // The branch loses lock (P03) between job A being created and it being consumed --
  // lock_epoch bumps, job A's snapshot is now behind.
  nr_rx_branch_lose_lock(&set.b[0]);
  EXPECT_EQ(nr_rx_branch_sync_is_stale(&job_a, &set.b[0]), 1)
      << "job A must be discarded: it was snapshotted under the old lock_epoch";

  // A fresh job B, snapshotted AFTER the epoch bump, is current.
  nr_rx_branch_sync_t job_b = {};
  job_b.snap_lock_epoch = set.b[0].lock_epoch;
  job_b.snap_acq_epoch = set.b[0].acq_epoch;
  EXPECT_EQ(nr_rx_branch_sync_is_stale(&job_b, &set.b[0]), 0) << "job B must be kept: current epoch";

  // Same story again for acq_epoch via an RF discontinuity, confirming both epoch halves of the
  // snapshot are independently load-bearing (a job stale on ONLY acq_epoch must also be caught).
  nr_rx_branch_sync_t job_c = {};
  job_c.snap_lock_epoch = set.b[0].lock_epoch;
  job_c.snap_acq_epoch = set.b[0].acq_epoch;
  ASSERT_EQ(nr_rx_branch_sync_is_stale(&job_c, &set.b[0]), 0);
  nr_rx_branch_set_rf_discontinuity(&set);
  EXPECT_EQ(nr_rx_branch_sync_is_stale(&job_c, &set.b[0]), 1)
      << "job C must be discarded: acq_epoch alone moved";
}

int main(int argc, char **argv)
{
  logInit();
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
