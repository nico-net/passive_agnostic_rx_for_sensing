/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */
/*! \file ldpc_cb0.cu
 * \brief CB0 batch entry of libldpc_cuda.so (nrLDPC_cb0_cuda.h): many already-dematched code blocks, mixed BG / Z /
 * iterations, one asynchronous submission, CRC and all-zero guard on the GPU.
 *
 * Per submission (all on this module's own low-priority stream, nothing synchronous):
 *   items are grouped by (BG, Z, iterations); each group is cut into L2-sized chunks (the iteration loop of a chunk
 *   then runs from L2 instead of DRAM); per chunk: prep (gather the chunk's inputs from the caller's buffer -- read in
 *   place on an integrated GPU -- into the fixed work buffer, reset the early-termination state), the iteration loop
 *   (a CUDA graph per (BG, Z, batch rounded up to a power of two, iterations), replayed), finish (hard decision, CRC,
 *   guard, result). All chunks of all groups are enqueued back to back; one host callback at the end wakes collect.
 *
 * Decoder = G1's (ldpc_decoder.cu update_cn_kernel / update_vn_kernel, batch_syndrome_kernel): normalised x3/4
 * flooding min-sum with the same int8 saturation, the same per-thread edge partition (UNROLL_NODES = 4) and the same
 * tie / clamp order, so the bits are identical to ldpc_pool_decode. One change of schedule, not of result: the syndrome
 * of iteration i is evaluated inside the check-node kernel of iteration i + 1 (it reads the same posterior anyway) and
 * the variable-node kernel of i + 1 stops a converged codeword before it writes, so the posterior a codeword ends with
 * is exactly G1's (2 kernels per iteration instead of 4; tested bit-exact against ldpc_pool_decode and a CPU model).
 */
#include <cuda_runtime.h>
#include <pthread.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <algorithm>
#include <map>
#include <vector>

#include "nrLDPC_cb0_cuda.h"
#include "ldpc_cuda_bg.h"

