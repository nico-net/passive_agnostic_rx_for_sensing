// SPDX-License-Identifier: LicenseRef-CSSL-1.0
// mpci B5: lease rules of the float lane (discovery / background workers) in the site pool. A separate binary: it injects the site pool.
#include <gtest/gtest.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <unistd.h>
extern "C" {
#include "nr_core_pool.h"
}

static nr_pool_t *mk(int n)
{
  nr_pool_cfg_t c = nr_pool_cfg_default_n(n);
  c.pin = false;
  static int cores[NR_POOL_MAX_TOKENS];
  for (int i = 0; i < n; i++) cores[i] = i;
  return nr_pool_create(cores, nullptr, n, &c);
}
static void wait_until(const std::function<bool()> &f)
{
  for (int i = 0; i < 20000 && !f(); i++) usleep(500);
  ASSERT_TRUE(f()) << "rendezvous timed out";
}
struct SiteGuard {
  nr_pool_t *p;
  explicit SiteGuard(int n) : p(mk(n)) { nr_pool_site_set_for_test(p); }
  ~SiteGuard() { nr_pool_site_set_for_test(nullptr); nr_pool_destroy(p); }
};

TEST(FloatLane, MergeRuleIsAFunctionOfTheTokenCountOnly)
{
  for (int t = 0; t < NR_POOL_FLOAT_MERGE_MIN_TOKENS; t++) EXPECT_FALSE(nr_pool_float_merged_n(t)) << t;
  for (int t = NR_POOL_FLOAT_MERGE_MIN_TOKENS; t <= NR_POOL_MAX_TOKENS; t++) EXPECT_TRUE(nr_pool_float_merged_n(t)) << t;
}

TEST(FloatLane, SmallPoolKeepsTheDedicatedLane)
{
  SiteGuard g(4);
  EXPECT_FALSE(nr_pool_site_float_merged());
  EXPECT_EQ(nr_pool_site_float_acquire(NR_POOL_DISC, 0, 0), -1);
  EXPECT_FALSE(nr_pool_holds(g.p));
  EXPECT_FALSE(nr_pool_site_float_yield());
  nr_pool_site_float_release(); // no-op
  nr_pool_stats_t st;
  nr_pool_stats(g.p, &st);
  for (int c = 0; c < NR_POOL_NPRIO; c++) EXPECT_EQ(st.leases[c], 0u);
}

TEST(FloatLane, MergedLeaseIsCountedPerClassAndCell)
{
  SiteGuard g(6);
  ASSERT_TRUE(nr_pool_site_float_merged());
  EXPECT_GE(nr_pool_site_float_acquire(NR_POOL_DISC, 2, 0), 0);
  EXPECT_TRUE(nr_pool_holds(g.p));
  nr_pool_site_float_release();
  EXPECT_FALSE(nr_pool_holds(g.p));
  EXPECT_GE(nr_pool_site_float_acquire(NR_POOL_HOUSE, 1, 0), 0);
  nr_pool_site_float_release();
  // a float never leases above DISC: a stray class is lifted to HOUSE, not to DECODE
  EXPECT_GE(nr_pool_site_float_acquire(NR_POOL_SCAN, 1, 0), 0);
  nr_pool_site_float_release();
  nr_pool_stats_t st;
  nr_pool_stats(g.p, &st);
  EXPECT_EQ(st.leases[NR_POOL_DISC], 1u);
  EXPECT_EQ(st.leases[NR_POOL_HOUSE], 2u);
  EXPECT_EQ(st.leases[NR_POOL_SCAN], 0u);
  EXPECT_EQ(st.leases_cell[2], 1u);
  EXPECT_EQ(st.leases_cell[1], 2u);
  EXPECT_EQ(st.nested, 0u);
}

