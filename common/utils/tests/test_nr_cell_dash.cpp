// SPDX-License-Identifier: LicenseRef-CSSL-1.0
// MPCI-DASH (docs/superpowers/reports/csd/MPCI-DASH-20261027.md section 3.1): formatting and gating of the per-cell log lines.
//  - every formatter writes NOTHING at n_cells == 1 (the N=1 log is unchanged) and its line at n_cells == 2;
//  - the first window of a cell only primes (no line), values are window deltas, a counter that went backwards reads 0, a short buffer is truncated safely;
//  - F1: the pool line splits into the process part (no cell block) and one block per cell; at one cell the two are byte-identical to the old line;
//  - P1 CELL_CPU counts leases per cell and class (UL separate from DL decode).
#include <gtest/gtest.h>
#include <string>
#include <thread>
extern "C" {
#include "nr_cell_dash.h"
#include "nr_core_pool.h"
}

static std::string S(const char *b) { return std::string(b); }

TEST(CellDash, WindowPrimesThenMeasures)
{
  nr_dash_win_t w = {};
  EXPECT_EQ(nr_dash_win_tick(&w, 1000000000ull), -1); // first call opens the window
  EXPECT_EQ(nr_dash_win_tick(&w, 3000000000ull), 2000000000);
  EXPECT_EQ(nr_dash_win_tick(&w, 3000000000ull), -1); // clock did not advance: re-primes
  EXPECT_EQ(nr_dash_win_tick(&w, 4000000000ull), 1000000000);
}

TEST(CellDash, SingleCellWritesNothing)
{
  char b[400];
  memset(b, 'X', sizeof(b));
  nr_dash_q_t q = {};
  nr_dash_ldpc_t l = {};
  nr_dash_timing_t t = {};
  nr_dash_gpu_t g = {};
  nr_dash_budget_t bud = {};
  nr_dash_slot_t sl = {};
  nr_dash_sum_t su = {};
  const int64_t win = 10000000000ll;
  EXPECT_EQ(nr_dash_fmt_cell_q(b, sizeof(b), 1, 0, 415, win, &q, &q), 0);
  EXPECT_EQ(nr_dash_fmt_ldpc_iter(b, sizeof(b), 1, 0, 415, win, &l, &l), 0);
  EXPECT_EQ(nr_dash_fmt_cell_timing(b, sizeof(b), 1, 0, 415, win, &t, &t), 0);
  EXPECT_EQ(nr_dash_fmt_cell_gpu(b, sizeof(b), 1, 0, 415, win, &g, &g), 0);
  EXPECT_EQ(nr_dash_fmt_cell_budget(b, sizeof(b), 1, 0, 415, &bud), 0);
  EXPECT_EQ(nr_dash_fmt_budget_proc(b, sizeof(b), 1, 1, 2, 3, 4, 5), 0);
  EXPECT_EQ(nr_dash_fmt_slot_cell(b, sizeof(b), 1, 0, 415, win, 500000, 3, &sl, &sl), 0);
  EXPECT_EQ(nr_dash_fmt_cell_sum(b, sizeof(b), 1, 0, 415, win, &su, &su), 0);
  for (size_t i = 0; i < sizeof(b); i++) ASSERT_EQ(b[i], 'X') << "buffer touched at " << i; // not a byte written
  // and no line without a measured window (first call of a cell)
  EXPECT_EQ(nr_dash_fmt_cell_q(b, sizeof(b), 2, 0, 415, -1, &q, &q), 0);
  EXPECT_EQ(nr_dash_fmt_cell_timing(b, sizeof(b), 2, 0, 415, 0, &t, &t), 0);
}

