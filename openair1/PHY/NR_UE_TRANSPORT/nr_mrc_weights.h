#ifndef __NR_MRC_WEIGHTS_H__
#define __NR_MRC_WEIGHTS_H__
#include <math.h>
#include <stdint.h>
/* NOISE-WEIGHTED MRC. The combiner sums h_a* y_a over branches with ONE scalar noise variance, which
 * is optimal only when every branch has the same noise. Measured OTA 2026-09-14: per-branch channel-
 * estimate noise [57 23000 281559 6314] -- up to 37 dB apart -- so the equal-noise sum injected the
 * noisy branches' noise at full weight and every combining mode lost to branch 0 alone. Scaling h_a
 * AND y_a by w_a = sqrt(nvar_min / nvar_a) makes the same sum  sum w_a^2 h_a* y_a  =  sum h_a* y_a /
 * (nvar_a / nvar_min), i.e. true MRC: a branch 30 dB noisier contributes 1/1000 of its terms, and
 * the combiner can never do worse than the best single branch. A branch with no estimate (nvar 0)
 * gets weight 0. */
static inline int nr_mrc_noise_weights(const uint32_t *nvar, int n, double *w)
{
  uint32_t nmin = 0;
  for (int a = 0; a < n; a++)
    if (nvar[a] > 0 && (nmin == 0 || nvar[a] < nmin)) nmin = nvar[a];
  int live = 0;
  for (int a = 0; a < n; a++) {
    w[a] = (nvar[a] > 0 && nmin > 0) ? sqrt((double)nmin / (double)nvar[a]) : 0.0;
    if (w[a] > 0.0) live++;
  }
  return live;
}
/* How many branches' worth of amplitude the weighted sum can reach -- the headroom the fixed-point
 * shift must reserve. sum w^2, not the branch count: charging 2 bits for four branches of which
 * three are weighted ~0 would throw away LLR resolution for nothing. */
static inline int nr_mrc_effective_branches(const double *w, int n)
{
  double s = 0.0;
  for (int a = 0; a < n; a++) s += w[a] * w[a];
  const int e = (int)ceil(s - 1e-9);
  return e < 1 ? 1 : e;
}
/* MRCBUG (ISAC_RX_MRC_SHIFT_SUM): branches' worth of amplitude an UNWEIGHTED MRC over the live mask reaches,
 * round(sum_live |h_a|^2 / max_live |h_a|^2) in 1..n. Balanced branches -> n (the old count); one dominant branch ->
 * 1 (the single-branch shift). Same integer arithmetic as nrd_decide() in nr_pdsch_gpu_demod.cuh (bit-exact). */
static inline int nr_mrc_shift_branches(const int32_t *avg, int live_mask, int n, int32_t avg_max)
{
  if (avg_max <= 0) return 1;
  int64_t s = 0;
  for (int a = 0; a < n; a++)
    if (live_mask & (1 << a)) s += avg[a];
  int e = (int)((s + avg_max / 2) / avg_max);
  return e < 1 ? 1 : e > n ? n : e;
}
/* PER-BRANCH NOISE-WEIGHTED MRC for the integer rank-1 combiner (CPU nr_rx_pdsch and GPU nrd_decide / k_mask_pw, bit-equal; the GPU twin is nrd_noise_wq in
 * nr_pdsch_gpu_demod.cuh). nv[a] = the chest's per-RX DM-RS residual noise (nr_dl_chest_nvar_ant), nre = the REs of the grant's first data symbol (PRBs p = nre / 12).
 * The branches of live_mask are scaled (h AND y) by wq[a] / 2^15 ~ sqrt(nmin / nv'[a]) (nmin: the live branch with the least noise), so the stock sum h* y / sum |h|^2
 * computes sum h_a* y_a / nv_a with the QAM thresholds sum |h_a|^2 / nv_a: the LLR scale stays consistent. The reference is the least-noise LIVE branch, so a dead
 * (noise-floor) branch that the live mask excluded cannot shrink the others.
 * nv' = the noise estimates shrunk toward their mean over the live branches: the estimate of one branch has a relative variance c / p (measured on the product chest,
 * equal-noise AWGN, 2..100 PRB: p * sum_a (nv_a / mean - 1)^2 / (n - 1) = 0.45..0.57, no dependence on p), so on a small grant equal branches would get random
 * weights (10 PRB: -0.15 dB). With d_a = nv_a / mean - 1 and S = sum d_a^2 the deviations are scaled by max(0, 1 - K (n - 1) c / (p S)) (K = 2: the test for "the
 * branches differ more than estimation noise"): equal branches stay at weight 1 (the former equal-noise combiner), a 10 dB spread keeps its weights. Plain IEEE
 * +,-,*,/,sqrt only (no libm transcendental), so the device twin is bit-equal; compiled without FMA contraction.
 * Returns 0 (all wq = 2^15, nothing to scale) when a live branch has no noise estimate (nv 0: no chest, a unit test), fewer than 2 branches are live, or the weights
 * are all 1. Q15 scaling of an int16 sample: nr_mrc_scale_q15. */
