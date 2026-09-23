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
  EXPECT_EQ(nr_passive_acq_snapshot().state, NR_ACQ_SEARCHING);
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
TEST_F(AcqState, EarlyEventsReportProgressBeforeAnyPdcchOccasionRuns) {
  /* Measured on 4 s raw captures: MIB and SIB1 both decode while the blind PDCCH monitor runs
   * ZERO occasions, so a poll-only tracker reported nothing at all. Events must advance it. */
  nr_passive_acq_note_pbch_locked();
  EXPECT_EQ(nr_passive_acq_snapshot().state, NR_ACQ_PBCH_LOCKED);
  nr_passive_acq_note_sib1();
  EXPECT_EQ(nr_passive_acq_snapshot().state, NR_ACQ_SIB1_DECODED);
  EXPECT_EQ(nr_passive_acq_snapshot().transitions, 2u);
  nr_passive_acq_inputs_t in{}; // first poll, monitor still knows nothing: must not regress
  nr_passive_acq_update(&in);
  EXPECT_EQ(nr_passive_acq_snapshot().state, NR_ACQ_SIB1_DECODED);
  EXPECT_EQ(nr_passive_acq_snapshot().consecutive_regressions, 0u);
}
TEST_F(AcqState, SyncLossClearsPbchButKeepsSib1Facts) {
  nr_passive_acq_note_pbch_locked();
  nr_passive_acq_note_sib1();
  nr_passive_acq_note_sync_loss();
  ASSERT_EQ(nr_passive_acq_snapshot().state, NR_ACQ_LOST);
  nr_passive_acq_inputs_t in{};
  nr_passive_acq_update(&in); // cell config survives a gap; the frame mapping does not
  EXPECT_EQ(nr_passive_acq_snapshot().state, NR_ACQ_SIB1_DECODED);
  nr_passive_acq_note_pbch_locked(); // re-lock is a lower rung than SIB1: no state change
  EXPECT_EQ(nr_passive_acq_snapshot().state, NR_ACQ_SIB1_DECODED);
  EXPECT_EQ(nr_passive_acq_snapshot().pbch_locks, 2u);
}
/* The cell every saved capture comes from: 273 PRB mu=1, SSB found at grid subcarrier 150 (the
 * oracle's own --ssb 150), MIB k_SSB=12, SIB1 offsetToPointA=24, offsetToCarrier=0. Point A must
 * land exactly on grid subcarrier 0 and the carrier must end exactly at 12*273. */
TEST_F(AcqState, CarrierVerifiedOnTheKnownCell) {
  nr_passive_acq_carrier_t c{273, 1, 150, 3450e6, 273, 1, 24, 0, 12};
  const auto v = nr_passive_acq_verify_carrier(&c);
  EXPECT_TRUE(v.bw_match); EXPECT_TRUE(v.mu_match); EXPECT_TRUE(v.grid_match);
  EXPECT_EQ(v.point_a_subcarrier, 0);
  EXPECT_EQ(v.carrier_end_subcarrier, 12 * 273);
  EXPECT_NEAR(v.derived_centre_hz, 3450e6, 1.0) << "matched grid: derived centre == started centre";
}
TEST_F(AcqState, CarrierMismatchesAreEachDetectedSeparately) {
  nr_passive_acq_carrier_t wrong_bw{273, 1, 150, 3450e6, 106, 1, 24, 0, 12};
  EXPECT_FALSE(nr_passive_acq_verify_carrier(&wrong_bw).bw_match);
  nr_passive_acq_carrier_t wrong_mu{273, 1, 150, 3450e6, 273, 0, 24, 0, 12};
  EXPECT_FALSE(nr_passive_acq_verify_carrier(&wrong_mu).mu_match);
  nr_passive_acq_carrier_t off_grid{273, 1, 162, 3450e6, 273, 1, 24, 0, 12}; // SSB found one RB higher
  const auto v = nr_passive_acq_verify_carrier(&off_grid);
  EXPECT_EQ(v.point_a_subcarrier, 12);
  EXPECT_FALSE(v.grid_match) << "the started grid is not this carrier";
  EXPECT_TRUE(v.bw_match) << "bandwidth alone is not enough to call it a match";
  // The carrier sits one RB (12 x 30 kHz) above the started grid: that is the retune target.
  EXPECT_NEAR(v.derived_centre_hz, 3450e6 + 12 * 30e3, 1.0);
}
TEST_F(AcqState, CarrierCheckReachesTheSnapshotAndNeedsPhyGeometryFirst) {
  nr_passive_acq_note_sib1_carrier(273, 1, 24, 0, 12); // PHY never registered: must not decide
  EXPECT_EQ(nr_passive_acq_snapshot().carrier_verified, 0);
  nr_passive_acq_set_phy_geometry(273, 1, 150, 3450e6);
  nr_passive_acq_note_sib1_carrier(273, 1, 24, 0, 12);
  EXPECT_EQ(nr_passive_acq_snapshot().carrier_verified, 1);
  nr_passive_acq_note_sib1_carrier(106, 1, 24, 0, 12);
  EXPECT_EQ(nr_passive_acq_snapshot().carrier_verified, -1);
}
TEST_F(AcqState, ResetReturnsToSearchingAndClearsCounters) {
  nr_passive_acq_inputs_t in{}; in.pdcch_length_found = true;
  nr_passive_acq_update(&in);
  nr_passive_acq_reset();
  const auto s = nr_passive_acq_snapshot();
  EXPECT_EQ(s.state, NR_ACQ_SEARCHING);
  EXPECT_EQ(s.updates, 0u);
  EXPECT_EQ(s.transitions, 0u);
}
int main(int argc,char **argv) { logInit(); testing::InitGoogleTest(&argc,argv); return RUN_ALL_TESTS(); }
