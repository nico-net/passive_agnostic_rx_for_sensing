/* SPDX-License-Identifier: LicenseRef-CSSL-1.0
 * GPU-DEMOD: independent scalar models (host + device) of the CPU demodulation primitives of nr_rx_pdsch()
 * (aarch64 build: SIMDe over NEON with x86 semantics; USE_128BIT is defined in nr_phy_common.c and
 * nr_channel_compensation.c). RULE: nothing in this header calls the CPU functions it models; the host test
 * (tests/nr_pdsch_gpu_demod_test.cc) compares these models with the real CPU functions, and the CUDA kernels
 * (nr_pdsch_gpu_demod.cu) call exactly these functions. Integer code only.
 * Compiled by nvcc (NR_DEMOD_HD = __host__ __device__) and by the host compilers (NR_DEMOD_HD empty). */
#ifndef NR_PDSCH_GPU_DEMOD_CUH
#define NR_PDSCH_GPU_DEMOD_CUH
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#ifdef __CUDACC__
#define NR_DEMOD_HD __host__ __device__
#else
#define NR_DEMOD_HD
#endif

struct nrd_c16 {
  int16_t r, i;
};

/* QAM amplitude constants, openair1/PHY/impl_defs_top.h */
#define NRD_QAM16_n1 20724
#define NRD_QAM64_n1 20225
#define NRD_QAM64_n2 10112
#define NRD_QAM256_n1 20106
#define NRD_QAM256_n2 10053
#define NRD_QAM256_n3 5026

NR_DEMOD_HD static inline int16_t nrd_sat16(int32_t v)
{
  return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}
NR_DEMOD_HD static inline int16_t nrd_adds(int16_t a, int16_t b)
{
  return nrd_sat16((int32_t)a + (int32_t)b);
}
NR_DEMOD_HD static inline int16_t nrd_subs(int16_t a, int16_t b)
{
  return nrd_sat16((int32_t)a - (int32_t)b);
}
/* _mm_madd_epi16 lane: a0*b0 + a1*b1, the int32 sum wraps (only (-32768)^2 * 2 does). */
NR_DEMOD_HD static inline int32_t nrd_madd(int16_t a0, int16_t b0, int16_t a1, int16_t b1)
{
  return (int32_t)((uint32_t)((int32_t)a0 * (int32_t)b0) + (uint32_t)((int32_t)a1 * (int32_t)b1));
}
/* _mm_srai_epi32: arithmetic shift, counts > 31 fill with the sign */
NR_DEMOD_HD static inline int32_t nrd_srai32(int32_t v, int s)
{
  return s > 31 ? (v >> 31) : (v >> s);
}
/* _mm_mulhrs_epi16 (NEON: vrshrn_n_s32(vmull_s16, 15)): (a*b + 0x4000) >> 15, truncated to 16 bit */
NR_DEMOD_HD static inline int16_t nrd_mulhrs(int16_t a, int16_t b)
{
  return (int16_t)(((int32_t)a * (int32_t)b + 0x4000) >> 15);
}
/* _mm_sign_epi16(a, -1): wrapping negation */
NR_DEMOD_HD static inline int16_t nrd_neg_wrap(int16_t a)
{
  return (int16_t)(uint16_t)(0u - (uint16_t)a);
}
/* oai_mm256_cpx_mult_conj(a, b, shift): conj(a) * b with madd, srai and packs_epi32 */
NR_DEMOD_HD static inline struct nrd_c16 nrd_cpx_mult_conj(struct nrd_c16 a, struct nrd_c16 b, int shift)
{
  struct nrd_c16 o;
  o.r = nrd_sat16(nrd_srai32(nrd_madd(a.r, b.r, a.i, b.i), shift));
  o.i = nrd_sat16(nrd_srai32(nrd_madd(nrd_neg_wrap(a.i), b.r, a.r, b.i), shift));
  return o;
}
/* nr_channel_compensation magnitude term of one RE: packs(smadd(h, h, shift)) */
NR_DEMOD_HD static inline int16_t nrd_mag16(struct nrd_c16 h, int shift)
{
  return nrd_sat16(nrd_srai32(nrd_madd(h.r, h.r, h.i, h.i), shift));
}

/* Model of nr_channel_compensation() (rho == NULL) for ONE (layer, re). rxF [nb_rx][len], chF_layer = this
 * layer's [nb_rx][len]; outputs point at THIS layer's row. Mags are written only when the CPU writes them. */
