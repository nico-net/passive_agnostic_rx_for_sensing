/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include <gtest/gtest.h>
extern "C" {
#include "nr_td_fieldbook.h"
}
static nr_pdsch_cfg_hypothesis_t H(int S, int L, int ap) {
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
  nr_td_fieldbook_converged(&fb, 0x4601, &h, 10);
  EXPECT_EQ(fb.f[NR_TD_F_TDRA].value, -1);
  nr_td_fieldbook_converged(&fb, 0x4601, &h, 20); /* same RNTI again: still one */
  EXPECT_EQ(fb.f[NR_TD_F_TDRA].value, -1);
  nr_td_fieldbook_converged(&fb, 0x4602, &h, 30);
  EXPECT_EQ(fb.f[NR_TD_F_TDRA].value, nr_td_pack_tdra(1, 13, 0, 0));
  EXPECT_EQ(fb.f[NR_TD_F_DMRS_ADD_POS].value, 1);
}
TEST(TdFieldBook, FieldsPromoteIndependently) {
  nr_td_fieldbook_t fb;
  nr_td_fieldbook_init(&fb, 2, 2);
  const auto a = H(1, 13, 1), b = H(1, 13, 2); /* same TDRA, different add-pos */
  nr_td_fieldbook_converged(&fb, 1, &a, 1);
  nr_td_fieldbook_converged(&fb, 2, &b, 2);
  EXPECT_NE(fb.f[NR_TD_F_TDRA].value, -1);
  EXPECT_EQ(fb.f[NR_TD_F_DMRS_ADD_POS].value, -1);
}
TEST(TdFieldBook, OneContradictionKeepsTwoWithdraw) {
  nr_td_fieldbook_t fb;
  nr_td_fieldbook_init(&fb, 2, 2);
  const auto h = H(1, 13, 1);
  nr_td_fieldbook_converged(&fb, 1, &h, 1);
  nr_td_fieldbook_converged(&fb, 2, &h, 2);
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
  nr_td_fieldbook_converged(&fb, 1, &h, 1);
  nr_td_fieldbook_converged(&fb, 2, &h, 2);
  nr_td_fieldbook_converged(&fb, 3, &o, 3);
  nr_td_fieldbook_converged(&fb, 4, &o, 4);
  /* x withdrawn by 2 distinct contradicting RNTIs; their own value o (2 supporters) is promoted in the same call */
  EXPECT_EQ(fb.f[NR_TD_F_TDRA].value, nr_td_pack_tdra(2, 12, 0, 0));
}
TEST(TdFieldBook, EpochBumpDropsBonus) {
  nr_td_fieldbook_t fb;
  nr_td_fieldbook_init(&fb, 2, 2);
  const auto h = H(1, 13, 1);
  nr_td_fieldbook_converged(&fb, 1, &h, 1);
  nr_td_fieldbook_converged(&fb, 2, &h, 2);
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
  s = si0();
  nr_td_fieldbook_fill_side_info(&fb, &s);
  EXPECT_EQ(s.f_S, -1);
  EXPECT_FLOAT_EQ(s.f_conf, 0.0f);
  nr_td_fieldbook_converged(&fb, 5, &h, 9); /* one re-confirmation restores it */
  s = si0();
  nr_td_fieldbook_fill_side_info(&fb, &s);
  EXPECT_EQ(s.f_S, 1);
}
TEST(TdFieldBook, EvictedRntiStillCountsForPromotion) {
  /* The field book keeps its own RNTI sets; it does not depend on Technique D's 64-slot RNTI context table. */
  nr_td_fieldbook_t fb;
  nr_td_fieldbook_init(&fb, 2, 2);
  const auto h = H(1, 13, 1);
  nr_td_fieldbook_converged(&fb, 1, &h, 1);
  nr_td_fieldbook_converged(&fb, 2, &h, 500); /* no intervening traffic: promotes on the second RNTI */
  EXPECT_NE(fb.f[NR_TD_F_TDRA].value, -1);
}
TEST(TdFieldBook, PromotionIsOrderIndependentAfterChurn) {
  /* Spec 4.6: two distinct RNTIs converged with the SAME value promote it, whatever converged in between.
   * Each value keeps its own support set in a small per-field table (evict fewest supporters, ties oldest). */
  nr_td_fieldbook_t fb;
  nr_td_fieldbook_init(&fb, 2, 2);
  const auto h = H(1, 13, 1);
  nr_td_fieldbook_converged(&fb, 1, &h, 1);
  for (int r = 100; r < 120; r++) {
    const auto o = H((r - 100) % 10, 4 + (r - 100) % 5, 0); /* 20 other RNTIs, other values */
    nr_td_fieldbook_converged(&fb, (uint16_t)r, &o, r);
  }
  /* RNTI 1's support for h may have been evicted from the 4-row table by the churn (it had 1 supporter, like the
   * newcomers; oldest goes first); so re-confirming by 2 fresh RNTIs must still promote. */
  nr_td_fieldbook_converged(&fb, 2, &h, 500);
  nr_td_fieldbook_converged(&fb, 3, &h, 501);
  EXPECT_EQ(fb.f[NR_TD_F_TDRA].value, nr_td_pack_tdra(1, 13, 0, 0));
}
TEST(TdFieldBook, SingleLaterRntiPromotesWhenChurnFitsTable) {
  /* A=x, then 3 other single-supporter values (table of 4 not overflowed), then B=x promotes x. With more
   * distinct other values than rows, x (1 supporter, oldest) is the eviction victim, see the test above. */
  nr_td_fieldbook_t fb;
  nr_td_fieldbook_init(&fb, 2, 2);
  const auto h = H(1, 13, 1);
  nr_td_fieldbook_converged(&fb, 1, &h, 1);
  for (int r = 100; r < 103; r++) {
    const auto o = H((r - 100) % 10, 4 + (r - 100) % 5, 0);
    nr_td_fieldbook_converged(&fb, (uint16_t)r, &o, r);
  }
  nr_td_fieldbook_converged(&fb, 2, &h, 500);
  EXPECT_EQ(fb.f[NR_TD_F_TDRA].value, nr_td_pack_tdra(1, 13, 0, 0));
}
TEST(TdFieldBook, InterleavedOtherValueDoesNotBlockPromotion) {
  nr_td_fieldbook_t fb;
  nr_td_fieldbook_init(&fb, 2, 2);
  const auto x = H(1, 13, 1), y = H(2, 12, 1);
  nr_td_fieldbook_converged(&fb, 1, &x, 1);
  nr_td_fieldbook_converged(&fb, 3, &y, 2);
  nr_td_fieldbook_converged(&fb, 2, &x, 3); /* A=x, C=y, B=x */
  EXPECT_EQ(fb.f[NR_TD_F_TDRA].value, nr_td_pack_tdra(1, 13, 0, 0));
}
TEST(TdFieldBook, EvictionKeepsBestSupportedCandidate) {
  nr_td_fieldbook_t fb;
  nr_td_fieldbook_init(&fb, 3, 2);
  const auto x = H(1, 13, 1);
  nr_td_fieldbook_converged(&fb, 1, &x, 1);
  nr_td_fieldbook_converged(&fb, 2, &x, 2); /* x has 2 supporters, needs 3 */
  for (int r = 100; r < 120; r++) {
    const auto o = H((r - 100) % 10, 4 + (r - 100) % 5, 0);
    nr_td_fieldbook_converged(&fb, (uint16_t)r, &o, r);
  }
  EXPECT_EQ(fb.f[NR_TD_F_TDRA].value, -1);
  nr_td_fieldbook_converged(&fb, 3, &x, 500); /* x survived eviction with its 2 supporters */
  EXPECT_EQ(fb.f[NR_TD_F_TDRA].value, nr_td_pack_tdra(1, 13, 0, 0));
}
TEST(TdFieldBook, WithdrawalPromotesContradictersValueInSameCall) {
  nr_td_fieldbook_t fb;
  nr_td_fieldbook_init(&fb, 2, 2);
  const auto x = H(1, 13, 1), y = H(2, 12, 1);
  nr_td_fieldbook_converged(&fb, 1, &x, 1);
  nr_td_fieldbook_converged(&fb, 2, &x, 2);
  nr_td_fieldbook_converged(&fb, 3, &y, 3);
  EXPECT_EQ(fb.f[NR_TD_F_TDRA].value, nr_td_pack_tdra(1, 13, 0, 0));
  nr_td_fieldbook_converged(&fb, 4, &y, 4); /* withdraws x AND promotes y in this same call */
  EXPECT_EQ(fb.f[NR_TD_F_TDRA].value, nr_td_pack_tdra(2, 12, 0, 0));
  EXPECT_EQ(fb.f[NR_TD_F_DMRS_ADD_POS].value, 1); /* unaffected field untouched */
}
TEST(TdFieldBook, ContradictionsClearedOnEpochBump) {
  nr_td_fieldbook_t fb;
  nr_td_fieldbook_init(&fb, 2, 2);
  const auto x = H(1, 13, 1);
  nr_td_fieldbook_converged(&fb, 1, &x, 1);
  nr_td_fieldbook_converged(&fb, 2, &x, 2);
  nr_td_fieldbook_contradict(&fb, 3, NR_TD_F_TDRA);
  nr_td_fieldbook_bump_epoch(&fb);
  nr_td_fieldbook_contradict(&fb, 4, NR_TD_F_TDRA);
  EXPECT_NE(fb.f[NR_TD_F_TDRA].value, -1); /* only 1 contradiction in the new epoch */
  EXPECT_EQ(fb.f[NR_TD_F_TDRA].n_contra, 1);
}
TEST(TdFieldBook, PackUnpackRoundTripAndMasking) {
  nr_td_fieldbook_t fb;
  nr_td_fieldbook_init(&fb, 1, 1);
  nr_pdsch_cfg_hypothesis_t h = H(2, 12, 2);
  h.mapping_type = 1;
  h.k0 = 5;
  nr_td_fieldbook_converged(&fb, 1, &h, 1);
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
  nr_td_fieldbook_converged(&fb, 1, &h, 1);
  nr_td_fieldbook_converged(&fb, 2, &h, 2);
  /* 15 keep, the 16th withdraws, a 17th afterwards is harmless */
  for (int r = 100; r < 115; r++)
    nr_td_fieldbook_contradict(&fb, (uint16_t)r, NR_TD_F_TDRA);
  EXPECT_NE(fb.f[NR_TD_F_TDRA].value, -1);
  nr_td_fieldbook_contradict(&fb, 115, NR_TD_F_TDRA);
  EXPECT_EQ(fb.f[NR_TD_F_TDRA].value, -1);
  nr_td_fieldbook_contradict(&fb, 116, NR_TD_F_TDRA);
  EXPECT_EQ(fb.f[NR_TD_F_TDRA].value, -1);
}
