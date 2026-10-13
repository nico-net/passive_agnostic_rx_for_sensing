/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* MPCI M1 per-cell DDC, see iq_ddc.h. Halfband y[m] = 0.5 x[2m+c] + sum_k p_k (x[2m+c+j_k] + x[2m+c-j_k]), j_k odd:
 * the centre tap reads the odd input phase, every pair reads the even phase, so after one deinterleave each tap is a
 * contiguous axpy over m (vectorises). */
#include "iq_ddc.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static double bessel_i0(double x)
{
  double s = 1, t = 1;
  for (int k = 1; k < 64; ++k) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; if (t < 1e-12 * s) break; }
  return s;
}

int iq_ddc_halfband_len(double fp, double atten_db)
{
  const double df = 0.5 - 2 * fp; /* transition fp .. 0.5-fp, in cycles per input sample */
  if (!(fp > 0) || df <= 0.005) return -1;
  int n = (int)ceil((atten_db - 7.95) / (14.36 * df)) + 1;
  if (n < 7) n = 7;
  while ((n - 3) % 4) ++n; /* (n-1)/2 odd: both end taps are nonzero */
  return n > 1023 ? -1 : n;
}

static int stage_init(iq_ddc_stage_t *s, double fp, double atten_db)
{
  const int L = iq_ddc_halfband_len(fp, atten_db);
  if (L < 0) return -1;
  const int c = (L - 1) / 2;
  const double beta = atten_db > 50 ? 0.1102 * (atten_db - 8.7) : 0.5842 * pow(atten_db - 21, 0.4) + 0.07886 * (atten_db - 21);
  s->ntaps = L; s->npairs = (c + 1) / 2;
  s->pair = calloc((size_t)s->npairs, sizeof(float));
  s->hr = calloc((size_t)L, sizeof(float)); s->hi = calloc((size_t)L, sizeof(float));
  if (!s->pair || !s->hr || !s->hi) return -1;
  double sum = 0, tmp[512];
  for (int k = 0; k < s->npairs; ++k) {
    const int j = 2 * k + 1;
    const double r = (double)j / c;
    tmp[k] = sin(M_PI * j / 2) / (M_PI * j) * bessel_i0(beta * sqrt(1 - r * r)) / bessel_i0(beta);
    sum += 2 * tmp[k];
  }
  for (int k = 0; k < s->npairs; ++k) s->pair[k] = (float)(tmp[k] * 0.5 / sum); /* DC gain exactly 1 */
  return 0;
}

int iq_ddc_init(iq_ddc_t *d, double rate_in, int decim, double passband_hz, double atten_db)
{
  memset(d, 0, sizeof(*d));
  if (decim != 1 && decim != 2 && decim != 4) return -1;
  d->decim = decim; d->rate_in = rate_in;
  d->nstages = decim == 4 ? 2 : decim == 2 ? 1 : 0;
  double rate = rate_in;
  for (int s = 0; s < d->nstages; ++s, rate /= 2)
    if (stage_init(&d->st[s], passband_hz / rate, atten_db)) { iq_ddc_free(d); return -1; }
  d->rot = malloc(sizeof(float) * 2 * IQ_DDC_ROT_BLOCK);
  if (!d->rot) { iq_ddc_free(d); return -1; }
  iq_ddc_set_shift(d, 0);
  return 0;
}

void iq_ddc_free(iq_ddc_t *d)
{
  for (int s = 0; s < IQ_DDC_MAX_STAGES; ++s) { free(d->st[s].pair); free(d->st[s].hr); free(d->st[s].hi); }
  free(d->rot); free(d->br); free(d->bi); free(d->er); free(d->ei); free(d->orr); free(d->oi);
  memset(d, 0, sizeof(*d));
}

void iq_ddc_set_shift(iq_ddc_t *d, double shift_hz)
{
  d->shift_hz = shift_hz;
  const double step = -2 * M_PI * shift_hz / d->rate_in;
  for (int n = 0; n < IQ_DDC_ROT_BLOCK; ++n) { d->rot[2 * n] = (float)cos(step * n); d->rot[2 * n + 1] = (float)sin(step * n); }
}

void iq_ddc_reset(iq_ddc_t *d)
{
  d->phase = 0;
  for (int s = 0; s < d->nstages; ++s) {
    memset(d->st[s].hr, 0, sizeof(float) * (size_t)d->st[s].ntaps);
    memset(d->st[s].hi, 0, sizeof(float) * (size_t)d->st[s].ntaps);
  }
}

static int ensure(iq_ddc_t *d, int n)
{
  if (n <= d->cap) return 0;
  const size_t full = (size_t)n + 1024, half = full / 2 + 1;
  float **bufs[] = {&d->br, &d->bi, &d->er, &d->ei, &d->orr, &d->oi};
  for (int k = 0; k < 6; ++k) {
    free(*bufs[k]);
    *bufs[k] = malloc(sizeof(float) * (k < 2 ? full : half));
    if (!*bufs[k]) { d->cap = 0; return -1; }
  }
  d->cap = n;
  return 0;
}

