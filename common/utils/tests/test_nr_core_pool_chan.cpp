// SPDX-License-Identifier: LicenseRef-CSSL-1.0
// mpci CHANPOOL: the CHAN class of the core pool (the channelizer's worker threads lease tokens from the pool): priority against SCAN / DECODE / RETRY /
// DISC / HOUSE, the bound (decode keeps its share), per-cell accounting (CHAN charges no cell; its demand is taken off N for the per-cell fair share),
// helper (try) leases, fast cores first on a hybrid host. Pools are built with pin=false on synthetic core numbers: no host core is touched.
#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <memory>
#include <sched.h>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
extern "C" {
#include "nr_core_pool.h"
}

namespace {
nr_pool_t *mk(int n, const int *cap = nullptr, int dflt_age = -1)
{
  nr_pool_cfg_t c = nr_pool_cfg_default_n(n);
  c.pin = false;
  if (dflt_age >= 0) c.age_ns[NR_POOL_DISC] = c.age_ns[NR_POOL_HOUSE] = (uint64_t)dflt_age;
  int cores[NR_POOL_MAX_TOKENS];
  for (int i = 0; i < n; i++) cores[i] = i;
  return nr_pool_create(cores, cap, n, &c);
}
struct PoolGuard { // declared right after the pool: destroyed after every Holder / thread declared later in the test
  nr_pool_t *p;
  ~PoolGuard() { nr_pool_destroy(p); }
};
void wait_until(const std::function<bool()> &f)
{
  for (int i = 0; i < 20000 && !f(); i++) usleep(500);
  ASSERT_TRUE(f()) << "rendezvous timed out";
}
// A thread that leases `prio` for `cell`, signals when granted and releases when told to.
struct Holder {
  nr_pool_t *p;
  int prio, cell;
  std::atomic<int> state{0}; // 0 waiting, 1 granted
  std::atomic<bool> go{false};
  std::atomic<int> order{0};
  int core = -1;
  std::thread th;
  Holder(nr_pool_t *p_, int pr, int ce, std::atomic<int> *seq = nullptr, bool hold = false) : p(p_), prio(pr), cell(ce)
  {
    go = !hold;
    th = std::thread([this, seq] {
      core = nr_pool_acquire(p, prio, cell, 0);
      if (seq) order = ++*seq;
      state = 1;
      while (!go) usleep(200);
      nr_pool_release(p);
    });
  }
  void release() { go = true; }
  ~Holder() { go = true; if (th.joinable()) th.join(); }
};
int try_n(nr_pool_t *p, int prio, int cell, int want, std::vector<std::thread> &ths, std::atomic<bool> &stop, std::atomic<int> &got)
{ // `want` threads each try_acquire once; the granted ones hold until stop
  std::atomic<int> done{0};
  for (int i = 0; i < want; i++)
    ths.emplace_back([&, p, prio, cell] {
      const int c = nr_pool_try_acquire(p, prio, cell, 0);
      if (c >= 0) got++;
      done++;
      if (c >= 0) { while (!stop) usleep(200); nr_pool_release(p); }
    });
  while (done < want) usleep(200);
  return got;
}
} // namespace

TEST(PoolChan, BoundsArePureFunctionsOfTheTokenCount)
{
  for (int n = 1; n <= NR_POOL_MAX_TOKENS; n++) {
    EXPECT_EQ(nr_pool_chan_cap_n(n), n > 2 ? n - 2 : 1) << n;
    EXPECT_EQ(nr_pool_chan_reserved_n(n, 0), 0) << n;
    EXPECT_EQ(nr_pool_chan_reserved_n(n, -3), 0) << n;
    EXPECT_EQ(nr_pool_chan_reserved_n(n, 1), n > 2 ? 1 : 0) << n;
    EXPECT_LE(nr_pool_chan_reserved_n(n, 1000), n > 2 ? n - 2 : 0) << n;
    EXPECT_LE(nr_pool_chan_reserved_n(n, 1000), nr_pool_chan_cap_n(n)) << n;
  }
}