NR_DEMOD_HD static inline void nrd_comp_re(int nb_rx, uint32_t len, uint32_t re, int mod_order, int shift,
                                           const struct nrd_c16 *rxF, const struct nrd_c16 *chF_layer,
                                           struct nrd_c16 *rxComp, struct nrd_c16 *maga, struct nrd_c16 *magb,
                                           struct nrd_c16 *magc)
{
  const int16_t qa = mod_order == 4 ? NRD_QAM16_n1 : mod_order == 6 ? NRD_QAM64_n1 : mod_order == 8 ? NRD_QAM256_n1 : 0;
  const int16_t qb = mod_order == 6 ? NRD_QAM64_n2 : mod_order == 8 ? NRD_QAM256_n2 : 0;
  const int16_t qc = mod_order == 8 ? NRD_QAM256_n3 : 0;
  struct nrd_c16 acc = {0, 0};
  int16_t ma = 0, mb = 0, mc = 0;
  for (int a = 0; a < nb_rx; a++) {
    const struct nrd_c16 h = chF_layer[(size_t)a * len + re];
    const struct nrd_c16 c = nrd_cpx_mult_conj(h, rxF[(size_t)a * len + re], shift);
    if (a == 0) {
      acc = c;
    } else {
      acc.r = nrd_adds(acc.r, c.r);
      acc.i = nrd_adds(acc.i, c.i);
    }
    if (mod_order > 2) {
      const int16_t m = nrd_mag16(h, shift);
      const int16_t ta = nrd_mulhrs(m, qa), tb = nrd_mulhrs(m, qb), tc = nrd_mulhrs(m, qc);
      ma = a == 0 ? ta : nrd_adds(ma, ta);
      mb = a == 0 ? tb : nrd_adds(mb, tb);
      mc = a == 0 ? tc : nrd_adds(mc, tc);
    }
  }
  rxComp[re] = acc;
  if (mod_order > 2) {
    maga[re].r = maga[re].i = ma;
    if (mod_order > 4)
      magb[re].r = magb[re].i = mb;
    if (mod_order > 6)
      magc[re].r = magc[re].i = mc;
  }
}

/* protected_abs128: abs(adds(subs(x, 1), 1)), i.e. |x| with -32768 -> 32767 */
NR_DEMOD_HD static inline int16_t nrd_pabs(int16_t x)
{
  const int16_t t = nrd_adds(nrd_subs(x, 1), 1);
  return (int16_t)(t < 0 ? -t : t);
}
/* saturating_sub() of the scalar tails (nr_phy_common.c): clamp(a - |b|, -32767, 32767), |b| in int */
NR_DEMOD_HD static inline int16_t nrd_tail_sub(int16_t a, int16_t b)
{
  int32_t t = (int32_t)a - (b < 0 ? -(int32_t)b : (int32_t)b);
  t = t < 32767 ? t : 32767;
  return (int16_t)(t > -32767 ? t : -32767);
}

/* Model of nr_qpsk_llr / nr_16qam_llr / nr_64qam_llr / nr_256qam_llr (USE_128BIT) for ONE RE `re` of `nb_re`:
 * the last nb_re % 4 REs take the scalar tail. Writes llr[re*qm .. re*qm+qm-1] in the CPU's order. */
