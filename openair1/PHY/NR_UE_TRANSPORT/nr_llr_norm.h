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

/* HMCS 2026-10-25: 256QAM target. The plain rule below samples every 16th LLR; with Qm = 8 that is always the same bit of the RE (8 LLRs per RE, the
 * layer-1 stride 16 = every second RE, index 0 of 8 = the OUTERMOST bit of the I rail, |LLR| = |Re y|, mean 8a where a is the lattice half-spacing).
 * Capping that mean at LLR_NORM_TARGET = 40 leaves a in (2.5, 5] LSB, i.e. a noise sigma of 1-2 LSB at the SNR a 256QAM grant needs (a/sigma ~ 2.4 at
 * the MCS 27 waterfall): the int8 decoder then works on a 5-11 level alphabet for its inner bits. nr_dlsim, 1 RX AWGN, 273 PRB, MCS 27 table 2, 16
 * iterations, 40-100 trials per point: the 50 % BLER point sits at 27.0 dB for a >= 10 LSB, 27.3 dB for a = 5, 27.7 dB for a = 3.5 and above 28.8 dB for
 * a = 2.5 [MEASURED, sens6 x86, HMCS-20261025]. For 256QAM the statistic is therefore capped at 8 * NR_LLR_NORM_A256 with the lattice half-spacing
 * ceiling NR_LLR_NORM_A256 = 24 LSB: after the power-of-two shift a is in (12, 24] (or the raw value when that is already lower), sigma 5-10 LSB at the
 * waterfall, outer bits clip at 127 (flat region of the same sweep, a = 10..80 at MCS 27, 5..20 at MCS 20). QPSK/16QAM/64QAM keep the target 40. */
#define NR_LLR_NORM_A256 24u
static inline uint32_t nr_llr_norm_target(int qm)
{
  return qm == 8 ? 8u * NR_LLR_NORM_A256 : LLR_NORM_TARGET;
}

/* Right shift for llr[0, span) with the target of modulation order qm: nr_llr_norm_shift with `target` in place of LLR_NORM_TARGET. */
static inline int nr_llr_norm_shift_t(const int16_t *llr, uint32_t span, uint32_t target)
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
  while (k < 8 && (mean >> k) > target)
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
/* HMCS 2026-10-25: the two-layer statistic is the mean |LLR| over ALL Qm bits of both layers, 3.75 a for 256QAM (8a, 4a, 2a, a), so the target 40 left the strong
 * layer at a in (5.3, 10.7] LSB before the weak-layer bonus. nr_dlsim 2 layers, 2 RX, 106 PRB MCS 27 table 2, 30.0 dB, 60 trials, BLER by a: 3 -> 1.00, 5 -> 0.82,
 * 8 -> 0.43, 12 -> 0.22, 20 -> 0.22, 40 -> 0.23, 80 -> 0.33 [MEASURED, sens6 x86, dlsim stock equaliser]: the same dependence as one layer. Qm 8 target:
 * 3.75 * 19 = 71 (a in (9.6, 19] for balanced layers; the weak-layer bonus of up to 2 bits keeps a strong layer under ~76 at a 12 dB gap). Qm 4/6: 40 as before. */
static inline uint32_t nr_llr_norm_l2_default(int qm)
{
  return qm == 8 ? 71u : LLR_NORM_TARGET;
}
static inline int nr_llr_norm_shift_nl(const int16_t *llr, uint32_t span, int nl, int qm)
{
  static int old_rule = -1, target = 0, dk = 2, target_env = 0; /* NR_TEST_L2NORM_OLD=1: the pre-2026-10-23 rule (A/B seam); NR_TEST_L2NORM_TARGET: weak-layer target (exploration) */
  if (old_rule < 0) {
    old_rule = getenv("NR_TEST_L2NORM_OLD") ? 1 : 0;
    target_env = getenv("NR_TEST_L2NORM_TARGET") != NULL;
    target = target_env ? atoi(getenv("NR_TEST_L2NORM_TARGET")) : (int)LLR_NORM_TARGET;
    dk = getenv("NR_TEST_L2NORM_DK") ? atoi(getenv("NR_TEST_L2NORM_DK")) : 2;
  }
  if (old_rule || nl != 2 || qm < 4 || qm > 8 || (qm & 1))
    return old_rule ? nr_llr_norm_shift(llr, span) /* NR_TEST_L2NORM_OLD: the whole pre-HMCS rule, 256QAM included */
                    : nr_llr_norm_shift_t(llr, span, nl == 2 ? LLR_NORM_TARGET /* two layers: unchanged, owned by the RANK2 work */ : nr_llr_norm_target(qm));
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
  const uint32_t tgt = target_env ? (uint32_t)target : nr_llr_norm_l2_default(qm);
  while (k < 8 && (mean >> k) > tgt)
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
