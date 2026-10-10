#include <gtest/gtest.h>
extern "C" {
#include "nr_pdsch_retry_pred.h"
}
TEST(RetryPred, StreakThenMiss)
{
  const uint16_t r = 0x137a;
  EXPECT_FALSE(nr_retry_pred_try_first(r));
  nr_retry_pred_hit(r);
  EXPECT_FALSE(nr_retry_pred_try_first(r));
  nr_retry_pred_hit(r);
  EXPECT_TRUE(nr_retry_pred_try_first(r));
  nr_retry_pred_miss(r);
  EXPECT_FALSE(nr_retry_pred_try_first(r));
}
TEST(RetryPred, CollidingRntiDoesNotInherit)
{
  const uint16_t a = 0x0101, b = a + NR_RETRY_PRED_SLOTS;
  nr_retry_pred_hit(a);
  nr_retry_pred_hit(a);
  EXPECT_TRUE(nr_retry_pred_try_first(a));
  EXPECT_FALSE(nr_retry_pred_try_first(b));
  nr_retry_pred_miss(b); /* must not clear a's streak */
  EXPECT_TRUE(nr_retry_pred_try_first(a));
  nr_retry_pred_hit(b); /* takes the slot over from a, with its own streak of 1 */
  EXPECT_FALSE(nr_retry_pred_try_first(b));
  EXPECT_FALSE(nr_retry_pred_try_first(a));
}
