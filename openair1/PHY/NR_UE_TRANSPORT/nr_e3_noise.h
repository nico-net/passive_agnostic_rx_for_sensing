/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* E3: noise per RE and antenna from the first DM-RS symbol of a slot, for nr_dl_layout_score() (header only, pure, unit-tested).
 *
 * RESIDUAL. y = the n_sc carrier subcarriers of one DM-RS symbol (n_ant arrays), gold = the identity's DM-RS bits. Type-1 group-0 pilots of PRB p sit at 12p + 2i, i = 0..5,
 * with value pilot(m = 6p + i) = ((1 - 2 c(2m)) + j (1 - 2 c(2m+1))) / sqrt 2 (unit modulus). z_i = y_i conj(pilot_i) = h + n_i with a channel h flat inside the PRB.
 * R = sum_a sum_i |z_i - mean_i z|^2 is, for noise-only PRBs (n_i ~ CN(0, s2) i.i.d.), s2 * Gamma(k, 1) with k = 5 n_ant: each antenna gives 5 independent
 * Exp(1) terms (6 values minus the estimated mean). e_p = R / k is therefore s2 * Gamma(k) / k, unbiased per PRB.
 * LOWER QUARTILE. A PRB that carries this DM-RS gives e_p = noise; one that does not gives at least the noise (its energy). So the 25 % quantile of e_p over the carrier is the
 * noise as long as at least a quarter of the PRBs are noise-like or carry the identity, and it is biased low by the quantile: q25(e) = s2 * G25(k) / k with G25(k) the 25 % point
 * of Gamma(k, 1) (an Erlang distribution: P(G <= x) = 1 - exp(-x) sum_{j<k} x^j / j!). The correction is c(k) = k / G25(k): c(5) = 1.43 (1 RX), c(10) = 1.25, c(20) = 1.18 (4 RX).
 * The sample quantile of n_prb values is read at the plotting position (n_prb + 1) / 4, which is the expectation-unbiased choice for an order statistic. */
#ifndef NR_E3_NOISE_H
#define NR_E3_NOISE_H
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "common/platform_types.h" /* c16_t */

/* 25 % point of Gamma(k, 1), integer k >= 1, by bisection on the Erlang CDF. */
static inline double nr_e3_gamma_q25(int k)
{
  double lo = 0.0, hi = 4.0 * k + 10.0;
  for (int it = 0; it < 80; it++) {
    const double x = 0.5 * (lo + hi);
    double term = 1.0, sum = 1.0;
    for (int j = 1; j < k; j++) { term *= x / j; sum += term; }
    const double cdf = 1.0 - exp(-x) * sum;
    if (cdf < 0.25) lo = x; else hi = x;
  }
  return 0.5 * (lo + hi);
}
static inline int nr_e3_cmp_f(const void *a, const void *b) { const float x = *(const float *)a, y = *(const float *)b; return (x > y) - (x < y); }

/* y[a] = n_sc subcarriers of antenna a (CRB 0 first), gold = >= 12 * n_sc / 12 bits. Returns the noise variance estimate, 0 on bad arguments. scratch: n_sc / 12 floats. */
static inline float nr_e3_noise_from_pilots(const c16_t *const *y, int n_ant, int n_sc, const uint8_t *gold, float *scratch)
{
  const int np = n_sc / 12;
  if (!y || n_ant < 1 || np < 4 || !gold || !scratch) return 0.f;
  for (int p = 0; p < np; p++) {
    double acc = 0;
    for (int a = 0; a < n_ant; a++) {
      double zr[6], zi[6], mr = 0, mi = 0;
      for (int i = 0; i < 6; i++) {
        const int m = 6 * p + i;
        const double pr = (1 - 2 * gold[2 * m]) * 0.70710678118654752, pi = (1 - 2 * gold[2 * m + 1]) * 0.70710678118654752;
        const c16_t v = y[a][12 * p + 2 * i];
        zr[i] = v.r * pr + v.i * pi; zi[i] = v.i * pr - v.r * pi;   /* y conj(p) */
        mr += zr[i]; mi += zi[i];
      }
      mr /= 6; mi /= 6;
      for (int i = 0; i < 6; i++) acc += (zr[i] - mr) * (zr[i] - mr) + (zi[i] - mi) * (zi[i] - mi);
    }
    scratch[p] = (float)(acc / (5.0 * n_ant));
  }
  qsort(scratch, (size_t)np, sizeof scratch[0], nr_e3_cmp_f);
  int idx = (int)lround((np + 1) / 4.0) - 1;   /* plotting position (np + 1) / 4, 0-based */
  if (idx < 0) idx = 0;
  const int k = 5 * n_ant;
  return (float)(scratch[idx] * (k / nr_e3_gamma_q25(k)));
}
#endif
