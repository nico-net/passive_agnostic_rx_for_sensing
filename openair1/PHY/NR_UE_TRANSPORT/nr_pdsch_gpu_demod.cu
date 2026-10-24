/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* GPU-DEMOD module side (part of libpdsch_gpu.so): bit-exact nr_rx_pdsch() demodulation of one passive PDSCH grant,
 * all symbols in one launch set. See nr_pdsch_gpu_demod.h. The integer arithmetic is nr_pdsch_gpu_demod.cuh (the
 * same functions the host test checks against the CPU). The ISAC_RX_MRC_ALIGN rotation is double precision with
 * the operations, operand order and fused multiply-adds GCC 13 -O2 (aarch64, -ffp-contract=fast) emits for the CPU
 * source (read from the disassembly of nr_dlsch_demodulation.c.o); this file is compiled with --fmad=false so nvcc
 * fuses nothing on its own. */
#include "nr_pdsch_gpu_demod.h"
#include "nr_pdsch_gpu_fep.h" // NR_GPU_PASS_RING
#include "../TOOLS/nr_gpu_poll.h" // P99: ISAC_GPU_POLL_SLEEP_US
#include "nr_pdsch_gpu_demod.cuh"
#include "PHY/TOOLS/nr_gpu_mem.h"
#include "nr_gpu_stage_census.h"
#include "nr_mmse_exact.h" /* MMSE3GPU: mode 3, the CPU's own source */
#include <cuda_runtime.h>
#include "nr_gpu_green.h"
#include "PHY/TOOLS/nr_gpu_budget_cuda.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstddef>
#include <cstring>
#include <ctime>
#include <thread>
#include <type_traits>

