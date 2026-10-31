#include <gtest/gtest.h>
extern "C" {
#include "nr_ul_tp_rules.h"
#include "nr_ul_ta_rules.h"
}

TEST(UlTpRules, LegalWidthsAreTheSmoothNumbers)
{
  const int legal[] = {1, 2, 3, 4, 5, 6, 8, 9, 10, 12, 15, 16, 18, 20, 24, 25, 27, 30, 32, 36, 40, 45, 48, 50, 54, 60, 64, 72, 75, 80, 81, 90, 96, 100, 108, 120, 125, 135, 144, 150, 162, 180, 192, 200, 216, 225, 240, 243, 250, 256, 270};
  for (int n : legal) EXPECT_TRUE(nr_ul_tp_nrb_legal(n)) << n;
  /* the widths TIM's refuted readings carried (46930 attempts): 77 78 79 82 88 94 101 106 109 131 141 142 */
  const int illegal[] = {0, -3, 7, 11, 13, 77, 78, 79, 82, 88, 94, 101, 106, 109, 131, 141, 142, 217};
  for (int n : illegal) EXPECT_FALSE(nr_ul_tp_nrb_legal(n)) << n;
}

TEST(UlTpRules, TailRefutationNeedsRepeatedIllegalWidths)
{
  EXPECT_FALSE(nr_ul_tp_tail_refuted(0, 0));
  EXPECT_FALSE(nr_ul_tp_tail_refuted(3, 0));   /* a few false DCIs are not a refutation */
  EXPECT_TRUE(nr_ul_tp_tail_refuted(4, 1));    /* CP-OFDM cell read as TP: ~70 % illegal */
  EXPECT_FALSE(nr_ul_tp_tail_refuted(4, 40));  /* a true TP cell: legal widths dominate */
  EXPECT_TRUE(nr_ul_tp_tail_refuted(30, 12));
}

TEST(UlTaRules, SeedIsTheMidpointOfTheTwoGeometries)
{
  EXPECT_EQ(nr_ul_ta_seed(1600, -1), 1600);
  EXPECT_EQ(nr_ul_ta_seed(1600, 0), 1600);
  EXPECT_EQ(nr_ul_ta_seed(1600, 400), 1800); /* gNB: 1600, UE: 2000 */
  EXPECT_EQ(nr_ul_ta_seed(400, 101), 450);
}

TEST(UlTaRules, NominalAdvanceFollowsSib1TimingAdvanceOffset)
{
  const uint32_t absent = 0xFFFFFFFFu;
  EXPECT_EQ(nr_ul_nta_offset_tc(39936, true, 25600), 39936); /* LTE-NR coexistence */
  EXPECT_EQ(nr_ul_nta_offset_tc(25600, true, 25600), 25600);
  EXPECT_EQ(nr_ul_nta_offset_tc(0, true, 25600), 0);          /* n0 is a real SIB1 value once the configuration is applied */
  EXPECT_EQ(nr_ul_nta_offset_tc(0, false, 25600), 25600);     /* a zeroed, never-applied config is not n0 */
  EXPECT_EQ(nr_ul_nta_offset_tc(absent, true, 25600), 25600); /* field absent: the FR1 TDD default */
  EXPECT_EQ(nr_ul_nta_offset_tc(12345, true, 25600), 25600);  /* not an ASN.1 value */
  /* 122.88 Msps: 122880 samples per 1 ms subframe */
  EXPECT_EQ(nr_ul_nta_tc_to_samples(25600, 122880), 1600);
  EXPECT_EQ(nr_ul_nta_tc_to_samples(39936, 122880), 2496);
  EXPECT_EQ(nr_ul_nta_tc_to_samples(0, 122880), 0);
}

extern "C" {
#include "nr_ul_trial_variant.h"
}

