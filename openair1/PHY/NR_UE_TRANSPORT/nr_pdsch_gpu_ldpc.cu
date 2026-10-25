/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* DECGPU exact LDPC, module side (part of libpdsch_gpu.so); see nr_pdsch_gpu_ldpc.h.
 *
 * Same arithmetic as OAI's nrLDPC_decoder.c on aarch64, per edge (the CPU only adds a memory layout around it):
 *   CN  (nrLDPC_cnProc_group_2pass_128): a = |v| as uint8 (|-128| = 128); min1/min2 over the row with
 *       min2 = min(min2, max(min1_old, a)); out = min(a == min1 ? min2 : min1, 127), negated when the xor of the
 *       other inputs is negative; min1 = min2 = 127 at the start.
 *   BN  (nrLDPC_bnProcPc): llrRes = sat8(llr + sum of the CN messages), the sum in int16 (never saturates: <= 31 terms).
 *   v2c (nrLDPC_bnProc):   subs8(llrRes, c2v).
 * The CPU keeps v2c in cnProcBuf; here v2c is formed on the fly as subs8(llrRes_prev, c2v_prev), which is the same
 * value (with c2v = 0 and llrRes = llr before the first iteration it is llr itself, OAI's llr2CnProcBuf).
 * One launch per batch, one 8-block cluster per code block, cluster barriers between the phases. */
#include "nr_pdsch_gpu_ldpc.h"
#include "nr_pdsch_gpu_fep.h" // NR_GPU_PASS_RING
#include "../TOOLS/nr_gpu_poll.h" // P99: ISAC_GPU_POLL_SLEEP_US
#include <cooperative_groups.h>
#include <cuda_runtime.h>
#include "nr_gpu_green.h"
#include "../TOOLS/nr_gpu_cuda.h" // nr_gpu_module_ready
#include "nr_pusch_gpu_diag.h"
#include "PHY/TOOLS/nr_gpu_mem.h"
#include "nr_gpu_stage_census.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#include "../CODING/nrLDPC_cuda/ldpc_tables_bg1.h"
#include "../CODING/nrLDPC_cuda/ldpc_tables_bg2.h"

namespace cg = cooperative_groups;

namespace nrldpc {
constexpr int MAXE = 316, MAXR = 46, MAXC = 68, ZMAX = 384, NJ = NR_GPU_LDPC_MAX_JOBS, NJP = NR_GPU_PASS_LDPC_JOBS, NIT = NR_GPU_LDPC_MAX_SNAP + 3;
constexpr int CTX = 8, THREADS = 512, CLB = 8; /* CLB: blocks per code block = cluster size (portable max) */

struct Graph {         /* rows 0..M-1 of the base graph (the rows nrLDPC_init() uses for R), lifted by Z */
  int M, ncol, E, Z;
  int16_t rs[MAXR + 1];  /* row r: edges rs[r] .. rs[r+1]-1 */
  uint16_t ecol[MAXE];   /* column of edge e */
  uint16_t esh[MAXE];    /* shift V mod Z of edge e: CN (r, z) <-> BN (ecol, (z + esh) mod Z) */
  int16_t cs[MAXC + 1];  /* column c: cedge[cs[c] .. cs[c+1]-1] */
  uint16_t cedge[MAXE];
  uint8_t efz[MAXE];     /* 1: OAI's bn2cnProcBuf never refreshes this edge (last BN of a CN group of degree < 19 (BG1) or
                            not 8 / 10 (BG2): the single-edge parity column), so its BN->CN message stays the channel LLR */
};

struct DJob { /* device view of one code block */
  const Graph *g;
  const int8_t *llr_in; /* pinned staging, Kc*Z */
  int K, it_max;
  unsigned long long *ts; /* measurement only (graph-pass instances; else null): [0] %globaltimer at the code block's entry, [1] at its stop */
};
__device__ __forceinline__ unsigned long long gtimer()
{
  unsigned long long t;
  asm volatile("mov.u64 %0, %%globaltimer;" : "=l"(t));
  return t;
}

struct Ctx {
  cudaStream_t s;
  int cap;             /* code blocks per launch: NJP for a graph-pass instance, NJ otherwise */
  DJob *dj;            /* pinned mapped [cap] */
  int8_t *in;          /* pinned [NJ][MAXC*ZMAX] */
  uint8_t *snap;       /* pinned mapped [NJ][NR_GPU_LDPC_MAX_SNAP][KB_MAX] */
  int *res;            /* pinned mapped [NJ][2]: nsnap, syn0 */
  int8_t *llr, *llrRes, *c2v; /* device [NJ][MAXC*ZMAX], [NJ][MAXC*ZMAX], [NJ][MAXE*ZMAX] */
  int *unsat;          /* device [NJ][NIT] */
  unsigned long long *ts; /* graph-pass instance: the bits buffer's NR_GPU_PASS_LDPC_TS_OFF (set by dev_impl); null otherwise */
  nr_gpu_buf_t sin, ssnap, sres, sdj; /* Q4, DISCRETE tier: device-only copies of in / snap / res / dj (the kernel reads and writes these; in / snap / res / dj above are then the pinned mirrors) */
};
static Ctx g_ctx[CTX + NR_GPU_PASS_RING + 1]; /* [CTX + i]: the GPUPASS context of graph-pass instance i (nr_pdsch_gpu_ldpc_dev: the extra context [CTX + RING]), never handed out by acquire() */
static std::atomic<unsigned> g_busy{0};
static int g_ok;
static std::mutex g_gm;
static std::map<int, Graph *> g_graphs; /* (BG, Z, R) -> device graph */

__device__ __forceinline__ int8_t sat8(int v) { return (int8_t)(v > 127 ? 127 : v < -128 ? -128 : v); }

/* sum of the CN messages of one BN (column col at z), D >= its degree, branch-free loads */
template <int D> __device__ __forceinline__ int bn_sum(const Graph &G, int Z, int z, int k0, int deg, const int8_t *c2v)
{
  int8_t m[D];
#pragma unroll
  for (int k = 0; k < D; k++) {
    const int e = G.cedge[k0 + (k < deg ? k : 0)];
    int zc = z - G.esh[e];
    zc += zc < 0 ? Z : 0;
    m[k] = __ldcg(&c2v[e * Z + zc]); /* written by other blocks: read from L2, never a stale L1 line */
  }
  int s = 0;
#pragma unroll
  for (int k = 0; k < D; k++)
    s += k < deg ? m[k] : 0;
  return s;
}

/* one CN (row r of the lifted graph at z), D >= its degree: loads first and branch-free (inactive slots reload edge 0)
 * so they are in flight together; returns the parity of the hard decisions it read (iteration it - 1) */
template <int D>
__device__ __forceinline__ unsigned cn_item(const Graph &G, int Z, int z, int e0, int deg, const int8_t *res, const int8_t *llr, int8_t *c2v)
{
    int8_t rr[D], cv[D], lv[D], v[D];
#pragma unroll
    for (int k = 0; k < D; k++) {
      const int e = e0 + (k < deg ? k : 0);
      int zb = z + G.esh[e];
      zb -= zb >= Z ? Z : 0;
      const int n = G.ecol[e] * Z + zb;
      rr[k] = __ldcg(&res[n]); /* written by other blocks */
      lv[k] = llr[n];
      cv[k] = c2v[e * Z + z];
    }
    unsigned par = 0; /* parity check of row r at z on the hard decisions of iteration it - 1 */
#pragma unroll
    for (int k = 0; k < D; k++)
      if (k < deg) {
        par ^= rr[k] < 0 ? 1u : 0u;
        v[k] = G.efz[e0 + k] ? lv[k] : sat8((int)rr[k] - (int)cv[k]);
      }
    unsigned m1 = 127, m2 = 127, sx = 0;
#pragma unroll
    for (int k = 0; k < D; k++)
      if (k < deg) {
        const unsigned a = v[k] < 0 ? (unsigned)(-(int)v[k]) : (unsigned)v[k]; /* 128 for -128, as abs_epi8 read unsigned */
        const unsigned n1 = min(m1, a), n2 = min(m2, max(m1, a));
        m1 = n1;
        m2 = n2;
        sx ^= (uint8_t)v[k];
      }
#pragma unroll
    for (int k = 0; k < D; k++)
      if (k < deg) {
        const unsigned a = v[k] < 0 ? (unsigned)(-(int)v[k]) : (unsigned)v[k];
        const int mag = (int)min(a == m1 ? m2 : m1, 127u);
        const bool neg = ((sx ^ (uint8_t)v[k]) & 0x80) != 0;
        c2v[(e0 + k) * Z + z] = (int8_t)(neg ? -mag : mag);
      }
  return par;
}

/* Block rank / barrier of one code block's blocks. Cluster (NC = false, sm_90+): CLB blocks, hardware cluster barrier. Non-cluster (NC = true,
 * k_ldpc_nc / k_ldpc4_nc): ONE block per code block, __syncthreads; the shared data was already in global memory (g_llr / g_res / g_c2v, read
 * with __ldcg), only the work split (gs) changes, and every phase is order-independent integer arithmetic, so the result is bit-identical. */
template <int CB> __device__ __forceinline__ int blk_rank()
{
  if constexpr (CB == 1)
    return 0;
  else {
#if defined(__CUDA_ARCH__) && __CUDA_ARCH__ < 900
    __trap(); /* thread-block clusters need sm_90+; the host picks the NC kernels below sm_90 */
    return 0;
#else
    return (int)cg::this_cluster().block_rank();
#endif
  }
}
template <int CB> __device__ __forceinline__ void blk_bar()
{
  if constexpr (CB == 1)
    __syncthreads();
  else {
#if defined(__CUDA_ARCH__) && __CUDA_ARCH__ < 900
    __trap();
#else
    cg::this_cluster().sync();
#endif
  }
}

/* One thread-block cluster (CLB blocks) per code block (NC: one block): the phases are separated by cluster barriers (hardware, cheap,
 * no grid-wide residency like a cooperative launch), cross-block data is read from L2 (__ldcg). */
template <int CB> __global__ void __launch_bounds__(THREADS) k_ldpc_t(const DJob *jobs, int8_t *g_llr, int8_t *g_res, int8_t *g_c2v, int *g_unsat,
                                                  uint8_t *g_snap, int *g_out)
{
  __shared__ Graph G;
  const int c = blockIdx.x / CB, b = blk_rank<CB>(), t = threadIdx.x;
  const DJob J = jobs[c];
  if (J.ts && b == 0 && t == 0) J.ts[0] = gtimer();
  if (J.K == 0) { /* a padding block of the shape class (pass_set): every block of its cluster reads the same job, so all return here, before any cluster barrier */
    if (b == 0 && t == 0) { g_out[2 * c] = 0; g_out[2 * c + 1] = 1; if (J.ts) J.ts[1] = gtimer(); }
    return;
  }
  {
    const int *src = (const int *)J.g;
    int *dst = (int *)&G;
    for (int i = t; i < (int)(sizeof(Graph) / sizeof(int)); i += THREADS)
      dst[i] = src[i];
  }
  __syncthreads();
  const int Z = G.Z, M = G.M, ncol = G.ncol, gt = b * THREADS + t, gs = CB * THREADS, itm = J.it_max;
  int8_t *llr = g_llr + (size_t)c * MAXC * ZMAX, *res = g_res + (size_t)c * MAXC * ZMAX, *c2v = g_c2v + (size_t)c * MAXE * ZMAX;
  int *unsat = g_unsat + c * NIT;
  const int KB = (J.K + 7) >> 3, ktail = J.K & 7; /* ktail: valid bits of the last byte (0 = full) */
  uint8_t *snap = g_snap + (size_t)c * NR_GPU_LDPC_MAX_SNAP * NR_GPU_LDPC_KB_MAX;
  for (int n = gt; n < ncol * Z; n += gs) {
    const int8_t v = J.llr_in[n];
    llr[n] = v;
    res[n] = v;
  }
  for (int n = gt; n < G.E * Z; n += gs)
    c2v[n] = 0;
  for (int n = gt; n < NIT; n += gs)
    unsat[n] = 0;
  blk_bar<CB>();
  /* Per iteration it: CN (+ the syndrome of iteration it - 1, from the same reads), barrier, verdict on it - 1 (every
   * thread reads the same flag, so the exit is uniform), BN (+ the snapshot of it), barrier. */
  for (int it = 1;; it++) {
    if (it <= itm + 1) { /* ---- CN phase: one (row, z) per work item ---- */
      unsigned bad = 0;
      for (int w = gt; w < M * Z; w += gs) {
        const int r = w / Z, z = w - r * Z, e0 = G.rs[r], deg = G.rs[r + 1] - e0;
        unsigned par;
        if (deg <= 4)
          par = cn_item<4>(G, Z, z, e0, deg, res, llr, c2v);
        else if (deg <= 10)
          par = cn_item<10>(G, Z, z, e0, deg, res, llr, c2v);
        else
          par = cn_item<19>(G, Z, z, e0, deg, res, llr, c2v);
        bad |= par;
      }
      if (it - 1 >= 3 && __syncthreads_or(bad) && t == 0)
        atomicOr(&unsat[it - 1], 1);
    }
    blk_bar<CB>();
    const int p = it - 1; /* verdict on iteration p */
    if (p >= 1) {
      const bool last = p >= itm + 1;
      const bool syn0 = p >= 3 && __ldcg(&unsat[p]) == 0;
      if (syn0 || last) {
        if (gt == 0) {
          g_out[2 * c] = p >= 3 ? p - 2 : 0;
          g_out[2 * c + 1] = syn0 && !last;
          if (J.ts) J.ts[1] = gtimer();
        }
        break;
      }
    }
    /* ---- BN phase: one (column, z) per work item; the first K hard decisions packed MSB first ---- */
    for (int w0 = gt - (gt & 31); w0 < ncol * Z; w0 += gs) { /* warp-uniform trip count for the ballot */
      const int w = w0 + (t & 31);
      int8_t rv = 0;
      if (w < ncol * Z) {
        const int col = w / Z, z = w - col * Z;
        int s = llr[w];
        const int k0 = G.cs[col], deg = G.cs[col + 1] - k0;
        s += deg == 1 ? bn_sum<1>(G, Z, z, k0, deg, c2v) : deg <= 8 ? bn_sum<8>(G, Z, z, k0, deg, c2v)
                                  : deg <= 16 ? bn_sum<16>(G, Z, z, k0, deg, c2v) : bn_sum<30>(G, Z, z, k0, deg, c2v);
        rv = sat8(s);
        res[w] = rv;
      }
      if (it >= 3 && w0 < J.K) {
        const unsigned m = __ballot_sync(0xffffffffu, rv < 0);
        if ((t & 31) == 0) {
          const unsigned word = __byte_perm(__brev(m), 0, 0x0123); /* byte j = lanes 8j..8j+7, lane 8j at bit 7 */
          uint8_t *o = snap + (size_t)(it - 3) * NR_GPU_LDPC_KB_MAX + (w0 >> 3);
          for (int q = 0; q < 4 && (w0 >> 3) + q < KB; q++)
            o[q] = (uint8_t)(word >> (8 * q)) & ((w0 >> 3) + q == KB - 1 && ktail ? 0xff << (8 - ktail) : 0xff); /* tail bits zero */
        }
      }
    }
    blk_bar<CB>();
  }
}

/* ---- 4 lifting indices per thread (Z % 4 == 0): the same per-byte arithmetic with the SIMD-in-word intrinsics --------
 * __vsubss4 = subs_epi8, abs4 (|-128| stays 0x80, read unsigned as 128) = abs_epi8, __vminu4 / __vmaxu4 = min/max_epu8,
 * __vcmpeq4 = cmpeq_epi8. BN sums in 16-bit lanes (never overflow), saturated to int8 = packs_epi16. */
/* bytes 0, 2 (lo) or 1, 3 (hi) of x sign-extended into the two 16-bit lanes */
__device__ __forceinline__ uint32_t sx_lo(uint32_t x) { return __vsub2((x & 0x00ff00ffu) ^ 0x00800080u, 0x00800080u); }
__device__ __forceinline__ uint32_t sx_hi(uint32_t x) { return __vsub2((x >> 8 & 0x00ff00ffu) ^ 0x00800080u, 0x00800080u); }
/* bytes row[(start + i) mod Z], i = 0..3, Z % 4 == 0; branch-free (three loads always issued, so an unrolled run of
 * them is in flight together): two aligned words around start, and the row head for the wrap. Reads up to 7 bytes
 * past the row (the buffers have slack). */
template <int SM = 0> /* SM: row lives in this block's shared memory (plain loads; __ldcg is global-only) */
__device__ __forceinline__ uint32_t ld4(const int8_t *row, int start, int Z)
{
  const int a = start & ~3, sh = start & 3;
  const uint32_t w0 = SM ? *(const unsigned int *)(row + a) : __ldcg((const unsigned int *)(row + a)),
                 w1 = SM ? *(const unsigned int *)(row + a + 4) : __ldcg((const unsigned int *)(row + a + 4)),
                 w2 = SM ? *(const unsigned int *)row : __ldcg((const unsigned int *)row);
  const uint32_t u = __funnelshift_r(w0, w1, 8 * sh);
  const int n = Z - start; /* valid bytes before the wrap, < 4 only when wrapping */
  return n >= 4 ? u : (u & ((1u << (8 * n)) - 1u)) | (w2 << (8 * n));
}
/* |v| per byte with |-128| = 0x80 (abs_epi8 read unsigned) */
__device__ __forceinline__ uint32_t abs4(uint32_t v)
{
  const uint32_t m = __vcmplts4(v, 0u);
  return (__vneg4(v) & m) | (v & ~m);
}
template <int D, int SM = 0>
__device__ __forceinline__ uint32_t cn_item4(const Graph &G, int Z, int z, int e0, int deg, const int8_t *res, const int8_t *llr, int8_t *c2v)
{
  uint32_t rr[D], cv[D], v[D];
#pragma unroll
  for (int k = 0; k < D; k++) {
    const int e = e0 + (k < deg ? k : 0);
    int zb = z + G.esh[e];
    zb -= zb >= Z ? Z : 0;
    rr[k] = ld4<SM>(res + G.ecol[e] * Z, zb, Z); /* written by other blocks: L2 */
    cv[k] = *(const uint32_t *)(c2v + e * Z + z);
  }
  uint32_t par = 0, m1 = 0x7f7f7f7fu, m2 = 0x7f7f7f7fu, sx = 0;
#pragma unroll
  for (int k = 0; k < D; k++)
    if (k < deg) {
      par ^= rr[k];
      if (G.efz[e0 + k]) {
        int zb = z + G.esh[e0 + k];
        zb -= zb >= Z ? Z : 0;
        v[k] = ld4(llr + G.ecol[e0 + k] * Z, zb, Z);
      } else {
        v[k] = __vsubss4(rr[k], cv[k]);
      }
      const uint32_t a = abs4(v[k]);
      const uint32_t n1 = __vminu4(m1, a), n2 = __vminu4(m2, __vmaxu4(m1, a));
      m1 = n1;
      m2 = n2;
      sx ^= v[k];
    }
#pragma unroll
  for (int k = 0; k < D; k++)
    if (k < deg) {
      const uint32_t a = abs4(v[k]);
      const uint32_t eq = __vcmpeq4(a, m1);
      const uint32_t mag = __vminu4((m2 & eq) | (m1 & ~eq), 0x7f7f7f7fu);
      const uint32_t s = sx ^ v[k];
      const uint32_t neg = __vcmpne4(s & 0x80808080u, 0u); /* 0xff where the other inputs' sign is negative */
      *(uint32_t *)(c2v + (e0 + k) * Z + z) = (__vneg4(mag) & neg) | (mag & ~neg);
    }
  return par & 0x80808080u; /* per-byte parity of the hard decisions read */
}
template <int D> __device__ __forceinline__ void bn_sum4(const Graph &G, int Z, int z, int k0, int deg, const int8_t *c2v, uint32_t &lo, uint32_t &hi)
{
  uint32_t m[D];
#pragma unroll
  for (int k = 0; k < D; k++) {
    const int e = G.cedge[k0 + (k < deg ? k : 0)];
    int zc = z - G.esh[e];
    zc += zc < 0 ? Z : 0;
    m[k] = ld4(c2v + e * Z, zc, Z);
  }
#pragma unroll
  for (int k = 0; k < D; k++)
    if (k < deg) {
      lo = __vadd2(lo, sx_lo(m[k]));
      hi = __vadd2(hi, sx_hi(m[k]));
    }
}

template <int CB, int SM = 0> /* SM = 1: the hard-decision state res in shared memory (CB == 1 only) */
__global__ void __launch_bounds__(THREADS) k_ldpc4_t(const DJob *jobs, int8_t *g_llr, int8_t *g_res, int8_t *g_c2v, int *g_unsat,
                                                   uint8_t *g_snap, int *g_out)
{
  static_assert(SM == 0 || CB == 1, "shared-memory state needs one block per code block");
  __shared__ Graph G;
  __shared__ __align__(16) int8_t sres[SM ? MAXC * ZMAX + 16 : 1];
  const int c = blockIdx.x / CB, b = blk_rank<CB>(), t = threadIdx.x;
  const DJob J = jobs[c];
  if (J.ts && b == 0 && t == 0) J.ts[0] = gtimer();
  if (J.K == 0) { /* a padding block of the shape class (pass_set): every block of its cluster reads the same job, so all return here, before any cluster barrier */
    if (b == 0 && t == 0) { g_out[2 * c] = 0; g_out[2 * c + 1] = 1; if (J.ts) J.ts[1] = gtimer(); }
    return;
  }
  {
    const int *src = (const int *)J.g;
    int *dst = (int *)&G;
    for (int i = t; i < (int)(sizeof(Graph) / sizeof(int)); i += THREADS)
      dst[i] = src[i];
  }
  __syncthreads();
  const int Z = G.Z, Z4 = Z >> 2, M = G.M, ncol = G.ncol, gt = b * THREADS + t, gs = CB * THREADS, itm = J.it_max;
  int8_t *llr = g_llr + (size_t)c * MAXC * ZMAX, *res = SM ? sres : g_res + (size_t)c * MAXC * ZMAX, *c2v = g_c2v + (size_t)c * MAXE * ZMAX;
  int *unsat = g_unsat + c * NIT;
  const int KB = J.K >> 3; /* Z % 4 == 0: K % 8 == 0 */
  uint8_t *snap = g_snap + (size_t)c * NR_GPU_LDPC_MAX_SNAP * NR_GPU_LDPC_KB_MAX;
  for (int n = gt; n < (ncol * Z) >> 2; n += gs) {
    const uint32_t v = ((const uint32_t *)J.llr_in)[n];
    ((uint32_t *)llr)[n] = v;
    ((uint32_t *)res)[n] = v;
  }
  for (int n = gt; n < (G.E * Z) >> 2; n += gs)
    ((uint32_t *)c2v)[n] = 0;
  for (int n = gt; n < NIT; n += gs)
    unsat[n] = 0;
  blk_bar<CB>();
  for (int it = 1;; it++) {
    if (it <= itm + 1) { /* ---- CN phase: one (row, 4 z) per work item ---- */
      uint32_t bad = 0;
      for (int w = gt; w < M * Z4; w += gs) {
        const int r = w / Z4, z = (w - r * Z4) << 2, e0 = G.rs[r], deg = G.rs[r + 1] - e0;
        if (deg <= 4)
          bad |= cn_item4<4, SM>(G, Z, z, e0, deg, res, llr, c2v);
        else if (deg <= 10)
          bad |= cn_item4<10, SM>(G, Z, z, e0, deg, res, llr, c2v);
        else
          bad |= cn_item4<19, SM>(G, Z, z, e0, deg, res, llr, c2v);
      }
      if (it - 1 >= 3 && __syncthreads_or(bad != 0) && t == 0)
        atomicOr(&unsat[it - 1], 1);
    }
    blk_bar<CB>();
    const int p = it - 1;
    if (p >= 1) {
      const bool last = p >= itm + 1;
      const bool syn0 = p >= 3 && __ldcg(&unsat[p]) == 0;
      if (syn0 || last) {
        if (gt == 0) {
          g_out[2 * c] = p >= 3 ? p - 2 : 0;
          g_out[2 * c + 1] = syn0 && !last;
          if (J.ts) J.ts[1] = gtimer();
        }
        break;
      }
    }
    /* ---- BN phase: one (column, 4 z) per work item; snapshot: lane pairs build one MSB-first byte ---- */
    for (int w0 = gt - (gt & 31); w0 < ncol * Z4; w0 += gs) { /* warp-uniform trip count (shuffle below) */
      const int w = w0 + (t & 31);
      uint32_t rv = 0;
      if (w < ncol * Z4) {
        const int col = w / Z4, z = (w - col * Z4) << 2;
        const uint32_t l = ((const uint32_t *)llr)[w];
        uint32_t lo = sx_lo(l), hi = sx_hi(l);
        const int k0 = G.cs[col], deg = G.cs[col + 1] - k0;
        if (deg == 1)
          bn_sum4<1>(G, Z, z, k0, deg, c2v, lo, hi);
        else if (deg <= 8)
          bn_sum4<8>(G, Z, z, k0, deg, c2v, lo, hi);
        else if (deg <= 16)
          bn_sum4<16>(G, Z, z, k0, deg, c2v, lo, hi);
        else
          bn_sum4<30>(G, Z, z, k0, deg, c2v, lo, hi);
        lo = __vmaxs2(__vmins2(lo, 0x007f007fu), 0xff80ff80u); /* packs_epi16 */
        hi = __vmaxs2(__vmins2(hi, 0x007f007fu), 0xff80ff80u);
        rv = __byte_perm(lo, hi, 0x6240); /* bytes: lo.0, hi.0, lo.2, hi.2 */
        ((uint32_t *)res)[w] = rv;
      }
      if (it >= 3 && 4 * w0 < J.K) { /* positions 4w .. 4w+3 as an MSB-first nibble, lane pairs make one byte */
        const uint32_t sg = rv & 0x80808080u;
        const uint32_t nib = (sg >> 4 & 8) | (sg >> 13 & 4) | (sg >> 22 & 2) | (sg >> 31);
        const uint32_t nx = __shfl_down_sync(0xffffffffu, nib, 1);
        if (!(t & 1) && w < ncol * Z4 && (w >> 1) < KB)
          snap[(size_t)(it - 3) * NR_GPU_LDPC_KB_MAX + (w >> 1)] = (uint8_t)(nib << 4 | nx);
      }
    }
    blk_bar<CB>();
  }
}

static int ils_of(int Z)
{
  static const int a[8] = {2, 3, 5, 7, 9, 11, 13, 15};
  for (int i = 0; i < 8; i++)
    if (Z % a[i] == 0) {
      const int q = Z / a[i];
      if ((q & (q - 1)) == 0)
        return i;
    }
  return -1;
}

/* host-built graph for (BG, Z, R), cached in device memory */
static const Graph *graph(int BG, int Z, int R)
{
  const int key = (BG << 16) | (Z << 7) | R;
  std::lock_guard<std::mutex> lk(g_gm);
  auto f = g_graphs.find(key);
  if (f != g_graphs.end())
    return f->second;
  const int ils = ils_of(Z);
  int ncol;
  if (BG == 1)
    ncol = R == 13 ? 68 : R == 23 ? 35 : R == 89 ? 27 : 0;
  else
    ncol = R == 15 ? 52 : R == 13 ? 32 : R == 23 ? 17 : 0;
  if (ils < 0 || !ncol || Z > ZMAX)
    return nullptr;
  Graph h;
  memset(&h, 0, sizeof(h));
  h.Z = Z;
  h.ncol = ncol;
  h.M = ncol - (BG == 1 ? 22 : 10);
  static const uint32_t *const d1[8] = {bg1_cn_degree_0, bg1_cn_degree_1, bg1_cn_degree_2, bg1_cn_degree_3,
                                        bg1_cn_degree_4, bg1_cn_degree_5, bg1_cn_degree_6, bg1_cn_degree_7};
  static const uint32_t *const d2[8] = {bg2_cn_degree_0, bg2_cn_degree_1, bg2_cn_degree_2, bg2_cn_degree_3,
                                        bg2_cn_degree_4, bg2_cn_degree_5, bg2_cn_degree_6, bg2_cn_degree_7};
  int e = 0;
  for (int r = 0; r < h.M; r++) {
    h.rs[r] = (int16_t)e;
    const int deg = (int)(BG == 1 ? d1[ils][r] : d2[ils][r]);
    for (int k = 0; k < deg; k++) {
      int vn, s;
#define PICK(T) do { vn = T[r][k].vn; s = T[r][k].s; } while (0)
      if (BG == 1) {
        switch (ils) {
          case 0: PICK(bg1_cn_0); break; case 1: PICK(bg1_cn_1); break; case 2: PICK(bg1_cn_2); break;
          case 3: PICK(bg1_cn_3); break; case 4: PICK(bg1_cn_4); break; case 5: PICK(bg1_cn_5); break;
          case 6: PICK(bg1_cn_6); break; default: PICK(bg1_cn_7); break;
        }
      } else {
        switch (ils) {
          case 0: PICK(bg2_cn_0); break; case 1: PICK(bg2_cn_1); break; case 2: PICK(bg2_cn_2); break;
          case 3: PICK(bg2_cn_3); break; case 4: PICK(bg2_cn_4); break; case 5: PICK(bg2_cn_5); break;
          case 6: PICK(bg2_cn_6); break; default: PICK(bg2_cn_7); break;
        }
      }
#undef PICK
      if (vn >= ncol) /* cannot happen for the R-truncated rows; refuse rather than mis-decode */
        return nullptr;
      h.ecol[e] = (uint16_t)vn;
      h.esh[e] = (uint16_t)(s % Z);
      e++;
    }
    /* nrLDPC_bn2cnProcBuf_BG1/BG2 skip the last BN of the CN groups other than 19 (BG1) / 8, 10 (BG2) */
    const bool full = BG == 1 ? deg == 19 : (deg == 8 || deg == 10);
    if (!full) {
      int last = h.rs[r];
      for (int k = h.rs[r]; k < e; k++)
        last = h.ecol[k] > h.ecol[last] ? k : last;
      h.efz[last] = 1;
    }
  }
  h.rs[h.M] = (int16_t)e;
  h.E = e;
  int n = 0;
  for (int col = 0; col < ncol; col++) {
    h.cs[col] = (int16_t)n;
    for (int k = 0; k < e; k++)
      if (h.ecol[k] == col)
        h.cedge[n++] = (uint16_t)k;
  }
  h.cs[ncol] = (int16_t)n;
  Graph *d = nullptr;
  if (cudaMalloc(&d, sizeof(Graph)) != cudaSuccess || cudaMemcpy(d, &h, sizeof(Graph), cudaMemcpyHostToDevice) != cudaSuccess)
    return nullptr;
  g_graphs[key] = d;
  return d;
}

/* GPUPASS: lookup only. warm_common() (run by nr_pdsch_gpu_ldpc_init, before the RX fence) builds every valid (BG, Z, R) graph; the launch-only
 * entry never builds, allocates or copies: a miss is counted and refused. */
static std::atomic<unsigned> g_dev_miss{0};
static const Graph *graph_lookup(int BG, int Z, int R)
{
  std::lock_guard<std::mutex> lk(g_gm);
  auto f = g_graphs.find((BG << 16) | (Z << 7) | R);
  if (f != g_graphs.end()) return f->second;
  g_dev_miss.fetch_add(1);
  return nullptr;
}

static int acquire(int limit = CTX) /* limit: Q4, the staged DISCRETE run only owns device staging on the first `limit` contexts */
{
  const uint64_t ss0 = nr_ss_now();
  for (;;) {
    unsigned m = g_busy.load();
    for (int i = 0; i < nr_gpu_ctx_limit(limit); i++)
      if (!(m >> i & 1u) && g_busy.compare_exchange_strong(m, m | 1u << i)) {
        nr_ss_add(NRSS_LK, ss0);
        return i;
      }
    std::this_thread::yield();
  }
}

/* ---- Q4: DISCRETE staging of the standalone LDPC runs (DL pool and UL pool) ------------------------------------------------------------------------
 * On DIRECT the kernel reads the code blocks' LLRs from, and writes its snapshots / results to, mapped pinned host memory (zero-copy). On DISCRETE every PCIe access of the
 * kernel would be a remote read or write, so the context's pinned buffers become mirrors of device buffers: the used LLR bytes of the jobs (packed, kc * Z each) and the job
 * descriptors go over in one async copy each, the kernel runs on the device copies, the result words and the snapshots (width (K + 7) / 8, it_max - 1 rows per job: the most the
 * kernel can write) come back in async copies on the same stream; the host then reads the mirrors exactly as it read the mapped buffers. */
static bool g_stg;      /* the standalone pools are staged (DISCRETE tier at init) */
static int g_stg_n;     /* DL contexts with staging buffers (the others are never handed out on DISCRETE) */
/* plain cudaMalloc like the context's own work buffers (x.llr / c2v): the UL pool initialises lazily, on the first PUSCH TB, which can be after the
 * allocation fence of nr_gpu_buf_alloc* closed (receiver running) -- the fenced allocator refused it there (sens6 rfsim, 2026-10-24) */
static bool dev_zalloc(nr_gpu_buf_t *b, size_t bytes)
{
  memset(b, 0, sizeof(*b));
  if (cudaMalloc(&b->dev, bytes) != cudaSuccess || cudaMemset(b->dev, 0, bytes) != cudaSuccess) {
    if (b->dev) cudaFree(b->dev);
    b->dev = nullptr;
    return false;
  }
  b->bytes = bytes;
  b->model = NR_GPU_MODEL_DISCRETE;
  return true;
}
static bool stage_alloc(Ctx &x, int cap, int cio)
{
  return dev_zalloc(&x.sin, (size_t)cap * MAXC * ZMAX + 16) && dev_zalloc(&x.ssnap, (size_t)cio * NR_GPU_LDPC_MAX_SNAP * NR_GPU_LDPC_KB_MAX)
         && dev_zalloc(&x.sres, sizeof(int) * 2 * (size_t)cio) && dev_zalloc(&x.sdj, sizeof(DJob) * (size_t)cap);
}
static size_t stage_ctx_bytes(int cap, int cio)
{
  return (size_t)cap * MAXC * ZMAX + 16 + (size_t)cio * NR_GPU_LDPC_MAX_SNAP * NR_GPU_LDPC_KB_MAX + sizeof(int) * 2 * (size_t)cio + sizeof(DJob) * (size_t)cap;
}
static cudaError_t ldpc_launch(int nc, int n, cudaStream_t st, bool z4, const DJob *ddj, Ctx &x, uint8_t *dsnap, int *dres);
/* packs jobs[0..n) into the context's mirrors, enqueues H2D, kernel, D2H on x.s; *h2d / *d2h = bytes, *ncopy = copies */
static cudaError_t stage_launch(int nc, Ctx &x, const nr_gpu_ldpc_job_t *jobs, int n, const Graph *const *gr, bool z4, uint64_t *h2d, uint64_t *d2h, int *ncopy)
{
  size_t off = 0;
  int8_t *din = (int8_t *)x.sin.dev;
  for (int k = 0; k < n; k++) {
    const size_t bytes = (size_t)(jobs[k].BG == 1 ? 68 : 52) * jobs[k].Z;
    memcpy(x.in + off, jobs[k].llr, bytes);
    x.dj[k] = DJob{gr[k], din + off, jobs[k].K, jobs[k].it_max};
    x.res[2 * k] = x.res[2 * k + 1] = -1;
    off += (bytes + 15) & ~(size_t)15;
  }
  cudaError_t e = cudaMemcpyAsync(x.sin.dev, x.in, off, cudaMemcpyHostToDevice, x.s);
  if (e == cudaSuccess) e = cudaMemcpyAsync(x.sdj.dev, x.dj, sizeof(DJob) * (size_t)n, cudaMemcpyHostToDevice, x.s);
  if (e == cudaSuccess) e = cudaMemsetAsync(x.sres.dev, 0xff, sizeof(int) * 2 * (size_t)n, x.s); /* the host's -1 sentinels */
  if (e != cudaSuccess) return e;
  *h2d = off + sizeof(DJob) * (size_t)n;
  e = ldpc_launch(nc, n, x.s, z4, (const DJob *)x.sdj.dev, x, (uint8_t *)x.ssnap.dev, (int *)x.sres.dev);
  if (e != cudaSuccess) return e;
  e = cudaMemcpyAsync(x.res, x.sres.dev, sizeof(int) * 2 * (size_t)n, cudaMemcpyDeviceToHost, x.s);
  *d2h = sizeof(int) * 2 * (size_t)n;
  *ncopy = 1;
  for (int k = 0; k < n && e == cudaSuccess; k++) {
    const size_t rows = (size_t)jobs[k].it_max - 1, wd = (size_t)(jobs[k].K + 7) >> 3, o = (size_t)k * NR_GPU_LDPC_MAX_SNAP * NR_GPU_LDPC_KB_MAX;
    if (!rows) continue;
    e = cudaMemcpy2DAsync(x.snap + o, NR_GPU_LDPC_KB_MAX, (uint8_t *)x.ssnap.dev + o, NR_GPU_LDPC_KB_MAX, wd, rows, cudaMemcpyDeviceToHost, x.s);
    *d2h += wd * rows;
    ++*ncopy;
  }
  return e;
}

/* CB = blocks per code block: 1 = _nc, 2/4 = small clusters, 8 = the portable-max cluster. Explicit instantiation so lazy loading sees every one. */
template __global__ void k_ldpc_t<1>(const DJob *, int8_t *, int8_t *, int8_t *, int *, uint8_t *, int *);
template __global__ void k_ldpc_t<2>(const DJob *, int8_t *, int8_t *, int8_t *, int *, uint8_t *, int *);
template __global__ void k_ldpc_t<4>(const DJob *, int8_t *, int8_t *, int8_t *, int *, uint8_t *, int *);
template __global__ void k_ldpc_t<8>(const DJob *, int8_t *, int8_t *, int8_t *, int *, uint8_t *, int *);
template __global__ void k_ldpc4_t<1>(const DJob *, int8_t *, int8_t *, int8_t *, int *, uint8_t *, int *);
template __global__ void k_ldpc4_t<2>(const DJob *, int8_t *, int8_t *, int8_t *, int *, uint8_t *, int *);
template __global__ void k_ldpc4_t<4>(const DJob *, int8_t *, int8_t *, int8_t *, int *, uint8_t *, int *);
template __global__ void k_ldpc4_t<8>(const DJob *, int8_t *, int8_t *, int8_t *, int *, uint8_t *, int *);
template __global__ void k_ldpc4_t<1, 1>(const DJob *, int8_t *, int8_t *, int8_t *, int *, uint8_t *, int *);

/* Kernel family: the cluster kernels need sm_90+; below that, the _nc kernels (one block per
 * code block, same results). Decided once, first thing in warm_common()/init, before any graph warm-up. g_nc_state: 0 undecided, 1 cluster,
 * 2 non-cluster, 3 CC query failed (GPU LDPC refused, CPU fallback). */
static std::atomic<int> g_nc_state{0}, g_refused{0};
static int g_sm_level = 0; /* 1 = res in shared memory for the _nc kernel (Z % 4 == 0 only) */
static std::atomic<uint64_t> g_sm_launches{0};
static int g_cl_min_cb = 0, g_rt_sms = 0; /* cluster selection min code blocks (0 = off) */
static std::atomic<uint64_t> g_cb_launches[4], g_cl_fallback{0};
static int g_nc_min_cb = 0; /* launches with >= N code blocks use the _nc kernels (0 = off); set before g_nc_state latches */
/* Blocks per code block for a launch of n code blocks. Rules in order: force; non-cluster state -> 1; selection off or n < min_cb -> today's
 * choice; else the largest cluster (4, 2) that keeps the whole launch in ONE wave of the RT partition, else 1. */
static int pick_cb(int state, int n, int rt_sms, int min_cb, int force)
{
  if (force == 1 || force == 2 || force == 4 || force == 8)
    return force;
  if (state != 1)
    return 1;
  if (min_cb <= 0 || n < min_cb)
    return g_nc_min_cb && n >= g_nc_min_cb ? 1 : CLB;
  return n * 4 <= rt_sms ? 4 : n * 2 <= rt_sms ? 2 : 1;
}
extern "C" int nr_pdsch_gpu_ldpc_pick_cb(int state, int n, int sms, int min_cb, int force) { return pick_cb(state, n, sms, min_cb, force); }
extern "C" void nr_pdsch_gpu_ldpc_cl_stats(uint64_t o[5])
{
  for (int i = 0; i < 4; i++)
    o[i] = g_cb_launches[i].load();
  o[4] = g_sm_launches.load();
}
static int decide_nc()
{
  int st = g_nc_state.load();
  if (st)
    return st;
  int dev = 0, major = 0;
  if (cudaGetDevice(&dev) != cudaSuccess || cudaDeviceGetAttribute(&major, cudaDevAttrComputeCapabilityMajor, dev) != cudaSuccess) {
    if (!g_refused.fetch_add(1))
      fprintf(stderr, "SENSING: GPU LDPC: compute capability unknown; using CPU LDPC\n");
    return 3; /* not latched: a later call with a device retries */
  }
  st = major < 9 ? 2 : 1;
  g_nc_min_cb = 8; /* frozen ISAC_GPU_LDPC_NC_MIN_CB=8 */
  g_cl_min_cb = 4; /* frozen ISAC_GPU_LDPC_CL_MIN_CB=4 */
  g_sm_level = 1;  /* frozen ISAC_GPU_LDPC_SM=1 */
  g_nc_state.store(st);
  if (g_cl_min_cb)
    fprintf(stderr, "SENSING: GPU_LDPC cl_min_cb=%d cl_force=%d\n", g_cl_min_cb, 0);
  if (g_nc_min_cb)
    fprintf(stderr, "SENSING: GPU_LDPC nc_min_cb=%d\n", g_nc_min_cb);
  if (st == 2)
    fprintf(stderr, "SENSING: GPU LDPC: non-cluster kernels (sm_%d)\n", major);
  return st;
}

/* one launch of n code blocks: CB blocks each (1 = _nc, 2/4/8 = cluster); CB 2/4 falls back to _nc in the same call if no SM group is free */
static cudaError_t ldpc_launch(int nc, int n, cudaStream_t st, bool z4, const DJob *ddj, Ctx &x, uint8_t *dsnap, int *dres)
{
  const int cb = pick_cb(nc, n, g_rt_sms, g_cl_min_cb, 0);
  cudaLaunchConfig_t cfg = {};
  cfg.gridDim = dim3(n * cb);
  cfg.blockDim = dim3(THREADS);
  cfg.stream = st;
  cudaLaunchAttribute at[1];
  at[0].id = cudaLaunchAttributeClusterDimension;
  at[0].val.clusterDim.x = cb;
  at[0].val.clusterDim.y = 1;
  at[0].val.clusterDim.z = 1;
  cfg.attrs = at;
  cfg.numAttrs = cb == 1 ? 0 : 1;
  void (*k)(const DJob *, int8_t *, int8_t *, int8_t *, int *, uint8_t *, int *) =
      z4 ? (cb == 1 ? k_ldpc4_t<1> : cb == 2 ? k_ldpc4_t<2> : cb == 4 ? k_ldpc4_t<4> : k_ldpc4_t<8>)
         : (cb == 1 ? k_ldpc_t<1> : cb == 2 ? k_ldpc_t<2> : cb == 4 ? k_ldpc_t<4> : k_ldpc_t<8>);
  const bool sm = cb == 1 && z4 && g_sm_level;
  if (sm)
    k = k_ldpc4_t<1, 1>;
  cudaError_t e = cudaLaunchKernelEx(&cfg, k, ddj, x.llr, x.llrRes, x.c2v, x.unsat, dsnap, dres);
  int used = cb;
  if (e != cudaSuccess && (cb == 2 || cb == 4)) {
    (void)cudaGetLastError();
    g_cl_fallback.fetch_add(1);
    cfg.gridDim = dim3(n);
    cfg.numAttrs = 0;
    used = 1;
    e = cudaLaunchKernelEx(&cfg, z4 ? k_ldpc4_t<1> : k_ldpc_t<1>, ddj, x.llr, x.llrRes, x.c2v, x.unsat, dsnap, dres);
  } else if (e != cudaSuccess && sm) { /* smem variant failed to launch: the global-memory _nc kernel */
    (void)cudaGetLastError();
    e = cudaLaunchKernelEx(&cfg, k_ldpc4_t<1>, ddj, x.llr, x.llrRes, x.c2v, x.unsat, dsnap, dres);
  } else if (sm && e == cudaSuccess)
    g_sm_launches.fetch_add(1);
  g_cb_launches[used == 1 ? 0 : used == 2 ? 1 : used == 4 ? 2 : 3].fetch_add(1);
  return e;
}

/* GPUSHARE: build every (BG, Z, R) graph now and load both kernels (CUDA lazy loading). Shared by the DL pool init and the
 * UL pool init (ISAC_UL_GPU_LDPC), which may run without the DL one. */
static bool warm_common()
{
  static std::mutex m;
  static bool done = false;
  std::lock_guard<std::mutex> lk(m);
  if (done)
    return true;
  const int nc = decide_nc(); /* before any graph warm-up */
  if (nc == 3)
    return false;
  for (int BG = 1; BG <= 2; BG++)
    for (int R : {13, 15, 23, 89})
      for (int a : {2, 3, 5, 7, 9, 11, 13, 15})
        for (int Z = a; Z <= ZMAX; Z *= 2)
          graph(BG, Z, R); /* nullptr for the (BG, R) pairs that do not exist: nothing cached */
  cudaFuncAttributes fa;
  if (nc == 2 ? cudaFuncGetAttributes(&fa, k_ldpc_t<1>) != cudaSuccess || cudaFuncGetAttributes(&fa, k_ldpc4_t<1>) != cudaSuccess
              : cudaFuncGetAttributes(&fa, k_ldpc_t<8>) != cudaSuccess || cudaFuncGetAttributes(&fa, k_ldpc4_t<8>) != cudaSuccess)
    return false;
  if (nc == 1 && g_nc_min_cb && (cudaFuncGetAttributes(&fa, k_ldpc_t<1>) != cudaSuccess || cudaFuncGetAttributes(&fa, k_ldpc4_t<1>) != cudaSuccess))
    return false; /* mixed selection: load the _nc module now, not on the first big TB */
  if (g_cl_min_cb && nc == 1 &&
      (cudaFuncGetAttributes(&fa, k_ldpc_t<2>) != cudaSuccess || cudaFuncGetAttributes(&fa, k_ldpc_t<4>) != cudaSuccess ||
       cudaFuncGetAttributes(&fa, k_ldpc4_t<2>) != cudaSuccess || cudaFuncGetAttributes(&fa, k_ldpc4_t<4>) != cudaSuccess ||
       cudaFuncGetAttributes(&fa, k_ldpc_t<1>) != cudaSuccess || cudaFuncGetAttributes(&fa, k_ldpc4_t<1>) != cudaSuccess))
    return false; /* lazy-load stall otherwise (the _nc kernels are the fallback) */
  if (g_sm_level && cudaFuncGetAttributes(&fa, k_ldpc4_t<1, 1>) != cudaSuccess)
    return false;
  done = true;
  return true;
}
} // namespace nrldpc

using namespace nrldpc;

extern "C" int nr_pdsch_gpu_ldpc_init(void)
{
  if (!nr_gpu_module_ready()) return -1; // CPU tier (no device, arch mismatch or NR_GPU_FORCE_MODEL=cpu): refuse, the host keeps the CPU path
  static std::once_flag once;
  std::call_once(once, [] {
    int dev = 0, cl = 0;
    const int nc = decide_nc();
    if (nc == 3 || (nc == 1 && (cudaGetDevice(&dev) != cudaSuccess || cudaDeviceGetAttribute(&cl, cudaDevAttrClusterLaunch, dev) != cudaSuccess || !cl)))
      return;
    int lo = 0, hi = 0;
    cudaDeviceGetStreamPriorityRange(&lo, &hi);
    g_stg = nr_gpu_model() != NR_GPU_MODEL_DIRECT; /* Q4 */
    if (g_stg) {
      g_stg_n = (int)nr_gpu_membudget_fit(nr_gpu_stage_share(), 0, stage_ctx_bytes(NJ, NJ), 1, CTX);
      if (g_stg_n < 1) return;
    }
    for (int i = 0; i < CTX + NR_GPU_PASS_RING + 1; i++) {
      Ctx &x = g_ctx[i];
      const bool pass_ctx = i >= CTX && i < CTX + NR_GPU_PASS_RING;
      const int cap = pass_ctx ? NJP : NJ;     /* the graph-pass instances decode up to 160 code blocks per launch ... */
      const int cio = pass_ctx ? 1 : NJ;       /* ... from and into their own buffers (nr_gpu_pass_graph.cu): the pinned staging of the standalone run() is not used */
      x.cap = cap;
      if ((!nr_gpu_green_stream(&x.s, hi, true, i) && cudaStreamCreateWithPriority(&x.s, cudaStreamNonBlocking, hi) != cudaSuccess)
          || cudaHostAlloc(&x.dj, sizeof(DJob) * cap, cudaHostAllocMapped) != cudaSuccess
          || cudaHostAlloc(&x.in, (size_t)cio * MAXC * ZMAX, cudaHostAllocMapped) != cudaSuccess
          || cudaHostAlloc(&x.snap, (size_t)cio * NR_GPU_LDPC_MAX_SNAP * NR_GPU_LDPC_KB_MAX, cudaHostAllocMapped) != cudaSuccess
          || cudaHostAlloc(&x.res, sizeof(int) * 2 * cio, cudaHostAllocMapped) != cudaSuccess
          || cudaMalloc(&x.llr, (size_t)cap * MAXC * ZMAX + 16) != cudaSuccess || cudaMalloc(&x.llrRes, (size_t)cap * MAXC * ZMAX + 16) != cudaSuccess
          || cudaMalloc(&x.c2v, (size_t)cap * MAXE * ZMAX + 16) != cudaSuccess || cudaMalloc(&x.unsat, sizeof(int) * cap * NIT) != cudaSuccess)
        return;
      if (g_stg && i < g_stg_n && !stage_alloc(x, cap, cio))
        return;
    }
    if (!warm_common())
      return;
    { /* SMs of the RT partition (device count if the mask query fails): pick_cb keeps a launch within one wave */
      unsigned m[4] = {0};
      int sms = 0;
      cudaDeviceGetAttribute(&sms, cudaDevAttrMultiProcessorCount, 0);
      g_rt_sms = nr_green_smmask(g_ctx[0].s, m) ? __builtin_popcount(m[0]) + __builtin_popcount(m[1]) + __builtin_popcount(m[2]) + __builtin_popcount(m[3]) : sms;
    }
    g_ok = 1;
  });
  return g_ok ? 0 : -1;
}

extern "C" int nr_pdsch_gpu_ldpc_run(nr_gpu_ldpc_job_t *jobs, int n)
{
  if (!g_ok)
    return NR_GPU_LDPC_E_INIT;
  if (n < 1 || n > NJ)
    return NR_GPU_LDPC_E_RANGE;
  const Graph *gr[NJ];
  for (int k = 0; k < n; k++) {
    const nr_gpu_ldpc_job_t &j = jobs[k];
    if (j.it_max < 1 || j.it_max - 1 > NR_GPU_LDPC_MAX_SNAP || ((j.K + 7) >> 3) > NR_GPU_LDPC_KB_MAX) {
      static std::atomic<int> logged{0};
      if (logged.fetch_add(1) < 8) /* which of the four checks: a few lines per process */
        fprintf(stderr, "SENSING: GPU_LDPC param refuse BG=%d Z=%d R=%d K=%d it_max=%d (K&7=%d K>>3=%d max %d, it_max-1 max %d)\n", j.BG, j.Z, j.R, j.K,
                j.it_max, j.K & 7, j.K >> 3, NR_GPU_LDPC_KB_MAX, NR_GPU_LDPC_MAX_SNAP);
      return NR_GPU_LDPC_E_PARAM;
    }
    if (!(gr[k] = graph(j.BG, j.Z, j.R)))
      return NR_GPU_LDPC_E_GRAPH_MISS;
  }
  const int c = g_stg ? acquire(g_stg_n) : acquire();
  Ctx &x = g_ctx[c];
  bool z4 = true; /* the 4-lane kernel when every block's Z is a multiple of 4 */
  for (int k = 0; k < n; k++)
    z4 = z4 && !(jobs[k].Z & 3);
  uint64_t st_h2d = 0, st_d2h = 0;
  int st_nd = 0;
  cudaError_t e;
  if (g_stg) { /* Q4: DISCRETE: staged copies (see stage_launch) */
    e = stage_launch(g_nc_state.load(), x, jobs, n, gr, z4, &st_h2d, &st_d2h, &st_nd);
  } else {
  int8_t *din = nullptr;
  cudaHostGetDevicePointer((void **)&din, x.in, 0);
  for (int k = 0; k < n; k++) {
    const int kc = jobs[k].BG == 1 ? 68 : 52;
    memcpy(x.in + (size_t)k * MAXC * ZMAX, jobs[k].llr, (size_t)kc * jobs[k].Z);
    x.dj[k] = DJob{gr[k], din + (size_t)k * MAXC * ZMAX, jobs[k].K, jobs[k].it_max};
    x.res[2 * k] = x.res[2 * k + 1] = -1;
  }
  DJob *ddj = nullptr;
  uint8_t *dsnap = nullptr;
  int *dres = nullptr;
  cudaHostGetDevicePointer((void **)&ddj, x.dj, 0);
  cudaHostGetDevicePointer((void **)&dsnap, x.snap, 0);
  cudaHostGetDevicePointer((void **)&dres, x.res, 0);
  e = ldpc_launch(g_nc_state.load(), n, x.s, z4, ddj, x, dsnap, dres);
  }
  /* GPUSHARE: poll like the demod/FEP waits. cudaStreamSynchronize under cudaDeviceScheduleBlockingSync (set by
   * nr_gpu_platform) sleeps on the completion interrupt; the TB waited far longer than its kernel. */
  uint64_t pt = 0;
  const uint64_t ss0 = nr_ss_now();
  if (e == cudaSuccess)
    while ((e = cudaStreamQuery(x.s)) == cudaErrorNotReady)
      nr_gpu_poll_relax(&pt); /* P99: ISAC_GPU_POLL_SLEEP_US */
  nr_ss_add(NRSS_LDPC, ss0);
  if (e != cudaSuccess) {
    fprintf(stderr, "nr_pdsch_gpu_ldpc_run: %s\n", cudaGetErrorString(e));
    g_busy.fetch_and(~(1u << c));
    return NR_GPU_LDPC_E_STREAM;
  }
  int rc = 0;
  for (int k = 0; k < n; k++) {
    nr_gpu_ldpc_job_t &j = jobs[k];
    j.nsnap = x.res[2 * k];
    j.syn0 = x.res[2 * k + 1];
    if (j.nsnap < 0 || j.nsnap > NR_GPU_LDPC_MAX_SNAP) {
      rc = NR_GPU_LDPC_E_SENTINEL;
      continue;
    }
    for (int s = 0; s < j.nsnap; s++)
      memcpy(j.snap[s], x.snap + ((size_t)k * NR_GPU_LDPC_MAX_SNAP + s) * NR_GPU_LDPC_KB_MAX, (j.K + 7) >> 3);
  }
  if (g_stg) nr_stage::note(NR_GPU_STG_LDPC_DL, st_h2d, 2, st_d2h, st_nd);
  g_busy.fetch_and(~(1u << c));
  return rc;
}

/* o[0] = kernel family (0 undecided, 1 cluster, 2 non-cluster, 3 refused: CC unknown), o[1] = refusal count (CC query failed), o[2] = dev graph misses */
extern "C" void nr_pdsch_gpu_ldpc_stats(uint64_t *o)
{
  o[0] = (uint64_t)g_nc_state.load(); o[1] = (uint64_t)g_refused.load(); o[2] = g_dev_miss.load();
}
extern "C" unsigned nr_pdsch_gpu_ldpc_dev_misses(void) { return g_dev_miss.load(); }
/* GPUPASS: launch-only decode of n <= NJ code blocks. Inputs already on the device (llr_dev, NR_GPU_LDPC_DEV_IN_STRIDE per job), outputs
 * into bits_dev (NR_GPU_LDPC_DEV_* layout); enqueues on `stream` only. jobs[k].llr / snap are not touched. Below CC 9 the non-cluster
 * kernels run; -1 on any refusal / error (init not done, bad job, graph miss, launch error). One pass in flight (private context). 0 or an error code. */
/* host: validates the jobs and writes them into the instance's DJob array (graph replay: the kernel reads it when it runs). 0 or -1. */
static int pass_set(int inst, const nr_gpu_ldpc_job_t *jobs, int n, const nr_gpu_buf_t *llr_dev, bool *z4)
{
  if (!g_ok || inst < 0 || inst > NR_GPU_PASS_RING || !jobs || n < 1 || n > (inst < NR_GPU_PASS_RING ? NJP : NJ) || !llr_dev || !llr_dev->dev || llr_dev->bytes < (size_t)n * NR_GPU_LDPC_DEV_IN_STRIDE)
    return -1;
  const Graph *gr[NJP];
  for (int k = 0; k < n; k++) {
    const nr_gpu_ldpc_job_t &j = jobs[k];
    if (j.it_max < 1 || j.it_max - 1 > NR_GPU_LDPC_MAX_SNAP || ((j.K + 7) >> 3) > NR_GPU_LDPC_KB_MAX
        || !(gr[k] = graph_lookup(j.BG, j.Z, j.R)))
      return -1;
  }
  Ctx &x = g_ctx[CTX + inst];
  for (int k = 0; k < n; k++)
    x.dj[k] = DJob{gr[k], (const int8_t *)llr_dev->dev + (size_t)k * NR_GPU_LDPC_DEV_IN_STRIDE, jobs[k].pad ? 0 : jobs[k].K, jobs[k].it_max, x.ts ? x.ts + 2 * k : nullptr};
  if (z4) {
    *z4 = true;
    for (int k = 0; k < n; k++)
      *z4 = *z4 && !(jobs[k].Z & 3);
  }
  return 0;
}
extern "C" int nr_pdsch_gpu_ldpc_pass_set(int inst, const nr_gpu_ldpc_job_t *jobs, int n, const nr_gpu_buf_t *llr_dev)
{
  return inst >= NR_GPU_PASS_RING ? -1 : pass_set(inst, jobs, n, llr_dev, nullptr);
}
static int dev_impl(int inst, const nr_gpu_ldpc_job_t *jobs, int n, const nr_gpu_buf_t *llr_dev, nr_gpu_buf_t *bits_dev, void *stream)
{
  bool z4 = true;
  if (inst >= 0 && inst < NR_GPU_PASS_RING && bits_dev && bits_dev->dev && bits_dev->bytes >= NR_GPU_PASS_LDPC_BITS_BYTES) /* the instance's stamp area (its one bits buffer), before pass_set fills the DJobs */
    g_ctx[CTX + inst].ts = (unsigned long long *)((char *)bits_dev->dev + NR_GPU_PASS_LDPC_TS_OFF);
  if (!bits_dev || !bits_dev->dev || bits_dev->bytes < (inst < NR_GPU_PASS_RING ? NR_GPU_PASS_LDPC_BITS_BYTES : NR_GPU_LDPC_DEV_BITS_BYTES) || pass_set(inst, jobs, n, llr_dev, &z4) != 0)
    return -1;
  Ctx &x = g_ctx[CTX + inst];
  const cudaStream_t s = (cudaStream_t)stream;
  DJob *ddj = nullptr;
  cudaHostGetDevicePointer((void **)&ddj, x.dj, 0);
  uint8_t *dsnap = (uint8_t *)bits_dev->dev;
  const int jmax = inst < NR_GPU_PASS_RING ? NJP : NJ; /* a pass instance's buffers hold 160 code blocks, the standalone context's 32 */
  int *dres = (int *)((char *)bits_dev->dev + (inst < NR_GPU_PASS_RING ? NR_GPU_PASS_LDPC_RES_OFF : NR_GPU_LDPC_DEV_RES_OFF));
  if (cudaMemsetAsync(dres, 0xff, 2 * jmax * sizeof(int), s) != cudaSuccess) return -1; /* the host's -1 sentinels */
  return ldpc_launch(g_nc_state.load(), n, s, z4, ddj, x, dsnap, dres) == cudaSuccess ? 0 : -1;
}
extern "C" int nr_pdsch_gpu_ldpc_dev(const nr_gpu_ldpc_job_t *jobs, int n, const nr_gpu_buf_t *llr_dev, nr_gpu_buf_t *bits_dev, void *stream)
{
  return dev_impl(NR_GPU_PASS_RING, jobs, n, llr_dev, bits_dev, stream); /* its own context: never an instance's */
}
/* graph pass of instance `inst`: the same, with the instance's own context; the DJob array is rewritten by nr_pdsch_gpu_ldpc_pass_set before a replay */
extern "C" int nr_pdsch_gpu_ldpc_pass_dev(int inst, const nr_gpu_ldpc_job_t *jobs, int n, const nr_gpu_buf_t *llr_dev, nr_gpu_buf_t *bits_dev, void *stream)
{
  return inst >= NR_GPU_PASS_RING ? -1 : dev_impl(inst, jobs, n, llr_dev, bits_dev, stream);
}

/* ---------------------------------------------------------------------------------------------------------------------
 * ULLDPC: ISAC_UL_GPU_LDPC=1 (default off). The same bit-exact decoder kernel for the passive PUSCH decode, in a pool of its own
 * so the DL LDPC contexts are never taken by, or waited on for, the UL:
 *   - UL_CTX (2) contexts, each with pinned buffers allocated AND written at warm-up (a first touch of an unwritten host page
 *     by the GPU is a page migration that stalled other launches 20-900 ms, FATTAILS), device pointers cached;
 *   - a low-priority stream on the discovery SM partition when ISAC_GPU_DISC_SMS gives one (else the lowest priority, and a call
 *     defers to the CPU while an RT GPU user is in flight); chosen by a start-up test decode with a deadline, because a cluster
 *     launch needs CLB SMs in one GPC and a small partition may not have them;
 *   - duty cap ISAC_UL_GPU_LDPC_PCT (default 15) of each 100 ms window, over it / contexts busy / error -> the caller runs the CPU
 *     decoder (counted);
 *   - a hard deadline ISAC_UL_GPU_LDPC_DEADLINE_US (default 4000): the kernel only writes the context's own pinned buffers, so a
 *     late call is abandoned (the context stays reserved until its stream drains) and the CPU decodes the TB: a GPU stall costs
 *     the UL at most the deadline, never the stall. */
extern "C" int nr_gpu_budget_rt_active(void); /* RT GPU users in flight (the executable's nr_gpu_budget.c) */
namespace nrldpc {
constexpr int UL_CTX = 2;
struct UlState {
  bool ok = false, part = false;
  int kind = 0;
  Ctx c[UL_CTX];
  cudaEvent_t e0[UL_CTX] = {}, e1[UL_CTX] = {};
  DJob *ddj[UL_CTX] = {};
  int8_t *din[UL_CTX] = {};
  uint8_t *dsnap[UL_CTX] = {};
  int *dres[UL_CTX] = {};
  std::atomic<unsigned> busy{0}, lost{0};
  unsigned pct = 15;
  uint64_t deadline_us = 4000;
  std::atomic<uint64_t> win_start_us{0}, win_used_us{0};
  std::atomic<uint64_t> calls{0}, cbs{0}, gpu_us{0}, max_us{0}, n_busy{0}, n_cap{0}, n_rt{0}, n_late{0}, n_deadline{0}, n_err{0}, n_reclaimed{0};
};
static UlState UL;

static uint64_t ul_now_us()
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

static bool ul_alloc_ctx(int i)
{
  Ctx &x = UL.c[i];
  if (cudaHostAlloc(&x.dj, sizeof(DJob) * NJ, cudaHostAllocMapped) != cudaSuccess
      || cudaHostAlloc(&x.in, (size_t)NJ * MAXC * ZMAX, cudaHostAllocMapped) != cudaSuccess
      || cudaHostAlloc(&x.snap, (size_t)NJ * NR_GPU_LDPC_MAX_SNAP * NR_GPU_LDPC_KB_MAX, cudaHostAllocMapped) != cudaSuccess
      || cudaHostAlloc(&x.res, sizeof(int) * 2 * NJ, cudaHostAllocMapped) != cudaSuccess
      || cudaMalloc(&x.llr, (size_t)NJ * MAXC * ZMAX + 16) != cudaSuccess || cudaMalloc(&x.llrRes, (size_t)NJ * MAXC * ZMAX + 16) != cudaSuccess
      || cudaMalloc(&x.c2v, (size_t)NJ * MAXE * ZMAX + 16) != cudaSuccess || cudaMalloc(&x.unsat, sizeof(int) * NJ * NIT) != cudaSuccess)
    return false;
  /* write every pinned byte now: the kernel then only ever touches resident pages */
  memset(x.dj, 0, sizeof(DJob) * NJ);
  memset(x.in, 0, (size_t)NJ * MAXC * ZMAX);
  memset(x.snap, 0, (size_t)NJ * NR_GPU_LDPC_MAX_SNAP * NR_GPU_LDPC_KB_MAX);
  memset(x.res, 0, sizeof(int) * 2 * NJ);
  cudaMemset(x.llr, 0, (size_t)NJ * MAXC * ZMAX + 16);
  cudaMemset(x.llrRes, 0, (size_t)NJ * MAXC * ZMAX + 16);
  cudaMemset(x.c2v, 0, (size_t)NJ * MAXE * ZMAX + 16);
  cudaMemset(x.unsat, 0, sizeof(int) * NJ * NIT);
  if (g_stg) /* Q4: DISCRETE: the kernel works on device copies (stage_launch), the mapped pinned buffers above are the mirrors */
    return stage_alloc(x, NJ, NJ) && (!uldiag::on() || (cudaEventCreate(&UL.e0[i]) == cudaSuccess && cudaEventCreate(&UL.e1[i]) == cudaSuccess));
  if (cudaHostGetDevicePointer((void **)&UL.din[i], x.in, 0) != cudaSuccess || cudaHostGetDevicePointer((void **)&UL.ddj[i], x.dj, 0) != cudaSuccess
      || cudaHostGetDevicePointer((void **)&UL.dsnap[i], x.snap, 0) != cudaSuccess || cudaHostGetDevicePointer((void **)&UL.dres[i], x.res, 0) != cudaSuccess)
    return false;
  if (uldiag::on() && (cudaEventCreate(&UL.e0[i]) != cudaSuccess || cudaEventCreate(&UL.e1[i]) != cudaSuccess))
    return false;
  return true;
}

/* Launch jobs[0..n) on context i and wait up to deadline_us. 0 = done (results copied into jobs); -1 launch / CUDA error;
 * -2 deadline: nothing was copied back and the context stays reserved (UL.lost) until its stream drains. */
static int ul_exec(int i, nr_gpu_ldpc_job_t *jobs, int n, const Graph *const *gr, uint64_t deadline_us, uint64_t *launch_us, double *gpu_us)
{
  Ctx &x = UL.c[i];
  const bool dg = uldiag::on();
  const uint64_t t0 = ul_now_us();
  bool z4 = true;
  for (int k = 0; k < n; k++)
    z4 = z4 && !(jobs[k].Z & 3);
  uint64_t st_h2d = 0, st_d2h = 0;
  int st_nd = 0;
  cudaError_t e;
  if (g_stg) { /* Q4: DISCRETE: staged copies (see stage_launch); the abandoned-call rule is unchanged: the copies only touch this context's own buffers */
    if (dg)
      cudaEventRecord(UL.e0[i], x.s);
    e = stage_launch(g_nc_state.load(), x, jobs, n, gr, z4, &st_h2d, &st_d2h, &st_nd);
  } else {
  for (int k = 0; k < n; k++) {
    const int kc = jobs[k].BG == 1 ? 68 : 52;
    memcpy(x.in + (size_t)k * MAXC * ZMAX, jobs[k].llr, (size_t)kc * jobs[k].Z);
    x.dj[k] = DJob{gr[k], UL.din[i] + (size_t)k * MAXC * ZMAX, jobs[k].K, jobs[k].it_max};
    x.res[2 * k] = x.res[2 * k + 1] = -1;
  }
  if (dg)
    cudaEventRecord(UL.e0[i], x.s);
  e = ldpc_launch(g_nc_state.load(), n, x.s, z4, UL.ddj[i], x, UL.dsnap[i], UL.dres[i]);
  }
  if (dg)
    cudaEventRecord(UL.e1[i], x.s);
  *launch_us = ul_now_us() - t0;
  uint64_t spin0 = ul_now_us();
  const uint64_t ss0 = nr_ss_now();
  while (e == cudaSuccess && (e = cudaStreamQuery(x.s)) == cudaErrorNotReady) {
    e = cudaSuccess;
    const uint64_t now = ul_now_us();
    if (now - t0 > deadline_us) {
      nr_ss_add(NRSS_LDPC, ss0);
      return -2;
    }
    if (now - spin0 > 60) { /* no busy spin: 25 us naps after 60 us */
      const struct timespec ts = {0, 25000};
      nanosleep(&ts, NULL);
    }
  }
  nr_ss_add(NRSS_LDPC, ss0);
  if (e != cudaSuccess)
    return -1;
  if (dg) {
    float ms = 0.f;
    *gpu_us = cudaEventElapsedTime(&ms, UL.e0[i], UL.e1[i]) == cudaSuccess ? 1000.0 * ms : 0.0;
  }
  for (int k = 0; k < n; k++) {
    nr_gpu_ldpc_job_t &j = jobs[k];
    j.nsnap = x.res[2 * k];
    j.syn0 = x.res[2 * k + 1];
    if (j.nsnap < 0 || j.nsnap > NR_GPU_LDPC_MAX_SNAP)
      return -1;
    for (int s = 0; s < j.nsnap; s++)
      memcpy(j.snap[s], x.snap + ((size_t)k * NR_GPU_LDPC_MAX_SNAP + s) * NR_GPU_LDPC_KB_MAX, (j.K + 7) >> 3);
  }
  if (g_stg) nr_stage::note(NR_GPU_STG_LDPC_UL, st_h2d, 2, st_d2h, st_nd);
  return 0;
}

/* start-up test decode of a clean all-zero BG2 codeword: proves the stream can run a (cluster) launch */
static bool ul_selftest(int i)
{
  static int8_t in[52 * 64 + 16];
  memset(in, 0, sizeof(in));
  for (int q = 2 * 64; q < 52 * 64; q++)
    in[q] = 24;
  static nr_gpu_ldpc_job_t job;
  job = nr_gpu_ldpc_job_t{};
  job.BG = 2;
  job.R = 15;
  job.Z = 64;
  job.K = 10 * 64;
  job.it_max = 8;
  job.llr = in;
  const Graph *gr = graph(2, 64, 15);
  uint64_t l = 0;
  double g = 0;
  return gr && ul_exec(i, &job, 1, &gr, 1000000, &l, &g) == 0 && job.nsnap > 0;
}

/* ULST: after an abandoned (deadline) call, every call goes to the CPU decoder while
 * it is still on the GPU and, once it drained, for as long again as it took (launch to reclaim): the FEP hold of
 * nr_pusch_gpu_fep.cuh, so a starved partition costs one deadline per observed GPU latency, not one per TB. */
static uint64_t ul_lost_t0[UL_CTX];
static std::atomic<uint64_t> ul_hold_until{0};
/* gives back context i if it was abandoned and its stream has drained; true = reclaimed by this call */
static bool ul_reclaim(int i)
{
  const unsigned bit = 1u << i;
  if (!(UL.lost.load() & bit) || cudaStreamQuery(UL.c[i].s) == cudaErrorNotReady)
    return false;
  if (!(UL.lost.fetch_and(~bit) & bit))
    return false; /* another consumer reclaimed it */
  cudaGetLastError(); /* an abandoned call that failed: the next use re-validates */
  {
    const uint64_t t = ul_now_us(), until = t + (t - ul_lost_t0[i]);
    uint64_t h = ul_hold_until.load();
    while (until > h && !ul_hold_until.compare_exchange_weak(h, until)) {}
  }
  UL.busy.fetch_and(~bit);
  ++UL.n_reclaimed;
  return true;
}

static int ul_acquire()
{
  for (int i = 0; i < UL_CTX; i++) {
    const unsigned bit = 1u << i;
    if (UL.busy.load() & bit) {
      if (!ul_reclaim(i))
        continue;
    }
    if (!(UL.busy.fetch_or(bit) & bit))
      return i;
  }
  return -1;
}
} // namespace nrldpc

extern "C" int nr_pusch_gpu_ldpc_init(int pct, int deadline_us)
{
  /* every refusal below logs its reason once (the host then runs the CPU LDPC decoder) */
  auto refuse = [](const char *why) {
    fprintf(stderr, "SENSING: UL_GPU_LDPC init refused: %s -- CPU LDPC decoder\n", why);
    return -1;
  };
  if (!nr_gpu_module_ready()) return refuse("module not ready"); // CPU tier (no device, arch mismatch or NR_GPU_FORCE_MODEL=cpu): refuse, the host keeps the CPU path
  static std::mutex m;
  std::lock_guard<std::mutex> lk(m);
  if (UL.ok)
    return 0;
  int dev = 0, cl = 0;
  const int nc = decide_nc();
  if (nc == 3 || (nc == 1 && (cudaGetDevice(&dev) != cudaSuccess || cudaDeviceGetAttribute(&cl, cudaDevAttrClusterLaunch, dev) != cudaSuccess || !cl)))
    return refuse("no usable kernel variant (compute capability / cluster launch)");
  g_stg = nr_gpu_model() != NR_GPU_MODEL_DIRECT; /* Q4 */
  if (g_stg && nr_gpu_membudget_fit(nr_gpu_stage_share(), 0, stage_ctx_bytes(NJ, NJ), 1, UL_CTX) < UL_CTX)
    return refuse("DISCRETE staging of the UL contexts does not fit the device-memory share"); /* the two UL contexts do not both fit the staging share */
  UL.pct = pct > 0 && pct <= 100 ? (unsigned)pct : 15u;
  UL.deadline_us = deadline_us > 0 ? (uint64_t)deadline_us : 4000u;
  if (!warm_common())
    return refuse("kernel warm-up failed");
  int lo = 0, hi = 0;
  cudaDeviceGetStreamPriorityRange(&lo, &hi);
  for (int attempt = 0; attempt < 2 && !UL.ok; attempt++) { /* 0: discovery partition, 1: plain lowest-priority stream */
    bool good = true;
    for (int i = 0; i < UL_CTX && good; i++) {
      Ctx &x = UL.c[i];
      good = ul_alloc_ctx(i);
      if (!good) {
        fprintf(stderr, "SENSING: UL_GPU_LDPC context %d allocation failed (attempt %d): %s\n", i, attempt, cudaGetErrorString(cudaGetLastError()));
        break;
      }
      if (attempt == 0) {
        UL.kind = nr_pusch_gpu_ul_stream(&x.s, hi, lo, i);
        UL.part = UL.kind != 0;
        if (!UL.part) {
          good = false; /* no partition configured: go straight to attempt 1 */
          break;
        }
      } else {
        UL.part = false;
        good = cudaStreamCreateWithPriority(&x.s, cudaStreamNonBlocking, lo) == cudaSuccess;
      }
      if (good && !ul_selftest(i)) {
        good = false;
        fprintf(stderr, "SENSING: UL_GPU_LDPC context %d self-test failed (attempt %d, %s): %s\n", i, attempt, g_stg ? "staged" : "mapped",
                cudaGetErrorString(cudaGetLastError()));
      }
      UL.busy.store(0);
    }
    if (good)
      UL.ok = true;
  }
  if (UL.ok)
    fprintf(stderr, "SENSING: UL_GPU_LDPC ready: %d contexts, %s stream, cap %u%% of each 100 ms, deadline %lu us\n", UL_CTX,
            UL.kind == 2 ? "real-time-partition" : UL.part ? "discovery-partition" : "lowest-priority", UL.pct, (unsigned long)UL.deadline_us);
  return UL.ok ? 0 : -1;
}

/* 0 = decoded (jobs filled); -1 contexts busy; -2 over the duty cap / RT in flight; -3 off / bad job / CUDA error; -4 deadline.
 * On non-zero nothing in jobs is valid and the caller runs the CPU decoder. */
extern "C" int nr_pusch_gpu_ldpc_run(nr_gpu_ldpc_job_t *jobs, int n)
{
  if (!UL.ok || n < 1 || n > NJ)
    return -3;
  const Graph *gr[NJ];
  for (int k = 0; k < n; k++) {
    const nr_gpu_ldpc_job_t &j = jobs[k];
    if (j.it_max < 1 || j.it_max - 1 > NR_GPU_LDPC_MAX_SNAP || ((j.K + 7) >> 3) > NR_GPU_LDPC_KB_MAX || !(gr[k] = graph(j.BG, j.Z, j.R)))
      return -3;
  }
  const uint64_t t0 = ul_now_us();
  uint64_t w0 = UL.win_start_us.load();
  if (t0 - w0 >= 100000 && UL.win_start_us.compare_exchange_strong(w0, t0))
    UL.win_used_us.store(0);
  if (UL.win_used_us.load() >= (uint64_t)UL.pct * 1000) {
    ++UL.n_cap;
    return -2;
  }
  if (!UL.part && nr_gpu_budget_rt_active() > 0) {
    ++UL.n_rt;
    return -2;
  }
  {
    for (int i = 0; i < UL_CTX; i++)
      ul_reclaim(i);
    if (UL.lost.load() || ul_now_us() < ul_hold_until.load()) {
      ++UL.n_busy;
      return -1;
    }
  }
  const int c = ul_acquire();
  if (c < 0) {
    ++UL.n_busy;
    return -1;
  }
  const uldiag::Snap r0 = uldiag::on() ? uldiag::snap() : uldiag::Snap();
  uint64_t launch_us = 0;
  double gpu_us = 0;
  const int rc = ul_exec(c, jobs, n, gr, UL.deadline_us, &launch_us, &gpu_us);
  const uint64_t dt = ul_now_us() - t0;
  if (rc == -2) { /* abandoned: keep the context reserved until the stream drains */
    ul_lost_t0[c] = t0;
    UL.lost.fetch_or(1u << c);
    ++UL.n_deadline;
    UL.win_used_us += dt;
    if (uldiag::on())
      uldiag::report("ldpc-deadline", dt, launch_us, gpu_us, n, r0, uldiag::snap());
    return -4;
  }
  UL.busy.fetch_and(~(1u << c));
  if (rc != 0) {
    ++UL.n_err;
    cudaGetLastError();
    return -3;
  }
  ++UL.calls;
  UL.cbs += (uint64_t)n;
  UL.gpu_us += dt;
  UL.win_used_us += dt;
  if (dt > 2000)
    ++UL.n_late;
  uint64_t m = UL.max_us.load();
  while (dt > m && !UL.max_us.compare_exchange_weak(m, dt)) {}
  if (uldiag::on())
    uldiag::report("ldpc", dt, launch_us, gpu_us, n, r0, uldiag::snap());
  return 0;
}

/* calls, cbs, gpu_us, max_us, busy, over_cap, rt_defer, late(>2 ms), deadline, errors, reclaimed, partitioned */
extern "C" void nr_pusch_gpu_ldpc_stats(uint64_t *o)
{
  o[0] = UL.calls; o[1] = UL.cbs; o[2] = UL.gpu_us; o[3] = UL.max_us; o[4] = UL.n_busy; o[5] = UL.n_cap; o[6] = UL.n_rt;
  o[7] = UL.n_late; o[8] = UL.n_deadline; o[9] = UL.n_err; o[10] = UL.n_reclaimed; o[11] = UL.part;
}
