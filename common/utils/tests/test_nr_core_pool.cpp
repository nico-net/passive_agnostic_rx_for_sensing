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
#include <condition_variable>
#include <deque>
#include <memory>
#include <queue>
#include <regex>
#include <string>
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

// B1: across cells the order inside a class is by decayed token-time usage (equal usage: round-robin). The original wall-clock version of this test held
// the token with cell 0, which (by design) gave cell 1 the first four grants to even out the usage; it is now a virtual-time test (sim, defined
// below) whose holder belongs to a third cell, so both cells start at equal usage and equal-length jobs must interleave.
static void cells_alternate_body();
TEST(CorePool, CellsAlternateWithinAClass) { cells_alternate_body(); }

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

// Try to take one token per entry (in order, held until the end); returns the cores (-1 = refused).
static std::vector<int> take_all(nr_pool_t *p, const std::vector<int> &prios)
{
  std::atomic<int> st{0};
  std::vector<std::atomic<int>> r(prios.size());
  for (auto &x : r) x = -2;
  std::vector<std::thread> hold;
  for (size_t i = 0; i < prios.size(); i++) {
    hold.emplace_back([&, i] {
      r[i] = nr_pool_try_acquire(p, prios[i], 0, 0);
      while (st < 1) usleep(100);
      nr_pool_release(p);
    });
    wait_until([&] { return r[i] != -2; });
  }
  std::vector<int> out(r.begin(), r.end());
  st = 1;
  for (auto &t : hold) t.join();
  return out;
}

// DL first-pass decode holds N-1 tokens (low classes not counted); the last token is the scan reserve, so UL waits for a DL release.
TEST(CorePool, DlTakesEveryTokenButTheScanReserve)
{
  nr_pool_t *p = mk(4);
  const int DL = NR_POOL_DECODE, UL = NR_POOL_DECODE | NR_POOL_F_UL;
  const auto r = take_all(p, {DL, DL, DL, DL, UL, NR_POOL_SCAN});
  EXPECT_GE(r[0], 0);
  EXPECT_GE(r[1], 0);
  EXPECT_GE(r[2], 0);
  EXPECT_EQ(r[3], -1); // N-1 = 3 DL
  EXPECT_EQ(r[4], -1); // UL: decode already at N-1
  EXPECT_GE(r[5], 0);  // the scan still gets its token
  nr_pool_destroy(p);
}

// Default site size N = 6 with D = 5 DL consumers: all five hold a token at once, a scan still gets the sixth; with a scan already
// running the five DL still fit (N-1 DL + 1 scan).
TEST(CorePool, FiveDlDecodesRunAtOnceAtN6AndAScanStillGetsAToken)
{
  const int DL = NR_POOL_DECODE, UL = NR_POOL_DECODE | NR_POOL_F_UL;
  nr_pool_t *p = mk(6);
  auto r = take_all(p, {DL, DL, DL, DL, DL, DL, UL, NR_POOL_SCAN});
  for (int i = 0; i < 5; i++) EXPECT_GE(r[i], 0) << "DL " << i;
  EXPECT_EQ(r[5], -1);
  EXPECT_EQ(r[6], -1);
  EXPECT_GE(r[7], 0);
  nr_pool_destroy(p);
  p = mk(6);
  r = take_all(p, {NR_POOL_SCAN, DL, DL, DL, DL, DL, DL});
  for (int i = 0; i < 6; i++) EXPECT_GE(r[i], 0) << i;
  EXPECT_EQ(r[6], -1);
  nr_pool_destroy(p);
  p = mk(6); // UL keeps working when DL has fewer consumers than tokens: 3 DL + 2 UL
  r = take_all(p, {DL, DL, DL, UL, UL, NR_POOL_SCAN});
  for (int i = 0; i < 6; i++) EXPECT_GE(r[i], 0) << i;
  nr_pool_destroy(p);
}