namespace {

constexpr uint32_t MAX_Z = 384, MAX_COLS = 68, MAX_ROWS = 46;
constexpr uint32_t CH = MAX_COLS * MAX_Z;                    /* chan / posterior stride per codeword (bytes) */
constexpr size_t MSG_MAX = (size_t)MAX_ROWS * MAX_COLS * MAX_Z; /* G1's message layout, per codeword at BG1 Z=384 */
constexpr int UNROLL = 4; /* = ldpc_decoder.cu UNROLL_NODES: G1's per-thread edge partition, reproduced */
constexpr uint32_t CRC_MAX_BITS = 8448;
constexpr int FIN_THREADS = 256;
constexpr int MAX_SLOTS = 4;
constexpr int MAX_LLR = 127;

inline uint32_t blocks_for(uint32_t n, uint32_t b) { return (n + b - 1) / b; }

/* per item, sorted order: what prep / finish need */
struct DMeta {
  uint32_t item;   /* index in the caller's arrays */
  uint16_t K, Kprime, guard;
  uint8_t crc_type, pad;
};

/* ---------------------------------------------------------------- kernels ---------------------------------------- */

/* Check nodes of iteration `iter`: G1's update_cn_kernel, re-scheduled as ONE thread per check (row, i) that walks all
 * the row's edges (independent loads: the old 4-way split across threadIdx.y plus two __syncthreads made the kernel
 * latency bound). G1's arithmetic is reproduced exactly: its thread y handled edges ii = y, y+4, ... in order with
 * strict-< minima, then the 4 partials were merged in y order; here the 4 partials live in registers (the inner loop is
 * unrolled over y) and are merged the same way. idx_min is kept as the edge index (unique per edge, so the same
 * comparison as G1's message index). Shifts come pre-reduced mod Z (cn = col | (s mod Z) << 16).
 * Plus (iter > 0) the syndrome of the posterior it reads (iteration iter - 1): unsat[(iter & 1) * nb + b] |= any
 * unsatisfied check. */
#define CB0_CN_THREADS 128
/* NCW codewords per thread (blockIdx.y * NCW + c): the same (row, i) of NCW codewords share the table loads and the
 * index arithmetic, and their loads are independent (more in flight per thread). Each codeword's arithmetic is the
 * single-codeword one above, unchanged. */
template <int NCW>
__launch_bounds__(CB0_CN_THREADS)
__global__ void cb0_cn_kernel(const int8_t* __restrict__ chan, const int8_t* __restrict__ total, int8_t* __restrict__ msg,
                              uint32_t Z, const uint32_t* __restrict__ bg_cn, const uint32_t* __restrict__ bg_cn_degree,
                              uint32_t max_degree, uint32_t num_rows, uint32_t iter, uint32_t msg_stride,
                              const uint32_t* __restrict__ done, uint32_t* __restrict__ unsat, uint32_t nb)
{
  const uint32_t b0 = blockIdx.y * NCW;
  bool act[NCW];
  bool any = false;
#pragma unroll
  for (int c = 0; c < NCW; c++) {
    act[c] = !done[b0 + c];
    any |= act[c];
  }
  if (!any)
    return; /* whole block: converged codewords (or padding) */
  const int8_t* __restrict__ lt = (iter == 0 ? chan : total) + (size_t)b0 * CH;
  int8_t* __restrict__ mg = msg + (size_t)b0 * msg_stride;
  const uint32_t tid = blockIdx.x * blockDim.x + threadIdx.x;
  const uint32_t i = tid % Z, row = tid / Z;
  int unsat_row[NCW];
#pragma unroll
  for (int c = 0; c < NCW; c++)
    unsat_row[c] = 0;
  if (row < num_rows) {
    const uint32_t deg = bg_cn_degree[row];
    const uint32_t* __restrict__ e = &bg_cn[row * max_degree];
    int m1[NCW][UNROLL], m2[NCW][UNROLL], im[NCW][UNROLL];
    uint32_t sg[NCW], ts[NCW];
#pragma unroll
    for (int c = 0; c < NCW; c++) {
      sg[c] = ts[c] = 0;
#pragma unroll
      for (int y = 0; y < UNROLL; y++) {
        m1[c][y] = INT_MAX;
        m2[c][y] = INT_MAX;
        im[c][y] = -1;
      }
    }
    for (uint32_t base = 0; base < deg; base += UNROLL) {
#pragma unroll
      for (int y = 0; y < UNROLL; y++) {
        const uint32_t ii = base + y;
        if (ii < deg) {
          const uint32_t cn = e[ii];
          const uint32_t col = cn & 0xffffu, s = cn >> 16;
          const uint32_t q = i + s;
          const uint32_t ot = col * Z + (q >= Z ? q - Z : q), om = (row + col * num_rows) * Z + i;
#pragma unroll
          for (int c = 0; c < NCW; c++) {
            if (!act[c])
              continue;
            const int tt = lt[(size_t)c * CH + ot];
            ts[c] |= (uint32_t)(tt < 0) << ii;
            int t = tt;
            if (iter != 0)
              t -= mg[(size_t)c * msg_stride + om];
            sg[c] |= (uint32_t)(t < 0) << ii;
            const int a = abs(t);
            if (a < m1[c][y]) {
              m2[c][y] = m1[c][y];
              m1[c][y] = a;
              im[c][y] = (int)ii;
            } else if (a < m2[c][y])
              m2[c][y] = a;
          }
        }
      }
    }
    int a1[NCW], a2[NCW], ai[NCW], ns[NCW];
#pragma unroll
    for (int c = 0; c < NCW; c++) {
      a1[c] = INT_MAX;
      a2[c] = INT_MAX;
      ai[c] = -1;
#pragma unroll
      for (int y = 0; y < UNROLL; y++) { /* G1's merge: y order, strict < */
        if (m1[c][y] < a1[c]) {
          a2[c] = a1[c];
          a1[c] = m1[c][y];
          ai[c] = im[c][y];
        } else if (m1[c][y] < a2[c])
          a2[c] = m1[c][y];
        a2[c] = min(a2[c], m2[c][y]);
      }
      ns[c] = (__popc(sg[c]) & 1) ? -1 : 1;
      unsat_row[c] = act[c] && (__popc(ts[c]) & 1);
      a1[c] = min(max(a1[c] * 3 / 4, -MAX_LLR), MAX_LLR); /* APPLY_DAMPING_INT, clip */
      a2[c] = min(max(a2[c] * 3 / 4, -MAX_LLR), MAX_LLR);
    }
    for (uint32_t ii = 0; ii < deg; ii++) {
      const uint32_t col = e[ii] & 0xffffu;
      const uint32_t om = (row + col * num_rows) * Z + i;
#pragma unroll
      for (int c = 0; c < NCW; c++) {
        if (!act[c])
          continue;
        const int v = ((int)ii == ai[c]) ? a2[c] : a1[c];
        mg[(size_t)c * msg_stride + om] = (int8_t)(v * ns[c] * (((sg[c] >> ii) & 1) ? -1 : 1));
      }
    }
  }
  if (iter != 0) { /* uniform per block */
#pragma unroll
    for (int c = 0; c < NCW; c++)
      if (__syncthreads_or(unsat_row[c]) && threadIdx.x == 0)
        atomicOr(&unsat[(iter & 1) * nb + b0 + c], 1u);
  }
}

/* Variable nodes of iteration `iter`: G1's update_vn_kernel, one thread per (col, i) walking all the column's edges;
 * G1's 4 partial sums (edges j = y, y+4, ..., channel LLR added to partial 0, each partial clamped, then the clamped
 * sum of the partials) are reproduced in registers. A codeword whose posterior of iteration iter - 1 satisfied every
 * check (unsat == 0, from this iteration's check-node kernel) stops here, before writing: its posterior stays the one
 * G1 stops with, iters_out = iter. Shifts pre-reduced mod Z (vn = row | (s mod Z) << 16). NCW as the CN kernel. */
template <int NCW>
__launch_bounds__(CB0_CN_THREADS)
__global__ void cb0_vn_kernel(const int8_t* __restrict__ msg, const int8_t* __restrict__ chan, int8_t* __restrict__ total,
                              uint32_t Z, const uint32_t* __restrict__ bg_vn, const uint32_t* __restrict__ bg_vn_degree,
                              uint32_t max_degree, uint32_t num_cols, uint32_t num_rows, uint32_t iter, uint32_t msg_stride,
                              uint32_t* __restrict__ done, uint32_t* __restrict__ unsat, uint32_t nb,
                              uint8_t* __restrict__ iters_out)
{
  const uint32_t b0 = blockIdx.y * NCW;
  const bool lead = blockIdx.x == 0 && threadIdx.x == 0;
  bool act[NCW];
  bool any = false;
#pragma unroll
  for (int c = 0; c < NCW; c++) {
    const uint32_t b = b0 + c;
    if (lead)
      unsat[((iter + 1) & 1) * nb + b] = 0; /* for the next check-node kernel (last read by the previous VN kernel) */
    act[c] = !done[b];
    if (act[c] && iter != 0 && unsat[(iter & 1) * nb + b] == 0) {
      if (lead) {
        done[b] = 1;
        iters_out[b] = (uint8_t)iter;
      }
      act[c] = false;
    }
    any |= act[c];
  }
  if (!any)
    return;
  const int8_t* __restrict__ mg = msg + (size_t)b0 * msg_stride;
  const uint32_t tid = blockIdx.x * blockDim.x + threadIdx.x;
  const uint32_t i = tid % Z, col = tid / Z;
  if (col >= num_cols)
    return;
  const uint32_t deg = bg_vn_degree[col];
  const uint32_t* __restrict__ e = &bg_vn[col * max_degree];
  int p[NCW][UNROLL];
#pragma unroll
  for (int c = 0; c < NCW; c++)
#pragma unroll
    for (int y = 0; y < UNROLL; y++)
      p[c][y] = 0;
  for (uint32_t base = 0; base < deg; base += UNROLL) {
#pragma unroll
    for (int y = 0; y < UNROLL; y++) {
      const uint32_t j = base + y;
      if (j < deg) {
        const uint32_t vn = e[j];
        const uint32_t row = vn & 0xffffu, s = vn >> 16;
        const uint32_t om = (row + col * num_rows) * Z + (i >= s ? i - s : i + Z - s);
#pragma unroll
        for (int c = 0; c < NCW; c++)
          if (act[c])
            p[c][y] += mg[(size_t)c * msg_stride + om];
      }
    }
  }
  const size_t o = (size_t)b0 * CH + col * Z + i;
#pragma unroll
  for (int c = 0; c < NCW; c++) {
    if (!act[c])
      continue;
    p[c][0] += chan[o + (size_t)c * CH];
    int sum = 0;
#pragma unroll
    for (int y = 0; y < UNROLL; y++)
      sum += min(max(p[c][y], -MAX_LLR), MAX_LLR);
    total[o + (size_t)c * CH] = (int8_t)min(max(sum, -MAX_LLR), MAX_LLR);
  }
}

/* (BG, Z) copy of the base-graph edge tables with the shifts reduced mod Z (raw V up to 383 > Z for small Z). */
__global__ void cb0_modz_kernel(const uint32_t* __restrict__ in, uint32_t* __restrict__ out, uint32_t n, uint32_t Z)
{
  const uint32_t k = blockIdx.x * blockDim.x + threadIdx.x;
  if (k < n)
    out[k] = (in[k] & 0xffffu) | ((in[k] >> 16) % Z) << 16;
}

/* Gather the chunk's inputs into the fixed work buffer and reset its early-termination state. grid (x, nb). */
__global__ void cb0_prep_kernel(const int8_t* __restrict__ src, size_t src_stride, const DMeta* __restrict__ meta,
                                uint32_t n, uint32_t nb, uint32_t KcZ, int8_t* __restrict__ chan, uint32_t* __restrict__ done,
                                uint32_t* __restrict__ unsat, uint8_t* __restrict__ iters_out, uint32_t num_iter)
{
  const uint32_t b = blockIdx.y;
  if (blockIdx.x == 0 && threadIdx.x == 0) {
    done[b] = b >= n; /* padding codewords [n, nb) never run */
    unsat[b] = 0;
    unsat[nb + b] = 0;
    iters_out[b] = (uint8_t)num_iter;
  }
  if (b >= n)
    return;
  const int8_t* s = src + (size_t)meta[b].item * src_stride;
  int8_t* d = chan + (size_t)b * CH;
  const uint32_t x = (blockIdx.x * blockDim.x + threadIdx.x) * 16;
  if (x >= KcZ)
    return;
  if (x + 16 <= KcZ && !(((uintptr_t)(s + x)) & 15)) {
    *(uint4*)(d + x) = *(const uint4*)(s + x);
  } else {
    for (uint32_t k = x; k < x + 16 && k < KcZ; k++)
      d[k] = s[k];
  }
}

/* Hard decision of K bits (MSB first, as pack_bits_kernel), CRC (OAI check_crc: remainder of the K' bits, data then
 * CRC, divided by the generator is 0), all-zero guard. One block per codeword. */
__global__ void cb0_finish_kernel(const int8_t* __restrict__ post, const DMeta* __restrict__ meta, uint32_t n,
                                  const uint8_t* __restrict__ iters_in, uint32_t num_iter,
                                  const uint32_t* __restrict__ crc_pow, ldpc_cb0_result_t* __restrict__ res,
                                  uint8_t* __restrict__ bits_out)
{
  const uint32_t b = blockIdx.x;
  if (b >= n)
    return;
  const DMeta m = meta[b];
  const int8_t* p = post + (size_t)b * CH;
  __shared__ uint8_t by[LDPC_CB0_BITS_STRIDE];
  __shared__ uint32_t red[FIN_THREADS / 32];
  const uint32_t nbytes = (m.K + 7u) >> 3;
  for (uint32_t j = threadIdx.x; j < nbytes; j += blockDim.x) {
    uint32_t v = 0;
    for (uint32_t k = 0; k < 8; k++) {
      const uint32_t idx = j * 8 + k;
      v |= (uint32_t)(idx < m.K && p[idx] < 0) << (7 - k);
    }
    by[j] = (uint8_t)v;
    if (bits_out)
      bits_out[(size_t)m.item * LDPC_CB0_BITS_STRIDE + j] = (uint8_t)v;
  }
  __syncthreads();
  /* remainder = XOR over set bits j < K' of x^(K'-1-j) mod g */
  const uint32_t* pw = crc_pow + (size_t)(m.crc_type > 2 ? 0 : m.crc_type) * CRC_MAX_BITS;
  uint32_t rem = 0, nz = 0;
  const uint32_t kb = m.Kprime >> 3;
  for (uint32_t j = threadIdx.x; j < kb; j += blockDim.x) {
    const uint32_t v = by[j];
    for (uint32_t k = 0; k < 8; k++)
      if ((v >> (7 - k)) & 1)
        rem ^= pw[m.Kprime - 1 - (j * 8 + k)];
  }
  for (uint32_t j = threadIdx.x; j < m.guard; j += blockDim.x)
    nz |= by[j];
  for (int o = 16; o > 0; o >>= 1)
    rem ^= __shfl_xor_sync(0xffffffffu, rem, o);
  if ((threadIdx.x & 31) == 0)
    red[threadIdx.x >> 5] = rem;
  const int any_nz = __syncthreads_or(nz != 0);
  if (threadIdx.x == 0) {
    uint32_t r = 0;
    for (int w = 0; w < FIN_THREADS / 32; w++)
      r ^= red[w];
    ldpc_cb0_result_t o = {};
    o.crc_ok = r == 0 ? 1 : 0;
    o.iters = iters_in[b];
    o.converged = iters_in[b] < num_iter;
    o.zero = m.guard ? !any_nz : 0;
    o.decoder_used = 2; /* NRLDPC_DECODER_CUDA_FLOODING */
    o.err = 0;
    res[m.item] = o;
  }
}

__global__ void cb0_stall_kernel(unsigned long long ns) /* test hook: keeps the stream busy */
{
  unsigned long long t0;
  asm volatile("mov.u64 %0, %%globaltimer;" : "=l"(t0));
  for (;;) {
    unsigned long long t;
    asm volatile("mov.u64 %0, %%globaltimer;" : "=l"(t));
    if (t - t0 >= ns)
      break;
    __nanosleep(100000);
  }
}

/* ---------------------------------------------------------------- host state ------------------------------------- */

enum { SLOT_FREE, SLOT_INFLIGHT, SLOT_DONE, SLOT_ABANDONED };
enum { MODE_NONE = 0, MODE_UNIFIED = 1, MODE_EXPLICIT = 2 };

struct Slot {
  int state = SLOT_FREE; /* guarded by g_state_mu */
  int idx = 0;
  int n = 0, want_bits = 0;
  long long deadline_ns = 0;
  /* host side (pinned; in UNIFIED mode mapped and written by the GPU directly) */
  DMeta* meta_h = nullptr;
  ldpc_cb0_result_t* res_h = nullptr;
  uint8_t* bits_h = nullptr;
  int8_t* valid = nullptr; /* plain host: item i valid */
  /* device side (EXPLICIT); UNIFIED: device views of the host buffers */
  DMeta* meta_d = nullptr;
  ldpc_cb0_result_t* res_d = nullptr;
  uint8_t* bits_d = nullptr;
  int8_t* stage_h = nullptr; /* EXPLICIT: pinned staging, cap * CH */
  int8_t* arena_d = nullptr; /* EXPLICIT: device inputs, cap * CH */
  cudaEvent_t ev_in = nullptr, ev_done = nullptr;
};

struct Ctx {
  int state = -1; /* -1 not initialised, 0 unusable, 1 ready */
  int mode = MODE_NONE, integrated = 0, cma = 0;
  int cap = 2048, nslots = 3, chunk_cfg = 0;
  size_t l2 = 0;
  cudaStream_t s = nullptr, s_copy = nullptr;
  int8_t *chan = nullptr, *total = nullptr, *msg = nullptr;
  uint32_t *done = nullptr, *unsat = nullptr;
  uint8_t* iters = nullptr;
  uint32_t* crc_pow = nullptr;
  uint32_t chunk_max = 256; /* codewords in the work buffers (at BG1 Z=384) */
  size_t msg_bytes = 0;
  Slot slot[MAX_SLOTS];
  std::map<uint64_t, cudaGraphExec_t> graphs;
  bool use_graph = true;
};
Ctx g;
pthread_mutex_t g_init_mu = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t g_submit_mu = PTHREAD_MUTEX_INITIALIZER; /* serialises enqueues on the stream (CUDA calls) */
pthread_mutex_t g_state_mu = PTHREAD_MUTEX_INITIALIZER;  /* slot states only: never held across a CUDA call */
pthread_cond_t g_state_cv;

/* tunables + breaker (G1 semantics, CB0's own state) */
volatile int g_timeout_ms = 200, g_timeout_us_item = 100, g_breaker_n = 4, g_breaker_ms = 5000;
volatile int g_inject = 0, g_stall_ms = 0;
int g_consec = 0, g_perm_off = 0, g_sticky = 0; /* under g_state_mu */
long long g_bypass_until_ns = 0;
uint64_t c_submits, c_items, c_ok, c_err, c_tmo, c_busy, c_byp, c_sticky, c_trips, c_graphs, c_h2d;

#define CADD(v, x) __atomic_fetch_add(&(v), (x), __ATOMIC_RELAXED)
#define CLD(v) __atomic_load_n(&(v), __ATOMIC_RELAXED)

long long mono_ns()
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
}

