/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* MPCI M1/C2 per-cell DDC, see iq_ddc.h. Halfband y[m] = 0.5 x[2m+c] + sum_k p_k (x[2m+c+j_k] + x[2m+c-j_k]), j_k odd:
 * the centre tap reads the odd input phase, every pair reads the even phase, so after one deinterleave each tap is a
 * contiguous axpy over m (vectorises). */
#include "iq_ddc.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
/* Paths. The portable path is plain C plus GCC/Clang generic vector extensions (the halfband accumulators), so it builds and runs on x86 (SSE/AVX through the
 * compiler) and on any other target. The NEON path only replaces the final float -> sc16 quantiser (round-to-nearest-even + saturate, the same values as the scalar
 * loop) on AArch64: vcvtnq_s32_f32 / vaddvq_u32 are A64 intrinsics, so 32-bit ARM NEON takes the portable path too. iq_ddc_set_force_scalar() is a test seam (a function, not a flag) that routes the quantiser through the portable loop so test_iq_ddc_equiv compares both paths in one binary. The old code had no x86 SIMD path. */
#if defined(__aarch64__) /* not `|| __ARM_NEON`: 32-bit NEON lacks vcvtnq_s32_f32 and vaddvq_u32 */
#define IQ_DDC_NEON 1
#include <arm_neon.h>
#else
#define IQ_DDC_NEON 0
#endif
static int g_force_scalar;
void iq_ddc_set_force_scalar(int on) { g_force_scalar = on; }
int iq_ddc_neon_compiled(void) { return IQ_DDC_NEON; }

double iq_ddc_cell_passband(int num_rb_dl, double scs_hz, double rate, double rx_bw)
{
  const double occ = num_rb_dl > 0 ? num_rb_dl * 12 * scs_hz : 0, lim = 0.95 * rate;
  return 0.5 * (rx_bw > 0 && rx_bw < lim ? rx_bw : occ > 0 && occ < lim ? occ : lim);
}

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
  if (decim < 1 || decim > 16 || (decim & (decim - 1))) return -1;
  d->decim = decim; d->rate_in = rate_in;
  d->nstages = __builtin_ctz((unsigned)decim);
  double rate = rate_in;
  for (int s = 0; s < d->nstages; ++s, rate /= 2)
    if (stage_init(&d->st[s], passband_hz / rate, atten_db)) { iq_ddc_free(d); return -1; }
  d->rot = malloc(sizeof(float) * 2 * IQ_DDC_ROT_BLOCK);
  d->rot4 = malloc(sizeof(float) * 2 * IQ_DDC_ROT_BLOCK);
  if (!d->rot || !d->rot4) { iq_ddc_free(d); return -1; }
  iq_ddc_set_shift(d, 0);
  return 0;
}

void iq_ddc_free(iq_ddc_t *d)
{
  for (int s = 0; s < IQ_DDC_MAX_STAGES; ++s) { free(d->st[s].pair); free(d->st[s].hr); free(d->st[s].hi); }
  free(d->rot); free(d->rot4); free(d->br); free(d->bi); free(d->er); free(d->ei); free(d->orr); free(d->oi);
  memset(d, 0, sizeof(*d));
}

