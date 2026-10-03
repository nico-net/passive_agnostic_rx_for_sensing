/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* LLR scale normalisation before the int8 LDPC decoder (passive PDSCH decode), K38 fix 2026-10-03.
 *
 * The decoder saturates every LLR to +-127; the multi-layer MMSE output scale is not controlled (mean
 * |LLR| ~450 at rank 4), so the decode applies one uniform right shift k per transport block that
 * brings the sampled mean |LLR| under LLR_NORM_TARGET.
 *
 * K38: the mean used to be taken over all G LLRs. A layout probe (code block 0 only) demodulates the
 * symbols up to its horizon and leaves the rest of the buffer at 0, so its mean was diluted by the
 * zeros, its shift came out smaller (rank 4: k 2 instead of 4) and its CB0 LLRs were 2^(dk) times the
 * whole-slot decode's -- clipped at the int8 rail where the full decode's were not. The statistic is
 * now taken over the first ceil(G/C) LLRs only: segment 0's span (E_0 <= ceil(G/C) by 38.212 5.4.2.1),
 * which every decode -- probe or full -- has demodulated (the probe horizon covers ceil(G/C) plus one
 * symbol). Same samples in, same shift out: probe CB0 == full CB0 bit for bit. Pure header:
 * unit-tested by test_nr_llr_norm. */
#ifndef NR_LLR_NORM_H
#define NR_LLR_NORM_H
#include <stdint.h>
#include <stdlib.h>

/* mean |LLR| the int8 decoder gets: 127/40 ~ 3.2x headroom over the mean for the 256QAM outer bits */
#define LLR_NORM_TARGET 40u

/* Code blocks C of a TBS per 38.212 5.2.2 (L = 24 CB CRC), as the probe horizon computes it. */
static inline uint32_t nr_llr_norm_num_cb(uint32_t tbs, int bg)
{
  const uint32_t Kcb = (bg == 2) ? 3840u : 8448u;
  const uint32_t B = tbs + 24u;
  return (B <= Kcb) ? 1u : (B + (Kcb - 24u) - 1u) / (Kcb - 24u);
}

/* LLRs the statistic reads: all G for one code block, else ceil(G/C) (code block 0's span). */
static inline uint32_t nr_llr_norm_span(uint32_t G, uint32_t C)
{
  return (C > 1u) ? (G + C - 1u) / C : G;
}

/* Right shift for llr[0, span): smallest k <= 8 with (sampled mean |LLR| >> k) <= LLR_NORM_TARGET,
 * the mean sampled every 16th LLR. */
static inline int nr_llr_norm_shift(const int16_t *llr, uint32_t span)
{
  uint64_t acc = 0;
  uint32_t cnt = 0;
  for (uint32_t i = 0; i < span; i += 16) {
    acc += (uint32_t)abs(llr[i]);
    cnt++;
  }
  if (cnt == 0)
    return 0;
  const uint32_t mean = (uint32_t)(acc / cnt);
  int k = 0;
  while (k < 8 && (mean >> k) > LLR_NORM_TARGET)
    k++;
  return k;
}
#endif