namespace nrdm {
constexpr int CTX = 8;
constexpr int NSYM = NR_GPU_DEMOD_NSYM;
constexpr int EXT = NR_GPU_DEMOD_MAX_EXT;
constexpr int MAXCH = (EXT + 47) / 48; /* ALIGN=2 chunks per symbol */

struct Shared { /* mapped: written by the GPU, read by the host after the stream completes */
  nrd_decision d;
  int32_t avg[16], median[16];
  int64_t pw[NSYM][4];
  int ref[NSYM];
  long long kap_sz[NSYM][4], kap_sm[NSYM][4];
  unsigned level_done; /* GPUFUSE: k_level blocks finished (the last one runs the decision) */
  double m3_s2[NSYM], m3_gain[NSYM]; /* MMSE3GPU: mode-3 noise variance and output gain per symbol (k_mmse3_prep) */
  float m3_s2f[NSYM], m3_gainf[NSYM]; /* the same for the FP32 instantiation (GPU pass on sm_89) */
  nr_gpu_evm_sym_t evm[NSYM];
};
struct Ctx {
  cudaStream_t s;
  nrd_c16 *y, *h; /* device memory, [14][4][EXT] and [14][16][EXT] */
  double2 *xs, *sd; /* EVM: per-RE zero-forcing solution and decisions [14][4][EXT] */
  double *part, *sum; /* EVM block partials [14][EVM_NCH][EVM_P] and per-symbol sums [14][EVM_P] */
  nrd_c16 *hy, *hh;   /* pinned host copies of y/h for the CPU EVM fallback (ISAC_PDSCH_GPU_EXACT_DEMOD_EVM=0) */
  cudaStream_t s2;           /* EVM chain, concurrent with compensation/MMSE/LLR */
  cudaEvent_t fork, join; /* fork/join of the EVM chain on s2 */
  Shared *sh;  /* pinned host copy, filled by one DMA at the end */
  Shared *dsh; /* device copy the kernels use (host-pinned memory is uncached for the GPU) */
};
constexpr int PASS_INST = NR_GPU_PASS_RING;
static Ctx g_ctx[CTX + PASS_INST + 1]; /* [CTX + PASS_INST]: the test-only nr_pdsch_gpu_demod_dev context, [CTX + i]: that of graph-pass instance i; never handed out by acquire() */
static nr_gpu_buf_t g_jb[PASS_INST]; /* per instance: the job block the graph's kernels read (jref) */
/* The kernels' job parameter: the job up to evm[] (outputs only, never read by a kernel). The whole job exceeds the
 * 32 KB kernel parameter space; __grid_constant__ lets the kernels address it in place as a nr_gpu_demod_job_t. */
struct JobK {
  unsigned char b[offsetof(nr_gpu_demod_job_t, evm)];
};
static inline const JobK &K(const nr_gpu_demod_job_t &j) { return *reinterpret_cast<const JobK *>(&j); }
/* the job a kernel works on: its by-value copy, or (graph replay) the job block jref points to */
#define NRD_JOB(jk) (reinterpret_cast<const nr_gpu_demod_job_t *>(&(jk))->jref ? *reinterpret_cast<const nr_gpu_demod_job_t *>(&(jk))->jref : *reinterpret_cast<const nr_gpu_demod_job_t *>(&(jk)))
#define NRD_NVAR(j) ((j).nvar_dev ? *(j).nvar_dev : (j).nvar)
static std::atomic<unsigned> g_busy{0};
static int g_ok;

/* ---- Q4: DISCRETE staging of nr_pdsch_gpu_demod_run ---------------------------------------------------------------------------------------------------
 * The standalone demod reads the caller's rxdataF / estimates / geometry and writes its comp / mag* / LLR buffers in place (DIRECT tier). On DISCRETE each of the first
 * g_stg_n contexts owns a pinned+device block pair: `in` = [rx span][estimate runs][geo_y rows][geo_h rows], one async H2D of exactly the bytes used; `out` = [LLRs][comp][mag]
 * [magb][magr], D2H of exactly what the host consumes. The kernels are the same; only their pointers change, with the compact layouts reached through biased base pointers:
 *   rx:  antenna a, symbols lo..hi only        (rxdataF' = base - lo N, rx_stride' = (hi - lo + 1) N)
 *   est: the symbols the geometry refers to    (geo_h's symbol field remapped to the rank among them, est_stride' = used N)
 *   geo: rows lo..hi, the first nre entries of each
 *   out: [symbol - lo][4 layers][ext]          (comp' = base - lo 4 ext, buf' = ext)
 * Default D2H (the decoder's needs): the LLRs, comp layer 0 of the first symbol with the most REs (the Qm oracle). stage_full (a grant that is compared with the CPU): every
 * array the kernels wrote, all layers, first nre entries per symbol. */
struct Stg {
  nr_gpu_buf_t in, out;
};
static Stg g_stg[CTX];
static int g_stg_n, g_stg_N, g_stg_nbrx;
static uint32_t g_stg_ext;
static size_t g_stg_llr_cap, g_stg_arr; /* LLR capacity (entries), bytes of one comp / mag array in `out` */
static size_t al256(size_t b) { return (b + 255) & ~(size_t)255; }
struct SPlan {
  int lo, hi, ns, used_n, rank[NSYM], qm_m;
  uint32_t used;
  size_t in_n, llr_n;
};
static inline const int16_t *bias(const void *dev, size_t off, size_t sub) /* dev + off - sub, modulo 2^64 (the kernels only dereference inside the block) */
{
  return reinterpret_cast<const int16_t *>(reinterpret_cast<uintptr_t>(dev) + off - sub);
}
/* which of mag / magb / magr the kernels write for this grant (comp is always written): launch_tail + nrd_comp_re + the MMSE kernels */
static unsigned mags_written(const nr_gpu_demod_job_t &j)
{
  const bool always = j.nl == 2 || (j.nl >= 3 && (j.qam_mode == 2 || j.qam_mode == 3));
  const bool mmse0 = j.nl >= 3 && j.qam_mode != 2 && j.qam_mode != 3 && j.qm > 2;
  return ((j.qm > 2 || always) ? 1u : 0u) | ((j.qm > 4 || always || mmse0) ? 2u : 0u) | ((j.qm > 6 || always || mmse0) ? 4u : 0u);
}
/* plans the grant and fills the context's input block; 0, or -1 when the grant does not fit (nothing is enqueued) */
static int stage_fill(int c, const nr_gpu_demod_job_t &j, SPlan &P)
{
  Stg &g = g_stg[c];
  const int N = j.N, nbRx = j.nbRx, nl = j.nl, ext = (int)j.ext;
  if (N != g_stg_N || nbRx > g_stg_nbrx || (uint32_t)ext > g_stg_ext || (uint32_t)ext > j.buf) return -1;
  P.lo = NSYM;
  P.hi = -1;
  for (int m = 0; m < NSYM; m++)
    if (j.sym_mask >> m & 1) {
      P.lo = P.lo < m ? P.lo : m;
      P.hi = m;
    }
  if (P.hi < 0) return -1;
  P.ns = P.hi - P.lo + 1;
  P.llr_n = (size_t)j.llr_off[P.hi] + (size_t)j.nre[P.hi] * j.qm * nl;
  if (P.llr_n > g_stg_llr_cap) return -1;
  const int16_t *gh = (const int16_t *)j.geo_h, *gy = (const int16_t *)j.geo_y;
  P.used = 0;
  P.qm_m = -1;
  int qn = -1;
  for (int m = P.lo; m <= P.hi; m++)
    if (j.sym_mask >> m & 1) {
      if ((int)j.nre[m] > qn) { qn = j.nre[m]; P.qm_m = m; }
      if ((int)j.nre[m] > ext) return -1;
      const int16_t *row = gh + 2 * ((size_t)m * ext);
      for (int i = 0; i < (int)j.nre[m]; i++) {
        const unsigned sidx = (unsigned)(row[2 * i + 1] - 1);
        if (sidx >= (unsigned)NSYM) return -1;
        P.used |= 1u << sidx;
      }
    }
  P.used_n = 0;
  for (int s = 0; s < NSYM; s++) P.rank[s] = (P.used >> s & 1) ? P.used_n++ : -1;
  if (!P.used_n) return -1;
  const int rows = nl * nbRx;
  const size_t rx_b = (size_t)nbRx * P.ns * N * 4, est_b = (size_t)rows * P.used_n * N * 4, geo_b = (size_t)P.ns * ext * 4;
  const size_t est_off = al256(rx_b), gy_off = al256(est_off + est_b), gh_off = al256(gy_off + geo_b);
  P.in_n = gh_off + geo_b;
  if (P.in_n > g.in.bytes) return -1;
  char *h = (char *)g.in.host;
  for (int a = 0; a < nbRx; a++)
    memcpy(h + (size_t)a * P.ns * N * 4, (const char *)j.rxdataF + ((size_t)a * j.rx_stride + (size_t)P.lo * N) * 4, (size_t)P.ns * N * 4);
  for (int r = 0; r < rows; r++) {
    char *dst = h + est_off + (size_t)r * P.used_n * N * 4;
    const char *src = (const char *)j.est + (size_t)r * j.est_stride * 4;
    for (int s = 0; s < NSYM;) {
      if (!(P.used >> s & 1)) { s++; continue; }
      int e = s;
      while (e + 1 < NSYM && (P.used >> (e + 1) & 1)) e++;
      memcpy(dst + (size_t)P.rank[s] * N * 4, src + (size_t)s * N * 4, (size_t)(e - s + 1) * N * 4);
      s = e + 1;
    }
  }
  for (int m = P.lo; m <= P.hi; m++)
    if (j.sym_mask >> m & 1) {
      const size_t n = j.nre[m];
      memcpy(h + gy_off + (size_t)(m - P.lo) * ext * 4, gy + 2 * ((size_t)m * ext), n * 4);
      int16_t *dh = (int16_t *)(h + gh_off + (size_t)(m - P.lo) * ext * 4);
      const int16_t *sh = gh + 2 * ((size_t)m * ext);
      for (size_t i = 0; i < n; i++) {
        dh[2 * i] = sh[2 * i];
        dh[2 * i + 1] = (int16_t)(P.rank[sh[2 * i + 1] - 1] + 1);
      }
    }
  return 0;
}
/* the job as the kernels see it on the device (call after a successful stage_fill) */
static void stage_point(int c, nr_gpu_demod_job_t &jj, const SPlan &P)
{
  const Stg &g = g_stg[c];
  const size_t N = jj.N, ext = jj.ext, rows = (size_t)jj.nl * jj.nbRx;
  const size_t rx_b = (size_t)jj.nbRx * P.ns * N * 4, est_b = rows * P.used_n * N * 4, geo_b = (size_t)P.ns * ext * 4;
  const size_t est_off = al256(rx_b), gy_off = al256(est_off + est_b), gh_off = al256(gy_off + geo_b);
  jj.rxdataF = bias(g.in.dev, 0, (size_t)P.lo * N * 4);
  jj.rx_stride = (uint32_t)(P.ns * N);
  jj.est = bias(g.in.dev, est_off, 0);
  jj.est_stride = (uint32_t)(P.used_n * N);
  jj.geo_y = bias(g.in.dev, gy_off, (size_t)P.lo * ext * 4);
  jj.geo_h = bias(g.in.dev, gh_off, (size_t)P.lo * ext * 4);
  const size_t sub = (size_t)P.lo * 4 * ext * 4;
  jj.llr = (int16_t *)g.out.dev;
  jj.comp = (int16_t *)bias(g.out.dev, al256(g_stg_llr_cap * 2), sub);
  jj.mag = (int16_t *)bias(g.out.dev, al256(g_stg_llr_cap * 2) + g_stg_arr, sub);
  jj.magb = (int16_t *)bias(g.out.dev, al256(g_stg_llr_cap * 2) + 2 * g_stg_arr, sub);
  jj.magr = (int16_t *)bias(g.out.dev, al256(g_stg_llr_cap * 2) + 3 * g_stg_arr, sub);
  jj.buf = (uint32_t)ext;
}

/* DECTAIL: pool exhaustion is counted (DEMOD_POOL_WAIT lines, first 30 waits above 1 ms, and the waits/total in the first line). */
static std::atomic<unsigned> g_pool_waits{0}, g_pool_log{0};
static int acquire(int limit = CTX) /* limit: only the first `limit` contexts (Q4: the staged DISCRETE run owns staging buffers on those) */
{
  struct timespec t0 = {};
  bool waited = false;
  const uint64_t ss0 = nr_ss_now();
  for (;;) {
    unsigned m = g_busy.load();
    for (int i = 0; i < nr_gpu_ctx_limit(limit); i++)
      if (!(m >> i & 1u) && g_busy.compare_exchange_strong(m, m | 1u << i)) {
        nr_ss_add(NRSS_LK, ss0);
        if (waited) {
          struct timespec t1;
          clock_gettime(CLOCK_MONOTONIC, &t1);
          const long us = (t1.tv_sec - t0.tv_sec) * 1000000L + (t1.tv_nsec - t0.tv_nsec) / 1000;
          const unsigned n = ++g_pool_waits;
          if (us > 1000 && g_pool_log.fetch_add(1) < 30)
            fprintf(stderr, "DEMOD_POOL_WAIT us=%ld waits=%u\n", us, n);
        }
        return i;
      }
    if (!waited) {
      waited = true;
      clock_gettime(CLOCK_MONOTONIC, &t0);
    }
    std::this_thread::yield();
  }
}

/* enqueues the D2H of the outputs the host consumes on `st` (counts the copies / bytes) */
static cudaError_t stage_d2h(int c, const nr_gpu_demod_job_t &j, const SPlan &P, cudaStream_t st, uint64_t *bytes, int *n)
{
  const Stg &g = g_stg[c];
  const size_t ext = j.ext, cbase = al256(g_stg_llr_cap * 2);
  cudaError_t e = cudaMemcpyAsync(g.out.host, g.out.dev, P.llr_n * 2, cudaMemcpyDeviceToHost, st);
  *bytes += P.llr_n * 2;
  ++*n;
  const unsigned w = mags_written(j);
  for (int k = 0; k < 4 && e == cudaSuccess; k++) {
    if (k && !(w >> (k - 1) & 1)) continue;
    for (int m = P.lo; m <= P.hi && e == cudaSuccess; m++) {
      if (!(j.sym_mask >> m & 1) || (!j.stage_full && (k || m != P.qm_m))) continue;
      const size_t off = cbase + (size_t)k * g_stg_arr + (size_t)(m - P.lo) * 4 * ext * 4, wd = (size_t)j.nre[m] * 4, ht = j.stage_full ? j.nl : 1;
      e = cudaMemcpy2DAsync((char *)g.out.host + off, ext * 4, (const char *)g.out.dev + off, ext * 4, wd, ht, cudaMemcpyDeviceToHost, st);
      *bytes += wd * ht;
      ++*n;
    }
  }
  return e;
}
/* after the stream finished: the mirror to the caller's buffers (exactly the copied spans) */
static void stage_scatter(int c, const nr_gpu_demod_job_t *j, const SPlan &P)
{
  const Stg &g = g_stg[c];
  const size_t ext = j->ext, cbase = al256(g_stg_llr_cap * 2);
  memcpy(j->llr, g.out.host, P.llr_n * 2);
  int16_t *dst[4] = {j->comp, j->mag, j->magb, j->magr}; /* the caller's buffers: the job passed here is the caller's, not the device-pointer copy */
  const unsigned w = mags_written(*j);
  for (int k = 0; k < 4; k++) {
    if (k && !(w >> (k - 1) & 1)) continue;
    for (int m = P.lo; m <= P.hi; m++) {
      if (!(j->sym_mask >> m & 1) || (!j->stage_full && (k || m != P.qm_m))) continue;
      for (int l = 0; l < (j->stage_full ? j->nl : 1); l++)
        memcpy(dst[k] + 2 * ((size_t)(m * NR_GPU_DEMOD_MAX_NL + l) * j->buf), (const char *)g.out.host + cbase + (size_t)k * g_stg_arr + ((size_t)(m - P.lo) * 4 + l) * ext * 4,
               (size_t)j->nre[m] * 4);
    }
  }
}

/* ---- gather: nr_dlsch_extract_rbs() with the CPU-made index map ------------------------------------------- */
__global__ void k_gather(__grid_constant__ const JobK jk_, nrd_c16 *y, nrd_c16 *h)
{
  const nr_gpu_demod_job_t &j = NRD_JOB(jk_);
  const int m = blockIdx.y;
  if (!(j.sym_mask >> m & 1))
    return;
  const nrd_c16 *gy = (const nrd_c16 *)j.geo_y + (size_t)m * j.ext;
  const nrd_c16 *gh = (const nrd_c16 *)j.geo_h + (size_t)m * j.ext;
  const nrd_c16 *rx = (const nrd_c16 *)j.rxdataF;
  const nrd_c16 *est = (const nrd_c16 *)j.est;
  const int nre = j.nre[m];
  for (uint32_t i = blockIdx.x * blockDim.x + threadIdx.x; i < j.ext; i += gridDim.x * blockDim.x) {
    const bool in = (int)i < nre;
    const nrd_c16 py = in ? gy[i] : nrd_c16{0, 0}, ph = in ? gh[i] : nrd_c16{0, 1};
    for (int a = 0; a < j.nbRx; a++) {
      nrd_c16 v = {0, 0};
      if (in && py.i)
        v = rx[(size_t)a * j.rx_stride + (size_t)m * j.N + (uint16_t)py.r];
      y[((size_t)m * NR_GPU_DEMOD_MAX_RX + a) * EXT + i] = v;
    }
    for (int r = 0; r < j.nl * j.nbRx; r++) {
      nrd_c16 v = {0, 0};
      if (in)
        v = est[(size_t)r * j.est_stride + (size_t)(ph.i - 1) * j.N + (uint16_t)ph.r];
      h[((size_t)m * 16 + r) * EXT + i] = v;
    }
  }
}

/* ---- channel level of the first data symbol, median (nl > 1), branch decision, output shift ---------------- */
__global__ void k_level(__grid_constant__ const JobK jk_, const nrd_c16 *h, Shared *sh)
{
  const nr_gpu_demod_job_t &j = NRD_JOB(jk_);
  __shared__ uint32_t lane[256][4];
  __shared__ long long mx[256], mn[256];
  const int f = j.first_symbol, t = threadIdx.x;
  const uint32_t len = j.nre[f];
  const int x = nrd_factor2(len);
  const int rows = j.nl * j.nbRx;
  for (int r = blockIdx.x; r < rows; r += gridDim.x) {
    const nrd_c16 *row = h + ((size_t)f * 16 + r) * EXT;
    uint32_t l4[4] = {0, 0, 0, 0};
    for (uint32_t g = t; g < (len >> 2); g += blockDim.x)
      for (int k = 0; k < 4; k++)
        l4[k] += nrd_level_term(row[4 * g + k], x);
    for (int k = 0; k < 4; k++)
      lane[t][k] = l4[k];
    __syncthreads();
    for (int s = blockDim.x / 2; s > 0; s >>= 1) {
      if (t < s)
        for (int k = 0; k < 4; k++)
          lane[t][k] += lane[t + s][k];
      __syncthreads();
    }
    if (t == 0)
      sh->avg[r] = nrd_level_finish(lane[0], len);
    __syncthreads();
    if (j.nl > 1) {
      long long a = sh->avg[r], b = sh->avg[r];
      for (uint32_t g = t; g < (len >> 2); g += blockDim.x) {
        const long long v = nrd_median_term(row + 4 * g);
        a = v > a ? v : a;
        b = v < b ? v : b;
      }
      mx[t] = a;
      mn[t] = b;
      __syncthreads();
      for (int s = blockDim.x / 2; s > 0; s >>= 1) {
        if (t < s) {
          mx[t] = mx[t + s] > mx[t] ? mx[t + s] : mx[t];
          mn[t] = mn[t + s] < mn[t] ? mn[t + s] : mn[t];
        }
        __syncthreads();
      }
      if (t == 0)
        sh->median[r] = (int32_t)((mx[0] + mn[0]) >> 1);
      __syncthreads();
    }
  }
  /* GPUFUSE: the last block to finish takes the decision (was k_decide, one more launch) */
  __shared__ bool last;
  if (t == 0) {
    __threadfence();
    last = atomicAdd(&sh->level_done, 1u) == gridDim.x - 1;
  }
  __syncthreads();
  if (last && t == 0) {
    __threadfence();
    int32_t avg[16], med[16];
    for (int r = 0; r < rows; r++) { /* L2 reads: other blocks wrote them */
      avg[r] = __ldcg(&sh->avg[r]);
      med[r] = __ldcg(&sh->median[r]);
    }
    nrd_decide(j.nl, j.nbRx, j.mode, j.thr_coef, j.l2_delta, j.shift_sum, avg, med, &sh->d);
    sh->level_done = 0;
  }
}

/* ---- mode-3 branch silencing (every symbol), then the per-symbol branch powers and reference of the alignment */
__global__ void k_mask_pw(__grid_constant__ const JobK jk_, nrd_c16 *h, Shared *sh)
{
  const nr_gpu_demod_job_t &j = NRD_JOB(jk_);
  __shared__ long long red[256];
  const int m = blockIdx.x, t = threadIdx.x;
  if (!(j.sym_mask >> m & 1))
    return;
  if (t < 4) { /* k_tail accumulates into them */
    sh->kap_sz[m][t] = 0;
    sh->kap_sm[m][t] = 0;
  }
  const nrd_decision d = sh->d;
  if (j.nl == 1 && d.zero_mask)
    for (int a = 0; a < j.nbRx; a++)
      if (d.zero_mask >> a & 1)
        for (uint32_t i = t; i < j.ext; i += blockDim.x)
          h[((size_t)m * 16 + a) * EXT + i] = nrd_c16{0, 0};
  const int nre = j.nre[m];
  if (!(j.nl == 1 && d.mrc_nb_rx > 1 && nre >= 12 && j.align)) {
    if (t == 0)
      sh->ref[m] = -1;
    return;
  }
  __syncthreads();
  for (int a = d.mrc_rx_index; a < d.mrc_rx_index + d.mrc_nb_rx; a++) {
    const nrd_c16 *row = h + ((size_t)m * 16 + a) * EXT;
    long long s = 0;
    for (int i = t; i < nre; i += blockDim.x)
      s += (long long)row[i].r * row[i].r + (long long)row[i].i * row[i].i;
    red[t] = s;
    __syncthreads();
    for (int k = blockDim.x / 2; k > 0; k >>= 1) {
      if (t < k)
        red[t] += red[t + k];
      __syncthreads();
    }
    if (t == 0)
      sh->pw[m][a] = red[0];
    __syncthreads();
  }
  if (t == 0) {
    int ref = -1;
    for (int a = d.mrc_rx_index; a < d.mrc_rx_index + d.mrc_nb_rx; a++)
      if (sh->pw[m][a] > 0 && (ref < 0 || sh->pw[m][a] > sh->pw[m][ref]))
        ref = a;
    sh->ref[m] = ref;
  }
}

/* ---- Task 5: precision R = double (the bit-exact path) | float (GPU pass on an RTX 4070, FP64 at 1/64 rate; not bit-exact, ISAC_GPU_PASS_FP32).
 * The double instantiations are the original code, operation for operation (the file is built with --fmad=false, so plain a*b+c is not fused). ---- */
template <typename R> struct cxr {
  R r, i;
};
typedef cxr<double> dcx;
__device__ __forceinline__ double fmaR(double a, double b, double c) { return __fma_rn(a, b, c); }
__device__ __forceinline__ float fmaR(float a, float b, float c) { return __fmaf_rn(a, b, c); }
__device__ __forceinline__ double divR(double a, double b) { return __ddiv_rn(a, b); }
__device__ __forceinline__ float divR(float a, float b) { return __fdiv_rn(a, b); }
__device__ __forceinline__ double sqrtR(double a) { return __dsqrt_rn(a); }
__device__ __forceinline__ float sqrtR(float a) { return __fsqrt_rn(a); }
__device__ __forceinline__ double absR(double a) { return fabs(a); }
__device__ __forceinline__ float absR(float a) { return fabsf(a); }
__device__ __forceinline__ double ldexpR(double, int e) { return ldexp(1.0, e); }
__device__ __forceinline__ float ldexpR(float, int e) { return ldexpf(1.0f, e); }
/* round to the nearest integer, ties away from zero (the CPU's lround) */
__device__ __forceinline__ double nanR(double) { return nan(""); }
__device__ __forceinline__ float nanR(float) { return nanf(""); }
__device__ __forceinline__ long long roundR(double v) { return llround(v); }
__device__ __forceinline__ long long roundR(float v) { return (long long)llroundf(v); }

/* ---- ISAC_RX_MRC_ALIGN: per (symbol, chunk, branch) one thread forms the chunk's cross-correlation sequentially in RE
 * order with GCC's FMA pattern (exactly the CPU's sums) and the rotation (cos, sin), then the block rotates the chunk's
 * REs. GPUFUSE: one launch (was k_align_terms / k_align_cs / k_align_rot, same arithmetic); ALC chunks per block. ---- */
constexpr int ALC = 2;
template <typename R = double> __global__ void k_align(__grid_constant__ const JobK jk_, const nrd_c16 *y, nrd_c16 *h, const Shared *sh)
{
  const nr_gpu_demod_job_t &j = NRD_JOB(jk_);
  __shared__ cxr<R> s_cs[ALC][4];
  const int m = blockIdx.y, t = threadIdx.x;
  if (!(j.sym_mask >> m & 1))
    return;
  const int ref = sh->ref[m];
  if (ref < 0)
    return;
  const nrd_decision d = sh->d;
  const uint32_t nre = j.nre[m];
  const uint32_t chunk = j.align >= 2 ? 48u : nre;
  const bool one = chunk == nre; /* one chunk: it rotates the whole row */
  if (one && blockIdx.x != 0)
    return;
  if (t < ALC * 4) {
    const int w = t & 3, c = blockIdx.x * ALC + (t >> 2), a = d.mrc_rx_index + w;
    cxr<R> out = {nanR(R()), (R)0}; /* nan = leave this chunk alone */
    const uint32_t c0 = (uint32_t)c * chunk;
    if (c < MAXCH && w < d.mrc_nb_rx && a != ref && sh->pw[m][a] > 0 && c0 < nre) {
      const uint32_t c1 = c0 + chunk < nre ? c0 + chunk : nre;
      const nrd_c16 *hr_row = h + ((size_t)m * 16 + ref) * EXT, *yr_row = y + ((size_t)m * NR_GPU_DEMOD_MAX_RX + ref) * EXT;
      const nrd_c16 *ha_row = h + ((size_t)m * 16 + a) * EXT, *ya_row = y + ((size_t)m * NR_GPU_DEMOD_MAX_RX + a) * EXT;
      R cr = 0, ci = 0;
      for (uint32_t i = c0; i < c1; i++) { /* the CPU's sequential fadd, RE order */
        const nrd_c16 hr = hr_row[i], yr = yr_row[i], ha = ha_row[i], ya = ya_row[i];
        /* products of int16 are exact in double, and so are these sums (|.| < 2^31); in float they are rounded */
        const R urr = (R)hr.r * yr.r + (R)hr.i * yr.i, uri = (R)hr.r * yr.i - (R)hr.i * yr.r;
        const R uar = (R)ha.r * ya.r + (R)ha.i * ya.i, uai = (R)ha.r * ya.i - (R)ha.i * ya.r;
        cr = cr + fmaR(uar, urr, uai * uri);  /* fmadd d, uar, urr, (uai*uri) */
        ci = ci + fmaR(uai, urr, -(uar * uri)); /* fnmsub d, uai, urr, (uar*uri) */
      }
      /* fmul ci*ci; fmadd cr,cr; fsqrt. float: scaled by the larger component first (cr, ci reach 2^60 and more: the squares would overflow) */
      R sc = (R)1;
      if constexpr (std::is_same<R, float>::value) {
        sc = absR(cr) > absR(ci) ? absR(cr) : absR(ci);
        if (sc > (R)0) { cr = divR(cr, sc); ci = divR(ci, sc); }
      }
      const R mag = sqrtR(fmaR(cr, cr, ci * ci));
      if (mag > (R)0)
        out = cxr<R>{divR(cr, mag), divR(ci, mag)};
    }
    s_cs[t >> 2][w] = out;
  }
  __syncthreads();
  const uint32_t i0 = one ? 0 : blockIdx.x * ALC * chunk;
  const uint32_t i1 = one ? j.ext : (i0 + ALC * chunk < nre ? i0 + ALC * chunk : nre); /* ALIGN=1 rotates the whole row */
  for (uint32_t i = i0 + t; i < i1; i += blockDim.x) {
    const int cl = one ? 0 : (int)(i / chunk) - (int)blockIdx.x * ALC;
    for (int w = 0; w < d.mrc_nb_rx && w < 4; w++) {
      const cxr<R> q = s_cs[cl][w];
      if (isnan(q.r))
        continue;
      nrd_c16 *row = h + ((size_t)m * 16 + d.mrc_rx_index + w) * EXT;
      const nrd_c16 v = row[i];
      const R hr2 = fmaR((R)v.r, q.r, -((R)v.i * q.i)); /* fnmsub d, h.r, c, (h.i*sn) */
      const R hi2 = fmaR((R)v.r, q.i, (R)v.i * q.r);  /* fmadd d, h.r, sn, (h.i*c) */
      row[i] = nrd_c16{(int16_t)roundR(hr2), (int16_t)roundR(hi2)};          /* lround, then (int16_t) wraps */
    }
  }
}

/* ---- nr_channel_compensation() over the whole extraction row, per layer ------------------------------------ */
__device__ __forceinline__ void re_comp(const nr_gpu_demod_job_t &j, const nrd_c16 *y, const nrd_c16 *h, const Shared *sh, int m, uint32_t i)
{
  const nrd_decision d = sh->d;
  const int shift = d.log2_maxh;
  const nrd_c16 *rxF = y + ((size_t)m * NR_GPU_DEMOD_MAX_RX + d.mrc_rx_index) * EXT;
  if (i < j.ext)
    for (int l = 0; l < j.nl; l++) {
      const nrd_c16 *chF = h + ((size_t)m * 16 + l * j.nbRx + d.mrc_rx_index) * EXT;
      const size_t row = ((size_t)m * NR_GPU_DEMOD_MAX_NL + l) * j.buf;
      nrd_comp_re(d.mrc_nb_rx, EXT, i, j.qm, shift, rxF, chF, (nrd_c16 *)j.comp + row, (nrd_c16 *)j.mag + row,
                  (nrd_c16 *)j.magb + row, (nrd_c16 *)j.magr + row);
      if (j.wide_mrc && j.nl == 1 && j.qm == 2 && i < j.nre[m]) {
        /* nr_rx_pdsch(): the CRC-validated recovery's full-precision matched-filter sum, one output clamp */
        long long re = 0, im = 0;
        for (int a = 0; a < d.mrc_nb_rx; a++) {
          const nrd_c16 hh = chF[(size_t)a * EXT + i], yy = rxF[(size_t)a * EXT + i];
          re += (long long)hh.r * yy.r + (long long)hh.i * yy.i;
          im += (long long)hh.r * yy.i - (long long)hh.i * yy.r;
        }
        re >>= shift;
        im >>= shift;
        ((nrd_c16 *)j.comp)[row + i] = nrd_c16{(int16_t)(re > 32767 ? 32767 : re < -32768 ? -32768 : re),
                                               (int16_t)(im > 32767 ? 32767 : im < -32768 ? -32768 : im)};
      }
    }
}


/* ---- QAM-FIX ISAC_PDSCH_MMSE_MODE=2 (nr_dlsch_mmse, Nl = 2): per RE, G = sum_a sat(conj(h_r) h_c >> shift) in
 * int16 (mult_cpx_conj_vector + nr_a_sum_b), then the double Gauss-Jordan [G | H^H y | I] of the CPU source.
 * Complex products follow GCC's lowering (re = fma(L.r, R.r, -(L.i R.i)), im = fma(L.r, R.i, (L.i R.r)));
 * division is libgcc 13's __divdc3 (ported from its aarch64 object code); cabs of integer components is the
 * correctly rounded sqrt of the exact sum (glibc's cabs agrees on 2e7 random integer pairs), else hypot. */
template <typename R> __device__ __forceinline__ cxr<R> cmul(cxr<R> L, cxr<R> Q)
{
  return cxr<R>{fmaR(L.r, Q.r, -(L.i * Q.i)), fmaR(L.r, Q.i, L.i * Q.r)};
}
__device__ __forceinline__ double cabs_c(cxr<double> x)
{
  const double lim = 67108864.0; /* 2^26: the squares and their sum are exact */
  if (x.r == trunc(x.r) && x.i == trunc(x.i) && fabs(x.r) < lim && fabs(x.i) < lim)
    return __dsqrt_rn(__dadd_rn(__dmul_rn(x.r, x.r), __dmul_rn(x.i, x.i)));
  /* GPUALLOC: glibc >= 2.35 __ieee754_hypot, FMA kernel (what the CPU's cabs() runs on aarch64). CUDA's hypot() differs
   * by 1 ulp on some inputs, which flipped a pivot tie and moved an exact .5 result by one LSB (Nl = 3, rare). */
  double ax = fabs(x.r), ay = fabs(x.i);
  if (ay > ax) { const double t = ax; ax = ay; ay = t; }
  const double t1 = __dadd_rn(ay, ay), t2 = __dadd_rn(ax, -ay);
  return t1 >= ax ? __dsqrt_rn(__fma_rn(t1, ax, __dmul_rn(t2, t2))) : __dsqrt_rn(__fma_rn(ax, ax, __dmul_rn(ay, ay)));
}
/* float: the pivots of the RTX 4070 path (hypotf: no overflow whatever the magnitudes) */
__device__ __forceinline__ float cabs_c(cxr<float> x) { return hypotf(x.r, x.i); }
__device__ cxr<double> divdc3(double a, double b, double c, double d)
{
  const double RBIG = __longlong_as_double(0x7fdfffffffffffffLL), RMIN2 = __longlong_as_double(0x3cb0000000000000LL);
  const double RMIN = __longlong_as_double(0x0010000000000000LL), RMAX2 = __longlong_as_double(0x7c9fffffffffffffLL);
  const double SC = __longlong_as_double(0x4330000000000000LL);
  double x, y;
  if (fabs(c) < fabs(d)) {
    if (fabs(d) >= RBIG) {
      a = __dmul_rn(a, 0.5); b = __dmul_rn(b, 0.5); c = __dmul_rn(c, 0.5); d = __dmul_rn(d, 0.5);
    }
    if (fabs(d) < RMIN2 || (fabs(a) < RMIN && fabs(b) < RMAX2 && fabs(d) < RMAX2) || (fabs(b) < RMIN && fabs(a) < RMAX2 && fabs(d) < RMAX2)) {
      a = __dmul_rn(a, SC); b = __dmul_rn(b, SC); c = __dmul_rn(c, SC); d = __dmul_rn(d, SC);
    }
    const double ratio = __ddiv_rn(c, d), denom = __fma_rn(c, ratio, d);
    if (fabs(ratio) > RMIN) {
      x = __ddiv_rn(__fma_rn(a, ratio, b), denom);
      y = __ddiv_rn(__fma_rn(b, ratio, -a), denom);
    } else {
      x = __ddiv_rn(__fma_rn(__ddiv_rn(a, d), c, b), denom);
      y = __ddiv_rn(__fma_rn(__ddiv_rn(b, d), c, -a), denom);
    }
  } else {
    if (fabs(c) >= RBIG) {
      a = __dmul_rn(a, 0.5); b = __dmul_rn(b, 0.5); c = __dmul_rn(c, 0.5); d = __dmul_rn(d, 0.5);
    }
    if (fabs(c) < RMIN2 || (fabs(a) < RMIN && fabs(b) < RMAX2 && fabs(c) < RMAX2) || (fabs(b) < RMIN && fabs(a) < RMAX2 && fabs(c) < RMAX2)) {
      a = __dmul_rn(a, SC); b = __dmul_rn(b, SC); c = __dmul_rn(c, SC); d = __dmul_rn(d, SC);
    }
    const double ratio = __ddiv_rn(d, c), denom = __fma_rn(d, ratio, c);
    if (fabs(ratio) > RMIN) {
      x = __ddiv_rn(__fma_rn(b, ratio, a), denom);
      y = __ddiv_rn(__fma_rn(-a, ratio, b), denom);
    } else {
      x = __ddiv_rn(__fma_rn(__ddiv_rn(b, c), d, a), denom);
      y = __ddiv_rn(__fma_rn(-__ddiv_rn(a, c), d, b), denom);
    }
  }
  return cxr<double>{x, y};
}
/* float: Smith's algorithm without libgcc's range scaling (the entries are int16-scale sums, far from over- and underflow) */
__device__ cxr<float> divdc3(float a, float b, float c, float d)
{
  if (fabsf(c) < fabsf(d)) {
    const float ratio = c / d, denom = __fmaf_rn(c, ratio, d);
    return cxr<float>{__fdiv_rn(__fmaf_rn(a, ratio, b), denom), __fdiv_rn(__fmaf_rn(b, ratio, -a), denom)};
  }
  const float ratio = d / c, denom = __fmaf_rn(d, ratio, c);
  return cxr<float>{__fdiv_rn(__fmaf_rn(b, ratio, a), denom), __fdiv_rn(__fmaf_rn(-a, ratio, b), denom)};
}
template <typename R> __device__ __forceinline__ int16_t qf_s16(R v)
{
  return (int16_t)(v > (R)32767.0 ? 32767 : v < (R)-32768.0 ? -32768 : roundR(v));
}
template <typename R> __device__ __forceinline__ void re_mmse2(const nr_gpu_demod_job_t &j, const nrd_c16 *h, const Shared *sh, int m, uint32_t i)
{
  const int shift = sh->d.log2_maxh, nb = j.nbRx;
  const int16_t qa = j.qm == 4 ? NRD_QAM16_n1 : j.qm == 6 ? NRD_QAM64_n1 : j.qm == 8 ? NRD_QAM256_n1 : 0;
  const int16_t qb = j.qm == 6 ? NRD_QAM64_n2 : j.qm == 8 ? NRD_QAM256_n2 : 0;
  const int16_t qr = j.qm == 8 ? NRD_QAM256_n3 : 0;
  const uint32_t nre = j.nre[m];
  if (i < nre) {
    cxr<R> M[2][5];
#pragma unroll
    for (int r = 0; r < 2; r++) {
#pragma unroll
      for (int c = 0; c < 2; c++) {
        nrd_c16 g = {0, 0};
        for (int a = 0; a < nb; a++) {
          const nrd_c16 hr = h[((size_t)m * 16 + r * nb + a) * EXT + i], hc = h[((size_t)m * 16 + c * nb + a) * EXT + i];
          const nrd_c16 p = nrd_cpx_mult_conj(hr, hc, shift);
          g = a == 0 ? p : nrd_c16{nrd_adds(g.r, p.r), nrd_adds(g.i, p.i)};
        }
        M[r][c] = cxr<R>{(R)g.r, (R)g.i};
        M[r][3 + c] = cxr<R>{(R)(r == c ? 1 : 0), (R)0};
      }
      const nrd_c16 b = ((const nrd_c16 *)j.comp)[((size_t)m * NR_GPU_DEMOD_MAX_NL + r) * j.buf + i];
      M[r][2] = cxr<R>{(R)b.r, (R)b.i};
    }
    /* the CPU's Gauss-Jordan for nl = 2, unrolled with static indices (no local-memory spills): k = 0 pivots between
     * the two rows, k = 1 has a single candidate; same operations, same order */
    bool singular = false;
    {
      const bool sw = cabs_c(M[1][0]) > cabs_c(M[0][0]);
      if (cabs_c(sw ? M[1][0] : M[0][0]) < (R)1e-9)
        singular = true;
      if (!singular) {
        if (sw) {
#pragma unroll
          for (int c = 0; c < 5; c++) {
            const cxr<R> t = M[0][c];
            M[0][c] = M[1][c];
            M[1][c] = t;
          }
        }
        const cxr<R> inv = divdc3((R)1, (R)0, M[0][0].r, M[0][0].i);
#pragma unroll
        for (int c = 0; c < 5; c++)
          M[0][c] = cmul(M[0][c], inv);
        const cxr<R> f = M[1][0];
        if (!(f.r == (R)0 && f.i == (R)0)) {
#pragma unroll
          for (int c = 0; c < 5; c++) {
            const cxr<R> t = cmul(M[0][c], f);
            M[1][c] = cxr<R>{M[1][c].r - t.r, M[1][c].i - t.i};
          }
        }
        if (cabs_c(M[1][1]) < (R)1e-9)
          singular = true;
        if (!singular) {
          const cxr<R> inv1 = divdc3((R)1, (R)0, M[1][1].r, M[1][1].i);
#pragma unroll
          for (int c = 0; c < 5; c++)
            M[1][c] = cmul(M[1][c], inv1);
          const cxr<R> f1 = M[0][1];
          if (!(f1.r == (R)0 && f1.i == (R)0)) {
#pragma unroll
            for (int c = 0; c < 5; c++) {
              const cxr<R> t = cmul(M[1][c], f1);
              M[0][c] = cxr<R>{M[0][c].r - t.r, M[0][c].i - t.i};
            }
          }
        }
      }
    }
#pragma unroll
    for (int r = 0; r < 2; r++) {
      const R dinv = singular ? (R)0 : M[r][3 + r].r;
      const R w = dinv > (R)1e-12 ? divR((R)1, dinv) : (R)0;
      const R orr = w > (R)0 ? M[r][2].r * w : (R)0, oi = w > (R)0 ? M[r][2].i * w : (R)0;
      const size_t row = ((size_t)m * NR_GPU_DEMOD_MAX_NL + r) * j.buf + i;
      ((nrd_c16 *)j.comp)[row] = nrd_c16{qf_s16<R>(orr), qf_s16<R>(oi)};
      const int16_t ma = qf_s16<R>((w * (R)qa) * (R)(1.0 / 32768.0)); /* == / 32768.0 exactly */
      const int16_t mb = qf_s16<R>((w * (R)qb) * (R)(1.0 / 32768.0));
      const int16_t mr = qf_s16<R>((w * (R)qr) * (R)(1.0 / 32768.0));
      ((nrd_c16 *)j.mag)[row] = nrd_c16{ma, ma};
      ((nrd_c16 *)j.magb)[row] = nrd_c16{mb, mb};
      ((nrd_c16 *)j.magr)[row] = nrd_c16{mr, mr};
    }
  }
}


/* ---- DECGPU: nr_dlsch_mmse() for Nl = 3, 4 (ISAC_PDSCH_GPU_EXACT_DEMOD_NL34=1). One thread per RE. --------------- */
/* QAM-FIX mode 2: the generic Gauss-Jordan of the CPU source with the operation order of the Nl = 2 kernel above
 * (same compiled loop): pivot by cabs, complex scaling cmul(M[k][c], inv), elimination M[r][c] - cmul(M[k][c], f). */
template <int NL, typename R> __device__ __forceinline__ void re_mmse2n(const nr_gpu_demod_job_t &j, const nrd_c16 *h, const Shared *sh, int m, uint32_t i)
{
  constexpr int NC = 2 * NL + 1;
  const int shift = sh->d.log2_maxh, nb = j.nbRx;
  const int16_t qa = j.qm == 4 ? NRD_QAM16_n1 : j.qm == 6 ? NRD_QAM64_n1 : j.qm == 8 ? NRD_QAM256_n1 : 0;
  const int16_t qb = j.qm == 6 ? NRD_QAM64_n2 : j.qm == 8 ? NRD_QAM256_n2 : 0;
  const int16_t qr = j.qm == 8 ? NRD_QAM256_n3 : 0;
  const uint32_t nre = j.nre[m];
  if (i < nre) {
    cxr<R> M[NL][NC];
    for (int r = 0; r < NL; r++) {
      for (int c = 0; c < NL; c++) {
        nrd_c16 g = {0, 0};
        for (int a = 0; a < nb; a++) {
          const nrd_c16 hr = h[((size_t)m * 16 + r * nb + a) * EXT + i], hc = h[((size_t)m * 16 + c * nb + a) * EXT + i];
          const nrd_c16 p = nrd_cpx_mult_conj(hr, hc, shift);
          g = a == 0 ? p : nrd_c16{nrd_adds(g.r, p.r), nrd_adds(g.i, p.i)};
        }
        M[r][c] = cxr<R>{(R)g.r, (R)g.i};
        M[r][NL + 1 + c] = cxr<R>{(R)(r == c ? 1 : 0), (R)0};
      }
      const nrd_c16 b = ((const nrd_c16 *)j.comp)[((size_t)m * NR_GPU_DEMOD_MAX_NL + r) * j.buf + i];
      M[r][NL] = cxr<R>{(R)b.r, (R)b.i};
    }
    bool singular = false;
    for (int k = 0; k < NL && !singular; k++) {
      int piv = k;
      for (int r = k + 1; r < NL; r++)
        if (cabs_c(M[r][k]) > cabs_c(M[piv][k]))
          piv = r;
      if (cabs_c(M[piv][k]) < (R)1e-9) {
        singular = true;
        break;
      }
      if (piv != k)
        for (int c = 0; c < NC; c++) {
          const cxr<R> t = M[k][c];
          M[k][c] = M[piv][c];
          M[piv][c] = t;
        }
      const cxr<R> inv = divdc3((R)1, (R)0, M[k][k].r, M[k][k].i);
      for (int c = 0; c < NC; c++)
        M[k][c] = cmul(M[k][c], inv);
      for (int r = 0; r < NL; r++) {
        if (r == k)
          continue;
        const cxr<R> f = M[r][k];
        if (f.r == (R)0 && f.i == (R)0)
          continue;
        for (int c = 0; c < NC; c++) {
          const cxr<R> t = cmul(M[k][c], f);
          M[r][c] = cxr<R>{M[r][c].r - t.r, M[r][c].i - t.i};
        }
      }
    }
    for (int r = 0; r < NL; r++) {
      const R dinv = singular ? (R)0 : M[r][NL + 1 + r].r;
      const R w = dinv > (R)1e-12 ? divR((R)1, dinv) : (R)0;
      const R orr = w > (R)0 ? M[r][NL].r * w : (R)0, oi = w > (R)0 ? M[r][NL].i * w : (R)0;
      const size_t row = ((size_t)m * NR_GPU_DEMOD_MAX_NL + r) * j.buf + i;
      ((nrd_c16 *)j.comp)[row] = nrd_c16{qf_s16<R>(orr), qf_s16<R>(oi)};
      const int16_t ma = qf_s16<R>((w * (R)qa) * (R)(1.0 / 32768.0));
      const int16_t mb = qf_s16<R>((w * (R)qb) * (R)(1.0 / 32768.0));
      const int16_t mr = qf_s16<R>((w * (R)qr) * (R)(1.0 / 32768.0));
      ((nrd_c16 *)j.mag)[row] = nrd_c16{ma, ma};
      ((nrd_c16 *)j.magb)[row] = nrd_c16{mb, mb};
      ((nrd_c16 *)j.magr)[row] = nrd_c16{mr, mr};
    }
  }
}

/* stock fixed-point MMSE (mode 0): mult_complex_vectors(in1, in2, s) = conj-free complex product, madd lanes, low 16
 * bits of re >> s and im >> s (srai / slli + blend, no saturation); nr_determin()'s cofactor recursion along index 0;
 * adds_epi16 sums; sign_epi16 negation. A[x][y] is the CPU's conjH_H_elements[0][x][y]. */
__device__ __forceinline__ nrd_c16 mcv(nrd_c16 a, nrd_c16 b, int s)
{
  const int32_t re = nrd_madd(a.r, b.r, a.i, nrd_neg_wrap(b.i)), im = nrd_madd(a.r, b.i, a.i, b.r);
  const int16_t ore = (int16_t)nrd_srai32(re, s);
  const int16_t oim = 16 - s >= 32 || 16 - s < 0 ? 0 : (int16_t)(((uint32_t)im << (16 - s)) >> 16);
  return nrd_c16{ore, oim};
}
/* GPUFUSE: the same cofactor recursion with the row / column sets as compile-time bit masks, so A[][] stays in
 * registers (the index-list version spilled it to local memory: 122 us per 51-RB 4-layer grant on an idle GB10).
 * Rows = set bits of XM in ascending order (xs), columns = set bits of YM (ys); same operations, same order. */
__host__ __device__ constexpr int dm_low(unsigned m) { return m & 1u ? 0 : 1 + dm_low(m >> 1); }
__host__ __device__ constexpr int dm_pop(unsigned m) { return m ? (int)(m & 1u) + dm_pop(m >> 1) : 0; }
__host__ __device__ constexpr int dm_nth(unsigned m, int k) { return k == 0 ? dm_low(m) : dm_nth(m & (m - 1), k - 1); }
template <int N, int K = 0, typename F> __device__ __forceinline__ void dm_for(F &&f)
{
  if constexpr (K < N) {
    f(std::integral_constant<int, K>{});
    dm_for<N, K + 1>(f);
  }
}
template <unsigned XM, unsigned YM> __device__ __forceinline__ nrd_c16 detm(const nrd_c16 (&A)[4][4], int sign, int s0)
{
  constexpr int S = dm_pop(XM), X0 = dm_low(XM);
  if constexpr (S == 1) {
    const nrd_c16 a = A[X0][dm_low(YM)];
    return sign < 0 ? nrd_c16{nrd_neg_wrap(a.r), nrd_neg_wrap(a.i)} : a;
  } else {
    nrd_c16 acc = {0, 0};
    dm_for<S>([&](auto rc) {
      constexpr int rtx = decltype(rc)::value, col = dm_nth(YM, rtx);
      const nrd_c16 t = detm<XM & (XM - 1), YM & ~(1u << col)>(A, ((rtx & 1) ? -1 : 1) * sign, s0);
      const nrd_c16 p = mcv(A[X0][col], t, s0);
      acc = rtx == 0 ? p : nrd_c16{nrd_adds(acc.r, p.r), nrd_adds(acc.i, p.i)};
    });
    return acc;
  }
}
template <int NL> __device__ __forceinline__ void re_mmse0n(const nr_gpu_demod_job_t &j, const nrd_c16 *h, const Shared *sh, int m, uint32_t i)
{
  const int shift = sh->d.log2_maxh, s0 = shift - 1, nb = j.nbRx;
  const uint32_t nre = j.nre[m];
  if (i < nre) {
    nrd_c16 A[4][4];
#pragma unroll
    for (int r = 0; r < NL; r++)
#pragma unroll
      for (int c = 0; c < NL; c++) { /* conjH_H_elements[a][c][r] = conj(h_r) h_c >> shift, summed over a */
        nrd_c16 g = {0, 0};
        for (int a = 0; a < nb; a++) {
          const nrd_c16 p = nrd_cpx_mult_conj(h[((size_t)m * 16 + r * nb + a) * EXT + i], h[((size_t)m * 16 + c * nb + a) * EXT + i], shift);
          g = a == 0 ? p : nrd_c16{nrd_adds(g.r, p.r), nrd_adds(g.i, p.i)};
        }
        A[c][r] = g;
      }
    if (NRD_NVAR(j) != 0)
#pragma unroll
      for (int p = 0; p < NL; p++) { /* add_epi32 of nvar >> 3 on the (re | im << 16) word */
        const uint32_t w = ((uint32_t)(uint16_t)A[p][p].r | (uint32_t)(uint16_t)A[p][p].i << 16) + (NRD_NVAR(j) >> 3);
        A[p][p] = nrd_c16{(int16_t)(uint16_t)(w & 0xffff), (int16_t)(uint16_t)(w >> 16)};
      }
    constexpr unsigned FULL = (1u << NL) - 1;
    const nrd_c16 det = detm<FULL, FULL>(A, 1, s0);
    nrd_c16 inv[NL][NL]; /* inv_H_h_H[rtx][ctx] */
    dm_for<NL>([&](auto rc) {
      dm_for<NL>([&](auto cc) {
        constexpr int rtx = decltype(rc)::value, ctx = decltype(cc)::value;
        inv[rtx][ctx] = detm<FULL & ~(1u << ctx), FULL & ~(1u << rtx)>(A, ((rtx & 1) ? -1 : 1) * ((ctx & 1) ? -1 : 1), s0);
      });
    });
    nrd_c16 y[NL];
#pragma unroll
    for (int l = 0; l < NL; l++)
      y[l] = ((const nrd_c16 *)j.comp)[((size_t)m * NR_GPU_DEMOD_MAX_NL + l) * j.buf + i];
#pragma unroll
    for (int rtx = 0; rtx < NL; rtx++) {
      nrd_c16 z = {0, 0};
#pragma unroll
      for (int ctx = 0; ctx < NL; ctx++) {
        const nrd_c16 p = mcv(inv[ctx][rtx], y[ctx], s0);
        z = nrd_c16{nrd_adds(z.r, p.r), nrd_adds(z.i, p.i)};
      }
      ((nrd_c16 *)j.comp)[((size_t)m * NR_GPU_DEMOD_MAX_NL + rtx) * j.buf + i] = z;
    }
    if (j.qm > 2) { /* thresholds from det: (det.r, det.r) * QAM amplitude, mulhrs, layer 0's rows */
      const int16_t qa = j.qm == 4 ? NRD_QAM16_n1 : j.qm == 6 ? NRD_QAM64_n1 : NRD_QAM256_n1;
      const int16_t qb = j.qm == 6 ? NRD_QAM64_n2 : j.qm == 8 ? NRD_QAM256_n2 : 0;
      const int16_t qr = j.qm == 8 ? NRD_QAM256_n3 : 0;
      const size_t row = ((size_t)m * NR_GPU_DEMOD_MAX_NL) * j.buf + i;
      const int16_t a = nrd_mulhrs(det.r, qa), b = nrd_mulhrs(det.r, qb), c = nrd_mulhrs(det.r, qr);
      ((nrd_c16 *)j.mag)[row] = nrd_c16{a, a};
      ((nrd_c16 *)j.magb)[row] = nrd_c16{b, b};
      ((nrd_c16 *)j.magr)[row] = nrd_c16{c, c};
    }
  }
}

/* ---- MMSE3GPU: nr_dlsch_mmse() mode 3 (exact double MMSE, ISAC_PDSCH_GPU_EXACT_DEMOD_MMSE3=1) ------------------------------
 * Per-RE arithmetic: nr_mmse_exact.h, the same functions the CPU runs (no FMA on either side). Per-symbol scalars
 * (k_mmse3_prep, one block per symbol): the strongest layer's sum |h|^2 is an integer sum (exact in int64 and in the CPU's
 * double), and the ZF residual of every 4th RE is summed by one thread in RE order, as the CPU loop does: bit-exact. */
__device__ __forceinline__ void m3_load(const nr_gpu_demod_job_t &j, const nrd_c16 *y, const nrd_c16 *h, int m, uint32_t i, int nl,
                                        int16_t (&hre)[4][4], int16_t (&him)[4][4], int16_t (&yre)[4], int16_t (&yim)[4])
{
  for (int a = 0; a < j.nbRx; a++) {
    const nrd_c16 v = y[((size_t)m * NR_GPU_DEMOD_MAX_RX + a) * EXT + i];
    yre[a] = v.r;
    yim[a] = v.i;
    for (int r = 0; r < nl; r++) {
      const nrd_c16 g = h[((size_t)m * 16 + r * j.nbRx + a) * EXT + i];
      hre[r][a] = g.r;
      him[r][a] = g.i;
    }
  }
}
/* the mode-3 solve in the precision of the instantiation (nr_mmse_exact.h: double = the CPU's function, float = Task 5) */
__device__ __forceinline__ int m3_exact(int nl, int nb, const int16_t (&hre)[4][4], const int16_t (&him)[4][4], const int16_t (&yre)[4], const int16_t (&yim)[4],
                                        double sc, double s2, double *xre, double *xim, double *ainv, double *r)
{
  return nr_mmse_exact_re(nl, nb, hre, him, yre, yim, sc, s2, xre, xim, ainv, r);
}
__device__ __forceinline__ int m3_exact(int nl, int nb, const int16_t (&hre)[4][4], const int16_t (&him)[4][4], const int16_t (&yre)[4], const int16_t (&yim)[4],
                                        float sc, float s2, float *xre, float *xim, float *ainv, float *r)
{
  return nr_mmse_exact_re_f(nl, nb, hre, him, yre, yim, sc, s2, xre, xim, ainv, r);
}
__device__ __forceinline__ double m3_gain_of(double e, int n, double sc) { return nr_mmse3_gain(e, n, sc); }
__device__ __forceinline__ float m3_gain_of(float e, int n, float sc) { return nr_mmse3_gain_f(e, n, sc); }
__device__ __forceinline__ void m3_re(int nl, int nb, const int16_t (&hre)[4][4], const int16_t (&him)[4][4], const int16_t (&yre)[4], const int16_t (&yim)[4], double sc, double s2,
                                      double gain, int16_t qa, int16_t qb, int16_t qr, int16_t (&z)[4][2], int16_t (&mg)[4][3])
{
  nr_mmse3_re(nl, nb, hre, him, yre, yim, sc, s2, gain, qa, qb, qr, z, mg);
}
__device__ __forceinline__ void m3_re(int nl, int nb, const int16_t (&hre)[4][4], const int16_t (&him)[4][4], const int16_t (&yre)[4], const int16_t (&yim)[4], float sc, float s2,
                                      float gain, int16_t qa, int16_t qb, int16_t qr, int16_t (&z)[4][2], int16_t (&mg)[4][3])
{
  nr_mmse3_re_f(nl, nb, hre, him, yre, yim, sc, s2, gain, qa, qb, qr, z, mg);
}
template <typename R> __device__ __forceinline__ R m3_nvfac_of(const nr_gpu_demod_job_t &j)
{
  if constexpr (std::is_same<R, double>::value) return j.m3_nvfac;
  else return j.m3_nvfac_f;
}
template <typename R> __device__ __forceinline__ void m3_put(Shared *sh, int m, R s2, R gain)
{
  if constexpr (std::is_same<R, double>::value) { sh->m3_s2[m] = s2; sh->m3_gain[m] = gain; }
  else { sh->m3_s2f[m] = s2; sh->m3_gainf[m] = gain; }
}
template <typename R> __device__ __forceinline__ R m3_s2_of(const Shared *sh, int m)
{
  if constexpr (std::is_same<R, double>::value) return sh->m3_s2[m];
  else return sh->m3_s2f[m];
}
template <typename R> __device__ __forceinline__ R m3_gain_at(const Shared *sh, int m)
{
  if constexpr (std::is_same<R, double>::value) return sh->m3_gain[m];
  else return sh->m3_gainf[m];
}
constexpr int M3_NRES = EXT / 4 + 1;
template <typename R = double> __global__ void k_mmse3_prep(__grid_constant__ const JobK jk_, const nrd_c16 *y, const nrd_c16 *h, Shared *sh)
{
  const nr_gpu_demod_job_t &j = NRD_JOB(jk_);
  __shared__ R rs[M3_NRES];
  __shared__ unsigned char okf[M3_NRES];
  __shared__ long long red[32][4];
  const int m = blockIdx.x, t = threadIdx.x, nl = j.nl, nb = j.nbRx;
  if (!(j.sym_mask >> m & 1))
    return;
  const int nre = j.nre[m];
  const R sc = ldexpR(R(), -sh->d.log2_maxh);
  long long e[4] = {0, 0, 0, 0};
  for (int i = t; i < nre; i += blockDim.x)
    for (int r = 0; r < nl; r++)
      for (int a = 0; a < nb; a++) {
        const nrd_c16 v = h[((size_t)m * 16 + r * nb + a) * EXT + i];
        e[r] += (long long)v.r * v.r + (long long)v.i * v.i;
      }
  for (int r = 0; r < 4; r++)
    for (int off = 16; off > 0; off >>= 1)
      e[r] += __shfl_down_sync(0xffffffffu, e[r], off);
  if ((t & 31) == 0)
    for (int r = 0; r < 4; r++)
      red[t >> 5][r] = e[r];
  const int nk = nb > nl ? (nre + 3) / 4 : 0;
  for (int k = t; k < nk; k += blockDim.x) {
    int16_t hre[4][4], him[4][4], yre[4], yim[4];
    m3_load(j, y, h, m, (uint32_t)(4 * k), nl, hre, him, yre, yim);
    R xre[4], xim[4], ainv[4], r = 0;
    okf[k] = (unsigned char)m3_exact(nl, nb, hre, him, yre, yim, sc, (R)0, xre, xim, ainv, &r);
    rs[k] = okf[k] ? r : (R)0;
  }
  __syncthreads();
  if (t == 0) {
    R gd = 0;
    for (int r = 0; r < nl; r++) {
      long long s = 0;
      for (int w = 0; w < (int)(blockDim.x >> 5); w++)
        s += red[w][r];
      if ((R)s > gd)
        gd = (R)s;
    }
    R s2 = 0;
    if (nb <= nl) {
      s2 = (R)NRD_NVAR(j) * sc * m3_nvfac_of<R>(j);
    } else {
      R res = 0;
      int nres = 0;
      for (int k = 0; k < nk; k++) {
        res += rs[k];
        nres += okf[k];
      }
      s2 = nres > 0 ? res / ((R)nres * (nb - nl)) * sc : (R)0;
    }
    m3_put<R>(sh, m, s2, m3_gain_of(gd, nre, sc));
  }
}
template <int NL, typename R> __device__ __forceinline__ void re_mmse3(const nr_gpu_demod_job_t &j, const nrd_c16 *y, const nrd_c16 *h, const Shared *sh, int m, uint32_t i)
{
  if (i >= j.nre[m])
    return;
  const int16_t qa = j.qm == 4 ? NRD_QAM16_n1 : j.qm == 6 ? NRD_QAM64_n1 : j.qm == 8 ? NRD_QAM256_n1 : 0;
  const int16_t qb = j.qm == 6 ? NRD_QAM64_n2 : j.qm == 8 ? NRD_QAM256_n2 : 0;
  const int16_t qr = j.qm == 8 ? NRD_QAM256_n3 : 0;
  int16_t hre[4][4], him[4][4], yre[4], yim[4], z[4][2], mg[4][3];
  m3_load(j, y, h, m, i, NL, hre, him, yre, yim);
  m3_re(NL, j.nbRx, hre, him, yre, yim, ldexpR(R(), -sh->d.log2_maxh), m3_s2_of<R>(sh, m), m3_gain_at<R>(sh, m), qa, qb, qr, z, mg);
  for (int r = 0; r < NL; r++) {
    const size_t row = ((size_t)m * NR_GPU_DEMOD_MAX_NL + r) * j.buf + i;
    ((nrd_c16 *)j.comp)[row] = nrd_c16{z[r][0], z[r][1]};
    ((nrd_c16 *)j.mag)[row] = nrd_c16{mg[r][0], mg[r][0]};
    ((nrd_c16 *)j.magb)[row] = nrd_c16{mg[r][1], mg[r][1]};
    ((nrd_c16 *)j.magr)[row] = nrd_c16{mg[r][2], mg[r][2]};
  }
}

/* ---- ISAC_PDSCH_EVM_GRANT on the GPU (nr_rx_pdsch()'s EVM block, Nl <= 2; Nl <= 4 with EVM_NL34): one block per symbol. Per-RE terms
 * follow the CPU formulas (the 1-layer solve x = b / G is the CPU's exact IEEE division); the sums are block
 * reductions, so the accumulator agrees with the CPU's sequential sums to rounding, not bit for bit. The decoder's
 * inputs (LLRs) never depend on it. ---------------------------------------------------------------------------- */
template <typename T> __device__ T block_sum(T v, T *red)
{
  const int t = threadIdx.x;
  red[t] = v;
  __syncthreads();
  for (int k = blockDim.x / 2; k > 0; k >>= 1) {
    if (t < k)
      red[t] += red[t + k];
    __syncthreads();
  }
  const T r = red[0];
  __syncthreads();
  return r;
}
/* block reduction of K partials per thread; the sums land in thread 0's v[] (warp shuffles, then the warps) */
template <typename T, int K> __device__ void block_vsum(T (&v)[K], T *sh)
{
  const int t = threadIdx.x, lane = t & 31, w = t >> 5, nw = blockDim.x >> 5;
  for (int k = 0; k < K; k++)
    for (int off = 16; off > 0; off >>= 1)
      v[k] += __shfl_down_sync(0xffffffffu, v[k], off);
  if (lane == 0)
    for (int k = 0; k < K; k++)
      sh[w * K + k] = v[k];
  __syncthreads();
  if (t == 0)
    for (int k = 0; k < K; k++) {
      T a = sh[k];
      for (int q = 1; q < nw; q++)
        a += sh[q * K + k];
      v[k] = a;
    }
  __syncthreads();
}
constexpr int EVM_CH = 240;                              /* REs per EVM block: 20 RBs, so an RB never straddles */
constexpr int EVM_NCH = (EXT + EVM_CH - 1) / EVM_CH;     /* blocks per symbol */
/* partial slots per (symbol, block). Pass 1: pw[4] nv; pass 2: err_l[4] qr[4] qi[4] qd[4] qx[4] cr[4] ci[4] P[4] Y[4] nterm,
 * then err_k[12] n_k[12]; pass 3: Cr[10] Ci[10]. */
constexpr int EVM_P1 = 0, EVM_NV = 4, EVM_P2 = 5, EVM_NP2 = 37, EVM_K = EVM_P2 + EVM_NP2, EVM_P3 = EVM_K + 24, EVM_P = EVM_P3 + 20;
template <class A = double> struct EvmCtx {
  int nl, nbRx, nb, a0, a1, lmax;
  A ideal_pow;
};
/* A: the storage / accumulator type of the EVM sums (double: the default chain; float: the FP32 pass class, no FP64 in the kernels). 2-vectors of it: */
template <class A> struct V2;
template <> struct V2<double> { typedef double2 t; };
template <> struct V2<float> { typedef float2 t; };
template <class A> __device__ __forceinline__ typename V2<A>::t mk2(A x, A y) { typename V2<A>::t v; v.x = x; v.y = y; return v; }
template <class A> __device__ __forceinline__ bool evm_ctx(const nr_gpu_demod_job_t &j, const Shared *sh, int m, EvmCtx<A> &c)
{
  if (!(j.sym_mask >> m & 1) || !(j.nl >= 1 && j.nl <= 4 && j.nre[m] >= 12 && j.nre[m] <= j.ext))
    return false;
  const nrd_decision d = sh->d;
  c.nl = j.nl;
  c.nbRx = j.nbRx;
  c.nb = j.nbRx < 4 ? j.nbRx : 4;
  c.a0 = j.nl == 1 ? d.mrc_rx_index : 0;
  c.a1 = j.nl == 1 ? d.mrc_rx_index + d.mrc_nb_rx : j.nbRx;
  c.lmax = (1 << (j.qm / 2)) - 1;
  A ms = 0;
  int nlev = 0;
  for (int l = 1; l <= c.lmax; l += 2) {
    ms += (A)l * l;
    nlev++;
  }
  c.ideal_pow = (A)2 * ms / nlev;
  return true;
}
template <class A> __device__ __forceinline__ bool evm_good(const A *S, int nl)
{
  bool g = (unsigned)S[EVM_NV] >= 12;
  for (int l = 0; l < nl; l++)
    g = g && S[EVM_P1 + l] > (A)0;
  return g;
}
template <typename T> __device__ __forceinline__ void evm_slice(T x, T scale, int lmax, T &v, T &s)
{
  v = x * scale;
  s = (T)2 * floor(v * (T)0.5) + (T)1; /* v * 0.5 == v / 2.0 exactly; 2 floor + 1 is exact */
  s = s > (T)lmax ? (T)lmax : s < (T)-lmax ? (T)-lmax : s;
}
/* Per-RE math type of the GPU EVM: T = float (default, ISAC_PDSCH_GPU_EXACT_DEMOD_EVM=1; the GB10 has ~1/64-rate FP64)
 * or double (=2, the CPU's formulas in double). Sums are always accumulated in double. Diagnostic only: the decoder's
 * LLRs never depend on it. */
/* block sums of K float terms per thread: float warp shuffles, then the 8 warp partials in double (fixed order) */
template <int K, class A = double> __device__ void block_colsum(const float (&v0)[K], float *col, A *out)
{
  const int t = threadIdx.x, lane = t & 31, w = t >> 5, nw = blockDim.x >> 5;
  float v[K];
#pragma unroll
  for (int k = 0; k < K; k++) {
    v[k] = v0[k];
#pragma unroll
    for (int off = 16; off > 0; off >>= 1)
      v[k] += __shfl_down_sync(0xffffffffu, v[k], off);
  }
  if (lane == 0)
#pragma unroll
    for (int k = 0; k < K; k++)
      col[w * K + k] = v[k];
  __syncthreads();
  if (t < K) {
    A a = 0;
    for (int q = 0; q < nw; q++)
      a += (A)col[q * K + t];
    out[t] = a;
  }
  __syncthreads();
}
template <typename T> struct cxT {
  T r, i;
};
template <typename T> __device__ __forceinline__ cxT<T> cmulT(cxT<T> a, cxT<T> b)
{
  return cxT<T>{a.r * b.r - a.i * b.i, a.r * b.i + a.i * b.r};
}
/* pass 1: per-RE zero forcing; block partials {pw0, pw1, nv} */
template <typename T, int NLM, typename A = double> /* NLM: largest Nl of the launch (2 keeps the Nl <= 2 register footprint) */
__global__ void k_evm1(__grid_constant__ const JobK jk_, const nrd_c16 *y, const nrd_c16 *h, typename V2<A>::t *xs, const Shared *sh, A *part, int erase)
{
  const nr_gpu_demod_job_t &j = NRD_JOB(jk_);
  __shared__ A shd[8 * 5];
  const int m = blockIdx.y, t = threadIdx.x;
  EvmCtx<A> c;
  if (!evm_ctx(j, sh, m, c))
    return;
  const uint32_t nre = j.nre[m], i = blockIdx.x * EVM_CH + t;
  if (blockIdx.x * EVM_CH >= nre)
    return;
  const nrd_c16 *Y = y + (size_t)m * NR_GPU_DEMOD_MAX_RX * EXT;
  const nrd_c16 *H = h + (size_t)m * 16 * EXT;
  typename V2<A>::t *X = xs + (size_t)m * 4 * EXT;
  A v[5] = {0, 0, 0, 0, 0};
  if (t < EVM_CH && i < nre) {
    const int nl = c.nl, nbRx = c.nbRx;
    long long g[NLM][NLM + 1][2] = {{{0}}}; /* [l][k < nl] = conj(h_l) h_k, [l][NLM] = conj(h_l) y */
    for (int a = c.a0; a < c.a1; a++) {
      const nrd_c16 yy = Y[(size_t)a * EXT + i];
#pragma unroll
      for (int l = 0; l < NLM; l++) {
        if (l >= nl)
          break;
        const nrd_c16 hl = H[((size_t)l * nbRx + a) * EXT + i];
#pragma unroll
        for (int k = 0; k < NLM; k++) {
          if (k >= nl)
            break;
          const nrd_c16 hk = H[((size_t)k * nbRx + a) * EXT + i];
          g[l][k][0] += (long long)hl.r * hk.r + (long long)hl.i * hk.i; /* conj(h_l) h_k: exact integers */
          g[l][k][1] += (long long)hl.r * hk.i - (long long)hl.i * hk.r;
        }
        g[l][NLM][0] += (long long)hl.r * yy.r + (long long)hl.i * yy.i;
        g[l][NLM][1] += (long long)hl.r * yy.i - (long long)hl.i * yy.r;
      }
    }
    bool ok = true;
    if (erase) {
      bool z = true;
      for (int a = c.a0; a < c.a1 && z; a++)
        z = Y[(size_t)a * EXT + i].r == 0 && Y[(size_t)a * EXT + i].i == 0;
      ok = !z;
    }
    typename V2<A>::t x[NLM];
    for (int l = 0; l < NLM; l++)
      x[l] = mk2<A>(0, 0);
    if (nl == 1) {
      const A g00 = (A)g[0][0][0];
      ok = ok && g00 > (A)1e-9;
      if (ok) { /* __divdc3(a, b, c, 0) == (a / c, b / c) */
        if constexpr (std::is_same<A, float>::value)
          x[0] = mk2<A>((float)g[0][NLM][0] / g00, (float)g[0][NLM][1] / g00);
        else if (sizeof(T) == sizeof(double))
          x[0] = make_double2(__ddiv_rn((double)g[0][NLM][0], g00), __ddiv_rn((double)g[0][NLM][1], g00));
        else
          x[0] = make_double2((double)((float)g[0][NLM][0] / (float)g00), (double)((float)g[0][NLM][1] / (float)g00));
      }
    } else if (std::is_same<A, float>::value ? (NLM > 2) : (sizeof(T) == sizeof(double) || NLM > 2)) { /* the CPU's elimination (double; float for the FP32 class; always for Nl = 3, 4) */
      cxr<A> G[NLM][NLM + 1]; /* column nl is b */
      for (int l = 0; l < nl; l++) {
        for (int k = 0; k < nl; k++)
          G[l][k] = cxr<A>{(A)g[l][k][0], (A)g[l][k][1]};
        G[l][nl] = cxr<A>{(A)g[l][NLM][0], (A)g[l][NLM][1]};
      }
      for (int cc = 0; cc < nl && ok; cc++) {
        int piv = cc;
        for (int r = cc + 1; r < nl; r++)
          if (cabs_c(G[r][cc]) > cabs_c(G[piv][cc]))
            piv = r;
        if (cabs_c(G[piv][cc]) <= (A)1e-9) {
          ok = false;
          break;
        }
        if (piv != cc)
          for (int k = 0; k <= nl; k++) {
            const cxr<A> tt = G[cc][k];
            G[cc][k] = G[piv][k];
            G[piv][k] = tt;
          }
        for (int r = 0; r < nl; r++) {
          if (r == cc)
            continue;
          const cxr<A> f = divdc3(G[r][cc].r, G[r][cc].i, G[cc][cc].r, G[cc][cc].i);
          for (int k = cc; k <= nl; k++) {
            const cxr<A> p = cmul(G[cc][k], f);
            G[r][k] = cxr<A>{G[r][k].r - p.r, G[r][k].i - p.i};
          }
        }
      }
      for (int l = 0; l < nl && ok; l++) {
        const cxr<A> q = divdc3(G[l][nl].r, G[l][nl].i, G[l][l].r, G[l][l].i);
        x[l] = mk2<A>(q.r, q.i);
      }
    } else { /* 2 x 2 Hermitian solve in float (Cramer): x = adj(G) b / det(G) */
      const float g00 = (float)g[0][0][0], g11 = (float)g[1][1][0];
      const cxT<float> g01 = {(float)g[0][1][0], (float)g[0][1][1]}, g10 = {(float)g[1][0][0], (float)g[1][0][1]};
      const cxT<float> b0 = {(float)g[0][NLM][0], (float)g[0][NLM][1]}, b1 = {(float)g[1][NLM][0], (float)g[1][NLM][1]};
      const cxT<float> p = cmulT(g01, g10);
      const float det = g00 * g11 - p.r; /* real for a Hermitian G */
      const float mx = fmaxf(fabsf(g00), fabsf(g11));
      ok = ok && mx > 1e-9f && fabsf(det) > 1e-7f * mx * mx;
      if (ok) {
        const cxT<float> u = cmulT(g01, b1), w = cmulT(g10, b0);
        x[0] = mk2<A>((A)((g11 * b0.r - u.r) / det), (A)((g11 * b0.i - u.i) / det));
        x[1] = mk2<A>((A)((g00 * b1.r - w.r) / det), (A)((g00 * b1.i - w.i) / det));
      }
    }
    for (int l = 0; l < nl; l++) {
      if (!ok) {
        X[(size_t)l * EXT + i] = mk2<A>(nanR(A()), nanR(A()));
        continue;
      }
      X[(size_t)l * EXT + i] = x[l];
      if constexpr (std::is_same<A, float>::value)
        v[l] = x[l].x * x[l].x + x[l].y * x[l].y;
      else
        v[l] = sizeof(T) == sizeof(double) || NLM > 2 ? __fma_rn(x[l].x, x[l].x, __dmul_rn(x[l].y, x[l].y))
                                           : (double)((float)x[l].x * (float)x[l].x + (float)x[l].y * (float)x[l].y);
    }
    v[EVM_NV] = ok ? (A)1 : (A)0;
  }
  block_vsum(v, shd);
  if (t == 0)
    for (int k = 0; k < 5; k++)
      part[((size_t)m * EVM_NCH + blockIdx.x) * EVM_P + EVM_P1 + k] = v[k];
}
/* fold the block partials of slots [o, o+n) per symbol into sum[m][0..n) (block order) */
template <class A = double> __global__ void k_evm_fold(__grid_constant__ const JobK jk_, const A *part, int o, int n, A *sum)
{
  const nr_gpu_demod_job_t &j = NRD_JOB(jk_);
  const int m = blockIdx.x, k = threadIdx.x;
  if (k >= n || !(j.sym_mask >> m & 1))
    return;
  const int nch = (j.nre[m] + EVM_CH - 1) / EVM_CH;
  A a = 0;
  for (int b = 0; b < nch && b < EVM_NCH; b++)
    a += part[((size_t)m * EVM_NCH + b) * EVM_P + o + k];
  sum[m * EVM_P + o + k] = a;
}
/* pass 2: decisions, error and LS-fit partials; per-(RE mod 12) and per-RB error sums in RE order from shared memory */
template <typename T, typename A = double>
__global__ void k_evm2(__grid_constant__ const JobK jk_, const nrd_c16 *y, const nrd_c16 *h, const typename V2<A>::t *xs, typename V2<A>::t *sd, Shared *sh,
                       A *part, const A *sum)
{
  const nr_gpu_demod_job_t &j = NRD_JOB(jk_);
  __shared__ A shd[8 * EVM_NP2];
  __shared__ float colf[8 * EVM_NP2];
  __shared__ float s_e[4][EVM_CH];  /* e2 / ideal per (layer, RE of the block), -1 = no term */
  __shared__ A s_sc[4];
  const int m = blockIdx.y, t = threadIdx.x;
  EvmCtx<A> c;
  nr_gpu_evm_sym_t *E = &sh->evm[m];
  if (!evm_ctx(j, sh, m, c))
    return;
  const uint32_t nre = j.nre[m], i = blockIdx.x * EVM_CH + t;
  if (blockIdx.x * EVM_CH >= nre)
    return;
  const A *S = sum + m * EVM_P;
  const unsigned nv = (unsigned)S[EVM_NV];
  if (!evm_good(S, c.nl))
    return;
  if (t < 4)
    s_sc[t] = t < c.nl ? sqrtR(c.ideal_pow / (S[EVM_P1 + t] / nv)) : (A)0;
  __syncthreads();
  const T scale[4] = {(T)s_sc[0], (T)s_sc[1], (T)s_sc[2], (T)s_sc[3]};
  const nrd_c16 *Y = y + (size_t)m * NR_GPU_DEMOD_MAX_RX * EXT;
  const nrd_c16 *H = h + (size_t)m * 16 * EXT;
  const typename V2<A>::t *X = xs + (size_t)m * 4 * EXT;
  typename V2<A>::t *D = sd + (size_t)m * 4 * EXT;
  const int lmax = c.lmax, nl = c.nl;
  const T inv_ideal = (T)1 / (T)c.ideal_pow;
  /* v: err_l[4] qr[4] qi[4] qd[4] qx[4] cr[4] ci[4] P[4] Y[4] nterm */
  T v[EVM_NP2];
#pragma unroll
  for (int k = 0; k < EVM_NP2; k++)
    v[k] = 0;
  if (t < EVM_CH)
    for (int l = 0; l < 4; l++)
      s_e[l][t] = -1.f;
  if (t < EVM_CH && i < nre && !isnan(X[i].x)) {
    T sr[4] = {0, 0, 0, 0}, sqv[4] = {0, 0, 0, 0};
#pragma unroll
    for (int l = 0; l < 4; l++) {
      if (l >= nl)
        break;
      const typename V2<A>::t x = X[(size_t)l * EXT + i];
      T vi, vq, si, sq;
      evm_slice<T>((T)x.x, scale[l], lmax, vi, si);
      evm_slice<T>((T)x.y, scale[l], lmax, vq, sq);
      const T e2 = (vi - si) * (vi - si) + (vq - sq) * (vq - sq);
      v[l] += e2;
      s_e[l][t] = (float)(e2 * inv_ideal);
      v[4 + l] += vi * si + vq * sq;
      v[8 + l] += vq * si - vi * sq;
      v[12 + l] += si * si + sq * sq;
      v[16 + l] += vi * vi + vq * vq;
      sr[l] = si / scale[l];
      sqv[l] = sq / scale[l];
      D[(size_t)l * EXT + i] = mk2<A>((A)sr[l], (A)sqv[l]);
      v[36] += (T)1;
    }
#pragma unroll
    for (int a = 0; a < 4; a++) {
      if (a >= c.nb)
        break;
      const nrd_c16 yy = Y[(size_t)a * EXT + i];
      T hsr = 0, hsi = 0;
#pragma unroll
      for (int l = 0; l < 4; l++) {
        if (l >= nl)
          break;
        const nrd_c16 hh = H[((size_t)l * c.nbRx + a) * EXT + i];
        hsr += (T)hh.r * sr[l] - (T)hh.i * sqv[l];
        hsi += (T)hh.r * sqv[l] + (T)hh.i * sr[l];
      }
      v[20 + a] += (T)yy.r * hsr + (T)yy.i * hsi;
      v[24 + a] += (T)yy.i * hsr - (T)yy.r * hsi;
      v[28 + a] += hsr * hsr + hsi * hsi;
      v[32 + a] += (T)yy.r * (T)yy.r + (T)yy.i * (T)yy.i;
    }
  }
  __syncthreads();
  /* per RE index mod 12 (this block starts at a multiple of 12): thread k sums its REs in order, layers in order */
  if (t < 12) {
    float acc = 0.f;
    unsigned n = 0;
    for (int q = t; q < EVM_CH; q += 12)
      for (int l = 0; l < nl; l++)
        if (s_e[l][q] >= 0.f) {
          acc += s_e[l][q];
          n++;
        }
    part[((size_t)m * EVM_NCH + blockIdx.x) * EVM_P + EVM_K + t] = acc;
    part[((size_t)m * EVM_NCH + blockIdx.x) * EVM_P + EVM_K + 12 + t] = n;
  }
  /* per-RB map (symbols 0, 1, 3; layer 0): the CPU's own float sequence for that RB */
  if ((m == 0 || m == 1 || m == 3) && t < EVM_CH / 12) {
    const uint32_t rb = blockIdx.x * (EVM_CH / 12) + t;
    float acc = 0.f;
    unsigned n = 0;
    for (int q = t * 12; q < t * 12 + 12; q++)
      if (s_e[0][q] >= 0.f) {
        acc += s_e[0][q];
        n++;
      }
    if (rb < 276 && rb * 12 < nre) {
      E->err_rb[rb] = acc;
      E->n_rb[rb] = (uint16_t)n;
    }
  }
  A *dst = part + ((size_t)m * EVM_NCH + blockIdx.x) * EVM_P + EVM_P2;
  if constexpr (sizeof(T) == sizeof(float)) {
    block_colsum(*(const float(*)[EVM_NP2])v, colf, dst);
  } else {
    A dv[EVM_NP2];
#pragma unroll
    for (int k = 0; k < EVM_NP2; k++)
      dv[k] = (A)v[k];
    block_vsum(dv, shd);
    if (t == 0)
      for (int k = 0; k < EVM_NP2; k++)
        dst[k] = dv[k];
  }
}
/* pass 3: cross-branch correlation of the LS residuals (needs the symbol's alpha_a from pass 2) */
template <typename T, typename A = double>
__global__ void k_evm3(__grid_constant__ const JobK jk_, const nrd_c16 *y, const nrd_c16 *h, const typename V2<A>::t *xs, const typename V2<A>::t *sd,
                       const Shared *sh, A *part, const A *sum)
{
  const nr_gpu_demod_job_t &j = NRD_JOB(jk_);
  __shared__ A shd[8 * 20];
  __shared__ float colf[8 * 20];
  __shared__ A s_ar[4][2];
  const int m = blockIdx.y, t = threadIdx.x;
  EvmCtx<A> c;
  if (!evm_ctx(j, sh, m, c))
    return;
  const uint32_t nre = j.nre[m], i = blockIdx.x * EVM_CH + t;
  if (blockIdx.x * EVM_CH >= nre)
    return;
  const A *S = sum + m * EVM_P;
  if (!evm_good(S, c.nl))
    return;
  if (t < c.nb) {
    const A P = S[EVM_P2 + 28 + t];
    s_ar[t][0] = P > (A)0 ? S[EVM_P2 + 20 + t] / P : (A)0;
    s_ar[t][1] = P > (A)0 ? S[EVM_P2 + 24 + t] / P : (A)0;
  }
  __syncthreads();
  const nrd_c16 *Y = y + (size_t)m * NR_GPU_DEMOD_MAX_RX * EXT;
  const nrd_c16 *H = h + (size_t)m * 16 * EXT;
  const typename V2<A>::t *X = xs + (size_t)m * 4 * EXT;
  const typename V2<A>::t *D = sd + (size_t)m * 4 * EXT;
  T v[20];
#pragma unroll
  for (int k = 0; k < 20; k++)
    v[k] = 0;
  if (t < EVM_CH && i < nre && !isnan(X[i].x)) {
    cxT<T> e[4];
#pragma unroll
    for (int a = 0; a < 4; a++) {
      e[a] = cxT<T>{0, 0};
      if (a >= c.nb)
        continue;
      cxT<T> hs = {0, 0};
#pragma unroll
      for (int l = 0; l < 4; l++) {
        if (l >= c.nl)
          break;
        const nrd_c16 hh = H[((size_t)l * c.nbRx + a) * EXT + i];
        const typename V2<A>::t dd = D[(size_t)l * EXT + i];
        const cxT<T> p = cmulT(cxT<T>{(T)hh.r, (T)hh.i}, cxT<T>{(T)dd.x, (T)dd.y});
        hs.r += p.r;
        hs.i += p.i;
      }
      const cxT<T> q = cmulT(cxT<T>{(T)s_ar[a][0], (T)s_ar[a][1]}, hs);
      const nrd_c16 yy = Y[(size_t)a * EXT + i];
      e[a] = cxT<T>{(T)yy.r - q.r, (T)yy.i - q.i};
    }
    int k = 0;
#pragma unroll
    for (int a = 0; a < 4; a++)
#pragma unroll
      for (int b = a; b < 4; b++, k++) {
        if (a >= c.nb || b >= c.nb)
          continue;
        const cxT<T> p = cmulT(e[a], cxT<T>{e[b].r, -e[b].i});
        v[k] = p.r;
        v[10 + k] = p.i;
      }
  }
  A *dst = part + ((size_t)m * EVM_NCH + blockIdx.x) * EVM_P + EVM_P3;
  if constexpr (sizeof(T) == sizeof(float)) {
    block_colsum(*(const float(*)[20])v, colf, dst);
  } else {
    A dv[20];
#pragma unroll
    for (int k = 0; k < 20; k++)
      dv[k] = (A)v[k];
    block_vsum(dv, shd);
    if (t == 0)
      for (int k = 0; k < 20; k++)
        dst[k] = dv[k];
  }
}
__global__ void k_evm_zero(Shared *sh)
{
  for (int rb = threadIdx.x; rb < 276; rb += blockDim.x) {
    sh->evm[blockIdx.x].err_rb[rb] = 0.f;
    sh->evm[blockIdx.x].n_rb[rb] = 0;
  }
}
/* export the symbol's sums for the host fold */
template <class A = double> __global__ void k_evm_out(__grid_constant__ const JobK jk_, Shared *sh, const A *sum)
{
  const nr_gpu_demod_job_t &j = NRD_JOB(jk_);
  const int m = blockIdx.x;
  nr_gpu_evm_sym_t *E = &sh->evm[m];
  EvmCtx<A> c;
  if (threadIdx.x != 0)
    return;
  E->valid = 0;
  if (!evm_ctx(j, sh, m, c))
    return;
  const A *S = sum + m * EVM_P;
  const unsigned nv = (unsigned)S[EVM_NV];
  if (!evm_good(S, c.nl))
    return;
  const A *V = S + EVM_P2;
  for (int l = 0; l < c.nl; l++) {
    E->err_l[l] = V[l];
    E->qr[l] = V[4 + l];
    E->qi[l] = V[8 + l];
    E->qd[l] = V[12 + l];
    E->qx[l] = V[16 + l];
  }
  for (int a = 0; a < c.nb; a++) {
    E->cr[a] = V[20 + a];
    E->ci[a] = V[24 + a];
    E->P[a] = V[28 + a];
    E->Y[a] = V[32 + a];
  }
  for (int k = 0; k < 12; k++) {
    E->err_k[k] = (float)S[EVM_K + k];
    E->n_k[k] = (uint32_t)S[EVM_K + 12 + k];
  }
  int k = 0;
  for (int a = 0; a < c.nb; a++)
    for (int b = a; b < c.nb; b++, k++) {
      E->Cr[a][b] = S[EVM_P3 + k];
      E->Ci[a][b] = S[EVM_P3 + 10 + k];
    }
  E->nv = nv;
  E->nterm = (uint32_t)V[36];
  E->ideal_pow = c.ideal_pow;
  E->nb = c.nb;
  E->rbmap = m == 0 || m == 1 || m == 3;
  E->row = m == 0 ? 0 : m == 1 ? 1 : 2;
  E->valid = 1;
}

/* ---- LLRs (nr_dlsch_llr) and layer demapping (nr_layer_demapping), straight into the decoder buffer ---------- */
__device__ __forceinline__ void re_llr(const nr_gpu_demod_job_t &j, int m, uint32_t i)
{
  const uint32_t nre = j.nre[m];
  if (i < nre)
    for (int l = 0; l < j.nl; l++) {
      const size_t row = ((size_t)m * NR_GPU_DEMOD_MAX_NL + l) * j.buf;
      const size_t mrow = ((size_t)m * NR_GPU_DEMOD_MAX_NL + (j.mag_per_layer ? l : 0)) * j.buf;
      nrd_llr_re_out(j.qm, i, nre, (const nrd_c16 *)j.comp + row, (const nrd_c16 *)j.mag + mrow,
                     (const nrd_c16 *)j.magb + mrow, (const nrd_c16 *)j.magr + mrow,
                     j.llr + j.llr_off[m] + ((size_t)i * j.nl + l) * j.qm);
    }
}

/* ---- GPUFUSE: compensation, multi-layer MMSE, kappa sums and LLRs of one RE in one launch (they are per-RE
 * independent; the thread that compensates RE i also equalises it and writes its LLRs). MM: 0 none (Nl = 1),
 * 1 QAM-FIX Nl = 2, 2/3 QAM-FIX Nl = 3/4, 4/5 stock MMSE Nl = 3/4, 6/7/8 MMSE3GPU mode 3 Nl = 2/3/4. The kappa sums are integers, so block sums plus
 * atomics give the same totals as the former ordered reduction (zeroed per symbol in k_mask_pw). */
template <int MM, typename R = double> __global__ void k_tail(__grid_constant__ const JobK jk_, const nrd_c16 *y, const nrd_c16 *h, Shared *sh)
{
  const nr_gpu_demod_job_t &j = NRD_JOB(jk_);
  __shared__ long long red[8][8];
  const int m = blockIdx.y;
  if (!(j.sym_mask >> m & 1))
    return;
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x, nre = j.nre[m];
  re_comp(j, y, h, sh, m, i);
  if (MM == 1)
    re_mmse2<R>(j, h, sh, m, i);
  else if (MM == 2)
    re_mmse2n<3, R>(j, h, sh, m, i);
  else if (MM == 3)
    re_mmse2n<4, R>(j, h, sh, m, i);
  else if (MM == 4)
    re_mmse0n<3>(j, h, sh, m, i);
  else if (MM == 5)
    re_mmse0n<4>(j, h, sh, m, i);
  else if (MM == 6)
    re_mmse3<2, R>(j, y, h, sh, m, i);
  else if (MM == 7)
    re_mmse3<3, R>(j, y, h, sh, m, i);
  else if (MM == 8)
    re_mmse3<4, R>(j, y, h, sh, m, i);
  if (j.want_kappa && j.qm > 2) { /* block-uniform condition */
    long long v[8];
    for (int l = 0; l < 4; l++) {
      long long a = 0, b = 0;
      if (l < j.nl && i < nre) {
        const nrd_c16 z = ((const nrd_c16 *)j.comp)[((size_t)m * NR_GPU_DEMOD_MAX_NL + l) * j.buf + i];
        const nrd_c16 ma = ((const nrd_c16 *)j.mag)[((size_t)m * NR_GPU_DEMOD_MAX_NL + (j.mag_per_layer ? l : 0)) * j.buf + i];
        a = (z.r < 0 ? -(int)z.r : z.r) + (z.i < 0 ? -(int)z.i : z.i);
        b = 2 * (long long)ma.r;
      }
      v[l] = a;
      v[4 + l] = b;
    }
    const int lane = threadIdx.x & 31, w = threadIdx.x >> 5;
    for (int k = 0; k < 8; k++)
      for (int off = 16; off > 0; off >>= 1)
        v[k] += __shfl_down_sync(0xffffffffu, v[k], off);
    if (lane == 0)
      for (int k = 0; k < 8; k++)
        red[w][k] = v[k];
    __syncthreads();
    if (threadIdx.x < 8) {
      const int k = threadIdx.x;
      long long s = 0;
      for (int q = 0; q < (int)(blockDim.x >> 5); q++)
        s += red[q][k];
      if ((k & 3) < j.nl && s != 0)
        atomicAdd((unsigned long long *)(k < 4 ? &sh->kap_sz[m][k] : &sh->kap_sm[m][k - 4]), (unsigned long long)s);
    }
  }
  re_llr(j, m, i);
}
} // namespace nrdm

