/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_PDSCH_DA_RECON_H
#define NR_PDSCH_DA_RECON_H
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include "nr_sense_quality.h" /* NR_SENSE_MAX_PRB */
/* x: the re-encoded modulation symbols, interleaved int16 (re, im) per RE ordinal; h: antenna-major complex float estimates H = Y/X. */
/* Reconstruction confidence (rank 1): share of data REs whose re-encoded symbol X equals the hard decision of the equalised symbol.
 * The equaliser is the leave-one-out per-PRB mean of the estimates H = Y/X (MRC over antennas), so a wrong X cannot vouch for itself:
 * eq = sum_a Y_a conj(Hs_a) / sum_a |Hs_a|^2 with Y_a = H_a X. The constellation unit u is the smallest |component| of the grant's own
 * symbols (QAM levels are odd multiples of u); NAN with < 64 symbols, or a PRB with < 2 REs (nothing to leave out).
 * shortcut: the per-PRB mean ignores the phase slope across the 12 subcarriers, so on a long-delay channel high-order QAM reads low;
 * upgrade to a +-2-subcarrier window when a calibration set shows the bias. */
static inline float nr_da_recon_conf(const float *h, uint32_t stride, uint32_t nant, const uint32_t *k, uint32_t n, const int16_t *x, uint32_t qm)
{
  if (n < 64 || qm < 2 || (qm & 1) || qm > 10 || nant < 1)
    return NAN;
  if (nant > 8)
    nant = 8;
  int umin = 0;
  for (uint32_t i = 0; i < n; i++) {
    const int r = abs((int)x[2 * i]), q = abs((int)x[2 * i + 1]);
    if (r && (!umin || r < umin)) umin = r;
    if (q && (!umin || q < umin)) umin = q;
  }
  if (!umin)
    return NAN;
  const float u = (float)umin;
  const int maxlev = (1 << (qm / 2)) - 1; /* odd levels -maxlev..maxlev, in units of u */
  /* stack, not TLS: a larger TLS block is what produced the AVX alignment fault noted above (<= 2 samples/PRB-symbol, 156 per PRB: float sums are exact enough) */
  float sum[NR_SENSE_MAX_PRB][8][2] = {{{0}}};
  uint16_t cnt[NR_SENSE_MAX_PRB] = {0};
  for (uint32_t i = 0; i < n; i++) {
    const uint32_t p = k[i] / 12;
    if (p >= NR_SENSE_MAX_PRB) return NAN;
    cnt[p]++;
    for (uint32_t a = 0; a < nant; a++) {
      sum[p][a][0] += h[2 * ((size_t)a * stride + i)];
      sum[p][a][1] += h[2 * ((size_t)a * stride + i) + 1];
    }
  }
  uint32_t seen = 0, match = 0;
  for (uint32_t i = 0; i < n; i++) {
    const uint32_t p = k[i] / 12;
    if (cnt[p] < 2) continue;
    const double inv = 1.0 / (cnt[p] - 1);
    double nr = 0, ni = 0, den = 0;
    for (uint32_t a = 0; a < nant; a++) {
      const double hr = h[2 * ((size_t)a * stride + i)], hi = h[2 * ((size_t)a * stride + i) + 1];
      const double sr = (sum[p][a][0] - hr) * inv, si = (sum[p][a][1] - hi) * inv;
      nr += hr * sr + hi * si; /* H conj(Hs) */
      ni += hi * sr - hr * si;
      den += sr * sr + si * si;
    }
    if (!(den > 0)) continue;
    const double rr = nr / den, ri = ni / den;
    const double er = (x[2 * i] * rr - x[2 * i + 1] * ri) / u, ei = (x[2 * i] * ri + x[2 * i + 1] * rr) / u; /* eq = X r, in units of u */
    int lr = 2 * (int)floor(er / 2.0) + 1, li = 2 * (int)floor(ei / 2.0) + 1; /* nearest odd level */
    lr = lr > maxlev ? maxlev : (lr < -maxlev ? -maxlev : lr);
    li = li > maxlev ? maxlev : (li < -maxlev ? -maxlev : li);
    const int xr = 2 * (int)floor((double)x[2 * i] / u / 2.0) + 1, xi = 2 * (int)floor((double)x[2 * i + 1] / u / 2.0) + 1;
    seen++;
    match += (lr == xr && li == xi);
  }
  return seen ? (float)match / (float)seen : NAN;
}
#endif