int env_int(const char* a, const char* b, int def)
{
  const char* e = getenv(a);
  if ((!e || !*e) && b)
    e = getenv(b);
  return e && *e ? atoi(e) : def;
}

/* x^k mod g for k < CRC_MAX_BITS; g = x^L + low (OAI's top-aligned 32-bit polynomials, crc_byte.c) */
void crc_pow_table(uint32_t poly_top, int L, uint32_t* out)
{
  const uint32_t low = poly_top >> (32 - L), mask = (L == 32) ? 0xffffffffu : ((1u << L) - 1);
  uint32_t r = 1;
  for (uint32_t k = 0; k < CRC_MAX_BITS; k++) {
    out[k] = r;
    const uint32_t top = (r >> (L - 1)) & 1;
    r = ((r << 1) & mask) ^ (top ? low : 0);
  }
}

void slot_free_buffers(Slot& s)
{
  if (s.meta_h) cudaFreeHost(s.meta_h);
  if (s.res_h) cudaFreeHost(s.res_h);
  if (s.bits_h) cudaFreeHost(s.bits_h);
  if (s.stage_h) cudaFreeHost(s.stage_h);
  if (s.arena_d) cudaFree(s.arena_d);
  if (g.mode == MODE_EXPLICIT) {
    if (s.meta_d) cudaFree(s.meta_d);
    if (s.res_d) cudaFree(s.res_d);
    if (s.bits_d) cudaFree(s.bits_d);
  }
  free(s.valid);
  const int idx = s.idx;
  cudaEvent_t a = s.ev_in, b = s.ev_done;
  s = Slot();
  s.idx = idx;
  s.ev_in = a;
  s.ev_done = b;
}

