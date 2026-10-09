/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Bit-exact CUDA port of the CPU receive FEP on aarch64 (GPU6, 2026-10-10).
 *
 * WHY: every downstream CPU stage (fixed-point pilot LS, IDFT delay peak, FIR, branch gate, MRC/ALIGN,
 * MMSE mode 2, LLR) is sensitive to the last bit of rxdataF -- CHEST-FAST measured that a float delay
 * IDFT alone changes the chosen delay on 297/368 inputs and one CRC in 123 grants. A GPU front end can
 * therefore be CPU-equivalent only if it hands the CPU chain IDENTICAL rxdataF. This file reproduces,
 * lane for lane, what nr_slot_fep_ant() computes on the DGX:
 *   - dft4096() of openair1/PHY/TOOLS/oai_dfts_neon.c (radix-4 DIT: dft16 -> bfly4_16 x2 -> bfly4 x2,
 *     NEON saturating int16 adds, vqshrn #15 packs, vshr scale shifts 3/1/1/1), and
 *   - apply_nr_rotation_symbol_RX(): rotate_cpx_vector(.., 15) (ARMv8.1 QRDMX path: vqdmulh + vqrdmlsh /
 *     vqrdmlah) by conj(symbol_rotation) then mult_cpx_vector(.., 15) by timeshift_symbol_rotation, on the
 *     occupied subcarriers only (other subcarriers keep the raw DFT output, as on the CPU).
 * FO compensation is NOT reproduced: the caller must not use this path when the CPU FEP would compensate
 * (cont_fo_comp or a per-branch FO); nr_pdsch_gpu_exact_fep() refuses fo != 0.
 * Validated bit for bit against CPU rxdataF recorded by the receiver (nr_gpu_slot_bench --exact). */
#ifndef NR_PDSCH_GPU_EXACT_FEP_CUH
#define NR_PDSCH_GPU_EXACT_FEP_CUH
#include "nr_pdsch_gpu_exact_tw.h"
#include <climits>

namespace nr_exact {
static const int N = 4096, THREADS = 256;

__device__ __forceinline__ short sat16(int x) { return (short)max(-32768, min(32767, x)); }
__device__ __forceinline__ int sat32(long long x) { return (int)max((long long)INT_MIN, min((long long)INT_MAX, x)); }
__device__ __forceinline__ short2 qadd(short2 a, short2 b) { return make_short2(sat16(a.x + b.x), sat16(a.y + b.y)); }
__device__ __forceinline__ short2 qsub(short2 a, short2 b) { return make_short2(sat16(a.x - b.x), sat16(a.y - b.y)); }
__device__ __forceinline__ short2 shr(short2 a, int s) { return make_short2((short)(a.x >> s), (short)(a.y >> s)); }
/* vrev32q_s16(vmulq_s16(x, {-1,1})): (re, im) -> (im, -re), int16 wrap */
__device__ __forceinline__ short2 flip(short2 a) { return make_short2(a.y, (short)(-(int)a.x)); }
/* vpadd_s32 of two vmull_s16 products: non-saturating int32 sum (wraps only for 2 x (-32768)^2) */
__device__ __forceinline__ int padd(int p, int q) { return (int)((unsigned)p + (unsigned)q); }
/* cpack: vqshrn_n_s32(x, 15) */
__device__ __forceinline__ short pk(int v) { return sat16(v >> 15); }
/* packed_cmult2(a, b, b2): re = a.r b[0] + a.i b[1], im = a.r b2[0] + a.i b2[1] */
__device__ __forceinline__ short2 pcm2(short2 a, short2 b, short2 b2)
{
  return make_short2(pk(padd(a.x * b.x, a.y * b.y)), pk(padd(a.x * b2.x, a.y * b2.y)));
}
/* cmult(a, b) of bfly4: bconj = b * {1,-1} (int16 wrap), bflip = (b.i, b.r) */
__device__ __forceinline__ void cmult(short2 a, short2 b, int &re, int &im)
{
  const short bi = (short)(-(int)b.y);
  re = padd(a.x * b.x, a.y * bi);
  im = padd(a.x * b.y, a.y * b.x);
}
__device__ __forceinline__ int qa(int a, int b) { return sat32((long long)a + b); }
__device__ __forceinline__ int qs(int a, int b) { return sat32((long long)a - b); }

/* radix-4 kernel with 16-bit twiddled inputs (bfly4_16 and the second dft16 stage) */
__device__ __forceinline__ void r4_16(short2 x0, short2 x1t, short2 x2t, short2 x3t, short2 &y0, short2 &y1, short2 &y2,
                                      short2 &y3)
{
  short2 x02 = qadd(x0, x2t), x13 = qadd(x1t, x3t);
  y0 = qadd(x02, x13);
  y2 = qsub(x02, x13);
  x02 = qsub(x0, x2t);
  x13 = qsub(flip(x1t), flip(x3t));
  y1 = qadd(x02, x13);
  y3 = qsub(x02, x13);
}

/* bfly4 (32-bit accumulation) */
__device__ __forceinline__ void bfly4(short2 x0, short2 x1, short2 x2, short2 x3, short2 t1, short2 t2, short2 t3, short2 &y0,
                                      short2 &y1, short2 &y2, short2 &y3)
{
  int x1r, x1i, x2r, x2i, x3r, x3i;
  cmult(x1, t1, x1r, x1i);
  cmult(x2, t2, x2r, x2i);
  cmult(x3, t3, x3r, x3i);
  y0 = qadd(x0, make_short2(pk(qa(x1r, qa(x2r, x3r))), pk(qa(x1i, qa(x2i, x3i)))));
  y1 = qadd(x0, make_short2(pk(qs(x1i, qa(x2r, x3i))), pk(qs(qs(x3r, x2i), x1r))));
  y2 = qadd(x0, make_short2(pk(qs(qs(x2r, x3r), x1r)), pk(qs(qs(x2i, x3i), x1i))));
  y3 = qadd(x0, make_short2(pk(qs(qs(x3i, x2r), x1i)), pk(qs(x1r, qa(x2i, x3r)))));
}

__device__ __forceinline__ short2 twc(const short *t, int k) { return make_short2(t[2 * k], t[2 * k + 1]); }

/* dft16 of oai_dfts_neon.c on 16 consecutive complex values */
__device__ __forceinline__ void dft16(short2 *s)
{
  short2 x[16], t[16];
#pragma unroll
  for (int n = 0; n < 16; n++) x[n] = s[n];
#pragma unroll
  for (int k = 0; k < 4; k++) /* first stage, lane k of the 4 input vectors */
    r4_16(x[k], x[4 + k], x[8 + k], x[12 + k], t[k], t[4 + k], t[8 + k], t[12 + k]);
#pragma unroll
  for (int k = 0; k < 4; k++) { /* after the 4x4 transpose, ytmp_l lane k = t[4k + l] */
    const short2 y1 = pcm2(t[4 * k + 1], twc(g_ex_tw16a, k), twc(g_ex_tw16b, k));
    const short2 y2 = pcm2(t[4 * k + 2], twc(g_ex_tw16a, 4 + k), twc(g_ex_tw16b, 4 + k));
    const short2 y3 = pcm2(t[4 * k + 3], twc(g_ex_tw16a, 8 + k), twc(g_ex_tw16b, 8 + k));
    r4_16(t[4 * k], y1, y2, y3, s[k], s[4 + k], s[8 + k], s[12 + k]);
  }
}

/* One block per (symbol, antenna). perm: input index of each in-place DIT position (the composed
 * transpose16_ooff permutations of dft4096/1024/256/64). Writes rxdataF exactly as the CPU does. */
__global__ void __launch_bounds__(THREADS) k_fep_exact(const short2 *__restrict__ rx, int slot_len, int ncp, int ncp0,
                                                       const uint16_t *__restrict__ sym_list, const uint16_t *__restrict__ perm,
                                                       const short *__restrict__ tw1024, const short *__restrict__ tw4096,
                                                       const short2 *__restrict__ rot, const short2 *__restrict__ ts,
                                                       int nb_rb, int fco, short2 *__restrict__ out)
{
  __shared__ short2 s[N];
  const int sym = sym_list[blockIdx.x], ant = blockIdx.y, t = threadIdx.x;
  const short2 *in = rx + (size_t)ant * (slot_len + N) + ncp0 + sym * (N + ncp);
  for (int n = t; n < N; n += THREADS) s[n] = in[__ldg(&perm[n])];
  __syncthreads();
  dft16(&s[16 * t]);
  __syncthreads();
  /* bfly4_16 levels: 64 (scale >> 3) and 256 (>> 1) */
#pragma unroll
  for (int lv = 0; lv < 2; lv++) {
    const int sub = lv ? 64 : 16, sh = lv ? 1 : 3;
    const short *ta = lv ? g_ex_tw256a : g_ex_tw64a, *tb = lv ? g_ex_tw256b : g_ex_tw64b;
    for (int b = t; b < N / 4; b += THREADS) {
      const int p = b % sub, base = (b / sub) * 4 * sub + p;
      short2 y0, y1, y2, y3;
      const short2 x1t = pcm2(s[base + sub], twc(ta, p), twc(tb, p));
      const short2 x2t = pcm2(s[base + 2 * sub], twc(ta, sub + p), twc(tb, sub + p));
      const short2 x3t = pcm2(s[base + 3 * sub], twc(ta, 2 * sub + p), twc(tb, 2 * sub + p));
      r4_16(s[base], x1t, x2t, x3t, y0, y1, y2, y3);
      s[base] = shr(y0, sh);
      s[base + sub] = shr(y1, sh);
      s[base + 2 * sub] = shr(y2, sh);
      s[base + 3 * sub] = shr(y3, sh);
    }
    __syncthreads();
  }
  /* bfly4 levels: 1024 and 4096 (each >> 1) */
#pragma unroll
  for (int lv = 0; lv < 2; lv++) {
    const int sub = lv ? 1024 : 256;
    const short *tw = lv ? tw4096 : tw1024;
    for (int b = t; b < N / 4; b += THREADS) {
      const int p = b % sub, base = (b / sub) * 4 * sub + p;
      short2 y0, y1, y2, y3;
      bfly4(s[base], s[base + sub], s[base + 2 * sub], s[base + 3 * sub], twc(tw, p), twc(tw, sub + p), twc(tw, 2 * sub + p), y0,
            y1, y2, y3);
      s[base] = shr(y0, 1);
      s[base + sub] = shr(y1, 1);
      s[base + 2 * sub] = shr(y2, 1);
      s[base + 3 * sub] = shr(y3, 1);
    }
    __syncthreads();
  }
  /* apply_nr_rotation_symbol_RX on the occupied subcarriers */
  const int len = (nb_rb & 1) ? (nb_rb + 1) * 6 : nb_rb * 6, start2 = (nb_rb & 1) ? fco - 6 : fco;
  const short2 r = rot[sym];
  const int ar = r.x, ai = (short)(-(int)r.y);
  short2 *o = out + ((size_t)ant * 14 + sym) * N;
  for (int k = t; k < N; k += THREADS) {
    short2 v = s[k];
    if (k < len || (k >= start2 && k < start2 + len)) {
      /* rotate_cpx_vector, QRDMX: real = vqrdmlsh(vqdmulh(ar, br), ai, bi), imag = vqrdmlah(vqdmulh(ar, bi), ai, br) */
      const int br = v.x, bi = v.y;
      const int re0 = sat16((int)((2LL * ar * br) >> 16)), im0 = sat16((int)((2LL * ar * bi) >> 16));
      const short re = sat16((int)((((long long)re0 << 16) - 2LL * ai * bi + 32768) >> 16));
      const short im = sat16((int)((((long long)im0 << 16) + 2LL * ai * br + 32768) >> 16));
      /* mult_cpx_vector(.., 15): madd_epi16 of (re, -im) / (im, re) with (c, d), srai 15, packs */
      const short2 w = __ldg(&ts[k]);
      const short nim = (short)(-(int)im);
      v = make_short2(sat16(padd(re * w.x, nim * w.y) >> 15), sat16(padd(im * w.x, re * w.y) >> 15));
    }
    o[k] = v;
  }
}

/* Host: composed input permutation of the recursive transposes (xtmp[N/4 l + m] = x[4m + l] at each
 * level down to 16) and the init_rad4() twiddles exactly as oai_dfts_neon.c computes them. */
static void build_perm(uint16_t *perm)
{
  static uint16_t a[N], b[N];
  for (int n = 0; n < N; n++) a[n] = (uint16_t)n;
  for (int L = N; L > 16; L /= 4) {
    for (int base = 0; base < N; base += L)
      for (int l = 0; l < 4; l++)
        for (int m = 0; m < L / 4; m++) b[base + l * (L / 4) + m] = a[base + 4 * m + l];
    memcpy(a, b, sizeof(a));
  }
  memcpy(perm, a, sizeof(a));
}
static void init_rad4_host(int n, short *tw)
{
  short *twa = tw, *twb = twa + n / 2, *twc_ = twb + n / 2;
  for (int i = 0; i < n / 4; i++) {
    *twa++ = (short)round(32767.0 * cos(2 * M_PI * i / n));
    *twa++ = -(short)round(32767.0 * sin(2 * M_PI * i / n));
    *twb++ = (short)round(32767.0 * cos(2 * M_PI * 2 * i / n));
    *twb++ = -(short)round(32767.0 * sin(2 * M_PI * 2 * i / n));
    *twc_++ = (short)round(32767.0 * cos(2 * M_PI * 3 * i / n));
    *twc_++ = -(short)round(32767.0 * sin(2 * M_PI * 3 * i / n));
  }
}
} // namespace nr_exact
#endif