#define NR_MRC_W_C 0.5
#define NR_MRC_W_K 2.0
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("-ffp-contract=off")))
#endif
static inline int nr_mrc_noise_wq15(int nb, int live_mask, const uint32_t *nv, uint32_t nre, uint16_t *wq)
{
  double mean = 0, nvs[8];
  int n = 0;
  for (int a = 0; a < nb; a++) wq[a] = 32768;
  if (!nv || nb > 8) return 0;
  for (int a = 0; a < nb; a++) {
    if (!(live_mask >> a & 1)) continue;
    if (nv[a] == 0) return 0;
    mean += (double)nv[a];
    n++;
  }
  if (n < 2) return 0;
  mean /= (double)n;
  double S = 0;
  for (int a = 0; a < nb; a++) {
    if (!(live_mask >> a & 1)) continue;
    const double d = (double)nv[a] / mean - 1.0;
    S += d * d;
  }
  const double p = (double)(nre / 12 < 1 ? 1 : nre / 12);
  double lam = 0;
  if (S > 0) {
    const double t = (NR_MRC_W_K * (double)(n - 1) * NR_MRC_W_C) / (p * S);
    lam = t >= 1.0 ? 0.0 : 1.0 - t;
  }
  double nmin = 0;
  for (int a = 0; a < nb; a++) {
    if (!(live_mask >> a & 1)) continue;
    nvs[a] = mean + lam * ((double)nv[a] - mean);
    if (nmin == 0 || nvs[a] < nmin) nmin = nvs[a];
  }
  int active = 0;
  for (int a = 0; a < nb; a++) {
    if (!(live_mask >> a & 1)) { wq[a] = 0; continue; }
    wq[a] = (uint16_t)floor(32768.0 * sqrt(nmin / nvs[a]) + 0.5);
    active |= wq[a] != 32768;
  }
  return active;
}
static inline int16_t nr_mrc_scale_q15(int16_t v, uint16_t wq)
{
  return (int16_t)(((int32_t)v * (int32_t)wq + 16384) >> 15);
}
/* The headroom of the weighted sum: levels avg[a] (|h|^2 means) times (wq/2^15)^2 for the live branches. *avgs = the largest weighted level, *contributing =
 * round(sum / largest) in 1..n_live, the nr_mrc_shift_branches() rule on weighted energies. */
static inline void nr_mrc_weighted_headroom(int nb, int live_mask, const int32_t *avg, const uint16_t *wq, int32_t *avgs, int *contributing)
{
  int64_t mx = 0, sum = 0;
  int nlive = 0;
  for (int a = 0; a < nb; a++) {
    if (!(live_mask >> a & 1)) continue;
    const int64_t w = ((int64_t)avg[a] * wq[a] * wq[a]) >> 30;
    mx = w > mx ? w : mx;
    sum += w;
    nlive++;
  }
  int c = mx > 0 ? (int)((sum + mx / 2) / mx) : 1;
  c = c < 1 ? 1 : c > nlive ? (nlive < 1 ? 1 : nlive) : c;
  *avgs = (int32_t)mx;
  *contributing = c;
}
/* Common-scale average AFTER channel conjugate multiplication. Widen before
 * summing so opposite phases can cancel before any saturation. Equal receiver
 * noise is assumed; this does not claim optimal weighting for unequal noise. */
static inline int16_t nr_mrc_average4(int16_t a, int16_t b, int16_t c, int16_t d)
{
  return ((int32_t)a + b + c + d) / 4;
}
#endif