/* in place on (xr, xi)[0..n): decimate by 2, result in (xr, xi)[0..n/2) */
static void stage_run(iq_ddc_t *d, iq_ddc_stage_t *s, float *restrict xr, float *restrict xi, int n)
{
  const int H = s->ntaps - 1, c = H / 2, T = H + n, m_out = n / 2;
  float *restrict er = d->er, *restrict ei = d->ei, *restrict orr = d->orr, *restrict oi = d->oi;
  /* concat [history | x] deinterleaved into even/odd phases */
  for (int t = 0; t < H; t += 2) { er[t / 2] = s->hr[t]; ei[t / 2] = s->hi[t]; orr[t / 2] = s->hr[t + 1]; oi[t / 2] = s->hi[t + 1]; }
  float *restrict er_n = er + H / 2, *restrict ei_n = ei + H / 2, *restrict or_n = orr + H / 2, *restrict oi_n = oi + H / 2;
  for (int q = 0; q < n / 2; ++q) { er_n[q] = xr[2 * q]; ei_n[q] = xi[2 * q]; or_n[q] = xr[2 * q + 1]; oi_n[q] = xi[2 * q + 1]; }
  /* new history = last H samples of the concat */
  for (int t = 0; t < H; t += 2) {
    const int q = (T - H + t) / 2;
    s->hr[t] = er[q]; s->hi[t] = ei[q]; s->hr[t + 1] = orr[q]; s->hi[t + 1] = oi[q];
  }
  const int co = (c - 1) / 2;
  /* 32 outputs at a time, accumulators held in registers across all taps (GCC vector extensions: SSE/AVX/NEON) */
  typedef float v8 __attribute__((vector_size(32)));
  int m = 0;
  for (; m + 32 <= m_out; m += 32) {
    v8 ar[4], ai[4], u, w;
    for (int q = 0; q < 4; ++q) {
      memcpy(&u, orr + m + co + 8 * q, 32); memcpy(&w, oi + m + co + 8 * q, 32);
      ar[q] = 0.5f * u; ai[q] = 0.5f * w;
    }
    for (int k = 0; k < s->npairs; ++k) {
      const float p = s->pair[k];
      const int j = 2 * k + 1, a = (c + j) / 2 + m, b = (c - j) / 2 + m;
      for (int q = 0; q < 4; ++q) {
        memcpy(&u, er + a + 8 * q, 32); memcpy(&w, er + b + 8 * q, 32); ar[q] += p * (u + w);
        memcpy(&u, ei + a + 8 * q, 32); memcpy(&w, ei + b + 8 * q, 32); ai[q] += p * (u + w);
      }
    }
    for (int q = 0; q < 4; ++q) { memcpy(xr + m + 8 * q, &ar[q], 32); memcpy(xi + m + 8 * q, &ai[q], 32); }
  }
  for (; m < m_out; ++m) {
    float r = 0.5f * orr[m + co], i = 0.5f * oi[m + co];
    for (int k = 0; k < s->npairs; ++k) {
      const int j = 2 * k + 1, a = (c + j) / 2 + m, b = (c - j) / 2 + m;
      r += s->pair[k] * (er[a] + er[b]); i += s->pair[k] * (ei[a] + ei[b]);
    }
    xr[m] = r; xi[m] = i;
  }
}

int iq_ddc_process(iq_ddc_t *d, const int16_t *in, int n_in, int16_t *out)
{
  if (n_in <= 0 || n_in % d->decim) return 0;
  if (d->decim == 1 && d->shift_hz == 0) { memcpy(out, in, (size_t)n_in * 4); return n_in; }
  if (ensure(d, n_in)) return 0;
  float *restrict xr = d->br, *restrict xi = d->bi;
  const double step = -2 * M_PI * d->shift_hz / d->rate_in;
  for (int n0 = 0; n0 < n_in; n0 += IQ_DDC_ROT_BLOCK) {
    const int len = n_in - n0 < IQ_DDC_ROT_BLOCK ? n_in - n0 : IQ_DDC_ROT_BLOCK;
    const float sr = (float)cos(d->phase), si = (float)sin(d->phase);
    const float *restrict rot = d->rot;
    const int16_t *restrict x = in + 2 * n0;
    for (int n = 0; n < len; ++n) {
      const float wr = sr * rot[2 * n] - si * rot[2 * n + 1], wi = sr * rot[2 * n + 1] + si * rot[2 * n];
      const float a = x[2 * n], b = x[2 * n + 1];
      xr[n0 + n] = a * wr - b * wi;
      xi[n0 + n] = a * wi + b * wr;
    }
    d->phase = remainder(d->phase + step * len, 2 * M_PI);
  }
  int n = n_in;
  for (int s = 0; s < d->nstages; ++s, n /= 2) stage_run(d, &d->st[s], xr, xi, n);
  uint64_t clipped = 0;
  const float R = 12582912.f; /* 1.5 * 2^23: x + R - R rounds to nearest even without a libm call, so the loop vectorises */
  for (int m = 0; m < n; ++m) {
    const float a = xr[m], b = xi[m];
    clipped += (a > 32767.f) + (a < -32768.f) + (b > 32767.f) + (b < -32768.f);
    const float ca = a > 32767.f ? 32767.f : a < -32768.f ? -32768.f : a;
    const float cb = b > 32767.f ? 32767.f : b < -32768.f ? -32768.f : b;
    out[2 * m] = (int16_t)(int32_t)((ca + R) - R);
    out[2 * m + 1] = (int16_t)(int32_t)((cb + R) - R);
  }
  d->clipped += clipped;
  return n;
}
