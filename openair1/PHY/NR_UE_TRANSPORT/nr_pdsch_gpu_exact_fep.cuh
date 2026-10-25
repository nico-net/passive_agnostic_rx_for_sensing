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
#include "nr_gpu_isa_twin.cuh"
#include "nr_gpu_isa_ref.h"
#include <climits>
#include <mutex>
#include <cstring>
#include <cstdio>

namespace nr_exact {
static const int N = 4096, THREADS = 256;

/* sat16: device helper: saturating int to int16.
 *   How it works: Clamp to [-32768, 32767].
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc_gpu.cu:291, openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc_gpu.cu:310, openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc_gpu.cu:342, openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc_gpu.cu:343 (+11 more).
 */
__device__ __forceinline__ short sat16(int x) { return (short)max(-32768, min(32767, x)); }
/* sat32: device helper: saturating 64-bit to int32.
 *   How it works: Clamp to the int32 range.
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_exact_fep.cuh:48, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_exact_fep.cuh:49.
 */
__device__ __forceinline__ int sat32(long long x) { return (int)max((long long)INT_MIN, min((long long)INT_MAX, x)); }
/* qadd: device helper: saturating add of two complex int16 pairs.
 *   How it works: Component-wise sat16 add.
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_exact_fep.cuh:57, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_exact_fep.cuh:58, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_exact_fep.cuh:62, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_exact_fep.cuh:88 (+3 more).
 */
__device__ __forceinline__ short2 qadd(short2 a, short2 b) { return make_short2(sat16(a.x + b.x), sat16(a.y + b.y)); }
/* qsub: device helper: saturating subtract of two complex int16 pairs.
 *   How it works: Component-wise sat16 subtract.
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_exact_fep.cuh:59, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_exact_fep.cuh:60, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_exact_fep.cuh:61, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_exact_fep.cuh:63.
 */
__device__ __forceinline__ short2 qsub(short2 a, short2 b) { return make_short2(sat16(a.x - b.x), sat16(a.y - b.y)); }
/* shr: device helper: arithmetic right shift of both components.
 *   How it works: a >> s on real and imaginary parts.
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_exact_fep.cuh:133, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_exact_fep.cuh:134, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_exact_fep.cuh:135, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_exact_fep.cuh:136 (+8 more).
 */
__device__ __forceinline__ short2 shr(short2 a, int s) { return make_short2((short)(a.x >> s), (short)(a.y >> s)); }
/* vrev32q_s16(vmulq_s16(x, {-1,1})): (re, im) -> (im, -re), int16 wrap */
/* flip: device helper: multiply a complex int16 by -j.
 *   How it works: (re, im) -> (im, -re).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_exact_fep.cuh:61.
 */
/* neg16: int16 wrapping negation (-(-32768) = -32768, as vmulq_s16 by -1) of a value that is STORED as int16. nr_isa::xneg (an
 * int-domain select) is not enough here: the short store lets NVVM turn the select back into trunc16 -> neg.s16 -> sext16, and
 * ptxas (CUDA 13.0, sm_121) folds that into a 32-bit negation when the operand comes from a [-32768, 32767] clamp (pcm2 / qsub):
 * +32768 for -32768, i.e. flip() of a saturated twiddled sample was wrong on the GB10 (dft64 level, about 1 output in 8000 at
 * amplitude 4000, most outputs at full scale). The sign extension is done with a byte permute (prmt sign-replicate) of the
 * 32-bit negation, which neither compiler folds. */
__device__ __forceinline__ short neg16(short a)
{
  int r;
  asm("prmt.b32 %0, %1, 0, 0x9910;" : "=r"(r) : "r"(-(int)a)); /* bytes {b0, b1, sign(b1), sign(b1)} = sext16(low 16 bits) */
  return (short)r;
}
__device__ __forceinline__ short2 flip(short2 a) { return make_short2(a.y, neg16(a.x)); }
/* vpadd_s32 of two vmull_s16 products: non-saturating int32 sum (wraps only for 2 x (-32768)^2) */
__device__ __forceinline__ int padd(int p, int q) { return (int)((unsigned)p + (unsigned)q); }
/* cpack: vqshrn_n_s32(x, 15) */
/* pk: device helper: Q15 narrowing of a 32-bit product to int16 (round shift by 15, saturate).
 *   How it works: sat16(v >> 15).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_exact_fep.cuh:39, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_exact_fep.cuh:88, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_exact_fep.cuh:89, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_exact_fep.cuh:90 (+1 more).
 */
__device__ __forceinline__ short pk(int v) { return sat16(v >> 15); }
/* packed_cmult2(a, b, b2): re = a.r b[0] + a.i b[1], im = a.r b2[0] + a.i b2[1] */
/* pcm2: device helper: two complex products of a with b and with b2, each Q15-narrowed.
 *   How it works: Uses pk on the real and imaginary sums.
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_exact_fep.cuh:109, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_exact_fep.cuh:110, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_exact_fep.cuh:111, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_exact_fep.cuh:129 (+5 more).
 */
__device__ __forceinline__ short2 pcm2(short2 a, short2 b, short2 b2)
{
  return make_short2(pk(padd(a.x * b.x, a.y * b.y)), pk(padd(a.x * b2.x, a.y * b2.y)));
}
/* cmult(a, b) of bfly4: bconj = b * {1,-1} (int16 wrap), bflip = (b.i, b.r) */
__device__ __forceinline__ void cmult(short2 a, short2 b, int &re, int &im)
{
  const int bi = nr_isa::xneg(b.y);
  re = padd(a.x * b.x, a.y * bi);
  im = padd(a.x * b.y, a.y * b.x);
}
/* qa: device helper: saturating int32 add.
 *   How it works: sat32 of the 64-bit sum.
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_exact_fep.cuh:88, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_exact_fep.cuh:89, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_exact_fep.cuh:91.
 */
__device__ __forceinline__ int qa(int a, int b) { return sat32((long long)a + b); }
/* qs: device helper: saturating int32 subtract.
 *   How it works: sat32 of the 64-bit difference.
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_exact_fep.cuh:89, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_exact_fep.cuh:90, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_exact_fep.cuh:91.
 */
__device__ __forceinline__ int qs(int a, int b) { return sat32((long long)a - b); }

/* radix-4 kernel with 16-bit twiddled inputs (bfly4_16 and the second dft16 stage); the inverse
 * (ibfly4_16, idft16) differs only in swapping outputs 1 and 3 */
/* r4_16: radix-4 butterfly stage of the 16-point DFT (INV selects the inverse output order).
 *   How it works: Pairwise qadd/qsub of the inputs, with flip for the odd pair.
 *   ISAC_* flags read: none.
 *   Callers: none found in tree.
 */
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
  const int bi = nr_isa::xneg(b.y);
  re = padd(a.x * b.x, a.y * b.y);
  im = padd(a.x * bi, a.y * b.x);
}