using namespace nrdm;

extern "C" int nr_pdsch_gpu_demod_init(int nant)
{
  if (!nr_gpu_module_ready()) return -1; // CPU tier (no device, arch mismatch or NR_GPU_FORCE_MODEL=cpu): refuse, the host keeps the CPU path
  if (g_ok)
    return 0;
  if (nant > NR_GPU_DEMOD_MAX_RX)
    return -1;
  int lo = 0, hi = 0;
  if (cudaDeviceGetStreamPriorityRange(&lo, &hi) != cudaSuccess)
    return -1;
  for (int c = 0; c < CTX + PASS_INST + 1; c++) {
    Ctx &x = g_ctx[c];
    if ((!nr_gpu_green_stream(&x.s, hi, true, c) && cudaStreamCreateWithPriority(&x.s, cudaStreamNonBlocking, hi) != cudaSuccess)
        || cudaMalloc((void **)&x.y, sizeof(nrd_c16) * NSYM * NR_GPU_DEMOD_MAX_RX * EXT) != cudaSuccess
        || cudaMalloc((void **)&x.h, sizeof(nrd_c16) * NSYM * 16 * EXT) != cudaSuccess
        || cudaMallocHost((void **)&x.hy, sizeof(nrd_c16) * NSYM * NR_GPU_DEMOD_MAX_RX * EXT) != cudaSuccess
        || cudaMallocHost((void **)&x.hh, sizeof(nrd_c16) * NSYM * 16 * EXT) != cudaSuccess
        || cudaMalloc((void **)&x.sd, sizeof(double2) * NSYM * 4 * EXT) != cudaSuccess
        || cudaMalloc((void **)&x.part, sizeof(double) * NSYM * EVM_NCH * EVM_P) != cudaSuccess
        || cudaMalloc((void **)&x.sum, sizeof(double) * NSYM * EVM_P) != cudaSuccess
        || cudaMalloc((void **)&x.xs, sizeof(double2) * NSYM * 4 * EXT) != cudaSuccess
        || cudaMalloc((void **)&x.dsh, sizeof(Shared)) != cudaSuccess
        || cudaMallocHost((void **)&x.sh, sizeof(Shared)) != cudaSuccess
) {
      fprintf(stderr, "nr_pdsch_gpu_demod_init: %s\n", cudaGetErrorString(cudaGetLastError()));
      return -1;
    }
    memset(x.sh, 0, sizeof(Shared));
    if (cudaMemset(x.dsh, 0, sizeof(Shared)) != cudaSuccess)
      return -1;
    if ((!nr_gpu_green_stream(&x.s2, hi, true, c) && cudaStreamCreateWithPriority(&x.s2, cudaStreamNonBlocking, hi) != cudaSuccess)
        || cudaEventCreateWithFlags(&x.fork, cudaEventDisableTiming) != cudaSuccess
        || cudaEventCreateWithFlags(&x.join, cudaEventDisableTiming) != cudaSuccess)
      return -1;
  }
  for (int i = 0; i < PASS_INST; i++)
    if (nr_gpu_buf_alloc(&g_jb[i], sizeof(JobK)) != 0)
      return -1;
  /* first launches before the RX fence (module load, kernel images) */
  nr_gpu_demod_job_t j = {};
  j.sym_mask = 0;
  j.ext = 16;
  k_gather<<<dim3(1, NSYM), 32, 0, g_ctx[0].s>>>(K(j), g_ctx[0].y, g_ctx[0].h);
  k_mask_pw<<<NSYM, 256, 0, g_ctx[0].s>>>(K(j), g_ctx[0].h, g_ctx[0].dsh);
  k_align<double><<<dim3(1, NSYM), 128, 0, g_ctx[0].s>>>(K(j), g_ctx[0].y, g_ctx[0].h, g_ctx[0].dsh);
  k_tail<0><<<dim3(1, NSYM), 128, 0, g_ctx[0].s>>>(K(j), g_ctx[0].y, g_ctx[0].h, g_ctx[0].dsh);
  k_tail<1><<<dim3(1, NSYM), 128, 0, g_ctx[0].s>>>(K(j), g_ctx[0].y, g_ctx[0].h, g_ctx[0].dsh);
  k_level<<<1, 256, 0, g_ctx[0].s>>>(K(j), g_ctx[0].h, g_ctx[0].dsh);
  k_evm_zero<<<NSYM, 288, 0, g_ctx[0].s>>>(g_ctx[0].dsh);
  k_evm1<float, 2><<<dim3(1, NSYM), 256, 0, g_ctx[0].s>>>(K(j), g_ctx[0].y, g_ctx[0].h, g_ctx[0].xs, g_ctx[0].dsh, g_ctx[0].part, 0);
  k_evm_fold<<<NSYM, 64, 0, g_ctx[0].s>>>(K(j), g_ctx[0].part, EVM_P1, 5, g_ctx[0].sum);
  k_evm2<float><<<dim3(1, NSYM), 256, 0, g_ctx[0].s>>>(K(j), g_ctx[0].y, g_ctx[0].h, g_ctx[0].xs, g_ctx[0].sd, g_ctx[0].dsh, g_ctx[0].part, g_ctx[0].sum);
  k_evm3<float><<<dim3(1, NSYM), 256, 0, g_ctx[0].s>>>(K(j), g_ctx[0].y, g_ctx[0].h, g_ctx[0].xs, g_ctx[0].sd, g_ctx[0].dsh, g_ctx[0].part, g_ctx[0].sum);
  k_evm3<double><<<dim3(1, NSYM), 256, 0, g_ctx[0].s>>>(K(j), g_ctx[0].y, g_ctx[0].h, g_ctx[0].xs, g_ctx[0].sd, g_ctx[0].dsh, g_ctx[0].part, g_ctx[0].sum);
  k_evm2<double><<<dim3(1, NSYM), 256, 0, g_ctx[0].s>>>(K(j), g_ctx[0].y, g_ctx[0].h, g_ctx[0].xs, g_ctx[0].sd, g_ctx[0].dsh, g_ctx[0].part, g_ctx[0].sum);
  k_evm1<double, 2><<<dim3(1, NSYM), 256, 0, g_ctx[0].s>>>(K(j), g_ctx[0].y, g_ctx[0].h, g_ctx[0].xs, g_ctx[0].dsh, g_ctx[0].part, 0);
  k_evm_out<<<NSYM, 32, 0, g_ctx[0].s>>>(K(j), g_ctx[0].dsh, g_ctx[0].sum);
  if (cudaStreamSynchronize(g_ctx[0].s) != cudaSuccess)
    return -1;
  /* GPUSHARE: load the Nl = 3, 4 kernels now. Under CUDA lazy loading their first launch (first Nl >= 3 grant) loaded
   * the module function inside the decode and blocked that launch for ~44 ms [nsys, replay b]. */
  cudaFuncAttributes fa;
  if (cudaFuncGetAttributes(&fa, k_tail<2>) != cudaSuccess || cudaFuncGetAttributes(&fa, k_tail<3>) != cudaSuccess
      || cudaFuncGetAttributes(&fa, k_tail<4>) != cudaSuccess || cudaFuncGetAttributes(&fa, k_tail<5>) != cudaSuccess
      || cudaFuncGetAttributes(&fa, k_tail<6>) != cudaSuccess || cudaFuncGetAttributes(&fa, k_tail<7>) != cudaSuccess
      || cudaFuncGetAttributes(&fa, k_tail<8>) != cudaSuccess || cudaFuncGetAttributes(&fa, k_mmse3_prep<double>) != cudaSuccess
      || cudaFuncGetAttributes(&fa, k_align<float>) != cudaSuccess || cudaFuncGetAttributes(&fa, k_mmse3_prep<float>) != cudaSuccess
      || cudaFuncGetAttributes(&fa, k_tail<1, float>) != cudaSuccess || cudaFuncGetAttributes(&fa, k_tail<2, float>) != cudaSuccess
      || cudaFuncGetAttributes(&fa, k_tail<3, float>) != cudaSuccess || cudaFuncGetAttributes(&fa, k_tail<6, float>) != cudaSuccess
      || cudaFuncGetAttributes(&fa, k_tail<7, float>) != cudaSuccess || cudaFuncGetAttributes(&fa, k_tail<8, float>) != cudaSuccess
      || cudaFuncGetAttributes(&fa, k_evm1<float, 4, float>) != cudaSuccess || cudaFuncGetAttributes(&fa, k_evm1<float, 2, float>) != cudaSuccess
      || cudaFuncGetAttributes(&fa, k_evm2<float, float>) != cudaSuccess || cudaFuncGetAttributes(&fa, k_evm3<float, float>) != cudaSuccess
      || cudaFuncGetAttributes(&fa, k_evm_fold<float>) != cudaSuccess || cudaFuncGetAttributes(&fa, k_evm_out<float>) != cudaSuccess
      || cudaFuncGetAttributes(&fa, k_evm1<float, 4>) != cudaSuccess || cudaFuncGetAttributes(&fa, k_evm1<double, 4>) != cudaSuccess)
    return -1;
  g_ok = 1;
  return 0;
}

