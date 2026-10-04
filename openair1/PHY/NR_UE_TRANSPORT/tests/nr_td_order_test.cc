// SPDX-License-Identifier: LicenseRef-CSSL-1.0
#include <gtest/gtest.h>
extern "C" {
#include "nr_td_order.h"
#include "nr_pdsch_qm_oracle.h"
}
static nr_pdsch_cfg_hypothesis_t H(int S, int L, int map, int k0) {
  nr_pdsch_cfg_hypothesis_t h = {}; h.tda_start = S; h.tda_length = L; h.mapping_type = map; h.k0 = k0; return h;
}
static nr_td_side_info_t neutral() {
  nr_td_side_info_t s = {}; s.obs_dmrs_mask = -1; s.obs_qm = -1; s.obs_last_symbol = -1;
  s.f_S = s.f_L = s.f_mapping = s.f_k0 = s.f_dmrs_add_pos = s.f_dmrs_max_len = -1; return s;
}
TEST(TdOrder, AllZeroWeightsGiveZero) {
  nr_td_side_info_t s = neutral(); s.n_sib1 = 1; s.sib1[0] = {1, 13, 0, 0}; s.dmrs_typeA_pos = 2;
  const nr_pdsch_cfg_hypothesis_t h = H(1, 13, 0, 0);
  EXPECT_EQ(nr_td_ordering_score(&h, &s), 0.0f);
}
TEST(TdOrder, Sib1MatchScoresAboveNonMatch) {
  nr_td_side_info_t s = neutral(); s.w_sib1 = 1; s.n_sib1 = 2; s.sib1[0] = {1, 13, 0, 0}; s.sib1[1] = {1, 5, 0, 0};
  const nr_pdsch_cfg_hypothesis_t a = H(1, 13, 0, 0), b = H(2, 12, 0, 0);
  EXPECT_GT(nr_td_ordering_score(&a, &s), nr_td_ordering_score(&b, &s));
}
TEST(TdOrder, DefaultTableARow1DependsOnTypeAPos) {
  nr_td_tdra_t r;
  ASSERT_TRUE(nr_td_default_table_a(1, 2, &r)); EXPECT_EQ(r.S, 2); EXPECT_EQ(r.L, 12); EXPECT_EQ(r.mapping, 0);
  ASSERT_TRUE(nr_td_default_table_a(1, 3, &r)); EXPECT_EQ(r.S, 3); EXPECT_EQ(r.L, 11);
  EXPECT_FALSE(nr_td_default_table_a(17, 2, &r));
}
TEST(TdOrder, DefaultTableAMatchesSpecRows) {
  /* TS 38.214 Table 5.1.2.1.1-2 (cross-checked with OAI table_5_1_2_1_1_2_*): row -> {map, S2, L2, S3, L3} */
  static const int e[16][5] = {{0, 2, 12, 3, 11}, {0, 2, 10, 3, 9}, {0, 2, 9, 3, 8}, {0, 2, 7, 3, 6}, {0, 2, 5, 3, 4},
                               {1, 9, 4, 10, 4},  {1, 4, 4, 6, 4},  {1, 5, 7, 5, 7}, {1, 5, 2, 5, 2}, {1, 9, 2, 9, 2},
                               {1, 12, 2, 12, 2}, {0, 1, 13, 1, 13}, {0, 1, 6, 1, 6}, {0, 2, 4, 2, 4}, {1, 4, 7, 4, 7}, {1, 8, 4, 8, 4}};
  for (int row = 1; row <= 16; row++)
    for (int pos = 2; pos <= 3; pos++) {
      nr_td_tdra_t r;
      ASSERT_TRUE(nr_td_default_table_a(row, pos, &r));
      EXPECT_EQ(r.mapping, e[row - 1][0]) << row;
      EXPECT_EQ(r.S, e[row - 1][pos == 2 ? 1 : 3]) << row << " pos" << pos;
      EXPECT_EQ(r.L, e[row - 1][pos == 2 ? 2 : 4]) << row << " pos" << pos;
      EXPECT_EQ(r.k0, 0);
    }
  nr_td_tdra_t r;
  EXPECT_FALSE(nr_td_default_table_a(0, 2, &r));
  EXPECT_FALSE(nr_td_default_table_a(1, 4, &r));
}
TEST(TdOrder, DefaultTableAScoresMatchingHypothesis) {
  nr_td_side_info_t s = neutral(); s.w_default = 1; s.dmrs_typeA_pos = 2;
  const nr_pdsch_cfg_hypothesis_t a = H(2, 12, 0, 0), b = H(3, 11, 0, 0);
  EXPECT_GT(nr_td_ordering_score(&a, &s), nr_td_ordering_score(&b, &s));
}
TEST(TdOrder, ObservedMaskAgreementScores) {
  nr_td_side_info_t s = neutral(); s.w_obs = 1; s.obs_dmrs_mask = 0x804;
  nr_pdsch_cfg_hypothesis_t a = H(1, 13, 0, 0), b = H(1, 13, 0, 0); a.dmrs_mask = 0x804; b.dmrs_mask = 0x4;
  EXPECT_GT(nr_td_ordering_score(&a, &s), nr_td_ordering_score(&b, &s));
}
TEST(TdOrder, ObservedQmAgreementScores) {
  nr_td_side_info_t s = neutral(); s.w_obs = 1; s.obs_mcs = 20; s.obs_qm = nr_pdsch_qm_of_mcs(20, 0);
  nr_pdsch_cfg_hypothesis_t a = H(1, 13, 0, 0), b = H(1, 13, 0, 0); a.mcs_table = 0; b.mcs_table = 1;
  ASSERT_NE(nr_pdsch_qm_of_mcs(20, 0), nr_pdsch_qm_of_mcs(20, 1));
  EXPECT_GT(nr_td_ordering_score(&a, &s), nr_td_ordering_score(&b, &s));
}
TEST(TdOrder, PromotedFieldsScaleWithConfidence) {
  nr_td_side_info_t s = neutral(); s.w_field = 1; s.f_S = 1; s.f_L = 13; s.f_conf = 0.5f;
  const nr_pdsch_cfg_hypothesis_t a = H(1, 13, 0, 0);
  const float half = nr_td_ordering_score(&a, &s); s.f_conf = 1.0f;
  EXPECT_FLOAT_EQ(nr_td_ordering_score(&a, &s), 2 * half);
}
TEST(TdOrder, ScoreIsNeverNegative) {
  nr_td_side_info_t s = neutral(); s.w_sib1 = s.w_default = s.w_obs = s.w_field = 1;
  const nr_pdsch_cfg_hypothesis_t a = H(9, 4, 1, 1);
  EXPECT_GE(nr_td_ordering_score(&a, &s), 0.0f);
}

