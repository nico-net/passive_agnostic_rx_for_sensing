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
#include <cooperative_groups.h>
#include <cuda_runtime.h>
#include "nr_gpu_green.h"
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
constexpr int MAXE = 316, MAXR = 46, MAXC = 68, ZMAX = 384, NJ = NR_GPU_LDPC_MAX_JOBS, NIT = NR_GPU_LDPC_MAX_SNAP + 3;
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
};

struct Ctx {
  cudaStream_t s;
  DJob *dj;            /* pinned mapped [NJ] */
  int8_t *in;          /* pinned [NJ][MAXC*ZMAX] */
  uint8_t *snap;       /* pinned mapped [NJ][NR_GPU_LDPC_MAX_SNAP][KB_MAX] */
  int *res;            /* pinned mapped [NJ][2]: nsnap, syn0 */
  int8_t *llr, *llrRes, *c2v; /* device [NJ][MAXC*ZMAX], [NJ][MAXC*ZMAX], [NJ][MAXE*ZMAX] */
  int *unsat;          /* device [NJ][NIT] */
};
static Ctx g_ctx[CTX];
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

/* One thread-block cluster (CLB blocks) per code block: the phases are separated by cluster barriers (hardware, cheap,
 * no grid-wide residency like a cooperative launch), cross-block data is read from L2 (__ldcg). */
