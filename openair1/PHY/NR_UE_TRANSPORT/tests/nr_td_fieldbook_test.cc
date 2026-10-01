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
  EXPECT_EQ(fb.f[NR_TD_F_TDRA].value, -1); /* withdrawn by 2 distinct contradicting RNTIs */
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
TEST(TdFieldBook, OtherTdraValuesBetweenResetCandidate) {
  /* Plan step-3 algorithm: while a field is unpromoted a converged RNTI with a different value REPLACES `candidate`
   * and restarts `support` at that single RNTI. So 20 other RNTIs, each with a distinct TDRA (never two with the
   * same value, so none promotes), wipe RNTI 1's support for h; the next RNTI with h only restarts the count, and
   * h is promoted only once a further distinct RNTI converges on h again. */
  nr_td_fieldbook_t fb;
  nr_td_fieldbook_init(&fb, 2, 2);
  const auto h = H(1, 13, 1);
  nr_td_fieldbook_converged(&fb, 1, &h, 1);
  for (int r = 100; r < 120; r++) {
    const auto o = H((r - 100) % 10, 4 + (r - 100) % 5, 0); /* varying S: distinct from each neighbour */
    nr_td_fieldbook_converged(&fb, (uint16_t)r, &o, r);
  }
  EXPECT_EQ(fb.f[NR_TD_F_TDRA].value, -1);
  nr_td_fieldbook_converged(&fb, 2, &h, 500); /* support restarts at {2}, RNTI 1 was forgotten */
  EXPECT_EQ(fb.f[NR_TD_F_TDRA].value, -1);
  nr_td_fieldbook_converged(&fb, 3, &h, 501);
  EXPECT_EQ(fb.f[NR_TD_F_TDRA].value, nr_td_pack_tdra(1, 13, 0, 0));
}
TEST(TdFieldBook, FullContradictSetIgnoresExtraRntis) {
  nr_td_fieldbook_t fb;
  nr_td_fieldbook_init(&fb, 2, 20); /* threshold above NR_TD_FB_MAX_RNTI: set fills, 17th is ignored */
  const auto h = H(1, 13, 1);
  nr_td_fieldbook_converged(&fb, 1, &h, 1);
  nr_td_fieldbook_converged(&fb, 2, &h, 2);
  for (int r = 100; r < 117; r++)
    nr_td_fieldbook_contradict(&fb, (uint16_t)r, NR_TD_F_TDRA);
  EXPECT_EQ(fb.f[NR_TD_F_TDRA].n_contra, NR_TD_FB_MAX_RNTI);
  EXPECT_NE(fb.f[NR_TD_F_TDRA].value, -1);
  /* threshold within capacity still behaves: 15 keep, the 16th withdraws, a 17th afterwards is harmless */
  nr_td_fieldbook_init(&fb, 2, 16);
  nr_td_fieldbook_converged(&fb, 1, &h, 1);
  nr_td_fieldbook_converged(&fb, 2, &h, 2);
  for (int r = 100; r < 115; r++)
    nr_td_fieldbook_contradict(&fb, (uint16_t)r, NR_TD_F_TDRA);
  EXPECT_NE(fb.f[NR_TD_F_TDRA].value, -1);
  nr_td_fieldbook_contradict(&fb, 115, NR_TD_F_TDRA);
  EXPECT_EQ(fb.f[NR_TD_F_TDRA].value, -1);
  nr_td_fieldbook_contradict(&fb, 116, NR_TD_F_TDRA);
  EXPECT_EQ(fb.f[NR_TD_F_TDRA].value, -1);
}
