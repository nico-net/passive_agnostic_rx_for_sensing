#ifndef NR_PDCCH_SCAN_SCORE_H
#define NR_PDCCH_SCAN_SCORE_H

#include <stdint.h>
#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#endif

/* QPSK amplitude consistency. Integer accumulation is exact for the PDCCH
 * maximum E; conversion to double happens once, as in the scalar reference. */
static inline double nr_pdcch_scan_score_scalar_sum(const int16_t *llr, int n, int64_t *abs_sum)
{
  int64_t a = 0, b = 0;
  for (int i = 0; i < n; i++) {
    const int32_t v = llr[i];
    const int32_t mag = v < 0 ? -v : v;
    a += mag;
    b += (int64_t)v * v;
  }
  if (abs_sum) *abs_sum = a;
  return b ? (double)a * (double)a / ((double)n * (double)b) : 0.0;
}

/* nr_pdcch_scan_score_scalar: scan score of one candidate's soft bits (the energy-consistency score used to rank candidates): the scalar form of nr_pdcch_scan_score_scalar_sum() with no absolute sum.
 *   Inline. Callers: joint_anon_score() and the scan path of nr_pdcch_blind_monitor_rt.c.
 */
static inline double nr_pdcch_scan_score_scalar(const int16_t *llr, int n)
{
  return nr_pdcch_scan_score_scalar_sum(llr, n, 0);
}

/* nr_pdcch_scan_score_neon_sum: NEON form of the scan score: returns (sum |x|)^2 / (n * sum x^2) over n soft bits, and stores sum |x| in *abs_sum when given; falls back to the scalar sum without NEON.
 *   Processes 8 lanes per step with a scalar tail. Inline. Caller: nr_pdcch_scan_score_neon().
 */
static inline double nr_pdcch_scan_score_neon_sum(const int16_t *llr, int n, int64_t *abs_sum)
{
#if defined(__aarch64__) && defined(__ARM_NEON)
  int64_t a = 0, b = 0;
  int i = 0;
  for (; i + 8 <= n; i += 8) {
    const int16x8_t v = vld1q_s16(llr + i);
    const int32x4_t lo = vmovl_s16(vget_low_s16(v));
    const int32x4_t hi = vmovl_s16(vget_high_s16(v));
    a += vaddvq_s32(vabsq_s32(lo)) + vaddvq_s32(vabsq_s32(hi));
    const int64x2_t sq0 = vmull_s32(vget_low_s32(lo), vget_low_s32(lo));
    const int64x2_t sq1 = vmull_s32(vget_high_s32(lo), vget_high_s32(lo));
    const int64x2_t sq2 = vmull_s32(vget_low_s32(hi), vget_low_s32(hi));
    const int64x2_t sq3 = vmull_s32(vget_high_s32(hi), vget_high_s32(hi));
    b += vaddvq_s64(sq0) + vaddvq_s64(sq1) + vaddvq_s64(sq2) + vaddvq_s64(sq3);
  }
  for (; i < n; i++) {
    const int32_t v = llr[i];
    a += v < 0 ? -v : v;
    b += (int64_t)v * v;
  }
  if (abs_sum) *abs_sum = a;
  return b ? (double)a * (double)a / ((double)n * (double)b) : 0.0;
#else
  return nr_pdcch_scan_score_scalar_sum(llr, n, abs_sum);
#endif
}

/* nr_pdcch_scan_score_neon: NEON scan score of n soft bits (no absolute sum returned). Inline. Callers: joint_anon_score() and nr_pdcch_blind_monitor_rt.c when ISAC_PDCCH_NEON_SCORE is set. */
static inline double nr_pdcch_scan_score_neon(const int16_t *llr, int n)
{
  return nr_pdcch_scan_score_neon_sum(llr, n, 0);
}

/* nr_pdcch_scan_abs_sum_scalar: sum of absolute values of n soft bits (scalar). Inline. Callers: nr_pdcch_scan_score_scalar_sum() users and the tests. */
static inline int64_t nr_pdcch_scan_abs_sum_scalar(const int16_t *v, int n)
{
  int64_t sum = 0;
  for (int i = 0; i < n; i++) sum += v[i] < 0 ? -(int32_t)v[i] : v[i];
  return sum;
}

/* nr_pdcch_scan_abs_sum_neon: sum of absolute values of n soft bits with NEON (8 lanes per step and a scalar tail); falls back to the scalar sum without NEON. Inline. Callers: nr_pdcch_blind_monitor_rt.c and the tests. */
static inline int64_t nr_pdcch_scan_abs_sum_neon(const int16_t *v, int n)
{
#if defined(__aarch64__) && defined(__ARM_NEON)
  int64_t sum = 0;
  int i = 0;
  for (; i + 8 <= n; i += 8) {
    const int16x8_t x = vld1q_s16(v + i);
    sum += vaddvq_s32(vabsq_s32(vmovl_s16(vget_low_s16(x))));
    sum += vaddvq_s32(vabsq_s32(vmovl_s16(vget_high_s16(x))));
  }
  for (; i < n; i++) sum += v[i] < 0 ? -(int32_t)v[i] : v[i];
  return sum;
#else
  return nr_pdcch_scan_abs_sum_scalar(v, n);
#endif
}

#endif