NR_DEMOD_HD static inline void nrd_llr_re_out(int qm, uint32_t re, uint32_t nb_re, const struct nrd_c16 *x,
                                              const struct nrd_c16 *ma, const struct nrd_c16 *mb, const struct nrd_c16 *mc,
                                              int16_t *o)
{
  const struct nrd_c16 v = x[re];
  if (qm == 2) {
    o[0] = (int16_t)(v.r >> 4);
    o[1] = (int16_t)(v.i >> 4);
    return;
  }
  const bool tail = re >= (nb_re & ~3u);
  o[0] = v.r;
  o[1] = v.i;
  if (!tail) {
    const struct nrd_c16 a = ma[re];
    const int16_t t1r = nrd_subs(a.r, nrd_pabs(v.r)), t1i = nrd_subs(a.i, nrd_pabs(v.i));
    o[2] = t1r;
    o[3] = t1i;
    if (qm == 4)
      return;
    const struct nrd_c16 b = mb[re];
    const int16_t t2r = nrd_subs(b.r, nrd_pabs(t1r)), t2i = nrd_subs(b.i, nrd_pabs(t1i));
    o[4] = t2r;
    o[5] = t2i;
    if (qm == 6)
      return;
    const struct nrd_c16 c = mc[re];
    o[6] = nrd_subs(c.r, nrd_pabs(t2r));
    o[7] = nrd_subs(c.i, nrd_pabs(t2i));
    return;
  }
  const struct nrd_c16 a = ma[re];
  const int16_t s1r = nrd_tail_sub(a.r, v.r), s1i = nrd_tail_sub(a.i, v.i);
  o[2] = s1r;
  o[3] = s1i;
  if (qm == 4)
    return;
  const struct nrd_c16 b = mb[re];
  const int16_t s2r = nrd_tail_sub(b.r, s1r), s2i = nrd_tail_sub(b.i, s1i);
  o[4] = s2r;
  o[5] = s2i;
  if (qm == 6)
    return;
  const struct nrd_c16 c = mc[re];
  o[6] = nrd_tail_sub(c.r, s2r);
  o[7] = nrd_tail_sub(c.i, s2i);
}
NR_DEMOD_HD static inline void nrd_llr_re(int qm, uint32_t re, uint32_t nb_re, const struct nrd_c16 *x,
                                          const struct nrd_c16 *ma, const struct nrd_c16 *mb, const struct nrd_c16 *mc,
                                          int16_t *llr)
{
  nrd_llr_re_out(qm, re, nb_re, x, ma, mb, mc, llr + (size_t)re * qm);
}

/* Model of nr_scale_channel() for one element: srai_epi16 per component (vector part and c16Shift tail agree). */
NR_DEMOD_HD static inline struct nrd_c16 nrd_scale(struct nrd_c16 v, int shift)
{
  const int s = shift > 15 ? 15 : shift;
  struct nrd_c16 o;
  o.r = (int16_t)(v.r >> s);
  o.i = (int16_t)(v.i >> s);
  return o;
}

/* simde_mm_average_sse lane term of a group of 4 REs: lane k accumulates (madd(h_k, h_k) >> shift) wrapping in
 * int32; the 4 lanes are then summed ZERO-extended (unsigned) into int64 and divided by `scale`. */
NR_DEMOD_HD static inline uint32_t nrd_level_term(struct nrd_c16 h, int shift)
{
  return (uint32_t)nrd_srai32(nrd_madd(h.r, h.r, h.i, h.i), shift);
}
NR_DEMOD_HD static inline int nrd_factor2(unsigned int x)
{
  int i;
  for (i = 0; i < 31; i++)
    if ((x & (1u << i)) != 0)
      break;
  return i;
}
NR_DEMOD_HD static inline int32_t nrd_level_finish(const uint32_t lane[4], uint32_t len)
{
  const int x = nrd_factor2(len);
  const int16_t y = (int16_t)(len >> x);
  const int64_t t = (int64_t)lane[0] + (int64_t)lane[1] + (int64_t)lane[2] + (int64_t)lane[3];
  return (int32_t)(uint32_t)(t / y);
}
/* Model of ONE (layer, antenna) row of nr_channel_level() (symbol 0): avg of ch[0..len) */
NR_DEMOD_HD static inline int32_t nrd_channel_level_row(const struct nrd_c16 *ch, uint32_t len)
{
  const int x = nrd_factor2(len);
  uint32_t lane[4] = {0, 0, 0, 0};
  for (uint32_t g = 0; g < (len >> 2); g++)
    for (int k = 0; k < 4; k++)
      lane[k] += nrd_level_term(ch[4 * g + k], x);
  return nrd_level_finish(lane, len);
}
/* nr_dlsch_channel_level_median() group term: sum over 4 SIGNED lanes of madd >> 2 */
NR_DEMOD_HD static inline int64_t nrd_median_term(const struct nrd_c16 *h4)
{
  int64_t s = 0;
  for (int k = 0; k < 4; k++)
    s += (int64_t)nrd_srai32(nrd_madd(h4[k].r, h4[k].r, h4[k].i, h4[k].i), 2);
  return s;
}

NR_DEMOD_HD static inline int nrd_clz32(uint32_t x)
{
  int n = 0;
  if (x == 0)
    return 32;
  while (!(x & 0x80000000u)) {
    x <<= 1;
    n++;
  }
  return n;
}
/* log2_approx() (TOOLS/log2_approx.c) */
NR_DEMOD_HD static inline int nrd_log2_approx(uint32_t x)
{
  const uint32_t round_val = 3037000499U;
  if (!x)
    return 0;
  int l2 = nrd_clz32(x);
  if (x > (round_val >> l2))
    l2 = 32 - l2;
  else
    l2 = 31 - l2;
  return l2;
}