/* BC12a: SIB1 common-TDRA / default table A census predicates (log + metrics only) */
TEST(TdCensus, Sib1NoneWhenListEmpty) {
  const nr_pdsch_cfg_hypothesis_t h = H(1, 13, 0, 0);
  EXPECT_EQ(nr_td_census_sib1(NULL, 0, 0, &h), NR_TD_CENSUS_NONE);
}
TEST(TdCensus, Sib1MatchChecksSLMappingAndK0) {
  const nr_td_tdra_t l[2] = {{1, 13, 0, 0}, {2, 7, 1, 1}};
  nr_pdsch_cfg_hypothesis_t h = H(1, 13, 0, 0);
  EXPECT_EQ(nr_td_census_sib1(l, 2, 0, &h), NR_TD_CENSUS_MATCH);
  EXPECT_EQ(nr_td_census_sib1(l, 2, 1, &h), NR_TD_CENSUS_MISMATCH); /* row 1 is another row */
  h = H(1, 12, 0, 0); EXPECT_EQ(nr_td_census_sib1(l, 2, 0, &h), NR_TD_CENSUS_MISMATCH);
  h = H(2, 13, 0, 0); EXPECT_EQ(nr_td_census_sib1(l, 2, 0, &h), NR_TD_CENSUS_MISMATCH);
  h = H(1, 13, 1, 0); EXPECT_EQ(nr_td_census_sib1(l, 2, 0, &h), NR_TD_CENSUS_MISMATCH); /* type A vs B */
  h = H(1, 13, 0, 1); EXPECT_EQ(nr_td_census_sib1(l, 2, 0, &h), NR_TD_CENSUS_MISMATCH); /* k0 */
  h = H(2, 7, 1, 1);  EXPECT_EQ(nr_td_census_sib1(l, 2, 1, &h), NR_TD_CENSUS_MATCH);     /* type B, k0 = 1 */
}
TEST(TdCensus, Sib1IndexBeyondListIsMismatchNotNone) {
  const nr_td_tdra_t l[1] = {{1, 13, 0, 0}};
  const nr_pdsch_cfg_hypothesis_t h = H(1, 13, 0, 0);
  EXPECT_EQ(nr_td_census_sib1(l, 1, 3, &h), NR_TD_CENSUS_MISMATCH);
}
TEST(TdCensus, DefaultTableARowIsTdaIndexPlusOne) {
  nr_pdsch_cfg_hypothesis_t h = H(2, 12, 0, 0); /* row 1, pos 2 */
  EXPECT_EQ(nr_td_census_deftab(2, 0, &h), NR_TD_CENSUS_MATCH);
  EXPECT_EQ(nr_td_census_deftab(3, 0, &h), NR_TD_CENSUS_MISMATCH); /* pos 3 row 1 is S=3 L=11 */
  h = H(3, 11, 0, 0); EXPECT_EQ(nr_td_census_deftab(3, 0, &h), NR_TD_CENSUS_MATCH);
  h = H(9, 4, 1, 0);  EXPECT_EQ(nr_td_census_deftab(2, 5, &h), NR_TD_CENSUS_MATCH);  /* row 6 type B pos 2 */
  h = H(10, 4, 1, 0); EXPECT_EQ(nr_td_census_deftab(3, 5, &h), NR_TD_CENSUS_MATCH);  /* row 6 type B pos 3 */
  h = H(1, 13, 0, 0); EXPECT_EQ(nr_td_census_deftab(2, 11, &h), NR_TD_CENSUS_MATCH); /* row 12 */
  h = H(2, 12, 0, 1); EXPECT_EQ(nr_td_census_deftab(2, 0, &h), NR_TD_CENSUS_MISMATCH); /* default table k0 = 0 */
  h = H(2, 12, 0, 0); EXPECT_EQ(nr_td_census_deftab(2, 16, &h), NR_TD_CENSUS_MISMATCH); /* invalid row */
}
TEST(TdCensus, DefaultTableUnknownPosIsNone) {
  const nr_pdsch_cfg_hypothesis_t h = H(2, 12, 0, 0);
  EXPECT_EQ(nr_td_census_deftab(0, 0, &h), NR_TD_CENSUS_NONE);
}
TEST(TdCensus, Sib1StoreRoundTripAndClear) {
  nr_td_tdra_t in[2] = {{1, 13, 0, 0}, {2, 7, 1, 1}}, out[NR_TD_MAX_SIB1_TDRA];
  nr_td_sib1_store_set(in, 2);
  ASSERT_EQ(nr_td_sib1_store_get(out), 2);
  EXPECT_EQ(out[1].S, 2); EXPECT_EQ(out[1].L, 7); EXPECT_EQ(out[1].mapping, 1); EXPECT_EQ(out[1].k0, 1);
  nr_td_sib1_store_set(NULL, 0);
  EXPECT_EQ(nr_td_sib1_store_get(out), 0);
}
TEST(TdCensus, CounterIndexingByFormat) {
  nr_td_census_reset();
  nr_td_census_count(11, NR_TD_CENSUS_MATCH);
  nr_td_census_count(11, NR_TD_CENSUS_MATCH);
  nr_td_census_count(10, NR_TD_CENSUS_NONE);
  nr_td_census_count(0, NR_TD_CENSUS_MISMATCH); /* unknown format is its own bucket, never folded into 1_0/1_1 */
  uint64_t c[3][3];
  nr_td_census_get(c);
  EXPECT_EQ(c[NR_TD_FMT_11][NR_TD_CENSUS_MATCH], 2u);
  EXPECT_EQ(c[NR_TD_FMT_10][NR_TD_CENSUS_NONE], 1u);
  EXPECT_EQ(c[NR_TD_FMT_UNK][NR_TD_CENSUS_MISMATCH], 1u);
  EXPECT_EQ(c[NR_TD_FMT_10][NR_TD_CENSUS_MATCH], 0u);
}
