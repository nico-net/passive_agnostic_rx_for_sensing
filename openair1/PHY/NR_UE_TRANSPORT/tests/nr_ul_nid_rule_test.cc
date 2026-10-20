#include <gtest/gtest.h>
#include "nr_ul_nid_rule.h"

TEST(UlNidRule, RivDecodeMatchesTheReportedTwinsOfRiv21609)
{
  int s, l;
  ASSERT_TRUE(nr_ul_nid_riv_decode(21609, 217, &s, &l));
  EXPECT_EQ(90, s); EXPECT_EQ(119, l);
  ASSERT_TRUE(nr_ul_nid_riv_decode(21609, 214, &s, &l));
  EXPECT_EQ(4, s); EXPECT_EQ(115, l);
  ASSERT_TRUE(nr_ul_nid_riv_decode(21609, 215, &s, &l));
  EXPECT_EQ(109, s); EXPECT_EQ(101, l);
}

TEST(UlNidRule, RivRoundTripAndTwinsShareTheFdraWidth)
{
  for (int n : {24, 106, 217}) {
    for (int S = 0; S < n; S += 7)
      for (int L = 1; S + L <= n; L += 5) {
        const uint32_t riv = (L - 1 <= n / 2) ? n * (L - 1) + S : n * (n - L + 1) + (n - 1 - S);
        int s, l;
        ASSERT_TRUE(nr_ul_nid_riv_decode(riv, n, &s, &l));
        ASSERT_EQ(S, s); ASSERT_EQ(L, l);
      }
  }
  uint16_t n[16], s[16], l[16];
  const int k = nr_ul_nid_twins(21609, 217, 4, n, s, l, 16);
  ASSERT_GE(k, 3);
  EXPECT_EQ(217, n[0]);
  for (int i = 0; i < k; i++) {
    EXPECT_EQ(nr_ul_nid_riv_bits(217), nr_ul_nid_riv_bits(n[i]));
    EXPECT_LE(s[i] + l[i], n[i]);
  }
}

TEST(UlNidRule, StrongHitConfirmsAloneWeakNeedsASecondDifferentSlot)
{
  nr_ul_nid_pend_t p = {};
  EXPECT_FALSE(nr_ul_nid_confirm_rule(&p, false, 100));
  EXPECT_FALSE(nr_ul_nid_confirm_rule(&p, false, 100)); /* the same slot again is not independent */
  EXPECT_TRUE(nr_ul_nid_confirm_rule(&p, false, 120));
  nr_ul_nid_pend_t q = {};
  EXPECT_TRUE(nr_ul_nid_confirm_rule(&q, true, 5));
}
