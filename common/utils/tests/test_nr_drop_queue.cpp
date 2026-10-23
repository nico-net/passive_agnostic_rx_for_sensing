// Drop-oldest discovery job queue + the DISC worker pattern of nr_pdcch_blind_monitor_rt.c (disc_worker): the producer (a SCAN lessee)
// never blocks and the job runs on another thread under a NR_POOL_DISC lease, so the pool's `nested` stays 0.
#include <gtest/gtest.h>
#include <atomic>
#include <thread>
extern "C" {
#include "nr_core_pool.h"
#include "nr_drop_queue.h"
}
struct Job { int id; };

TEST(DropQueue, DropsOldestAndStaysBounded)
{
  static nr_dq_t q = NR_DQ_INIT;
  int dropped = 0;
  for (int i = 0; i < NR_DQ_CAP + 5; i++) {
    Job j{i};
    dropped += nr_dq_push(&q, &j, sizeof(j));
  }
  EXPECT_EQ(dropped, 5);
  EXPECT_EQ(q.drops, 5u);
  EXPECT_EQ(q.n, NR_DQ_CAP);
  Job j;
  nr_dq_pop(&q, &j, sizeof(j));
  EXPECT_EQ(j.id, 5); // jobs 0..4 were dropped
}

TEST(DropQueue, JobRunsOnWorkerUnderDiscLeaseAndNestedStaysZero)
{
  static nr_dq_t q = NR_DQ_INIT;
  nr_pool_cfg_t cfg = nr_pool_cfg_default();
  cfg.pin = false;
  const int cores[3] = {0, 1, 2};
  nr_pool_t *p = nr_pool_create(cores, NULL, 3, &cfg);
  std::atomic<int> ran{0}, under_disc{0};
  std::atomic<std::thread::id> worker_id{};
  std::thread w([&] {
    worker_id = std::this_thread::get_id();
    for (;;) {
      Job j;
      nr_dq_pop(&q, &j, sizeof(j));
      if (j.id < 0) return;
      nr_pool_acquire(p, NR_POOL_DISC, 0, 0);
      under_disc += nr_pool_holds(p) ? 1 : 0;
      ran++;
      nr_pool_release(p);
    }
  });
  nr_pool_acquire(p, NR_POOL_SCAN, 0, 0); // the scan consumer holds its lease while it submits: push must not block
  for (int i = 0; i < 20; i++) {
    Job j{i};
    nr_dq_push(&q, &j, sizeof(j));
  }
  nr_pool_release(p);
  Job stop{-1};
  nr_dq_push(&q, &stop, sizeof(stop));
  w.join();
  nr_pool_stats_t s;
  nr_pool_stats(p, &s);
  EXPECT_EQ(ran.load(), under_disc.load());
  EXPECT_GT(ran.load(), 0);
  EXPECT_LE(ran.load(), 20);
  EXPECT_EQ(s.leases[NR_POOL_DISC], (uint64_t)ran.load());
  EXPECT_EQ(s.leases[NR_POOL_SCAN], 1u);
  EXPECT_EQ(s.nested, 0u);
  nr_pool_destroy(p);
}
