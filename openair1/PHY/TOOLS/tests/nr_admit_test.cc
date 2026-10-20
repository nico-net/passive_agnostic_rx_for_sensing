#include <gtest/gtest.h>
extern "C" {
#include "nr_admit.h"
}
// Admission control is off: every class runs, even with a deadline already passed and a token-less busy lane.
TEST(Admit, AlwaysRuns)
{
  EXPECT_EQ(0, nr_admit_on());
  EXPECT_EQ(1, nr_admit_try(NR_ADMIT_PROBE, 3000, 2000, 0));
  EXPECT_EQ(1, nr_admit_try(NR_ADMIT_JANON, 1000, 2000, 1));
}