/* (Re)allocate every slot's buffers for `mode`. Idle only (init, or the test switch). */
int slots_alloc(int mode)
{
  for (int k = 0; k < g.nslots; k++)
    slot_free_buffers(g.slot[k]);
  g.mode = mode;
  for (int k = 0; k < g.nslots; k++) {
    Slot& s = g.slot[k];
    const size_t n = (size_t)g.cap;
    if (cudaHostAlloc(&s.meta_h, n * sizeof(DMeta), cudaHostAllocMapped) != cudaSuccess
        || cudaHostAlloc(&s.res_h, n * sizeof(ldpc_cb0_result_t), cudaHostAllocMapped) != cudaSuccess
        || cudaHostAlloc(&s.bits_h, n * LDPC_CB0_BITS_STRIDE, cudaHostAllocMapped) != cudaSuccess)
      return -1;
    if (!(s.valid = (int8_t*)calloc(n, 1)))
      return -1;
    if (mode == MODE_UNIFIED) {
      if (cudaHostGetDevicePointer((void**)&s.meta_d, s.meta_h, 0) != cudaSuccess
          || cudaHostGetDevicePointer((void**)&s.res_d, s.res_h, 0) != cudaSuccess
          || cudaHostGetDevicePointer((void**)&s.bits_d, s.bits_h, 0) != cudaSuccess)
        return -1;
    } else {
      if (cudaMalloc(&s.meta_d, n * sizeof(DMeta)) != cudaSuccess
          || cudaMalloc(&s.res_d, n * sizeof(ldpc_cb0_result_t)) != cudaSuccess
          || cudaMalloc(&s.bits_d, n * LDPC_CB0_BITS_STRIDE) != cudaSuccess
          || cudaHostAlloc(&s.stage_h, n * CH, cudaHostAllocDefault) != cudaSuccess
          || cudaMalloc(&s.arena_d, n * CH) != cudaSuccess)
        return -1;
    }
    if (!s.ev_in && cudaEventCreateWithFlags(&s.ev_in, cudaEventDisableTiming) != cudaSuccess)
      return -1;
    if (!s.ev_done && cudaEventCreateWithFlags(&s.ev_done, cudaEventDisableTiming) != cudaSuccess)
      return -1;
  }
  return 0;
}

int init_locked()
{
  if (g.state >= 0)
    return g.state;
  g.state = 0;
  {
    pthread_condattr_t at;
    pthread_condattr_init(&at);
    pthread_condattr_setclock(&at, CLOCK_MONOTONIC);
    pthread_cond_init(&g_state_cv, &at);
    pthread_condattr_destroy(&at);
  }
  g_timeout_ms = env_int("LDPC_CB0_TIMEOUT_MS", "LDPC_CUDA_TIMEOUT_MS", 200);
  if (g_timeout_ms <= 0) g_timeout_ms = 200;
  g_timeout_us_item = env_int("LDPC_CB0_TIMEOUT_US_PER_ITEM", nullptr, 100);
  if (g_timeout_us_item < 0) g_timeout_us_item = 100;
  g_breaker_n = env_int("LDPC_CB0_BREAKER_N", "LDPC_CUDA_BREAKER_N", 4);
  if (g_breaker_n <= 0) g_breaker_n = 4;
  g_breaker_ms = env_int("LDPC_CB0_BREAKER_S", "LDPC_CUDA_BREAKER_S", 5) * 1000;
  if (g_breaker_ms <= 0) g_breaker_ms = 5000;
  g_inject = env_int("LDPC_CB0_TEST_INJECT", nullptr, 0);
  g_stall_ms = env_int("LDPC_CB0_TEST_STALL_MS", nullptr, 0);
  g.cap = std::min(std::max(env_int("LDPC_CB0_MAX_ITEMS", nullptr, 2048), 1), 65536);
  g.nslots = std::min(std::max(env_int("LDPC_CB0_SLOTS", nullptr, 3), 1), MAX_SLOTS);
  g.chunk_cfg = env_int("LDPC_CB0_CHUNK", nullptr, 0);
  g.use_graph = env_int("LDPC_CB0_GRAPH", "LDPC_CUDA_GRAPH", 1) != 0;

  ldpc_cuda_bg_t bg;
  if (ldpc_cuda_basegraph(1, 384, &bg) != 0) /* tables first: sets cudaDeviceScheduleBlockingSync before the context */
    return 0;
  int dev = 0, v = 0;
  if (cudaGetDevice(&dev) != cudaSuccess)
    return 0;
  cudaDeviceGetAttribute(&g.integrated, cudaDevAttrIntegrated, dev);
  cudaDeviceGetAttribute(&g.cma, cudaDevAttrConcurrentManagedAccess, dev);
  if (cudaDeviceGetAttribute(&v, cudaDevAttrL2CacheSize, dev) == cudaSuccess)
    g.l2 = (size_t)v;
  /* UNIFIED only on an integrated GPU: a discrete GPU with HMM also reports pageable access, but every access
   * would then cross PCIe. LDPC_CB0_MEM=explicit forces the copy path (tests on GB10). */
  const char* me = getenv("LDPC_CB0_MEM");
  int mode = g.integrated ? MODE_UNIFIED : MODE_EXPLICIT;
  if (me && !strcmp(me, "explicit"))
    mode = MODE_EXPLICIT;
  int lo = 0, hi = 0;
  cudaDeviceGetStreamPriorityRange(&lo, &hi); /* lo = numerically greatest = lowest priority: TB decodes go first */
  if (cudaStreamCreateWithPriority(&g.s, cudaStreamNonBlocking, lo) != cudaSuccess
      || cudaStreamCreateWithPriority(&g.s_copy, cudaStreamNonBlocking, lo) != cudaSuccess)
    return 0;
  g.chunk_max = (uint32_t)std::min(std::max(env_int("LDPC_CB0_WORK_CW", nullptr, 256), 16), 512);
  g.msg_bytes = (size_t)g.chunk_max * MSG_MAX;
  if (cudaMalloc(&g.msg, g.msg_bytes) != cudaSuccess || cudaMalloc(&g.chan, (size_t)g.chunk_max * 8 * CH) != cudaSuccess
      || cudaMalloc(&g.total, (size_t)g.chunk_max * 8 * CH) != cudaSuccess
      || cudaMalloc(&g.done, (size_t)g.chunk_max * 8 * sizeof(uint32_t)) != cudaSuccess
      || cudaMalloc(&g.unsat, (size_t)g.chunk_max * 8 * 2 * sizeof(uint32_t)) != cudaSuccess
      || cudaMalloc(&g.iters, (size_t)g.chunk_max * 8) != cudaSuccess
      || cudaMalloc(&g.crc_pow, 3 * CRC_MAX_BITS * sizeof(uint32_t)) != cudaSuccess)
    return 0;
  std::vector<uint32_t> pw(3 * CRC_MAX_BITS);
  crc_pow_table(0x864cfb00u, 24, &pw[0]);                /* CRC24_A */
  crc_pow_table(0x80006300u, 24, &pw[CRC_MAX_BITS]);     /* CRC24_B */
  crc_pow_table(0x10210000u, 16, &pw[2 * CRC_MAX_BITS]); /* CRC16 */
  if (cudaMemcpy(g.crc_pow, pw.data(), pw.size() * sizeof(uint32_t), cudaMemcpyHostToDevice) != cudaSuccess)
    return 0;
  for (int k = 0; k < MAX_SLOTS; k++)
    g.slot[k].idx = k;
  if (slots_alloc(mode) != 0)
    return 0;
  g.state = 1;
  return 1;
}

