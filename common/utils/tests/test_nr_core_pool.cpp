// SPDX-License-Identifier: LicenseRef-CSSL-1.0
// Scheduler tests for the shared core pool (nr_core_pool): order, fairness, reserve, aging, exclusivity, bounded wait.
// Pinning is off (cfg.pin=false) so the tests run on any host; the bench pins to cores 0-4.
#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <unistd.h>
extern "C" {
#include "nr_core_pool.h"
}

static nr_pool_t *mk(int n, int reserve = 0, bool no_age = true)
{
  nr_pool_cfg_t c = nr_pool_cfg_default();
  c.pin = false;
  c.reserve = reserve;
  if (no_age) for (auto &a : c.age_ns) a = 0;
  static int cores[NR_POOL_MAX_TOKENS];
  for (int i = 0; i < n; i++) cores[i] = i;
  return nr_pool_create(cores, nullptr, n, &c);
}
static void settle() { usleep(30000); }

// Holder owns the only token; waiters {prio, cell, deadline} queue up one by one; the grant order is the result.
struct W { int prio, cell; uint64_t dl; int id; };
static std::vector<int> grant_order(nr_pool_t *p, const std::vector<W> &ws)
{
  nr_pool_acquire(p, NR_POOL_HOUSE, 0, 0);
  std::vector<int> order;
  std::mutex m;
  std::vector<std::thread> th;
  for (const W &w : ws) {
    th.emplace_back([&, w] {
      nr_pool_acquire(p, w.prio, w.cell, w.dl);
      { std::lock_guard<std::mutex> g(m); order.push_back(w.id); }
      usleep(500); // hold briefly so the next grant is a hand-over, not a race
      nr_pool_release(p);
    });
    settle();
  }
  nr_pool_release(p);
  for (auto &t : th) t.join();
  return order;
}

TEST(CorePool, StrictPriorityOrder)
{
  nr_pool_t *p = mk(1);
  const auto o = grant_order(p, {{NR_POOL_HOUSE, 0, 0, 4}, {NR_POOL_DISC, 0, 0, 3}, {NR_POOL_RETRY, 0, 0, 2}, {NR_POOL_DECODE, 0, 0, 1}, {NR_POOL_SCAN, 0, 0, 0}});
  EXPECT_EQ(o, (std::vector<int>{0, 1, 2, 3, 4}));
  nr_pool_destroy(p);
}

TEST(CorePool, OldestDeadlineFirstInAClass)
{
  nr_pool_t *p = mk(1);
  const auto o = grant_order(p, {{NR_POOL_DECODE, 0, 900, 0}, {NR_POOL_DECODE, 0, 100, 1}, {NR_POOL_DECODE, 0, 500, 2}, {NR_POOL_DECODE, 0, 300, 3}});
  EXPECT_EQ(o, (std::vector<int>{1, 3, 2, 0}));
  nr_pool_destroy(p);
}

TEST(CorePool, CellsAlternateWithinAClass)
{
  nr_pool_t *p = mk(1);
  // cell 0 has the four oldest deadlines; plain deadline order would serve it first, round-robin must interleave.
  const auto o = grant_order(p, {{NR_POOL_DECODE, 0, 10, 0}, {NR_POOL_DECODE, 0, 20, 1}, {NR_POOL_DECODE, 0, 30, 2}, {NR_POOL_DECODE, 0, 40, 3},
                                 {NR_POOL_DECODE, 1, 50, 4}, {NR_POOL_DECODE, 1, 60, 5}, {NR_POOL_DECODE, 1, 70, 6}, {NR_POOL_DECODE, 1, 80, 7}});
  ASSERT_EQ(o.size(), 8u);
  for (size_t i = 0; i + 1 < o.size(); i++) EXPECT_NE(o[i] / 4, o[i + 1] / 4) << "grants " << i << "," << i + 1 << " from the same cell";
  EXPECT_LT(o[0], o[2]); // inside a cell still by deadline
  nr_pool_destroy(p);
}

