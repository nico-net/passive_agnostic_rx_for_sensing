/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* T6b: the shared device-memory budget. Pure functions + the process state (driven through NR_GPU_MEM_BUDGET_MB, no device needed). */
#include <gtest/gtest.h>
#include <cstdlib>
#include <cstring>
#include <string>
#include "PHY/TOOLS/nr_gpu_membudget.h"
#include "PHY/TOOLS/nr_gpu_elastic.h"
#include "PHY/TOOLS/nr_gpu_memstat.h"
#include "PHY/NR_UE_TRANSPORT/nr_csirs_disc_mem.h"
namespace {
constexpr size_t MB = (size_t)1 << 20;
/* full-size needs at 273 PRB 4 RX [MEASURED, sens6 census, device MB] (csirs: nr_csirs_disc_mem model) */
const size_t kPool = 620 * MB, kCb0 = 900 * MB, kTd = 556 * MB, kNid = 776 * MB, kPass = 361 * MB + 104 * MB;
size_t csirs_full() {
  return nr_csirs_disc_mem_fixed(273, 4, 4096, 14 * (4096 + 288 + 288), 100) + (size_t)640 * nr_csirs_disc_mem_per_slot(273, 4);
}
}
TEST(MemBudget, WeightsSum100) {
  int s = 0;
  for (int i = 0; i < NR_GPU_MEM_N; i++) s += nr_gpu_mem_weight[i];
  EXPECT_EQ(100, s);
}
TEST(MemBudget, UnknownMemoryIsUnlimited) {
  EXPECT_EQ(SIZE_MAX, nr_gpu_membudget_calc(0));
  EXPECT_EQ(SIZE_MAX, nr_gpu_membudget_share_of(SIZE_MAX, NR_GPU_MEM_CSIRS));
  EXPECT_EQ(1.0, nr_gpu_membudget_scale_of(SIZE_MAX, kPool));
}
TEST(MemBudget, Gb10EverythingFullSize) {
  const size_t B = nr_gpu_membudget_calc(120ull << 30);
  EXPECT_NEAR(0.85 * 120 * 1024 - 768, (double)(B / MB), 2.0);
  const struct { nr_gpu_mem_module_t m; size_t need; } t[] = {{NR_GPU_MEM_CSIRS, csirs_full()}, {NR_GPU_MEM_LDPC_POOL, kPool}, {NR_GPU_MEM_LDPC_CB0, kCb0},
                                                             {NR_GPU_MEM_TD_CB0, kTd}, {NR_GPU_MEM_PDCCH_NID, kNid}, {NR_GPU_MEM_PASS, kPass}};
  for (auto &e : t) {
    const size_t share = nr_gpu_membudget_share_of(B, e.m);
    EXPECT_EQ(1.0, nr_gpu_membudget_scale_of(share, e.need)) << nr_gpu_mem_name[e.m];
  }
  /* knobs: exactly the full values */
  const size_t sh = nr_gpu_membudget_share_of(B, NR_GPU_MEM_CSIRS);
  EXPECT_EQ(640, nr_csirs_disc_mem_capacity(sh, 273, 4, 4096, 14 * (4096 + 288 + 288), 100, 640));
  EXPECT_EQ(2048, nr_gpu_membudget_fit(nr_gpu_membudget_share_of(B, NR_GPU_MEM_TD_CB0), 47 * MB, 410 * 1024, 512, 2048));
  EXPECT_EQ(10, nr_gpu_membudget_fit(nr_gpu_membudget_share_of(B, NR_GPU_MEM_PDCCH_NID), 100 * MB, 70 * MB, 5, 10));
}
TEST(MemBudget, EightGbCardFitsUnderBudget) {
  const size_t B = nr_gpu_membudget_calc(7500 * MB); /* RTX 4060 Ti: 7803 MB total, ~7500 free at the probe */
  EXPECT_NEAR(0.85 * 7500 - 768, (double)(B / MB), 2.0);
  /* every module's share is below B and the sum of shares is the budget */
  size_t sum = 0;
  for (int m = 0; m < NR_GPU_MEM_N; m++) sum += nr_gpu_membudget_share_of(B, (nr_gpu_mem_module_t)m);
  EXPECT_LE(sum, B);
  const size_t sh = nr_gpu_membudget_share_of(B, NR_GPU_MEM_CSIRS);
  const int cap = nr_csirs_disc_mem_capacity(sh, 273, 4, 4096, 14 * (4096 + 288 + 288), 100, 640);
  EXPECT_GE(cap, NR_CSIRS_DISC_CAPACITY_MIN);
  EXPECT_LT(cap, 640);
  EXPECT_LE(nr_csirs_disc_mem_fixed(273, 4, 4096, 14 * (4096 + 288 + 288), 100) + (size_t)cap * nr_csirs_disc_mem_per_slot(273, 4), sh);
  const double scale = nr_gpu_membudget_scale_of(nr_gpu_membudget_share_of(B, NR_GPU_MEM_LDPC_POOL), kPool);
  EXPECT_LT(scale, 1.0);
  EXPECT_GT(scale, 0.5);
}
TEST(MemBudget, FourGbCardHitsFloors) {
  const size_t B = nr_gpu_membudget_calc(3500 * MB);
  const size_t sh = nr_gpu_membudget_share_of(B, NR_GPU_MEM_CSIRS);
  EXPECT_EQ(0, nr_csirs_disc_mem_capacity(sh, 273, 4, 4096, 14 * (4096 + 288 + 288), 100, 640)); /* not ready: CPU discovery */
  /* a 106 PRB carrier needs a smaller fixed part: not ready at 4 GB (the fixed local-memory reserve alone is 512 MB), ready on a 6 GB card */
  EXPECT_EQ(0, nr_csirs_disc_mem_capacity(sh, 106, 4, 2048, 14 * (2048 + 144 + 144), 100, 640));
  const size_t sh6 = nr_gpu_membudget_share_of(nr_gpu_membudget_calc(5500 * MB), NR_GPU_MEM_CSIRS);
  EXPECT_GE(nr_csirs_disc_mem_capacity(sh6, 106, 4, 2048, 14 * (2048 + 144 + 144), 100, 640), NR_CSIRS_DISC_CAPACITY_MIN);
  EXPECT_EQ(0, nr_gpu_membudget_fit(10 * MB, 100 * MB, 70 * MB, 5, 10));      /* share below the fixed part */
  EXPECT_EQ(0, nr_gpu_membudget_fit(120 * MB, 100 * MB, 70 * MB, 5, 10));     /* below the floor */
  EXPECT_EQ(5, nr_gpu_membudget_fit(100 * MB + 5 * 70 * MB, 100 * MB, 70 * MB, 5, 10));
  EXPECT_EQ(0, nr_gpu_membudget_calc(500 * MB));                              /* below the reserve: B = 0 */
}
TEST(MemBudget, FitNeverExceedsHi) {
  EXPECT_EQ(2048, nr_gpu_membudget_fit(SIZE_MAX, 1, 1, 512, 2048));
  EXPECT_EQ(2048, nr_gpu_membudget_fit((size_t)1 << 40, 47 * MB, 410 * 1024, 512, 2048));
  EXPECT_EQ(1000, nr_gpu_membudget_fit(47 * MB + 1000 * 410 * 1024ull, 47 * MB, 410 * 1024, 512, 2048));
}
TEST(MemBudget, StateCommitGraphRemainderAndFormat) {
  setenv("NR_GPU_MEM_BUDGET_MB", "7500", 1);
  nr_gpu_membudget_test_reset();
  const size_t B = nr_gpu_membudget_total();
  EXPECT_EQ(nr_gpu_membudget_calc(7500 * MB), B);
  const size_t g0 = nr_gpu_membudget_graph_bytes();
  EXPECT_EQ(nr_gpu_membudget_share_of(B, NR_GPU_MEM_GRAPH), g0);
  const size_t sh = nr_gpu_membudget_share(NR_GPU_MEM_PASS);
  nr_gpu_membudget_commit(NR_GPU_MEM_PASS, sh / 2, 1.0);
  EXPECT_EQ(g0 + (sh - sh / 2), nr_gpu_membudget_graph_bytes()); /* the unused part of a share goes to the graphs */
  nr_gpu_membudget_commit(NR_GPU_MEM_CSIRS, 0, 0.0);
  char buf[512];
  nr_gpu_membudget_format(buf, sizeof buf);
  EXPECT_NE(nullptr, strstr(buf, "budget_mb="));
  EXPECT_NE(nullptr, strstr(buf, "csirs=0MB"));
  EXPECT_NE(nullptr, strstr(buf, "(CPU)"));
  EXPECT_NE(nullptr, strstr(buf, "ldpc_pool=n/a"));
  unsetenv("NR_GPU_MEM_BUDGET_MB");
  nr_gpu_membudget_test_reset();
}

