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

/* RANK2-WEAK 2026-10-23: rank-2 (two layers) normalisation for Qm 4, 6, 8. The shift above samples every 16th LLR; with 2 layers the buffer
 * alternates one modulation symbol (Qm bits) per layer, so for Qm 4 and 8 that sample lands on layer 0 ONLY, and for every Qm the mean is
 * dominated by the stronger layer. At a layer gap >= 8 dB (the weak layer is layer 1) the weak layer is left with mean |LLR| of 2-5 LSB and
 * ~35 % exact zeros after the shift [MEASURED, DGX replay raw_pci415_12s_b, CPU path]. Rule: sample every 16th RE block (Qm LLRs per layer),
 * k from the mean of both layers (unbiased version of the old sample), minus e = min(2, floor(log2(strong/weak mean))) bits so the weak layer
 * keeps resolution while the strong layer saturates only by 2x. Any other nl, QPSK: the function above, bit for bit. The GPU twin is
 * k_norm_sum (nr_gpu_pass_graph.cu); keep the two identical. */
#define NR_LLR_NORM_L2_STRIDE 16u
static inline int nr_llr_norm_shift_nl(const int16_t *llr, uint32_t span, int nl, int qm)
{
  static int old_rule = -1, target = 0, dk = 2; /* NR_TEST_L2NORM_OLD=1: the pre-2026-10-23 rule (A/B seam); NR_TEST_L2NORM_TARGET: weak-layer target (exploration) */
  if (old_rule < 0) {
    old_rule = getenv("NR_TEST_L2NORM_OLD") ? 1 : 0;
    target = getenv("NR_TEST_L2NORM_TARGET") ? atoi(getenv("NR_TEST_L2NORM_TARGET")) : (int)LLR_NORM_TARGET;
    dk = getenv("NR_TEST_L2NORM_DK") ? atoi(getenv("NR_TEST_L2NORM_DK")) : 2;
  }
  if (old_rule || nl != 2 || qm < 4 || qm > 8 || (qm & 1))
    return nr_llr_norm_shift(llr, span);
  const uint32_t blk = 2u * (uint32_t)qm;
  uint64_t acc[2] = {0, 0};
  uint32_t nb = 0;
  for (uint32_t b = 0; (b + 1u) * blk <= span; b += NR_LLR_NORM_L2_STRIDE) {
    const int16_t *p = llr + (size_t)b * blk;
    for (int q = 0; q < qm; q++) {
      acc[0] += (uint32_t)abs(p[q]);
      acc[1] += (uint32_t)abs(p[qm + q]);
    }
    nb++;
  }
  if (nb == 0)
    return nr_llr_norm_shift(llr, span);
  const uint64_t wk = acc[0] < acc[1] ? acc[0] : acc[1], st = acc[0] < acc[1] ? acc[1] : acc[0];
  const uint32_t mean = (uint32_t)((wk + st) / (2u * (uint64_t)nb * (uint32_t)qm)); /* both layers: the stride-16 sample of the plain rule reads layer 0 only */
  int k = 0;
  while (k < 8 && (mean >> k) > (uint32_t)target)
    k++;
  int e = 0; /* extra bits for the weak layer: floor(log2(strong / weak)) of the mean |LLR|, at most `dk` */
  while (e < dk && (wk << (e + 1)) <= st)
    e++;
  return k > e ? k - e : 0;
}

/* NR_TEST_RANK2W_DIAG (replay-only test seam): per-layer statistics of a 2-layer LLR buffer (layer = (i / Qm) & 1: layer-mapping
 * of 38.211 7.3.1.3, one modulation symbol of Qm bits per layer in turn), before the shift k. Prints one line per call. */
#include <stdio.h>
static inline void nr_llr_layer_diag(const int16_t *llr, uint32_t G, int Qm, int k, unsigned rnti, int frame, int slot, int mcs)
{
  uint64_t sum[2] = {0, 0}, n[2] = {0, 0}, z0[2] = {0, 0}, z1[2] = {0, 0}, sat[2] = {0, 0};
  for (uint32_t i = 0; i < G; i++) {
    const int l = (int)((i / (uint32_t)Qm) & 1u);
    const uint32_t a = (uint32_t)abs(llr[i]);
    n[l]++;
    sum[l] += a;
    z0[l] += (a == 0);
    z1[l] += ((a >> k) == 0);
    sat[l] += ((a >> k) >= 127u);
  }
  for (int l = 0; l < 2; l++)
    if (!n[l]) n[l] = 1;
  fprintf(stderr, "RANK2W rnti=0x%04x %d.%d mcs=%d Qm=%d k=%d mean=%.1f,%.1f zero_pre=%.3f,%.3f zero_post=%.3f,%.3f sat=%.3f,%.3f\n", rnti, frame, slot, mcs, Qm, k,
          (double)sum[0] / n[0], (double)sum[1] / n[1], (double)z0[0] / n[0], (double)z0[1] / n[1], (double)z1[0] / n[0], (double)z1[1] / n[1],
          (double)sat[0] / n[0], (double)sat[1] / n[1]);
}
#endif
