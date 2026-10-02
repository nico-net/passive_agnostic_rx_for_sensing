/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include <gtest/gtest.h>
#include <string.h>
extern "C" {
#include "nr_td_fieldbook.h"
}
static nr_pdsch_cfg_hypothesis_t H(int S, int L, int ap = 1) {
  nr_pdsch_cfg_hypothesis_t h = {};
  h.tda_start = S;
  h.tda_length = L;
  h.dmrs_add_pos = ap;
  h.dmrs_max_len = 1;
  return h;
}
static nr_td_side_info_t si0() {
  nr_td_side_info_t s = {};
  s.f_S = s.f_L = s.f_mapping = s.f_k0 = s.f_dmrs_add_pos = s.f_dmrs_max_len = -1;
  return s;
}
TEST(TdFieldBook, OneRntiNeverPromotesTwoDo) {
  nr_td_fieldbook_t fb;
  nr_td_fieldbook_init(&fb, 2, 2);
  const auto h = H(1, 13, 1);
  nr_td_fieldbook_converged(&fb, 0x4601, &h, 10, 0);
  EXPECT_EQ(fb.f[NR_TD_F_TDRA].value, -1);
  nr_td_fieldbook_converged(&fb, 0x4601, &h, 20, 0); /* same RNTI again: still one */
  EXPECT_EQ(fb.f[NR_TD_F_TDRA].value, -1);
  nr_td_fieldbook_converged(&fb, 0x4602, &h, 30, 0);
  EXPECT_EQ(fb.f[NR_TD_F_TDRA].value, nr_td_pack_tdra(1, 13, 0, 0));
  EXPECT_EQ(fb.f[NR_TD_F_DMRS_ADD_POS].value, 1);
}
TEST(TdFieldBook, FieldsPromoteIndependently) {
  nr_td_fieldbook_t fb;
  nr_td_fieldbook_init(&fb, 2, 2);
  const auto a = H(1, 13, 1), b = H(1, 13, 2); /* same TDRA, different add-pos */
  nr_td_fieldbook_converged(&fb, 1, &a, 1, 0);
  nr_td_fieldbook_converged(&fb, 2, &b, 2, 0);
  EXPECT_NE(fb.f[NR_TD_F_TDRA].value, -1);
  EXPECT_EQ(fb.f[NR_TD_F_DMRS_ADD_POS].value, -1);
}
TEST(TdFieldBook, OneContradictionKeepsTwoWithdraw) {
  nr_td_fieldbook_t fb;
  nr_td_fieldbook_init(&fb, 2, 2);
  const auto h = H(1, 13, 1);
  nr_td_fieldbook_converged(&fb, 1, &h, 1, 0);
  nr_td_fieldbook_converged(&fb, 2, &h, 2, 0);
  nr_td_fieldbook_contradict(&fb, 3, NR_TD_F_TDRA);
  nr_td_fieldbook_contradict(&fb, 3, NR_TD_F_TDRA); /* same RNTI twice = one */
  EXPECT_NE(fb.f[NR_TD_F_TDRA].value, -1);
  nr_td_fieldbook_contradict(&fb, 4, NR_TD_F_TDRA);
  EXPECT_EQ(fb.f[NR_TD_F_TDRA].value, -1);
}
TEST(TdFieldBook, ConvergedWithOtherValueContradicts) {
  nr_td_fieldbook_t fb;
  nr_td_fieldbook_init(&fb, 2, 2);
  const auto h = H(1, 13, 1), o = H(2, 12, 1);
  nr_td_fieldbook_converged(&fb, 1, &h, 1, 0);
  nr_td_fieldbook_converged(&fb, 2, &h, 2, 0);
  nr_td_fieldbook_converged(&fb, 3, &o, 3, 0);
  nr_td_fieldbook_converged(&fb, 4, &o, 4, 0);
  /* x withdrawn by 2 distinct contradicting RNTIs; their own value o (2 supporters) is promoted in the same call */
  EXPECT_EQ(fb.f[NR_TD_F_TDRA].value, nr_td_pack_tdra(2, 12, 0, 0));
}
/* DELIBERATE CHANGE (BC4): was EpochBumpDropsBonus (1 RNTI re-confirms, no hint). Spec addendum s4: a bump keeps a hint
 * (ordering only), pruning stops, and 2 independent new-epoch supporters are needed to promote again. */