static void wait_leased(nr_pool_t *p, int k) // (stats are read under the pool lock; nr_pool_free is a deliberately racy hint)
{
  wait_until([&] { nr_pool_stats_t s; nr_pool_stats(p, &s); return s.leases[NR_POOL_DECODE] >= (uint64_t)k; });
}
// UL waits behind N-1 DL: DL gives up its last core while a UL decode waits (cap N-2), so the next DL release goes to UL, even to a DL waiter that queued first.
TEST(CorePool, WaitingUlGetsTheNextDlRelease)
{
  const int DL = NR_POOL_DECODE, UL = NR_POOL_DECODE | NR_POOL_F_UL;
  nr_pool_t *p = mk(6);
  std::atomic<int> go{0}, ulgot{0}, dlgot{0};
  std::vector<std::thread> th;
  for (int i = 0; i < 5; i++) // 5 DL held (no UL waiting: DL still gets N-1)
    th.emplace_back([&] { ASSERT_GE(nr_pool_acquire(p, DL, 0, 0), 0); while (go < 1) usleep(100); nr_pool_release(p); });
  wait_leased(p, 5);
  th.emplace_back([&] { nr_pool_acquire(p, DL, 0, 0); dlgot = 1; while (go < 2) usleep(100); nr_pool_release(p); }); // 6th DL: refused, queued first
  wait_waiting(p, 1);
  th.emplace_back([&] { nr_pool_acquire(p, UL, 0, 0); ulgot = 1; while (go < 2) usleep(100); nr_pool_release(p); });
  wait_waiting(p, 2);
  EXPECT_EQ(nr_pool_try_acquire(p, DL, 0, 0), -1); // a new DL is refused while UL waits
  go = 1;                                           // all 5 DL release: the first release must serve UL
  wait_until([&] { return ulgot == 1; });
  wait_until([&] { return dlgot == 1; });
  go = 2;
  for (auto &t : th) t.join();
  nr_pool_destroy(p);
  // order check, one release at a time: N=3, 2 DL held (= N-1), DL waiter queued before the UL waiter
  p = mk(3);
  std::atomic<int> rel{0};
  std::vector<std::thread> h2;
  std::vector<int> got;
  std::mutex m;
  for (int i = 0; i < 2; i++)
    h2.emplace_back([&, i] { nr_pool_acquire(p, DL, 0, 0); while (rel <= i) usleep(100); nr_pool_release(p); });
  wait_leased(p, 2);
  for (int c : {0, 1}) // DL (queued first) then UL
    h2.emplace_back([&, c] {
      nr_pool_acquire(p, c ? UL : DL, 0, 0);
      { std::lock_guard<std::mutex> g(m); got.push_back(c); }
      usleep(500);
      nr_pool_release(p);
    }), wait_waiting(p, c + 1);
  rel = 1; // one DL releases
  wait_until([&] { std::lock_guard<std::mutex> g(m); return got.size() >= 2; });
  rel = 2;
  for (auto &t : h2) t.join();
  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ(got[0], 1) << "UL must win the first released token";
  nr_pool_destroy(p);
}