/* Q4: sets up the DISCRETE staging of nr_pdsch_gpu_demod_run (see the Stg block): device + pinned blocks for the largest grant of this carrier, as many contexts as the shared
 * memory budget allows (1 .. CTX). Call after nr_pdsch_gpu_demod_init, before RX starts. 0 = ready (also on a DIRECT platform: nothing to stage), -1 = off: the run refuses and
 * the CPU demodulates. */
extern "C" int nr_pdsch_gpu_demod_stage_init(int nbRx, int N, uint32_t ext_max)
{
  if (!g_ok) return -1;
  if (nr_gpu_model() == NR_GPU_MODEL_DIRECT) return 0;
  if (g_stg_n) return nbRx == g_stg_nbrx && N == g_stg_N ? 0 : -1;
  if (nbRx < 1 || nbRx > NR_GPU_DEMOD_MAX_RX || N < 16 || N > 32768 || ext_max < 16 || ext_max > (uint32_t)EXT) return -1;
  g_stg_N = N;
  g_stg_nbrx = nbRx;
  g_stg_ext = ext_max;
  g_stg_llr_cap = (size_t)NSYM * ext_max * NR_GPU_DEMOD_MAX_NL * 8; /* 14 symbols x 4 layers x 256QAM */
  g_stg_arr = al256((size_t)NSYM * 4 * ext_max * 4);
  const size_t in_b = al256((size_t)nbRx * NSYM * N * 4) + al256((size_t)4 * nbRx * NSYM * N * 4) + 2 * al256((size_t)NSYM * ext_max * 4);
  const size_t out_b = al256(g_stg_llr_cap * 2) + 4 * g_stg_arr;
  const int n = (int)nr_gpu_membudget_fit(nr_gpu_stage_share(), 0, in_b + out_b, 1, CTX);
  int ok = n;
  for (int c = 0; c < n && ok; c++)
    ok = nr_gpu_buf_alloc(&g_stg[c].in, in_b) == 0 && nr_gpu_buf_alloc(&g_stg[c].out, out_b) == 0;
  if (!ok) {
    for (int c = 0; c < CTX; c++) { nr_gpu_buf_free(&g_stg[c].in); nr_gpu_buf_free(&g_stg[c].out); }
    return -1;
  }
  g_stg_n = n;
  fprintf(stderr, "SENSING: GPU_EXACT_DEMOD staged (DISCRETE): %d contexts x (%zu KB device + %zu KB pinned)\n", n, (in_b + out_b) >> 10, (in_b + out_b) >> 10);
  return 0;
}

