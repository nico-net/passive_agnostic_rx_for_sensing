#include <gtest/gtest.h>
extern "C" {
#include "nr_pdsch_retry_gate.h"
}
TEST(RetryGate, WarmupThenRate)
{
  EXPECT_TRUE(nr_retry_gate_pass(255, 0, 1));    /* warm-up: always tried */
  EXPECT_TRUE(nr_retry_gate_pass(1000, 20, 1));  /* 2 %: worth it */
  EXPECT_FALSE(nr_retry_gate_pass(1000, 19, 1)); /* 1.9 %: gated */
  EXPECT_TRUE(nr_retry_gate_pass(1000, 0, 32));  /* exploration: 1 in 16 */
  int n = 0;
  for (uint64_t s = 0; s < 160; s++) n += nr_retry_gate_pass(12097, 12, s);
  EXPECT_EQ(n, 10); /* on-air TIM arm statistics: 1 in 16 chains */
}
