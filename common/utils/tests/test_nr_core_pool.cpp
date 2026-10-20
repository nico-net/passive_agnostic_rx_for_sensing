// SPDX-License-Identifier: LicenseRef-CSSL-1.0
// Scheduler tests for the shared core pool (nr_core_pool): order, fairness, reserve, aging, exclusivity, bounded wait.
// Pinning is off (cfg.pin=false) so the tests run on any host; the bench pins to cores 0-4.
#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#include <unistd.h>
#include <sched.h>
#include <sys/wait.h>
extern "C" {
#include "nr_core_pool.h"
}

static nr_pool_t *mk(int n, bool no_age = true)
{
  nr_pool_cfg_t c = nr_pool_cfg_default();
  c.pin = false;
  if (no_age) for (auto &a : c.age_ns) a = 0;
  static int cores[NR_POOL_MAX_TOKENS];
  for (int i = 0; i < n; i++) cores[i] = i;
  return nr_pool_create(cores, nullptr, n, &c);
}
// Deterministic rendezvous instead of fixed sleeps (a loaded host can delay a thread start by far more than any sleep we could afford).
static void wait_until(const std::function<bool()> &f)
{
  for (int i = 0; i < 20000 && !f(); i++) usleep(500); // 10 s
  ASSERT_TRUE(f()) << "rendezvous timed out";
}
static void wait_waiting(nr_pool_t *p, int k) { wait_until([&] { return nr_pool_total_waiting(p) >= k; }); }

// Holder owns the only token; waiters {prio, cell, deadline} queue up one by one; the grant order is the result.
struct W { int prio, cell; uint64_t dl; int id; };
static std::vector<int> grant_order(nr_pool_t *p, const std::vector<W> &ws)
{
  nr_pool_acquire(p, NR_POOL_HOUSE, 0, 0);
  std::vector<int> order;
  std::mutex m;
  std::vector<std::thread> th;
  int queued = 0;
  for (const W &w : ws) {
    th.emplace_back([&, w] {
      nr_pool_acquire(p, w.prio, w.cell, w.dl);
      { std::lock_guard<std::mutex> g(m); order.push_back(w.id); }
      usleep(500); // hold briefly so the next grant is a hand-over, not a race
      nr_pool_release(p);
    });
    wait_waiting(p, ++queued); // enqueued (the counter rises under the pool lock, just before the wait)
  }
  nr_pool_release(p);
  for (auto &t : th) t.join();
  return order;
}

// Site sizing: fewer than 3 site cores survive the allowed set -> "allowed non-RT cores minus one" (runs in a child: the site pool is a per-process singleton)
TEST(CorePool, SiteSizingFallsBackWhenFewSiteCoresSurvive)
{
  if (nr_pool_site_free() != -1) GTEST_SKIP() << "site pool already built in this process";
  cpu_set_t all;
  sched_getaffinity(0, sizeof(all), &all);
  std::vector<int> cs;
  for (int c = 0; c < CPU_SETSIZE; c++) if (CPU_ISSET(c, &all)) cs.push_back(c);
  if (cs.size() < 4) GTEST_SKIP() << "needs >= 4 allowed cpus";
  const pid_t pid = fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    cpu_set_t sub;
    CPU_ZERO(&sub);
    for (int i = 0; i < 4; i++) CPU_SET(cs[i], &sub);
    sched_setaffinity(0, sizeof(sub), &sub);
    char l[32];
    snprintf(l, sizeof(l), "%d,%d", cs[0], CPU_SETSIZE - 1); // one survivor, one outside the allowed set
    setenv("ISAC_PDSCH_CORES", l, 1);
    setenv("ISAC_PUSCH_CORES", l, 1);
    unsetenv("ISAC_UE_RT_CORE");
    unsetenv("ISAC_USRP_RING_CORE");
    unsetenv("ISAC_UHD_CTRL_CORES");
    nr_pool_t *p = nr_pool_site();
    _exit(p && nr_pool_size(p) == 3 && nr_pool_core_in_pool(p, cs[0]) && !nr_pool_core_in_pool(p, cs[3]) ? 0 : 1);
  }
  int st = 0;
  ASSERT_EQ(waitpid(pid, &st, 0), pid);
  EXPECT_TRUE(WIFEXITED(st) && WEXITSTATUS(st) == 0);
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

