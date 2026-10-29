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

/* mpci audit: the memory is per cell. The same RNTI in two cells never shares a streak (interleaved cells, one thread re-binding). */
TEST(RetryPred, TwoCellsInterleaved)
{
  const uint16_t r = 0x0abc;
  nr_cell_bind(0);
  nr_retry_pred_hit(r);
  nr_cell_bind(1);
  EXPECT_FALSE(nr_retry_pred_try_first(r)); /* cell 0's first hit is invisible here */
  nr_retry_pred_hit(r);
  nr_retry_pred_hit(r);
  EXPECT_TRUE(nr_retry_pred_try_first(r));  /* cell 1 reached the streak */
  nr_cell_bind(0);
  EXPECT_FALSE(nr_retry_pred_try_first(r)); /* cell 0 has one hit only */
  nr_retry_pred_hit(r);
  EXPECT_TRUE(nr_retry_pred_try_first(r));
  nr_retry_pred_miss(r);                    /* cell 0 miss ... */
  nr_cell_bind(1);
  EXPECT_TRUE(nr_retry_pred_try_first(r));  /* ... leaves cell 1's streak intact */
  nr_cell_bind(0);
}
