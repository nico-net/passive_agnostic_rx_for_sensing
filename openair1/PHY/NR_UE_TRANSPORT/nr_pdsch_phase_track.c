/* SPDX-License-Identifier: LicenseRef-CSSL-1.0
 * PHASE-TRACK CPU side (see nr_pdsch_phase_track.h). Built with -ffp-contract=off: every fused multiply-add is an explicit fma(), the GPU twin (nr_pdsch_gpu_demod.cu, --fmad=false) evaluates
 * the same expressions, so the double results are bit-identical. */
#include <math.h>
#include <string.h>
#include "nr_pdsch_phase_track.h"
#include "nr_pdsch_gpu_demod.cuh"

#define NRPT_T double
#define NRPT_S _d
#define NRPT_FN static inline
#define NRPT_FMA(a, b, c) fma((a), (b), (c))
#define NRPT_DIV(a, b) ((a) / (b))
#define NRPT_SQRT(a) sqrt(a)
#define NRPT_RND(v) lround(v)
#include "nr_pdsch_phase_track_impl.h"
#undef NRPT_T
#undef NRPT_S
#undef NRPT_FN
#undef NRPT_FMA
#undef NRPT_DIV
#undef NRPT_SQRT
#undef NRPT_RND

int nr_pdsch_phase_tinterp(c16_t *const rows[], int nl, int nrx, int ofdm_size, int ncol, uint16_t dmrs_mask, int s0, int s1)
{
  int dsym[14], nd = 0;
  for (int m = 0; m < 14; m++)
    if ((dmrs_mask >> m) & 1)
      dsym[nd++] = m;
  if (nd < 2 || nl * nrx > NR_PT_MAX_ROWS || ncol <= 0 || ncol > NRPT_MAXCH * NRPT_BLK)
    return 0;
  const int nblk = (ncol + NRPT_BLK - 1) / NRPT_BLK;
  /* the phase of each DM-RS pair, per branch and block: sum over layers and the block's subcarriers of hb conj(ha); integer sums, exact */
  double cd[13][4][NRPT_MAXCH], sd[13][4][NRPT_MAXCH]; /* 15 kB of stack: no file-scope state (multi-PCI static audit) */
  for (int p = 0; p + 1 < nd; p++)
    for (int a = 0; a < nrx && a < 4; a++)
      for (int c = 0; c < nblk; c++) {
        long long cr = 0, ci = 0;
        const int k1 = (c + 1) * NRPT_BLK < ncol ? (c + 1) * NRPT_BLK : ncol;
        for (int l = 0; l < nl; l++) {
          const struct nrd_c16 *ha = (const struct nrd_c16 *)(rows[l * nrx + a] + (size_t)ofdm_size * dsym[p]);
          const struct nrd_c16 *hb = (const struct nrd_c16 *)(rows[l * nrx + a] + (size_t)ofdm_size * dsym[p + 1]);
          for (int k = c * NRPT_BLK; k < k1; k++) {
            cr += (long long)hb[k].r * ha[k].r + (long long)hb[k].i * ha[k].i;
            ci += (long long)hb[k].i * ha[k].r - (long long)hb[k].r * ha[k].i;
          }
        }
        nrpt_delta_phasor_d((double)cr, (double)ci, &cd[p][a][c], &sd[p][a][c]);
      }
  int interpolated = 0;
  for (int m = s0; m < s1 && m < 14; m++) {
    if ((dmrs_mask >> m) & 1)
      continue;
    int p = -1;
    for (int q = 0; q < nd; q++)
      if (dsym[q] < m)
        p = q;
    if (p < 0 || p + 1 >= nd) { /* before the first / after the last DM-RS symbol: hold the nearest one before it (the first one when none precedes) */
      const int src = dsym[p < 0 ? 0 : p];
      for (int r = 0; r < nl * nrx; r++)
        memcpy(rows[r] + (size_t)ofdm_size * m, rows[r] + (size_t)ofdm_size * src, sizeof(c16_t) * (size_t)ncol);
      continue;
    }
    const double t = (double)(m - dsym[p]) / (double)(dsym[p + 1] - dsym[p]);
    interpolated = 1;
    for (int a = 0; a < nrx; a++)
      for (int c = 0; c < nblk; c++) {
        const double dc = a < 4 ? cd[p][a][c] : 1.0, ds = a < 4 ? sd[p][a][c] : 0.0;
        double ur, ui;
        nrpt_blend_phasor_d(t, dc, ds, &ur, &ui);
        const int k1 = (c + 1) * NRPT_BLK < ncol ? (c + 1) * NRPT_BLK : ncol;
        for (int l = 0; l < nl; l++) {
          const struct nrd_c16 *ha = (const struct nrd_c16 *)(rows[l * nrx + a] + (size_t)ofdm_size * dsym[p]);
          const struct nrd_c16 *hb = (const struct nrd_c16 *)(rows[l * nrx + a] + (size_t)ofdm_size * dsym[p + 1]);
          struct nrd_c16 *ho = (struct nrd_c16 *)(rows[l * nrx + a] + (size_t)ofdm_size * m);
          for (int k = c * NRPT_BLK; k < k1; k++)
            ho[k] = nrpt_interp_re_d(ha[k], hb[k], t, dc, ds, ur, ui);
        }
      }
  }
  return interpolated;
}