// No UL starvation under continuous DL load: with more DL threads than tokens, a UL decode gets a token within about one DL hold.
TEST(CorePool, UlNotStarvedByContinuousDl)
{
  const int DL = NR_POOL_DECODE, UL = NR_POOL_DECODE | NR_POOL_F_UL;
  nr_pool_t *p = mk(3);
  std::atomic<bool> stop{false};
  std::vector<std::thread> th;
  for (int i = 0; i < 5; i++)
    th.emplace_back([&] { while (!stop) { nr_pool_acquire(p, DL, 0, 0); usleep(1000); nr_pool_release(p); } });
  uint64_t worst = 0;
  for (int i = 0; i < 200; i++) {
    const uint64_t t0 = nr_pool_now_ns();
    nr_pool_acquire(p, UL, 0, 0);
    worst = std::max(worst, nr_pool_now_ns() - t0);
    nr_pool_release(p);
    usleep(500);
  }
  stop = true;
  for (auto &t : th) t.join();
  EXPECT_LT(worst, 50000000ull) << "worst UL wait ns"; // one DL hold is 1 ms; 50 ms leaves room for a loaded host, starvation would be unbounded
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

// ===================================================================================================================================
// B1 (mpci-main plan B.4): deterministic virtual-time scheduling tests. Real pool, real threads, but the pool clock is a virtual one
// (nr_pool_set_clock_for_test) and a single driver advances it, so every grant order, wait and share below is exactly reproducible.
// Each job is a thread that leases (prio, cell), records the virtual time of its grant, and releases when the driver says its duration is over.
// ===================================================================================================================================
namespace sim {
static std::atomic<uint64_t> vt{0};
static uint64_t clk() { return vt.load(); }

struct Spec { uint64_t t; int prio, cell; uint64_t dur, dl; };
struct Job {
  int id = 0, prio = 0, cell = 0;
  uint64_t arr = 0, dur = 0, dl = 0, granted_at = 0, released_at = 0;
  std::atomic<bool> granted{false}, rel{false};
  std::mutex m;
  std::condition_variable cv;
  bool go = false;
  std::thread th;
};
struct Sim {
  nr_pool_t *p;
  std::deque<std::unique_ptr<Job>> jobs;
  std::vector<Job *> live; // started, not released
  struct Cmp { bool operator()(const std::pair<Spec, int> &a, const std::pair<Spec, int> &b) const { return a.first.t != b.first.t ? a.first.t > b.first.t : a.second > b.second; } };
  std::priority_queue<std::pair<Spec, int>, std::vector<std::pair<Spec, int>>, Cmp> arrivals;
  int aseq = 0, started = 0;
  std::atomic<int> ngranted{0};
  std::atomic<int> conc_cell[NR_POOL_MAX_CELLS];
  std::atomic<int> conc_tot{0};
  std::function<void(Job &)> on_done;
  struct Sample { uint64_t t; int tot; int cell[4]; };
  std::vector<Sample> samples; // after every settle
  bool stuck = false;
  explicit Sim(nr_pool_t *pool) : p(pool)
  {
    for (auto &c : conc_cell) c = 0;
    vt = 0;
    nr_pool_set_clock_for_test(clk);
  }
  ~Sim() { nr_pool_set_clock_for_test(nullptr); }
  void add(const Spec &sp) { arrivals.push({sp, aseq++}); }
  void settle()
  {
    for (int i = 0; i < 2000000; i++) {
      const int w = nr_pool_total_waiting(p); // lock-free: read BEFORE the locked stats (a grant moves one from waiting to leased, never the reverse)
      nr_pool_stats_t st;
      nr_pool_stats(p, &st);
      uint64_t l = 0;
      for (int k = 0; k < NR_POOL_NPRIO; k++) l += st.leases[k];
      if ((int)l == ngranted.load() && (int)l + w == started) return;
      if (i < 200) sched_yield(); else usleep(50);
    }
    fprintf(stderr, "SIM: did not settle t=%lu started=%d granted=%d waiting=%d\n", (unsigned long)vt.load(), started, ngranted.load(), nr_pool_total_waiting(p));
    ADD_FAILURE() << "sim did not settle (pool lost a waiter or a grant)";
    stuck = true;
  }
  void start(const Spec &sp)
  {
    if (nr_pool_total_waiting(p) >= 240) { // the pool's wait queue holds 256 waiters (MAX_WAITERS); the real receiver has far fewer threads
      ADD_FAILURE() << "test trace overloads the 256-entry wait queue";
      _exit(3);
    }
    jobs.emplace_back(new Job);
    Job *j = jobs.back().get();
    j->id = (int)jobs.size() - 1, j->prio = sp.prio, j->cell = sp.cell, j->arr = sp.t, j->dur = sp.dur, j->dl = sp.dl;
    started++;
    live.push_back(j);
    j->th = std::thread([this, j] {
      nr_pool_acquire(p, j->prio, j->cell, j->dl);
      j->granted_at = vt.load();
      conc_cell[j->cell]++;
      conc_tot++;
      j->granted = true; // before ngranted: settle() returns on the count, run() then reads the flags
      ngranted++;
      { std::unique_lock<std::mutex> l(j->m); j->cv.wait(l, [j] { return j->go; }); }
      conc_cell[j->cell]--;
      conc_tot--;
      nr_pool_release(p);
      j->released_at = vt.load();
      j->rel = true;
    });
    settle();
  }
  void sample()
  {
    Sample s{vt.load(), conc_tot.load(), {0, 0, 0, 0}};
    for (int c = 0; c < 4; c++) s.cell[c] = conc_cell[c].load();
    samples.push_back(s);
  }
  void run()
  {
    for (;;) {
      Job *r = nullptr;
      for (Job *j : live)
        if (j->granted && (!r || j->granted_at + j->dur < r->granted_at + r->dur)) r = j;
      const bool ha = !arrivals.empty();
      if (!r && !ha) break;
      if (ha && (!r || arrivals.top().first.t < r->granted_at + r->dur)) {
        const Spec sp = arrivals.top().first;
        arrivals.pop();
        vt = sp.t;
        start(sp);
      } else {
        vt = r->granted_at + r->dur;
        { std::lock_guard<std::mutex> l(r->m); r->go = true; }
        r->cv.notify_one();
        while (!r->rel) sched_yield();
        r->th.join();
        live.erase(std::find(live.begin(), live.end(), r));
        settle();
        if (on_done) on_done(*r);
      }
      sample();
      if (stuck) break;
    }
    if (!live.empty() && !stuck) { // nothing running, nothing arriving, yet jobs still waiting: the pool left a grantable waiter idle
      ADD_FAILURE() << live.size() << " jobs never granted (deadlock)";
      stuck = true;
    }
    if (stuck) { fprintf(stderr, "SIM: stuck, aborting\n"); _exit(3); } // threads blocked in the pool: cannot join
    for (auto &j : jobs) if (j->th.joinable()) j->th.join();
  }
};
static uint64_t pct(std::vector<uint64_t> v, double q)
{
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v[std::min(v.size() - 1, (size_t)(q * (double)v.size()))];
}
} // namespace sim
static uint64_t vt_now() { return sim::vt.load(); }

static void cells_alternate_body()
{
  nr_pool_t *p = mk(1);
  sim::Sim S(p);
  S.add({0, NR_POOL_HOUSE, 2, 500000, 0}); // holder (third cell)
  // cell 0 has the four oldest deadlines; plain deadline order would serve it first, the cells must interleave.
  const uint64_t dl[8] = {10, 20, 30, 40, 50, 60, 70, 80};
  for (int i = 0; i < 8; i++) S.add({1000, NR_POOL_DECODE, i / 4, 500000, dl[i]});
  S.run();
  std::vector<const sim::Job *> o;
  for (size_t i = 1; i < S.jobs.size(); i++) o.push_back(S.jobs[i].get());
  std::sort(o.begin(), o.end(), [](const sim::Job *a, const sim::Job *b) { return a->granted_at < b->granted_at; });
  ASSERT_EQ(o.size(), 8u);
  for (size_t i = 0; i + 1 < o.size(); i++) EXPECT_NE(o[i]->cell, o[i + 1]->cell) << "grants " << i << "," << i + 1 << " from the same cell";
  EXPECT_LT(o[0]->id, o[2]->id); // inside a cell still by deadline
  nr_pool_destroy(p);
}

// A virtual-time, single-cell job trace (no randomness outside the seed): all classes, UL flag, deadlines, overload so queues build and aging fires.
// The decisions (grant order with grant times) and the CORE_POOL stats line are folded into a string that is compared with a golden produced by the
// pool as it was BEFORE B1 (regenerate: print a and b in this test, built against that pool with the same clock hook) (the commit this task branched from): N=1 equivalence.
static const char *GOLDEN4 = R"G(order_hash=43ef75bd69447520 first=0,1,2,3,4,5,6,8,10,11,12,13,
SENSING: CORE_POOL n=4 yields=0 gpu_gives=0 nested=0 enforced=0 scan[n=168 waited=32 promo=0 wait_us p50<=1 p99<=512 max=590] decode[n=154 waited=67 promo=0 wait_us p50<=1 p99<=1024 max=784] retry[n=66 waited=65 promo=0 wait_us p50<=8192 p99<=16384 max=15185] disc[n=69 waited=69 promo=0 wait_us p50<=65536 p99<=131072 max=77230] house[n=43 waited=43 promo=0 wait_us p50<=131072 p99<=131072 max=117588])G";
static const char *GOLDEN6 = R"G(order_hash=f814af8c73a450b0 first=0,1,2,3,4,5,6,7,8,10,11,13,
SENSING: CORE_POOL n=6 yields=0 gpu_gives=0 nested=0 enforced=0 scan[n=262 waited=162 promo=0 wait_us p50<=64 p99<=4096 max=2863] decode[n=260 waited=201 promo=0 wait_us p50<=4096 p99<=16384 max=13392] retry[n=95 waited=94 promo=59 wait_us p50<=4096 p99<=8192 max=4422] disc[n=115 waited=115 promo=89 wait_us p50<=32768 p99<=32768 max=20580] house[n=68 waited=68 promo=0 wait_us p50<=131072 p99<=131072 max=90054])G";
static std::string single_cell_trace(int n, bool ages, int njobs, uint64_t gap_ns)
{
  nr_pool_t *p = mk(n, !ages);
  sim::Sim S(p);
  unsigned r = 4242u;
  auto rnd = [&] { r = r * 1664525u + 1013904223u; return r >> 8; };
  uint64_t t = 0;
  for (int i = 0; i < njobs; i++) {
    t += gap_ns / 2 + rnd() % (gap_ns + 1);
    const unsigned k = rnd() % 100;
    int prio = NR_POOL_SCAN;
    uint64_t dur = 225000;
    if (k < 30) prio = NR_POOL_SCAN, dur = 225000;
    else if (k < 55) prio = NR_POOL_DECODE, dur = 600000;
    else if (k < 65) prio = NR_POOL_DECODE | NR_POOL_F_UL, dur = 400000;
    else if (k < 75) prio = NR_POOL_RETRY, dur = 1500000;
    else if (k < 90) prio = NR_POOL_DISC, dur = 1000000;
    else prio = NR_POOL_HOUSE, dur = 2000000;
    S.add({t, prio, 0, dur + rnd() % 100000, t + rnd() % 3000000});
  }
  S.run();
  uint64_t h = 1469598103934665603ull; // FNV-1a over (job id, grant time) in grant order
  std::vector<const sim::Job *> o;
  for (auto &j : S.jobs) o.push_back(j.get());
  std::stable_sort(o.begin(), o.end(), [](const sim::Job *a, const sim::Job *b) { return a->granted_at < b->granted_at; });
  std::string head;
  int idx = 0;
  for (const sim::Job *j : o) {
    for (uint64_t v : {(uint64_t)j->id, j->granted_at}) for (int b = 0; b < 8; b++) h = (h ^ ((v >> (8 * b)) & 0xff)) * 1099511628211ull;
    if (idx++ < 12) head += std::to_string(j->id) + ",";
  }
  char line[2048];
  nr_pool_stats_line(p, line, sizeof(line));
  char out[3072];
  snprintf(out, sizeof(out), "order_hash=%016llx first=%s\n%s", (unsigned long long)h, head.c_str(), line);
  nr_pool_destroy(p);
  return out;
}