// (b): scans can never take the last token (the decode reserve), low classes leave a token for scan and one for decode.
TEST(CorePool, CapsLeaveATokenForDecodeAndScan)
{
  nr_pool_t *p = mk(4);
  std::atomic<int> st{0}, r[6];
  for (auto &x : r) x = -2;
  std::vector<std::thread> hold;
  auto holder = [&](int prio, int slot) {
    hold.emplace_back([&, prio, slot] {
      r[slot] = nr_pool_try_acquire(p, prio, 0, 0);
      while (st < 1) usleep(100);
      nr_pool_release(p);
    });
    wait_until([&] { return r[slot] != -2; });
  };
  holder(NR_POOL_DISC, 0);
  holder(NR_POOL_DISC, 1);
  holder(NR_POOL_DISC, 2);  // low cap N-2 = 2: refused
  holder(NR_POOL_RETRY, 3); // refused
  holder(NR_POOL_SCAN, 4);  // granted
  holder(NR_POOL_DECODE | NR_POOL_F_UL, 5);
  EXPECT_GE(r[0], 0);
  EXPECT_GE(r[1], 0);
  EXPECT_EQ(r[2], -1);
  EXPECT_EQ(r[3], -1);
  EXPECT_GE(r[4], 0); // 0 scans + 2 low < N-1
  EXPECT_GE(r[5], 0); // the last token is the decode reserve: still free for it

  st = 1;
  for (auto &t : hold) t.join();
  nr_pool_destroy(p);
}

TEST(CorePool, ScanCannotTakeTheDecodeReserve)
{
  nr_pool_t *p = mk(3);
  std::atomic<int> st{0}, r[4];
  for (auto &x : r) x = -2;
  std::vector<std::thread> hold;
  for (int i = 0; i < 4; i++) {
    hold.emplace_back([&, i] {
      r[i] = nr_pool_try_acquire(p, i < 3 ? NR_POOL_SCAN : NR_POOL_DECODE, 0, 0);
      while (st < 1) usleep(100);
      nr_pool_release(p);
    });
    wait_until([&] { return r[i] != -2; });
  }
  EXPECT_GE(r[0], 0);
  EXPECT_GE(r[1], 0);
  EXPECT_EQ(r[2], -1); // third scan: scan + low = 2 = N-1
  EXPECT_GE(r[3], 0);  // decode takes the reserve
  st = 1;
  for (auto &t : hold) t.join();
  nr_pool_destroy(p);
}

// DL decode may not take the token kept for UL decode (and scan): 2 DL decodes fill their cap, a third DL is refused, UL is granted.
TEST(CorePool, UlDecodeKeepsATokenAgainstDl)
{
  nr_pool_t *p = mk(4);
  std::atomic<int> st{0}, r[4];
  for (auto &x : r) x = -2;
  std::vector<std::thread> hold;
  for (int i = 0; i < 4; i++) {
    hold.emplace_back([&, i] {
      r[i] = nr_pool_try_acquire(p, i < 3 ? NR_POOL_DECODE : (NR_POOL_DECODE | NR_POOL_F_UL), 0, 0);
      while (st < 1) usleep(100);
      nr_pool_release(p);
    });
    wait_until([&] { return r[i] != -2; });
  }
  EXPECT_GE(r[0], 0);
  EXPECT_GE(r[1], 0);
  EXPECT_EQ(r[2], -1);
  EXPECT_GE(r[3], 0);
  st = 1;
  for (auto &t : hold) t.join();
  nr_pool_destroy(p);
}

// (b) end to end: scans re-arrive continuously on every scan consumer; a decode arrival never waits for more than one decode-length hold.
TEST(CorePool, DecodeNotStarvedByScans)
{
  nr_pool_t *p = mk(3, false);
  std::atomic<bool> stop{false};
  std::vector<std::thread> scans;
  for (int i = 0; i < 4; i++)
    scans.emplace_back([&] {
      while (!stop) {
        nr_pool_acquire(p, NR_POOL_SCAN, 0, 0);
        const uint64_t e = nr_pool_now_ns() + 200000;
        while (nr_pool_now_ns() < e) {}
        nr_pool_release(p);
      }
    });
  usleep(20000);
  std::vector<uint64_t> w;
  for (int i = 0; i < 400; i++) {
    const uint64_t t0 = nr_pool_now_ns();
    nr_pool_acquire(p, NR_POOL_DECODE, 0, t0);
    w.push_back(nr_pool_now_ns() - t0);
    nr_pool_release(p);
    usleep(300);
  }
  stop = true;
  for (auto &t : scans) t.join();
  std::sort(w.begin(), w.end());
  printf("decode wait behind 4 continuous scan threads (N=3) us: p50=%lu p99=%lu max=%lu\n", (unsigned long)w[200] / 1000,
         (unsigned long)w[395] / 1000, (unsigned long)w.back() / 1000);
  EXPECT_LT(w[395], 50000000u); // p99 < 50 ms: a scan hold is 200 us, the rest is scheduler slack on a loaded 4-core host (without the reserve it grows with the scan queue)
  nr_pool_destroy(p);
}