/* Codewords per chunk for (BG, Z): the chunk's working set (messages + posterior + input) about one L2,
 * a power of two, at most what the work buffers hold. LDPC_CB0_CHUNK overrides. */
uint32_t chunk_for(uint32_t rows, uint32_t cols, uint32_t edges, uint32_t Z)
{
  const uint32_t room = (uint32_t)std::min<size_t>(g.msg_bytes / ((size_t)rows * cols * Z), (size_t)g.chunk_max * 8);
  uint32_t c;
  if (g.chunk_cfg > 0) {
    c = (uint32_t)g.chunk_cfg;
  } else {
    const size_t ws = (size_t)edges + 2u * cols * Z + (size_t)cols * Z; /* msg + posterior + input */
    /* measured on GB10 (24 MB L2), BG1 Z=384 wrong-heavy N=600: chunk 64 / 128 / 256 / 512 = 81 / 76 / - / 91 us per CB0
     * (task-CB0GPU report): about one L2 of working set per chunk */
    const size_t budget = g.l2 ? g.l2 + g.l2 / 8 : (size_t)24 << 20;
    c = (uint32_t)std::max<size_t>(budget / ws, 1);
  }
  uint32_t p = 1;
  while (p * 2 <= c)
    p *= 2;
  p = std::min<uint32_t>(p, 512);
  while (p > room)
    p >>= 1;
  return std::max<uint32_t>(p, 1);
}

/* codewords per thread for a chunk of nb (a power of two): LDPC_CB0_NCW (1, 2, 4), default 2, at most nb */
uint32_t ncw_for(uint32_t nb)
{
  static const int cfg = [] {
    const char* e = getenv("LDPC_CB0_NCW");
    const int v = e ? atoi(e) : 2;
    return v >= 4 ? 4 : v >= 2 ? 2 : 1;
  }();
  return (uint32_t)std::min<uint32_t>((uint32_t)cfg, nb);
}

/* (BG, Z) edge tables with the shifts reduced mod Z, built once per pair on the stream (stream-ordered before use). */
struct ModZ {
  uint32_t *cn = nullptr, *vn = nullptr;
};
std::map<uint32_t, ModZ> g_modz; /* under g_submit_mu */
int modz_tables(const ldpc_cuda_bg_t& bg, uint32_t BG, uint32_t Z, ModZ* out)
{
  ModZ& m = g_modz[BG << 16 | Z];
  if (!m.cn) {
    const uint32_t ncn = bg.num_rows * bg.cn_stride, nvn = bg.num_cols * bg.vn_stride;
    uint32_t *cn = nullptr, *vn = nullptr;
    cudaError_t e = cudaMalloc(&cn, ncn * sizeof(uint32_t));
    if (!e) e = cudaMalloc(&vn, nvn * sizeof(uint32_t));
    if (e) {
      cudaFree(cn);
      cudaFree(vn);
      return (int)e;
    }
    cb0_modz_kernel<<<blocks_for(ncn, 256), 256, 0, g.s>>>(bg.cn, cn, ncn, Z);
    cb0_modz_kernel<<<blocks_for(nvn, 256), 256, 0, g.s>>>(bg.vn, vn, nvn, Z);
    if ((e = cudaPeekAtLastError()))
      return (int)e;
    m.cn = cn;
    m.vn = vn;
  }
  *out = m;
  return 0;
}

/* The iteration loop of one chunk: captured once per key, replayed. On error: rc != 0, capture aborted. */
int launch_iterations(const ldpc_cuda_bg_t& bg, uint32_t BG, uint32_t Z, uint32_t nb, uint32_t iters)
{
  const uint32_t msg_stride = bg.num_rows * bg.num_cols * Z;
  ModZ mz;
  if (int rc = modz_tables(bg, BG, Z, &mz))
    return rc;
  const uint32_t ncw = ncw_for(nb);
  auto body = [&]() {
    const dim3 bcn(blocks_for(bg.num_rows * Z, CB0_CN_THREADS), nb / ncw),
        bvn(blocks_for(bg.num_cols * Z, CB0_CN_THREADS), nb / ncw);
    for (uint32_t i = 0; i < iters; i++) {
#define CB0_LAUNCH(N)                                                                                                   \
  do {                                                                                                                  \
    cb0_cn_kernel<N><<<bcn, CB0_CN_THREADS, 0, g.s>>>(g.chan, g.total, g.msg, Z, mz.cn, bg.cn_degree, bg.cn_stride,     \
                                                      bg.num_rows, i, msg_stride, g.done, g.unsat, nb);                 \
    cb0_vn_kernel<N><<<bvn, CB0_CN_THREADS, 0, g.s>>>(g.msg, g.chan, g.total, Z, mz.vn, bg.vn_degree, bg.vn_stride,     \
                                                      bg.num_cols, bg.num_rows, i, msg_stride, g.done, g.unsat, nb,    \
                                                      g.iters);                                                         \
  } while (0)
      if (ncw == 4)
        CB0_LAUNCH(4);
      else if (ncw == 2)
        CB0_LAUNCH(2);
      else
        CB0_LAUNCH(1);
#undef CB0_LAUNCH
    }
  };
  if (!g.use_graph) {
    body();
    return (int)cudaPeekAtLastError();
  }
  const uint64_t key = (uint64_t)BG << 48 | (uint64_t)Z << 32 | (uint64_t)nb << 8 | iters;
  auto it = g.graphs.find(key);
  if (it != g.graphs.end())
    return (int)cudaGraphLaunch(it->second, g.s);
  if (g.graphs.size() >= 2048) { /* never evict (no device sync on the hot path): direct launches instead */
    body();
    return (int)cudaPeekAtLastError();
  }
  cudaError_t e = cudaStreamBeginCapture(g.s, cudaStreamCaptureModeThreadLocal);
  if (e)
    return (int)e;
  body();
  cudaGraph_t gr = nullptr;
  e = cudaStreamEndCapture(g.s, &gr);
  if (e) {
    if (gr) cudaGraphDestroy(gr);
    return (int)e;
  }
  cudaGraphExec_t ge = nullptr;
  e = cudaGraphInstantiate(&ge, gr, 0);
  cudaGraphDestroy(gr);
  if (e)
    return (int)e;
  g.graphs[key] = ge;
  CADD(c_graphs, 1);
  return (int)cudaGraphLaunch(ge, g.s);
}

