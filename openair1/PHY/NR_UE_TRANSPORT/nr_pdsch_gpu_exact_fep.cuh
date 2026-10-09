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

/* radix-4 kernel with 16-bit twiddled inputs (bfly4_16 and the second dft16 stage); the inverse
 * (ibfly4_16, idft16) differs only in swapping outputs 1 and 3 */
template <bool INV>
__device__ __forceinline__ void r4_16(short2 x0, short2 x1t, short2 x2t, short2 x3t, short2 &y0, short2 &y1, short2 &y2,
                                      short2 &y3)
{
  short2 x02 = qadd(x0, x2t), x13 = qadd(x1t, x3t);
  y0 = qadd(x02, x13);
  y2 = qsub(x02, x13);
  x02 = qsub(x0, x2t);
  x13 = qsub(flip(x1t), flip(x3t));
  (INV ? y3 : y1) = qadd(x02, x13);
  (INV ? y1 : y3) = qsub(x02, x13);
}
/* cmultc(a, b) of ibfly4: a * conj(b) with bflip = (-b.i, b.r) */
__device__ __forceinline__ void cmultc(short2 a, short2 b, int &re, int &im)
{
  const short bi = (short)(-(int)b.y);
  re = padd(a.x * b.x, a.y * b.y);
  im = padd(a.x * bi, a.y * b.x);
}

/* bfly4 (32-bit accumulation); ibfly4 = cmultc twiddles and outputs 1/3 swapped */
template <bool INV>
__device__ __forceinline__ void bfly4(short2 x0, short2 x1, short2 x2, short2 x3, short2 t1, short2 t2, short2 t3, short2 &y0,
                                      short2 &y1, short2 &y2, short2 &y3)
{
  int x1r, x1i, x2r, x2i, x3r, x3i;
  if (INV) {
    cmultc(x1, t1, x1r, x1i);
    cmultc(x2, t2, x2r, x2i);
    cmultc(x3, t3, x3r, x3i);
  } else {
    cmult(x1, t1, x1r, x1i);
    cmult(x2, t2, x2r, x2i);
    cmult(x3, t3, x3r, x3i);
  }
  y0 = qadd(x0, make_short2(pk(qa(x1r, qa(x2r, x3r))), pk(qa(x1i, qa(x2i, x3i)))));
  (INV ? y3 : y1) = qadd(x0, make_short2(pk(qs(x1i, qa(x2r, x3i))), pk(qs(qs(x3r, x2i), x1r))));
  y2 = qadd(x0, make_short2(pk(qs(qs(x2r, x3r), x1r)), pk(qs(qs(x2i, x3i), x1i))));
  (INV ? y1 : y3) = qadd(x0, make_short2(pk(qs(qs(x3i, x2r), x1i)), pk(qs(x1r, qa(x2i, x3r)))));
}

__device__ __forceinline__ short2 twc(const short *t, int k) { return make_short2(t[2 * k], t[2 * k + 1]); }

/* dft16 / idft16 of oai_dfts_neon.c on 16 consecutive complex values */
template <bool INV>
__device__ __forceinline__ void dft16(short2 *s)
{
  const short *ta = INV ? g_ex_tw16 : g_ex_tw16a, *tb = INV ? g_ex_tw16c : g_ex_tw16b;
  short2 x[16], t[16];
#pragma unroll
  for (int n = 0; n < 16; n++) x[n] = s[n];
#pragma unroll
  for (int k = 0; k < 4; k++) /* first stage, lane k of the 4 input vectors */
    r4_16<INV>(x[k], x[4 + k], x[8 + k], x[12 + k], t[k], t[4 + k], t[8 + k], t[12 + k]);
#pragma unroll
  for (int k = 0; k < 4; k++) { /* after the 4x4 transpose, ytmp_l lane k = t[4k + l] */
    const short2 y1 = pcm2(t[4 * k + 1], twc(ta, k), twc(tb, k));
    const short2 y2 = pcm2(t[4 * k + 2], twc(ta, 4 + k), twc(tb, 4 + k));
    const short2 y3 = pcm2(t[4 * k + 3], twc(ta, 8 + k), twc(tb, 8 + k));
    r4_16<INV>(t[4 * k], y1, y2, y3, s[k], s[4 + k], s[8 + k], s[12 + k]);
  }
}

/* The in-place transform after the permuted load (s[n] = x[perm[n]]): dft4096() (INV=false) or
 * idft4096() (INV=true) of oai_dfts_neon.c. Level 64: (i)bfly4_16, >> 3. Level 256: bfly4_16 forward
 * but 32-bit ibfly4 with tw256 inverse, >> 1. Levels 1024 and 4096: (i)bfly4, >> 1 each. */
