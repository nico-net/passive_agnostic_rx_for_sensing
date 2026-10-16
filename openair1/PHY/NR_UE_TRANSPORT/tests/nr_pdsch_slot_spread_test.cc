#include <gtest/gtest.h>
extern "C" {
#include "nr_pdsch_slot_spread.h"
}

TEST(SlotSpread, GiveOnePerIdleKeepsOne)
{
  EXPECT_EQ(nr_slot_spread_give(4, 0), 0);
  EXPECT_EQ(nr_slot_spread_give(4, 2), 2);
  EXPECT_EQ(nr_slot_spread_give(4, 9), 3);
  EXPECT_EQ(nr_slot_spread_give(1, 3), 0);
}

TEST(SlotSpread, EveryMemberExactlyOnceSmallestFirst)
{
  const uint64_t cost[5] = {900, 10, 500, 10, 70};
  int ord[5];
  nr_slot_spread_order(cost, 5, ord);
  const int want[5] = {1, 3, 4, 2, 0}; /* stable: equal costs keep arrival order */
  for (int i = 0; i < 5; i++) EXPECT_EQ(ord[i], want[i]);
  for (int idle = 0; idle < 7; idle++) {
    const int give = nr_slot_spread_give(5, idle), keep = 5 - give;
    int seen[5] = {0};
    for (int k = 0; k < 5; k++) seen[ord[k]]++; /* kept = ord[0..keep), given = ord[keep..5) */
    for (int i = 0; i < 5; i++) EXPECT_EQ(seen[i], 1);
    EXPECT_GE(keep, 1);
    if (give) EXPECT_EQ(ord[4], 0); /* the heaviest is always given away first */
  }
}