void CUDART_CB host_done(void* arg)
{
  Slot* s = (Slot*)arg;
  pthread_mutex_lock(&g_state_mu);
  if (s->state == SLOT_ABANDONED)
    s->state = SLOT_FREE; /* the submitter timed out: the slot is free once the GPU let go of it */
  else
    s->state = SLOT_DONE;
  pthread_cond_broadcast(&g_state_cv);
  pthread_mutex_unlock(&g_state_mu);
}

/* breaker (g_state_mu held) */
int breaker_state_locked()
{
  if (g_perm_off)
    return 2;
  return mono_ns() < g_bypass_until_ns ? 1 : 0;
}
void breaker_fail_locked(bool sticky)
{
  if (sticky) {
    if (!g_perm_off) {
      g_perm_off = 1;
      CADD(c_trips, 1);
      fprintf(stderr, "LDPC_CB0: sticky CUDA error -> CB0 GPU entry disabled (the TB path keeps its own state)\n");
    }
    return;
  }
  if (++g_consec >= g_breaker_n) {
    g_consec = 0;
    g_bypass_until_ns = mono_ns() + (long long)g_breaker_ms * 1000000LL;
    CADD(c_trips, 1);
    fprintf(stderr, "LDPC_CB0: %d consecutive failures -> CB0 GPU entry bypassed for %d ms\n", g_breaker_n, g_breaker_ms);
  }
}

/* after a failed enqueue: abort a capture, drain the stream, decide sticky (as G1's pool fail path) */
bool recover_stream()
{
  cudaStreamCaptureStatus st = cudaStreamCaptureStatusNone;
  if (cudaStreamIsCapturing(g.s, &st) == cudaSuccess && st != cudaStreamCaptureStatusNone) {
    cudaGraph_t gr = nullptr;
    cudaStreamEndCapture(g.s, &gr);
    if (gr) cudaGraphDestroy(gr);
  }
  cudaStreamSynchronize(g.s_copy);
  cudaStreamSynchronize(g.s);
  cudaGetLastError();
  const bool sticky = cudaGetLastError() != cudaSuccess || cudaStreamSynchronize(g.s) != cudaSuccess;
  if (!sticky)
    cudaGetLastError();
  return sticky;
}

/* input pointer classification for the EXPLICIT path */
enum { SRC_DEVICE, SRC_PINNED, SRC_PAGEABLE };
int classify(const void* p)
{
  cudaPointerAttributes a;
  if (cudaPointerGetAttributes(&a, p) != cudaSuccess) {
    cudaGetLastError();
    return SRC_PAGEABLE;
  }
  if (a.type == cudaMemoryTypeDevice)
    return SRC_DEVICE;
  if (a.type == cudaMemoryTypeManaged)
    return g.cma ? SRC_DEVICE : SRC_PAGEABLE; /* concurrent managed access: read in place (migrates on demand) */
  if (a.type == cudaMemoryTypeHost)
    return SRC_PINNED;
  return SRC_PAGEABLE;
}

struct Key {
  uint8_t BG, iters;
  uint16_t Z;
  bool operator<(const Key& o) const
  {
    return BG != o.BG ? BG < o.BG : Z != o.Z ? Z < o.Z : iters < o.iters;
  }
};

bool item_ok(const ldpc_cb0_item_t& it, size_t stride)
{
  ldpc_cuda_bg_t bg;
  if (it.iters < 1 || it.iters > LDPC_CB0_MAX_ITERS || it.crc_type > 2 || ldpc_cuda_basegraph(it.BG, it.Z, &bg) != 0)
    return false;
  const uint32_t KcZ = bg.num_cols * it.Z;
  if (KcZ > stride || it.K == 0 || it.K > 22u * it.Z || it.K > CRC_MAX_BITS || it.Kprime > it.K || (it.Kprime & 7)
      || it.Kprime <= (it.crc_type == 2 ? 16u : 24u) || it.guard_bytes > (it.K >> 3))
    return false;
  return true;
}

} // namespace

/* ---------------------------------------------------------------- API -------------------------------------------- */

struct ldpc_cb0_ticket {
  int slot;
};
static ldpc_cb0_ticket g_tickets[MAX_SLOTS];

extern "C" int ldpc_cb0_init(void)
{
  pthread_mutex_lock(&g_init_mu);
  const int st = init_locked();
  pthread_mutex_unlock(&g_init_mu);
  return st == 1 ? 0 : LDPC_CB0_E_DISABLED;
}

extern "C" int ldpc_cb0_mem_mode(void)
{
  return g.state == 1 ? g.mode : 0;
}

extern "C" int ldpc_cb0_max_items(void)
{
  return ldpc_cb0_init() == 0 ? g.cap : 0;
}