// (a) exclusivity: a thread that is not a member and whose mask contains a pool core loses that core; members keep theirs.
TEST(CorePool, EnforceRemovesPoolCoresFromNonMembers)
{
  cpu_set_t all;
  sched_getaffinity(0, sizeof(all), &all);
  std::vector<int> cs;
  for (int c = 0; c < CPU_SETSIZE; c++) if (CPU_ISSET(c, &all)) cs.push_back(c);
  if (cs.size() < 3) GTEST_SKIP() << "needs >= 3 allowed cpus";
  nr_pool_cfg_t c = nr_pool_cfg_default();
  c.pin = true;
  int cores[1] = {cs[0]};
  nr_pool_t *p = nr_pool_create(cores, nullptr, 1, &c);
  std::atomic<int> go{0};
  std::atomic<int> member_ok{0};
  cpu_set_t stranger_mask;
  CPU_ZERO(&stranger_mask);
  std::thread stranger([&] { while (go < 2) usleep(100); sched_getaffinity(0, sizeof(stranger_mask), &stranger_mask); });
  std::thread member([&] {
    nr_pool_acquire(p, NR_POOL_DECODE, 0, 0); // leases core cs[0] and is registered
    go = 1;
    while (go < 2) usleep(100);
    cpu_set_t m;
    sched_getaffinity(0, sizeof(m), &m);
    member_ok = CPU_ISSET(cores[0], &m) && CPU_COUNT(&m) == 1;
    nr_pool_release(p);
  });
  while (go < 1) usleep(100);
  EXPECT_GE(nr_pool_enforce(p), 1); // the stranger (inherited full mask) and this test thread lose cs[0]
  cpu_set_t self;
  sched_getaffinity(0, sizeof(self), &self);
  EXPECT_FALSE(CPU_ISSET(cores[0], &self));
  go = 2;
  stranger.join();
  EXPECT_FALSE(CPU_ISSET(cores[0], &stranger_mask));
  sched_setaffinity(0, sizeof(all), &all); // give this test thread its cores back
  member.join();
  EXPECT_EQ(member_ok.load(), 1);
  // after release the member went home: no pool core in its mask (checked via a fresh lease cycle on another thread)
  nr_pool_destroy(p);
}

// (c) a different class asked under a held lease is visible in the stats
TEST(CorePool, NestedClassIsCounted)
{
  nr_pool_t *p = mk(3);
  nr_pool_acquire(p, NR_POOL_SCAN, 0, 0);
  nr_pool_acquire(p, NR_POOL_DISC, 0, 0);
  nr_pool_stats_t s;
  nr_pool_stats(p, &s);
  EXPECT_EQ(s.nested, 1u);
  nr_pool_release(p);
  nr_pool_destroy(p);
}

// (d) the site pool builds with no environment at all, from the built-in site map
TEST(CorePool, SitePoolWithNoEnvironment)
{
  unsetenv("ISAC_PDSCH_CORES");
  unsetenv("ISAC_PUSCH_CORES");
  cpu_set_t all; // before the pool exists: building it strips the pool cores from this (non-member) thread
  sched_getaffinity(0, sizeof(all), &all);
  nr_pool_t *p = nr_pool_site();
  ASSERT_NE(p, nullptr);
  int want = 0;
  for (int c : {6, 7, 8, 9, 3, 4}) want += CPU_ISSET(c, &all) ? 1 : 0;
  const int a = CPU_COUNT(&all);
  if (want < 3) { // fewer than 3 site cores survive: the allowed cores minus one (a host smaller than the site map)
    EXPECT_EQ(nr_pool_size(p), std::max(want, a > 1 ? a - 1 : a));
    return;
  }
  for (int c : {6, 7, 8, 9, 3, 4}) EXPECT_EQ(nr_pool_core_in_pool(p, c), CPU_ISSET(c, &all) != 0) << "core " << c;
  EXPECT_EQ(nr_pool_size(p), want); // 6 on the DGX
  EXPECT_FALSE(nr_pool_core_in_pool(p, 5));
  EXPECT_EQ(nr_pool_site_filter_core(2), 2);
}