/* GPUFUSE: compensation + MMSE + kappa + LLRs, one launch (shared by nr_pdsch_gpu_demod_run and nr_pdsch_gpu_demod_dev) */
static void launch_tail(const nr_gpu_demod_job_t &jj, Ctx &x, cudaStream_t s, int gx)
{
  const dim3 g(gx, NSYM);
  const bool f = jj.demod_fp32 != 0; /* Task 5: the float instantiation of the kernels that have one (the others are integer-only) */
  if (jj.qam_mode == 3 && jj.nl >= 2) { /* MMSE3GPU: per-symbol s2 and gain, then the per-RE solve */
    if (f) k_mmse3_prep<float><<<NSYM, 256, 0, s>>>(K(jj), x.y, x.h, x.dsh);
    else k_mmse3_prep<double><<<NSYM, 256, 0, s>>>(K(jj), x.y, x.h, x.dsh);
  }
  if (jj.qam_mode == 3 && jj.nl == 2) {
    if (f) k_tail<6, float><<<g, 128, 0, s>>>(K(jj), x.y, x.h, x.dsh);
    else k_tail<6><<<g, 128, 0, s>>>(K(jj), x.y, x.h, x.dsh);
  } else if (jj.qam_mode == 3 && jj.nl == 3) {
    if (f) k_tail<7, float><<<g, 128, 0, s>>>(K(jj), x.y, x.h, x.dsh);
    else k_tail<7><<<g, 128, 0, s>>>(K(jj), x.y, x.h, x.dsh);
  } else if (jj.qam_mode == 3 && jj.nl == 4) {
    if (f) k_tail<8, float><<<g, 128, 0, s>>>(K(jj), x.y, x.h, x.dsh);
    else k_tail<8><<<g, 128, 0, s>>>(K(jj), x.y, x.h, x.dsh);
  } else if (jj.nl == 2) {
    if (f) k_tail<1, float><<<g, 128, 0, s>>>(K(jj), x.y, x.h, x.dsh);
    else k_tail<1><<<g, 128, 0, s>>>(K(jj), x.y, x.h, x.dsh);
  } else if (jj.nl == 3 && jj.qam_mode == 2) {
    if (f) k_tail<2, float><<<g, 128, 0, s>>>(K(jj), x.y, x.h, x.dsh);
    else k_tail<2><<<g, 128, 0, s>>>(K(jj), x.y, x.h, x.dsh);
  } else if (jj.nl == 4 && jj.qam_mode == 2) {
    if (f) k_tail<3, float><<<g, 128, 0, s>>>(K(jj), x.y, x.h, x.dsh);
    else k_tail<3><<<g, 128, 0, s>>>(K(jj), x.y, x.h, x.dsh);
  } else if (jj.nl == 3)
    k_tail<4><<<g, 128, 0, s>>>(K(jj), x.y, x.h, x.dsh);
  else if (jj.nl == 4)
    k_tail<5><<<g, 128, 0, s>>>(K(jj), x.y, x.h, x.dsh);
  else
    k_tail<0><<<g, 128, 0, s>>>(K(jj), x.y, x.h, x.dsh);
}
static void launch_align(const nr_gpu_demod_job_t &jj, Ctx &x, cudaStream_t s)
{
  const dim3 g((MAXCH + ALC - 1) / ALC, NSYM);
  if (jj.demod_fp32) k_align<float><<<g, 128, 0, s>>>(K(jj), x.y, x.h, x.dsh);
  else k_align<double><<<g, 128, 0, s>>>(K(jj), x.y, x.h, x.dsh);
}

