/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* TIMRANK34: exact (double) per-RE multi-layer MMSE solve for nr_dlsch_mmse() mode 3. Pure header (C and C++), unit-tested by
 * test_nr_llr_norm (nr_llr_norm_test.cc).
 *
 * Inputs are the unshifted int16 channel estimates h[l][a] and samples y[a] of one RE (nl <= 4 layers, nrx <= 4 branches), the
 * scale sc (2^-shift, mode 2's units) and the noise variance s2 in those units. Gauss-Jordan on A = sc H^H H + s2 I with
 * [sc H^H y | I] appended gives x = A^-1 b and diag(A^-1). With s2 = 0 this is zero forcing; out_res (if non-NULL) then
 * receives ||y - H x||^2, the noise energy in the nrx - nl dimensional complement of H. Returns 0 when A is singular. */
#ifndef NR_MMSE_EXACT_H
#define NR_MMSE_EXACT_H
#include <math.h>
#include <stdint.h>

static inline int nr_mmse_exact_re(int nl, int nrx, const int16_t hre[4][4], const int16_t him[4][4], const int16_t yre[4],
                                   const int16_t yim[4], double sc, double s2, double xre[4], double xim[4], double ainv[4],
                                   double *out_res)
{
  double mr[4][9], mi[4][9]; /* [A | b | I] */
  const int nc = 2 * nl + 1;
  for (int r = 0; r < nl; r++) {
    double br = 0.0, bi = 0.0;
    for (int c = 0; c < nl; c++) {
      double gr = 0.0, gi = 0.0;
      for (int a = 0; a < nrx; a++) { /* conj(h_r) h_c */
        gr += (double)hre[r][a] * hre[c][a] + (double)him[r][a] * him[c][a];
        gi += (double)hre[r][a] * him[c][a] - (double)him[r][a] * hre[c][a];
      }
      mr[r][c] = gr * sc + (r == c ? s2 : 0.0);
      mi[r][c] = gi * sc;
      mr[r][nl + 1 + c] = r == c ? 1.0 : 0.0;
      mi[r][nl + 1 + c] = 0.0;
    }
    for (int a = 0; a < nrx; a++) { /* conj(h_r) y */
      br += (double)hre[r][a] * yre[a] + (double)him[r][a] * yim[a];
      bi += (double)hre[r][a] * yim[a] - (double)him[r][a] * yre[a];
    }
    mr[r][nl] = br * sc;
    mi[r][nl] = bi * sc;
  }
  double b0r[4], b0i[4];
  for (int r = 0; r < nl; r++) {
    b0r[r] = mr[r][nl];
    b0i[r] = mi[r][nl];
  }
  for (int k = 0; k < nl; k++) {
    int piv = k;
    for (int r = k + 1; r < nl; r++)
      if (hypot(mr[r][k], mi[r][k]) > hypot(mr[piv][k], mi[piv][k]))
        piv = r;
    if (hypot(mr[piv][k], mi[piv][k]) < 1e-12)
      return 0;
    if (piv != k)
      for (int c = 0; c < nc; c++) {
        double t = mr[k][c]; mr[k][c] = mr[piv][c]; mr[piv][c] = t;
        t = mi[k][c]; mi[k][c] = mi[piv][c]; mi[piv][c] = t;
      }
    const double d = mr[k][k] * mr[k][k] + mi[k][k] * mi[k][k], ir = mr[k][k] / d, ii = -mi[k][k] / d;
    for (int c = 0; c < nc; c++) {
      const double tr = mr[k][c] * ir - mi[k][c] * ii, ti = mr[k][c] * ii + mi[k][c] * ir;
      mr[k][c] = tr;
      mi[k][c] = ti;
    }
    for (int r = 0; r < nl; r++) {
      if (r == k)
        continue;
      const double fr = mr[r][k], fi = mi[r][k];
      if (fr == 0.0 && fi == 0.0)
        continue;
      for (int c = 0; c < nc; c++) {
        mr[r][c] -= fr * mr[k][c] - fi * mi[k][c];
        mi[r][c] -= fr * mi[k][c] + fi * mr[k][c];
      }
    }
  }
  for (int r = 0; r < nl; r++) {
    xre[r] = mr[r][nl];
    xim[r] = mi[r][nl];
    ainv[r] = mr[r][nl + 1 + r];
  }
  if (out_res) { /* ||y||^2 - Re(b^H x) / sc */
    double yy = 0.0, bx = 0.0;
    for (int a = 0; a < nrx; a++)
      yy += (double)yre[a] * yre[a] + (double)yim[a] * yim[a];
    for (int r = 0; r < nl; r++)
      bx += b0r[r] * xre[r] + b0i[r] * xim[r];
    const double e = yy - bx / sc;
    *out_res = e > 0.0 ? e : 0.0;
  }
  return 1;
}
#endif