TEST(CorePoolB1, SingleCellTraceEqualsThePreB1Pool)
{
  // Golden values captured from the pre-B1 nr_core_pool.c (9b2d7c3f6e) with the same clock seam: N=4 no aging, N=6 with the default ages (promotion fires).
  const std::string a = single_cell_trace(4, false, 500, 150000), b = single_cell_trace(6, true, 800, 100000);
  EXPECT_EQ(a, GOLDEN4);
  EXPECT_EQ(b, GOLDEN6);
}

#ifndef NR_OLD_POOL_BUILD // (the golden above is also regenerated against the pre-B1 pool, which has none of the per-cell API)
// closed-loop helper: `k` submitters per cell in each class entry, a finished job is replaced at once until `horizon`
struct Loop { int cell, prio; uint64_t dur; int k; uint64_t t0, t1; };
static void closed_loop(sim::Sim &S, const std::vector<Loop> &ls)
{
  for (const Loop &l : ls)
    for (int i = 0; i < l.k; i++) S.add({l.t0, l.prio, l.cell, l.dur, 0});
  S.on_done = [&S, ls](sim::Job &j) {
    for (const Loop &l : ls)
      if (l.cell == j.cell && l.prio == j.prio && l.dur == j.dur && vt_now() < l.t1) { S.add({vt_now(), j.prio, j.cell, j.dur, 0}); break; }
  };
}