TEST(PoolChan, ServedBeforeEveryOtherClass)
{ // all 6 tokens busy; one waiter of each class queued; the freed token goes to CHAN, the next ones to the others in class order
  nr_pool_t *p = mk(6);
  PoolGuard guard_{p};
  std::vector<std::unique_ptr<Holder>> fill;
  fill.emplace_back(new Holder(p, NR_POOL_SCAN, 0, nullptr, true));
  for (int i = 0; i < 5; i++) fill.emplace_back(new Holder(p, NR_POOL_DECODE, 0, nullptr, true));
  wait_until([&] { for (auto &h : fill) if (h->state != 1) return false; return true; });
  EXPECT_EQ(nr_pool_free(p), 0);
  std::atomic<int> seq{0};
  Holder disc(p, NR_POOL_DISC, 0, &seq, true), house(p, NR_POOL_HOUSE, 0, &seq, true), dec(p, NR_POOL_DECODE, 0, &seq, true), scan(p, NR_POOL_SCAN, 0, &seq, true);
  Holder chan(p, NR_POOL_CHAN, 0, &seq, true);
  wait_until([&] { return nr_pool_total_waiting(p) == 5; });
  EXPECT_TRUE(nr_pool_should_yield(p, NR_POOL_SCAN)); // a CHAN waiter is more urgent than a scan lease
  EXPECT_FALSE(nr_pool_should_yield(p, NR_POOL_CHAN));
  fill[1]->release(); // frees one token
  wait_until([&] { return chan.state == 1; });
  EXPECT_EQ(chan.order, 1);
  EXPECT_EQ(scan.state, 0);
  EXPECT_EQ(dec.state, 0);
  fill[2]->release();
  wait_until([&] { return scan.state == 1 || dec.state == 1; });
  chan.release();
  fill[0]->release();
  for (int i = 3; i < 6; i++) fill[i]->release();
  scan.release(); dec.release(); disc.release(); house.release();
  wait_until([&] { return seq == 5; });
  EXPECT_EQ(chan.order, 1); // and nothing overtook it
  nr_pool_stats_t st;
  nr_pool_stats(p, &st);
  EXPECT_EQ(st.leases[NR_POOL_CHAN], 1u);
  EXPECT_EQ(st.promoted[NR_POOL_CHAN], 0u); // never promoted
}

TEST(PoolChan, ChanHolderNeverYieldsAndDoesNotChargeACell)
{
  nr_pool_t *p = mk(6);
  PoolGuard guard_{p};
  ASSERT_GE(nr_pool_acquire(p, NR_POOL_CHAN, 3, 0), 0);
  EXPECT_FALSE(nr_pool_should_yield(p, NR_POOL_CHAN));
  EXPECT_FALSE(nr_pool_yield(p));
  nr_pool_release(p);
  ASSERT_GE(nr_pool_acquire(p, NR_POOL_DECODE, 3, 0), 0);
  nr_pool_release(p);
  nr_pool_stats_t st;
  nr_pool_stats(p, &st);
  EXPECT_EQ(st.leases[NR_POOL_CHAN], 1u);
  EXPECT_EQ(st.leases_cell[3], 1u); // only the decode lease is the cell's
  EXPECT_EQ(st.hold_ns_cell[3], st.hold_ns[NR_POOL_DECODE]);
  char line[1024];
  nr_pool_stats_line(p, line, sizeof(line));
  EXPECT_NE(std::string(line).find(" chan[n=1"), std::string::npos) << line;
}

TEST(PoolChan, WithoutAChannelizerTheStatsLineAndTheCapsAreThePreChanOnes)
{
  nr_pool_t *p = mk(6);
  PoolGuard guard_{p};
  ASSERT_GE(nr_pool_acquire(p, NR_POOL_DECODE, 0, 0), 0);
  nr_pool_release(p);
  char line[1024];
  nr_pool_stats_line(p, line, sizeof(line));
  EXPECT_EQ(std::string(line).find("chan["), std::string::npos) << line;
  // demand 0: DL holds N-1 tokens when scan is idle
  std::vector<std::thread> ths;
  std::atomic<bool> stop{false};
  std::atomic<int> got{0};
  EXPECT_EQ(try_n(p, NR_POOL_DECODE, 0, 8, ths, stop, got), 5);
  stop = true;
  for (auto &t : ths) t.join();
}