/* ---- on-demand sizing (nr_gpu_elastic.h + the budget headroom) ---- */
TEST(Elastic, DisabledWhenTheShareCoversTheFullSize) {
  nr_gpu_elastic_t e;
  nr_gpu_elastic_init(&e, 640, 640, 64, 10, 5); /* GB10 / unlimited budget: base == full */
  EXPECT_FALSE(nr_gpu_elastic_enabled(&e));
  EXPECT_EQ(640, e.cur);
  for (int d : {0, 100, 640, 5000}) EXPECT_EQ(640, nr_gpu_elastic_target(&e, d));
  EXPECT_EQ(0u, e.grows + e.shrinks + e.denied);
}
TEST(Elastic, GrowsInStepsToTheDemandAndNeverBeyondFull) {
  nr_gpu_elastic_t e;
  nr_gpu_elastic_init(&e, 221, 640, 64, 10, 5);
  EXPECT_TRUE(nr_gpu_elastic_enabled(&e));
  EXPECT_EQ(221, nr_gpu_elastic_target(&e, 100));  /* demand below the base: nothing to do */
  EXPECT_EQ(221, nr_gpu_elastic_target(&e, 221));
  EXPECT_EQ(285, nr_gpu_elastic_target(&e, 222));  /* one step from the base */
  EXPECT_EQ(413, nr_gpu_elastic_target(&e, 400));  /* 221 + 3 * 64 = 413 >= 400 */
  EXPECT_EQ(640, nr_gpu_elastic_target(&e, 640));  /* clamped to full */
  EXPECT_EQ(640, nr_gpu_elastic_target(&e, 9999));
}
TEST(Elastic, FitTakesAsManyWholeStepsAsTheHeadroomHolds) {
  nr_gpu_elastic_t e;
  nr_gpu_elastic_init(&e, 221, 640, 64, 10, 5);
  const size_t per = 3 * MB;
  EXPECT_EQ(640, nr_gpu_elastic_fit(&e, 640, per, SIZE_MAX));                 /* unlimited */
  EXPECT_EQ(640, nr_gpu_elastic_fit(&e, 640, per, (640 - 221) * per));        /* exactly fits */
  EXPECT_EQ(221 + 6 * 64, nr_gpu_elastic_fit(&e, 640, per, 400 * per));       /* 400 slots of headroom: 6 whole steps (384) */
  EXPECT_EQ(221, nr_gpu_elastic_fit(&e, 640, per, 63 * per));                 /* not even one step */
  EXPECT_EQ(221, nr_gpu_elastic_fit(&e, 640, per, 0));
  e.cur = 413;
  EXPECT_EQ(413, nr_gpu_elastic_fit(&e, 300, per, 0));                        /* target below cur: unchanged */
  EXPECT_EQ(477, nr_gpu_elastic_fit(&e, 640, per, 100 * per));
}
TEST(Elastic, ShrinksToTheBaseOnlyAfterIdleTicksAndKeepsWhatIsStillNeeded) {
  nr_gpu_elastic_t e;
  nr_gpu_elastic_init(&e, 6, 10, 1, 4, 3);
  e.cur = 9;
  EXPECT_EQ(9, nr_gpu_elastic_target(&e, 6));
  EXPECT_EQ(9, nr_gpu_elastic_target(&e, 6));
  EXPECT_EQ(9, nr_gpu_elastic_target(&e, 8));      /* demand between base and cur resets the idle count and keeps the size */
  for (int i = 0; i < 3; i++) EXPECT_EQ(9, nr_gpu_elastic_target(&e, 6));
  EXPECT_EQ(6, nr_gpu_elastic_target(&e, 6));      /* 4th consecutive idle tick */
  nr_gpu_elastic_done(&e, 6, 6, 1);                /* the module applied the shrink */
  EXPECT_EQ(6, nr_gpu_elastic_target(&e, 6));      /* already at the base */
  EXPECT_EQ(1u, e.shrinks);
}
TEST(Elastic, RefusedGrowthKeepsTheSizeCountsOnceLogsOnceAndBacksOff) {
  nr_gpu_elastic_t e;
  nr_gpu_elastic_init(&e, 221, 640, 64, 10, 3);
  EXPECT_EQ(285, nr_gpu_elastic_target(&e, 285));
  EXPECT_EQ(1, nr_gpu_elastic_done(&e, 285, 221, 1));    /* the budget had no headroom: first denial -> log */
  EXPECT_EQ(221, e.cur);
  EXPECT_EQ(1u, e.denied);
  for (int i = 0; i < 3; i++) EXPECT_EQ(221, nr_gpu_elastic_target(&e, 285)); /* back-off: no new request */
  EXPECT_EQ(285, nr_gpu_elastic_target(&e, 285));
  EXPECT_EQ(0, nr_gpu_elastic_done(&e, 285, 285, 0));    /* allocation failed: counted, logged only the first time */
  EXPECT_EQ(2u, e.denied);
  EXPECT_EQ(221, e.cur);
  for (int i = 0; i < 3; i++) nr_gpu_elastic_target(&e, 285);
  EXPECT_EQ(413, nr_gpu_elastic_target(&e, 400));
  EXPECT_EQ(0, nr_gpu_elastic_done(&e, 413, 285, 1));    /* partial: reached one of two steps, the rest refused */
  EXPECT_EQ(285, e.cur);
  EXPECT_EQ(1u, e.grows);
  EXPECT_EQ(3u, e.denied);
  EXPECT_EQ(285, nr_gpu_elastic_target(&e, 100)); /* demand below the base counts as idle: no immediate shrink (idle_limit 10) */
}
TEST(Elastic, GrowThenIdleThenShrinkCounters) {
  nr_gpu_elastic_t e;
  nr_gpu_elastic_init(&e, 5, 10, 1, 2, 0);
  int t = nr_gpu_elastic_target(&e, 6);   /* demand rises one unit per tick: one grow each */
  EXPECT_EQ(6, t);
  nr_gpu_elastic_done(&e, t, t, 1);
  t = nr_gpu_elastic_target(&e, 7);
  nr_gpu_elastic_done(&e, t, t, 1);
  EXPECT_EQ(7, e.cur);
  EXPECT_EQ(5, (nr_gpu_elastic_target(&e, 0), nr_gpu_elastic_target(&e, 0)));
  nr_gpu_elastic_done(&e, 5, 5, 1);
  EXPECT_EQ(2u, e.grows);
  EXPECT_EQ(1u, e.shrinks);
  EXPECT_EQ(0u, e.denied);
  EXPECT_EQ(5, e.cur);
}
TEST(Elastic, BudgetHeadroomGrantsDenialsAndTheHardCeiling) {
  setenv("NR_GPU_MEM_BUDGET_MB", "7500", 1);
  nr_gpu_membudget_test_reset();
  const size_t B = nr_gpu_membudget_total();
  /* nothing committed: every module at its share, the graph at its cap -> headroom is the not-yet-committed part only (0 here) */
  EXPECT_LT(nr_gpu_membudget_headroom(), 1000u); /* (integer rounding of the shares only) */
  EXPECT_EQ(0, nr_gpu_membudget_dyn_acquire(NR_GPU_MEM_CSIRS, MB));
  uint64_t g, s, d;
  nr_gpu_membudget_dyn_counts(NR_GPU_MEM_CSIRS, &g, &s, &d);
  EXPECT_EQ(0u, g); EXPECT_EQ(1u, d);
  /* every module commits less than its share, the graph its actual bytes */
  size_t committed = 0;
  for (int m = 0; m < NR_GPU_MEM_N; m++) {
    const size_t sh = nr_gpu_membudget_share((nr_gpu_mem_module_t)m);
    const size_t use = m == NR_GPU_MEM_GRAPH ? 140 * MB : sh * 9 / 10;
    nr_gpu_membudget_commit((nr_gpu_mem_module_t)m, use, 1.0);
    committed += use;
  }
  const size_t hr = nr_gpu_membudget_headroom();
  EXPECT_EQ(B - committed, hr);
  EXPECT_GT(hr, 1000 * MB); /* the graph cap that was not used is real headroom */
  EXPECT_EQ(1, nr_gpu_membudget_dyn_acquire(NR_GPU_MEM_CSIRS, hr / 2));
  EXPECT_EQ(hr - hr / 2, nr_gpu_membudget_headroom());
  EXPECT_EQ(0, nr_gpu_membudget_dyn_acquire(NR_GPU_MEM_PDCCH_NID, hr)); /* more than is left: refused, nothing taken */
  EXPECT_EQ(hr - hr / 2, nr_gpu_membudget_headroom());
  EXPECT_EQ(1, nr_gpu_membudget_dyn_acquire(NR_GPU_MEM_PDCCH_NID, hr - hr / 2)); /* exactly the rest */
  EXPECT_EQ(0u, nr_gpu_membudget_headroom());
  EXPECT_EQ(0, nr_gpu_membudget_dyn_acquire(NR_GPU_MEM_CSIRS, 1));        /* never exceeds the budget */
  nr_gpu_membudget_dyn_release(NR_GPU_MEM_CSIRS, hr / 2);                 /* idle: memory returned, another module can take it */
  EXPECT_EQ(hr / 2, nr_gpu_membudget_headroom());
  nr_gpu_membudget_dyn_abort(NR_GPU_MEM_PDCCH_NID, hr - hr / 2);          /* a grant whose allocation failed is undone */
  EXPECT_EQ(hr, nr_gpu_membudget_headroom());
  nr_gpu_membudget_dyn_counts(NR_GPU_MEM_CSIRS, &g, &s, &d);
  EXPECT_EQ(1u, g); EXPECT_EQ(1u, s); EXPECT_EQ(2u, d);
  nr_gpu_membudget_dyn_counts(NR_GPU_MEM_PDCCH_NID, &g, &s, &d);
  EXPECT_EQ(0u, g); EXPECT_EQ(0u, s); EXPECT_EQ(2u, d);                   /* the refusal and the aborted grant */
  /* the census line carries the dynamic counters */
  nr_gpu_membudget_dyn_acquire(NR_GPU_MEM_CSIRS, 5 * MB);
  char buf[1024];
  nr_gpu_membudget_format(buf, sizeof buf);
  EXPECT_NE(nullptr, strstr(buf, "csirs_dyn=5MB/grows=2/shrinks=1/denied=2")) << buf;
  EXPECT_NE(nullptr, strstr(buf, "pdcch_nid_dyn=0MB/grows=0/shrinks=0/denied=2")) << buf;
  unsetenv("NR_GPU_MEM_BUDGET_MB");
  nr_gpu_membudget_test_reset();
}
TEST(Elastic, UnlimitedBudgetNeverDeniesAndGb10StaysAtFullSize) {
  unsetenv("NR_GPU_MEM_BUDGET_MB");
  nr_gpu_membudget_test_reset();
  if (nr_gpu_membudget_total() == SIZE_MAX) {
    EXPECT_EQ(SIZE_MAX, nr_gpu_membudget_headroom());
    EXPECT_EQ(1, nr_gpu_membudget_dyn_acquire(NR_GPU_MEM_CSIRS, (size_t)1 << 40));
  }
  nr_gpu_membudget_test_reset();
}

