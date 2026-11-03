/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* MPCI-DBG3: the GPU exact FEP self-check only disables the path for a mismatch that repeats on input that stays put
 * (nr_fep_selfcheck.h). The fakes model the IQ ring as a row that the producer may overwrite between the calls. */
#include <gtest/gtest.h>
#include <cstring>
#include <vector>
extern "C" {
#include "nr_fep_selfcheck.h"
}

namespace {
constexpr int N = 64;
struct Ring {
  std::vector<uint32_t> row = std::vector<uint32_t>(N, 7); /* the IQ row the FEP reads */
  int calls = 0;            /* cpu + gpu calls made by the confirmation */
  int overwrite_at = -1;    /* the producer overwrites the row before call number overwrite_at (-1: never) */
  bool keep_moving = false; /* the producer overwrites before every call */
  bool gpu_fault = false;   /* the GPU output differs from the CPU's on any input */
  bool gpu_refuses = false; /* the GPU call is refused (busy / late) */
  void step()
  {
    if (keep_moving || calls == overwrite_at)
      for (auto &v : row) v += 3;
    calls++;
  }
};
int cpu(void *ctx, void *out)
{
  Ring *r = (Ring *)ctx;
  r->step();
  memcpy(out, r->row.data(), N * sizeof(uint32_t));
  return 0;
}
int gpu(void *ctx, void *out)
{
  Ring *r = (Ring *)ctx;
  r->step();
  if (r->gpu_refuses) return -1;
  uint32_t *o = (uint32_t *)out;
  for (int i = 0; i < N; i++) o[i] = r->row[i] + (r->gpu_fault && i == 5 ? 1 : 0);
  return 0;
}
int confirm(Ring &r)
{
  std::vector<uint32_t> s(3 * N);
  return nr_fep_selfcheck_confirm(cpu, gpu, &r, N * sizeof(uint32_t), s.data());
}
} // namespace

/* the first check raced the producer (the GPU read the old samples, the CPU reference the new ones); the ring is then stable:
 * the GPU on the same input agrees -> not a GPU fault (this is the cell-1 MISMATCH of mp27e_P1 at a SHMIQ_OVERRUN / RXDISCONT) */
TEST(FepSelfcheck, InputOverwrittenBeforeTheCheckIsVoid)
{
  Ring r; /* overwritten before the confirmation starts: all three confirmation calls see the same (new) input */
  EXPECT_EQ(NR_FEP_SELFCHECK_VOID, confirm(r));
}

TEST(FepSelfcheck, GpuFaultOnStableInputIsConfirmed)
{
  Ring r;
  r.gpu_fault = true;
  EXPECT_EQ(NR_FEP_SELFCHECK_CONFIRMED, confirm(r));
}

TEST(FepSelfcheck, InputStillMovingIsVoid)
{
  Ring r;
  r.gpu_fault = true; /* even a faulty GPU cannot be judged on a row that changes under the comparison */
  r.keep_moving = true;
  EXPECT_EQ(NR_FEP_SELFCHECK_VOID, confirm(r));
}

TEST(FepSelfcheck, OverwriteDuringTheGpuRerunIsVoid)
{
  for (int at = 1; at <= 2; at++) { /* before the GPU re-run or before the closing CPU reference */
    Ring r;
    r.gpu_fault = true;
    r.overwrite_at = at;
    EXPECT_EQ(NR_FEP_SELFCHECK_VOID, confirm(r)) << "overwrite before call " << at;
  }
}

TEST(FepSelfcheck, RefusedGpuRerunIsVoid)
{
  Ring r;
  r.gpu_refuses = true;
  EXPECT_EQ(NR_FEP_SELFCHECK_VOID, confirm(r));
}

TEST(FepSelfcheck, ThreeCallsCpuGpuCpu)
{
  Ring r;
  confirm(r);
  EXPECT_EQ(3, r.calls);
}

/* OTA 2026-11-03 (415+923, 4 of 8 pair runs): ONE confirmed mismatch switched the GPU FEP / chest / graph pass off for every cell for the rest
 * of the run (decode p50 5 us -> 5 ms, ring overruns, sensing drops). A GPU fault repeats; it is declared only after a run of confirmed checks. */
TEST(FepSelfcheck, FaultNeedsConfirmedChecksInARow)
{
  EXPECT_FALSE(nr_fep_selfcheck_fault(1));
  EXPECT_FALSE(nr_fep_selfcheck_fault(NR_FEP_SELFCHECK_FAULT_RUN - 1));
  EXPECT_TRUE(nr_fep_selfcheck_fault(NR_FEP_SELFCHECK_FAULT_RUN));
}

TEST(FepSelfcheck, DiffCountsDifferingSamplesAndLargestStep)
{
  int16_t a[8] = {0, 1, 2, 3, -4, 5, 6, 7}, b[8] = {0, 2, 2, 3, 4, 5, 6, 7};
  int n = -1, mx = -1;
  nr_fep_selfcheck_diff(a, b, sizeof(a), &n, &mx);
  EXPECT_EQ(2, n);
  EXPECT_EQ(8, mx);
}