TEST(CellDash, CellQ)
{
  nr_dash_q_t p = {}, c = {};
  p.scan = {5, 2, 4, 9, 100, 10, 1};
  c.scan = {7, 4, 8, 12, 130, 17, 1};
  c.pdsch = {3, -1, -1, 6, 40, 5, 0};
  p.pdsch = {0, -1, -1, 0, 30, 2, 0};
  c.pusch = {-1, -1, -1, 0, 0, 0, 0};
  p.pusch = c.pusch;
  char b[600];
  const int n = nr_dash_fmt_cell_q(b, sizeof(b), 2, 1, 923, 10000000000ll, &c, &p);
  EXPECT_EQ(S(b),
            "SENSING: CELL_Q cell=1 pci=923 win_ms=10000 scan[depth_now=7 age_p50_slots<=4 age_p99_slots<=8 max_lag_win=12 drop_full=30 drop_stale=7 drop_epoch=0] "
            "pdsch[depth_now=3 max_lag_win=6 drop_full=10 drop_stale=3 drop_epoch=0] pusch[max_lag_win=0 drop_full=0 drop_stale=0 drop_epoch=0]");
  EXPECT_EQ(n, (int)strlen(b));
}

TEST(CellDash, LdpcIterBinsAndLine)
{
  EXPECT_EQ(nr_dash_it_bin(0, 20), -1);
  EXPECT_EQ(nr_dash_it_bin(1, 20), 0);
  EXPECT_EQ(nr_dash_it_bin(3, 20), 2);
  EXPECT_EQ(nr_dash_it_bin(4, 20), 3);
  EXPECT_EQ(nr_dash_it_bin(6, 20), 3);
  EXPECT_EQ(nr_dash_it_bin(7, 20), 4);
  EXPECT_EQ(nr_dash_it_bin(10, 20), 4);
  EXPECT_EQ(nr_dash_it_bin(11, 20), 5);
  EXPECT_EQ(nr_dash_it_bin(19, 20), 5);
  EXPECT_EQ(nr_dash_it_bin(20, 20), 6); // a block hit the cap
  EXPECT_EQ(nr_dash_it_bin(5, 0), 3);   // unknown cap: never "cap"
  nr_dash_ldpc_t p = {}, c = {};
  c.tb = 10;
  c.cb_tried = 40;
  c.cb_ok = 36;
  c.it_sum = 72;
  const uint64_t h[NR_DASH_IT_BINS] = {1, 2, 3, 2, 1, 0, 1};
  memcpy(c.hist, h, sizeof(h));
  char b[400];
  nr_dash_fmt_ldpc_iter(b, sizeof(b), 2, 0, 415, 10000000000ll, &c, &p);
  EXPECT_EQ(S(b), "SENSING: LDPC_ITER cell=0 pci=415 win_ms=10000 tb=10 cb=40 it_hist[1,2,3,4-6,7-10,11+,cap]=1,2,3,2,1,0,1 at_cap_pct=10.0 cb_fail_pct=10.0 mean_it=2.00");
}