extern "C" void nr_pdsch_gpu_demod_release(int c)
{
  if (c >= 0 && c < CTX)
    g_busy.fetch_and(~(1u << c));
}

extern "C" int nr_pdsch_gpu_demod_evm_wait(nr_gpu_demod_job_t *j)
{
  if (j->ctx < 0 || j->ctx >= CTX)
    return -1;
  Ctx &x = g_ctx[j->ctx];
  cudaError_t e;
  uint64_t pt = 0;
  const uint64_t ss0 = nr_ss_now();
  while ((e = cudaStreamQuery(x.s2)) == cudaErrorNotReady)
    nr_gpu_poll_relax(&pt); /* P99: ISAC_GPU_POLL_SLEEP_US */
  nr_ss_add(NRSS_DEMOD, ss0);
  if (e != cudaSuccess)
    return -1;
  memcpy(j->evm, x.sh->evm, sizeof(j->evm));
  j->evm_pending = 0;
  return 0;
}

extern "C" int nr_pdsch_gpu_demod_run(nr_gpu_demod_job_t *j)
{
  /* The kernels read j->rxdataF/est/geo/comp/mag/llr (caller memory) in place on the DIRECT tier. On DISCRETE (Q4) the run is staged (see the Stg block above): the
   * span the kernels read goes to the device, the outputs the host consumes come back; without staging (not set up, or the grant does not fit) it refuses before
   * any CUDA call and the host runs the CPU. The pass demod (nr_pdsch_gpu_demod_dev) uses nr_gpu_buf_t. */
  const bool staged = nr_gpu_model() != NR_GPU_MODEL_DIRECT;
  if (staged && !g_stg_n)
    return -1;
  if (!g_ok || j->nbRx > NR_GPU_DEMOD_MAX_RX || j->nl > NR_GPU_DEMOD_MAX_NL || j->ext > (uint32_t)EXT
      || j->nl * j->nbRx > 16)
    return -1;
  NrGpuRtScope rt;
  if (!rt.admitted)
    return -1;
  const int c = staged ? acquire(g_stg_n) : acquire();
  Ctx &x = g_ctx[c];
  nr_gpu_demod_job_t jj = *j;
  SPlan P = {};
  uint64_t h2d_b = 0, d2h_b = 0;
  int d2h_n = 0;
  if (staged) {
    if (stage_fill(c, *j, P) != 0) {
      nr_pdsch_gpu_demod_release(c);
      nr_stage::refused(NR_GPU_STG_DEMOD);
      return -1;
    }
    stage_point(c, jj, P);
    cudaMemcpyAsync(g_stg[c].in.dev, g_stg[c].in.host, P.in_n, cudaMemcpyHostToDevice, x.s);
    h2d_b = P.in_n;
  }
  const int gx = (int)((jj.ext + 127) / 128);
  k_gather<<<dim3(gx, NSYM), 128, 0, x.s>>>(K(jj), x.y, x.h);
  k_level<<<jj.nl * jj.nbRx, 256, 0, x.s>>>(K(jj), x.h, x.dsh); /* + the decision (last block) */
  k_mask_pw<<<NSYM, 256, 0, x.s>>>(K(jj), x.h, x.dsh);
  if (jj.nl == 1 && jj.align)
    launch_align(jj, x, x.s);
  if (jj.want_evm) {
    cudaStream_t es = x.s2;
    cudaEventRecord(x.fork, x.s);
    cudaStreamWaitEvent(x.s2, x.fork, 0);
    k_evm_zero<<<NSYM, 288, 0, es>>>(x.dsh);
    if (jj.evm_fp64) {
      if (jj.nl > 2)
        k_evm1<double, 4><<<dim3(EVM_NCH, NSYM), 256, 0, es>>>(K(jj), x.y, x.h, x.xs, x.dsh, x.part, jj.evm_erase);
      else
        k_evm1<double, 2><<<dim3(EVM_NCH, NSYM), 256, 0, es>>>(K(jj), x.y, x.h, x.xs, x.dsh, x.part, jj.evm_erase);
      k_evm_fold<<<NSYM, 64, 0, es>>>(K(jj), x.part, EVM_P1, 5, x.sum);
      k_evm2<double><<<dim3(EVM_NCH, NSYM), 256, 0, es>>>(K(jj), x.y, x.h, x.xs, x.sd, x.dsh, x.part, x.sum);
      k_evm_fold<<<NSYM, 64, 0, es>>>(K(jj), x.part, EVM_P2, EVM_NP2 + 24, x.sum);
      k_evm3<double><<<dim3(EVM_NCH, NSYM), 256, 0, es>>>(K(jj), x.y, x.h, x.xs, x.sd, x.dsh, x.part, x.sum);
    } else {
      if (jj.nl > 2)
        k_evm1<float, 4><<<dim3(EVM_NCH, NSYM), 256, 0, es>>>(K(jj), x.y, x.h, x.xs, x.dsh, x.part, jj.evm_erase);
      else
        k_evm1<float, 2><<<dim3(EVM_NCH, NSYM), 256, 0, es>>>(K(jj), x.y, x.h, x.xs, x.dsh, x.part, jj.evm_erase);
      k_evm_fold<<<NSYM, 64, 0, es>>>(K(jj), x.part, EVM_P1, 5, x.sum);
      k_evm2<float><<<dim3(EVM_NCH, NSYM), 256, 0, es>>>(K(jj), x.y, x.h, x.xs, x.sd, x.dsh, x.part, x.sum);
      k_evm_fold<<<NSYM, 64, 0, es>>>(K(jj), x.part, EVM_P2, EVM_NP2 + 24, x.sum);
      k_evm3<float><<<dim3(EVM_NCH, NSYM), 256, 0, es>>>(K(jj), x.y, x.h, x.xs, x.sd, x.dsh, x.part, x.sum);
    }
    k_evm_fold<<<NSYM, 64, 0, es>>>(K(jj), x.part, EVM_P3, 20, x.sum);
    k_evm_out<<<NSYM, 32, 0, es>>>(K(jj), x.dsh, x.sum);
    cudaEventRecord(x.join, x.s2);
  }
  launch_tail(jj, x, x.s, gx);
  if (jj.want_kappa && !jj.want_evm) { /* CPU EVM fallback reads the extraction buffers */
    cudaMemcpyAsync(x.hy, x.y, sizeof(nrd_c16) * NSYM * NR_GPU_DEMOD_MAX_RX * EXT, cudaMemcpyDeviceToHost, x.s);
    cudaMemcpyAsync(x.hh, x.h, sizeof(nrd_c16) * NSYM * 16 * EXT, cudaMemcpyDeviceToHost, x.s);
    d2h_b += sizeof(nrd_c16) * NSYM * (NR_GPU_DEMOD_MAX_RX + 16) * EXT;
    d2h_n += 2;
  }
  cudaError_t se = staged ? stage_d2h(c, *j, P, x.s, &d2h_b, &d2h_n) : cudaSuccess;
  const bool defer = jj.want_evm && jj.evm_defer;
  if (defer) { /* the LLRs need only the main stream; the EVM sums follow on s2 (nr_pdsch_gpu_demod_evm_wait) */
    cudaMemcpyAsync(x.sh, x.dsh, offsetof(Shared, evm), cudaMemcpyDeviceToHost, x.s);
    cudaMemcpyAsync(x.sh->evm, x.dsh->evm, sizeof(x.sh->evm), cudaMemcpyDeviceToHost, x.s2);
    d2h_b += sizeof(Shared);
    d2h_n += 2;
  } else {
    if (jj.want_evm)
      cudaStreamWaitEvent(x.s, x.join, 0);
    cudaMemcpyAsync(x.sh, x.dsh, sizeof(Shared), cudaMemcpyDeviceToHost, x.s);
    d2h_b += sizeof(Shared);
    ++d2h_n;
  }
  cudaError_t e = se != cudaSuccess ? se : cudaPeekAtLastError();
  uint64_t pt = 0;
  const uint64_t ss0 = nr_ss_now();
  while (e == cudaSuccess) {
    e = cudaStreamQuery(x.s);
    if (e == cudaErrorNotReady) {
      e = cudaSuccess;
      nr_gpu_poll_relax(&pt); /* P99: ISAC_GPU_POLL_SLEEP_US */
    } else
      break;
  }
  nr_ss_add(NRSS_DEMOD, ss0);
  if (e != cudaSuccess) {
    fprintf(stderr, "nr_pdsch_gpu_demod_run: %s\n", cudaGetErrorString(e));
    g_ok = 0;
    nr_pdsch_gpu_demod_release(c);
    return -1;
  }
  if (staged) {
    stage_scatter(c, j, P);
    nr_stage::note(NR_GPU_STG_DEMOD, h2d_b, 1, d2h_b, d2h_n);
  }
  const nrd_decision d = x.sh->d;
  j->log2_maxh = d.log2_maxh;
  j->used_branch_set = d.used_branch_set;
  j->used_branch = d.used_branch;
  j->mrc_rx_index = d.mrc_rx_index;
  j->mrc_nb_rx = d.mrc_nb_rx;
  memcpy(j->kappa_sz, x.sh->kap_sz, sizeof(j->kappa_sz));
  memcpy(j->kappa_sm, x.sh->kap_sm, sizeof(j->kappa_sm));
  if (jj.want_evm && !defer)
    memcpy(j->evm, x.sh->evm, sizeof(j->evm));
  j->evm_pending = defer;
  j->y_ext = (const int16_t *)x.hy;
  j->h_ext = (const int16_t *)x.hh;
  j->ctx = c;
  return 0;
}