TEST(TdFieldBook, EpochBumpKeepsHintNeedsTwoNewSupporters) {
  nr_td_fieldbook_t fb;
  nr_td_fieldbook_init(&fb, 2, 2);
  const auto h = H(1, 13, 1);
  nr_td_fieldbook_converged(&fb, 1, &h, 1, 0);
  nr_td_fieldbook_converged(&fb, 2, &h, 2, 0);
  nr_td_side_info_t s = si0();
  nr_td_fieldbook_fill_side_info(&fb, &s);
  EXPECT_EQ(s.f_S, 1);
  EXPECT_EQ(s.f_L, 13);
  EXPECT_EQ(s.f_mapping, 0);
  EXPECT_EQ(s.f_k0, 0);
  EXPECT_EQ(s.f_dmrs_add_pos, 1);
  EXPECT_EQ(s.f_dmrs_max_len, 1);
  EXPECT_FLOAT_EQ(s.f_conf, 1.0f);
  nr_td_fieldbook_bump_epoch(&fb);
  EXPECT_EQ(fb.f[NR_TD_F_TDRA].epoch, fb.epoch - 1); /* last-confirmed epoch kept across the bump */
  int32_t v;
  for (int f = 0; f < NR_TD_F_COUNT; f++)
    EXPECT_FALSE(nr_td_fieldbook_prunes(&fb, (nr_td_field_t)f, &v));
  s = si0();
  nr_td_fieldbook_fill_side_info(&fb, &s);
  EXPECT_EQ(s.f_S, 1); /* hint */
  EXPECT_EQ(s.f_L, 13);
  nr_td_fieldbook_converged(&fb, 5, &h, 9, 0); /* one new-epoch converger does not promote */
  EXPECT_FALSE(nr_td_fieldbook_prunes(&fb, NR_TD_F_TDRA, &v));
  nr_td_fieldbook_converged(&fb, 6, &h, 10, 0); /* a second distinct one does */
  EXPECT_TRUE(nr_td_fieldbook_prunes(&fb, NR_TD_F_TDRA, &v));
  EXPECT_EQ(v, nr_td_pack_tdra(1, 13, 0, 0));
  EXPECT_EQ(fb.f[NR_TD_F_TDRA].epoch, fb.epoch); /* re-promoted in new epoch */
}
TEST(TdFieldBook, EvictedRntiStillCountsForPromotion) {
  /* The field book keeps its own RNTI sets; it does not depend on Technique D's 64-slot RNTI context table. */
  nr_td_fieldbook_t fb;
  nr_td_fieldbook_init(&fb, 2, 2);
  const auto h = H(1, 13, 1);
  nr_td_fieldbook_converged(&fb, 1, &h, 1, 0);
  nr_td_fieldbook_converged(&fb, 2, &h, 500, 0); /* no intervening traffic: promotes on the second RNTI */
  EXPECT_NE(fb.f[NR_TD_F_TDRA].value, -1);
}
TEST(TdFieldBook, PromotionIsOrderIndependentAfterChurn) {
  /* Spec 4.6: two distinct RNTIs converged with the SAME value promote it, whatever converged in between.
   * Each value keeps its own support set in a small per-field table (evict fewest supporters, ties oldest). */
  nr_td_fieldbook_t fb;
  nr_td_fieldbook_init(&fb, 2, 2);
  const auto h = H(1, 13, 1);
  nr_td_fieldbook_converged(&fb, 1, &h, 1, 0);
  for (int r = 100; r < 120; r++) {
    const auto o = H((r - 100) % 10, 4 + (r - 100) % 5, 0); /* 20 other RNTIs, other values */
    nr_td_fieldbook_converged(&fb, (uint16_t)r, &o, r, 0);
  }
  /* RNTI 1's support for h may have been evicted from the 4-row table by the churn (it had 1 supporter, like the
   * newcomers; oldest goes first); so re-confirming by 2 fresh RNTIs must still promote. */
  nr_td_fieldbook_converged(&fb, 2, &h, 500, 0);
  nr_td_fieldbook_converged(&fb, 3, &h, 501, 0);
  EXPECT_EQ(fb.f[NR_TD_F_TDRA].value, nr_td_pack_tdra(1, 13, 0, 0));
}
TEST(TdFieldBook, SingleLaterRntiPromotesWhenChurnFitsTable) {
  /* A=x, then 3 other single-supporter values (table of 4 not overflowed), then B=x promotes x. With more
   * distinct other values than rows, x (1 supporter, oldest) is the eviction victim, see the test above. */
  nr_td_fieldbook_t fb;
  nr_td_fieldbook_init(&fb, 2, 2);
  const auto h = H(1, 13, 1);
  nr_td_fieldbook_converged(&fb, 1, &h, 1, 0);
  for (int r = 100; r < 103; r++) {
    const auto o = H((r - 100) % 10, 4 + (r - 100) % 5, 0);
    nr_td_fieldbook_converged(&fb, (uint16_t)r, &o, r, 0);
  }
  nr_td_fieldbook_converged(&fb, 2, &h, 500, 0);
  EXPECT_EQ(fb.f[NR_TD_F_TDRA].value, nr_td_pack_tdra(1, 13, 0, 0));
}
TEST(TdFieldBook, InterleavedOtherValueDoesNotBlockPromotion) {
  nr_td_fieldbook_t fb;
  nr_td_fieldbook_init(&fb, 2, 2);
  const auto x = H(1, 13, 1), y = H(2, 12, 1);
  nr_td_fieldbook_converged(&fb, 1, &x, 1, 0);
  nr_td_fieldbook_converged(&fb, 3, &y, 2, 0);
  nr_td_fieldbook_converged(&fb, 2, &x, 3, 0); /* A=x, C=y, B=x */
  EXPECT_EQ(fb.f[NR_TD_F_TDRA].value, nr_td_pack_tdra(1, 13, 0, 0));
}
TEST(TdFieldBook, EvictionKeepsBestSupportedCandidate) {
  nr_td_fieldbook_t fb;
  nr_td_fieldbook_init(&fb, 3, 2);
  const auto x = H(1, 13, 1);
  nr_td_fieldbook_converged(&fb, 1, &x, 1, 0);
  nr_td_fieldbook_converged(&fb, 2, &x, 2, 0); /* x has 2 supporters, needs 3 */
  for (int r = 100; r < 120; r++) {
    const auto o = H((r - 100) % 10, 4 + (r - 100) % 5, 0);
    nr_td_fieldbook_converged(&fb, (uint16_t)r, &o, r, 0);
  }
  EXPECT_EQ(fb.f[NR_TD_F_TDRA].value, -1);
  nr_td_fieldbook_converged(&fb, 3, &x, 500, 0); /* x survived eviction with its 2 supporters */
  EXPECT_EQ(fb.f[NR_TD_F_TDRA].value, nr_td_pack_tdra(1, 13, 0, 0));
}
TEST(TdFieldBook, WithdrawalPromotesContradictersValueInSameCall) {
  nr_td_fieldbook_t fb;
  nr_td_fieldbook_init(&fb, 2, 2);
  const auto x = H(1, 13, 1), y = H(2, 12, 1);
  nr_td_fieldbook_converged(&fb, 1, &x, 1, 0);
  nr_td_fieldbook_converged(&fb, 2, &x, 2, 0);
  nr_td_fieldbook_converged(&fb, 3, &y, 3, 0);
  EXPECT_EQ(fb.f[NR_TD_F_TDRA].value, nr_td_pack_tdra(1, 13, 0, 0));
  nr_td_fieldbook_converged(&fb, 4, &y, 4, 0); /* withdraws x AND promotes y in this same call */
  EXPECT_EQ(fb.f[NR_TD_F_TDRA].value, nr_td_pack_tdra(2, 12, 0, 0));
  EXPECT_EQ(fb.f[NR_TD_F_DMRS_ADD_POS].value, 1); /* unaffected field untouched */
}
/* DELIBERATE CHANGE (BC4): was ContradictionsClearedOnEpochBump (value stayed set, n_contra == 1 after bump). After a bump
 * the field is CANDIDATE with value -1: a contradiction is a no-op, and the pre-bump contradiction is cleared. */
