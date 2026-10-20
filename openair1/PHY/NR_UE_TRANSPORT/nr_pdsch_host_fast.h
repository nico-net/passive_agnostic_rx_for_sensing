#ifndef NR_PDSCH_HOST_FAST_H
#define NR_PDSCH_HOST_FAST_H
/* ISAC_PDSCH_HOST_FAST=1 (default off): host-side pre-LDPC work of a large first pass, bit-identical to the scalar originals.
 *  nrhf_unscramble : nr_codeword_unscrambling (negate llr[i] where bit i%32 of seq[i/32] is set), 32 LLRs per sequence word
 *  nrhf_deinterleave: nr_deinterleaving_ldpc (e[j*E/Qm + i] = f[i*Qm + j]) for Qm 2/4/6/8
 *  nrhf_rm_rx       : nr_rate_matching_ldpc_rx bit selection as vector adds over contiguous runs (int16 wrap like d[ind] += s)
 * aarch64 uses NEON; elsewhere the scalar loops (x86 keeps today's behaviour). */
#include <stdint.h>
#include <stddef.h>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

/* Processes ceil(size/32) full words, like the original (the buffer must be padded to a multiple of 32 LLRs). */
static inline void nrhf_unscramble(int16_t *llr, uint32_t size, const uint32_t *seq)
{
  const uint32_t words = (size + 31) / 32;
#if defined(__aarch64__)
  static const uint16_t bit[8] = {1, 2, 4, 8, 16, 32, 64, 128};
  const uint16x8_t bits = vld1q_u16(bit);
  for (uint32_t w = 0; w < words; w++) {
    const uint32_t s = seq[w];
    int16_t *p = llr + (size_t)w * 32;
    for (int q = 0; q < 4; q++) {
      const int16x8_t m = vreinterpretq_s16_u16(vtstq_u16(vdupq_n_u16((uint16_t)((s >> (8 * q)) & 0xff)), bits));
      vst1q_s16(p + 8 * q, vsubq_s16(veorq_s16(vld1q_s16(p + 8 * q), m), m));
    }
  }
#else
  for (uint32_t i = 0; i < words * 32; i++)
    if (seq[i / 32] & (1U << (i % 32)))
      llr[i] = (int16_t)-llr[i];
#endif
}

/* E % Qm == 0 and Qm in {2,4,6,8}; returns 0 otherwise without touching e (caller falls back). */
static inline int nrhf_deinterleave(uint32_t E, uint8_t Qm, int16_t *e, const int16_t *f)
{
  if ((Qm != 2 && Qm != 4 && Qm != 6 && Qm != 8) || E % Qm)
    return 0;
  const uint32_t n = E / Qm;
  uint32_t i = 0;
#if defined(__aarch64__)
  if (Qm == 2) {
    for (; i + 8 <= n; i += 8) {
      const int16x8x2_t v = vld2q_s16(f + 2 * (size_t)i);
      vst1q_s16(e + i, v.val[0]);
      vst1q_s16(e + n + i, v.val[1]);
    }
  } else if (Qm == 4) {
    for (; i + 8 <= n; i += 8) {
      const int16x8x4_t v = vld4q_s16(f + 4 * (size_t)i);
      for (int j = 0; j < 4; j++)
        vst1q_s16(e + (size_t)j * n + i, v.val[j]);
    }
  } else if (Qm == 6) {
    for (; i + 4 <= n; i += 4) {
      const int16x8x3_t v = vld3q_s16(f + 6 * (size_t)i);
      for (int j = 0; j < 3; j++) {
        vst1_s16(e + (size_t)j * n + i, vget_low_s16(vuzp1q_s16(v.val[j], v.val[j])));
        vst1_s16(e + (size_t)(j + 3) * n + i, vget_low_s16(vuzp2q_s16(v.val[j], v.val[j])));
      }
    }
  } else {
    for (; i + 4 <= n; i += 4) {
      const int16x8x4_t v = vld4q_s16(f + 8 * (size_t)i);
      for (int j = 0; j < 4; j++) {
        vst1_s16(e + (size_t)j * n + i, vget_low_s16(vuzp1q_s16(v.val[j], v.val[j])));
        vst1_s16(e + (size_t)(j + 4) * n + i, vget_low_s16(vuzp2q_s16(v.val[j], v.val[j])));
      }
    }
  }
#endif
  for (; i < n; i++)
    for (uint32_t j = 0; j < Qm; j++)
      e[(size_t)j * n + i] = f[(size_t)i * Qm + j];
  return 1;
}

static inline void nrhf_add_run(int16_t *d, const int16_t *s, uint32_t n)
{
  uint32_t i = 0;
#if defined(__aarch64__)
  for (; i + 8 <= n; i += 8)
    vst1q_s16(d + i, vaddq_s16(vld1q_s16(d + i), vld1q_s16(s + i)));
#endif
  for (; i < n; i++)
    d[i] = (int16_t)(d[i] + s[i]);
}

/* The accumulate part of nr_rate_matching_ldpc_rx (after parameter checks and the clear): start index ind (= k0), circular
 * buffer [0, Ncb) without the filler [Foffset, Foffset + F), E soft inputs. Same visiting order as the original loops. */
static inline void nrhf_rm_rx(int16_t *d, const int16_t *soft, uint32_t E, uint32_t ind, uint32_t Ncb, uint32_t Foffset, uint32_t F)
{
  uint32_t k = 0, n;
  if (ind < Foffset) {
    n = Foffset - ind < E ? Foffset - ind : E;
    nrhf_add_run(d + ind, soft, n);
    ind += n;
    k += n;
  }
  if (ind >= Foffset && ind < Foffset + F)
    ind = Foffset + F;
  if (ind < Ncb && k < E) {
    n = Ncb - ind < E - k ? Ncb - ind : E - k;
    nrhf_add_run(d + ind, soft + k, n);
    k += n;
  }
  while (k < E) {
    const uint32_t k0 = k;
    n = Foffset < E - k ? Foffset : E - k;
    nrhf_add_run(d, soft + k, n);
    k += n;
    if (k < E && Ncb > Foffset + F) {
      n = Ncb - (Foffset + F) < E - k ? Ncb - (Foffset + F) : E - k;
      nrhf_add_run(d + Foffset + F, soft + k, n);
      k += n;
    }
    if (k == k0)
      break; /* the original would spin forever here (Ncb <= Foffset + F and Foffset == 0): never reached with valid parameters */
  }
}

/* l[0, Kc*Z) of prep_segment(): the int8 decoder input. z[i] = d[i-2Z] except z[0,2Z) = 0 and z[Kprime,K) = 127 (saturated). */
static inline void nrhf_pack(int8_t *l, const int16_t *d, int Z, int K, int F, int Kprime, int KcZ)
{
  (void)F;
  for (int i = 0; i < 2 * Z; i++)
    l[i] = 0;
  int i = 2 * Z;
#if defined(__aarch64__)
  for (; i + 16 <= KcZ; i += 16)
    vst1q_s8(l + i, vcombine_s8(vqmovn_s16(vld1q_s16(d + i - 2 * Z)), vqmovn_s16(vld1q_s16(d + i - 2 * Z + 8))));
#endif
  for (; i < KcZ; i++) {
    const int16_t v = d[i - 2 * Z];
    l[i] = v > 127 ? 127 : (v < -128 ? -128 : (int8_t)v);
  }
  for (int j = Kprime; j < K; j++) /* overwrite the filler region last (the vector loop packed d there) */
    l[j] = 127;
}
#endif
