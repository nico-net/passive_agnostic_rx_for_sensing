/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/* Decimated float PSS correlator and multi-capture fold; see nr_pss_decim.h for the why. */
#include "nr_pss_decim.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

int nr_pss_decim_factor(int N)
{
  return N >= 512 ? N / 256 : 0;
}

#define PSS_LEN 127

/* in-place radix-2 FFT (sign -1: forward, +1: inverse unscaled); N power of two */
static void fft(double *re, double *im, int N, int sign)
{
  for (int i = 1, j = 0; i < N; i++) {
    int bit = N >> 1;
    for (; j & bit; bit >>= 1)
      j ^= bit;
    j ^= bit;
    if (i < j) {
      double t = re[i]; re[i] = re[j]; re[j] = t;
      t = im[i]; im[i] = im[j]; im[j] = t;
    }
  }
  for (int len = 2; len <= N; len <<= 1)
    for (int i = 0; i < N; i += len)
      for (int k = 0; k < len / 2; k++) {
        const double a = sign * 2 * M_PI * k / len, wr = cos(a), wi = sin(a);
        const int p = i + k, q = i + k + len / 2;
        const double xr = re[q] * wr - im[q] * wi, xi = re[q] * wi + im[q] * wr;
        re[q] = re[p] - xr; im[q] = im[p] - xi;
        re[p] += xr; im[p] += xi;
      }
}

/* Bandlimit the int16 PSS reference to its 127 occupied bins centred on kc (signed bin). The reference out of the int16 IDFT
 * carries a DC bias and a stray ~60-bin component near DC that outweigh the PSS when it sits off DC (TIM 923 ssb 233: PSS at
 * bin -950, any spectrum-derived centre lands near DC); a decimation chain would follow them. Writes 2N int16 scaled to
 * peak 8192. */
static void pss_clean(const int16_t *x, int N, int kc, int16_t *clean)
{
  double *re = malloc(sizeof(double) * 2 * N), *im = re + N;
  for (int n = 0; n < N; n++) {
    re[n] = x[2 * n];
    im[n] = x[2 * n + 1];
  }
  fft(re, im, N, -1);
  const int b0 = ((kc - PSS_LEN / 2) % N + N) % N;
  for (int k = 0; k < N; k++)
    if (((k - b0) % N + N) % N >= PSS_LEN)
      re[k] = im[k] = 0;
  fft(re, im, N, +1);
  double mx = 1;
  for (int n = 0; n < N; n++)
    mx = fmax(mx, fmax(fabs(re[n]), fabs(im[n])));
  for (int n = 0; n < N; n++) {
    clean[2 * n] = (int16_t)lround(re[n] / mx * 8192);
    clean[2 * n + 1] = (int16_t)lround(im[n] / mx * 8192);
  }
  free(re);
}

/* CIC-3 taps: boxcar(D) convolved 3 times, normalised to unit DC gain */
static int cic3(int D, float *h)
{
  const int n = 3 * D - 2;
  for (int t = 0; t < n; t++) {
    double v = 0; /* number of (a,b,c) in [0,D)^3 with a+b+c = t */
    for (int a = 0; a < D; a++)
      for (int b = 0; b < D; b++) {
        const int c = t - a - b;
        v += (c >= 0 && c < D);
      }
    h[t] = (float)(v / ((double)D * D * D));
  }
  return n;
}

void nr_pss_decimate(const int16_t *x, int xlen, int s0, int nout, int D, int kc, int N, nr_cf_t *out)
{
  float h[3 * 256];
  const int nt = cic3(D, h);
  float *cs = malloc(sizeof(float) * 2 * N); /* e^{-j2pi kc n/N}, period N */
  for (int n = 0; n < N; n++) {
    const double a = -2 * M_PI * (double)kc * n / N;
    cs[2 * n] = (float)cos(a);
    cs[2 * n + 1] = (float)sin(a);
  }
  for (int m = 0; m < nout; m++) {
    float ar = 0, ai = 0;
    const int c = s0 + m * D;
    for (int t = 0; t < nt; t++) {
      const int n = c - t;
      if (n < 0 || n >= xlen)
        continue;
      const float xr = x[2 * n], xi = x[2 * n + 1];
      const int p = (int)(((unsigned)n) % (unsigned)N);
      const float wr = cs[2 * p] * h[t], wi = cs[2 * p + 1] * h[t];
      ar += xr * wr - xi * wi;
      ai += xr * wi + xi * wr;
    }
    out[m] = (nr_cf_t){ar, ai};
  }
  free(cs);
}

int nr_pss_decim_ref(const int16_t *ref, int N, int D, int kc, nr_cf_t *out)
{
  const int L = N / D + 3;
  int16_t *c = malloc(sizeof(int16_t) * 2 * N);
  pss_clean(ref, N, kc, c);
  nr_pss_decimate(c, N, 0, L, D, kc, N, out);
  free(c);
  return L;
}

void nr_pss_corr_acc(const nr_cf_t *xd, int npos, const nr_cf_t *rd, int L, float *prof)
{
  for (int m = 0; m < npos; m++) {
    float cr = 0, ci = 0;
    const nr_cf_t *x = xd + m;
    for (int j = 0; j < L; j++) { /* conj(r) * x */
      cr += rd[j].r * x[j].r + rd[j].i * x[j].i;
      ci += rd[j].r * x[j].i - rd[j].i * x[j].r;
    }
    prof[m] += cr * cr + ci * ci;
  }
}

void nr_pss_cdot(const int16_t *r, const int16_t *x, int n, double *re, double *im)
{
  double a = 0, b = 0;
  for (int k = 0; k < n; k++) {
    a += (double)r[2 * k] * x[2 * k] + (double)r[2 * k + 1] * x[2 * k + 1];
    b += (double)r[2 * k] * x[2 * k + 1] - (double)r[2 * k + 1] * x[2 * k];
  }
  *re = a;
  *im = b;
}

void nr_pss_fold_reset(nr_pss_fold_t *f)
{
  for (int s = 0; s < 3; s++) {
    free(f->acc[s]);
    f->acc[s] = NULL;
  }
  f->period = 0;
  f->key = 0;
  f->last_abs = -1;
  f->captures = 0;
}

void nr_pss_fold_update(nr_pss_fold_t *f, int s, int64_t a0, float *prof, int n, int period, int K, int key)
{
  if (f->period != period || f->key != key || f->K != K || (f->last_abs >= 0 && a0 + n <= f->last_abs - period)) {
    nr_pss_fold_reset(f);
    f->period = period;
    f->key = key;
    f->K = K;
  }
  if (!f->acc[s])
    f->acc[s] = calloc(period, sizeof(float));
  const float keep = K > 1 ? 1.0f - 1.0f / K : 0.0f;
  float *acc = f->acc[s];
  for (int m = 0; m < n; m++) {
    const int64_t a = a0 + m;
    float *v = &acc[((a % period) + period) % period];
    *v = *v * keep + prof[m];
    prof[m] = *v;
  }
  if (a0 + n > f->last_abs)
    f->last_abs = a0 + n;
  if (s == 0)
    f->captures++;
}
