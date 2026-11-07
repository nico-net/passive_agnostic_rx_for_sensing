/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* See nr_pdsch_mimo_ls.h. */
#include "nr_pdsch_mimo_ls.h"
#include <math.h>
#include <string.h>

typedef struct { double r, i; } cd;
static inline cd cmul(cd a, cd b) { return (cd){a.r * b.r - a.i * b.i, a.r * b.i + a.i * b.r}; }
static inline cd csub(cd a, cd b) { return (cd){a.r - b.r, a.i - b.i}; }
static inline cd cconj(cd a) { return (cd){a.r, -a.i}; }
static inline double cabs2(cd a) { return a.r * a.r + a.i * a.i; }
static inline cd cdiv(cd a, cd b)
{
  const double d = cabs2(b);
  return (cd){(a.r * b.r + a.i * b.i) / d, (a.i * b.r - a.r * b.i) / d};
}

/* Inverse of the nl x nl matrix g by Gauss-Jordan with partial pivoting; -1 when singular. */
static int inv(int nl, const cd g[NR_MIMO_LS_MAX_NL][NR_MIMO_LS_MAX_NL], cd out[NR_MIMO_LS_MAX_NL][NR_MIMO_LS_MAX_NL])
{
  cd a[NR_MIMO_LS_MAX_NL][2 * NR_MIMO_LS_MAX_NL];
  double scale = 0;
  for (int r = 0; r < nl; r++)
    for (int c = 0; c < 2 * nl; c++) {
      a[r][c] = c < nl ? g[r][c] : (cd){c - nl == r ? 1.0 : 0.0, 0.0};
      if (c < nl && cabs2(g[r][c]) > scale) scale = cabs2(g[r][c]);
    }
  if (!(scale > 0)) return -1;
  for (int p = 0; p < nl; p++) {
    int best = p;
    for (int r = p + 1; r < nl; r++)
      if (cabs2(a[r][p]) > cabs2(a[best][p])) best = r;
    if (cabs2(a[best][p]) < 1e-18 * scale) return -1;
    if (best != p)
      for (int c = 0; c < 2 * nl; c++) { cd t = a[p][c]; a[p][c] = a[best][c]; a[best][c] = t; }
    const cd piv = a[p][p];
    for (int c = 0; c < 2 * nl; c++) a[p][c] = cdiv(a[p][c], piv);
    for (int r = 0; r < nl; r++) {
      if (r == p) continue;
      const cd f = a[r][p];
      for (int c = 0; c < 2 * nl; c++) a[r][c] = csub(a[r][c], cmul(f, a[p][c]));
    }
  }
  for (int r = 0; r < nl; r++)
    for (int c = 0; c < nl; c++) out[r][c] = a[r][nl + c];
  return 0;
}