__global__ void __launch_bounds__(THREADS) k_ldpc(const DJob *jobs, int8_t *g_llr, int8_t *g_res, int8_t *g_c2v, int *g_unsat,
                                                  uint8_t *g_snap, int *g_out)
{
  cg::cluster_group cl = cg::this_cluster();
  __shared__ Graph G;
  const int c = blockIdx.x / CLB, b = (int)cl.block_rank(), t = threadIdx.x;
  const DJob J = jobs[c];
  {
    const int *src = (const int *)J.g;
    int *dst = (int *)&G;
    for (int i = t; i < (int)(sizeof(Graph) / sizeof(int)); i += THREADS)
      dst[i] = src[i];
  }
  __syncthreads();
  const int Z = G.Z, M = G.M, ncol = G.ncol, gt = b * THREADS + t, gs = CLB * THREADS, itm = J.it_max;
  int8_t *llr = g_llr + (size_t)c * MAXC * ZMAX, *res = g_res + (size_t)c * MAXC * ZMAX, *c2v = g_c2v + (size_t)c * MAXE * ZMAX;
  int *unsat = g_unsat + c * NIT;
  const int KB = J.K >> 3;
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
  cl.sync();
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
    cl.sync();
    const int p = it - 1; /* verdict on iteration p */
    if (p >= 1) {
      const bool last = p >= itm + 1;
      const bool syn0 = p >= 3 && __ldcg(&unsat[p]) == 0;
      if (syn0 || last) {
        if (gt == 0) {
          g_out[2 * c] = p >= 3 ? p - 2 : 0;
          g_out[2 * c + 1] = syn0 && !last;
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
            o[q] = (uint8_t)(word >> (8 * q));
        }
      }
    }
    cl.sync();
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
__device__ __forceinline__ uint32_t ld4(const int8_t *row, int start, int Z)
{
  const int a = start & ~3, sh = start & 3;
  const uint32_t w0 = __ldcg((const unsigned int *)(row + a)), w1 = __ldcg((const unsigned int *)(row + a + 4));
  const uint32_t w2 = __ldcg((const unsigned int *)row);
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
template <int D>
__device__ __forceinline__ uint32_t cn_item4(const Graph &G, int Z, int z, int e0, int deg, const int8_t *res, const int8_t *llr, int8_t *c2v)
{
  uint32_t rr[D], cv[D], v[D];
#pragma unroll
  for (int k = 0; k < D; k++) {
    const int e = e0 + (k < deg ? k : 0);
    int zb = z + G.esh[e];
    zb -= zb >= Z ? Z : 0;
    rr[k] = ld4(res + G.ecol[e] * Z, zb, Z); /* written by other blocks: L2 */
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

__global__ void __launch_bounds__(THREADS) k_ldpc4(const DJob *jobs, int8_t *g_llr, int8_t *g_res, int8_t *g_c2v, int *g_unsat,
                                                   uint8_t *g_snap, int *g_out)
{
  cg::cluster_group cl = cg::this_cluster();
  __shared__ Graph G;
  const int c = blockIdx.x / CLB, b = (int)cl.block_rank(), t = threadIdx.x;
  const DJob J = jobs[c];
  {
    const int *src = (const int *)J.g;
    int *dst = (int *)&G;
    for (int i = t; i < (int)(sizeof(Graph) / sizeof(int)); i += THREADS)
      dst[i] = src[i];
  }
  __syncthreads();
  const int Z = G.Z, Z4 = Z >> 2, M = G.M, ncol = G.ncol, gt = b * THREADS + t, gs = CLB * THREADS, itm = J.it_max;
  int8_t *llr = g_llr + (size_t)c * MAXC * ZMAX, *res = g_res + (size_t)c * MAXC * ZMAX, *c2v = g_c2v + (size_t)c * MAXE * ZMAX;
  int *unsat = g_unsat + c * NIT;
  const int KB = J.K >> 3;
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
  cl.sync();
  for (int it = 1;; it++) {
    if (it <= itm + 1) { /* ---- CN phase: one (row, 4 z) per work item ---- */
      uint32_t bad = 0;
      for (int w = gt; w < M * Z4; w += gs) {
        const int r = w / Z4, z = (w - r * Z4) << 2, e0 = G.rs[r], deg = G.rs[r + 1] - e0;
        if (deg <= 4)
          bad |= cn_item4<4>(G, Z, z, e0, deg, res, llr, c2v);
        else if (deg <= 10)
          bad |= cn_item4<10>(G, Z, z, e0, deg, res, llr, c2v);
        else
          bad |= cn_item4<19>(G, Z, z, e0, deg, res, llr, c2v);
      }
      if (it - 1 >= 3 && __syncthreads_or(bad != 0) && t == 0)
        atomicOr(&unsat[it - 1], 1);
    }
    cl.sync();
    const int p = it - 1;
    if (p >= 1) {
      const bool last = p >= itm + 1;
      const bool syn0 = p >= 3 && __ldcg(&unsat[p]) == 0;
      if (syn0 || last) {
        if (gt == 0) {
          g_out[2 * c] = p >= 3 ? p - 2 : 0;
          g_out[2 * c + 1] = syn0 && !last;
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
    cl.sync();
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

static int acquire()
{
  for (;;) {
    unsigned m = g_busy.load();
    for (int i = 0; i < CTX; i++)
      if (!(m >> i & 1u) && g_busy.compare_exchange_strong(m, m | 1u << i))
        return i;
    std::this_thread::yield();
  }
}
} // namespace nrldpc

using namespace nrldpc;

extern "C" int nr_pdsch_gpu_ldpc_init(void)
{
  static std::once_flag once;
  std::call_once(once, [] {
    int dev = 0, cl = 0;
    if (cudaGetDevice(&dev) != cudaSuccess || cudaDeviceGetAttribute(&cl, cudaDevAttrClusterLaunch, dev) != cudaSuccess || !cl
)
      return;
    int lo = 0, hi = 0;
    cudaDeviceGetStreamPriorityRange(&lo, &hi);
    for (int i = 0; i < CTX; i++) {
      Ctx &x = g_ctx[i];
      if ((!nr_gpu_green_stream(&x.s, hi, true) && cudaStreamCreateWithPriority(&x.s, cudaStreamNonBlocking, hi) != cudaSuccess)
          || cudaHostAlloc(&x.dj, sizeof(DJob) * NJ, cudaHostAllocMapped) != cudaSuccess
          || cudaHostAlloc(&x.in, (size_t)NJ * MAXC * ZMAX, cudaHostAllocMapped) != cudaSuccess
          || cudaHostAlloc(&x.snap, (size_t)NJ * NR_GPU_LDPC_MAX_SNAP * NR_GPU_LDPC_KB_MAX, cudaHostAllocMapped) != cudaSuccess
          || cudaHostAlloc(&x.res, sizeof(int) * 2 * NJ, cudaHostAllocMapped) != cudaSuccess
          || cudaMalloc(&x.llr, (size_t)NJ * MAXC * ZMAX + 16) != cudaSuccess || cudaMalloc(&x.llrRes, (size_t)NJ * MAXC * ZMAX + 16) != cudaSuccess
          || cudaMalloc(&x.c2v, (size_t)NJ * MAXE * ZMAX + 16) != cudaSuccess || cudaMalloc(&x.unsat, sizeof(int) * NJ * NIT) != cudaSuccess)
        return;
    }
    /* GPUSHARE: build every (BG, Z, R) graph now. Built lazily, each first use ran cudaMalloc + a synchronous
     * cudaMemcpy on a decode thread: up to 69 ms each behind a busy GPU (~110 of them in a 12 s replay). */
    for (int BG = 1; BG <= 2; BG++)
      for (int R : {13, 15, 23, 89})
        for (int a : {2, 3, 5, 7, 9, 11, 13, 15})
          for (int Z = a; Z <= ZMAX; Z *= 2)
            graph(BG, Z, R); /* nullptr for the (BG, R) pairs that do not exist: nothing cached */
    cudaFuncAttributes fa; /* and load both kernels now, not at the first TB (CUDA lazy loading) */
    if (cudaFuncGetAttributes(&fa, k_ldpc) != cudaSuccess || cudaFuncGetAttributes(&fa, k_ldpc4) != cudaSuccess)
      return;
    g_ok = 1;
  });
  return g_ok ? 0 : -1;
}

extern "C" int nr_pdsch_gpu_ldpc_run(nr_gpu_ldpc_job_t *jobs, int n)
{
  if (!g_ok || n < 1 || n > NJ)
    return -1;
  const Graph *gr[NJ];
  for (int k = 0; k < n; k++) {
    const nr_gpu_ldpc_job_t &j = jobs[k];
    if (j.it_max < 1 || j.it_max - 1 > NR_GPU_LDPC_MAX_SNAP || (j.K & 7) || (j.K >> 3) > NR_GPU_LDPC_KB_MAX
        || !(gr[k] = graph(j.BG, j.Z, j.R)))
      return -1;
  }
  const int c = acquire();
  Ctx &x = g_ctx[c];
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
  cudaLaunchConfig_t cfg = {};
  cfg.gridDim = dim3(n * CLB);
  cfg.blockDim = dim3(THREADS);
  cfg.stream = x.s;
  cudaLaunchAttribute at[1];
  at[0].id = cudaLaunchAttributeClusterDimension;
  at[0].val.clusterDim.x = CLB;
  at[0].val.clusterDim.y = 1;
  at[0].val.clusterDim.z = 1;
  cfg.attrs = at;
  cfg.numAttrs = 1;
  bool z4 = true; /* the 4-lane kernel when every block's Z is a multiple of 4 */
  for (int k = 0; k < n; k++)
    z4 = z4 && !(jobs[k].Z & 3);
  cudaError_t e = cudaLaunchKernelEx(&cfg, z4 ? k_ldpc4 : k_ldpc, (const DJob *)ddj, x.llr, x.llrRes, x.c2v, x.unsat, dsnap, dres);
  /* GPUSHARE: poll like the demod/FEP waits. cudaStreamSynchronize under cudaDeviceScheduleBlockingSync (set by
   * nr_gpu_platform) sleeps on the completion interrupt; the TB waited far longer than its kernel. */
  if (e == cudaSuccess)
    while ((e = cudaStreamQuery(x.s)) == cudaErrorNotReady) {}
  if (e != cudaSuccess) {
    fprintf(stderr, "nr_pdsch_gpu_ldpc_run: %s\n", cudaGetErrorString(e));
    g_busy.fetch_and(~(1u << c));
    return -1;
  }
  int rc = 0;
  for (int k = 0; k < n; k++) {
    nr_gpu_ldpc_job_t &j = jobs[k];
    j.nsnap = x.res[2 * k];
    j.syn0 = x.res[2 * k + 1];
    if (j.nsnap < 0 || j.nsnap > NR_GPU_LDPC_MAX_SNAP) {
      rc = -1;
      continue;
    }
    for (int s = 0; s < j.nsnap; s++)
      memcpy(j.snap[s], x.snap + ((size_t)k * NR_GPU_LDPC_MAX_SNAP + s) * NR_GPU_LDPC_KB_MAX, j.K >> 3);
  }
  g_busy.fetch_and(~(1u << c));
  return rc;
}
