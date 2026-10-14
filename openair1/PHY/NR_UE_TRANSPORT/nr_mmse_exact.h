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

/* MMSE3GPU: these functions are the CPU mode 3 and, compiled by nvcc (nr_pdsch_gpu_demod.cu, --fmad=false), the GPU mode 3.
 * GCC contracts a*b+c into fused multiply-adds by default (-ffp-contract=fast); here it must not, so that both sides do the
 * same IEEE operations (+ - * / sqrt, correctly rounded) and the GPU exact-demod CHECK can stay bit-exact. noinline keeps a
 * caller's contraction from reaching the inlined body. */
#if defined(__CUDACC__)
#define NR_MMSE_FN static __host__ __device__ inline
#define NR_MMSE_HD static __host__ __device__ inline
#elif defined(__GNUC__) && !defined(__clang__)
#define NR_MMSE_FN static __attribute__((noinline, unused, optimize("fp-contract=off")))
#else
#define NR_MMSE_FN static inline
#endif
#ifndef NR_MMSE_HD
#define NR_MMSE_HD static inline
#endif

NR_MMSE_FN int nr_mmse_exact_re(int nl, int nrx, const int16_t hre[4][4], const int16_t him[4][4], const int16_t yre[4],
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

NR_MMSE_HD int16_t nr_mmse3_s16(double v)
{
  return (int16_t)(v > 32767.0 ? 32767 : v < -32768.0 ? -32768 : lround(v));
}

/* MMSE3GPU: mode-3 output gain from the strongest layer's energy sum_{i,a} |h|^2 over the symbol's length REs: its mean
 * G_ll (2^-shift units, sc) lands at 2^10 LSB */
NR_MMSE_FN double nr_mmse3_gain(double emax, int length, double sc)
{
  const double gd = length > 0 ? emax * sc / length : 0.0;
  return gd > 0.0 ? 1024.0 / gd : 1.0;
}

/* MMSE3GPU: mode-3 output of one RE: z_l = x_l / [A^-1]_ll * gain and the per-layer thresholds w_l q / 32768,
 * w_l = (1 - s2 [A^-1]_ll) / [A^-1]_ll * gain (mu / [A^-1]_ll); a singular A or a vanishing [A^-1]_ll gives 0.
 * z[l] = {re, im}, mag[l] = {qa, qb, qr} thresholds. */
NR_MMSE_FN void nr_mmse3_re(int nl, int nrx, const int16_t hre[4][4], const int16_t him[4][4], const int16_t yre[4], const int16_t yim[4],
                            double sc, double s2, double gain, int16_t qa, int16_t qb, int16_t qr, int16_t z[4][2], int16_t mag[4][3])
{
  double xre[4], xim[4], ainv[4];
  const int ok = nr_mmse_exact_re(nl, nrx, hre, him, yre, yim, sc, s2, xre, xim, ainv, 0);
  for (int r = 0; r < nl; r++) {
    const double ai = ok ? ainv[r] : 0.0;
    const double w = ai > 1e-12 ? (1.0 - s2 * ai) / ai * gain : 0.0;
    z[r][0] = nr_mmse3_s16(ai > 1e-12 ? xre[r] / ai * gain : 0.0);
    z[r][1] = nr_mmse3_s16(ai > 1e-12 ? xim[r] / ai * gain : 0.0);
    mag[r][0] = nr_mmse3_s16(w * qa / 32768.0);
    mag[r][1] = nr_mmse3_s16(w * qb / 32768.0);
    mag[r][2] = nr_mmse3_s16(w * qr / 32768.0);
  }
}

/* MMSE3GPU: nr_dlsch_mmse() mode 3 for one data symbol of length REs (CPU; the GPU splits the same steps into k_mmse3_prep
 * and re_mmse3). Rows are interleaved c16 (re, im): y[a], h[r * nrx + a], and the outputs comp/mag/magb/magr[r]. Noise
 * variance s2: the ZF residual ||y - H x||^2 of every 4th RE over the nrx - nl free dimensions when nrx > nl, else
 * nvar x nvfac. Output gain: nr_mmse3_gain(). Returns s2 (2^-shift units). */
static inline double nr_mmse3_symbol(int nl, int nrx, int length, const int16_t *const *y, const int16_t *const *h, int shift,
                                     uint32_t nvar, double nvfac, int16_t qa, int16_t qb, int16_t qr, int16_t *const *comp,
                                     int16_t *const *mag, int16_t *const *magb, int16_t *const *magr)
{
  const double sc = ldexp(1.0, -shift); /* mode 2's units: G and b in |h|^2 >> shift */
  double gd = 0.0; /* integer sums, exact in double */
  for (int r = 0; r < nl; r++) {
    double e = 0.0;
    for (int i = 0; i < length; i++)
      for (int a = 0; a < nrx; a++) {
        const int16_t *v = h[r * nrx + a] + 2 * i;
        e += (double)v[0] * v[0] + (double)v[1] * v[1];
      }
    if (e > gd)
      gd = e;
  }
  const double gain = nr_mmse3_gain(gd, length, sc);
  double s2 = 0.0;
  if (nrx <= nl)
    s2 = (double)nvar * sc * nvfac;
  for (int pass = nrx > nl ? 0 : 1; pass < 2; pass++) {
    double res = 0.0;
    int nres = 0;
    for (int i = 0; i < length; i += pass == 0 ? 4 : 1) {
      int16_t hre[4][4], him[4][4], yre[4], yim[4];
      for (int a = 0; a < nrx; a++) {
        yre[a] = y[a][2 * i];
        yim[a] = y[a][2 * i + 1];
        for (int r = 0; r < nl; r++) {
          hre[r][a] = h[r * nrx + a][2 * i];
          him[r][a] = h[r * nrx + a][2 * i + 1];
        }
      }
      if (pass == 0) {
        double xre[4], xim[4], ainv[4], e = 0.0;
        const int ok = nr_mmse_exact_re(nl, nrx, hre, him, yre, yim, sc, 0.0, xre, xim, ainv, &e);
        res += ok ? e : 0.0;
        nres += ok;
        continue;
      }
      int16_t z[4][2], mg[4][3];
      nr_mmse3_re(nl, nrx, hre, him, yre, yim, sc, s2, gain, qa, qb, qr, z, mg);
      for (int r = 0; r < nl; r++) {
        comp[r][2 * i] = z[r][0];
        comp[r][2 * i + 1] = z[r][1];
        mag[r][2 * i] = mag[r][2 * i + 1] = mg[r][0];
        magb[r][2 * i] = magb[r][2 * i + 1] = mg[r][1];
        magr[r][2 * i] = magr[r][2 * i + 1] = mg[r][2];
      }
    }
    if (pass == 0)
      s2 = nres > 0 ? res / ((double)nres * (nrx - nl)) * sc : 0.0;
  }
  return s2;
}
#endif