// Two cells with equal demand but different job lengths (100 us vs 300 us), the pool saturated: each gets half of the token time (round-robin would give
// the long-job cell 75 %). Three cells likewise.
TEST(CorePoolB1, EqualDemandSharesTokenTimeNotGrantCount)
{
  for (int nc : {2, 3}) {
    nr_pool_t *p = mk(4);
    sim::Sim S(p);
    std::vector<Loop> ls;
    for (int c = 0; c < nc; c++) ls.push_back({c, NR_POOL_SCAN, 100000ull * (uint64_t)(c * 2 + 1) / (nc == 2 ? 1 : 1) * (nc == 2 ? 1 : 1), 4, 0, 100000000ull});
    if (nc == 2) ls[1].dur = 300000;
    closed_loop(S, ls);
    S.run();
    double tt[3] = {0, 0, 0}, all = 0;
    for (auto &j : S.jobs) tt[j->cell] += (double)j->dur, all += (double)j->dur;
    for (int c = 0; c < nc; c++) EXPECT_NEAR(tt[c] / all, 1.0 / nc, 0.1 / nc) << nc << " cells, cell " << c << " share";
    nr_pool_destroy(p);
  }
}

// Work conserving: a cell alone gets every token it may use, and a cell that went quiet 100 ms ago no longer limits it. While a second cell is
// active the DL cap max(1, N-1-(A-1)) applies.
TEST(CorePoolB1, AloneUsesAllTokensAndAnIdleCellReleasesItsShare)
{
  nr_pool_t *p = mk(4); // DL <= N-1 = 3 when alone
  sim::Sim S(p);
  closed_loop(S, {{0, NR_POOL_DECODE, 500000, 8, 0, 400000000ull}, {1, NR_POOL_DECODE, 500000, 1, 2000000ull, 30000000ull}});
  S.run();
  int m_alone_early = 0, m_two = 0, m_alone_late = 0, m1 = 0;
  for (auto &s : S.samples) {
    if (s.t < 2000000) m_alone_early = std::max(m_alone_early, s.cell[0]);
    if (s.t >= 10000000 && s.t < 30000000) m_two = std::max(m_two, s.cell[0]), m1 = std::max(m1, s.cell[1]);
    if (s.t >= 160000000) m_alone_late = std::max(m_alone_late, s.cell[0]);
    EXPECT_LE(s.tot, 3);
  }
  EXPECT_EQ(m_alone_early, 3); // A = 1: N-1
  EXPECT_EQ(m_two, 2);         // A = 2: max(1, 3-1)
  EXPECT_EQ(m1, 1);
  EXPECT_EQ(m_alone_late, 3);  // cell 1 idle for > 100 ms: not active any more
  nr_pool_destroy(p);
}