/* GPUPASS: launch-only demod of one grant (gather, level, mask, align, MMSE, LLRs) on `stream`: no wait, no D2H of decisions/kappa,
 * no EVM (want_evm -> -1). rxdataF/est/geo_y/geo_h/comp/mag* of the job must be device-accessible; the LLRs go to llr_dev
 * (int16, the job's llr_off layout). Uses the module's private GPUPASS context: one pass in flight. 0 or -1. */
extern "C" int nr_pdsch_gpu_demod_dev(const nr_gpu_demod_job_t *j, nr_gpu_buf_t *llr_dev, void *stream)
{
  if (!g_ok || !j || !llr_dev || !llr_dev->dev || j->want_evm || j->nbRx > NR_GPU_DEMOD_MAX_RX || j->nl > NR_GPU_DEMOD_MAX_NL
      || j->ext > (uint32_t)EXT || j->nl * j->nbRx > 16)
    return -1;
  Ctx &x = g_ctx[CTX + PASS_INST];
  const cudaStream_t s = (cudaStream_t)stream;
  nr_gpu_demod_job_t jj = *j;
  jj.llr = (int16_t *)llr_dev->dev;
  jj.m3_nvfac_f = (float)jj.m3_nvfac;
  const int gx = (int)((jj.ext + 127) / 128);
  k_gather<<<dim3(gx, NSYM), 128, 0, s>>>(K(jj), x.y, x.h);
  k_level<<<jj.nl * jj.nbRx, 256, 0, s>>>(K(jj), x.h, x.dsh);
  k_mask_pw<<<NSYM, 256, 0, s>>>(K(jj), x.h, x.dsh);
  if (jj.nl == 1 && jj.align)
    launch_align(jj, x, s);
  launch_tail(jj, x, s, gx);
  return cudaPeekAtLastError() == cudaSuccess ? 0 : -1;
}