static int window_ex(int n, int nl, int nant, const int16_t *x, const int16_t *y, size_t ystride, float *h, double *resid, double *gdiag)
{
  if (n < nl || nl < 1 || nl > NR_MIMO_LS_MAX_NL || nant < 1 || nant > NR_MIMO_LS_MAX_ANT) return -1;
  cd g[NR_MIMO_LS_MAX_NL][NR_MIMO_LS_MAX_NL] = {{{0}}}, gi[NR_MIMO_LS_MAX_NL][NR_MIMO_LS_MAX_NL];
  for (int i = 0; i < n; i++)
    for (int l = 0; l < nl; l++) {
      const cd xl = {x[2 * (i * nl + l)], x[2 * (i * nl + l) + 1]};
      for (int m = 0; m < nl; m++) {
        const cd xm = {x[2 * (i * nl + m)], x[2 * (i * nl + m) + 1]};
        const cd t = cmul(cconj(xl), xm);
        g[l][m].r += t.r;
        g[l][m].i += t.i;
      }
    }
  if (inv(nl, g, gi) != 0) return -1;
  for (int l = 0; l < nl; l++)
    if (!(g[l][l].r * gi[l][l].r <= NR_MIMO_LS_RHO_MAX)) return -1; /* diagonal of G and G^-1 are real; also rejects NaN */
  cd hh[NR_MIMO_LS_MAX_NL][NR_MIMO_LS_MAX_ANT];
  double res = 0;
  for (int a = 0; a < nant; a++) {
    const int16_t *ya = y + 2 * (size_t)a * ystride;
    cd b[NR_MIMO_LS_MAX_NL] = {{0}};
    for (int i = 0; i < n; i++) {
      const cd yi = {ya[2 * i], ya[2 * i + 1]};
      for (int l = 0; l < nl; l++) {
        const cd t = cmul(cconj((cd){x[2 * (i * nl + l)], x[2 * (i * nl + l) + 1]}), yi);
        b[l].r += t.r;
        b[l].i += t.i;
      }
    }
    for (int l = 0; l < nl; l++) {
      cd s = {0, 0};
      for (int m = 0; m < nl; m++) {
        const cd t = cmul(gi[l][m], b[m]);
        s.r += t.r;
        s.i += t.i;
      }
      hh[l][a] = s;
    }
    for (int i = 0; i < n; i++) {
      cd e = {ya[2 * i], ya[2 * i + 1]};
      for (int l = 0; l < nl; l++) e = csub(e, cmul(hh[l][a], (cd){x[2 * (i * nl + l)], x[2 * (i * nl + l) + 1]}));
      res += cabs2(e);
    }
  }
  for (int l = 0; l < nl; l++)
    for (int a = 0; a < nant; a++) {
      h[2 * (l * nant + a)] = (float)hh[l][a].r;
      h[2 * (l * nant + a) + 1] = (float)hh[l][a].i;
    }
  *resid = res;
  if (gdiag)
    for (int l = 0; l < nl; l++) gdiag[l] = gi[l][l].r; /* noise gain of layer l: var(h_l) = sigma_y^2 * (G^-1)_ll */
  return 0;
}
int nr_pdsch_mimo_ls_window(int n, int nl, int nant, const int16_t *x, const int16_t *y, size_t ystride, float *h, double *resid)
{
  return window_ex(n, nl, nant, x, y, ystride, h, resid, NULL);
}

typedef struct {
  double resid, power;
  double dof; /* complex degrees of freedom of the residual: windows * nant * (n - nl) */
  int used, skipped, written;
} pass_t;

/* One sweep of width W over every run of data REs. `step` > 1 evaluates every step-th window only (the flatness ladder samples;
 * the final pass is complete). Output rows are written when out_h != NULL. */
static void sweep(int nre, int nl, int nant, const int16_t *x, const uint32_t *k, const uint32_t *sym, const int16_t *y, size_t ystride,
                  int W, int step, int out_cap, size_t out_stride, float *out_h, uint32_t *out_k, uint32_t *out_sym, uint16_t *out_raw, float *out_nv,
                  pass_t *p)
{
  memset(p, 0, sizeof(*p));
  int widx = 0, i = 0;
  while (i < nre) {
    int j = i + 1; /* run [i, j): same symbol, neighbours at most 6 apart */
    while (j < nre && sym[j] == sym[i] && k[j] > k[j - 1] && k[j] - k[j - 1] <= 6) j++;
    const int len = j - i, nwin = len / W; /* the last window absorbs the remainder (W..2W-1 REs) */
    for (int w = 0; w < nwin; w++, widx++) {
      if (!out_h && (widx % step)) continue;
      const int s = i + w * W, n = (w == nwin - 1) ? len - w * W : W;
      float h[2 * NR_MIMO_LS_MAX_NL * NR_MIMO_LS_MAX_ANT];
      double res, gd[NR_MIMO_LS_MAX_NL];
      /* gathered y planes for the window: nr_pdsch_mimo_ls_window takes plane stride, so offset the base */
      if (window_ex(n, nl, nant, x + 2 * (size_t)s * nl, y + 2 * (size_t)s, ystride, h, &res, gd) != 0) {
        p->skipped++;
        continue;
      }
      p->used++;
      p->resid += res;
      p->dof += (double)nant * (n - nl);
      for (int a = 0; a < nant; a++)
        for (int q = 0; q < n; q++) {
          const double yr = y[2 * ((size_t)a * ystride + s + q)], yi = y[2 * ((size_t)a * ystride + s + q) + 1];
          p->power += yr * yr + yi * yi;
        }
      if (out_h && p->written < out_cap) {
        const int o = p->written++;
        for (int l = 0; l < nl; l++)
          for (int a = 0; a < nant; a++) {
            out_h[2 * ((size_t)(l * nant + a) * out_stride + o)] = h[2 * (l * nant + a)];
            out_h[2 * ((size_t)(l * nant + a) * out_stride + o) + 1] = h[2 * (l * nant + a) + 1];
          }
        out_k[o] = k[s + (n - 1) / 2];
        out_sym[o] = sym[s];
        out_raw[o] = (uint16_t)n;
        if (out_nv) /* the layer's per-RE noise variance in H scale: LS residual per complex dof (unbiased) x the layer's noise gain */
          for (int l = 0; l < nl; l++) out_nv[(size_t)l * out_stride + o] = (float)(res / ((double)nant * (n - nl)) * gd[l]);
      }
    }
    i = j;
  }
}