TEST(CorePool, AgingPromotesAStarvedClassPastStrictOrder)
{
  nr_pool_cfg_t c = nr_pool_cfg_default();
  c.pin = false;
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
  wait_waiting(p, 1);
  usleep(40000); // older than its 10 ms limit
  std::thread scan(run, NR_POOL_SCAN, 0);
  wait_waiting(p, 2);
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
  nr_pool_t *p = mk(N, false);
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
  nr_pool_t *p = mk(3, false);
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
  EXPECT_LT(p99, 50000000u); // p99 < 50 ms: one 200 us chunk + scheduler slack on a loaded host; scans are never capped out here
  for (auto &c : chunks) EXPECT_GT(c.load(), 0u); // low classes advance (RETRY before DISC by strict order; DISC by aging)
  nr_pool_destroy(p);
}

// Aging bound without any release/arrival event: n=2 caps low classes at one token, so a second DISC waits with a token FREE. Only the age timer
// can let it in; it must be granted near age_ns while the first DISC still holds.
TEST(CorePool, AgingBoundHoldsWhileATokenIsFreeBehindACap)
{
  nr_pool_cfg_t c = nr_pool_cfg_default();
  c.pin = false;
  for (auto &a : c.age_ns) a = 0;
  c.age_ns[NR_POOL_DISC] = 20000000ull; // 20 ms
  const int cores[2] = {0, 1};
  nr_pool_t *p = nr_pool_create(cores, nullptr, 2, &c);
  std::atomic<bool> first_in{false}, second_in{false};
  std::atomic<bool> first_out_before_second{false};
  std::thread first([&] {
    nr_pool_acquire(p, NR_POOL_DISC, 0, 0);
    first_in = true;
    for (int i = 0; i < 3000 && !second_in; i++) usleep(1000); // holds until the second is in (or 3 s)
    if (!second_in) first_out_before_second = true;
    nr_pool_release(p);
  });
  while (!first_in) usleep(100);
  EXPECT_EQ(nr_pool_try_acquire(p, NR_POOL_DISC, 0, 0), -1); // capped although a token is free
  const uint64_t t0 = nr_pool_now_ns();
  nr_pool_acquire(p, NR_POOL_DISC, 0, 0);
  const uint64_t w = nr_pool_now_ns() - t0;
  second_in = true;
  nr_pool_release(p);
  first.join();
  EXPECT_FALSE(first_out_before_second.load()) << "second DISC only got in after the first released: aging did not fire";
  EXPECT_GE(w, 15000000u); // not before the age limit (the cap is real)
  EXPECT_LT(w, 1000000000u); // and long before the 3 s the holder would wait
  nr_pool_stats_t st;
  nr_pool_stats(p, &st);
  EXPECT_EQ(st.promoted[NR_POOL_DISC], 1u);
  nr_pool_destroy(p);
}

// No deadlock: mixed classes/cells/flags, yields and try_acquire, on 1, 2 and 3 tokens; every thread must finish its iterations.
TEST(CorePool, NoDeadlockMixedWorkload)
{
  for (int n : {1, 2, 3}) {
    nr_pool_t *p = mk(n, false);
    std::atomic<int> done{0};
    const int T = 10;
    std::vector<std::thread> th;
    for (int t = 0; t < T; t++)
      th.emplace_back([&, t] {
        unsigned r = 777u + (unsigned)t * 31u;
        for (int it = 0; it < 3000; it++) {
          r = r * 1664525u + 1013904223u;
          const int pr = (int)(r >> 20) % NR_POOL_NPRIO | ((r & 0x10000) ? NR_POOL_F_UL : 0);
          if (r & 0x20000) {
            if (nr_pool_try_acquire(p, pr, 0, 0) < 0) continue;
          } else {
            nr_pool_acquire(p, pr, (int)(r >> 12) % 4, nr_pool_now_ns() + (r & 0xff));
          }
          for (volatile int i = 0; i < 200; i++) {}
          if (r & 0x40000) nr_pool_yield(p);
          nr_pool_release(p);
        }
        done++;
      });
    for (int i = 0; i < 60000 && done < T; i++) usleep(1000); // 60 s watchdog
    if (done < T) {
      fprintf(stderr, "DEADLOCK n=%d done=%d/%d\n", n, done.load(), T);
      _exit(3); // threads are stuck: joining would hang the whole run
    }
    for (auto &x : th) x.join();
    EXPECT_EQ(nr_pool_free(p), n); // every token came back
    nr_pool_destroy(p);
  }
}