/* ---------------------------------------------------------------- GPUPASS graph instances ---------------------------------------------------------------- */
static bool pass_ok(int inst, const nr_gpu_demod_job_t *j)
{
  return g_ok && inst >= 0 && inst < PASS_INST && j && g_jb[inst].dev && j->nbRx <= NR_GPU_DEMOD_MAX_RX && j->nl <= NR_GPU_DEMOD_MAX_NL
         && j->ext <= (uint32_t)EXT && j->nl * j->nbRx <= 16;
}
/* host: writes the job of the next replay into the instance's job block (the kernels read it through jref when they run) */
extern "C" int nr_pdsch_gpu_demod_pass_set(int inst, const nr_gpu_demod_job_t *j)
{
  if (!pass_ok(inst, j)) return -1;
  memcpy(g_jb[inst].host, j, sizeof(JobK));
  ((nr_gpu_demod_job_t *)g_jb[inst].host)->m3_nvfac_f = (float)j->m3_nvfac;
  ((nr_gpu_demod_job_t *)g_jb[inst].host)->jref = (const nr_gpu_demod_job_t *)g_jb[inst].dev;
  return 0;
}
/* launch-only demod of instance `inst` (see nr_pdsch_gpu_demod.h): the kernels read the job block; `jcap` fixes what the graph structure depends on
 * (nl, nbRx, ext as the grid bound, qam_mode, align, want_evm, evm_fp64). stream2 is the EVM branch (forked from and joined to `stream`). */
extern "C" int nr_pdsch_gpu_demod_pass_dev(int inst, const nr_gpu_demod_job_t *jcap, nr_gpu_buf_t *llr_dev, const uint32_t *nvar_dev, void *stream, void *stream2)
{
  if (!pass_ok(inst, jcap) || !llr_dev || !llr_dev->dev || !nvar_dev || !stream || (jcap->want_evm && !stream2)) return -1;
  Ctx &x = g_ctx[CTX + inst];
  const cudaStream_t s = (cudaStream_t)stream, es = (cudaStream_t)stream2;
  nr_gpu_demod_job_t jj = *jcap;
  jj.llr = (int16_t *)llr_dev->dev;
  jj.nvar_dev = nvar_dev;
  jj.m3_nvfac_f = (float)jj.m3_nvfac;
  jj.jref = (const nr_gpu_demod_job_t *)g_jb[inst].dev;
  if (nr_gpu_buf_h2d_async(&g_jb[inst], 0, sizeof(JobK), s) != 0) return -1;
  const int gx = (int)((jj.ext + 127) / 128);
  k_gather<<<dim3(gx, NSYM), 128, 0, s>>>(K(jj), x.y, x.h);
  k_level<<<jj.nl * jj.nbRx, 256, 0, s>>>(K(jj), x.h, x.dsh);
  k_mask_pw<<<NSYM, 256, 0, s>>>(K(jj), x.h, x.dsh);
  if (jj.nl == 1 && jj.align)
    launch_align(jj, x, s);
  if (jj.want_evm) { /* the EVM chain on its own branch, as in nr_pdsch_gpu_demod_run */
    cudaEventRecord(x.fork, s);
    cudaStreamWaitEvent(es, x.fork, 0);
    k_evm_zero<<<NSYM, 288, 0, es>>>(x.dsh);
    if (jj.demod_fp32) { /* FP32 class: the whole EVM chain in float (storage and sums), no FP64 instruction */
      float2 *xs = (float2 *)x.xs, *sd = (float2 *)x.sd;
      float *part = (float *)x.part, *sum = (float *)x.sum;
      const dim3 eg(EVM_NCH, NSYM);
      if (jj.nl > 2)
        k_evm1<float, 4, float><<<eg, 256, 0, es>>>(K(jj), x.y, x.h, xs, x.dsh, part, jj.evm_erase);
      else
        k_evm1<float, 2, float><<<eg, 256, 0, es>>>(K(jj), x.y, x.h, xs, x.dsh, part, jj.evm_erase);
      k_evm_fold<float><<<NSYM, 64, 0, es>>>(K(jj), part, EVM_P1, 5, sum);
      k_evm2<float, float><<<eg, 256, 0, es>>>(K(jj), x.y, x.h, xs, sd, x.dsh, part, sum);
      k_evm_fold<float><<<NSYM, 64, 0, es>>>(K(jj), part, EVM_P2, EVM_NP2 + 24, sum);
      k_evm3<float, float><<<eg, 256, 0, es>>>(K(jj), x.y, x.h, xs, sd, x.dsh, part, sum);
      k_evm_fold<float><<<NSYM, 64, 0, es>>>(K(jj), part, EVM_P3, 20, sum);
      k_evm_out<float><<<NSYM, 32, 0, es>>>(K(jj), x.dsh, sum);
    } else if (jj.evm_fp64) {
      if (jj.nl > 2)
        k_evm1<double, 4><<<dim3(EVM_NCH, NSYM), 256, 0, es>>>(K(jj), x.y, x.h, x.xs, x.dsh, x.part, jj.evm_erase);
      else
        k_evm1<double, 2><<<dim3(EVM_NCH, NSYM), 256, 0, es>>>(K(jj), x.y, x.h, x.xs, x.dsh, x.part, jj.evm_erase);
      k_evm_fold<<<NSYM, 64, 0, es>>>(K(jj), x.part, EVM_P1, 5, x.sum);
      k_evm2<double><<<dim3(EVM_NCH, NSYM), 256, 0, es>>>(K(jj), x.y, x.h, x.xs, x.sd, x.dsh, x.part, x.sum);
      k_evm_fold<<<NSYM, 64, 0, es>>>(K(jj), x.part, EVM_P2, EVM_NP2 + 24, x.sum);
      k_evm3<double><<<dim3(EVM_NCH, NSYM), 256, 0, es>>>(K(jj), x.y, x.h, x.xs, x.sd, x.dsh, x.part, x.sum);
    } else {
      if (jj.nl > 2)
        k_evm1<float, 4><<<dim3(EVM_NCH, NSYM), 256, 0, es>>>(K(jj), x.y, x.h, x.xs, x.dsh, x.part, jj.evm_erase);
      else
        k_evm1<float, 2><<<dim3(EVM_NCH, NSYM), 256, 0, es>>>(K(jj), x.y, x.h, x.xs, x.dsh, x.part, jj.evm_erase);
      k_evm_fold<<<NSYM, 64, 0, es>>>(K(jj), x.part, EVM_P1, 5, x.sum);
      k_evm2<float><<<dim3(EVM_NCH, NSYM), 256, 0, es>>>(K(jj), x.y, x.h, x.xs, x.sd, x.dsh, x.part, x.sum);
      k_evm_fold<<<NSYM, 64, 0, es>>>(K(jj), x.part, EVM_P2, EVM_NP2 + 24, x.sum);
      k_evm3<float><<<dim3(EVM_NCH, NSYM), 256, 0, es>>>(K(jj), x.y, x.h, x.xs, x.sd, x.dsh, x.part, x.sum);
    }
    if (!jj.demod_fp32) {
      k_evm_fold<<<NSYM, 64, 0, es>>>(K(jj), x.part, EVM_P3, 20, x.sum);
      k_evm_out<<<NSYM, 32, 0, es>>>(K(jj), x.dsh, x.sum);
    }
    cudaEventRecord(x.join, es);
  }
  launch_tail(jj, x, s, gx);
  if (jj.want_evm)
    cudaStreamWaitEvent(s, x.join, 0);
  cudaMemcpyAsync(x.sh, x.dsh, sizeof(Shared), cudaMemcpyDeviceToHost, s);
  return cudaPeekAtLastError() == cudaSuccess ? 0 : -1;
}
/* host, after the pass: the decisions, kappa sums and EVM sums the pass left in the instance's pinned block */
extern "C" void nr_pdsch_gpu_demod_pass_out(int inst, nr_gpu_demod_job_t *j)
{
  if (!g_ok || inst < 0 || inst >= PASS_INST || !j) return;
  Ctx &x = g_ctx[CTX + inst];
  const nrd_decision d = x.sh->d;
  j->log2_maxh = d.log2_maxh;
  j->used_branch_set = d.used_branch_set;
  j->used_branch = d.used_branch;
  j->mrc_rx_index = d.mrc_rx_index;
  j->mrc_nb_rx = d.mrc_nb_rx;
  memcpy(j->kappa_sz, x.sh->kap_sz, sizeof(j->kappa_sz));
  memcpy(j->kappa_sm, x.sh->kap_sm, sizeof(j->kappa_sm));
  if (j->want_evm)
    memcpy(j->evm, x.sh->evm, sizeof(j->evm));
  j->evm_pending = 0;
}