int nr_pdsch_mimo_ls_cfr_nv(int nre, int nl, int nant, const int16_t *x, const uint32_t *k_abs, const uint32_t *sym, const int16_t *y,
                            size_t ystride, int out_cap, size_t out_stride, float *out_h, uint32_t *out_k, uint32_t *out_sym,
                            uint16_t *out_raw, float *out_nv, nr_pdsch_mimo_ls_info_t *info)
{
  if (info) memset(info, 0, sizeof(*info));
  if (nre < 2 * nl || nl < 1 || nl > NR_MIMO_LS_MAX_NL || nant < 1 || nant > NR_MIMO_LS_MAX_ANT) return 0;
  /* Flatness ladder on a sample of windows (<= ~64 per width): widen while the residual stays within the statistical spread of the
   * narrowest width's residual per degree of freedom. A chi-square of d complex dof has relative spread 1/sqrt(d). */
  const int W0 = 2 * nl;
  const int step0 = (nre / W0 + 63) / 64;
  pass_t p0;
  sweep(nre, nl, nant, x, k_abs, sym, y, ystride, W0, step0 > 0 ? step0 : 1, 0, 0, NULL, NULL, NULL, NULL, NULL, &p0);
  int W = W0;
  if (p0.used > 0 && p0.dof > 0) {
    const double r0 = p0.resid / p0.dof;
    for (int cand = 2 * W0; cand <= NR_MIMO_LS_MAX_WIN; cand *= 2) {
      pass_t pc;
      const int stepc = (nre / cand + 63) / 64;
      sweep(nre, nl, nant, x, k_abs, sym, y, ystride, cand, stepc > 0 ? stepc : 1, 0, 0, NULL, NULL, NULL, NULL, NULL, &pc);
      if (pc.used == 0 || pc.dof <= 0) break;
      const double rc = pc.resid / pc.dof;
      const double tol = 1.0 + 3.0 / sqrt(p0.dof) + 3.0 / sqrt(pc.dof);
      const double floor_ = 1e-6 * (pc.power / (double)(pc.used * nant * cand > 0 ? pc.used * nant * cand : 1)); /* numerical zero */
      if (rc > r0 * tol + floor_) break;
      W = cand;
    }
  }
  pass_t pf;
  sweep(nre, nl, nant, x, k_abs, sym, y, ystride, W, 1, out_cap, out_stride, out_h, out_k, out_sym, out_raw, out_nv, &pf);
  if (info) {
    info->win = W;
    info->n_windows = pf.written;
    info->n_skipped = pf.skipped;
  }
  return pf.written;
}

int nr_pdsch_mimo_ls_cfr(int nre, int nl, int nant, const int16_t *x, const uint32_t *k_abs, const uint32_t *sym, const int16_t *y,
                         size_t ystride, int out_cap, size_t out_stride, float *out_h, uint32_t *out_k, uint32_t *out_sym,
                         uint16_t *out_raw, nr_pdsch_mimo_ls_info_t *info)
{
  return nr_pdsch_mimo_ls_cfr_nv(nre, nl, nant, x, k_abs, sym, y, ystride, out_cap, out_stride, out_h, out_k, out_sym, out_raw, NULL, info);
}