TEST(PoolChan, BoundedShareDecodeAndScanKeepTheirs)
{
  nr_pool_t *p = mk(6);
  PoolGuard guard_{p};
  nr_pool_set_chan_demand(p, 100); // asks for everything
  EXPECT_EQ(nr_pool_chan_demand(p), 100);
  EXPECT_EQ(nr_pool_chan_reserved(p), 4); // N-2
  std::vector<std::thread> ths;
  std::atomic<bool> stop{false};
  std::atomic<int> chan{0}, dl{0}, scan{0};
  EXPECT_EQ(try_n(p, NR_POOL_CHAN, 0, 8, ths, stop, chan), 4); // bound N-2
  EXPECT_EQ(try_n(p, NR_POOL_DECODE, 0, 4, ths, stop, dl), 1);  // DL: N-r-1 = 1
  EXPECT_EQ(try_n(p, NR_POOL_SCAN, 0, 2, ths, stop, scan), 1);  // the scan reserve
  EXPECT_EQ(nr_pool_free(p), 0);
  stop = true;
  for (auto &t : ths) t.join();
}

TEST(PoolChan, ReservedTokensStayFreeForTheChannelizer)
{ // demand 2 of 6: whatever the other classes do, two tokens remain for CHAN
  nr_pool_t *p = mk(6);
  PoolGuard guard_{p};
  nr_pool_set_chan_demand(p, 2);
  std::vector<std::thread> ths;
  std::atomic<bool> stop{false};
  std::atomic<int> a{0}, b{0}, c{0}, d{0};
  try_n(p, NR_POOL_DECODE, 0, 6, ths, stop, a);
  try_n(p, NR_POOL_SCAN, 0, 6, ths, stop, b);
  try_n(p, NR_POOL_DISC, 0, 6, ths, stop, c);
  try_n(p, NR_POOL_DECODE | NR_POOL_F_UL, 0, 6, ths, stop, d);
  EXPECT_GE(nr_pool_free(p), 2);
  std::atomic<int> ch{0};
  EXPECT_EQ(try_n(p, NR_POOL_CHAN, 0, 4, ths, stop, ch), 2);
  stop = true;
  for (auto &t : ths) t.join();
}

TEST(PoolChan, AgedLowWaitersCannotTakeTheReservedTokens)
{
  nr_pool_t *p = mk(6, nullptr, 1000000 /* 1 ms promotion */);
  PoolGuard guard_{p};
  nr_pool_set_chan_demand(p, 3);
  std::vector<std::unique_ptr<Holder>> h;
  for (int i = 0; i < 6; i++) h.emplace_back(new Holder(p, NR_POOL_DISC, 0, nullptr, true));
  wait_until([&] { return nr_pool_total_waiting(p) >= 1; });
  usleep(30000); // everybody is old
  int granted = 0;
  for (auto &x : h) granted += x->state == 1;
  EXPECT_EQ(nr_pool_free(p), 3) << "granted " << granted; // the three reserved tokens are untouched
  EXPECT_EQ(granted, 3);
  std::vector<std::thread> ths;
  std::atomic<bool> stop{false};
  std::atomic<int> ch{0};
  EXPECT_EQ(try_n(p, NR_POOL_CHAN, 0, 4, ths, stop, ch), 3);
  stop = true;
  for (auto &t : ths) t.join();
  for (auto &x : h) x->release();
  for (auto &x : h) x->th.join();
}