// A worker shaped like janon / USS / dmrsScan: cond wait for a job, lease around the job only.
struct Worker {
  std::mutex m;
  std::condition_variable cv;
  std::deque<int> q;
  bool stop = false;
  std::atomic<int> done{0}, held_across_wait{0};
};
static void worker(Worker *w, int cell, int prio)
{
  for (;;) {
    {
      std::unique_lock<std::mutex> l(w->m);
      w->cv.wait(l, [&] { return !w->q.empty() || w->stop; });
      if (w->q.empty()) return;
      w->q.pop_front();
    }
    nr_pool_site_float_acquire(prio, cell, 0);
    usleep(100); // the job
    nr_pool_site_float_release();
    w->done++;
    // after the release and before the next cond wait the thread must hold nothing
    if (nr_pool_holds(nr_pool_site())) w->held_across_wait++;
  }
}

TEST(FloatLane, NCellsOfWorkersLeasePerJobNeverAcrossTheWait)
{
  for (int cells = 1; cells <= 3; cells++) {
    SiteGuard g(6);
    const int per_cell = 2, jobs = 60;
    std::vector<std::unique_ptr<Worker>> ws;
    std::vector<std::thread> th;
    for (int c = 0; c < cells; c++)
      for (int k = 0; k < per_cell; k++) {
        ws.emplace_back(new Worker);
        th.emplace_back(worker, ws.back().get(), c, k ? NR_POOL_HOUSE : NR_POOL_DISC);
      }
    for (auto &w : ws)
      for (int j = 0; j < jobs; j++) { std::lock_guard<std::mutex> l(w->m); w->q.push_back(j); w->cv.notify_one(); }
    for (auto &w : ws) wait_until([&] { return w->done == jobs; });
    for (auto &w : ws) { std::lock_guard<std::mutex> l(w->m); w->stop = true; w->cv.notify_all(); }
    for (auto &t : th) t.join();
    nr_pool_stats_t st;
    nr_pool_stats(g.p, &st);
    EXPECT_EQ(st.leases[NR_POOL_DISC], (uint64_t)cells * jobs);
    EXPECT_EQ(st.leases[NR_POOL_HOUSE], (uint64_t)cells * jobs);
    for (int c = 0; c < cells; c++) EXPECT_EQ(st.leases_cell[c], 2u * jobs) << "cell " << c;
    for (auto &w : ws) EXPECT_EQ(w->held_across_wait.load(), 0);
    EXPECT_EQ(st.nested, 0u);
  }
}

// ulBgDisc: one long HOUSE job chunked with nr_pool_site_float_yield(): a more urgent waiter gets the token at the next step, the job continues after.
TEST(FloatLane, ChunkedHouseJobYieldsToAnUrgentWaiter)
{
  SiteGuard g(5);
  // fill the pool: 4 DISC holders + this thread (HOUSE)
  std::atomic<bool> release_disc{false};
  std::atomic<int> disc_in{0};
  std::vector<std::thread> holders;
  for (int i = 0; i < 4; i++)
    holders.emplace_back([&] {
      nr_pool_site_float_acquire(NR_POOL_DISC, 0, 0);
      disc_in++;
      while (!release_disc) usleep(200);
      nr_pool_site_float_release();
    });
  wait_until([&] { return disc_in == 4; });
  ASSERT_GE(nr_pool_site_float_acquire(NR_POOL_HOUSE, 0, 0), 0);
  EXPECT_FALSE(nr_pool_site_float_yield()); // nobody waits
  std::atomic<bool> urgent_done{false};
  std::thread urgent([&] {
    nr_pool_acquire(g.p, NR_POOL_DECODE, 0, 0);
    urgent_done = true;
    nr_pool_release(g.p);
  });
  wait_until([&] { return nr_pool_total_waiting(g.p) >= 1; });
  EXPECT_TRUE(nr_pool_site_float_yield()); // step boundary: the token goes to the decode waiter, the job re-acquires
  wait_until([&] { return urgent_done.load(); });
  EXPECT_TRUE(nr_pool_holds(g.p)); // re-acquired after the yield
  nr_pool_site_float_release();
  release_disc = true;
  urgent.join();
  for (auto &t : holders) t.join();
}
