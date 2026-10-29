// SPDX-License-Identifier: LicenseRef-CSSL-1.0
// mpci B6: the core pool against the demand of N cells. Value order under overload (DISC / HOUSE are not promoted into the urgent classes' tokens when the cells
// have fewer than NR_POOL_PROMO_MIN_TOKENS tokens each; the background classes are shed lowest value first by the admission gate), single cell unchanged.
// Pools use synthetic token numbers 0..n-1 (the derived config pins the lessee to its token: the cores exist on any host with >= n cpus; a host with fewer
// still passes, the pin simply fails).
#include <gtest/gtest.h>
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <functional>
#include <unistd.h>
extern "C" {
#include "nr_core_pool.h"
#include "nr_admit.h"
}

namespace {
nr_pool_t *mk_derived(int n)
{
  int cores[NR_POOL_MAX_TOKENS];
  for (int i = 0; i < n; i++) cores[i] = i;
  return nr_pool_create(cores, nullptr, n, nullptr); // cfg == NULL: the derived config (promotion follows the tokens per active cell)
}
nr_pool_t *mk_explicit(int n)
{
  nr_pool_cfg_t c = nr_pool_cfg_default();
  c.pin = false;
  int cores[NR_POOL_MAX_TOKENS];
  for (int i = 0; i < n; i++) cores[i] = i;
  return nr_pool_create(cores, nullptr, n, &c);
}
void wait_until(const std::function<bool()> &f)
{
  for (int i = 0; i < 20000 && !f(); i++) usleep(500);
  ASSERT_TRUE(f()) << "rendezvous timed out";
}
struct Holder {
  nr_pool_t *p;
  int prio, cell;
  std::atomic<int> state{0};
  std::atomic<bool> go{false};
  std::atomic<int> order{0};
  std::thread th;
  Holder(nr_pool_t *p_, int pr, int ce, std::atomic<int> *seq = nullptr, bool hold = true) : p(p_), prio(pr), cell(ce)
  {
    go = !hold;
    th = std::thread([this, seq] {
      nr_pool_acquire(p, prio, cell, 0);
      if (seq) order = ++*seq;
      state = 1;
      while (!go) usleep(200);
      nr_pool_release(p);
    });
  }
  void release() { go = true; }
  ~Holder() { go = true; if (th.joinable()) th.join(); }
};
struct PoolGuard {
  nr_pool_t *p;
  ~PoolGuard() { nr_pool_destroy(p); }
};
// n-1 SCAN holders (cell alternates over `cells`) and one UL decode: all n - r tokens busy
std::vector<std::unique_ptr<Holder>> fill_all(nr_pool_t *p, int busy, int cells)
{
  std::vector<std::unique_ptr<Holder>> v;
  for (int i = 0; i < busy - 1; i++) v.emplace_back(new Holder(p, NR_POOL_SCAN, i % cells));
  v.emplace_back(new Holder(p, NR_POOL_DECODE | NR_POOL_F_UL, (busy - 1) % cells));
  wait_until([&] { for (auto &h : v) if (h->state != 1) return false; return true; });
  return v;
}
} // namespace

TEST(PoolBudget, PromotionRuleIsPureAndMeasured)
{
  EXPECT_TRUE(nr_pool_promo_allowed_m(6, 1.9)); // one token of headroom over the measured essential demand
  EXPECT_TRUE(nr_pool_promo_allowed_m(6, 5.0));
  EXPECT_FALSE(nr_pool_promo_allowed_m(6, 5.1));
  EXPECT_FALSE(nr_pool_promo_allowed_m(9, 8.5)); // DGX, 2 cells, CHAN reserve 1: 9 tokens
  EXPECT_TRUE(nr_pool_promo_allowed_m(9, 3.8));
  EXPECT_FALSE(nr_pool_promo_allowed_m(4, 4.0));
}