TEST(CorePool, ReserveKeepsATokenForTheUrgentClasses)
{
  nr_pool_t *p = mk(3, 1);
  std::atomic<int> st{0};
  std::thread a([&] { nr_pool_acquire(p, NR_POOL_DECODE, 0, 0); st = 1; while (st < 9) usleep(100); nr_pool_release(p); });
  std::thread b([&] { while (st < 1) usleep(100); nr_pool_acquire(p, NR_POOL_DISC, 0, 0); st = 2; while (st < 9) usleep(100); nr_pool_release(p); });
  while (st < 2) usleep(100);
  // 2 of 3 tokens used; the last one is the reserve: a DISC or RETRY lease is refused, a DECODE or SCAN lease is granted.
  std::atomic<int> got_disc{-2}, got_retry{-2}, got_decode{-2};
  std::thread c([&] {
    got_disc = nr_pool_try_acquire(p, NR_POOL_DISC, 0, 0);
    got_retry = nr_pool_try_acquire(p, NR_POOL_RETRY, 0, 0);
    got_decode = nr_pool_try_acquire(p, NR_POOL_DECODE, 0, 0);
    nr_pool_release(p);
  });
  c.join();
  EXPECT_EQ(got_disc, -1);
  EXPECT_EQ(got_retry, -1);
  EXPECT_GE(got_decode, 0);
  st = 9;
  a.join();
  b.join();
  nr_pool_destroy(p);
}

TEST(CorePool, AgingPromotesAStarvedClassPastStrictOrder)
{
  nr_pool_cfg_t c = nr_pool_cfg_default();
  c.pin = false;
  c.reserve = 0;
  c.age_ns[NR_POOL_DISC] = 10000000ull; // 10 ms
  int core = 0;
  nr_pool_t *p = nr_pool_create(&core, nullptr, 1, &c);
  nr_pool_acquire(p, NR_POOL_HOUSE, 0, 0);
  std::vector<int> order;
  std::mutex m;
  auto run = [&](int prio, int id) {
    nr_pool_acquire(p, prio, 0, 0);
    { std::lock_guard<std::mutex> g(m); order.push_back(id); }
    usleep(300);
    nr_pool_release(p);
  };
  std::thread disc(run, NR_POOL_DISC, 1);
  usleep(40000); // older than its 10 ms limit
  std::thread scan(run, NR_POOL_SCAN, 0);
  usleep(30000);
  nr_pool_release(p);
  disc.join();
  scan.join();
  EXPECT_EQ(order, (std::vector<int>{1, 0})); // the overdue DISC job goes first, SCAN follows within one hold
  nr_pool_stats_t s;
  nr_pool_stats(p, &s);
  EXPECT_EQ(s.promoted[NR_POOL_DISC], 1u);
  nr_pool_destroy(p);
}

TEST(CorePool, NoTwoThreadsOnOneToken)
{
  const int N = 3;
  nr_pool_t *p = mk(N, 1, false);
  std::atomic<int> occ[N] = {};
  std::atomic<int> bad{0};
  std::atomic<bool> stop{false};
  std::vector<std::thread> th;
  for (int t = 0; t < 12; t++)
    th.emplace_back([&, t] {
      unsigned r = 12345u + (unsigned)t;
      while (!stop) {
        r = r * 1664525u + 1013904223u;
        const int core = nr_pool_acquire(p, (int)(r >> 20) % NR_POOL_NPRIO, (int)(r >> 12) % 3, 0);
        if (core < 0 || core >= N || occ[core].fetch_add(1) != 0) bad++;
        for (volatile int i = 0; i < 2000; i++) {}
        occ[core].fetch_sub(1);
        nr_pool_release(p);
        if (r & 4) usleep(20);
      }
    });
  usleep(1500000);
  stop = true;
  for (auto &x : th) x.join();
  EXPECT_EQ(bad.load(), 0);
  nr_pool_stats_t s;
  nr_pool_stats(p, &s);
  uint64_t tot = 0;
  for (auto v : s.leases) tot += v;
  EXPECT_GT(tot, 1000u);
  nr_pool_destroy(p);
}