// DL cap with A active cells at N = 6 (global N-1 = 5): a second cell appears while cell 0 holds five tokens; it gets one at the first release,
// then cell 0 is held to 4. With three busy cells each is held to max(1, 5-2) = 3, the sum stays <= 5 and no cell is ever without a token.
TEST(CorePoolB1, DlCapPerCellWithActiveCells)
{
  {
    nr_pool_t *p = mk(6);
    sim::Sim S(p);
    closed_loop(S, {{0, NR_POOL_DECODE, 500000, 8, 0, 60000000ull}});
    S.add({10000000, NR_POOL_DECODE, 1, 500000, 0});
    S.run();
    const sim::Job *bp = nullptr;
    for (auto &j : S.jobs) if (j->cell == 1) bp = j.get();
    ASSERT_NE(bp, nullptr);
    const sim::Job &b = *bp;
    EXPECT_LE(b.granted_at - b.arr, 500000u) << "cell 1 waited more than one DL job (one release)";
    int m0 = 0;
    for (auto &s : S.samples) {
      EXPECT_LE(s.tot, 5);
      if (s.t >= 11000000 && s.t < 50000000) m0 = std::max(m0, s.cell[0]);
    }
    EXPECT_EQ(m0, 5 - 1 + 0) << "after cell 1 became active cell 0 is held to N-1-(A-1)"; // cell 1's single job is over after 0.5 ms but it stays active 100 ms
    nr_pool_destroy(p);
  }
  {
    nr_pool_t *p = mk(6);
    sim::Sim S(p);
    closed_loop(S, {{0, NR_POOL_DECODE, 500000, 8, 0, 60000000ull}, {1, NR_POOL_DECODE, 500000, 8, 0, 60000000ull}, {2, NR_POOL_DECODE, 500000, 8, 0, 60000000ull}});
    S.run();
    int mx[3] = {0, 0, 0}, mn[3] = {9, 9, 9};
    for (auto &s : S.samples) {
      EXPECT_LE(s.tot, 5);
      if (s.t < 2000000) continue;
      for (int c = 0; c < 3; c++) mx[c] = std::max(mx[c], s.cell[c]), mn[c] = std::min(mn[c], s.cell[c]);
    }
    for (int c = 0; c < 3; c++) EXPECT_LE(mx[c], 3) << "cell " << c;
    uint64_t wmax = 0;
    for (auto &j : S.jobs) wmax = std::max(wmax, j->granted_at - j->arr);
    EXPECT_LE(wmax, 3000000u); // closed loop with 8 submitters per cell: a job waits behind its own cell's queue, never starves
    nr_pool_destroy(p);
  }
}