void iq_ddc_set_shift(iq_ddc_t *d, double shift_hz)
{
  d->shift_hz = shift_hz;
  const double step = -2 * M_PI * shift_hz / d->rate_in;
  const int hb = IQ_DDC_ROT_BLOCK / 2;
  for (int n = 0; n < IQ_DDC_ROT_BLOCK; ++n) {
    const float cr = (float)cos(step * n), ci = (float)sin(step * n);
    d->rot[2 * n] = cr; d->rot[2 * n + 1] = ci;
    d->rot4[(n & 1) * 2 * hb + n / 2] = cr; d->rot4[(n & 1) * 2 * hb + hb + n / 2] = ci;
  }
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
/* fused: the mixer already wrote the even/odd phases of the block at er/ei/orr/oi + H/2 (stage 0), skip the deinterleave of xr/xi */
static void stage_run(iq_ddc_t *d, iq_ddc_stage_t *s, float *restrict xr, float *restrict xi, int n, int fused)
{
  const int H = s->ntaps - 1, c = H / 2, T = H + n, m_out = n / 2;
  float *restrict er = d->er, *restrict ei = d->ei, *restrict orr = d->orr, *restrict oi = d->oi;
  /* concat [history | x] deinterleaved into even/odd phases */
  for (int t = 0; t < H; t += 2) { er[t / 2] = s->hr[t]; ei[t / 2] = s->hi[t]; orr[t / 2] = s->hr[t + 1]; oi[t / 2] = s->hi[t + 1]; }
  float *restrict er_n = er + H / 2, *restrict ei_n = ei + H / 2, *restrict or_n = orr + H / 2, *restrict oi_n = oi + H / 2;
  if (!fused)
  for (int q = 0; q < n / 2; ++q) { er_n[q] = xr[2 * q]; ei_n[q] = xi[2 * q]; or_n[q] = xr[2 * q + 1]; oi_n[q] = xi[2 * q + 1]; }
  /* new history = last H samples of the concat */
  for (int t = 0; t < H; t += 2) {
    const int q = (T - H + t) / 2;
    s->hr[t] = er[q]; s->hi[t] = ei[q]; s->hr[t + 1] = orr[q]; s->hi[t + 1] = oi[q];
  }
  const int co = (c - 1) / 2;
  /* 32 outputs at a time, accumulators held in registers across all taps (GCC vector extensions). 16-byte vectors: 32-byte generic vectors
   spilled on NEON (stages 3.5x slower on GB10) */
  typedef float v8 __attribute__((vector_size(16)));
  int m = 0;
  for (; m + 32 <= m_out; m += 32) {
    v8 ar[8], ai[8], u, w;
    for (int q = 0; q < 8; ++q) {
      memcpy(&u, orr + m + co + 4 * q, 16); memcpy(&w, oi + m + co + 4 * q, 16);
      ar[q] = 0.5f * u; ai[q] = 0.5f * w;
    }
    for (int k = 0; k < s->npairs; ++k) {
      const float p = s->pair[k];
      const int j = 2 * k + 1, a = (c + j) / 2 + m, b = (c - j) / 2 + m;
      for (int q = 0; q < 8; ++q) {
        memcpy(&u, er + a + 4 * q, 16); memcpy(&w, er + b + 4 * q, 16); ar[q] += p * (u + w);
        memcpy(&u, ei + a + 4 * q, 16); memcpy(&w, ei + b + 4 * q, 16); ai[q] += p * (u + w);
      }
    }
    for (int q = 0; q < 8; ++q) { memcpy(xr + m + 4 * q, &ar[q], 16); memcpy(xi + m + 4 * q, &ai[q], 16); }
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

/* decim >= 2: the mixer writes the even/odd sample phases of stage 0 directly (no float scratch round trip, no deinterleave pass) */
static void mix_split(iq_ddc_t *d, const int16_t *in, int n_in, float *restrict er_n, float *restrict ei_n, float *restrict or_n, float *restrict oi_n)
{
  const double step = -2 * M_PI * d->shift_hz / d->rate_in;
  const int hb = IQ_DDC_ROT_BLOCK / 2;
  const float *restrict Re = d->rot4, *restrict Ie = d->rot4 + hb, *restrict Ro = d->rot4 + 2 * hb, *restrict Io = d->rot4 + 3 * hb;
  for (int n0 = 0; n0 < n_in; n0 += IQ_DDC_ROT_BLOCK) {
    const int len = n_in - n0 < IQ_DDC_ROT_BLOCK ? n_in - n0 : IQ_DDC_ROT_BLOCK;
    const float sr = (float)cos(d->phase), si = (float)sin(d->phase);
    const int16_t *restrict x = in + 2 * n0;
    float *restrict oer = er_n + n0 / 2, *restrict oei = ei_n + n0 / 2, *restrict oor = or_n + n0 / 2, *restrict ooi = oi_n + n0 / 2;
    for (int q = 0; q < len / 2; ++q) {
      const float wer = sr * Re[q] - si * Ie[q], wei = sr * Ie[q] + si * Re[q];
      const float wor = sr * Ro[q] - si * Io[q], woi = sr * Io[q] + si * Ro[q];
      const float a0 = x[4 * q], b0 = x[4 * q + 1], a1 = x[4 * q + 2], b1 = x[4 * q + 3];
      oer[q] = a0 * wer - b0 * wei; oei[q] = a0 * wei + b0 * wer;
      oor[q] = a1 * wor - b1 * woi; ooi[q] = a1 * woi + b1 * wor;
    }
    d->phase = remainder(d->phase + step * len, 2 * M_PI);
  }
}

/* decim 1: mixer straight to sc16 (split mixer per block, then rounded and re-interleaved) */
static void mix_only(iq_ddc_t *d, const int16_t *in, int n_in, int16_t *out)
{
  float t[4][IQ_DDC_ROT_BLOCK / 2];
  const float R = 12582912.f;
  uint64_t clipped = 0;
  for (int n0 = 0; n0 < n_in; n0 += IQ_DDC_ROT_BLOCK) {
    const int len = n_in - n0 < IQ_DDC_ROT_BLOCK ? n_in - n0 : IQ_DDC_ROT_BLOCK;
    mix_split(d, in + 2 * n0, len, t[0], t[1], t[2], t[3]);
    int16_t *restrict y = out + 2 * n0;
    int q = 0;
#if IQ_DDC_NEON
    if (!g_force_scalar) { /* round-to-nearest-even + saturate, same values as the scalar path; vst4 re-interleaves even/odd re/im */
      const float32x4_t hi = vdupq_n_f32(32767.f), lo = vdupq_n_f32(-32768.f);
      uint32x4_t cnt = vdupq_n_u32(0);
      for (; q + 4 <= len / 2; q += 4) {
        int16x4x4_t r;
        float32x4_t v;
        v = vld1q_f32(t[0] + q); cnt = vsubq_u32(cnt, vorrq_u32(vcgtq_f32(v, hi), vcltq_f32(v, lo))); r.val[0] = vqmovn_s32(vcvtnq_s32_f32(v));
        v = vld1q_f32(t[1] + q); cnt = vsubq_u32(cnt, vorrq_u32(vcgtq_f32(v, hi), vcltq_f32(v, lo))); r.val[1] = vqmovn_s32(vcvtnq_s32_f32(v));
        v = vld1q_f32(t[2] + q); cnt = vsubq_u32(cnt, vorrq_u32(vcgtq_f32(v, hi), vcltq_f32(v, lo))); r.val[2] = vqmovn_s32(vcvtnq_s32_f32(v));
        v = vld1q_f32(t[3] + q); cnt = vsubq_u32(cnt, vorrq_u32(vcgtq_f32(v, hi), vcltq_f32(v, lo))); r.val[3] = vqmovn_s32(vcvtnq_s32_f32(v));
        vst4_s16(y + 4 * q, r);
      }
      clipped += vaddvq_u32(cnt);
    }
#endif
    for (; q < len / 2; ++q)
      for (int k = 0; k < 4; ++k) {
        const float v = t[k][q];
        clipped += (v > 32767.f) + (v < -32768.f);
        const float cv = v > 32767.f ? 32767.f : v < -32768.f ? -32768.f : v;
        y[4 * q + k] = (int16_t)(int32_t)((cv + R) - R);
      }
  }
  d->clipped += clipped;
}

int iq_ddc_process(iq_ddc_t *d, const int16_t *in, int n_in, int16_t *out)
{
  if (n_in <= 0 || n_in % d->decim) return 0;
  if (d->decim == 1) {
    if (d->shift_hz == 0) memcpy(out, in, (size_t)n_in * 4); else mix_only(d, in, n_in, out);
    return n_in;
  }
  if (ensure(d, n_in)) return 0;
  float *restrict xr = d->br, *restrict xi = d->bi;
  const int H0 = d->st[0].ntaps - 1;
  mix_split(d, in, n_in, d->er + H0 / 2, d->ei + H0 / 2, d->orr + H0 / 2, d->oi + H0 / 2);
  int n = n_in;
  for (int s = 0; s < d->nstages; ++s, n /= 2) stage_run(d, &d->st[s], xr, xi, n, s == 0);
  uint64_t clipped = 0;
  const float R = 12582912.f; /* 1.5 * 2^23: x + R - R rounds to nearest even without a libm call, so the loop vectorises */
  int m = 0;
#if IQ_DDC_NEON
  if (!g_force_scalar) { /* same rounding/saturation as the scalar loop below */
    const float32x4_t hi = vdupq_n_f32(32767.f), lo = vdupq_n_f32(-32768.f);
    uint32x4_t cnt = vdupq_n_u32(0);
    for (; m + 4 <= n; m += 4) {
      int16x4x2_t r;
      float32x4_t v = vld1q_f32(xr + m), w = vld1q_f32(xi + m);
      cnt = vsubq_u32(cnt, vorrq_u32(vcgtq_f32(v, hi), vcltq_f32(v, lo)));
      cnt = vsubq_u32(cnt, vorrq_u32(vcgtq_f32(w, hi), vcltq_f32(w, lo)));
      r.val[0] = vqmovn_s32(vcvtnq_s32_f32(v)); r.val[1] = vqmovn_s32(vcvtnq_s32_f32(w));
      vst2_s16(out + 2 * m, r);
    }
    clipped += vaddvq_u32(cnt);
  }
#endif
  for (; m < n; ++m) {
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