// Urgent work arriving every 500 us behind chunked low-priority threads that yield: its wait stays below one chunk (reserve + yield),
// and the low-priority work still advances (no starvation).
TEST(CorePool, UrgentWaitBoundedBehindChunkedLowPriority)
{
  nr_pool_t *p = mk(3, 1, false);
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> chunks[4] = {};
  std::vector<std::thread> low;
  for (int t = 0; t < 4; t++)
    low.emplace_back([&, t] {
      while (!stop) {
        nr_pool_acquire(p, t < 2 ? NR_POOL_DISC : NR_POOL_RETRY, 0, 0);
        for (int k = 0; k < 10 && !stop; k++) {
          const uint64_t e = nr_pool_now_ns() + 200000; // 200 us chunk
          while (nr_pool_now_ns() < e) {}
          chunks[t]++;
          nr_pool_yield(p);
        }
        nr_pool_release(p);
        usleep(50);
      }
    });
  std::vector<uint64_t> waits;
  for (int i = 0; i < 1500; i++) {
    const uint64_t t0 = nr_pool_now_ns();
    nr_pool_acquire(p, NR_POOL_SCAN, 0, t0);
    waits.push_back(nr_pool_now_ns() - t0);
    const uint64_t e = nr_pool_now_ns() + 100000;
    while (nr_pool_now_ns() < e) {}
    nr_pool_release(p);
    usleep(400);
  }
  stop = true;
  for (auto &x : low) x.join();
  std::sort(waits.begin(), waits.end());
  const uint64_t p99 = waits[waits.size() * 99 / 100], mx = waits.back();
  printf("urgent wait us: p50=%lu p99=%lu max=%lu; low chunks %lu %lu %lu %lu\n", (unsigned long)waits[waits.size() / 2] / 1000,
         (unsigned long)p99 / 1000, (unsigned long)mx / 1000, (unsigned long)chunks[0].load(), (unsigned long)chunks[1].load(),
         (unsigned long)chunks[2].load(), (unsigned long)chunks[3].load());
  EXPECT_LT(p99, 250000u); // one chunk (200 us) + scheduling slack on a shared test host
  for (auto &c : chunks) EXPECT_GT(c.load(), 100u);
  nr_pool_destroy(p);
}

TEST(CorePool, GrantHandOverLatency)
{
  nr_pool_t *p = mk(1);
  std::vector<uint64_t> lat;
  for (int i = 0; i < 200; i++) {
    nr_pool_acquire(p, NR_POOL_HOUSE, 0, 0);
    std::atomic<uint64_t> t_got{0};
    std::thread w([&] { nr_pool_acquire(p, NR_POOL_SCAN, 0, 0); t_got = nr_pool_now_ns(); nr_pool_release(p); });
    usleep(2000); // the waiter is asleep in its futex
    const uint64_t t0 = nr_pool_now_ns();
    nr_pool_release(p);
    w.join();
    lat.push_back(t_got - t0);
  }
  std::sort(lat.begin(), lat.end());
  printf("hand-over (release -> sleeping waiter running) us: p50=%lu p99=%lu max=%lu\n", (unsigned long)lat[100] / 1000,
         (unsigned long)lat[198] / 1000, (unsigned long)lat[199] / 1000);
  EXPECT_LT(lat[100], 2000000u);
  nr_pool_destroy(p);
}

TEST(CorePool, SiteOffIsANoOp)
{
  unsetenv("ISAC_POOL_CORES");
  if (nr_pool_site()) GTEST_SKIP() << "site pool already built by another test";
  EXPECT_EQ(nr_pool_site_acquire(NR_POOL_SCAN, 0, 0), -1);
  nr_pool_site_release();
  EXPECT_FALSE(nr_pool_site_yield());
}

TEST(CorePool, BigCoresGoToUrgentClasses)
{
  nr_pool_cfg_t c = nr_pool_cfg_default();
  c.pin = false;
  c.reserve = 0;
  const int cores[3] = {10, 6, 11}, cap[3] = {512, 1024, 512};
  nr_pool_t *p = nr_pool_create(cores, cap, 3, &c);
  EXPECT_EQ(nr_pool_acquire(p, NR_POOL_SCAN, 0, 0), 6);
  nr_pool_release(p);
  const int lo = nr_pool_acquire(p, NR_POOL_DISC, 0, 0);
  EXPECT_NE(lo, 6);
  nr_pool_release(p);
  nr_pool_destroy(p);
}

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