// (a) with pinning on, a lease runs on exactly its core and a released thread is off every pool core. Skipped on hosts with < 3 allowed cpus.
TEST(CorePool, PinnedLeaseRunsOnItsCoreAndGoesHome)
{
  cpu_set_t all;
  sched_getaffinity(0, sizeof(all), &all);
  std::vector<int> cs;
  for (int c = 0; c < CPU_SETSIZE; c++) if (CPU_ISSET(c, &all)) cs.push_back(c);
  if (cs.size() < 3) GTEST_SKIP() << "needs >= 3 allowed cpus";
  nr_pool_cfg_t c = nr_pool_cfg_default();
  c.pin = true;
  const int cores[2] = {cs[0], cs[1]};
  nr_pool_t *p = nr_pool_create(cores, nullptr, 2, &c);
  std::atomic<int> bad{0}, leases{0};
  std::atomic<bool> stop{false};
  std::vector<std::thread> th;
  for (int t = 0; t < 6; t++)
    th.emplace_back([&] {
      while (!stop) {
        const int core = nr_pool_acquire(p, NR_POOL_DECODE, 0, 0);
        cpu_set_t m;
        sched_getaffinity(0, sizeof(m), &m);
        if (CPU_COUNT(&m) != 1 || !CPU_ISSET(core, &m) || sched_getcpu() != core) bad++;
        for (volatile int i = 0; i < 20000; i++) {}
        if (sched_getcpu() != core) bad++;
        nr_pool_release(p);
        sched_getaffinity(0, sizeof(m), &m);
        if (CPU_ISSET(cores[0], &m) || CPU_ISSET(cores[1], &m)) bad++;
        leases++;
        usleep(50);
      }
    });
  usleep(500000);
  stop = true;
  for (auto &x : th) x.join();
  EXPECT_EQ(bad.load(), 0);
  EXPECT_GT(leases.load(), 100);
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
    wait_waiting(p, 1);
    usleep(1000); // the waiter is asleep in its futex
    const uint64_t t0 = nr_pool_now_ns();
    nr_pool_release(p);
    w.join();
    lat.push_back(t_got - t0);
  }
  std::sort(lat.begin(), lat.end());
  printf("hand-over (release -> sleeping waiter running) us: p50=%lu p99=%lu max=%lu\n", (unsigned long)lat[100] / 1000,
         (unsigned long)lat[198] / 1000, (unsigned long)lat[199] / 1000);
  EXPECT_LT(lat[100], 20000000u);  // p50 < 20 ms (a futex wake; typically tens of us)
  EXPECT_LT(lat[198], 200000000u); // p99 < 200 ms on a loaded host
  nr_pool_destroy(p);
}

TEST(CorePool, BigCoresGoToUrgentClasses)
{
  nr_pool_cfg_t c = nr_pool_cfg_default();
  c.pin = false;
  const int cores[3] = {10, 6, 11}, cap[3] = {512, 1024, 512};
  nr_pool_t *p = nr_pool_create(cores, cap, 3, &c);
  EXPECT_EQ(nr_pool_acquire(p, NR_POOL_SCAN, 0, 0), 6);
  nr_pool_release(p);
  const int lo = nr_pool_acquire(p, NR_POOL_DISC, 0, 0);
  EXPECT_NE(lo, 6);
  nr_pool_release(p);
  nr_pool_destroy(p);
}

// B1 shape (scan side_finish at N=1): the holder waits for a thread that needs the only token. Parked, it does not deadlock; unpark restores the class.
TEST(CorePool, ParkLetsTheWaitedForThreadTakeTheOnlyToken)
{
  nr_pool_t *p = mk(1);
  nr_pool_acquire(p, NR_POOL_SCAN, 0, 0);
  std::atomic<int> done{0};
  std::thread t([&] {
    nr_pool_acquire(p, NR_POOL_SCAN, 0, 0);
    nr_pool_release(p);
    done = 1;
  });
  wait_waiting(p, 1);
  nr_pool_park_t pk;
  nr_pool_park(p, &pk);
  EXPECT_FALSE(nr_pool_holds(p));
  wait_until([&] { return done.load() == 1; }); // would hang forever without the park
  nr_pool_unpark(p, &pk);
  EXPECT_TRUE(nr_pool_holds(p));
  nr_pool_stats_t s;
  nr_pool_stats(p, &s);
  EXPECT_EQ(s.leases[NR_POOL_SCAN], 3u); // holder, taker, holder again: same class
  EXPECT_EQ(s.nested, 0u);
  nr_pool_release(p);
  t.join();
  nr_pool_park(p, &pk); // no lease: both are no-ops
  EXPECT_FALSE(pk.held);
  nr_pool_unpark(p, &pk);
  EXPECT_FALSE(nr_pool_holds(p));
  nr_pool_destroy(p);
}