TEST(PoolBudget, ShedTiersArePureAndMonotone)
{
  for (int urgent = 0; urgent < 6; urgent++) {
    EXPECT_EQ(nr_pool_shed_tiers_n(urgent, 0, 1), 0) << "one cell: never";
    EXPECT_EQ(nr_pool_shed_tiers_n(urgent, 1, 2), 0) << "a free token: the urgent waiters are capped, not starved";
    EXPECT_EQ(nr_pool_shed_tiers_n(urgent, 0, 2), urgent > NR_POOL_SHED_TIERS ? NR_POOL_SHED_TIERS : urgent);
  }
  EXPECT_EQ(nr_pool_shed_tiers_n(0, 0, 3), 0);
  EXPECT_EQ(nr_pool_shed_tiers_n(1, 0, 3), 1);
  EXPECT_EQ(nr_pool_shed_tiers_n(2, 0, 3), 2);
  EXPECT_EQ(nr_pool_shed_tiers_n(9, 0, 3), NR_POOL_SHED_TIERS);
}

// two cells on a derived pool of 6 (3 tokens each): a DISC waiter that has outlived its promotion age does NOT overtake a SCAN waiter
TEST(PoolBudget, TwoCellsDoNotPromoteDiscIntoTheUrgentClasses)
{
  nr_pool_t *p = mk_derived(7);
  PoolGuard g{p};
  nr_pool_set_chan_demand(p, 1); // the channelizer's token: 6 left for the cells, 2 cells -> 3 each < 6
  ASSERT_EQ(nr_pool_chan_reserved(p), 1);
  auto fill = fill_all(p, 6, 2);
  ASSERT_EQ(nr_pool_active_cells(p), 2);
  std::atomic<int> seq{0};
  Holder disc(p, NR_POOL_DISC, 1, &seq);
  wait_until([&] { return nr_pool_total_waiting(p) == 1; });
  usleep(40000); // past the DISC promotion age (20 ms)
  Holder scan(p, NR_POOL_SCAN, 0, &seq);
  wait_until([&] { return nr_pool_total_waiting(p) == 2; });
  fill[0]->release(); // a SCAN holder frees its token
  wait_until([&] { return scan.state == 1 || disc.state == 1; });
  EXPECT_EQ(scan.state, 1) << "the urgent class takes the token";
  EXPECT_EQ(disc.state, 0) << "not promoted: discovery waits, it is shed first";
  nr_pool_stats_t st;
  nr_pool_stats(p, &st);
  EXPECT_EQ(st.promoted[NR_POOL_DISC], 0u);
  for (auto &h : fill) h->release();
  scan.release();
  disc.release();
}

// the same state with ONE active cell and no channelizer: the promotion bound of before (the overdue DISC waiter takes the token)
TEST(PoolBudget, OneCellKeepsTheAgePromotionOfBefore)
{
  nr_pool_t *p = mk_derived(6);
  PoolGuard g{p};
  auto fill = fill_all(p, 6, 1);
  ASSERT_EQ(nr_pool_active_cells(p), 1);
  std::atomic<int> seq{0};
  Holder disc(p, NR_POOL_DISC, 0, &seq);
  wait_until([&] { return nr_pool_total_waiting(p) == 1; });
  usleep(40000);
  Holder scan(p, NR_POOL_SCAN, 0, &seq);
  wait_until([&] { return nr_pool_total_waiting(p) == 2; });
  fill[0]->release();
  wait_until([&] { return scan.state == 1 || disc.state == 1; });
  EXPECT_EQ(disc.state, 1) << "N = 1: the starvation bound of discovery is unchanged";
  EXPECT_EQ(scan.state, 0);
  nr_pool_stats_t st;
  nr_pool_stats(p, &st);
  EXPECT_EQ(st.promoted[NR_POOL_DISC], 1u);
  for (auto &h : fill) h->release();
  scan.release();
  disc.release();
}

// an explicit config is used as given (tests and tools that build a pool by hand keep their promotion at any cell count)
TEST(PoolBudget, ExplicitConfigIsNotOverridden)
{
  nr_pool_t *p = mk_explicit(6);
  PoolGuard g{p};
  auto fill = fill_all(p, 6, 2);
  ASSERT_EQ(nr_pool_active_cells(p), 2);
  std::atomic<int> seq{0};
  Holder disc(p, NR_POOL_DISC, 1, &seq);
  wait_until([&] { return nr_pool_total_waiting(p) == 1; });
  usleep(40000);
  Holder scan(p, NR_POOL_SCAN, 0, &seq);
  wait_until([&] { return nr_pool_total_waiting(p) == 2; });
  fill[0]->release();
  wait_until([&] { return scan.state == 1 || disc.state == 1; });
  EXPECT_EQ(disc.state, 1);
  for (auto &h : fill) h->release();
  scan.release();
  disc.release();
}

