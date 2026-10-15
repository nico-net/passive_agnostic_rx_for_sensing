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

#define NR_MMSE_NM(x) x
#define NR_MMSE_R double
#define NR_MMSE_SQRT sqrt
#define NR_MMSE_LROUND lround
#include "nr_mmse_exact_body.h"
#undef NR_MMSE_NM
#undef NR_MMSE_R
#undef NR_MMSE_SQRT
#undef NR_MMSE_LROUND
/* Task 5 (GPU pass FP32 demod): the float instantiation, nr_mmse_exact_re_f / nr_mmse3_s16_f / nr_mmse3_gain_f / nr_mmse3_re_f. Not bit-exact
 * with the double one by design; compiled only where asked (the CUDA file) so that the CPU build is untouched. */
#if defined(__CUDACC__) || defined(NR_MMSE_WITH_FLOAT)
#define NR_MMSE_NM(x) x##_f
#define NR_MMSE_R float
#define NR_MMSE_SQRT sqrtf
#define NR_MMSE_LROUND lroundf
#include "nr_mmse_exact_body.h"
#undef NR_MMSE_NM
#undef NR_MMSE_R
#undef NR_MMSE_SQRT
#undef NR_MMSE_LROUND
#endif

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
