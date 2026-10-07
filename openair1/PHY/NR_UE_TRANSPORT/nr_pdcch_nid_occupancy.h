/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif
#include "nr_pdcch_nid_gpu.h"

/* Single producer state. Selection deliberately never reads the scoring REs
 * (1,5,9): under the independent circular-noise null it cannot select DM-RS
 * phase/amplitude fluctuations. This is a capture policy, not an ID screen. */
typedef struct {
  float noise[14];
  uint64_t offered[14];
} nr_nid_occupancy_t;

static inline uint32_t nr_nid_sample_power(nr_pdcch_nid_iq_t y)
{
  int r = y.r, i = y.i;
  return (uint32_t)(r * r) + (uint32_t)(i * i);
}

static inline bool nr_nid_occupancy_take(nr_nid_occupancy_t *s, const void *samples,
                                        int fft, int fco, int nsc, int pa,
                                        int css_lo, int css_nrb, int sym,
                                        float ratio, unsigned floor_pct)
{
  if (!samples || fft < nsc || nsc < 72 || fft <= 0 || fco < 0 || fco >= fft || sym < 0 || sym >= 14)
    return true; /* invalid geometry is rejected by the existing producer */
  int lo = pa < 0 ? (-pa + 11) / 12 : 0, hi = (nsc - pa) / 12;
  if (hi > 275 || hi - lo < 6) return true;
  const nr_pdcch_nid_iq_t *iq = (const nr_pdcch_nid_iq_t *)samples;
  float energy[46], peak = 0;
  int ng = 0;
  for (int rb = lo; rb + 6 <= hi; rb += 6) {
    if (css_nrb > 0 && rb < css_lo + css_nrb && rb + 6 > css_lo) continue;
    uint64_t sum = 0;
    int sc = (fco + pa + 12 * rb) % fft;
    if (sc + 72 <= fft) {
      /* Straight-line loads enable SIMD widening/reduction; no per-RE modulo,
       * mask branch, or FFT wrap test on the usual contiguous group. */
#if defined(__aarch64__)
      int64x2_t acc0 = vdupq_n_s64(0), acc1 = vdupq_n_s64(0);
      const int16x8_t mask = {-1, -1, 0, 0, -1, -1, -1, -1};
      #pragma GCC unroll 3
      for (int j = 0; j < 72; j += 4) {
        int16x8_t y = vandq_s16(vld1q_s16((const int16_t *)(iq + sc + j)), mask);
        int32x4_t a = vmull_s16(vget_low_s16(y), vget_low_s16(y));
        int32x4_t b = vmull_s16(vget_high_s16(y), vget_high_s16(y));
        acc0 = vpadalq_s32(acc0, a); acc1 = vpadalq_s32(acc1, b);
      }
      sum = (uint64_t)vaddvq_s64(vaddq_s64(acc0, acc1));
#else
      for (int j = 0; j < 72; j += 4) {
        sum += nr_nid_sample_power(iq[sc+j]);
        sum += nr_nid_sample_power(iq[sc+j+2]);
        sum += nr_nid_sample_power(iq[sc+j+3]);
      }
#endif
    } else {
      for (int j = 0; j < 72; ++j) {
        if ((j & 3) != 1) sum += nr_nid_sample_power(iq[sc]);
        if (++sc == fft) sc = 0;
      }
    }
    float e = (float)sum / 54.f;
    if (e > peak) peak = e;
    /* <=45 values; insertion order is bounded, no allocation or library sort. */
    int j = ng++;
    while (j && energy[j - 1] > e) { energy[j] = energy[j - 1]; --j; }
    energy[j] = e;
  }
  uint64_t offered = s->offered[sym]++;
  /* Exactly floor_pct independent probes per 100 offers for EACH symbol.
   * Permute their positions between blocks to avoid a fixed slot-period alias. */
  uint64_t block = offered / 100;
  uint32_t hash = (uint32_t)block * 747796405u + (uint32_t)sym * 2891336453u + 1u;
  hash = (hash ^ (hash >> 16)) * 2246822519u;
  bool probe = ((offered % 100 * 37 + (hash ^ (hash >> 13)) % 100) % 100) < floor_pct;
  if (!ng) return probe;
  float low = energy[ng / 5]; /* 20th percentile of complete, unmasked groups */
  float previous = s->noise[sym];
  s->noise[sym] = previous <= 0 ? low : previous + (low - previous) * (low < previous ? .25f : .0625f);
  /* Immediate robust floor prevents a gain/noise increase looking occupied
   * during the EMA's attack. Decay preserves sensitivity after gain drops. */
  float floor = low > s->noise[sym] ? low : s->noise[sym];
  return probe || (peak > 0 && peak > ratio * floor);
}
