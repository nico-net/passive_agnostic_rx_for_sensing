/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* GPU twins of the ISA-dependent CPU primitives the bit-exact GPU front end mirrors (2026-10-24).
 *
 * WHY: the exact FEP / delay / UL FEP kernels must hand the CPU chain the SAME int16 bits the CPU of the host computes.
 * The CPU DFT and the rotation are ISA-specific:
 *   - aarch64: oai_dfts_neon.c dft4096/idft4096 (radix-4 DIT, vqshrn) -> nr_exact::fft4096 (nr_pdsch_gpu_exact_fep.cuh),
 *              rotate_cpx_vector QRDMX (ARMv8.1+) or vmull/vqrshrn (ARMv8.0);
 *   - x86_64:  oai_dfts.c dft_mixed_radix_c16_scaled -> dft_split_radix_pure_simd (N >= 256 power of two), leaves
 *              dft64_avx (8x8, mulhrs twiddles, rounded DC) and dft128_dir (one radix-2 stage + 2 x dft64), the combine
 *              sr_combine_simd (madd/round/packs twiddles, wrapping add/sub); the same code in AVX2 and AVX-512 builds
 *              (the file has no other x86 variant; it needs AVX2 to compile at all);
 *              rotate_cpx_vector: madd_epi16 / srai 15 / packs_epi32 in its AVX-512, AVX2 and SSE tiers alike.
 * This file is the x86 twin (scalar per element, lane for lane what the intrinsics compute) and the rotation twins.
 * Which twin a process uses is decided at run time by nr_exact::isa_profile() (nr_pdsch_gpu_exact_fep.cuh): a self-test
 * against the host's own CPU dft()/idft() and rotate_cpx_vector (nr_gpu_isa_ref.c); no twin matches -> profile NONE and
 * the bit-exact GPU paths are refused (CPU path). */
#ifndef NR_GPU_ISA_TWIN_CUH
#define NR_GPU_ISA_TWIN_CUH
#include <cmath>
#include <cstdint>
#include <vector>