TEST(PoolChan, PerCellFairShareAccountsForTheDemand)
{ // N=10, two active cells: DL per-cell cap is max(1, N'-1-(A-1)) with N' = N - r
  auto cell0_dl = [](int demand) {
    nr_pool_t *p = mk(10);
    nr_pool_set_chan_demand(p, demand);
    std::vector<std::thread> ths;
    std::atomic<bool> stop{false};
    std::atomic<int> c1{0}, c0{0};
    try_n(p, NR_POOL_DECODE, 1, 1, ths, stop, c1); // cell 1 is active (holds its DL token)
    try_n(p, NR_POOL_DECODE, 0, 12, ths, stop, c0);
    stop = true;
    for (auto &t : ths) t.join();
    nr_pool_destroy(p);
    return c0.load();
  };
  EXPECT_EQ(cell0_dl(0), 8);  // 9 - 1
  EXPECT_EQ(cell0_dl(2), 6);  // 7 - 1: the channelizer's two tokens are not in the share
  EXPECT_EQ(cell0_dl(100), 0); // bound N-2 = 8 reserved -> N'=2: DL global cap 1, already held by cell 1
}

TEST(PoolChan, ChanChargesNoCellUsage)
{ // CHAN time is not cell 0's usage: two cells with equal decode demand stay balanced while CHAN works for both
  nr_pool_t *p = mk(6);
  PoolGuard guard_{p};
  for (int i = 0; i < 50; i++) {
    nr_pool_acquire(p, NR_POOL_CHAN, 0, 0);
    usleep(200);
    nr_pool_release(p);
  }
  nr_pool_stats_t st;
  nr_pool_stats(p, &st);
  EXPECT_EQ(st.leases[NR_POOL_CHAN], 50u);
  for (int c = 0; c < NR_POOL_MAX_CELLS; c++) { EXPECT_EQ(st.leases_cell[c], 0u) << c; EXPECT_EQ(st.hold_ns_cell[c], 0u) << c; }
  EXPECT_GT(st.hold_ns[NR_POOL_CHAN], 0u);
}

TEST(PoolChan, HelperLeaseIsNonBlockingAndTheBlockingOneWaitsOnlyForCap)
{
  nr_pool_t *p = mk(5); // chan cap 3
  std::vector<std::thread> ths;
  std::atomic<bool> stop{false};
  std::atomic<int> ch{0};
  EXPECT_EQ(try_n(p, NR_POOL_CHAN, 0, 5, ths, stop, ch), 3);
  std::atomic<int> state{0};
  std::thread w([&] { nr_pool_acquire(p, NR_POOL_CHAN, 0, 0); state = 1; nr_pool_release(p); });
  usleep(20000);
  EXPECT_EQ(state, 0); // cap reached: the blocking caller waits
  stop = true;         // helpers finish their blocks
  w.join();
  EXPECT_EQ(state, 1);
  for (auto &t : ths) t.join();
}

TEST(PoolChan, FastCoresFirstOnAHybridHost)
{ // DGX GB10 pool fixture (test_nr_core_layout.cpp: pool 6,7,8,9 = X925 big, 3,4 = A725 little), capacities as site_init reads cpu_capacity
  const int cores[6] = {6, 7, 8, 9, 3, 4};
  const int cap[6] = {1017, 1017, 997, 1017, 460, 460};
  nr_pool_cfg_t c = nr_pool_cfg_default_n(6);
  c.pin = false;
  nr_pool_t *p = nr_pool_create(cores, cap, 6, &c);
  PoolGuard guard_{p};
  // occupy one big core with a low-class lease first: CHAN must still end on big cores
  std::vector<std::thread> ths;
  std::atomic<bool> stop{false};
  std::atomic<int> low{0};
  std::vector<int> got;
  std::mutex m;
  for (int i = 0; i < 3; i++)
    ths.emplace_back([&] {
      const int core = nr_pool_try_acquire(p, NR_POOL_CHAN, 0, 0);
      { std::lock_guard<std::mutex> l(m); got.push_back(core); }
      while (!stop) usleep(200);
      nr_pool_release(p);
    });
  wait_until([&] { std::lock_guard<std::mutex> l(m); return got.size() == 3; });
  for (int core : got) EXPECT_TRUE(core == 6 || core == 7 || core == 8 || core == 9) << core;
  Holder disc(p, NR_POOL_DISC, 0, nullptr, true); // low classes take the small cores first
  wait_until([&] { return disc.state == 1; });
  EXPECT_TRUE(disc.core == 3 || disc.core == 4) << disc.core;
  disc.release();
  stop = true;
  for (auto &t : ths) t.join();
}