/* bfly4 (32-bit accumulation); ibfly4 = cmultc twiddles and outputs 1/3 swapped */
/* bfly4: radix-4 butterfly with twiddles (conjugate twiddles when INV).
 *   How it works: Twiddle multiply (cmult or cmultc), then the four outputs from qa/qs and pk.
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_exact_fep.cuh:73, openair1/PHY/TOOLS/oai_dfts.c:3720, openair1/PHY/TOOLS/oai_dfts_neon.c:371, openair1/PHY/TOOLS/oai_dfts_neon.c:375 (+17 more).
 */
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

/* twc: device helper: loads the complex twiddle k from an interleaved int16 table.
 *   How it works: (t[2k], t[2k+1]).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_exact_fep.cuh:109, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_exact_fep.cuh:110, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_exact_fep.cuh:111, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_exact_fep.cuh:129 (+8 more).
 */
__device__ __forceinline__ short2 twc(const short *t, int k) { return make_short2(t[2 * k], t[2 * k + 1]); }

/* dft16 / idft16 of oai_dfts_neon.c on 16 consecutive complex values */
/* dft16: 16-point DFT done in place on 16 short2 values, in two stages with twiddles.
 *   How it works: First stage: four r4_16 across the lanes. Then twiddle products (pcm2, tables ta/tb) and a second r4_16 per lane.
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/TOOLS/oai_dfts.c:1786, openair1/PHY/TOOLS/oai_dfts_neon.c:688, openair1/PHY/TOOLS/oai_dfts_neon.c:690, openair1/PHY/TOOLS/oai_dfts_neon.c:957 (+3 more).
 */
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
/* fft4096: 4096-point FFT of one thread block, in place in shared memory.
 *   How it works: Levels: dft16 on each group of 16, then a radix-4 level with a >> 3 shift, then a radix-4 level with a >> 1 shift (256-level). Uses the tw1024 and tw4096 tables; INV selects the inverse tables.
 *   ISAC_* flags read: none.
 *   Callers: none found in tree.
 */
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

