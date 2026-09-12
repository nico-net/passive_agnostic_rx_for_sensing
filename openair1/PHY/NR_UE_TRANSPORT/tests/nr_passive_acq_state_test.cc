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
#include "nr_passive_acq_state.h"
#include "common/utils/LOG/log.h"
#include "common/config/config_userapi.h"
configmodule_interface_t *uniqCfg = nullptr;
void exit_function(const char *,const char *,int,const char *,int) { std::abort(); }
}
class AcqState : public ::testing::Test { void SetUp() override { nr_passive_acq_reset(); } };

TEST_F(AcqState, StartsSearchingAndProgressesForwardImmediately) {
  EXPECT_EQ(nr_passive_acq_snapshot().state, NR_ACQ_SEARCHING_PDCCH);
  nr_passive_acq_inputs_t in{};
  in.pdcch_length_found = true;
  nr_passive_acq_update(&in);
  EXPECT_EQ(nr_passive_acq_snapshot().state, NR_ACQ_PDCCH_LOCKED);
  in.coreset_extent_verified = true;
  nr_passive_acq_update(&in);
  EXPECT_EQ(nr_passive_acq_snapshot().state, NR_ACQ_CORESET_VERIFIED);
  in.ul_bwp_known = true;
  nr_passive_acq_update(&in);
  EXPECT_EQ(nr_passive_acq_snapshot().state, NR_ACQ_CELL_CONFIGURED);
  in.ul_width_winners = 1;
  nr_passive_acq_update(&in);
  EXPECT_EQ(nr_passive_acq_snapshot().state, NR_ACQ_UL_CONVERGED);
  in.dl_search_winners = 1;
  nr_passive_acq_update(&in);
  EXPECT_EQ(nr_passive_acq_snapshot().state, NR_ACQ_TRACKING);
  EXPECT_EQ(nr_passive_acq_snapshot().transitions, 5u);
}
TEST_F(AcqState, SingleBlipDoesNotDropOutOfTracking) {
  nr_passive_acq_inputs_t in{};
  in.pdcch_length_found = in.coreset_extent_verified = in.ul_bwp_known = true;
  in.dl_search_winners = in.ul_width_winners = 1;
  nr_passive_acq_update(&in);
  ASSERT_EQ(nr_passive_acq_snapshot().state, NR_ACQ_TRACKING);
  nr_passive_acq_inputs_t blip{}; // one bad update: everything reads as unresolved
  nr_passive_acq_update(&blip);
  EXPECT_EQ(nr_passive_acq_snapshot().state, NR_ACQ_TRACKING) << "one blip must not be loss";
  EXPECT_EQ(nr_passive_acq_snapshot().consecutive_regressions, 1u);
}
TEST_F(AcqState, SustainedRegressionDeclaresLostAfterHysteresis) {
  nr_passive_acq_inputs_t in{};
  in.pdcch_length_found = in.coreset_extent_verified = in.ul_bwp_known = true;
  in.dl_search_winners = in.ul_width_winners = 1;
  nr_passive_acq_update(&in);
  ASSERT_EQ(nr_passive_acq_snapshot().state, NR_ACQ_TRACKING);
  nr_passive_acq_inputs_t blip{};
  for (int i = 0; i < NR_PASSIVE_ACQ_LOSS_HYSTERESIS - 1; ++i) {
    nr_passive_acq_update(&blip);
    ASSERT_EQ(nr_passive_acq_snapshot().state, NR_ACQ_TRACKING) << "iteration " << i;
  }
  nr_passive_acq_update(&blip); // the Nth consecutive regressed update
  EXPECT_EQ(nr_passive_acq_snapshot().state, NR_ACQ_LOST);
}
TEST_F(AcqState, ReacquisitionFromLostIsImmediateNoHysteresis) {
  nr_passive_acq_inputs_t in{};
  in.pdcch_length_found = in.coreset_extent_verified = in.ul_bwp_known = true;
  in.dl_search_winners = in.ul_width_winners = 1;
  nr_passive_acq_update(&in);
  nr_passive_acq_inputs_t blip{};
  for (int i = 0; i < NR_PASSIVE_ACQ_LOSS_HYSTERESIS; ++i) nr_passive_acq_update(&blip);
  ASSERT_EQ(nr_passive_acq_snapshot().state, NR_ACQ_LOST);
  nr_passive_acq_inputs_t partial{};
  partial.pdcch_length_found = true;
  nr_passive_acq_update(&partial); // ONE update, evidence supports PDCCH_LOCKED, no debounce needed
  EXPECT_EQ(nr_passive_acq_snapshot().state, NR_ACQ_PDCCH_LOCKED);
}
TEST_F(AcqState, DlAndUlConvergedAreEqualRankSiblingsNotARegression) {
  nr_passive_acq_inputs_t in{};
  in.pdcch_length_found = in.coreset_extent_verified = in.ul_bwp_known = true;
  in.ul_width_winners = 1;
  nr_passive_acq_update(&in);
  ASSERT_EQ(nr_passive_acq_snapshot().state, NR_ACQ_UL_CONVERGED);
  const uint64_t regressions_before = nr_passive_acq_snapshot().consecutive_regressions;
  in.ul_width_winners = 0;
  in.dl_search_winners = 1; // UL evidence gone, DL evidence appeared: lateral, not a regression
  nr_passive_acq_update(&in);
  EXPECT_EQ(nr_passive_acq_snapshot().state, NR_ACQ_DL_CONVERGED);
  EXPECT_EQ(nr_passive_acq_snapshot().consecutive_regressions, regressions_before);
}
TEST_F(AcqState, SyncLossDropsToLostImmediatelyDespiteLatchedEvidence) {
  nr_passive_acq_inputs_t in{};
  in.pdcch_length_found = in.coreset_extent_verified = in.ul_bwp_known = true;
  in.dl_search_winners = in.ul_width_winners = 1;
  nr_passive_acq_update(&in);
  ASSERT_EQ(nr_passive_acq_snapshot().state, NR_ACQ_TRACKING);
  /* This is the case a 3 s gap injected into a saved raw capture actually produced: the receiver
   * lost the stream and reacquired, but every FSM input stayed latched, so the evidence path alone
   * left the state in TRACKING throughout. */
  nr_passive_acq_note_sync_loss();
  EXPECT_EQ(nr_passive_acq_snapshot().state, NR_ACQ_LOST);
  EXPECT_EQ(nr_passive_acq_snapshot().sync_losses, 1u);
  nr_passive_acq_update(&in); // same latched evidence: recovery is immediate, as it should be
  EXPECT_EQ(nr_passive_acq_snapshot().state, NR_ACQ_TRACKING);
}
TEST_F(AcqState, RepeatedSyncLossCountsOnceIntoTheStateButAlwaysIntoTheCounter) {
  nr_passive_acq_note_sync_loss();
  ASSERT_EQ(nr_passive_acq_snapshot().state, NR_ACQ_LOST);
  const uint64_t t = nr_passive_acq_snapshot().transitions;
  nr_passive_acq_note_sync_loss();
  EXPECT_EQ(nr_passive_acq_snapshot().transitions, t) << "already LOST: no second transition";
  EXPECT_EQ(nr_passive_acq_snapshot().sync_losses, 2u);
}
TEST_F(AcqState, ResetReturnsToSearchingAndClearsCounters) {
  nr_passive_acq_inputs_t in{}; in.pdcch_length_found = true;
  nr_passive_acq_update(&in);
  nr_passive_acq_reset();
  const auto s = nr_passive_acq_snapshot();
  EXPECT_EQ(s.state, NR_ACQ_SEARCHING_PDCCH);
  EXPECT_EQ(s.updates, 0u);
  EXPECT_EQ(s.transitions, 0u);
}
int main(int argc,char **argv) { logInit(); testing::InitGoogleTest(&argc,argv); return RUN_ALL_TESTS(); }