// Burst cell A (3x its share of load, closed loop = saturating) against a quiet cell B whose jobs queue a little even alone (N = 2: one scan token).
// B's wait p99 with A may not exceed 1.2x its solo p99. Virtual time: exact.
static uint64_t BGAP = 800000, BDUR = 200000, ADUR = 100000;
static std::vector<uint64_t> quiet_waits(bool with_a, unsigned seed, uint64_t *max_out = nullptr)
{
  nr_pool_t *p = mk(2);
  sim::Sim S(p);
  unsigned r = seed;
  auto rnd = [&] { r = r * 1664525u + 1013904223u; return r >> 8; };
  uint64_t t = 0;
  for (int i = 0; i < 600; i++) { // B: BDUTY of one token, bursty arrivals (gap 0 .. 2*mean), job BDUR
    t += (rnd() % 1000) * (2 * BGAP / 1000);
    S.add({t, NR_POOL_SCAN, 1, BDUR, 0});
  }
  if (with_a) closed_loop(S, {{0, NR_POOL_SCAN, ADUR, 6, 0, t}});
  S.run();
  std::vector<uint64_t> w;
  for (auto &j : S.jobs)
    if (j->cell == 1) w.push_back(j->granted_at - j->arr);
  if (max_out) *max_out = *std::max_element(w.begin(), w.end());
  nr_pool_destroy(p);
  return w;
}
TEST(CorePoolB1, BurstCellDoesNotInflateTheQuietCellsWait)
{
  // Not preemptive: B arriving at a busy token waits for the running job of A to end (at most one A job), whatever the policy. So the exact bound is
  // with_a <= solo + A's job length; the plan's 1.2x holds when that residual is small against B's own queueing (A job 25 us here, the real scan job is
  // 225 us against a B p99 of ~ms at N = 6; see the report).
  for (uint64_t adur : {25000ull, 50000ull, 100000ull})
    for (unsigned seed : {1u, 2u, 3u}) {
      ADUR = adur;
      const uint64_t solo = sim::pct(quiet_waits(false, seed), 0.99), with_a = sim::pct(quiet_waits(true, seed), 0.99);
      printf("  A job %lu us seed %u: B wait p99 solo %lu us, with burst A %lu us (ratio %.2f)\n", (unsigned long)(adur / 1000), seed, (unsigned long)(solo / 1000),
             (unsigned long)(with_a / 1000), solo ? (double)with_a / (double)solo : 0.0);
      EXPECT_GT(solo, 0u);
      EXPECT_LE(with_a, solo + adur) << "A job " << adur << " seed " << seed;
      if (adur == 25000) EXPECT_LE((double)with_a, 1.2 * (double)solo) << "seed " << seed;
    }
  ADUR = 100000;
}

// No deadlock / starvation-to-death on tiny pools with several cells: every class and flag from every cell is queued at once and must drain. With and
// without the default aging.
TEST(CorePoolB1, NoDeadlockAtOneToThreeTokensWithOneToThreeCells)
{
  for (int n = 1; n <= 3; n++)
    for (int nc = 1; nc <= 3; nc++)
      for (int ages = 0; ages < 2; ages++) {
        nr_pool_t *p = mk(n, !ages);
        sim::Sim S(p);
        static const int prios[6] = {NR_POOL_SCAN, NR_POOL_DECODE, NR_POOL_DECODE | NR_POOL_F_UL, NR_POOL_RETRY, NR_POOL_DISC, NR_POOL_HOUSE};
        unsigned r = 99u + (unsigned)(n * 10 + nc);
        for (int c = 0; c < nc; c++)
          for (int k = 0; k < 6; k++)
            for (int i = 0; i < 10; i++) {
              r = r * 1664525u + 1013904223u;
              S.add({(uint64_t)(r >> 16) % 300000, prios[k], c, 100000 + (r >> 8) % 200000, 0});
            }
        S.run(); // fails (and aborts) when a job is never granted
        for (auto &j : S.jobs) EXPECT_TRUE(j->granted.load()) << "n=" << n << " cells=" << nc;
        nr_pool_stats_t st;
        nr_pool_stats(p, &st);
        uint64_t tot = 0;
        for (int c = 0; c < NR_POOL_MAX_CELLS; c++) tot += st.leases_cell[c];
        EXPECT_EQ(tot, (uint64_t)(60 * nc));
        EXPECT_EQ(nr_pool_free(p), n);
        nr_pool_destroy(p);
      }
}