template <bool INV>
__device__ __forceinline__ void fft4096(short2 *s, int t, const short *__restrict__ tw1024, const short *__restrict__ tw4096)
{
  dft16<INV>(&s[16 * t]);
  __syncthreads();
  /* level 64: (i)bfly4_16, >> 3 */
  for (int b = t; b < N / 4; b += THREADS) {
    const int sub = 16, p = b % sub, base = (b / sub) * 4 * sub + p;
    const short *ta = INV ? g_ex_tw64 : g_ex_tw64a, *tb = INV ? g_ex_tw64c : g_ex_tw64b;
    short2 y0, y1, y2, y3;
    const short2 x1t = pcm2(s[base + sub], twc(ta, p), twc(tb, p));
    const short2 x2t = pcm2(s[base + 2 * sub], twc(ta, sub + p), twc(tb, sub + p));
    const short2 x3t = pcm2(s[base + 3 * sub], twc(ta, 2 * sub + p), twc(tb, 2 * sub + p));
    r4_16<INV>(s[base], x1t, x2t, x3t, y0, y1, y2, y3);
    s[base] = shr(y0, 3);
    s[base + sub] = shr(y1, 3);
    s[base + 2 * sub] = shr(y2, 3);
    s[base + 3 * sub] = shr(y3, 3);
  }
  __syncthreads();
  /* level 256 */
  for (int b = t; b < N / 4; b += THREADS) {
    const int sub = 64, p = b % sub, base = (b / sub) * 4 * sub + p;
    short2 y0, y1, y2, y3;
    if (INV) {
      bfly4<true>(s[base], s[base + sub], s[base + 2 * sub], s[base + 3 * sub], twc(g_ex_tw256, p), twc(g_ex_tw256, sub + p),
                  twc(g_ex_tw256, 2 * sub + p), y0, y1, y2, y3);
    } else {
      const short2 x1t = pcm2(s[base + sub], twc(g_ex_tw256a, p), twc(g_ex_tw256b, p));
      const short2 x2t = pcm2(s[base + 2 * sub], twc(g_ex_tw256a, sub + p), twc(g_ex_tw256b, sub + p));
      const short2 x3t = pcm2(s[base + 3 * sub], twc(g_ex_tw256a, 2 * sub + p), twc(g_ex_tw256b, 2 * sub + p));
      r4_16<false>(s[base], x1t, x2t, x3t, y0, y1, y2, y3);
    }
    s[base] = shr(y0, 1);
    s[base + sub] = shr(y1, 1);
    s[base + 2 * sub] = shr(y2, 1);
    s[base + 3 * sub] = shr(y3, 1);
  }
  __syncthreads();
  /* (i)bfly4 levels: 1024 and 4096 (each >> 1) */
#pragma unroll
  for (int lv = 0; lv < 2; lv++) {
    const int sub = lv ? 1024 : 256;
    const short *tw = lv ? tw4096 : tw1024;
    for (int b = t; b < N / 4; b += THREADS) {
      const int p = b % sub, base = (b / sub) * 4 * sub + p;
      short2 y0, y1, y2, y3;
      bfly4<INV>(s[base], s[base + sub], s[base + 2 * sub], s[base + 3 * sub], twc(tw, p), twc(tw, sub + p), twc(tw, 2 * sub + p),
                 y0, y1, y2, y3);
      s[base] = shr(y0, 1);
      s[base + sub] = shr(y1, 1);
      s[base + 2 * sub] = shr(y2, 1);
      s[base + 3 * sub] = shr(y3, 1);
    }
    __syncthreads();
  }
}

/* Where the kernel reads the time-domain slot and writes rxdataF. Staged: the module's device copy of
 * the window (origin 0, no wrap) and its device output. Direct (shared-memory platforms, GB10): the
 * receiver's own host ring (modulo ring_len) and the caller's rxdataF -- no copies at all. */
struct ExactIO {
  const short2 *rx[4];
  uint32_t ring_len, origin;
  short2 *out;
  size_t ant_stride; /* c16 per antenna in out; symbol sym starts at sym * N */
  short2 rot[14];    /* symbol_rotation of this slot (by value: no per-call upload) */
  uint16_t sym_list[14];
};

/* One block per (symbol, antenna). perm: input index of each in-place DIT position (the composed
 * transpose16_ooff permutations of dft4096/1024/256/64). Writes rxdataF exactly as the CPU does. */