/* ZF output of RE i; 0 when the RE is ill-conditioned */
static inline int cpe_re(int nl, int a0, int nb, uint32_t i, c16_t *const y[4], c16_t *const h[4][4], double *xr, double *xi)
{
  double yr[4], yi[4], hr[4][4], hi[4][4];
  for (int a = 0; a < nb; a++) {
    yr[a] = y[a0 + a][i].r;
    yi[a] = y[a0 + a][i].i;
    for (int l = 0; l < nl; l++) {
      hr[l][a] = h[l][a0 + a][i].r;
      hi[l][a] = h[l][a0 + a][i].i;
    }
  }
  return nrpt_zf_d(nl, nb, yr, yi, hr, hi, xr, xi);
}

void nr_pdsch_cpe_sums(int nl, int a0, int nb, uint32_t nre, c16_t *const y[4], c16_t *const h[4][4], nr_pt_cpe_sums_t *s)
{
  memset(s, 0, sizeof(*s));
  const uint32_t nch = (nre + NRPT_BLK - 1) / NRPT_BLK;
  /* pass 1: mean power of each layer's ZF output. Chunk partials added in chunk order (the GPU kernel's order). */
  for (uint32_t c = 0; c < nch; c++) {
    double pp[4] = {0, 0, 0, 0};
    uint32_t pn = 0;
    const uint32_t i1 = (c + 1) * NRPT_BLK < nre ? (c + 1) * NRPT_BLK : nre;
    for (uint32_t i = c * NRPT_BLK; i < i1; i++) {
      double xr[4], xi[4];
      if (!cpe_re(nl, a0, nb, i, y, h, xr, xi))
        continue;
      for (int l = 0; l < nl; l++)
        pp[l] = pp[l] + nrpt_pow2_d(xr[l], xi[l]);
      pn++;
    }
    for (int l = 0; l < nl; l++)
      s->pw[l] = s->pw[l] + pp[l];
    s->npw[0] += pn;
  }
  for (int l = 1; l < nl; l++)
    s->npw[l] = s->npw[0];
  double g[4] = {0, 0, 0, 0};
  for (int l = 0; l < nl; l++)
    g[l] = s->pw[l] > 0.0 && s->npw[l] > 0 ? 1.0 / (s->pw[l] / (double)s->npw[l]) : 0.0;
  /* pass 2: the clipped 4th-power statistic */
  for (uint32_t c = 0; c < nch; c++) {
    double pr[4] = {0, 0, 0, 0}, pi[4] = {0, 0, 0, 0}, pq[4] = {0, 0, 0, 0};
    uint32_t pn[4] = {0, 0, 0, 0};
    const uint32_t i1 = (c + 1) * NRPT_BLK < nre ? (c + 1) * NRPT_BLK : nre;
    for (uint32_t i = c * NRPT_BLK; i < i1; i++) {
      double xr[4], xi[4];
      if (!cpe_re(nl, a0, nb, i, y, h, xr, xi))
        continue;
      for (int l = 0; l < nl; l++) {
        double tr, ti, tq;
        if (g[l] > 0.0 && nrpt_term4_d(xr[l], xi[l], g[l], &tr, &ti, &tq)) {
          pr[l] = pr[l] + tr;
          pi[l] = pi[l] + ti;
          pq[l] = pq[l] + tq;
          pn[l]++;
        }
      }
    }
    for (int l = 0; l < nl; l++) {
      s->sr[l] = s->sr[l] + pr[l];
      s->si[l] = s->si[l] + pi[l];
      s->q[l] = s->q[l] + pq[l];
      s->n[l] += pn[l];
    }
  }
}

int nr_pdsch_cpe_symbol(int nl, int a0, int nb, uint32_t nre, c16_t *const y[4], c16_t *const h[4][4], double phi_out[4])
{
  for (int l = 0; l < 4; l++)
    phi_out[l] = 0.0;
  if (nl < 1 || nl > 4 || nb < 1 || nb > 4 || a0 < 0 || a0 + nb > 4 || nre < NRPT_MIN_RE)
    return 0;
  nr_pt_cpe_sums_t s;
  nr_pdsch_cpe_sums(nl, a0, nb, nre, y, h, &s);
  double cr[4], ci[4];
  int ok[4], nok = 0;
  for (int l = 0; l < nl; l++)
    nok += ok[l] = nrpt_cpe_decide_d(s.sr[l], s.si[l], s.q[l], s.n[l], &cr[l], &ci[l]);
  if (nl > 1 && nok < nl) { /* a layer without a significant estimate of its own takes the pooled one: the oscillator phase noise is common to the layers */
    double sr = 0, si = 0, q = 0;
    uint32_t n = 0;
    for (int l = 0; l < nl; l++) {
      sr = sr + s.sr[l];
      si = si + s.si[l];
      q = q + s.q[l];
      n += s.n[l];
    }
    double pcr, pci;
    if (nrpt_cpe_decide_d(sr, si, q, n, &pcr, &pci))
      for (int l = 0; l < nl; l++)
        if (!ok[l]) {
          cr[l] = pcr;
          ci[l] = pci;
          ok[l] = 1;
          nok++;
        }
  }
  int rotated = 0;
  for (int l = 0; l < nl; l++) {
    if (!ok[l])
      continue;
    rotated++;
    phi_out[l] = atan2(ci[l], cr[l]); /* counters only: never feeds the rotation */
    for (int a = a0; a < a0 + nb; a++) {
      struct nrd_c16 *row = (struct nrd_c16 *)h[l][a];
      for (uint32_t i = 0; i < nre; i++)
        row[i] = nrpt_rot_re_d(row[i], cr[l], ci[l]);
    }
  }
  return rotated;
}
