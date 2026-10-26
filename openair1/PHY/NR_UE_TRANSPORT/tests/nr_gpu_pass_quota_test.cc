// MPCI G3: the per-cell instance quota (nr_gpu_pass_quota.h), pure logic. [OFFLINE VERIFIED when run, DGX GB10 aarch64 unit]
#include <gtest/gtest.h>
#include "nr_gpu_pass_quota.h"
extern "C" {
#include "common/utils/nr_cell.h"
}
static_assert(NR_PQ_CELLS == NR_CELL_MAX, "the quota table covers every cell");

// takes while allowed; returns how many
static int burst(nr_pass_quota_t &q, int cls, int cell, int &free_n)
{
  int n = 0;
  while (nr_pq_may_take(&q, cls, cell, free_n)) {
    nr_pq_take(&q, cls, cell);
    free_n--;
    n++;
  }
  return n;
}

TEST(PassQuota, OneCellSeesTheWholePool)
{
  nr_pass_quota_t q;
  nr_pq_reset(&q);
  nr_pq_register(&q, 0);
  EXPECT_FALSE(nr_pq_active(&q));
  int fr = 4;
  EXPECT_EQ(4, burst(q, 0, 0, fr));
}

TEST(PassQuota, BurstCellCannotTakeLastInstanceOfQuietCell)
{
  for (int pool : {2, 4, 7, 12}) { // pool sizes come from the caller, no constant of this header
    nr_pass_quota_t q;
    nr_pq_reset(&q);
    nr_pq_register(&q, 0);
    nr_pq_register(&q, 1);
    ASSERT_TRUE(nr_pq_active(&q));
    int fr = pool;
    EXPECT_EQ(pool - 1, burst(q, 0, 0, fr)) << "pool " << pool;
    EXPECT_EQ(1, fr);
    EXPECT_TRUE(nr_pq_may_take(&q, 0, 1, fr)) << "the quiet cell's floor";
    nr_pq_take(&q, 0, 1);
    fr--;
    EXPECT_TRUE(nr_pq_may_take(&q, 0, 0, 1)) << "once the quiet cell holds its floor nothing is reserved any more";
  }
}

TEST(PassQuota, FloorOfOnePerCellAndWorkConserving)
{
  nr_pass_quota_t q;
  nr_pq_reset(&q);
  for (int c = 0; c < 3; c++) nr_pq_register(&q, c);
  int fr = 5;
  EXPECT_EQ(3, burst(q, 0, 0, fr)) << "5 instances, 3 cells: the burst cell leaves one for each of the other two";
  EXPECT_TRUE(nr_pq_may_take(&q, 0, 1, fr));
  EXPECT_TRUE(nr_pq_may_take(&q, 0, 2, fr));
  nr_pq_take(&q, 0, 1);
  fr--;
  EXPECT_TRUE(nr_pq_may_take(&q, 0, 2, fr));
  // cell 1 now holds one: with one free instance left for cell 2 it may not take it
  EXPECT_FALSE(nr_pq_may_take(&q, 0, 1, fr));
  nr_pq_unregister(&q, 2); // cell 2 leaves: its reservation disappears (work conserving)
  EXPECT_TRUE(nr_pq_may_take(&q, 0, 1, fr));
  nr_pq_release(&q, 0, 0);
  nr_pq_release(&q, 0, 0);
  nr_pq_release(&q, 0, 0);
  nr_pq_release(&q, 0, 0); // over-release saturates
  EXPECT_EQ(0, q.held[0][0]);
}

TEST(PassQuota, ClassesAreIndependentAndUnregisteredCellsAreFree)
{
  nr_pass_quota_t q;
  nr_pq_reset(&q);
  nr_pq_register(&q, 0);
  nr_pq_register(&q, 1);
  int fr = 2;
  EXPECT_EQ(1, burst(q, 1, 0, fr)) << "UL pool of 2: one each";
  fr = 4;
  EXPECT_EQ(3, burst(q, 0, 0, fr)) << "the DL class is counted separately";
  EXPECT_TRUE(nr_pq_may_take(&q, 0, 9, 1)) << "a cell nobody registered has no claim on the floors";
  EXPECT_FALSE(nr_pq_may_take(&q, 0, 0, 0));
  EXPECT_FALSE(nr_pq_may_take(&q, 5, 0, 3));
  EXPECT_EQ(-1, nr_pq_register(&q, NR_PQ_CELLS));
}