TEST(TdFieldBook, ContradictAfterEpochBumpIsNoOp) {
  nr_td_fieldbook_t fb;
  nr_td_fieldbook_init(&fb, 2, 2);
  const auto x = H(1, 13, 1);
  nr_td_fieldbook_converged(&fb, 1, &x, 1, 0);
  nr_td_fieldbook_converged(&fb, 2, &x, 2, 0);
  nr_td_fieldbook_contradict(&fb, 3, NR_TD_F_TDRA);
  EXPECT_EQ(fb.f[NR_TD_F_TDRA].n_contra, 1);
  nr_td_fieldbook_bump_epoch(&fb);
  EXPECT_EQ(fb.f[NR_TD_F_TDRA].n_contra, 0); /* pre-bump contradiction cleared */
  nr_td_fieldbook_contradict(&fb, 4, NR_TD_F_TDRA);
  EXPECT_EQ(fb.f[NR_TD_F_TDRA].value, -1);
  EXPECT_EQ(fb.f[NR_TD_F_TDRA].n_contra, 0);
  EXPECT_EQ(nr_td_fieldbook_state(&fb, NR_TD_F_TDRA), NR_TD_FS_CANDIDATE);
}
TEST(TdFieldBook, PackUnpackRoundTripAndMasking) {
  nr_td_fieldbook_t fb;
  nr_td_fieldbook_init(&fb, 1, 1);
  nr_pdsch_cfg_hypothesis_t h = H(2, 12, 2);
  h.mapping_type = 1;
  h.k0 = 5;
  nr_td_fieldbook_converged(&fb, 1, &h, 1, 0);
  nr_td_side_info_t s = si0();
  nr_td_fieldbook_fill_side_info(&fb, &s);
  EXPECT_EQ(s.f_S, 2);
  EXPECT_EQ(s.f_L, 12);
  EXPECT_EQ(s.f_mapping, 1);
  EXPECT_EQ(s.f_k0, 5);
  EXPECT_EQ(nr_td_pack_tdra(0x1F2, 0x3C, 3, 0x45), nr_td_pack_tdra(2, 28, 1, 5)); /* inputs masked to field widths */
}
TEST(TdFieldBook, ThresholdsClampedToValidRange) {
  nr_td_fieldbook_t fb;
  nr_td_fieldbook_init(&fb, 0, 99);
  EXPECT_EQ(fb.promote_rntis, 1);
  EXPECT_EQ(fb.withdraw_rntis, NR_TD_FB_MAX_RNTI);
}
TEST(TdFieldBook, FullContradictSetAtCapacityWithdrawsAndExtraIsHarmless) {
  /* Thresholds are clamped to NR_TD_FB_MAX_RNTI, so the largest threshold is reachable exactly at capacity. */
  nr_td_fieldbook_t fb;
  nr_td_fieldbook_init(&fb, 2, 16);
  const auto h = H(1, 13, 1);
  nr_td_fieldbook_converged(&fb, 1, &h, 1, 0);
  nr_td_fieldbook_converged(&fb, 2, &h, 2, 0);
  /* 15 keep, the 16th withdraws, a 17th afterwards is harmless */
  for (int r = 100; r < 115; r++)
    nr_td_fieldbook_contradict(&fb, (uint16_t)r, NR_TD_F_TDRA);
  EXPECT_NE(fb.f[NR_TD_F_TDRA].value, -1);
  nr_td_fieldbook_contradict(&fb, 115, NR_TD_F_TDRA);
  EXPECT_EQ(fb.f[NR_TD_F_TDRA].value, -1);
  nr_td_fieldbook_contradict(&fb, 116, NR_TD_F_TDRA);
  EXPECT_EQ(fb.f[NR_TD_F_TDRA].value, -1);
}