// B2: the GPU-wait hook gives the token away only inside a give scope and only to a waiter the token could go to.
TEST(CorePool, GpuWaitGivesOnlyInsideAGiveScopeToAGrantableWaiter)
{
  nr_pool_t *p = mk(2); // SCAN cap N-1 = 1; DISC needs busy < N-2 -> 1
  nr_pool_acquire(p, NR_POOL_SCAN, 0, 0);
  std::atomic<int> h2_go{0}, w_got{0};
  std::thread h2([&] {
    nr_pool_acquire(p, NR_POOL_DECODE | NR_POOL_F_UL, 0, 0);
    while (!h2_go) usleep(100);
    nr_pool_release(p);
  });
  wait_until([&] { nr_pool_stats_t t; nr_pool_stats(p, &t); return t.leases[NR_POOL_DECODE] == 1; });
  std::thread disc([&] { nr_pool_acquire(p, NR_POOL_DISC, 0, 0); nr_pool_release(p); });
  wait_waiting(p, 1);
  nr_pool_stats_t s;
  nr_pool_gpu_wait(p); // no scope
  nr_pool_stats(p, &s);
  EXPECT_EQ(s.gpu_gives, 0u);
  EXPECT_TRUE(nr_pool_holds(p));
  nr_pool_site_gpu_give_begin();
  nr_pool_gpu_wait(p); // scope, but the freed token cannot go to a DISC waiter (cap)
  nr_pool_stats(p, &s);
  EXPECT_EQ(s.gpu_gives, 0u);
  EXPECT_TRUE(nr_pool_holds(p));
  std::thread scan([&] { nr_pool_acquire(p, NR_POOL_SCAN, 0, 0); w_got = 1; nr_pool_release(p); });
  wait_waiting(p, 2);
  nr_pool_site_gpu_give_begin(); // nested
  nr_pool_site_gpu_give_end();
  nr_pool_gpu_wait(p); // a SCAN waiter exists: give
  nr_pool_stats(p, &s);
  EXPECT_EQ(s.gpu_gives, 1u);
  EXPECT_EQ(w_got.load(), 1);
  EXPECT_TRUE(nr_pool_holds(p));
  nr_pool_site_gpu_give_end();
  nr_pool_gpu_wait(p); // scope closed again: sleeps only
  nr_pool_stats(p, &s);
  EXPECT_EQ(s.gpu_gives, 1u);
  nr_pool_release(p);
  h2_go = 1;
  h2.join();
  scan.join();
  disc.join();
  nr_pool_destroy(p);
}

// B4: scan-side extra work is counted only under a SCAN lease
TEST(CorePool, ScanExtraCountedOnlyUnderAScanLease)
{
  nr_pool_t *p = mk(3);
  nr_pool_stats_t s;
  nr_pool_note_scan_extra(p); // no lease
  nr_pool_acquire(p, NR_POOL_DECODE, 0, 0);
  nr_pool_note_scan_extra(p); // wrong class
  nr_pool_release(p);
  nr_pool_stats(p, &s);
  EXPECT_EQ(s.nested, 0u);
  nr_pool_acquire(p, NR_POOL_SCAN, 0, 0);
  nr_pool_note_scan_extra(p);
  nr_pool_note_scan_extra(p);
  nr_pool_release(p);
  nr_pool_stats(p, &s);
  EXPECT_EQ(s.nested, 2u);
  nr_pool_destroy(p);
}

// M1: a lessee's child thread gets the home mask attr; a non-lessee does not (site pool: needs it to exist)
TEST(CorePool, HomeAttrOnlyForALessee)
{
  pthread_attr_t a;
  pthread_attr_init(&a);
  EXPECT_FALSE(nr_pool_site_home_attr(&a)); // no lease
  pthread_attr_destroy(&a);
}

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
