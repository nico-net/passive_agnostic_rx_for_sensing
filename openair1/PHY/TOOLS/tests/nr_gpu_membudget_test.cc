/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* T6b: the shared device-memory budget. Pure functions + the process state (driven through NR_GPU_MEM_BUDGET_MB, no device needed). */
#include <gtest/gtest.h>
#include <cstdlib>
#include <cstring>
#include <string>
#include "PHY/TOOLS/nr_gpu_membudget.h"
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