static nr_pdsch_cfg_hypothesis_t Hq(int S, int L) { return H(S, L, 1); }
TEST(FieldBookSM, PromoteSuspectWithdraw) {
  nr_td_fieldbook_t fb; nr_td_fieldbook_init(&fb, 2, 2);
  auto a = Hq(2, 12), b = Hq(1, 13);
  nr_td_fieldbook_converged(&fb, 1, &a, 0, 0); nr_td_fieldbook_converged(&fb, 2, &a, 0, 0);
  EXPECT_EQ(nr_td_fieldbook_state(&fb, NR_TD_F_TDRA), NR_TD_FS_PROMOTED);
  nr_td_fieldbook_converged(&fb, 3, &b, 0, 0);
  EXPECT_EQ(nr_td_fieldbook_state(&fb, NR_TD_F_TDRA), NR_TD_FS_SUSPECT);
  int32_t v; EXPECT_FALSE(nr_td_fieldbook_prunes(&fb, NR_TD_F_TDRA, &v));
  nr_td_fieldbook_converged(&fb, 4, &b, 0, 0);
  EXPECT_EQ(fb.n_withdrawn, 1u);
  EXPECT_TRUE(nr_td_fieldbook_prunes(&fb, NR_TD_F_TDRA, &v)); /* b had 2 supporters: promoted at once */
  EXPECT_EQ(v, nr_td_pack_tdra(1, 13, 0, 0));
}
TEST(FieldBookSM, SuspectReconfirmed) {
  nr_td_fieldbook_t fb; nr_td_fieldbook_init(&fb, 2, 2); auto a = Hq(2, 12), b = Hq(1, 13);
  nr_td_fieldbook_converged(&fb, 1, &a, 0, 0); nr_td_fieldbook_converged(&fb, 2, &a, 0, 0);
  nr_td_fieldbook_converged(&fb, 3, &b, 0, 0); nr_td_fieldbook_converged(&fb, 5, &a, 0, 0);
  EXPECT_EQ(nr_td_fieldbook_state(&fb, NR_TD_F_TDRA), NR_TD_FS_PROMOTED);
}
TEST(FieldBookSM, PrunedContextIsNotIndependent) {
  nr_td_fieldbook_t fb; nr_td_fieldbook_init(&fb, 2, 2); auto a = Hq(2, 12);
  nr_td_fieldbook_converged(&fb, 1, &a, 0, 1u << NR_TD_F_TDRA); nr_td_fieldbook_converged(&fb, 2, &a, 0, 1u << NR_TD_F_TDRA);
  EXPECT_NE(nr_td_fieldbook_state(&fb, NR_TD_F_TDRA), NR_TD_FS_PROMOTED);
}
TEST(FieldBookSM, EpochBumpStopsPruningKeepsHint) { /* Review Focus 4 */
  nr_td_fieldbook_t fb; nr_td_fieldbook_init(&fb, 2, 2); auto a = Hq(2, 12);
  nr_td_fieldbook_converged(&fb, 1, &a, 0, 0); nr_td_fieldbook_converged(&fb, 2, &a, 0, 0);
  const uint32_t g = nr_td_fieldbook_generation(&fb);
  nr_td_fieldbook_bump_epoch(&fb);
  int32_t v; for (int f = 0; f < NR_TD_F_COUNT; f++) EXPECT_FALSE(nr_td_fieldbook_prunes(&fb, (nr_td_field_t)f, &v));
  EXPECT_GT(nr_td_fieldbook_generation(&fb), g);
  nr_td_side_info_t si; memset(&si, 0, sizeof(si)); si.f_S = si.f_L = -1;
  nr_td_fieldbook_fill_side_info(&fb, &si); EXPECT_EQ(si.f_S, 2); EXPECT_EQ(si.f_L, 12);
  nr_td_fieldbook_converged(&fb, 1, &a, 0, 0);
  EXPECT_FALSE(nr_td_fieldbook_prunes(&fb, NR_TD_F_TDRA, &v)); /* old-epoch support does not count */
}
TEST(FieldBookSM, HypMatches) {
  auto a = Hq(2, 12);
  EXPECT_TRUE(nr_td_fieldbook_hyp_matches(NR_TD_F_TDRA, nr_td_pack_tdra(2, 12, 0, 0), &a));
  EXPECT_FALSE(nr_td_fieldbook_hyp_matches(NR_TD_F_DMRS_ADD_POS, 2, &a));
}
TEST(FieldBookSM, GenerationBumpsOnEveryStateChangeAndForcePromote) {
  nr_td_fieldbook_t fb; nr_td_fieldbook_init(&fb, 2, 2); auto a = Hq(2, 12);
  uint32_t g = nr_td_fieldbook_generation(&fb);
  nr_td_fieldbook_converged(&fb, 1, &a, 0, 0);
  EXPECT_EQ(nr_td_fieldbook_state(&fb, NR_TD_F_TDRA), NR_TD_FS_CANDIDATE);
  nr_td_fieldbook_converged(&fb, 2, &a, 0, 0);
  EXPECT_GT(nr_td_fieldbook_generation(&fb), g); g = nr_td_fieldbook_generation(&fb);
  nr_td_fieldbook_contradict(&fb, 3, NR_TD_F_TDRA); /* -> SUSPECT */
  EXPECT_GT(nr_td_fieldbook_generation(&fb), g); g = nr_td_fieldbook_generation(&fb);
  nr_td_fieldbook_contradict(&fb, 3, NR_TD_F_TDRA); /* same RNTI: no change */
  EXPECT_EQ(nr_td_fieldbook_generation(&fb), g);
  nr_td_fieldbook_force_promote(&fb, NR_TD_F_DMRS_MAX_LEN, 2);
  int32_t v; EXPECT_TRUE(nr_td_fieldbook_prunes(&fb, NR_TD_F_DMRS_MAX_LEN, &v)); EXPECT_EQ(v, 2);
}
TEST(FieldBookSM, ReconfirmWorksWithFullSupportSet) {
  nr_td_fieldbook_t fb; nr_td_fieldbook_init(&fb, 2, 2); auto a = Hq(2, 12), b = Hq(1, 13);
  nr_td_fieldbook_converged(&fb, 1, &a, 0, 0); nr_td_fieldbook_converged(&fb, 2, &a, 0, 0);
  for (int r = 100; r < 116; r++) nr_td_fieldbook_converged(&fb, (uint16_t)r, &a, 0, 0);
  nr_td_fieldbook_converged(&fb, 3, &b, 0, 0);
  EXPECT_EQ(nr_td_fieldbook_state(&fb, NR_TD_F_TDRA), NR_TD_FS_SUSPECT);
  nr_td_fieldbook_converged(&fb, 500, &a, 0, 0);
  EXPECT_EQ(nr_td_fieldbook_state(&fb, NR_TD_F_TDRA), NR_TD_FS_PROMOTED);
}
TEST(FieldBookSM, HypMatchesMappingAndK0) {
  auto a = Hq(2, 12);
  EXPECT_FALSE(nr_td_fieldbook_hyp_matches(NR_TD_F_TDRA, nr_td_pack_tdra(2, 12, 1, 0), &a));
  EXPECT_TRUE(nr_td_fieldbook_hyp_matches(NR_TD_F_TDRA, nr_td_pack_tdra(2, 12, 0, 3), &a)); /* BC6b: k0 is never part of the prune predicate */
}
TEST(FieldBookSM, FieldBookTdraPruneIgnoresK0) {
  auto a = Hq(2, 12);
  for (int k0 = 0; k0 <= 32; k0++) {
    a.k0 = k0;
    EXPECT_TRUE(nr_td_fieldbook_hyp_matches(NR_TD_F_TDRA, nr_td_pack_tdra(2, 12, 0, 0), &a)) << k0;
    EXPECT_TRUE(nr_td_fieldbook_hyp_matches(NR_TD_F_TDRA, nr_td_pack_tdra(2, 12, 0, 5), &a)) << k0;
  }
  a.k0 = 0;
  EXPECT_FALSE(nr_td_fieldbook_hyp_matches(NR_TD_F_TDRA, nr_td_pack_tdra(1, 12, 0, 0), &a)); /* S, L, mapping still prune */
  EXPECT_FALSE(nr_td_fieldbook_hyp_matches(NR_TD_F_TDRA, nr_td_pack_tdra(2, 11, 0, 0), &a));
  EXPECT_FALSE(nr_td_fieldbook_hyp_matches(NR_TD_F_TDRA, nr_td_pack_tdra(2, 12, 1, 0), &a));
}
TEST(FieldBookSM, WinnerDifferingOnlyInK0ContradictsK0Part) {
  nr_td_fieldbook_t fb; nr_td_fieldbook_init(&fb, 2, 2);
  auto a = Hq(2, 12), b = Hq(2, 12);
  a.k0 = 0; b.k0 = 1;
  nr_td_fieldbook_converged(&fb, 1, &a, 0, 0); nr_td_fieldbook_converged(&fb, 2, &a, 0, 0);
  ASSERT_EQ(nr_td_fieldbook_state(&fb, NR_TD_F_TDRA), NR_TD_FS_PROMOTED);
  const uint32_t g0 = fb.generation;
  nr_td_fieldbook_converged(&fb, 3, &b, 0, 1u << NR_TD_F_TDRA); /* pruned context: still k0-independent evidence */
  EXPECT_EQ(nr_td_fieldbook_state(&fb, NR_TD_F_TDRA), NR_TD_FS_PROMOTED); /* S/L/mapping support is not withdrawn, not even SUSPECT */
  EXPECT_EQ(fb.f[NR_TD_F_TDRA].n_contra, 0);
  EXPECT_EQ(fb.f[NR_TD_F_TDRA].value, nr_td_pack_tdra(2, 12, 0, 0)); /* one k0 contradiction is not enough */
  EXPECT_EQ(fb.generation, g0);
  nr_td_fieldbook_converged(&fb, 3, &b, 0, 0); /* the same RNTI again does not count twice */
  EXPECT_EQ(fb.f[NR_TD_F_TDRA].value, nr_td_pack_tdra(2, 12, 0, 0));
  nr_td_fieldbook_converged(&fb, 4, &b, 0, 0);  /* a second distinct RNTI: the k0 part is re-learned */
  EXPECT_EQ(nr_td_fieldbook_state(&fb, NR_TD_F_TDRA), NR_TD_FS_PROMOTED);
  EXPECT_EQ(fb.f[NR_TD_F_TDRA].value, nr_td_pack_tdra(2, 12, 0, 1));
  EXPECT_EQ(fb.n_withdrawn, 0u);
  int32_t v;
  EXPECT_TRUE(nr_td_fieldbook_prunes(&fb, NR_TD_F_TDRA, &v)); /* the prune never lapsed */
  auto c = Hq(2, 12); c.k0 = 0;
  EXPECT_TRUE(nr_td_fieldbook_hyp_matches(NR_TD_F_TDRA, v, &c)); /* and still keeps the k0 = 0 sibling */
  nr_td_side_info_t s = si0(); nr_td_fieldbook_fill_side_info(&fb, &s);
  EXPECT_EQ(s.f_k0, 1);
}
TEST(FieldBookSM, K0ContradictionsOfDifferentK0DoNotCombine) {
  nr_td_fieldbook_t fb; nr_td_fieldbook_init(&fb, 2, 2);
  auto a = Hq(2, 12), b = Hq(2, 12), c = Hq(2, 12);
  a.k0 = 0; b.k0 = 1; c.k0 = 2;
  nr_td_fieldbook_converged(&fb, 1, &a, 0, 0); nr_td_fieldbook_converged(&fb, 2, &a, 0, 0);
  nr_td_fieldbook_converged(&fb, 3, &b, 0, 0); nr_td_fieldbook_converged(&fb, 4, &c, 0, 0);
  EXPECT_EQ(fb.f[NR_TD_F_TDRA].value, nr_td_pack_tdra(2, 12, 0, 0));
  nr_td_fieldbook_converged(&fb, 5, &a, 0, 0); /* a confirming RNTI clears the pending k0 evidence */
  nr_td_fieldbook_converged(&fb, 6, &b, 0, 0);
  EXPECT_EQ(fb.f[NR_TD_F_TDRA].value, nr_td_pack_tdra(2, 12, 0, 0));
}
TEST(FieldBookSM, PrunedConvergeDoesNotContradict) {
  nr_td_fieldbook_t fb; nr_td_fieldbook_init(&fb, 2, 2); auto a = Hq(2, 12), b = Hq(1, 13);
  nr_td_fieldbook_converged(&fb, 1, &a, 0, 0); nr_td_fieldbook_converged(&fb, 2, &a, 0, 0);
  nr_td_fieldbook_converged(&fb, 3, &b, 0, 1u << NR_TD_F_TDRA);
  EXPECT_EQ(nr_td_fieldbook_state(&fb, NR_TD_F_TDRA), NR_TD_FS_PROMOTED);
  EXPECT_EQ(fb.f[NR_TD_F_TDRA].n_contra, 0);
}
TEST(FieldBookSM, RepeatSupporterDoesNotReconfirm) {
  nr_td_fieldbook_t fb; nr_td_fieldbook_init(&fb, 2, 2); auto a = Hq(2, 12), b = Hq(1, 13);
  nr_td_fieldbook_converged(&fb, 1, &a, 0, 0); nr_td_fieldbook_converged(&fb, 2, &a, 0, 0);
  nr_td_fieldbook_converged(&fb, 3, &b, 0, 0);
  nr_td_fieldbook_converged(&fb, 1, &a, 0, 0);
  EXPECT_EQ(nr_td_fieldbook_state(&fb, NR_TD_F_TDRA), NR_TD_FS_SUSPECT);
}
TEST(FieldBookSM, ContradictingRntiCannotReconfirm) {
  nr_td_fieldbook_t fb; nr_td_fieldbook_init(&fb, 2, 2); auto a = Hq(2, 12), b = Hq(1, 13);
  nr_td_fieldbook_converged(&fb, 1, &a, 0, 0); nr_td_fieldbook_converged(&fb, 2, &a, 0, 0);
  nr_td_fieldbook_converged(&fb, 3, &b, 0, 0);
  nr_td_fieldbook_converged(&fb, 3, &a, 0, 0); /* the contradicter flips back: not independent */
  EXPECT_EQ(nr_td_fieldbook_state(&fb, NR_TD_F_TDRA), NR_TD_FS_SUSPECT);
}