extern "C" int ldpc_cb0_submit(const ldpc_cb0_item_t* items, int n, const int8_t* llr, size_t llr_stride, int want_bits,
                               ldpc_cb0_ticket_t** t)
{
  if (!t)
    return LDPC_CB0_E_ARG;
  *t = nullptr;
  if (n <= 0 || !items || !llr || llr_stride == 0)
    return LDPC_CB0_E_ARG;
  if (ldpc_cb0_init() != 0)
    return LDPC_CB0_E_DISABLED;
  if (n > g.cap)
    return LDPC_CB0_E_TOOBIG;
  CADD(c_submits, 1);
  CADD(c_items, (uint64_t)n);
  /* breaker + slot */
  pthread_mutex_lock(&g_state_mu);
  const int bs = (g_sticky || g_perm_off) ? 2 : breaker_state_locked();
  int k = -1;
  if (bs == 0)
    for (int q = 0; q < g.nslots; q++)
      if (g.slot[q].state == SLOT_FREE) {
        k = q;
        g.slot[q].state = SLOT_INFLIGHT;
        break;
      }
  pthread_mutex_unlock(&g_state_mu);
  if (bs) {
    CADD(c_byp, 1);
    return bs == 2 ? LDPC_CB0_E_DISABLED : LDPC_CB0_E_BYPASSED;
  }
  if (k < 0) {
    CADD(c_busy, 1);
    return LDPC_CB0_E_BUSY;
  }
  Slot& S = g.slot[k];
  S.n = n;
  S.want_bits = want_bits;

  /* validate, sort by (BG, Z, iterations) */
  std::vector<int> ord;
  ord.reserve(n);
  for (int i = 0; i < n; i++) {
    S.valid[i] = item_ok(items[i], llr_stride);
    if (S.valid[i])
      ord.push_back(i);
  }
  std::stable_sort(ord.begin(), ord.end(), [&](int a, int b) {
    return Key{items[a].BG, items[a].iters, items[a].Z} < Key{items[b].BG, items[b].iters, items[b].Z};
  });
  for (size_t p = 0; p < ord.size(); p++) {
    const ldpc_cb0_item_t& it = items[ord[p]];
    S.meta_h[p] = DMeta{(uint32_t)ord[p], it.K, it.Kprime, it.guard_bytes, it.crc_type, 0};
  }

  int rc = 0;
  pthread_mutex_lock(&g_submit_mu);
  const int inject = g_inject;
  const int8_t* src = llr;
  size_t src_stride = llr_stride;
  if (inject == 1) { /* test hook: a real, non-sticky CUDA error (invalid copy kind) */
    rc = (int)cudaMemcpyAsync(g.chan, g.total, 1, (cudaMemcpyKind)99, g.s);
    if (!rc) rc = -1;
  }
  if (!rc && inject == 2 && g_stall_ms > 0)
    cb0_stall_kernel<<<1, 1, 0, g.s>>>((unsigned long long)g_stall_ms * 1000000ULL);
  if (!rc && g.mode == MODE_EXPLICIT) {
    /* metadata to the device; inputs: device-resident in place, host via pinned staging + async copies in pieces */
    if (!ord.empty())
      rc = (int)cudaMemcpyAsync(S.meta_d, S.meta_h, ord.size() * sizeof(DMeta), cudaMemcpyHostToDevice, g.s);
    const int cls = classify(llr);
    if (!rc && cls != SRC_DEVICE) {
      const size_t w = std::min<size_t>(llr_stride, CH);
      const int piece = std::max(1, (int)((8u << 20) / CH)); /* ~8 MB per copy */
      for (int a = 0; !rc && a < n; a += piece) {
        const int m = std::min(piece, n - a);
        if (cls == SRC_PINNED) {
          rc = (int)cudaMemcpy2DAsync(S.arena_d + (size_t)a * CH, CH, llr + (size_t)a * llr_stride, llr_stride, w, m,
                                      cudaMemcpyHostToDevice, g.s_copy);
        } else {
          for (int r = 0; r < m; r++) /* CPU pack of this piece overlaps the DMA of the previous one */
            memcpy(S.stage_h + (size_t)(a + r) * CH, llr + (size_t)(a + r) * llr_stride, w);
          rc = (int)cudaMemcpyAsync(S.arena_d + (size_t)a * CH, S.stage_h + (size_t)a * CH, (size_t)m * CH,
                                    cudaMemcpyHostToDevice, g.s_copy);
        }
        CADD(c_h2d, (uint64_t)m * (cls == SRC_PINNED ? w : CH));
      }
      if (!rc) rc = (int)cudaEventRecord(S.ev_in, g.s_copy);
      if (!rc) rc = (int)cudaStreamWaitEvent(g.s, S.ev_in, 0);
      src = S.arena_d;
      src_stride = CH;
    }
  }
  /* groups -> chunks, back to back on one stream */
  for (size_t p0 = 0; !rc && p0 < ord.size();) {
    const ldpc_cb0_item_t& it0 = items[ord[p0]];
    size_t p1 = p0 + 1;
    while (p1 < ord.size() && items[ord[p1]].BG == it0.BG && items[ord[p1]].Z == it0.Z
           && items[ord[p1]].iters == it0.iters)
      p1++;
    ldpc_cuda_bg_t bg;
    ldpc_cuda_basegraph(it0.BG, it0.Z, &bg);
    const uint32_t cnt = (uint32_t)(p1 - p0), cap = chunk_for(bg.num_rows, bg.num_cols, bg.num_edges, it0.Z);
    const uint32_t nch = (cnt + cap - 1) / cap, per = (cnt + nch - 1) / nch;
    const uint32_t KcZ = bg.num_cols * it0.Z;
    for (uint32_t off = 0; !rc && off < cnt; off += per) {
      const uint32_t m = std::min(per, cnt - off);
      uint32_t nb = 1;
      while (nb < m)
        nb <<= 1;
      const DMeta* md = S.meta_d + p0 + off;
      cb0_prep_kernel<<<dim3(blocks_for(KcZ, 16 * 256), nb), 256, 0, g.s>>>(src, src_stride, md, m, nb, KcZ, g.chan,
                                                                            g.done, g.unsat, g.iters, it0.iters);
      rc = (int)cudaPeekAtLastError();
      if (!rc) rc = launch_iterations(bg, it0.BG, it0.Z, nb, it0.iters);
      if (!rc) {
        cb0_finish_kernel<<<m, FIN_THREADS, 0, g.s>>>(g.total, md, m, g.iters, it0.iters, g.crc_pow, S.res_d,
                                                      want_bits ? S.bits_d : nullptr);
        rc = (int)cudaPeekAtLastError();
      }
    }
    p0 = p1;
  }
  if (!rc && g.mode == MODE_EXPLICIT && !ord.empty()) {
    rc = (int)cudaMemcpyAsync(S.res_h, S.res_d, (size_t)n * sizeof(ldpc_cb0_result_t), cudaMemcpyDeviceToHost, g.s);
    if (!rc && want_bits)
      rc = (int)cudaMemcpyAsync(S.bits_h, S.bits_d, (size_t)n * LDPC_CB0_BITS_STRIDE, cudaMemcpyDeviceToHost, g.s);
  }
  if (!rc) rc = (int)cudaEventRecord(S.ev_done, g.s);
  if (!rc) rc = (int)cudaLaunchHostFunc(g.s, host_done, &S);
  bool sticky = false;
  if (rc) {
    sticky = recover_stream() || inject == 3; /* drained: the slot is ours again */
    CADD(c_err, 1);
  }
  /* deadline: own budget, starting after the deadlines of submissions still in flight ahead of us (at most
   * LDPC_CB0_SLOTS - 1 of them, so the chain is bounded; an abandoned one does not extend it) */
  if (!rc) {
    long long start = mono_ns();
    const long long budget = ((long long)g_timeout_ms * 1000 + (long long)g_timeout_us_item * n) * 1000LL;
    pthread_mutex_lock(&g_state_mu);
    for (int q = 0; q < g.nslots; q++)
      if (q != k && g.slot[q].state == SLOT_INFLIGHT && g.slot[q].deadline_ns > start)
        start = g.slot[q].deadline_ns;
    S.deadline_ns = start + budget;
    pthread_mutex_unlock(&g_state_mu);
  }
  pthread_mutex_unlock(&g_submit_mu);
  if (rc) {
    pthread_mutex_lock(&g_state_mu);
    S.state = SLOT_FREE;
    if (sticky) {
      g_sticky = 1;
      CADD(c_sticky, 1);
    }
    breaker_fail_locked(sticky);
    pthread_mutex_unlock(&g_state_mu);
    return LDPC_CB0_E_CUDA;
  }
  g_tickets[k].slot = k;
  *t = &g_tickets[k];
  return LDPC_CB0_OK;
}