TEST(PoolBudget, ShedTiersFollowTheUrgentWaiters)
{
  nr_pool_t *p = mk_derived(6);
  PoolGuard g{p};
  auto fill = fill_all(p, 6, 2);
  EXPECT_EQ(nr_pool_shed_tiers(p), 0) << "saturated but nobody urgent waits";
  Holder scan(p, NR_POOL_SCAN, 0);
  wait_until([&] { return nr_pool_total_waiting(p) == 1; });
  EXPECT_EQ(nr_pool_shed_tiers(p), 1);
  Holder dec(p, NR_POOL_DECODE, 1);
  wait_until([&] { return nr_pool_total_waiting(p) == 2; });
  EXPECT_EQ(nr_pool_shed_tiers(p), 2);
  Holder retry(p, NR_POOL_RETRY, 0);
  wait_until([&] { return nr_pool_total_waiting(p) == 3; });
  EXPECT_EQ(nr_pool_shed_tiers(p), 3);
  Holder disc(p, NR_POOL_DISC, 0); // a background waiter is not urgent
  wait_until([&] { return nr_pool_total_waiting(p) == 4; });
  EXPECT_EQ(nr_pool_shed_tiers(p), 3);
  for (auto &h : fill) h->release();
  scan.release(); dec.release(); retry.release(); disc.release();
}

TEST(PoolBudget, ShedTiersAreOffForOneCell)
{
  nr_pool_t *p = mk_derived(6);
  PoolGuard g{p};
  auto fill = fill_all(p, 6, 1);
  Holder scan(p, NR_POOL_SCAN, 0);
  wait_until([&] { return nr_pool_total_waiting(p) == 1; });
  EXPECT_EQ(nr_pool_shed_tiers(p), 0);
  for (auto &h : fill) h->release();
  scan.release();
}

// the admission gate (openair1/PHY/TOOLS/nr_admit.c) on the site pool: value order, classes that are never shed, counters, single cell
TEST(PoolBudget, AdmissionSkipsTheLowestValueFirstAndNeverTheDecodeGatingHelpers)
{
  nr_pool_t *p = mk_derived(6);
  PoolGuard g{p};
  nr_pool_site_set_for_test(p);
  struct Reset { ~Reset() { nr_pool_site_set_for_test(nullptr); } } reset;
  EXPECT_EQ(nr_admit_on(), 0) << "no cell active yet";
  auto fill = fill_all(p, 6, 2);
  EXPECT_EQ(nr_admit_on(), 1) << "two active cells";
  for (int c = 0; c < NR_ADMIT_N; c++) EXPECT_EQ(nr_admit_try((nr_admit_class_t)c, 0, 0, 0), 1) << "saturated, nobody urgent waits: everything runs";
  Holder scan(p, NR_POOL_SCAN, 0);
  wait_until([&] { return nr_pool_total_waiting(p) == 1; });
  // one urgent waiter: tier 0 only
  EXPECT_EQ(nr_admit_try(NR_ADMIT_UL_DISC, 0, 0, 0), 0);
  EXPECT_EQ(nr_admit_try(NR_ADMIT_CSIRS, 0, 0, 0), 1);
  EXPECT_EQ(nr_admit_try(NR_ADMIT_JANON, 0, 0, 0), 1);
  Holder dec(p, NR_POOL_DECODE, 1);
  wait_until([&] { return nr_pool_total_waiting(p) == 2; });
  EXPECT_EQ(nr_admit_try(NR_ADMIT_CSIRS, 0, 0, 0), 0);
  EXPECT_EQ(nr_admit_try(NR_ADMIT_DMRS_ID, 0, 0, 0), 0);
  EXPECT_EQ(nr_admit_try(NR_ADMIT_NID, 0, 0, 0), 1);
  Holder retry(p, NR_POOL_RETRY, 0);
  wait_until([&] { return nr_pool_total_waiting(p) == 3; });
  EXPECT_EQ(nr_admit_try(NR_ADMIT_NID, 0, 0, 0), 0);
  EXPECT_EQ(nr_admit_try(NR_ADMIT_JANON, 0, 0, 0), 0);
  // never shed, at any pressure: PDSCH CRC is never traded for latency
  for (auto c : {NR_ADMIT_USS, NR_ADMIT_DMRS_SCAN, NR_ADMIT_PROBE}) {
    EXPECT_EQ(nr_admit_tier(c), -1);
    EXPECT_EQ(nr_admit_try(c, 0, 0, 0), 1);
  }
  char s[256];
  nr_admit_stats(s, sizeof(s));
  EXPECT_NE(std::string(s).find("tier0(housekeeping)=1 tier1(csirs,dmrs_id)=2 tier2(janon,nid)=2"), std::string::npos) << s;
  for (auto &h : fill) h->release();
  scan.release(); dec.release(); retry.release();
}