/* ---- per-grant receive-branch decision and output shift: nr_rx_pdsch()'s first_symbol_flag block --------------
 * Supported subset (the host refuses the GPU path otherwise): no forced mask/branch, ISAC_AGNOSTIC_V2 off,
 * ISAC_RX_BEST_BY_SNR off, ISAC_L2MAXH_PEAK / _HEADROOM off, mode 0..3. thr_coef = pow(10, -min_db/10) computed by
 * the host libm exactly as the CPU does, so thr = thr_coef * avg[best] is the same IEEE product. */
struct nrd_decision {
  int mrc_rx_index, mrc_nb_rx, live_mask, zero_mask, log2_maxh, used_branch_set, used_branch;
};
NR_DEMOD_HD static inline void nrd_decide(int nl, int nbRx, int mode, double thr_coef, int l2_delta, int shift_sum,
                                          const int32_t *avg, const int32_t *median /* [nl*nbRx], the median rows when nl > 1 */,
                                          struct nrd_decision *d)
{
  int avgs = 0;
  for (int l = 0; l < nl; l++)
    for (int a = 0; a < nbRx; a++)
      avgs = avgs > avg[l * nbRx + a] ? avgs : avg[l * nbRx + a];
  d->mrc_nb_rx = nbRx;
  d->mrc_rx_index = 0;
  d->live_mask = 0xF;
  d->zero_mask = 0;
  d->used_branch_set = 0;
  d->used_branch = -1;
  if (nl == 1 && nbRx == 4) {
    int best = 0;
    for (int a = 1; a < nbRx; a++)
      if (avg[a] > avg[best])
        best = a;
    if (mode == 0) {
      d->mrc_nb_rx = 1;
      d->mrc_rx_index = 0;
      avgs = avg[0];
    } else if (mode == 1) {
      d->mrc_nb_rx = 1;
      d->mrc_rx_index = best;
      avgs = avg[best];
    } else if (mode == 2) {
      d->mrc_nb_rx = nbRx;
      d->live_mask = 0xF;
    } else {
      const double thr = thr_coef * (double)avg[best];
      int mask = 0;
      for (int a = 0; a < nbRx; a++)
        if ((double)avg[a] >= thr)
          mask |= 1 << a;
      if (mask == 0)
        mask = 1 << best;
      d->live_mask = mask;
      d->mrc_nb_rx = nbRx;
    }
    d->used_branch_set = 1;
    d->used_branch = d->mrc_nb_rx == 1 ? d->mrc_rx_index : -1;
  }
  if (nl > 1)
    for (int l = 0; l < nl; l++)
      for (int a = 0; a < nbRx; a++)
        avgs = avgs > median[l * nbRx + a] ? avgs : median[l * nbRx + a];
  if (nl == 1) {
    int contributing = d->mrc_nb_rx;
    if (nbRx == 4 && mode == 3) {
      contributing = 0;
      for (int a = 0; a < nbRx; a++)
        if (d->live_mask & (1 << a))
          contributing++;
      if (contributing < 1)
        contributing = 1;
      if (shift_sum && avgs > 0) { /* MRCBUG ISAC_RX_MRC_SHIFT_SUM: nr_mrc_shift_branches() (nr_mrc_weights.h), same arithmetic */
        long long s = 0;
        for (int a = 0; a < nbRx; a++)
          if (d->live_mask & (1 << a))
            s += avg[a];
        contributing = (int)((s + avgs / 2) / avgs);
        contributing = contributing < 1 ? 1 : contributing > nbRx ? nbRx : contributing;
      }
    }
    d->log2_maxh = (nrd_log2_approx((uint32_t)avgs) >> 1) + 1 + nrd_log2_approx((uint32_t)contributing);
  } else {
    d->log2_maxh = (nrd_log2_approx((uint32_t)avgs) >> 1) + nrd_log2_approx((uint32_t)(nbRx >> 1));
  }
  if (l2_delta != 0) {
    d->log2_maxh += l2_delta;
    if (d->log2_maxh < 0)
      d->log2_maxh = 0;
  }
  if (nl == 1 && nbRx == 4 && mode == 3 && d->live_mask != 0xF)
    d->zero_mask = (~d->live_mask) & 0xF;
}
#endif
