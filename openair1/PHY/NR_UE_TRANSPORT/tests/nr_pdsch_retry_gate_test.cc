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

/* FATTAILS: the bounded, never-blocking hand-off of the GPU self-checks (nr_gpu_bgq.h) */
#include "../../TOOLS/nr_gpu_bgq.h"
static int g_ran;
static void bgq_item(void *arg) { g_ran += *(int *)arg; }
TEST(GpuCheckQueue, BoundedDropsAndRuns)
{
  nr_bgq_t q;
  nr_bgq_init(&q, 2);
  int one = 1, ten = 10, hundred = 100;
  EXPECT_TRUE(nr_bgq_room(&q));
  EXPECT_EQ(nr_bgq_push(&q, bgq_item, &one), 0);
  EXPECT_EQ(nr_bgq_push(&q, bgq_item, &ten), 0);
  EXPECT_FALSE(nr_bgq_room(&q));
  EXPECT_EQ(nr_bgq_push(&q, bgq_item, &hundred), -1); /* full: dropped, counted */
  EXPECT_EQ(q.dropped, 1u);
  g_ran = 0;
  nr_bgq_run_one(&q);
  nr_bgq_run_one(&q);
  EXPECT_EQ(g_ran, 11); /* FIFO, both ran, the dropped one never */
  EXPECT_EQ(q.done, 2u);
  EXPECT_EQ(q.queued, 2u);
  EXPECT_TRUE(nr_bgq_room(&q));
  for (int k = 0; k < 20; k++) { /* wrap-around of the ring */
    EXPECT_EQ(nr_bgq_push(&q, bgq_item, &one), 0);
    nr_bgq_run_one(&q);
  }
  EXPECT_EQ(g_ran, 31);
  pthread_mutex_lock(&q.mu); /* a producer that finds the lock held never waits */
  EXPECT_EQ(nr_bgq_push(&q, bgq_item, &one), -1);
  pthread_mutex_unlock(&q.mu);
  EXPECT_EQ(q.dropped, 2u);
}
static void bgq_slow(void *arg)
{
  nr_bgq_t *q = (nr_bgq_t *)arg;
  EXPECT_EQ(q->run, 1u); /* the item in progress holds its slot */
  EXPECT_FALSE(nr_bgq_room(q));
}
TEST(GpuCheckQueue, InProgressCountsAsBusy)
{
  nr_bgq_t q;
  nr_bgq_init(&q, 1);
  EXPECT_EQ(nr_bgq_push(&q, bgq_slow, &q), 0);
  nr_bgq_run_one(&q);
  EXPECT_TRUE(nr_bgq_room(&q));
}