TEST(CellDash, TimingGpuBudgetSlotSum)
{
  char b[500];
  nr_dash_timing_t tp = {0, 0, 0, 0, 0, 3, 5}, tc = {-1.5, -120, 4, -2, 12.34, 5, 6};
  nr_dash_fmt_cell_timing(b, sizeof(b), 3, 2, 403, 10000000000ll, &tc, &tp);
  EXPECT_EQ(S(b), "SENSING: CELL_TIMING cell=2 pci=403 win_ms=10000 sfo_eps_ppm=-1.500 cfo_hz=-120 max_pos_acc=4 shift_next=-2 sync_age_s=12.3 discont=2 resync=1");

  nr_dash_gpu_t gp = {100, 4, 0, 0, 0, 1}, gc = {200, 14, 3ull << 20, 1ull << 20, 5ull << 20, 1};
  nr_dash_fmt_cell_gpu(b, sizeof(b), 2, 1, 923, 10000000000ll, &gc, &gp);
  EXPECT_EQ(S(b), "SENSING: CELL_GPU cell=1 pci=923 win_ms=10000 active=1 calls=100 fallback=10 fallback_pct=10.0 gpu_mem_mb=4 pin_mb=5");

  nr_dash_budget_t bud = {1200, 800, 300, 20, 4.0};
  nr_dash_fmt_cell_budget(b, sizeof(b), 2, 0, 415, &bud);
  EXPECT_EQ(S(b), "SENSING: CELL_BUDGET scope=cell cell=0 pci=415 ram_admit_mb=1200 gpu_admit_mb=800 gpu_now_mb=300 pin_now_mb=20 tokens_fair=4.00");
  nr_dash_fmt_budget_proc(b, sizeof(b), 2, 20000, 3000, 17000, 15000, 2500);
  EXPECT_EQ(S(b), "SENSING: CELL_BUDGET scope=proc cells=2 host_budget_mb=20000 host_used_mb=3000 headroom_mb=17000 avail_now_mb=15000 rss_mb=2500");

  nr_dash_slot_t sp = {}, sc = {};
  sc.slots = 20000;
  sc.late = 3;
  sc.hist[8] = 19990; // [128, 256) us
  sc.hist[12] = 10;   // [2048, 4096) us
  sc.max_ns = 3500000;
  nr_dash_fmt_slot_cell(b, sizeof(b), 2, 1, 923, 10000000000ll, 500000, 7, &sc, &sp);
  EXPECT_EQ(S(b), "SENSING: SLOT_CELL cell=1 pci=923 win_ms=10000 slot_us=500 slots=20000 proc_us p50<=256 p99<=256 max=3500 late_slots=3 cpu=7");

  nr_dash_sum_t up = {"", 0, 0, 0, 0, 0, 0, 0}, uc = {"TRACKING", 100, 90, 400, 380, 12, 2, 5000000000ull};
  nr_dash_fmt_cell_sum(b, sizeof(b), 2, 1, 923, 10000000000ll, &uc, &up);
  EXPECT_EQ(S(b), "SENSING: CELL_SUM cell=1 pci=923 win_ms=10000 state=TRACKING decode_ok_pct=90.0 grants=100 cb_crc_pct=95.0 drops=12 disc=2 tokens_avg=0.50");
}

TEST(CellDash, BackwardsCounterAndShortBuffer)
{
  nr_dash_q_t p = {}, c = {};
  p.scan.drop_full = 50;
  c.scan.drop_full = 10; // re-initialised cell: a delta of 0, never a huge unsigned value
  c.scan.depth = c.pdsch.depth = c.pusch.depth = -1;
  c.scan.age_p50 = c.pdsch.age_p50 = c.pusch.age_p50 = -1;
  char b[600];
  nr_dash_fmt_cell_q(b, sizeof(b), 2, 0, 415, 1000000000ll, &c, &p);
  EXPECT_NE(S(b).find("scan[max_lag_win=0 drop_full=0 "), std::string::npos) << b;
  char s[24];
  const int n = nr_dash_fmt_cell_q(s, sizeof(s), 2, 0, 415, 1000000000ll, &c, &p);
  EXPECT_EQ(n, (int)sizeof(s) - 1); // truncated, NUL terminated, nothing written past the end
  EXPECT_EQ(strlen(s), sizeof(s) - 1);
}

// ---- pool: F1 split and P1 CELL_CPU
static nr_pool_t *mkpool(int n)
{
  nr_pool_cfg_t c = nr_pool_cfg_default();
  c.pin = false;
  int cores[NR_POOL_MAX_TOKENS];
  for (int i = 0; i < n; i++) cores[i] = i;
  return nr_pool_create(cores, nullptr, n, &c);
}
static void lease(nr_pool_t *p, int prio, int cell)
{
  std::thread([&] {
    nr_pool_acquire(p, prio, cell, 0);
    nr_pool_release(p);
  }).join();
}

