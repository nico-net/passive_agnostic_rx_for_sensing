#include <gtest/gtest.h>
#include <cmath>
extern "C" {
#include "nr_fep_cache.h"
}
TEST(FepCache, ScorerAndTapShareOneFepPerSlotSymbol)
{
  nr_fep_cache_t c = NR_FEP_CACHE_INIT;
  int iq;
  EXPECT_TRUE(nr_fep_cache_claim(&c, 100, &iq, 0.0, 2));   // scorer
  EXPECT_FALSE(nr_fep_cache_claim(&c, 100, &iq, 0.0, 2));  // tap, same (slot, symbol)
  EXPECT_EQ(c.n_fep, 1u);
  EXPECT_TRUE(nr_fep_cache_claim(&c, 100, &iq, 0.0, 3));   // another symbol of the slot: its own FEP
  EXPECT_FALSE(nr_fep_cache_claim(&c, 100, &iq, 0.0, 3));
  EXPECT_EQ(c.n_fep, 2u);
}
TEST(FepCache, TapFirstThenScorerSharesToo)
{
  nr_fep_cache_t c = NR_FEP_CACHE_INIT;
  int iq;
  EXPECT_TRUE(nr_fep_cache_claim(&c, 7, &iq, NAN, 5));
  EXPECT_FALSE(nr_fep_cache_claim(&c, 7, &iq, NAN, 5));
  EXPECT_EQ(c.n_fep, 1u);
}
TEST(FepCache, NewSlotIqOrFoDropsTheCache)
{
  nr_fep_cache_t c = NR_FEP_CACHE_INIT;
  int iq1, iq2;
  EXPECT_TRUE(nr_fep_cache_claim(&c, 1, &iq1, 10.0, 2));
  EXPECT_TRUE(nr_fep_cache_claim(&c, 2, &iq1, 10.0, 2));   // next slot
  EXPECT_TRUE(nr_fep_cache_claim(&c, 2, &iq2, 10.0, 2));   // other IQ buffer
  EXPECT_TRUE(nr_fep_cache_claim(&c, 2, &iq2, 11.0, 2));   // other de-rotation frequency
  EXPECT_FALSE(nr_fep_cache_claim(&c, 2, &iq2, 11.0, 2));
  EXPECT_EQ(c.n_fep, 4u);
}
