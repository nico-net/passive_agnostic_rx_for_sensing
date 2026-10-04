#include <gtest/gtest.h>
extern "C" {
#include "nr_passive_sample_lifetime.h"
}
TEST(Lifetime, CreditAllowedOnlyWhileSamplesValid)
{
  EXPECT_TRUE(nr_passive_credit_allowed(100, 99, 20));
  EXPECT_FALSE(nr_passive_credit_allowed(100, 99 - 18, 20)); /* spf - 2 = 18 slots: overwritten */
  EXPECT_FALSE(nr_passive_credit_allowed(98, 99, 20));       /* producer behind job: invalid */
}
TEST(Lifetime, CreditAllowedMatchesSamplesValid)
{
  for (long p = 90; p < 130; p++)
    for (long s = 80; s < 130; s++)
      EXPECT_EQ(nr_passive_credit_allowed(p, s, 20), nr_passive_samples_valid(p, s, 20));
}
