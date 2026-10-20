// SPDX-License-Identifier: LicenseRef-CSSL-1.0
// PDCCH-REDESIGN-20261019 micro-benchmark 4: candidate occupancy screen over a whole CORESET ladder (36 CCE, AL 1/2/4/8 = 67 positions,
// 15552 soft bits). (a) scalar per candidate = what runs in the frozen config (ISAC_PDCCH_NEON_SCORE unset);
// (b) the shipped NEON per candidate; (c) prototype: per-CCE (sum|x|, sum x^2) once, every AL candidate by prefix sums.
// Same score (sum|x|)^2/(n sum x^2) for all; (c) checked equal to (a) on random data. Build: g++ -O3 -mcpu=native screen_bench.cc
#include <arm_neon.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <math.h>
#include <vector>
#include "PHY/NR_UE_TRANSPORT/nr_pdcch_scan_score.h"
static double now_us() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e6 + t.tv_nsec / 1e3; }
int main()
{
  const int NC = 36; std::vector<int16_t> x(NC * 108 + 16);
  srand(1); for (auto &v : x) v = (int16_t)((rand() % 61) - 30);
  const int als[4] = {1, 2, 4, 8}; double sink = 0;
  const int R = 20000; double t0 = now_us();
  for (int r = 0; r < R; r++) for (int a = 0; a < 4; a++) for (int c = 0; c + als[a] <= NC; c += als[a]) sink += nr_pdcch_scan_score_scalar(&x[c * 108], als[a] * 108);
  const double ts = (now_us() - t0) / R;
  t0 = now_us();
  for (int r = 0; r < R; r++) for (int a = 0; a < 4; a++) for (int c = 0; c + als[a] <= NC; c += als[a]) sink += nr_pdcch_scan_score_neon(&x[c * 108], als[a] * 108);
  const double tn = (now_us() - t0) / R;
  int64_t A[NC], B[NC]; double maxerr = 0; int ncand = 0;
  t0 = now_us();
  for (int r = 0; r < R; r++) {
    for (int c = 0; c < NC; c++) {
      int32x4_t sa = vdupq_n_s32(0); int64x2_t sb = vdupq_n_s64(0);
      { const int16x4_t t4 = vld1_s16(&x[c * 108 + 104]); const int16x8_t v = vcombine_s16(t4, vdup_n_s16(0));
        sa = vpadalq_s16(sa, vabsq_s16(v)); sb = vpadalq_s32(sb, vmull_s16(t4, t4)); }
      for (int i = 0; i < 104; i += 8) {
        const int16x8_t v = vld1q_s16(&x[c * 108 + i]);
        sa = vpadalq_s16(sa, vabsq_s16(v));
        int32x4_t q0 = vmull_s16(vget_low_s16(v), vget_low_s16(v)), q1 = vmull_high_s16(v, v);
        sb = vpadalq_s32(sb, q0); sb = vpadalq_s32(sb, q1);
      }
      A[c] = vaddvq_s32(sa); B[c] = vaddvq_s64(sb);
    }
    for (int a = 0; a < 4; a++) for (int c = 0; c + als[a] <= NC; c += als[a]) {
      int64_t sa = 0, sb = 0; for (int k = 0; k < als[a]; k++) { sa += A[c + k]; sb += B[c + k]; }
      sink += sb ? (double)sa * sa / ((double)als[a] * 108 * sb) : 0;
    }
  }
  const double tp = (now_us() - t0) / R;
  for (int a = 0; a < 4; a++) for (int c = 0; c + als[a] <= NC; c += als[a]) {
    int64_t sa = 0, sb = 0; for (int k = 0; k < als[a]; k++) { sa += A[c + k]; sb += B[c + k]; }
    const double p = (double)sa * sa / ((double)als[a] * 108 * sb), q = nr_pdcch_scan_score_scalar(&x[c * 108], als[a] * 108);
    maxerr = fmax(maxerr, fabs(p - q)); ncand++;
  }
  printf("screen of %d candidates (15552 soft bits): scalar per candidate %.2f us | shipped NEON per candidate %.2f us | per-CCE prefix-sum prototype %.2f us | max |score diff| %.2e (%.0f)\n", ncand, ts, tn, tp, maxerr, sink > 1e300);
  return 0;
}