TEST(PoolBudget, AdmissionRunsEverythingWithOneCell)
{
  nr_pool_t *p = mk_derived(6);
  PoolGuard g{p};
  nr_pool_site_set_for_test(p);
  struct Reset { ~Reset() { nr_pool_site_set_for_test(nullptr); } } reset;
  auto fill = fill_all(p, 6, 1);
  Holder scan(p, NR_POOL_SCAN, 0);
  wait_until([&] { return nr_pool_total_waiting(p) == 1; });
  EXPECT_EQ(nr_admit_on(), 0);
  for (int c = 0; c < NR_ADMIT_N; c++) EXPECT_EQ(nr_admit_try((nr_admit_class_t)c, 0, 0, 0), 1);
  char s[64];
  nr_admit_stats(s, sizeof(s));
  EXPECT_STREQ(s, "");
  for (auto &h : fill) h->release();
  scan.release();
}

TEST(PoolBudget, StatsGrammarOfTheCorePoolLineIsUnchanged)
{ // the shed counters are not part of the CORE_POOL line (A9 compares it)
  nr_pool_t *p = mk_derived(6);
  PoolGuard g{p};
  nr_pool_shed_note(p, 1);
  char line[2048];
  nr_pool_stats_line(p, line, sizeof(line));
  EXPECT_EQ(std::string(line).find("shed"), std::string::npos) << line;
  EXPECT_EQ(std::string(line).find("tier"), std::string::npos) << line;
}

namespace {
std::atomic<uint64_t> g_vnow{1000000000ull};
uint64_t vclock() { return g_vnow.load(); }
} // namespace

// pause / resume of a cell (A12 hooks): the channelizer's reserved token goes back with the demand (nr_pool_set_chan_demand(0)), and a cell that stopped leasing
// stops counting as active after the active window (100 ms): the promotion bound and the shed gate follow the cells that really run.
TEST(PoolBudget, PromotionFollowsTheMeasuredDemandNotAFixedTokenCount)
{
  // phase 0: two active cells whose demand is not measured yet: sheddable work does not jump the queue (blocked), 12 tokens and a CHAN reserve of 1.
  // phase 1: both cells measured at ~0.5 token (virtual clock), no reserve: 12 - 1.0 >= 1 token of headroom, the aged discovery waiter is promoted again.
  for (int phase = 0; phase < 2; phase++) {
    if (phase == 1) { nr_pool_set_clock_for_test(vclock); g_vnow = 1000000000ull; }
    {
      nr_pool_t *p = mk_derived(12);
      PoolGuard g{p};
      if (phase == 0) nr_pool_set_chan_demand(p, 1);
      EXPECT_EQ(nr_pool_chan_reserved(p), phase == 0 ? 1 : 0);
      if (phase == 1) {
        for (int c = 0; c < 2; c++) {
          ASSERT_GE(nr_pool_acquire(p, NR_POOL_SCAN, c, 0), 0);
          g_vnow += 600000000ull;
          nr_pool_release(p);
        }
        double e, sh;
        ASSERT_EQ(nr_pool_cell_demand(p, 0, &e, &sh), 1);
        ASSERT_EQ(nr_pool_cell_demand(p, 1, &e, &sh), 1);
      }
      auto fill = fill_all(p, phase == 0 ? 11 : 12, 2);
      ASSERT_EQ(nr_pool_active_cells(p), 2);
      std::atomic<int> seq{0};
      Holder disc(p, NR_POOL_DISC, 1, &seq);
      wait_until([&] { return nr_pool_total_waiting(p) == 1; });
      if (phase == 0) usleep(40000); else g_vnow += 40000000ull;
      Holder scan(p, NR_POOL_SCAN, 0, &seq);
      wait_until([&] { return nr_pool_total_waiting(p) == 2; });
      fill[0]->release();
      wait_until([&] { return scan.state == 1 || disc.state == 1; });
      if (phase == 0) { EXPECT_EQ(scan.state, 1); EXPECT_EQ(disc.state, 0); }
      else { EXPECT_EQ(disc.state, 1) << "measured headroom: the aged discovery waiter is promoted"; EXPECT_EQ(scan.state, 0); }
      for (auto &h : fill) h->release();
      scan.release();
      disc.release();
    }
    if (phase == 1) nr_pool_set_clock_for_test(nullptr);
  }
}

