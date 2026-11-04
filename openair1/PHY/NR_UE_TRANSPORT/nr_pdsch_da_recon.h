/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_PDSCH_DA_RECON_H
#define NR_PDSCH_DA_RECON_H
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "nr_sense_quality.h" /* NR_SENSE_MAX_PRB, NR_SENSE_PRB_RECON_MIN */

/* Per-RE symbol accuracy of a data-aided reconstruction (spec 4(d)).
 *
 * WHAT IT MEASURES. Data-aided reconstructs X only from CRC-verified TBs, so X is right by construction. This is therefore NOT "is the
 * reconstruction correct": it is the share of data REs whose RAW hard decision, taken from the received Y equalised with a channel estimate
 * that never used that RE's own Y, equals the verified X. It reads high when the local channel model (precoder/PRG, delay, Doppler) and
 * the SNR support symbol decisions, and low when they do not. S4 must read it as a per-PRB model-fit/SNR measure, not as a correctness flag.
 *
 * ESTIMATOR. For RE i (symbol l, subcarrier k, PRB p = k/12) the equaliser uses the leave-one-out, symmetric local window
 * {l-1,l,l+1} x {k-2..k+2} clipped to PRB p (never across a PRB/PRG boundary) and to symbols that exist:
 *   Hs_a = sum_n Y_a,n conj(X_n) / sum_n |X_n|^2 = sum_n H_a,n w_n / sum_n w_n     (n != i; ML weighting w_n = |X_n|^2, H = Y/X;
 *          the window is used as point-reflected pairs about (l,k), both members present, each with the pair's smaller |X|^2)
 *   eq   = sum_a Y_a,i conj(Hs_a) / sum_a |Hs_a|^2 = X_i * (sum_a H_a,i conj(Hs_a) / sum_a |Hs_a|^2)
 * Pairing makes the window exactly symmetric, so the first-order phase slope (delay) across subcarriers and the first-order rotation
 * (CFO/Doppler) across symbols cancel in the weighted mean whatever the random |X|.
 * The decision is the nearest odd QAM level per axis. The constellation unit u is the smallest |component| of the grant's symbols,
 * cross-checked against the unit implied by the symbols' mean energy (the modulator normalises to unit mean energy): a mismatch > 15 %
 * or too few symbols (< 64, < 256 for 256QAM, so the innermost level is present) gives NAN and no PRB verdicts.
 *
 * h: antenna-major complex float H = Y/X, stride in complex REs. k/l: subcarrier (absolute) and symbol of each RE, REs ordered symbol by
 * symbol with k ascending (the data-aided enumeration). x: interleaved int16 (re,im) of the re-encoded symbols, RE ordinal i <-> x[2i].
 * ok_abs (out, (NR_SENSE_MAX_PRB+7)/8 bytes): bit p = PRB p passes (>= 6 scored REs and agreement >= NR_SENSE_PRB_RECON_MIN).
 * Returns the grant-level agreement (all scored REs), NAN when not computable. Stack use < 2 KB. */