TEST(UlTdaRows, RowsAreLegalAndDistinctAndKeepTheCore)
{
  EXPECT_EQ(nr_ul_tda_row(0).l, 14);
  EXPECT_EQ(nr_ul_tda_row(1).l, 13); /* the pre-existing second row: S0 L13 type B */
  EXPECT_EQ(nr_ul_tda_row(1).map, 1);
  for (int i = 0; i < NR_UL_TDA_ROWS; i++) {
    const nr_ul_tda_row_t a = nr_ul_tda_row(i);
    EXPECT_TRUE(nr_ul_tda_legal(a.s, a.l, a.map)) << i;
    for (int j = 0; j < i; j++) {
      const nr_ul_tda_row_t b = nr_ul_tda_row(j);
      EXPECT_FALSE(a.s == b.s && a.l == b.l && a.map == b.map) << i << " " << j;
    }
  }
  /* TS 38.214 Table 6.1.2.1.1-2 rows that are in the set: A 0/12, A 0/10, B 2/10, B 4/10 */
  bool have[4] = {false};
  for (int i = 0; i < NR_UL_TDA_ROWS; i++) {
    const nr_ul_tda_row_t r = nr_ul_tda_row(i);
    have[0] |= r.s == 0 && r.l == 12 && r.map == 0;
    have[1] |= r.s == 0 && r.l == 10 && r.map == 0;
    have[2] |= r.s == 2 && r.l == 10 && r.map == 1;
    have[3] |= r.s == 4 && r.l == 10 && r.map == 1;
  }
  for (bool h : have) EXPECT_TRUE(h);
  /* L = 13 type A (SRS-aware) is there */
  bool l13a = false;
  for (int i = 0; i < NR_UL_TDA_ROWS; i++) l13a |= nr_ul_tda_row(i).l == 13 && nr_ul_tda_row(i).map == 0;
  EXPECT_TRUE(l13a);
  EXPECT_FALSE(nr_ul_tda_legal(2, 12, 0)); /* type A starts at symbol 0 */
  EXPECT_FALSE(nr_ul_tda_legal(8, 8, 1));  /* S + L > 14 */
}

TEST(UlTdaRows, VariantCycleKeepsTheCoreFirstAndCoversEveryRow)
{
  for (int tp = 0; tp <= 1; tp++) {
    const int n2 = nr_ul_variant_count_rows(true, true, 2, tp), n8 = nr_ul_variant_count_rows(true, true, NR_UL_TDA_ROWS, tp);
    EXPECT_EQ(n2, nr_ul_variant_count(true, true, true, tp)); /* the two-row cycle is the former one */
    EXPECT_EQ(n8, n2 / 2 * NR_UL_TDA_ROWS);
    bool seen[NR_UL_TDA_ROWS] = {false};
    for (int v = 0; v < n8; v++) {
      int tbl, k2d, tda, tbl2, k2d2, tda2;
      nr_ul_variant_get_rows(v, true, true, NR_UL_TDA_ROWS, tp, &tbl, &k2d, &tda);
      seen[tda] = true;
      if (v < n2) { /* the first n2 variants are exactly the former ones */
        nr_ul_variant_get(v, true, true, true, tp, &tbl2, &k2d2, &tda2);
        EXPECT_EQ(tbl, tbl2); EXPECT_EQ(k2d, k2d2); EXPECT_EQ(tda, tda2);
      }
    }
    for (bool s : seen) EXPECT_TRUE(s);
  }
}

TEST(UlTdaRows, LiveTrialsSpendThreeCyclesInFourOnTheCore)
{
  int core = 0, ext = 0;
  bool seen[NR_UL_TDA_ROWS] = {false};
  for (uint32_t round = 0; round < 96; round++)
    for (int base = 0; base < 2; base++) {
      const int t = nr_ul_variant_tda_round(round, base);
      seen[t] = true;
      (t < 2 ? core : ext)++;
      if ((round & 3) != 3) EXPECT_EQ(t, base);
    }
  EXPECT_EQ(core, 3 * ext);
  for (bool s : seen) EXPECT_TRUE(s) << "every row is reached";
}