__global__ void __launch_bounds__(THREADS) k_fep_exact(const ExactIO io, int ncp, int ncp0, const uint16_t *__restrict__ perm,
                                                       const short *__restrict__ tw1024, const short *__restrict__ tw4096,
                                                       const short2 *__restrict__ ts, int nb_rb, int fco)
{
  __shared__ short2 s[N];
  const int sym = io.sym_list[blockIdx.x], ant = blockIdx.y, t = threadIdx.x;
  const short2 *in = io.rx[ant];
  const uint32_t first = io.origin + (uint32_t)(ncp0 + sym * (N + ncp));
  for (int n = t; n < N; n += THREADS) s[n] = in[(first + __ldg(&perm[n])) % io.ring_len];
  __syncthreads();
  fft4096<false>(s, t, tw1024, tw4096);
  /* apply_nr_rotation_symbol_RX on the occupied subcarriers */
  const int len = (nb_rb & 1) ? (nb_rb + 1) * 6 : nb_rb * 6, start2 = (nb_rb & 1) ? fco - 6 : fco;
  const short2 r = io.rot[sym];
  const int ar = r.x, ai = (short)(-(int)r.y);
  short2 *o = io.out + (size_t)ant * io.ant_stride + (size_t)sym * N;
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

/* GPU6 delay estimate: nr_passive_est_delay() of nr_passive_delay.h, i.e. the 6 dB-headroom block scaling
 * (C division by 2^shift, truncating toward zero), nr_est_delay()'s idft4096 and its |x|^2 >> 1 peak search
 * (first index wins ties, mean over all N, PEAK_DETECT_THRESHOLD 15), with the peak power restored to input
 * units. One block per LS row (host memory on a shared-memory platform). out[3 * row] = {est_delay,
 * delay_max_pos, delay_max_val} of the CANDIDATE; the caller keeps its own "replace if >=" rule. */
static const int DELAY_MAX_ROWS = 16;
struct DelayIO {
  const short2 *ls[DELAY_MAX_ROWS];
};
__global__ void __launch_bounds__(THREADS) k_delay_exact(DelayIO io, const uint16_t *__restrict__ perm,
                                                         const short *__restrict__ tw1024, const short *__restrict__ tw4096,
                                                         int *__restrict__ out)
{
  __shared__ short2 s[N];
  __shared__ int peak_l1, shift;
  __shared__ unsigned long long sum, best;
  const int t = threadIdx.x;
  const short2 *ls = io.ls[blockIdx.x];
  if (t == 0) { peak_l1 = 0; sum = 0; best = 0; }
  __syncthreads();
  int pk = 0;
  for (int n = t; n < N; n += THREADS) {
    const short2 v = ls[n];
    pk = max(pk, abs((int)v.x) + abs((int)v.y));
  }
  atomicMax(&peak_l1, pk);
  __syncthreads();
  if (t == 0) {
    int sh = 0;
    const int limit = 16383 / 64; /* root = 64 for N = 4096 */
    while ((peak_l1 >> sh) > limit) ++sh;
    shift = sh;
  }
  __syncthreads();
  const int div = 1 << shift;
  for (int n = t; n < N; n += THREADS) {
    const short2 v = ls[__ldg(&perm[n])];
    s[n] = make_short2((short)(v.x / div), (short)(v.y / div));
  }
  __syncthreads();
  fft4096<true>(s, t, tw1024, tw4096);
  unsigned long long my_sum = 0, my_best = 0;
  for (int k = t; k < N; k += THREADS) {
    const short2 v = s[k];
    const int temp = (int)((unsigned)((int)v.x * v.x + (int)v.y * v.y) >> 1);
    my_sum += (unsigned long long)temp;
    /* max value, then LOWEST index among equal values (the CPU scan keeps the first strict maximum) */
    const unsigned long long key = ((unsigned long long)(unsigned)temp << 13) | (unsigned)(N - 1 - k);
    my_best = key > my_best ? key : my_best;
  }
  atomicAdd(&sum, my_sum);
  atomicMax(&best, my_best);
  __syncthreads();
  if (t == 0) {
    const int max_val = (int)(best >> 13);
    int max_pos = max_val > 0 ? N - 1 - (int)(best & 8191) : 0;
    const unsigned long long mean = sum / N;
    if (max_pos > N / 2) max_pos -= N;
    const int est = mean > 0 && (unsigned long long)max_val / mean > 15 ? max_pos : 0;
    const unsigned long long restored = ((unsigned long long)max_val << (2 * shift)) / (unsigned)N;
    out[3 * blockIdx.x] = est;
    out[3 * blockIdx.x + 1] = max_pos;
    out[3 * blockIdx.x + 2] = restored > INT_MAX ? INT_MAX : (int)restored;
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
