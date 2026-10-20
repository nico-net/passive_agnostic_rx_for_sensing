#ifndef NR_PASSIVE_DELAY_FAST_H
#define NR_PASSIVE_DELAY_FAST_H
/* UL-LAT-20261018: nr_passive_est_delay()'s decision for an LS row whose support is
 * [0,m) with 8m <= n/2, without the n-point IDFT. That IDFT (one per antenna x DM-RS symbol, n = 4096) was ~55 % of the CPU
 * of a PUSCH decode call while a 3-PRB grant has m = 36 non-zero bins.
 *
 * P(t) = |sum_k ls[k] e^{+j 2 pi k t / n}|^2 has bandwidth m/n. A float FFT of np >= 8m points samples it at t = i*n/np. The
 * coarse sample nearest the fine maximum t* (<= R/2 = n/(2np) away) holds >= f P(t*), f = cos^2(pi m/(2np)) (Bernstein, applied
 * to Re(x conj(x(t*)))), so the fine maximum is searched, in double, only within +/- R/2 of the coarse samples >= f Cmax.
 * The peak/mean test of nr_est_delay (PEAK_DETECT_THRESHOLD 15) uses Parseval: mean over t of P = sum_k |ls[k]|^2.
 * Units: delay_max_val = P/(2 n^2), the input-domain units nr_passive_est_delay restores (unitary IDFT, |y|^2 >> 1, / n).
 * When even Cmax/f cannot clear the threshold (noise), est_delay = 0 and the coarse maximum is reported, unrefined.
 * Float vs the fixed-point IDFT: a ratio within rounding of 15 can decide differently. Returns false (the caller runs the
 * full estimator) when not applicable: np does not divide n, 8m > n/2, more than NR_PDF_MAX_CAND candidates. */
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include "common/utils/nr/nr_common.h"

#define NR_PDF_THRESHOLD 15.0
#define NR_PDF_MAX_CAND 8

static inline bool nr_ul_delay_fast_on(void) { return true; }

/* in-place radix-2 FFT with the +j kernel (an unscaled IDFT) */
static inline void nr_pdf_fft(float *re, float *im, int np)
{
  for (int i = 1, j = 0; i < np; i++) {
    int bit = np >> 1;
    for (; j & bit; bit >>= 1)
      j ^= bit;
    j ^= bit;
    if (i < j) {
      float t = re[i]; re[i] = re[j]; re[j] = t;
      t = im[i]; im[i] = im[j]; im[j] = t;
    }
  }
  for (int len = 2; len <= np; len <<= 1) {
    const int h = len >> 1;
    const double sr = cos(2.0 * M_PI / len), si = sin(2.0 * M_PI / len);
    double cr = 1, ci = 0; /* w^k by recurrence, in double: one sincos per stage */
    for (int k = 0; k < h; k++) {
      const float wr = (float)cr, wi = (float)ci;
      for (int i = k; i < np; i += len) {
        const float vr = re[i + h] * wr - im[i + h] * wi, vi = re[i + h] * wi + im[i + h] * wr;
        re[i + h] = re[i] - vr; im[i + h] = im[i] - vi;
        re[i] += vr; im[i] += vi;
      }
      const double t = cr * sr - ci * si;
      ci = cr * si + ci * sr;
      cr = t;
    }
  }
}

/* P(t) at one fine position, Horner in double */
static inline double nr_pdf_power(const c16_t *ls, int m, int n, int t)
{
  const double a = 2.0 * M_PI * (double)t / n, wr = cos(a), wi = sin(a);
  double ar = 0, ai = 0;
  for (int k = m - 1; k >= 0; k--) {
    const double r = ar * wr - ai * wi + ls[k].r, i = ar * wi + ai * wr + ls[k].i;
    ar = r; ai = i;
  }
  return ar * ar + ai * ai;
}

static inline bool nr_passive_est_delay_fast(int n, const c16_t *ls, delay_t *delay)
{
  if (n < 256)
    return false;
  int m = n;
  while (m > 0 && ls[m - 1].r == 0 && ls[m - 1].i == 0)
    m--;
  int np = 64;
  while (np < 8 * m)
    np <<= 1;
  if (np > n / 2 || np > 2048 || n % np)
    return false;
  float re[2048], im[2048];
  double e = 0;
  for (int k = 0; k < np; k++) {
    re[k] = k < m ? ls[k].r : 0.f;
    im[k] = k < m ? ls[k].i : 0.f;
    e += (double)re[k] * re[k] + (double)im[k] * im[k];
  }
  delay_t cand = {0};
  if (e > 0) {
    nr_pdf_fft(re, im, np);
    const int R = n / np;
    double cmax = 0;
    int imax = 0;
    for (int i = 0; i < np; i++) {
      const double c = (double)re[i] * re[i] + (double)im[i] * im[i];
      re[i] = (float)c; /* reuse as the coarse power */
      if (c > cmax) { cmax = c; imax = i; }
    }
    /* the coarse sample nearest the fine maximum (<= R/2 away) holds >= f * P(t*), f = cos^2(pi m / (2 np)) >= cos^2(pi/16) */
    const double f = pow(cos(M_PI * m / (2.0 * np)), 2) * (1.0 - 1e-4);
    double pbest = cmax;
    int tbest = imax * R;
    if (cmax > f * NR_PDF_THRESHOLD * e) { /* a peak may clear the threshold: refine around every coarse sample >= f cmax */
      int cand_i[NR_PDF_MAX_CAND], nc = 0;
      for (int i = 0; i < np; i++)
        if (re[i] >= f * cmax) {
          if (nc == NR_PDF_MAX_CAND)
            return false;
          cand_i[nc++] = i;
        }
      pbest = -1;
      for (int c = 0; c < nc; c++)
        for (int d = -R / 2 - 1; d <= R / 2 + 1; d++) {
          const int t = (cand_i[c] * R + d + n) % n;
          const double p = nr_pdf_power(ls, m, n, t);
          if (p > pbest || (p == pbest && t < tbest)) { pbest = p; tbest = t; }
        }
    }
    const int pos = tbest > n / 2 ? tbest - n : tbest;
    const double val = pbest / (2.0 * (double)n * n);
    cand.delay_max_pos = pos;
    cand.delay_max_val = val > 2147483647.0 ? 2147483647 : (int)val;
    cand.est_delay = pbest > NR_PDF_THRESHOLD * e ? pos : 0;
  }
  if (cand.delay_max_val >= delay->delay_max_val) /* nr_passive_est_delay()'s keep-the-stronger rule */
    *delay = cand;
  return true;
}
#endif