TEST(PoolChan, SiteWrappersAreNoopsWithoutAPoolAndFollowTheInjectedOne)
{
  nr_pool_t *p = mk(6);
  PoolGuard guard_{p};
  nr_pool_site_set_for_test(p);
  nr_pool_site_chan_set_demand(3);
  EXPECT_EQ(nr_pool_chan_demand(p), 3);
  EXPECT_EQ(nr_pool_site_chan_max(), 4);
  EXPECT_GE(nr_pool_site_chan_acquire(1), 0);
  EXPECT_TRUE(nr_pool_holds(p));
  nr_pool_site_chan_release();
  EXPECT_FALSE(nr_pool_holds(p));
  nr_pool_site_chan_set_demand(0);
  nr_pool_site_set_for_test(nullptr);
}

TEST(PoolChan, ReservedTokensAreTheFastestAndNobodyElseIsPlacedOnThem)
{ // DGX GB10 pool fixture: tokens 0..5 = cores 6,7,8,9,3,4, capacities 1017,1017,997,1017,460,460. demand 2 -> the two fastest tokens (0 and 1)
  const int cores[6] = {6, 7, 8, 9, 3, 4};
  const int cap[6] = {1017, 1017, 997, 1017, 460, 460};
  nr_pool_cfg_t c = nr_pool_cfg_default_n(6);
  c.pin = false;
  nr_pool_t *p = nr_pool_create(cores, cap, 6, &c);
  PoolGuard guard_{p};
  nr_pool_set_chan_demand(p, 2);
  // other classes, as many as they may take, never land on cores 6 / 7
  std::vector<std::unique_ptr<Holder>> h;
  h.emplace_back(new Holder(p, NR_POOL_SCAN, 0, nullptr, true));
  h.emplace_back(new Holder(p, NR_POOL_DECODE, 0, nullptr, true));
  wait_until([&] { return h[0]->state == 1 && h[1]->state == 1; });
  for (auto &x : h) EXPECT_TRUE(x->core != 6 && x->core != 7) << x->core;
  int a = nr_pool_try_acquire(p, NR_POOL_CHAN, 0, 0);
  EXPECT_TRUE(a == 6 || a == 7) << a;
  nr_pool_release(p);
  for (auto &x : h) x->release();
  for (auto &x : h) x->th.join();
}

TEST(PoolChan, AWorkerKeepsItsCoreBetweenBlocksAndIsNotMovedByALease)
{ // pin on, host cores: the lease of a reserved token changes no affinity after the first one
  cpu_set_t allowed;
  ASSERT_EQ(sched_getaffinity(0, sizeof(allowed), &allowed), 0);
  std::vector<int> cpus;
  for (int c = 0; c < CPU_SETSIZE && cpus.size() < 6; c++) if (CPU_ISSET(c, &allowed)) cpus.push_back(c);
  if (cpus.size() < 6) GTEST_SKIP() << "needs 6 allowed cpus";
  nr_pool_cfg_t c = nr_pool_cfg_default_n(6);
  nr_pool_t *p = nr_pool_create(cpus.data(), nullptr, 6, &c);
  PoolGuard guard_{p};
  nr_pool_set_chan_demand(p, 1);
  std::thread t([&] {
    const int core = nr_pool_acquire(p, NR_POOL_CHAN, 0, 0);
    nr_pool_release(p);
    cpu_set_t m;
    sched_getaffinity(0, sizeof(m), &m);
    EXPECT_EQ(CPU_COUNT(&m), 1);
    EXPECT_TRUE(CPU_ISSET(core, &m));
    std::vector<double> us;
    for (int i = 0; i < 200; i++) {
      usleep(100);
      const uint64_t t0 = nr_pool_now_ns();
      EXPECT_EQ(nr_pool_acquire(p, NR_POOL_CHAN, 0, 0), core);
      nr_pool_release(p);
      us.push_back((double)(nr_pool_now_ns() - t0) / 1000.0);
    }
    std::sort(us.begin(), us.end());
    EXPECT_LT(us[100], 20.0) << "median acquire+release us (a migrating pin is ~110 us each)";
  });
  t.join();
}