TEST(CellDashPool, OneCellProcLineEqualsTheOldLine)
{
  nr_pool_t *p = mkpool(4);
  lease(p, NR_POOL_SCAN, 0);
  lease(p, NR_POOL_DECODE, 0);
  char a[1200], b[1200], c[1200];
  nr_pool_stats_line(p, a, sizeof(a));
  nr_pool_stats_line_proc(p, b, sizeof(b));
  EXPECT_STREQ(a, b); // N=1: the process part is the whole line, no cell block
  EXPECT_EQ(S(a).find("cell"), std::string::npos);
  EXPECT_EQ(nr_pool_stats_line_cell(p, 3, 7, c, sizeof(c)), 0); // a cell that never leased has no block
  nr_pool_destroy(p);
}

TEST(CellDashPool, TwoCellsSplitAndCellCpu)
{
  nr_pool_t *p = mkpool(4);
  lease(p, NR_POOL_SCAN, 0);
  lease(p, NR_POOL_SCAN, 1);
  lease(p, NR_POOL_SCAN, 1);
  lease(p, NR_POOL_DECODE, 1);
  nr_pool_cpu_win_t w = {};
  char b[1200];
  // first trigger primes, one cell never prints
  EXPECT_EQ(nr_pool_cell_cpu_line(p, 1, 923, 2, 1000000000ull, &w, b, sizeof(b)), 0);
  lease(p, NR_POOL_DECODE | NR_POOL_F_UL, 1);
  lease(p, NR_POOL_DECODE | NR_POOL_F_UL, 1);
  lease(p, NR_POOL_DECODE, 1);
  lease(p, NR_POOL_RETRY, 0);
  nr_pool_cpu_win_t w1 = w;
  EXPECT_EQ(nr_pool_cell_cpu_line(p, 1, 923, 1, 3000000000ull, &w1, b, sizeof(b)), 0); // N = 1: nothing
  const int n = nr_pool_cell_cpu_line(p, 1, 923, 2, 3000000000ull, &w, b, sizeof(b));
  ASSERT_GT(n, 0);
  const std::string l = b;
  EXPECT_EQ(l.rfind("SENSING: CELL_CPU cell=1 pci=923 win_ms=2000 ", 0), 0u) << l;
  EXPECT_NE(l.find("fair_pct=50.0"), std::string::npos) << l;
  EXPECT_NE(l.find(" n=3 "), std::string::npos) << l; // 3 leases of the cell in the window (2 UL + 1 DL decode)
  EXPECT_NE(l.find(" scan[hold_ms="), std::string::npos);
  EXPECT_NE(l.find(" scan[hold_ms=0 n=0] decode[hold_ms=0 n=1] retry[hold_ms=0 n=0] disc[hold_ms=0 n=0] house[hold_ms=0 n=0] ul[hold_ms=0 n=2]"), std::string::npos) << l;

  // F1: the process part has no cell block, each cell has its own
  nr_pool_stats_line_proc(p, b, sizeof(b));
  EXPECT_EQ(S(b).find("cell"), std::string::npos) << b;
  EXPECT_EQ(S(b).rfind("SENSING: CORE_POOL n=4 ", 0), 0u);
  char c1[400], c0[400], all[1200];
  ASSERT_GT(nr_pool_stats_line_cell(p, 1, 923, c1, sizeof(c1)), 0);
  ASSERT_GT(nr_pool_stats_line_cell(p, 0, 415, c0, sizeof(c0)), 0);
  EXPECT_EQ(S(c1).rfind("SENSING: CORE_POOL cell=1 pci=923 cell1[n=6 ", 0), 0u) << c1;
  EXPECT_EQ(S(c0).rfind("SENSING: CORE_POOL cell=0 pci=415 cell0[n=2 ", 0), 0u) << c0;
  // the combined line (kept for the 'cell<k>[...]' grammar) is the process part plus the same blocks
  nr_pool_stats_line(p, all, sizeof(all));
  EXPECT_NE(S(all).find(" cell0[n=2 "), std::string::npos);
  EXPECT_NE(S(all).find(" cell1[n=6 "), std::string::npos);
  EXPECT_EQ(S(all).rfind(S(b), 0), 0u);
  nr_pool_destroy(p);
}