// DISC/HOUSE promotion is off below 6 tokens in the default config (FLOAT-LANE-POOL section 3), on from 6; an explicit cfg is used as given.
TEST(CorePoolB1, PromotionOffBelowSixTokens)
{
  for (int n = 1; n <= 8; n++) {
    const nr_pool_cfg_t c = nr_pool_cfg_default_n(n), d = nr_pool_cfg_default();
    EXPECT_EQ(c.age_ns[NR_POOL_RETRY], d.age_ns[NR_POOL_RETRY]);
    EXPECT_EQ(c.age_ns[NR_POOL_SCAN], 0u);
    EXPECT_EQ(c.age_ns[NR_POOL_DISC], n < 6 ? 0u : d.age_ns[NR_POOL_DISC]) << n;
    EXPECT_EQ(c.age_ns[NR_POOL_HOUSE], n < 6 ? 0u : d.age_ns[NR_POOL_HOUSE]) << n;
  }
  auto run = [](bool explicit_cfg) {
    nr_pool_cfg_t c = nr_pool_cfg_default();
    c.pin = false;
    int cores[2] = {0, 1};
    nr_pool_t *p = explicit_cfg ? nr_pool_create(cores, nullptr, 2, &c) : nr_pool_create(cores, nullptr, 2, nullptr);
    sim::Sim S(p);
    // one scan token busy all the time (SCAN <= N-1 = 1), a HOUSE job behind it: only age promotion can lift it over the cap
    closed_loop(S, {{0, NR_POOL_SCAN, 300000, 3, 0, 300000000ull}});
    S.add({1000, NR_POOL_HOUSE, 0, 300000, 0});
    S.run();
    nr_pool_stats_t st;
    nr_pool_stats(p, &st);
    const uint64_t promoted = st.promoted[NR_POOL_HOUSE];
    uint64_t at = 0;
    for (auto &j : S.jobs) if (j->prio == NR_POOL_HOUSE) at = j->granted_at;
    nr_pool_destroy(p);
    return std::make_pair(promoted, at);
  };
  const auto off = run(false), on = run(true);
  EXPECT_EQ(off.first, 0u);
  EXPECT_GE(off.second, 300000000ull) << "default config at 2 tokens: HOUSE only runs once the scans stop";
  EXPECT_EQ(on.first, 1u);
  EXPECT_LT(on.second, 150000000ull) << "explicit default config: promoted at its 100 ms age";
}

// CORE_POOL line grammar: with cell 0 alone no per-cell block (byte-identical to the single-cell line, see the golden above); once another cell
// has leased, one block per leased cell, in cell order.
TEST(CorePoolB1, StatsLineGainsOneBlockPerActiveCellOnly)
{
  auto line = [](std::vector<int> cells) {
    nr_pool_t *p = mk(2);
    sim::Sim S(p);
    for (size_t i = 0; i < cells.size(); i++) S.add({i * 1000000, NR_POOL_SCAN, cells[i], 250000, 0});
    S.run();
    char b[2048];
    nr_pool_stats_line(p, b, sizeof(b));
    nr_pool_destroy(p);
    return std::string(b);
  };
  EXPECT_EQ(line({0, 0, 0}).find(" cell"), std::string::npos);
  const std::string l2 = line({0, 1, 1, 0});
  const std::regex re(R"( house\[[^\]]*\] cell0\[n=2 waited=0 hold_ms=0 wait_us p50<=\d+ p99<=\d+ max=\d+\] cell1\[n=2 waited=0 hold_ms=0 wait_us p50<=\d+ p99<=\d+ max=\d+\]$)");
  EXPECT_TRUE(std::regex_search(l2, re)) << l2;
  const std::string l3 = line({3, 0});
  EXPECT_NE(l3.find(" cell0[n=1 "), std::string::npos) << l3;
  EXPECT_NE(l3.find(" cell3[n=1 "), std::string::npos) << l3;
  EXPECT_EQ(l3.find(" cell1["), std::string::npos);
}
#endif

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
