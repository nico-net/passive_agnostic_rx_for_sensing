/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Sampling-frequency-offset (SFO) correction of one OFDM symbol in the frequency domain: the per-RE arithmetic shared bit for bit by
 * every FEP that applies it (CPU nr_slot_fep*, UL passive FEP, the GPU exact FEP kernels). Pure integer, header-only, C and CUDA.
 *
 * Physics and sign (see nr_sfo_state.h): with the receiver sample clock fast by eps (it takes (1 + eps) fs samples per second of the
 * transmitter), symbol l of a slot, whose FFT window starts n_l samples after the window of symbol 0, sees the signal delayed by
 * eps * n_l samples, i.e. subcarrier k (signed, DC = 0) rotated by exp(-j 2 pi k eps n_l / N). The correction multiplies subcarrier k
 * by exp(+j k dphi) with dphi = 2 pi eps n_l / N, carried as an unsigned 32-bit phase step (2^32 = one turn): nr_sfo_dphi().
 *
 * Bit-exactness: the phasor of subcarrier k is the Q15 product of two integer sin/cos values, exp(j 64 h dphi) * exp(j b dphi) with
 * k = 64 h + b (b = k & 63, h = floor(k / 64)); every operation is an integer add / multiply / shift / division by a constant, so the CPU
 * (tables per symbol) and the GPU (per RE) produce identical int16 output. */
#ifndef NR_SFO_ROT_H
#define NR_SFO_ROT_H
#include <stdint.h>

#if defined(__CUDACC__)
#define NR_SFO_HD static __host__ __device__ __forceinline__
#else
#define NR_SFO_HD static inline
#endif

typedef struct {
  int16_t r, i;
} nr_sfo_c16_t;

NR_SFO_HD int16_t nr_sfo_sat16(int32_t v)
{
  return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

/* cos and sin of the angle ph * 2 pi / 2^32 in Q15 (saturated at +32767). Quadrant fold, then Taylor series to x^9 / x^8 on
 * [0, pi/4] in Q30 (truncation error < 2e-9, i.e. far below the Q15 output step). */
NR_SFO_HD nr_sfo_c16_t nr_sfo_sincos(uint32_t ph)
{
  const uint32_t q = ph >> 30;
  uint32_t r = ph & 0x3FFFFFFFu; /* [0, pi/2) */
  const int fold = r >= (1u << 29);
  if (fold)
    r = (1u << 30) - r; /* pi/2 - angle, in [0, pi/4] */
  const int64_t one = (int64_t)1 << 30;
  const int64_t x = ((int64_t)r * 1686629713) >> 30; /* radians in Q30: r * (pi/2 * 2^30) / 2^30 */
  const int64_t x2 = (x * x) >> 30;
  int64_t s = one - x2 / 72;
  s = one - ((x2 * s) >> 30) / 42;
  s = one - ((x2 * s) >> 30) / 20;
  s = one - ((x2 * s) >> 30) / 6;
  s = (x * s) >> 30;
  int64_t c = one - x2 / 56;
  c = one - ((x2 * c) >> 30) / 30;
  c = one - ((x2 * c) >> 30) / 12;
  c = one - ((x2 * c) >> 30) / 2;
  int64_t cs = fold ? s : c, sn = fold ? c : s; /* cos, sin of the quadrant-reduced angle */
  int64_t co, si;
  switch (q) {
    case 0: co = cs; si = sn; break;
    case 1: co = -sn; si = cs; break;
    case 2: co = -cs; si = -sn; break;
    default: co = sn; si = -cs; break;
  }
  nr_sfo_c16_t out;
  out.r = nr_sfo_sat16((int32_t)((co + (1 << 14)) >> 15));
  out.i = nr_sfo_sat16((int32_t)((si + (1 << 14)) >> 15));
  return out;
}

/* Q15 complex product a * b, rounded, saturated. */
NR_SFO_HD nr_sfo_c16_t nr_sfo_cmul(nr_sfo_c16_t a, nr_sfo_c16_t b)
{
  nr_sfo_c16_t o;
  o.r = nr_sfo_sat16(((int32_t)a.r * b.r - (int32_t)a.i * b.i + (1 << 14)) >> 15);
  o.i = nr_sfo_sat16(((int32_t)a.r * b.i + (int32_t)a.i * b.r + (1 << 14)) >> 15);
  return o;
}

/* Phasor of signed subcarrier k (DC = 0) for the step dphi: exp(j 64 h dphi) * exp(j b dphi), k = 64 h + b. */
NR_SFO_HD nr_sfo_c16_t nr_sfo_phasor(int k, uint32_t dphi)
{
  const int h = k >= 0 ? k / 64 : -((63 - k) / 64); /* floor(k / 64) */
  const int b = k - 64 * h;
  return nr_sfo_cmul(nr_sfo_sincos((uint32_t)(64 * h) * dphi), nr_sfo_sincos((uint32_t)b * dphi));
}

/* One RE: v * phasor(k). */
NR_SFO_HD nr_sfo_c16_t nr_sfo_re(nr_sfo_c16_t v, int k, uint32_t dphi)
{
  return nr_sfo_cmul(v, nr_sfo_phasor(k, dphi));
}

/* Signed subcarrier index of FFT bin n (0..N-1): bins >= N/2 are negative frequencies. */
NR_SFO_HD int nr_sfo_bin_k(int n, int N)
{
  return n < N / 2 ? n : n - N;
}

#endif