TEST(PoolBudget, ACellThatStoppedLeasingStopsCountingAfterTheActiveWindow)
{
  nr_pool_t *p = mk_derived(6);
  PoolGuard g{p};
  { Holder a(p, NR_POOL_SCAN, 0, nullptr, false), b(p, NR_POOL_SCAN, 1, nullptr, false); wait_until([&] { return a.state == 1 && b.state == 1; }); }
  EXPECT_EQ(nr_pool_active_cells(p), 2) << "within the window";
  usleep(150000);
  EXPECT_EQ(nr_pool_active_cells(p), 1) << "both cells idle for > 100 ms: only the floor of one remains";
  { Holder a(p, NR_POOL_SCAN, 0, nullptr, false); wait_until([&] { return a.state == 1; }); }
  usleep(20000);
  EXPECT_EQ(nr_pool_active_cells(p), 1);
}

// measured demand: a cell that held one token for 0.5 s of a 1.0 s window uses 0.5 tokens; essential and sheddable classes are apart; unobserved cells are not measured
TEST(PoolBudget, CellDemandIsMeasuredFromTheLeaseHoldTime)
{
  nr_pool_set_clock_for_test(vclock);
  g_vnow = 1000000000ull;
  nr_pool_t *p = mk_explicit(6);
  PoolGuard g{p};
  double e = -1, s = -1;
  EXPECT_EQ(nr_pool_cell_demand(p, 0, &e, &s), 0);
  EXPECT_EQ(e, 0.0);
  ASSERT_GE(nr_pool_acquire(p, NR_POOL_DECODE, 0, 0), 0);
  g_vnow += 500000000ull;
  nr_pool_release(p);
  ASSERT_GE(nr_pool_acquire(p, NR_POOL_DISC, 1, 0), 0);
  g_vnow += 250000000ull;
  nr_pool_release(p);
  g_vnow += 250000000ull; // 1.0 s since cell 0 first ran
  EXPECT_EQ(nr_pool_cell_demand(p, 0, &e, &s), 1);
  EXPECT_NEAR(e, 0.5, 0.05);
  EXPECT_NEAR(s, 0.0, 1e-9);
  EXPECT_EQ(nr_pool_cell_demand(p, 1, &e, &s), 1); // cell 1: 0.25 s of DISC over its 0.5 s window
  EXPECT_NEAR(e, 0.0, 1e-9);
  EXPECT_GT(s, 0.3);
  EXPECT_EQ(nr_pool_cell_demand(p, 2, &e, &s), 0); // never ran
  g_vnow += 60000000000ull; // a quiet minute: the demand decays away
  EXPECT_EQ(nr_pool_cell_demand(p, 0, &e, &s), 1);
  EXPECT_LT(e, 0.01);
  nr_pool_set_clock_for_test(nullptr);
}
