/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* TIMRANK34: exact (double) per-RE multi-layer MMSE solve for nr_dlsch_mmse() mode 3. Pure header (C and C++), unit-tested by
 * test_nr_llr_norm (nr_llr_norm_test.cc).
 *
 * Inputs are the unshifted int16 channel estimates h[l][a] and samples y[a] of one RE (nl <= 4 layers, nrx <= 4 branches), the
 * scale sc (2^-shift, mode 2's units) and the noise variance s2 in those units. A = sc H^H H + s2 I is Hermitian positive
 * definite, so a Cholesky A = L L^H gives x = A^-1 b (b = sc H^H y) and diag(A^-1) = column norms of L^-1 at a fraction of a
 * Gauss-Jordan's cost. With s2 = 0 this is zero forcing; out_res (if non-NULL) then receives ||y - H x||^2 = ||y||^2 - b^H A^-1 b
 * / sc, the noise energy in the nrx - nl dimensional complement of H. Returns 0 when A is singular. */
#ifndef NR_MMSE_EXACT_H
#define NR_MMSE_EXACT_H
#include <math.h>
#include <stdint.h>

static inline int nr_mmse_exact_re(int nl, int nrx, const int16_t hre[4][4], const int16_t him[4][4], const int16_t yre[4],
                                   const int16_t yim[4], double sc, double s2, double xre[4], double xim[4], double ainv[4],
                                   double *out_res)
{
  double ar[4][4], ai[4][4], br[4], bi[4]; /* lower triangle of A (i >= j), b */
  for (int i = 0; i < nl; i++) {
    for (int j = 0; j <= i; j++) { /* A[i][j] = sum_a conj(h_i) h_j */
      double gr = 0.0, gi = 0.0;
      for (int a = 0; a < nrx; a++) {
        gr += (double)hre[i][a] * hre[j][a] + (double)him[i][a] * him[j][a];
        gi += (double)hre[i][a] * him[j][a] - (double)him[i][a] * hre[j][a];
      }
      ar[i][j] = gr * sc + (i == j ? s2 : 0.0);
      ai[i][j] = gi * sc;
    }
    double ur = 0.0, ui = 0.0; /* conj(h_i) y */
    for (int a = 0; a < nrx; a++) {
      ur += (double)hre[i][a] * yre[a] + (double)him[i][a] * yim[a];
      ui += (double)hre[i][a] * yim[a] - (double)him[i][a] * yre[a];
    }
    br[i] = ur * sc;
    bi[i] = ui * sc;
  }
  /* Cholesky: L[i][j] (i > j), real diagonal d[j] */
  double d[4], lr[4][4], li[4][4];
  for (int j = 0; j < nl; j++) {
    double s = ar[j][j];
    for (int k = 0; k < j; k++)
      s -= lr[j][k] * lr[j][k] + li[j][k] * li[j][k];
    if (!(s > 1e-12 * (ar[j][j] > 1.0 ? ar[j][j] : 1.0)))
      return 0;
    d[j] = sqrt(s);
    for (int i = j + 1; i < nl; i++) { /* (A[i][j] - sum_k L[i][k] conj(L[j][k])) / d_j */
      double tr = ar[i][j], ti = ai[i][j];
      for (int k = 0; k < j; k++) {
        tr -= lr[i][k] * lr[j][k] + li[i][k] * li[j][k];
        ti -= li[i][k] * lr[j][k] - lr[i][k] * li[j][k];
      }
      lr[i][j] = tr / d[j];
      li[i][j] = ti / d[j];
    }
  }
  /* L u = b */
  double ur[4], ui[4], uu = 0.0;
  for (int i = 0; i < nl; i++) {
    double tr = br[i], ti = bi[i];
    for (int k = 0; k < i; k++) {
      tr -= lr[i][k] * ur[k] - li[i][k] * ui[k];
      ti -= lr[i][k] * ui[k] + li[i][k] * ur[k];
    }
    ur[i] = tr / d[i];
    ui[i] = ti / d[i];
    uu += ur[i] * ur[i] + ui[i] * ui[i];
  }
  /* L^H x = u */
  for (int i = nl - 1; i >= 0; i--) {
    double tr = ur[i], ti = ui[i];
    for (int k = i + 1; k < nl; k++) { /* conj(L[k][i]) x_k */
      tr -= lr[k][i] * xre[k] + li[k][i] * xim[k];
      ti -= lr[k][i] * xim[k] - li[k][i] * xre[k];
    }
    xre[i] = tr / d[i];
    xim[i] = ti / d[i];
  }
  /* M = L^-1 (lower): [A^-1]_ll = sum_{k >= l} |M[k][l]|^2 */
  for (int l = 0; l < nl; l++) {
    double mr[4], mi[4];
    mr[l] = 1.0 / d[l];
    mi[l] = 0.0;
    double n = mr[l] * mr[l];
    for (int i = l + 1; i < nl; i++) {
      double tr = 0.0, ti = 0.0;
      for (int k = l; k < i; k++) {
        tr += lr[i][k] * mr[k] - li[i][k] * mi[k];
        ti += lr[i][k] * mi[k] + li[i][k] * mr[k];
      }
      mr[i] = -tr / d[i];
      mi[i] = -ti / d[i];
      n += mr[i] * mr[i] + mi[i] * mi[i];
    }
    ainv[l] = n;
  }
  if (out_res) {
    double yy = 0.0;
    for (int a = 0; a < nrx; a++)
      yy += (double)yre[a] * yre[a] + (double)yim[a] * yim[a];
    const double e = yy - uu / sc;
    *out_res = e > 0.0 ? e : 0.0;
  }
  return 1;
}
#endif