extern "C" int ldpc_cb0_collect(ldpc_cb0_ticket_t* t, ldpc_cb0_result_t* out, uint8_t* bits, size_t bits_stride)
{
  if (!t || !out || t->slot < 0 || t->slot >= g.nslots)
    return LDPC_CB0_E_ARG;
  Slot& S = g.slot[t->slot];
  const int n = S.n;
  int rc = LDPC_CB0_OK;
  struct timespec dl;
  dl.tv_sec = S.deadline_ns / 1000000000LL;
  dl.tv_nsec = S.deadline_ns % 1000000000LL;
  pthread_mutex_lock(&g_state_mu);
  while (S.state == SLOT_INFLIGHT)
    if (pthread_cond_timedwait(&g_state_cv, &g_state_mu, &dl) == ETIMEDOUT)
      break;
  if (S.state != SLOT_DONE) {
    S.state = SLOT_ABANDONED; /* host_done frees it when the GPU finishes */
    rc = LDPC_CB0_E_TIMEOUT;
  }
  pthread_mutex_unlock(&g_state_mu);
  bool sticky = false;
  if (rc == LDPC_CB0_E_TIMEOUT) { /* hung, or the context died before reaching our callback */
    const cudaError_t e = cudaStreamQuery(g.s);
    sticky = e != cudaSuccess && e != cudaErrorNotReady;
  }
  if (rc == LDPC_CB0_OK) {
    const cudaError_t e = cudaEventQuery(S.ev_done); /* an async fault of this submission surfaces here */
    if (e != cudaSuccess) {
      rc = LDPC_CB0_E_CUDA;
      sticky = true; /* the stream reached its host callback with a failed event: the context is gone */
    } else if (g_inject == 3) {
      rc = LDPC_CB0_E_CUDA;
      sticky = true;
    }
  }
  if (rc == LDPC_CB0_OK) {
    for (int i = 0; i < n; i++) {
      if (S.valid[i]) {
        out[i] = S.res_h[i];
        if (bits && S.want_bits)
          memcpy(bits + (size_t)i * bits_stride, S.bits_h + (size_t)i * LDPC_CB0_BITS_STRIDE,
                 std::min<size_t>(bits_stride, LDPC_CB0_BITS_STRIDE));
      } else {
        out[i] = ldpc_cb0_result_t{};
        out[i].crc_ok = -1;
        out[i].err = LDPC_CB0_E_ARG;
      }
    }
    CADD(c_ok, 1);
  } else {
    for (int i = 0; i < n; i++) {
      out[i] = ldpc_cb0_result_t{};
      out[i].crc_ok = -1;
      out[i].err = (int8_t)rc;
    }
    if (rc == LDPC_CB0_E_TIMEOUT)
      CADD(c_tmo, 1);
    else
      CADD(c_err, 1);
  }
  pthread_mutex_lock(&g_state_mu);
  if (rc != LDPC_CB0_E_TIMEOUT)
    S.state = SLOT_FREE;
  if (rc == LDPC_CB0_OK) {
    g_consec = 0;
  } else {
    if (sticky) {
      g_sticky = 1;
      CADD(c_sticky, 1);
    }
    breaker_fail_locked(sticky);
  }
  pthread_mutex_unlock(&g_state_mu);
  t->slot = -1;
  return rc;
}

extern "C" int ldpc_cb0_decode(const ldpc_cb0_item_t* items, int n, const int8_t* llr, size_t llr_stride,
                               ldpc_cb0_result_t* out, uint8_t* bits, size_t bits_stride)
{
  ldpc_cb0_ticket_t* t = nullptr;
  int rc = ldpc_cb0_submit(items, n, llr, llr_stride, bits != nullptr, &t);
  if (rc != LDPC_CB0_OK) {
    for (int i = 0; out && i < n; i++) {
      out[i] = ldpc_cb0_result_t{};
      out[i].crc_ok = -1;
      out[i].err = (int8_t)rc;
    }
    return rc;
  }
  return ldpc_cb0_collect(t, out, bits, bits_stride);
}

extern "C" int ldpc_cb0_healthy(void)
{
  if (ldpc_cb0_init() != 0)
    return 0;
  pthread_mutex_lock(&g_state_mu);
  const int ok = !g_sticky && !g_perm_off && breaker_state_locked() == 0;
  pthread_mutex_unlock(&g_state_mu);
  return ok;
}

extern "C" void ldpc_cb0_get_counters(ldpc_cb0_counters_t* c)
{
  c->submits = CLD(c_submits);
  c->items = CLD(c_items);
  c->ok = CLD(c_ok);
  c->cuda_errors = CLD(c_err);
  c->timeouts = CLD(c_tmo);
  c->busy = CLD(c_busy);
  c->bypassed = CLD(c_byp);
  c->sticky = CLD(c_sticky);
  c->breaker_trips = CLD(c_trips);
  c->graphs = CLD(c_graphs);
  c->h2d_bytes = CLD(c_h2d);
  pthread_mutex_lock(&g_state_mu);
  c->state = (g_sticky || g_perm_off) ? 2 : breaker_state_locked();
  pthread_mutex_unlock(&g_state_mu);
  c->mode = ldpc_cb0_mem_mode();
}

extern "C" void ldpc_cb0_test_hooks(int inject, int stall_ms, int timeout_ms, int breaker_n, int breaker_ms)
{
  ldpc_cb0_init();
  g_inject = inject;
  g_stall_ms = stall_ms;
  if (timeout_ms > 0) g_timeout_ms = timeout_ms;
  if (breaker_n > 0) g_breaker_n = breaker_n;
  if (breaker_ms > 0) g_breaker_ms = breaker_ms;
}

extern "C" void ldpc_cb0_test_reset(void)
{
  pthread_mutex_lock(&g_state_mu);
  g_consec = 0;
  g_bypass_until_ns = 0;
  g_perm_off = 0;
  g_sticky = 0;
  pthread_mutex_unlock(&g_state_mu);
}

extern "C" int ldpc_cb0_test_set_mem(int mode)
{
  if (ldpc_cb0_init() != 0)
    return 0;
  if (mode == MODE_UNIFIED && !g.integrated)
    mode = MODE_EXPLICIT;
  if (mode != MODE_UNIFIED && mode != MODE_EXPLICIT)
    return g.mode;
  pthread_mutex_lock(&g_submit_mu);
  cudaStreamSynchronize(g.s);
  if (mode != g.mode && slots_alloc(mode) != 0)
    g.state = 0;
  pthread_mutex_unlock(&g_submit_mu);
  return ldpc_cb0_mem_mode();
}
