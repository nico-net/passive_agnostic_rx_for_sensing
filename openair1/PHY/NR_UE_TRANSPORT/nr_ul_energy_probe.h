/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_UL_ENERGY_PROBE_H
#define NR_UL_ENERGY_PROBE_H
/* ULEN maths (header only, no state): mean energy per RE over a PRB range, and a timing-agnostic DM-RS correlation.
 *
 * Correlation. z = y * p over the DM-RS REs (p = the conjugate of the transmitted sequence). The REs are summed COHERENTLY in groups of
 * NR_ULEN_GRP_RB PRBs after removing the ramp e^{-j 2 pi k tau / N} (k = subcarrier inside the group; tau = how many samples EARLIER the FFT
 * window would start, the sign of `ta=` in UL_PUSCH_CRC; a signal that is d samples late in the window peaks at tau = -d). Groups are
 * added incoherently, so the unknown channel phase of each group does not matter. Result = sum|coherent|^2 / sum(n_re * |z|^2): 1 for a clean
 * pilot, about 1/n_re (n_re = NR_ULEN_GRP_RB * RE per PRB) for noise or a wrong pilot. The best value over the tau set is returned, so the
 * same call with a wrong pilot gives what noise reaches after the same maximisation. Resolution in tau is ~ N / (12 * NR_ULEN_GRP_RB). */
#include <math.h>
#include <stdint.h>
#include "common/platform_types.h" /* c16_t */

#define NR_ULEN_GRP_RB 2
#define NR_ULEN_MAX_TAU 63

/* Adds the energy of the REs of PRBs [rb0, rb1) minus [ex0, ex1) to *sum and their count to *cnt. re_off NULL = all 12 REs of a PRB. */
static inline void nr_ulen_energy_acc(const c16_t *row, int fft, int first_carrier, int rb0, int rb1, int ex0, int ex1,
                                      const int *re_off, int nre, double *sum, uint64_t *cnt)
{
  if (!row || fft <= 0 || first_carrier < 0 || first_carrier >= fft) return;
  const int n = re_off ? nre : 12;
  for (int p = rb0; p < rb1; p++) {
    if (p >= ex0 && p < ex1) continue;
    for (int r = 0; r < n; r++) {
      const c16_t y = row[(first_carrier + p * 12 + (re_off ? re_off[r] : r)) % fft];
      *sum += (double)y.r * y.r + (double)y.i * y.i;
    }
    *cnt += (uint64_t)n;
  }
}

static inline double nr_ulen_db(double sum, uint64_t cnt)
{
  return (cnt && sum > 0.0) ? 10.0 * log10(sum / (double)cnt) : -99.0;
}

/* rows[r] = one FFT symbol of one antenna, pils[r] = its pilots indexed nre * carrier_prb + j (same for every antenna of a symbol).
 * The nrb PRBs start at carrier PRB rb0. taus[] is scanned in order, a later tau has to win strictly (give 0 first: ties keep the nearest). */
static inline double nr_ulen_corr_best(const c16_t *const *rows, const c16_t *const *pils, int nrow, int fft, int first_carrier, int rb0, int nrb,
                                       const int *re_off, int nre, const int *taus, int ntau, int *best_tau)
{
  *best_tau = 0;
  if (nrow < 1 || nrb < 1 || nre < 1 || nre > 6 || ntau < 1 || fft <= 0) return 0.0;
  if (ntau > NR_ULEN_MAX_TAU) ntau = NR_ULEN_MAX_TAU;
  double den = 0.0, best = -1.0;
  for (int t = 0; t < ntau; t++) {
    float cs[12 * NR_ULEN_GRP_RB], sn[12 * NR_ULEN_GRP_RB];
    for (int k = 0; k < 12 * NR_ULEN_GRP_RB; k++) {
      const double ph = -2.0 * M_PI * (double)k * (double)taus[t] / (double)fft;
      cs[k] = (float)cos(ph);
      sn[k] = (float)sin(ph);
    }
    double acc = 0.0;
    for (int g0 = 0; g0 < nrb; g0 += NR_ULEN_GRP_RB) {
      const int g1 = g0 + NR_ULEN_GRP_RB < nrb ? g0 + NR_ULEN_GRP_RB : nrb;
      for (int r = 0; r < nrow; r++) {
        double ar = 0.0, ai = 0.0, s2 = 0.0;
        for (int p = g0; p < g1; p++)
          for (int j = 0; j < nre; j++) {
            const int kk = 12 * (p - g0) + re_off[j];
            const c16_t y = rows[r][(first_carrier + 12 * (rb0 + p) + re_off[j]) % fft];
            const c16_t q = pils[r][nre * (rb0 + p) + j];
            const double zr = (double)y.r * q.r - (double)y.i * q.i, zi = (double)y.r * q.i + (double)y.i * q.r;
            ar += zr * cs[kk] - zi * sn[kk];
            ai += zi * cs[kk] + zr * sn[kk];
            s2 += zr * zr + zi * zi;
          }
        acc += ar * ar + ai * ai;
        if (t == 0) den += s2 * (double)((g1 - g0) * nre);
      }
    }
    if (acc > best * (1.0 + 1e-9)) { /* strictly better only */
      best = acc;
      *best_tau = taus[t];
    }
  }
  return den > 0.0 ? best / den : 0.0;
}
#endif