/* ISA twin dispatch (nr_gpu_isa_twin.cuh). P = the profile chosen at init by isa_profile(); the shared buffer s holds
 * smem_c16(P) short2 (the x86 twin uses s[N..2N) as scratch). Table arguments (ta, tb): NEON twin = (tw1024, tw4096) of
 * init_rad4_host, input permutation build_perm; x86 twin = (the twiddle blob, unused), permutation nr_isa::x86_tables. */
__host__ __device__ constexpr int smem_c16(int P) { return nr_isa::dft_of(P) == nr_isa::DFT_X86 ? 2 * N : N; }
template <int P, bool INV>
__device__ __forceinline__ void fft_twin(short2 *s, int t, const short *__restrict__ ta, const short *__restrict__ tb, int n = N)
{
  if (nr_isa::dft_of(P) == nr_isa::DFT_X86)
    nr_isa::fft_x86<INV, THREADS>(s, s + n, t, n, (const short2 *)ta);
  else
    fft4096<INV>(s, t, ta, tb);
}
/* FFT sizes of the bit-exact FEP: the NEON twin is dft4096 only; the x86 twin (split radix, nr_gpu_isa_twin.cuh) covers every power of two the
 * CPU runs through dft_split_radix_pure_simd, i.e. 256..4096 (N_RB 21..341 at any SCS without 3/4 sampling: 512, 1024, 2048, 4096). The 3/4-sampling
 * sizes (768, 1536, 3072: radix-3 over a strided split radix on the CPU) have no twin: CPU FEP. */
__host__ __device__ constexpr bool fep_size_ok(int P, int n)
{
  return n == N || (nr_isa::dft_of(P) == nr_isa::DFT_X86 && n >= 256 && n <= N && (n & (n - 1)) == 0);
}
/* apply_nr_rotation_symbol_RX on one RE: rotate_cpx_vector(.., 15) by (ar, ai) = conj(symbol_rotation) with the twin of P,
 * then mult_cpx_vector(.., 15) by the timeshift w (madd_epi16 of (re, -im) / (im, re) with (c, d), srai 15, packs; SIMDe:
 * the same on every ISA) */
template <int P>
__device__ __forceinline__ short2 rot_ts(short2 v, int ar, int ai, short2 w)
{
  const short2 r = nr_isa::rot_twin<P>(v, ar, ai);
  const int re = r.x, im = r.y, nim = nr_isa::xneg(im);
  return make_short2(sat16(padd(re * w.x, nim * w.y) >> 15), sat16(padd(im * w.x, re * w.y) >> 15));
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
  const uint32_t *par; /* GPUPASS: non-null = the per-slot staging block, read at execution (graph replay); null = the fields above. Layout (u32 words):
                        * [0] origin, [1..14] rot[14] as short2, [16] ring_len, [17] 1 = out / ant_stride below override io.out / io.ant_stride,
                        * [18..19] out (u64), [20] ant_stride (short2 per antenna), [21] symbols to transform (bit l), [22 + 2 a .. 23 + 2 a] rx[a] (u64) */
};
enum { PAR_WORDS = 32 };

/* One block per (symbol, antenna). perm: input index of each in-place DIT position (the composed
 * transpose16_ooff permutations of dft4096/1024/256/64). Writes rxdataF exactly as the CPU does. */
/* fft_n: the FFT size of this launch; only the x86 twin reads it (the NEON twin is dft4096: N, a compile-time constant there, so the GB10
 * kernel is unchanged). The shared buffer is sized for N = 4096 and serves every smaller size. */