namespace nr_isa {
/* profile = (DFT twin, rotation twin) of a real CPU build; the value doubles as the rotation-twin id */
enum { P_NONE = 0, P_NEON_QRDMX = 1, P_NEON_V80 = 2, P_X86 = 3 };
enum { DFT_NONE = 0, DFT_NEON = 1, DFT_X86 = 2 };
__host__ __device__ constexpr int dft_of(int p) { return p == P_X86 ? DFT_X86 : (p == P_NEON_QRDMX || p == P_NEON_V80) ? DFT_NEON : DFT_NONE; }
static inline const char *dft_name(int d) { return d == DFT_NEON ? "neon" : d == DFT_X86 ? "avx2" : "none"; }
static inline const char *rot_name(int r) { return r == P_NEON_QRDMX ? "qrdmx" : r == P_NEON_V80 ? "v80" : r == P_X86 ? "madd" : "none"; }

/* ---- x86 intrinsic semantics, per int16 lane ---- */
__host__ __device__ __forceinline__ short xs16(int v) { return (short)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
__host__ __device__ __forceinline__ short xadds(short a, short b) { return xs16((int)a + b); }          /* _mm256_adds_epi16 */
__host__ __device__ __forceinline__ short xsubs(short a, short b) { return xs16((int)a - b); }          /* _mm256_subs_epi16 */
/* int16 wrap-around, spelled as selects in the int domain. NOT (short)(-(int)a) / (short)(uint16_t)(..): ptxas (nvcc 12.4,
 * sm_89, measured 2026-10-24) folds sext16(neg16(a)) into a 32-bit negation, so a = -32768 gives +32768 when the result is
 * consumed in a register (same for neg.s16 in inline PTX). */
__host__ __device__ __forceinline__ int xwrap(int v) { return v > 32767 ? v - 65536 : v < -32768 ? v + 65536 : v; }
__host__ __device__ __forceinline__ short xaddw(short a, short b) { return (short)xwrap((int)a + b); } /* _mm256_add_epi16 */
__host__ __device__ __forceinline__ short xsubw(short a, short b) { return (short)xwrap((int)a - b); } /* _mm256_sub_epi16 */
/* xneg returns int on purpose: a short return lets ptxas fold the select back into the faulty 16-bit negation */
__host__ __device__ __forceinline__ int xneg(int a) { return a == -32768 ? -32768 : -a; } /* sign_epi16 by -1, vmulq_s16 by -1 (a in int16) */
__host__ __device__ __forceinline__ short xmulhrs(int a, int b) { return (short)xwrap((a * b + 0x4000) >> 15); } /* _mm256_mulhrs_epi16 (a, b in int16) */
__host__ __device__ __forceinline__ int xmadd(int a, int b, int c, int d) /* _mm256_madd_epi16: a*b + c*d (int16 operands), wraps */
{
  return (int)((unsigned)(a * b) + (unsigned)(c * d));
}
__host__ __device__ __forceinline__ short2 xadds2(short2 a, short2 b) { return make_short2(xadds(a.x, b.x), xadds(a.y, b.y)); }
__host__ __device__ __forceinline__ short2 xsubs2(short2 a, short2 b) { return make_short2(xsubs(a.x, b.x), xsubs(a.y, b.y)); }
__host__ __device__ __forceinline__ short2 xaddw2(short2 a, short2 b) { return make_short2(xaddw(a.x, b.x), xaddw(a.y, b.y)); }
__host__ __device__ __forceinline__ short2 xsubw2(short2 a, short2 b) { return make_short2(xsubw(a.x, b.x), xsubw(a.y, b.y)); }
__host__ __device__ __forceinline__ short2 xmulc(short2 a, short c) { return make_short2(xmulhrs(a.x, c), xmulhrs(a.y, c)); }
/* mul_minus_j_i16_256: swap + sign(+1,-1): (r, i) -> (i, -r); mul_j_i16_256: (r, i) -> (-i, r) */
__host__ __device__ __forceinline__ short2 xmj(short2 z) { return make_short2(z.y, xneg(z.x)); }
__host__ __device__ __forceinline__ short2 xpj(short2 z) { return make_short2(xneg(z.y), z.x); }
/* complex_mul8_prepack_q15_256 with (w_re_re, w_im_signed) = ((wr, wr), (-wi, wi)) */
__host__ __device__ __forceinline__ short2 xcmul8(short2 a, short2 w)
{
  return make_short2(xadds(xmulhrs(a.x, w.x), xmulhrs(a.y, xneg(w.y))), xadds(xmulhrs(a.y, w.x), xmulhrs(a.x, w.y)));
}
/* c16_mul_q15_simd256: madd with (wr, -wi) / (wi, wr), + 2^14 (wraps), srai 15, packs */
__host__ __device__ __forceinline__ short2 xcmulq(short2 x, short2 w)
{
  const short nwi = xs16(-(int)w.y);
  const int re = (int)((unsigned)xmadd(x.x, w.x, x.y, nwi) + 16384u) >> 15;
  const int im = (int)((unsigned)xmadd(x.x, w.y, x.y, w.x) + 16384u) >> 15;
  return make_short2(xs16(re), xs16(im));
}

static const short X_INV_SQRT2 = 23170;

/* mul_minus_j_dir_i16_256 / mul_plus_j_dir_i16_256 */
template <bool INV> __host__ __device__ __forceinline__ short2 xmjd(short2 z) { return INV ? xpj(z) : xmj(z); }
template <bool INV> __host__ __device__ __forceinline__ short2 xpjd(short2 z) { return INV ? xmj(z) : xpj(z); }
/* dft8x8_q15_256_dir on one lane: x[0..7] -> Y[0..7] */
template <bool INV>
__host__ __device__ __forceinline__ void xdft8(const short2 *x, short2 *Y)
{
  const short2 s04 = xadds2(x[0], x[4]), d04 = xsubs2(x[0], x[4]);
  const short2 s15 = xadds2(x[1], x[5]), d15 = xsubs2(x[1], x[5]);
  const short2 s26 = xadds2(x[2], x[6]), d26 = xsubs2(x[2], x[6]);
  const short2 s37 = xadds2(x[3], x[7]), d37 = xsubs2(x[3], x[7]);
  const short2 s02 = xadds2(s04, s26), d02 = xsubs2(s04, s26);
  const short2 s13 = xadds2(s15, s37), d13 = xsubs2(s15, s37);
  Y[0] = xadds2(s02, s13);
  Y[4] = xsubs2(s02, s13);
  Y[2] = xadds2(d02, xmjd<INV>(d13));
  Y[6] = xadds2(d02, xpjd<INV>(d13));
  const short2 p = xadds2(d15, d37), q = xsubs2(d15, d37);
  const short2 base_mj = xadds2(d04, xmjd<INV>(d26)), base_pj = xadds2(d04, xpjd<INV>(d26));
  const short2 t1 = xmulc(xadds2(q, xmjd<INV>(p)), X_INV_SQRT2), t3 = xmulc(xadds2(q, xpjd<INV>(p)), X_INV_SQRT2);
  Y[1] = xadds2(base_mj, t1);
  Y[5] = xsubs2(base_mj, t1);
  Y[7] = xadds2(base_pj, t3);
  Y[3] = xsubs2(base_pj, t3);
}

/* Layout: the split-radix tree is laid out in place, as dft_split_radix_pure_simd_core's sub_out: a node of size P at
 * offset off has E at [off, off + P/2), O1 at [off + P/2, off + 3P/4), O3 at [off + 3P/4, off + P); leaves are 64 / 128.
 * The leaf containing position o: */
__host__ __device__ __forceinline__ void xleaf(int N, int o, int &off, int &P)
{
  off = 0;
  P = N;
  while (P > 128) {
    const int h = P >> 1, q = P >> 2, r = o - off;
    if (r < h) P = h;
    else if (r < h + q) { off += h; P = q; }
    else { off += h + q; P = q; }
  }
}
/* is [o, o + M) a split-radix node (M >= 256)? */
__host__ __device__ __forceinline__ bool xnode(int N, int o, int M)
{
  int off = 0, P = N;
  while (P > M) {
    const int h = P >> 1, q = P >> 2, r = o - off;
    if (r < h) P = h;
    else if (r < h + q) { off += h; P = q; }
    else { off += h + q; P = q; }
  }
  return P == M && off == o;
}

/* twiddle blob (short2): [0,64) C64 fwd (m*8+j), [64,128) C64 inv, [128,192) W128 fwd, [192,256) W128 inv,
 * [256, 256 + (N-128)) SR fwd, then SR inv: level M (256..N) at (M - 256) / 2: w1[0..M/4), w3[0..M/4) */
__host__ __device__ __forceinline__ int xsr_base(int N, bool inv) { return 256 + (inv ? (N >= 256 ? N - 128 : 0) : 0); }
static inline size_t xblob_len(int N) { return 256 + (N >= 256 ? 2 * (size_t)(N - 128) : 0); }

/* In-place DFT (INV = idft) of N (power of two, 64..4096) samples in s[], loaded through xperm (s[p] = x[perm[p]]), scratch
 * t[N]; natural-order output in s[]. One block, THREADS threads, uniform barriers. Bit-exact twin of oai_dfts.c (x86). */
template <bool INV, int THREADS>
__device__ void fft_x86(short2 *s, short2 *t, int tid, int N, const short2 *__restrict__ tw)
{
  const short2 *C64 = tw + (INV ? 64 : 0), *W128 = tw + (INV ? 192 : 128);
  /* phase 1: per dft64 unit u (a 64-leaf or one half of a 128-leaf) and column j: (128 stage0), first dft8, twiddles */
  for (int w = tid; w < N / 8; w += THREADS) {
    const int u = w >> 3, j = w & 7;
    int loff, P;
    xleaf(N, 64 * u, loff, P);
    short2 x[8], H[8];
    if (P == 64) {
#pragma unroll
      for (int m = 0; m < 8; m++) x[m] = s[loff + 8 * m + j];
    } else {
      const bool hb = (64 * u - loff) != 0;
#pragma unroll
      for (int m = 0; m < 8; m++) {
        const int i = 8 * m + j;
        const short2 x0 = s[loff + i], x1 = s[loff + 64 + i];
        x[m] = hb ? xcmul8(xsubs2(x0, x1), __ldg(&W128[i])) : xmulc(xadds2(x0, x1), X_INV_SQRT2);
      }
    }
    xdft8<INV>(x, H);
    t[64 * u + j] = H[0]; /* unshifted: the DC is taken from it in phase 2 */
#pragma unroll
    for (int m = 1; m < 8; m++) t[64 * u + 8 * m + j] = xcmul8(H[m], __ldg(&C64[8 * m + j]));
  }
  __syncthreads();
  /* phase 2: per unit and row m (k1): rounded DC, >> 3 of row 0, second dft8, write the leaf output */
  for (int w = tid; w < N / 8; w += THREADS) {
    const int u = w >> 3, m = w & 7;
    int loff, P;
    xleaf(N, 64 * u, loff, P);
    short2 X[8], Y[8];
#pragma unroll
    for (int n = 0; n < 8; n++) X[n] = t[64 * u + 8 * m + n];
    short2 dc = make_short2(0, 0);
    if (m == 0) {
      int R = 0, I = 0;
#pragma unroll
      for (int n = 0; n < 8; n++) { R += X[n].x; I += X[n].y; }
      R = (R + 4 + (R < 0 ? -1 : 0)) >> 3;
      I = (I + 4 + (I < 0 ? -1 : 0)) >> 3;
      dc = make_short2(xs16(R), xs16(I));
#pragma unroll
      for (int n = 0; n < 8; n++) X[n] = make_short2((short)(X[n].x >> 3), (short)(X[n].y >> 3));
    }
    xdft8<INV>(X, Y);
    if (m == 0) Y[0] = dc;
    const int h = (P == 64) ? 0 : (64 * u - loff) / 64;
#pragma unroll
    for (int k = 0; k < 8; k++) {
      const int q = 8 * k + m;
      s[P == 64 ? loff + q : loff + 2 * q + h] = Y[k];
    }
  }
  __syncthreads();
  /* phase 3: split-radix combines, smallest nodes first, in place */
  for (int M = 256; M <= N; M <<= 1) {
    const int q = M >> 2;
    const short2 *w1 = tw + xsr_base(N, INV) + (M - 256) / 2, *w3 = w1 + q;
    for (int w = tid; w < N / 4; w += THREADS) {
      const int o = (w / q) * M, k = w % q;
      if (!xnode(N, o, M)) continue;
      short2 *y = s + o;
      const short2 t1 = xcmulq(y[2 * q + k], __ldg(&w1[k])), t2 = xcmulq(y[3 * q + k], __ldg(&w3[k]));
      const short2 a = xaddw2(t1, t2), d = xsubw2(t1, t2);
      const short2 b = INV ? make_short2(xneg(d.y), d.x) : make_short2(d.y, xneg(d.x));
      const short2 E0 = xmulc(y[k], X_INV_SQRT2), E1 = xmulc(y[q + k], X_INV_SQRT2);
      y[k] = xaddw2(E0, a);
      y[2 * q + k] = xsubw2(E0, a);
      y[q + k] = xaddw2(E1, b);
      y[3 * q + k] = xsubw2(E1, b);
    }
    __syncthreads();
  }
}

/* ---- host: tables, computed with the float formulas of oai_dfts.c (same libm calls, same evaluation order) ---- */
static inline int16_t xh_sat(long v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
static inline int16_t xh_q15(float x) { return xh_sat((int32_t)lrintf(32767.0f * x)); } /* q15_from_float */
static inline void xh_w(int k, int N, bool inv, float &re, float &im) /* twiddle_table_create: forward c - I s, inverse c + I s */
{
  const float theta = 2.0f * (float)M_PI * (float)k / (float)N;
  const float c = cosf(theta), sn = sinf(theta);
  re = c;
  im = inv ? sn : -sn;
}
/* input permutation: layout position -> input index (E = x[2m], O1 = x[4m+1], O3 = x[4m+3], recursively) */
static inline void xh_perm(uint16_t *perm, int off, int P, int base, int stride)
{
  if (P <= 128) {
    for (int i = 0; i < P; i++) perm[off + i] = (uint16_t)(base + i * stride);
    return;
  }
  xh_perm(perm, off, P / 2, base, 2 * stride);
  xh_perm(perm, off + P / 2, P / 4, base + stride, 4 * stride);
  xh_perm(perm, off + 3 * P / 4, P / 4, base + 3 * stride, 4 * stride);
}
static inline void x86_tables(int N, std::vector<uint16_t> &perm, std::vector<short> &blob)
{
  perm.assign(N, 0);
  xh_perm(perm.data(), 0, N, 0, 1);
  blob.assign(2 * xblob_len(N), 0);
  short *b = blob.data();
  for (int inv = 0; inv < 2; inv++)
    for (int m = 0; m < 8; m++)
      for (int j = 0; j < 8; j++) { /* pack8_twiddle_q15_*256i, N = 64: q15 / 8 */
        float re, im;
        xh_w((m * j) % 64, 64, inv, re, im);
        const int e = 2 * (64 * inv + 8 * m + j);
        b[e] = (int16_t)(float)(xh_q15(re) / 8);
        b[e + 1] = (int16_t)(float)(xh_q15(im) / 8);
      }
  for (int inv = 0; inv < 2; inv++)
    for (int i = 0; i < 64; i++) { /* N = 128: q15 / sqrtf(2) (float), truncated */
      float re, im;
      xh_w(i, 128, inv, re, im);
      const int e = 2 * (128 + 64 * inv + i);
      b[e] = (int16_t)((float)xh_q15(re) / sqrtf(2.0f));
      b[e + 1] = (int16_t)((float)xh_q15(im) / sqrtf(2.0f));
    }
  for (int inv = 0; inv < 2; inv++) { /* init_sr_twiddle_simd */
    const int dir = inv ? 1 : -1;
    for (int M = 256; M <= N; M <<= 1) {
      const int q = M / 4, base = xsr_base(N, inv) + (M - 256) / 2;
      for (int k = 0; k < q; k++) {
        const float theta1 = (float)dir * 2.0f * (float)M_PI * k / (float)M;
        const float theta3 = (float)dir * 6.0f * (float)M_PI * k / (float)M;
        short *w1 = b + 2 * (base + k), *w3 = b + 2 * (base + q + k);
        w1[0] = (int16_t)(xh_sat(lrintf(32767.0f * cosf(theta1))) / 2);
        w1[1] = (int16_t)(xh_sat(lrintf(32767.0f * sinf(theta1))) / 2);
        w3[0] = (int16_t)(xh_sat(lrintf(32767.0f * cosf(theta3))) / 2);
        w3[1] = (int16_t)(xh_sat(lrintf(32767.0f * sinf(theta3))) / 2);
      }
    }
  }
}

/* ---- rotation twins: rotate_cpx_vector(x, alpha, ., n, 15) with alpha = (ar, ai) = conj(symbol_rotation) ---- */
template <int R>
__host__ __device__ __forceinline__ short2 rot_twin(short2 v, int ar, int ai)
{
  const int br = v.x, bi = v.y;
  if (R == P_X86) { /* madd_epi16 of (br, bi) with (ar, -ai) / (ai, ar), srai 15, packs: truncating */
    return make_short2(xs16(xmadd(br, ar, bi, xneg(ai)) >> 15), xs16(xmadd(br, ai, bi, ar) >> 15));
  } else if (R == P_NEON_V80) { /* vmull/vmlsl (wrapping int32), vqrshrn_n_s32(., 15) */
    const int re = (int)((unsigned)(ar * br) - (unsigned)(ai * bi)), im = (int)((unsigned)(ar * bi) + (unsigned)(ai * br));
    return make_short2(xs16((int)(((long long)re + 16384) >> 15)), xs16((int)(((long long)im + 16384) >> 15)));
  } else { /* QRDMX: real = vqrdmlsh(vqdmulh(ar, br), ai, bi), imag = vqrdmlah(vqdmulh(ar, bi), ai, br) */
    const int re0 = xs16((int)((2LL * ar * br) >> 16)), im0 = xs16((int)((2LL * ar * bi) >> 16));
    const short re = xs16((int)((((long long)re0 << 16) - 2LL * ai * bi + 32768) >> 16));
    const short im = xs16((int)((((long long)im0 << 16) + 2LL * ai * br + 32768) >> 16));
    return make_short2(re, im);
  }
}
} // namespace nr_isa
#endif
