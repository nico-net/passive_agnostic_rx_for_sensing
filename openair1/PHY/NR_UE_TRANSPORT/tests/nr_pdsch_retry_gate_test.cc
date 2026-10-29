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

/* mpci audit: every item of the (process-wide) check queue runs as the cell that pushed it, whatever cell started the queue or ran the worker before. */
static int g_seen[8], g_nseen;
static int g_lease_cell[8], g_nlease;
static void bgq_cell_item(void *) { g_seen[g_nseen++] = nr_cell_idx(); }
static void bgq_lease_in(int cell) { g_lease_cell[g_nlease++] = cell; }
static void bgq_lease_out(void) {}
TEST(GpuCheckQueue, ItemRunsAsItsCell)
{
  nr_bgq_t q;
  nr_bgq_init(&q, 4);
  q.lease_in = bgq_lease_in;
  q.lease_out = bgq_lease_out;
  q.cell = 0; /* the starter of the queue is cell 0 */
  g_nseen = g_nlease = 0;
  int dummy = 0;
  nr_cell_bind(1);
  EXPECT_EQ(nr_bgq_push(&q, bgq_cell_item, &dummy), 0);
  nr_cell_bind(0);
  EXPECT_EQ(nr_bgq_push(&q, bgq_cell_item, &dummy), 0);
  nr_cell_bind(3);
  EXPECT_EQ(nr_bgq_push(&q, bgq_cell_item, &dummy), 0);
  nr_cell_bind(0); /* the worker thread starts unbound */
  for (int k = 0; k < 3; k++) nr_bgq_run_one(&q);
  ASSERT_EQ(g_nseen, 3);
  EXPECT_EQ(g_seen[0], 1);
  EXPECT_EQ(g_seen[1], 0);
  EXPECT_EQ(g_seen[2], 3);
  EXPECT_EQ(g_lease_cell[0], 1); /* the lease is the item's cell, not the starter's */
  EXPECT_EQ(g_lease_cell[1], 0);
  EXPECT_EQ(g_lease_cell[2], 3);
  nr_cell_bind(0);
}
