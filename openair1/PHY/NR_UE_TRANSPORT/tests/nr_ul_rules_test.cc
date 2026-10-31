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