#define NR_DA_RECON_MAXSYM 16
static inline float nr_da_recon(const float *h, uint32_t stride, uint32_t nant, const uint32_t *k, const uint32_t *l, uint32_t n,
                                const int16_t *x, uint32_t qm, uint8_t ok_abs[(NR_SENSE_MAX_PRB + 7) / 8])
{
  memset(ok_abs, 0, (NR_SENSE_MAX_PRB + 7) / 8);
  if (qm < 2 || (qm & 1) || qm > 8 || nant < 1 || n < (qm >= 8 ? 256u : 64u))
    return NAN;
  if (nant > 8)
    nant = 8;
  const int M = 1 << (qm / 2);
  int umin = 0;
  double e = 0;
  for (uint32_t i = 0; i < n; i++) {
    const int r = abs((int)x[2 * i]), q = abs((int)x[2 * i + 1]);
    if (r && (!umin || r < umin)) umin = r;
    if (q && (!umin || q < umin)) umin = q;
    e += (double)x[2 * i] * x[2 * i] + (double)x[2 * i + 1] * x[2 * i + 1];
  }
  if (!umin)
    return NAN;
  const double u_e = sqrt(e / n * 3.0 / (2.0 * ((double)M * M - 1.0))); /* E|x|^2 = 2 (M^2-1)/3 u^2 */
  if (fabs(umin - u_e) > 0.15 * u_e)
    return NAN;
  const float u = (float)umin;
  const int maxlev = M - 1;
  struct { uint32_t b, e; uint32_t l; } sp[NR_DA_RECON_MAXSYM];
  int ns = 0;
  for (uint32_t i = 0; i < n;) {
    uint32_t j = i;
    while (j < n && l[j] == l[i]) j++;
    if (ns == NR_DA_RECON_MAXSYM) return NAN;
    sp[ns].b = i; sp[ns].e = j; sp[ns].l = l[i]; ns++;
    i = j;
  }
  uint16_t seen[NR_SENSE_MAX_PRB] = {0}, match[NR_SENSE_MAX_PRB] = {0};
  for (int s = 0; s < ns; s++) {
    const int up = (s > 0 && sp[s - 1].l + 1 == sp[s].l) ? s - 1 : -1, dn = (s + 1 < ns && sp[s].l + 1 == sp[s + 1].l) ? s + 1 : -1;
    for (uint32_t i = sp[s].b; i < sp[s].e; i++) {
      const uint32_t p = k[i] / 12;
      if (p >= NR_SENSE_MAX_PRB) return NAN;
      float num[8][2] = {{0}};
      double den = 0;
      /* point-reflected pairs about (l,k): a pair is used only when BOTH members exist, each with the pair's smaller weight, so the window is
       * exactly symmetric and the linear phase terms (delay across k, CFO across l) cancel in the weighted mean whatever |X| is */
      for (int pass = 0; pass < 2; pass++) {
        for (int dk = (pass ? -2 : 1); dk <= 2; dk++) {
          const int sa = pass ? up : s, sb = pass ? dn : s;
          if (sa < 0 || sb < 0) break;
          const int ka = (int)k[i] + (pass ? dk : -dk), kb = (int)k[i] + (pass ? -dk : dk);
          if (ka < 0 || kb < 0 || (uint32_t)ka / 12 != p || (uint32_t)kb / 12 != p) continue;
          int ja = -1, jb = -1;
          { uint32_t lo = sp[sa].b, hi = sp[sa].e; while (lo < hi) { uint32_t m = (lo + hi) / 2; if ((int)k[m] < ka) lo = m + 1; else hi = m; }
            if (lo < sp[sa].e && (int)k[lo] == ka) ja = (int)lo; }
          { uint32_t lo = sp[sb].b, hi = sp[sb].e; while (lo < hi) { uint32_t m = (lo + hi) / 2; if ((int)k[m] < kb) lo = m + 1; else hi = m; }
            if (lo < sp[sb].e && (int)k[lo] == kb) jb = (int)lo; }
          if (ja < 0 || jb < 0) continue;
          const float wa = (float)x[2 * ja] * x[2 * ja] + (float)x[2 * ja + 1] * x[2 * ja + 1];
          const float wb = (float)x[2 * jb] * x[2 * jb] + (float)x[2 * jb + 1] * x[2 * jb + 1];
          const float w = wa < wb ? wa : wb;
          den += 2.0 * w;
          for (uint32_t a = 0; a < nant; a++)
            for (int c = 0; c < 2; c++)
              num[a][c] += w * (h[2 * ((size_t)a * stride + ja) + c] + h[2 * ((size_t)a * stride + jb) + c]);
        }
      }
      if (!(den > 0)) continue;
      double nr = 0, ni = 0, dd = 0;
      for (uint32_t a = 0; a < nant; a++) {
        const double sr = num[a][0] / den, si = num[a][1] / den;
        const double hr = h[2 * ((size_t)a * stride + i)], hi = h[2 * ((size_t)a * stride + i) + 1];
        nr += hr * sr + hi * si; /* H conj(Hs) */
        ni += hi * sr - hr * si;
        dd += sr * sr + si * si;
      }
      if (!(dd > 0)) continue;
      const double rr = nr / dd, ri = ni / dd;
      const double er = (x[2 * i] * rr - x[2 * i + 1] * ri) / u, ei = (x[2 * i] * ri + x[2 * i + 1] * rr) / u; /* eq = X r, units of u */
      int lr = 2 * (int)floor(er / 2.0) + 1, li = 2 * (int)floor(ei / 2.0) + 1; /* nearest odd level */
      lr = lr > maxlev ? maxlev : (lr < -maxlev ? -maxlev : lr);
      li = li > maxlev ? maxlev : (li < -maxlev ? -maxlev : li);
      const int xr = 2 * (int)floor((double)x[2 * i] / u / 2.0) + 1, xi = 2 * (int)floor((double)x[2 * i + 1] / u / 2.0) + 1;
      seen[p]++;
      match[p] += (lr == xr && li == xi);
    }
  }
  uint32_t S = 0, Mt = 0;
  for (uint32_t p = 0; p < NR_SENSE_MAX_PRB; p++) {
    S += seen[p]; Mt += match[p];
    if (seen[p] >= 6 && (float)match[p] >= NR_SENSE_PRB_RECON_MIN * (float)seen[p]) ok_abs[p >> 3] |= (uint8_t)(1u << (p & 7));
  }
  return S ? (float)Mt / (float)S : NAN;
}
#endif
