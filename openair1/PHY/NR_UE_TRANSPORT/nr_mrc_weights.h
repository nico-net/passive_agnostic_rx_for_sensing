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
/* Common-scale average AFTER channel conjugate multiplication. Widen before
 * summing so opposite phases can cancel before any saturation. Equal receiver
 * noise is assumed; this does not claim optimal weighting for unequal noise. */
static inline int16_t nr_mrc_average4(int16_t a, int16_t b, int16_t c, int16_t d)
{
  return ((int32_t)a + b + c + d) / 4;
}
#endif