template <int P>
__global__ void __launch_bounds__(THREADS) k_fep_exact(const ExactIO io, int ncp, int ncp0, const uint16_t *__restrict__ perm,
                                                       const short *__restrict__ tw1024, const short *__restrict__ tw4096,
                                                       const short2 *__restrict__ ts, int nb_rb, int fco, int fft_n = N)
{
  __shared__ short2 s[smem_c16(P)];
  const int Nn = nr_isa::dft_of(P) == nr_isa::DFT_X86 ? fft_n : N;
  const int sym = io.sym_list[blockIdx.x], ant = blockIdx.y, t = threadIdx.x;
  if (io.par && !(io.par[21] >> sym & 1u)) return; /* GPUPASS: this grant's symbol set, read when the kernel runs (the graph keeps its full grid) */
  const short2 *in = io.par ? (const short2 *)*(const unsigned long long *)(io.par + 22 + 2 * ant) : io.rx[ant];
  const uint32_t ring_len = io.par ? io.par[16] : io.ring_len;
  const uint32_t first = (io.par ? io.par[0] : io.origin) + (uint32_t)(ncp0 + sym * (Nn + ncp));
  for (int n = t; n < Nn; n += THREADS) s[n] = in[(first + __ldg(&perm[n])) % ring_len];
  __syncthreads();
  fft_twin<P, false>(s, t, tw1024, tw4096, Nn);
  /* apply_nr_rotation_symbol_RX on the occupied subcarriers */
  const int len = (nb_rb & 1) ? (nb_rb + 1) * 6 : nb_rb * 6, start2 = (nb_rb & 1) ? fco - 6 : fco;
  const short2 r = io.par ? ((const short2 *)(io.par + 1))[sym] : io.rot[sym];
  const int ar = r.x, ai = nr_isa::xneg(r.y); /* rot2.i = -rot2.i (int16) */
  const bool redir = io.par && io.par[17];
  short2 *o = (redir ? (short2 *)*(const unsigned long long *)(io.par + 18) : io.out) + (size_t)ant * (redir ? (size_t)io.par[20] : io.ant_stride) + (size_t)sym * Nn;
  for (int k = t; k < Nn; k += THREADS) {
    short2 v = s[k];
    if (k < len || (k >= start2 && k < start2 + len)) v = rot_ts<P>(v, ar, ai, __ldg(&ts[k]));
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
  const short2 *base; /* GPU-DEMOD chest: non-null = row r at base + r * N (any number of rows, one launch) */
};
template <int P>
__global__ void __launch_bounds__(THREADS) k_delay_exact(DelayIO io, const uint16_t *__restrict__ perm,
                                                         const short *__restrict__ tw1024, const short *__restrict__ tw4096,
                                                         int *__restrict__ out)
{
  __shared__ short2 s[smem_c16(P)];
  __shared__ int peak_l1, shift;
  __shared__ unsigned long long sum, best;
  const int t = threadIdx.x;
  const short2 *ls = io.base ? io.base + (size_t)blockIdx.x * N : io.ls[blockIdx.x];
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
  fft_twin<P, true>(s, t, tw1024, tw4096);
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
/* build_perm: host side: builds the permutation table that orders the 4096-point FFT input for the radix-4 stages.
 *   How it works: Starts from the identity and applies one radix-4 digit-reversal pass per level L, from N down to 16, ping-ponging between two static arrays. The result is copied to perm.
 *   ISAC_* flags read: none.
 *   Callers: none found in tree.
 */
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
/* init_rad4_host: host side: fills the radix-4 twiddle table (cos and -sin of k, 2k and 3k, scaled to Q15) for length n.
 *   How it works: Three interleaved int16 pairs per index i in [0, n/4), written into the three consecutive sub-tables of tw.
 *   ISAC_* flags read: none.
 *   Callers: none found in tree.
 */
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

/* twin_tables: the input permutation and the two table arguments (ta, tb) of the twin of profile P, on the host. */
static void twin_tables(int P, std::vector<uint16_t> &perm, std::vector<short> &ta, std::vector<short> &tb, int n = N)
{
  if (nr_isa::dft_of(P) == nr_isa::DFT_X86) {
    nr_isa::x86_tables(n, perm, ta);
    tb.assign(2, 0);
  } else {
    perm.resize(N);
    build_perm(perm.data());
    ta.assign(3 * 512, 0);
    tb.assign(3 * 2048, 0);
    init_rad4_host(1024, ta.data());
    init_rad4_host(4096, tb.data());
  }
}

/* self-test kernels: one block per vector (DFT), one thread per RE (rotation) */
template <int P, bool INV>
__global__ void __launch_bounds__(THREADS) k_twin_dft(const short2 *__restrict__ in, short2 *__restrict__ out, const uint16_t *__restrict__ perm,
                                                      const short *__restrict__ ta, const short *__restrict__ tb)
{
  __shared__ short2 s[smem_c16(P)];
  const short2 *x = in + (size_t)blockIdx.x * N;
  for (int n = threadIdx.x; n < N; n += THREADS) s[n] = x[__ldg(&perm[n])];
  __syncthreads();
  fft_twin<P, INV>(s, threadIdx.x, ta, tb);
  for (int n = threadIdx.x; n < N; n += THREADS) out[(size_t)blockIdx.x * N + n] = s[n];
}
/* the x86 twin at a runtime size n (power of two, 256..4096): the per-size self-test of the bit-exact FEP (fep_size_ok) */
template <bool INV>
__global__ void __launch_bounds__(THREADS) k_twin_dft_x86n(const short2 *__restrict__ in, short2 *__restrict__ out, const uint16_t *__restrict__ perm,
                                                           const short *__restrict__ blob, int n)
{
  __shared__ short2 s[2 * N];
  const short2 *x = in + (size_t)blockIdx.x * n;
  for (int k = threadIdx.x; k < n; k += THREADS) s[k] = x[__ldg(&perm[k])];
  __syncthreads();
  nr_isa::fft_x86<INV, THREADS>(s, s + n, threadIdx.x, n, (const short2 *)blob);
  for (int k = threadIdx.x; k < n; k += THREADS) out[(size_t)blockIdx.x * n + k] = s[k];
}
template <int R>
__global__ void k_twin_rot(const short2 *__restrict__ x, short2 *__restrict__ y, const short2 *__restrict__ ts, int n, short2 rot)
{
  const int k = blockIdx.x * blockDim.x + threadIdx.x;
  if (k < n) y[k] = rot_ts<R>(x[k], rot.x, nr_isa::xneg(rot.y), ts[k]);
}

/* ISA profile of this process, chosen once (thread-safe) by a self-test of every GPU twin against the host's CPU:
 *   DFT twins (NEON, x86) on NV vectors (random at several amplitudes, full scale, a full-scale tone, an impulse pair,
 *   a constant -32768), forward (dft4096) and inverse (idft4096), vs nr_gpu_isa_cpu_dft(); rotation twins (QRDMX, V80,
 *   x86 madd) on full-scale data, extreme and random rotations, two lengths, vs nr_gpu_isa_cpu_rot().
 * The profile is the pair a CPU build can have: (NEON, QRDMX), (NEON, V80) or (x86, madd). Anything else -- no CPU DFT
 * reachable, no twin bit-identical, a CUDA error -- is P_NONE: the caller refuses the bit-exact GPU paths (CPU path). */
static int g_isa_profile = nr_isa::P_NONE;
static char g_isa_desc[160] = "dft_twin=none rot_twin=none";
static std::once_flag g_isa_once;
static uint32_t isa_rng(uint32_t &st) { st = st * 1664525u + 1013904223u; return st >> 8; }
template <int P>
static int twin_dft_ok(const std::vector<short2> &in, const std::vector<short2> &ref_f, const std::vector<short2> &ref_i, int nv)
{
  std::vector<uint16_t> perm;
  std::vector<short> ta, tb;
  twin_tables(P, perm, ta, tb);
  uint16_t *d_perm = nullptr;
  short *d_ta = nullptr, *d_tb = nullptr;
  short2 *d_in = nullptr, *d_out = nullptr;
  const size_t nb = (size_t)nv * N * sizeof(short2);
  int ok = 0;
  if (cudaMalloc(&d_perm, perm.size() * 2) == cudaSuccess && cudaMalloc(&d_ta, ta.size() * 2) == cudaSuccess
      && cudaMalloc(&d_tb, tb.size() * 2) == cudaSuccess && cudaMalloc(&d_in, nb) == cudaSuccess && cudaMalloc(&d_out, 2 * nb) == cudaSuccess
      && cudaMemcpy(d_perm, perm.data(), perm.size() * 2, cudaMemcpyHostToDevice) == cudaSuccess
      && cudaMemcpy(d_ta, ta.data(), ta.size() * 2, cudaMemcpyHostToDevice) == cudaSuccess
      && cudaMemcpy(d_tb, tb.data(), tb.size() * 2, cudaMemcpyHostToDevice) == cudaSuccess
      && cudaMemcpy(d_in, in.data(), nb, cudaMemcpyHostToDevice) == cudaSuccess) {
    k_twin_dft<P, false><<<nv, THREADS>>>(d_in, d_out, d_perm, d_ta, d_tb);
    k_twin_dft<P, true><<<nv, THREADS>>>(d_in, d_out + (size_t)nv * N, d_perm, d_ta, d_tb);
    std::vector<short2> g(2 * (size_t)nv * N);
    if (cudaGetLastError() == cudaSuccess && cudaMemcpy(g.data(), d_out, 2 * nb, cudaMemcpyDeviceToHost) == cudaSuccess)
      ok = !memcmp(g.data(), ref_f.data(), nb) && !memcmp(g.data() + (size_t)nv * N, ref_i.data(), nb);
  }
  cudaFree(d_perm); cudaFree(d_ta); cudaFree(d_tb); cudaFree(d_in); cudaFree(d_out);
  (void)cudaGetLastError();
  return ok;
}
template <int R>
static int twin_rot_ok(const std::vector<short2> &x, const std::vector<short2> &ts, const short2 *rots, int nrot, const int *lens, int nlen,
                       const std::vector<short2> &ref)
{
  short2 *d_x = nullptr, *d_y = nullptr, *d_ts = nullptr;
  int ok = 0;
  if (cudaMalloc(&d_x, N * sizeof(short2)) == cudaSuccess && cudaMalloc(&d_y, (size_t)nrot * nlen * N * sizeof(short2)) == cudaSuccess
      && cudaMalloc(&d_ts, N * sizeof(short2)) == cudaSuccess && cudaMemcpy(d_x, x.data(), N * sizeof(short2), cudaMemcpyHostToDevice) == cudaSuccess
      && cudaMemcpy(d_ts, ts.data(), N * sizeof(short2), cudaMemcpyHostToDevice) == cudaSuccess) {
    for (int r = 0; r < nrot; r++)
      for (int l = 0; l < nlen; l++) k_twin_rot<R><<<(lens[l] + 255) / 256, 256>>>(d_x, d_y + (size_t)(r * nlen + l) * N, d_ts, lens[l], rots[r]);
    std::vector<short2> g((size_t)nrot * nlen * N);
    if (cudaGetLastError() == cudaSuccess && cudaMemcpy(g.data(), d_y, g.size() * sizeof(short2), cudaMemcpyDeviceToHost) == cudaSuccess) {
      ok = 1;
      for (int r = 0; r < nrot && ok; r++)
        for (int l = 0; l < nlen && ok; l++)
          ok = !memcmp(&g[(size_t)(r * nlen + l) * N], &ref[(size_t)(r * nlen + l) * N], (size_t)lens[l] * sizeof(short2));
    }
  }
  cudaFree(d_x); cudaFree(d_y); cudaFree(d_ts);
  (void)cudaGetLastError();
  return ok;
}
static void isa_select()
{
  const int NV = 8;
  std::vector<short2> in((size_t)NV * N), ref_f(in.size()), ref_i(in.size());
  uint32_t st = 0x15a7u;
  for (int v = 0; v < NV; v++)
    for (int n = 0; n < N; n++) {
      short2 &e = in[(size_t)v * N + n];
      const int amp[4] = {300, 3000, 12000, 32768};
      if (v < 4) e = make_short2((short)((int)(isa_rng(st) % (2u * amp[v])) - amp[v]), (short)((int)(isa_rng(st) % (2u * amp[v])) - amp[v]));
      else if (v == 4) e = make_short2((short)(isa_rng(st) & 0xffff), (short)(isa_rng(st) & 0xffff)); /* full int16 range */
      else if (v == 5) {
        const double a = 2 * M_PI * 37.0 * n / N;
        e = make_short2((short)lrint(32767 * cos(a)), (short)lrint(32767 * sin(a)));
      } else if (v == 6) e = make_short2(n == 0 ? 32767 : n == 1 ? -32768 : 0, n == 0 ? -32768 : n == 2 ? 32767 : 0);
      else e = make_short2(-32768, -32768);
    }
  int cpu = 1;
  for (int v = 0; v < NV && cpu; v++)
    cpu = nr_gpu_isa_cpu_dft(N, 0, (const int16_t *)&in[(size_t)v * N], (int16_t *)&ref_f[(size_t)v * N]) == 0
          && nr_gpu_isa_cpu_dft(N, 1, (const int16_t *)&in[(size_t)v * N], (int16_t *)&ref_i[(size_t)v * N]) == 0;
  int dft = nr_isa::DFT_NONE, rot = nr_isa::P_NONE;
  if (cpu) {
    if (twin_dft_ok<nr_isa::P_NEON_QRDMX>(in, ref_f, ref_i, NV)) dft = nr_isa::DFT_NEON;
    else if (twin_dft_ok<nr_isa::P_X86>(in, ref_f, ref_i, NV)) dft = nr_isa::DFT_X86;
  }
  /* rotation: full-range data, the timeshift-like table, extreme / random rotations, a full and an odd-PRB range length */
  const short2 rots[6] = {make_short2(23170, -23170), make_short2(-32768, -32768), make_short2(32767, 1), make_short2(-12345, 30001),
                          make_short2(1, -32768), make_short2((short)(isa_rng(st) & 0xffff), (short)(isa_rng(st) & 0xffff))};
  const int lens[2] = {N, 1644};
  std::vector<short2> x(N), ts(N), rref((size_t)6 * 2 * N);
  for (int n = 0; n < N; n++) {
    x[n] = make_short2((short)(isa_rng(st) & 0xffff), (short)(isa_rng(st) & 0xffff));
    ts[n] = make_short2((short)(isa_rng(st) & 0xffff), (short)(isa_rng(st) & 0xffff));
  }
  x[0] = make_short2(-32768, -32768);
  ts[0] = make_short2(-32768, -32768);
  for (int r = 0; r < 6; r++)
    for (int l = 0; l < 2; l++)
      nr_gpu_isa_cpu_rot((const int16_t *)x.data(), rots[r].x, rots[r].y, (const int16_t *)ts.data(), (int16_t *)&rref[(size_t)(r * 2 + l) * N], lens[l]);
  if (twin_rot_ok<nr_isa::P_NEON_QRDMX>(x, ts, rots, 6, lens, 2, rref)) rot = nr_isa::P_NEON_QRDMX;
  else if (twin_rot_ok<nr_isa::P_X86>(x, ts, rots, 6, lens, 2, rref)) rot = nr_isa::P_X86;
  else if (twin_rot_ok<nr_isa::P_NEON_V80>(x, ts, rots, 6, lens, 2, rref)) rot = nr_isa::P_NEON_V80;
  int prof = nr_isa::P_NONE;
  if (dft == nr_isa::DFT_NEON && (rot == nr_isa::P_NEON_QRDMX || rot == nr_isa::P_NEON_V80)) prof = rot;
  if (dft == nr_isa::DFT_X86 && rot == nr_isa::P_X86) prof = nr_isa::P_X86;
  g_isa_profile = prof;
  snprintf(g_isa_desc, sizeof(g_isa_desc), "dft_twin=%s rot_twin=%s", nr_isa::dft_name(prof ? dft : nr_isa::DFT_NONE), nr_isa::rot_name(prof ? rot : nr_isa::P_NONE));
  fprintf(stderr, "SENSING: GPU_ISA_TWIN %s (self-test vs the host CPU: dft=%s rot=%s; %d vectors x dft4096+idft4096, 6 rotations x 2 lengths; cpu_dft=%s)%s\n",
          g_isa_desc, nr_isa::dft_name(dft), nr_isa::rot_name(rot), NV, nr_gpu_isa_cpu_dft_origin(),
          prof ? "" : " -- no bit-identical GPU twin: bit-exact GPU FEP/delay/UL FEP refused, CPU path");
}
static int isa_profile()
{
  std::call_once(g_isa_once, isa_select);
  return g_isa_profile;
}
/* fep_size_selftest: the x86 twin at FFT size n (fep_size_ok, n != N) against the host CPU's dft(n), forward, on the vector set of
 * isa_select (random at four amplitudes, full int16 range, a full-scale tone, an impulse pair, constant -32768). 1 = bit-identical on all of
 * them; 0 = mismatch, no CPU dft(n) or a CUDA error (the caller then keeps the CPU FEP). N itself is covered by isa_select. */
static int fep_size_selftest(int n)
{
  if (n == N) return 1;
  if (!fep_size_ok(nr_isa::P_X86, n)) return 0;
  const int NV = 8;
  std::vector<short2> in((size_t)NV * n), ref((size_t)NV * n), g((size_t)NV * n);
  uint32_t st = 0x2b1du + (uint32_t)n;
  for (int v = 0; v < NV; v++)
    for (int k = 0; k < n; k++) {
      short2 &e = in[(size_t)v * n + k];
      const int amp[4] = {300, 3000, 12000, 32768};
      if (v < 4) e = make_short2((short)((int)(isa_rng(st) % (2u * amp[v])) - amp[v]), (short)((int)(isa_rng(st) % (2u * amp[v])) - amp[v]));
      else if (v == 4) e = make_short2((short)(isa_rng(st) & 0xffff), (short)(isa_rng(st) & 0xffff));
      else if (v == 5) {
        const double a = 2 * M_PI * 37.0 * k / n;
        e = make_short2((short)lrint(32767 * cos(a)), (short)lrint(32767 * sin(a)));
      } else if (v == 6) e = make_short2(k == 0 ? 32767 : k == 1 ? -32768 : 0, k == 0 ? -32768 : k == 2 ? 32767 : 0);
      else e = make_short2(-32768, -32768);
    }
  for (int v = 0; v < NV; v++)
    if (nr_gpu_isa_cpu_dft(n, 0, (const int16_t *)&in[(size_t)v * n], (int16_t *)&ref[(size_t)v * n]) != 0) return 0;
  std::vector<uint16_t> perm;
  std::vector<short> ta, tb;
  twin_tables(nr_isa::P_X86, perm, ta, tb, n);
  uint16_t *d_perm = nullptr;
  short *d_ta = nullptr;
  short2 *d_in = nullptr, *d_out = nullptr;
  const size_t nb = (size_t)NV * n * sizeof(short2);
  int ok = 0;
  if (cudaMalloc(&d_perm, perm.size() * 2) == cudaSuccess && cudaMalloc(&d_ta, ta.size() * 2) == cudaSuccess && cudaMalloc(&d_in, nb) == cudaSuccess
      && cudaMalloc(&d_out, nb) == cudaSuccess && cudaMemcpy(d_perm, perm.data(), perm.size() * 2, cudaMemcpyHostToDevice) == cudaSuccess
      && cudaMemcpy(d_ta, ta.data(), ta.size() * 2, cudaMemcpyHostToDevice) == cudaSuccess
      && cudaMemcpy(d_in, in.data(), nb, cudaMemcpyHostToDevice) == cudaSuccess) {
    k_twin_dft_x86n<false><<<NV, THREADS>>>(d_in, d_out, d_perm, d_ta, n);
    if (cudaGetLastError() == cudaSuccess && cudaMemcpy(g.data(), d_out, nb, cudaMemcpyDeviceToHost) == cudaSuccess) ok = !memcmp(g.data(), ref.data(), nb);
  }
  cudaFree(d_perm); cudaFree(d_ta); cudaFree(d_in); cudaFree(d_out);
  (void)cudaGetLastError();
  fprintf(stderr, "SENSING: GPU_ISA_TWIN size self-test N=%d dft_twin=avx2: %s (%d vectors vs the host CPU dft%d)\n", n, ok ? "bit-identical" : "MISMATCH -- CPU FEP", NV, n);
  return ok;
}
} // namespace nr_exact

/* Launch KERN<profile> (a template kernel of the bit-exact set) with the profile chosen at init; P_NONE launches nothing
 * (callers never get there: the exact working set is not created without a profile). */
#define NR_EXACT_LAUNCH(PROF, KERN, GRID, BLOCK, STREAM, ...)                                                   \
  do {                                                                                                          \
    switch (PROF) {                                                                                             \
      case nr_isa::P_NEON_QRDMX: KERN<nr_isa::P_NEON_QRDMX><<<GRID, BLOCK, 0, STREAM>>>(__VA_ARGS__); break;    \
      case nr_isa::P_NEON_V80: KERN<nr_isa::P_NEON_V80><<<GRID, BLOCK, 0, STREAM>>>(__VA_ARGS__); break;        \
      case nr_isa::P_X86: KERN<nr_isa::P_X86><<<GRID, BLOCK, 0, STREAM>>>(__VA_ARGS__); break;                  \
      default: break;                                                                                           \
    }                                                                                                           \
  } while (0)
/* cudaFuncGetAttributes of KERN<profile> (first-use module load before the RX fence) */
#define NR_EXACT_ATTR(PROF, ATTR, KERN)                                                                         \
  ((PROF) == nr_isa::P_NEON_QRDMX ? cudaFuncGetAttributes(ATTR, KERN<nr_isa::P_NEON_QRDMX>)                    \
   : (PROF) == nr_isa::P_NEON_V80 ? cudaFuncGetAttributes(ATTR, KERN<nr_isa::P_NEON_V80>)                      \
   : (PROF) == nr_isa::P_X86      ? cudaFuncGetAttributes(ATTR, KERN<nr_isa::P_X86>)                           \
                                  : cudaErrorInvalidValue)
#endif