/* G4: csirs and pdcch_nid commit per cell (the module figure is the sum), each cell sizes against share / active cells; the other modules and one cell are unchanged. */
namespace {
int g_test_cell = 0;
int test_cell_fn() { return g_test_cell; }
}
TEST(MemBudget, PerCellModulesCommitPerCellAndSplitTheShare) {
  setenv("NR_GPU_MEM_BUDGET_MB", "10000", 1);
  nr_gpu_membudget_test_reset();
  nr_gpu_memstat_set_cell_fn_for_test(test_cell_fn);
  const size_t sh_csirs = nr_gpu_membudget_share(NR_GPU_MEM_CSIRS), sh_pass = nr_gpu_membudget_share(NR_GPU_MEM_PASS);
  nr_gpu_membudget_test_cells(2);
  EXPECT_EQ(sh_csirs / 2, nr_gpu_membudget_share(NR_GPU_MEM_CSIRS));
  EXPECT_EQ(nr_gpu_membudget_share_of(nr_gpu_membudget_total(), NR_GPU_MEM_PDCCH_NID) / 2, nr_gpu_membudget_share(NR_GPU_MEM_PDCCH_NID));
  EXPECT_EQ(sh_pass, nr_gpu_membudget_share(NR_GPU_MEM_PASS)); /* process-wide module: not split */
  g_test_cell = 0; nr_gpu_membudget_commit(NR_GPU_MEM_CSIRS, 100 * MB, 1.0);
  g_test_cell = 1; nr_gpu_membudget_commit(NR_GPU_MEM_CSIRS, 60 * MB, 0.5);
  EXPECT_EQ(160 * MB, nr_gpu_membudget_committed_total());
  g_test_cell = 0; nr_gpu_membudget_commit(NR_GPU_MEM_CSIRS, 80 * MB, 1.0); /* a re-commit replaces that cell's figure only */
  EXPECT_EQ(140 * MB, nr_gpu_membudget_committed_total());
  g_test_cell = 1; nr_gpu_membudget_commit(NR_GPU_MEM_CSIRS, 0, 0.0);      /* cell 1 fell back to the CPU */
  EXPECT_EQ(80 * MB, nr_gpu_membudget_committed_total());
  /* a process-wide module keeps ONE figure whatever the cell */
  g_test_cell = 1; nr_gpu_membudget_commit(NR_GPU_MEM_PASS, 50 * MB, 1.0);
  g_test_cell = 0; nr_gpu_membudget_commit(NR_GPU_MEM_PASS, 70 * MB, 1.0);
  EXPECT_EQ(150 * MB, nr_gpu_membudget_committed_total());
  /* one cell: the commit replaces, the share is whole */
  nr_gpu_membudget_test_reset(); nr_gpu_membudget_test_cells(0); g_test_cell = 0;
  nr_gpu_membudget_commit(NR_GPU_MEM_CSIRS, 100 * MB, 1.0); nr_gpu_membudget_commit(NR_GPU_MEM_CSIRS, 90 * MB, 1.0);
  EXPECT_EQ(90 * MB, nr_gpu_membudget_committed_total());
  EXPECT_EQ(sh_csirs, nr_gpu_membudget_share(NR_GPU_MEM_CSIRS));
  nr_gpu_memstat_set_cell_fn_for_test(nullptr);
  nr_gpu_membudget_test_reset();
  unsetenv("NR_GPU_MEM_BUDGET_MB");
}
