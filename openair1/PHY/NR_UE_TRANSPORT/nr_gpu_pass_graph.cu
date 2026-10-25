/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* GPUPASS Task 4 (part of libpdsch_gpu.so): the shape-class graph cache of the DL pass. See nr_gpu_pass.h for the contract, the graph and the concurrency model.
 *
 * Design in one paragraph. A graph is captured per (class key, instance) from a synthetic job of the class, with cudaStreamCaptureModeRelaxed on the instance's
 * stream, and instantiated before the RX fence. Everything that varies between two grants of one class is data in memory the kernels read when they run:
 * the staged FEP block (rx pointers, ring, origin, rotation, output pointer), the staged chest rows, the demod job block (jref), the PassBlk (chest-post
 * parameters, LLR post parameters, rate-dematching and CRC tables), the LDPC DJob array. The host rewrites those blocks and replays the graph; the by-value
 * arguments of the captured kernels never change. The structure that IS baked into a graph is the class key: nbRx, nl, number of DM-RS symbols, padded code block
 * count (the grid sizes), the demod variant (tail template from qam_mode and nl, align, EVM chain), the EVM math (float / double) and the grid bound of the
 * PRB class. A grant outside the cached keys, with no free instance, or outside what the graph reproduces, returns 1 and is counted. */
#include "nr_gpu_pass.h"
#include "nr_pdsch_gpu_fep.h"
#include "nr_pdsch_gpu_chest.h"
#include "nr_pdsch_gpu_demod.h"
#include "nr_pdsch_gpu_ldpc.h"
#include "nr_gpu_pass_post.h"
#include "nr_gpu_pass_cap.h"
#include "PHY/TOOLS/nr_gpu_membudget.h"
#include "PHY/TOOLS/nr_gpu_platform.h"
#include "PHY/TOOLS/nr_gpu_memstat_cuda.cuh"
#include "nr_gpu_cell.h"
#include "PHY/TOOLS/nr_gpu_compat.h"
#include "PHY/NR_UE_ESTIMATION/filt16a_32.h"
#include <cuda_runtime.h>
#include <nvtx3/nvToolsExt.h> /* header-only; a no-op unless a tool (nsys -t nvtx) is attached */
#include "nr_gpu_green.h"
#include "nr_pdsch_gpu_prio.cuh"
#include "PHY/TOOLS/nr_gpu_budget_cuda.h"
#include "nr_pusch_gpu_fep.h" /* nr_pusch_gpu_fep_item_t: the layout of ulfep::Item (the PUSCH pass feeds k_ul_fep_p) */
#define NR_GPU_PASS_UL_EQ
#include "nr_gpu_pass_ul.cuh"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <map>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <sys/prctl.h>
#include <time.h>
#include <thread>

/* entry points of the other translation units of libpdsch_gpu.so */
extern "C" {
int nr_pdsch_gpu_fep_stage_at(const nr_gpu_fep_job_t *j, int slot);
int nr_pdsch_gpu_fep_dev(const nr_gpu_fep_job_t *j, nr_gpu_buf_t *rxF, int slot, void *stream);
int nr_pdsch_gpu_chest_stage_at(const nr_gpu_chest_job_t *j, int n, int slot);
int nr_pdsch_gpu_pass_ls_rows(void);
int nr_pdsch_gpu_chest_dev(const nr_gpu_chest_job_t *j, int n, nr_gpu_buf_t *est_dev, nr_gpu_buf_t *nvar_dev, int slot, void *stream);
int nr_pdsch_gpu_chest_dev_at(const nr_gpu_chest_job_t *j, int r0, int n, nr_gpu_buf_t *est_dev, nr_gpu_buf_t *nvar_dev, int slot, void *stream);
int nr_pdsch_gpu_demod_pass_set(int inst, const nr_gpu_demod_job_t *j);
int nr_pdsch_gpu_demod_pass_dev(int inst, const nr_gpu_demod_job_t *jcap, nr_gpu_buf_t *llr_dev, const uint32_t *nvar_dev, void *stream, void *stream2);
void nr_pdsch_gpu_demod_pass_out(int inst, nr_gpu_demod_job_t *j);
int nr_pdsch_gpu_ldpc_pass_set(int inst, const nr_gpu_ldpc_job_t *jobs, int n, const nr_gpu_buf_t *llr_dev);
int nr_pdsch_gpu_ldpc_pass_dev(int inst, const nr_gpu_ldpc_job_t *jobs, int n, const nr_gpu_buf_t *llr_dev, nr_gpu_buf_t *bits_dev, void *stream);
int nr_gpu_pass_ul_fep_dev(const void *items_dev, const int *par_dev, int nant, void *stream);
}

namespace nrpass {
constexpr int NI = NR_GPU_PASS_RING;
/* Instance pools (Fix A+D). Real DL grants, real UL grants and probes (DL layout probes, UL probes) each own a disjoint set of instances, and with it the instance's streams, graph
 * executables, FEP staging slot, demod context and LDPC context. A submission therefore never waits for an instance or stream that another pool holds:
 *   - a real grant that finds its pool full waits on a condition variable for a release of ITS OWN pool (bounded, then the old path, counted as busy);
 *   - a probe is admitted only while no real grant of either pool waits (g_real_waiting) and only on probe-pool instances, so it can never be the reason a real grant waits;
 *   - DL streams run at the highest priority and UL / probe streams at the lowest, all in the RT green context (discovery keeps its own SM set): when the RT SMs are contended the block
 *     scheduler serves the DL blocks first. UL real work does not register as an RT user of the discovery budget (it never blocks a CSI-RS slice). */
constexpr unsigned M_DL = ((1u << NR_GPU_PASS_DL) - 1u), M_UL = ((1u << NR_GPU_PASS_UL) - 1u) << NR_GPU_PASS_DL,
                   M_PROBE = ((1u << NR_GPU_PASS_PROBE) - 1u) << (NR_GPU_PASS_DL + NR_GPU_PASS_UL);
constexpr int MAXCB = NR_GPU_PASS_MAX_CB;
static_assert(NR_GPU_PASS_MAX_CB == NR_GPU_PASS_LDPC_JOBS, "one LDPC job per code block");
constexpr int NSYM = 14;
constexpr int N = 4096;
constexpr int NSNAP = NR_GPU_LDPC_MAX_SNAP;
constexpr int KBMAX = NR_GPU_LDPC_KB_MAX;
constexpr int EXTMAX = NR_GPU_DEMOD_MAX_EXT;
constexpr uint32_t LLR_CAP = 1500000;      /* LLRs of one pass: 273 PRB x 13 symbols x 12 x Qm 8 x 4 layers = 1362816 */
constexpr int PIL_STRIDE = 3328;           /* c16 per (DM-RS symbol, layer) pilot row */
constexpr int ULW_LEN = 66368;             /* PUSCH pass, DISCRETE: c16 of IQ copied per antenna (14 symbols + one FFT window: 14 * (4096 + 352) + 4096) */
static int SEG_ROWS = 512;                 /* chest rows of one pass: nd * nl * nrx * nseg_class (<= PASS_LS_ROWS of nr_pdsch_gpu_fep.cu); 1024 in split mode (32 segments) */
static std::atomic<int> g_sized{1};       /* grant-sized work (FEP symbol set, LDPC padding blocks stop at entry): always 1 in the receiver; nr_gpu_pass_test_sized() switches it for the equality tests and the bench only */

/* ---- device-accessible blocks ---- */
struct PassBlk { /* written by the host before every replay */
  nr_gpu_chest_post_par_t post;
  uint32_t G, span, tbs;
  int norm, ncb, KB; /* ncb: the code blocks decoded (k_pick) */
  int nl, qm, l2t, l2dk;   /* RANK2-WEAK: layers and modulation order of the DL grant (the 2-layer LLR normalisation, nr_llr_norm_shift_nl); 0 = the plain rule */
  const uint32_t *scr;
  nr_gpu_rdm_seg_t rdm[MAXCB];
  nr_gpu_crc_seg_t cand[MAXCB * NSNAP]; /* every snapshot of every code block */
  nr_gpu_crc_seg_t sel[MAXCB];          /* the chosen block of every code block */
  uint16_t seg_n[NR_GPU_PASS_MAX_SEG], seg_d[NR_GPU_PASS_MAX_SEG]; /* segmented estimate: REs (12 * PRBs) and first data RE of every segment (n == 0: padding) */
  int dsym[NR_GPU_PASS_MAX_DMRS];       /* DM-RS symbols (the compose kernel's destination rows) */
  uint32_t Gd;                          /* v2 Task 2: LLRs the demod wrote; k_unscr zeroes [Gd, G) (= G outside a probe horizon) */
  /* Fix A+D: the no-chance gate (nr_snr_gate.h) evaluated on the device right after the chest post: the DM-RS comb SNR of the last DM-RS symbol and layer, from the smoothed estimate
   * rows and the chest's per-antenna residual noise (nr_pdsch_dmrs_snr.h's nr_dmrs_snr_ant, single precision). gate_on = 0: never skip. */
  int gate_on, gate_nbrx, gate_n, gate_type, gate_seg;
  float gate_thr_db;
  const short2 *gate_est;               /* row of antenna 0; antenna a at + a * gate_astride */
  size_t gate_astride;
  const uint32_t *gate_nvar;            /* per antenna residual noise (non-segmented); segmented: ResBlk.post.nvar_ant */
  /* DISCRETE: the rows of the estimate and of the equaliser output the host reads (k_gather): ge_row[r] = element offset of antenna 0's row (r = 0 / 1: layer 0 at the first / last DM-RS symbol of the
   * frequency-offset measurement, the branch FO rows; r = 2: layer nl - 1 at the last DM-RS symbol, the DM-RS SNR rows), -1 = none; qm_off / qm_n: the equalised symbol row of scratch (comp), qm_n = 0 none */
  int ge_row[3];
  int qm_off, qm_n;
};
struct ResBlk { /* written by the kernels, read by the host after the pass */
  nr_gpu_chest_post_out_t post;
  uint32_t shift;
  int need_cpu, err, all_ok;
  uint8_t tb_crc, dummy_tb;
  uint8_t seg_ok[MAXCB];
  uint8_t seg_crc[MAXCB];
  uint8_t it[MAXCB]; /* RANK2-ITER: OAI numIter of every code block (hit snapshot + 1; 255 = the block that hit the cap, the host reports it_max; 0 = aborted / not decoded) */
  uint8_t cand_ok[MAXCB * NSNAP];
  uint8_t c[MAXCB][KBMAX];
  nrul::UlOut ul; /* Task 7: after c[] so that offsetof(ResBlk, c) is unchanged */
  nr_gpu_llr_stats_t llr_stats; /* v2 Task 1: k_llr_stats */
  uint32_t skip;                /* Fix A+D: the in-graph gate said no-chance: the demod, LDPC and CRC nodes did not run */
  float gate_db;                /* the DM-RS comb SNR it compared (-99 = no estimate) */
  unsigned long long tg[4];     /* measurement only: %globaltimer at k_gate_d entry / IF decided, k_gate_l entry / IF decided */
};
__device__ __forceinline__ unsigned long long pass_gtimer()
{
  unsigned long long t;
  asm volatile("mov.u64 %0, %%globaltimer;" : "=l"(t));
  return t;
}

/* ---- Fix A+D: the no-chance gate kernels. The condition handle of the IF node that follows decides whether the demod / LDPC / CRC sub-graph runs: no host round trip. ---- */
__global__ void k_gate_d(const PassBlk *pb, ResBlk *rb, cudaGraphConditionalHandle h)
{
  __shared__ unsigned long long red[256];
  __shared__ float lin;
  const int t = threadIdx.x;
  if (t == 0) { lin = 0.f; rb->tg[0] = pass_gtimer(); }
  for (int a = 0; a < pb->gate_nbrx; a++) {
    unsigned long long e = 0;
    const short2 *row = pb->gate_est + (size_t)a * pb->gate_astride;
    for (int i = t; i < pb->gate_n; i += blockDim.x) {
      const short2 v = row[i];
      e += (unsigned long long)(uint32_t)((int)v.x * v.x + (int)v.y * v.y);
    }
    red[t] = e;
    __syncthreads();
    for (int q = blockDim.x / 2; q > 0; q >>= 1) {
      if (t < q) red[t] += red[t + q];
      __syncthreads();
    }
    if (t == 0) {
      const uint32_t nres = pb->gate_seg ? rb->post.nvar_ant[a] : pb->gate_nvar[a];
      if (nres) {
        const float c = pb->gate_type == 2 ? 0.59f : 0.72f, g = pb->gate_type == 2 ? 0.33f : 0.22f;
        const float nls = (float)nres / c;
        float sg = (float)red[0] / (float)pb->gate_n - g * nls;
        if (sg < 0.02f * nls) sg = 0.02f * nls;
        lin += sg / (2.f * nls);
      }
    }
    __syncthreads();
  }
  if (t == 0) {
    const float db = lin > 0.f ? 10.f * log10f(lin) : -99.f;
    const uint32_t skip = pb->gate_on && db > -90.f && db < pb->gate_thr_db;
    rb->skip = skip;
    rb->gate_db = db;
    rb->tg[1] = pass_gtimer();
    cudaGraphSetConditional(h, skip ? 0u : 1u);
  }
}
__global__ void k_gate_l(ResBlk *rb, cudaGraphConditionalHandle h)
{
  if (threadIdx.x == 0) {
    rb->tg[2] = pass_gtimer();
    rb->tg[3] = pass_gtimer();
    cudaGraphSetConditional(h, rb->skip ? 0u : 1u);
  }
}
/* UL: nr_ulsch_dmrs_snr_db(sig2, nvar) of the grant on the device (hsum of k_ul_level, the chest's noise sums) */
__global__ void k_gate_ul(const nrul::UlBlk *ub, ResBlk *rb, const uint64_t *acc, cudaGraphConditionalHandle h)
{
  if (threadIdx.x != 0) return;
  const int nrx = ub->nbRx, nd = ub->nd, nre = ub->nre12;
  float hacc = 0.f;
  uint32_t nvar = 0;
  for (int d = 0; d < nd; d++) {
    unsigned long long noise = 0;
    for (int a = 0; a < nrx; a++) {
      noise += acc[d * nrx + a];
      hacc += (float)rb->ul.hsum[d * nrx + a];
    }
    nvar += (uint32_t)(noise / (unsigned long long)(nrx * nre));
  }
  nvar /= (uint32_t)nd;
  const float sig2 = hacc / ((float)nre * nrx * nd), resid2 = (float)nvar;
  float db = -99.f;
  if (sig2 > 0.f && resid2 > 0.f) {
    const float sigma2 = resid2 / 0.875f, h2 = sig2 - 0.125f * sigma2;
    db = 10.f * log10f((h2 > 1e-3f * sigma2 ? h2 : 1e-3f * sigma2) / sigma2);
  }
  const uint32_t skip = ub->gate_on && db > -90.f && db < ub->gate_thr_db;
  rb->skip = skip;
  rb->gate_db = db;
  cudaGraphSetConditional(h, skip ? 0u : 1u);
}

/* ---- kernels of the LLR post and the stop rule ---- */
__global__ void k_unscr(int16_t *llr, const PassBlk *pb)
{
  const uint32_t G = pb->G;
  const uint32_t *scr = pb->scr, Gd = pb->Gd;
  for (uint32_t i = blockIdx.x * blockDim.x + threadIdx.x; i < G; i += gridDim.x * blockDim.x)
    if (i >= Gd) llr[i] = 0; /* past a probe's horizon: the body's buffer is cleared (an earlier pass of the instance left LLRs here) */
    else if (scr[i >> 5] >> (i & 31) & 1u) llr[i] = (int16_t)(-llr[i]);
}
/* v2 Task 1: the LLR statistics of passive_decode_body_core (nr_pdsch_passive_decode.c, demod_ok block) on the descrambled LLRs, before the K38 shift. The host
 * takes the magnitude stats before its descrambling and the sign stats after it; descrambling only flips signs and |-v| == |v| for every int16 (-32768 included,
 * it maps to itself), so one pass after k_unscr gives both. One block; integer sums are order-independent, so the result is bit-equal. */
static_assert(offsetof(nr_gpu_llr_stats_t, llr_clip8) == 4 * sizeof(uint64_t) && offsetof(nr_gpu_llr_stats_t, llr_pos) == 5 * sizeof(uint64_t) &&
              offsetof(nr_gpu_llr_stats_t, nbit) == 9 * sizeof(uint64_t) && sizeof(nr_gpu_llr_stats_t) == 11 * sizeof(uint64_t), "k_llr_stats writes the fields in order");
__global__ void k_llr_stats(const int16_t *llr, const PassBlk *pb, ResBlk *rb)
{
  constexpr int K = 10; /* n, absum, zero, sat, clip8, pos, sgnsum, posbit0, posbit1, n4 (nbit[0] = nbit[1] = 2 * n4) */
  __shared__ unsigned long long sh[K][32];
  unsigned long long v[K] = {};
  const uint32_t G = pb->G;
  for (uint32_t i = threadIdx.x * 32u; i < G; i += blockDim.x * 32u) {
    const int x = llr[i], a = x < 0 ? -x : x;
    v[0]++; v[1] += (unsigned)a; v[2] += a == 0; v[3] += a >= 32767; v[4] += a > 127;
    if (i + 3 < G) {
      v[5] += x > 0; v[6] += (unsigned long long)(long long)x; /* two's complement: the wrapping sum is the signed sum */
      v[7] += (llr[i + 1] > 0) + (x > 0); v[8] += (llr[i + 2] > 0) + (llr[i + 3] > 0); v[9]++;
    }
  }
  for (int k = 0; k < K; k++)
    for (int o = 16; o; o >>= 1) v[k] += __shfl_down_sync(0xffffffffu, v[k], o);
  if (!(threadIdx.x & 31))
    for (int k = 0; k < K; k++) sh[k][threadIdx.x >> 5] = v[k];
  __syncthreads();
  if (threadIdx.x < K) {
    unsigned long long t = 0;
    for (unsigned w = 0; w < blockDim.x / 32; w++) t += sh[threadIdx.x][w];
    unsigned long long *o = (unsigned long long *)&rb->llr_stats;
    if (threadIdx.x < 9) o[threadIdx.x] = t;
    else o[9] = o[10] = 2 * t;
  }
}
/* Segmented estimate (Task 4b): the passive loop's `memset(row, 0); memcpy(&row[data_index * 12], segment estimate, n_prb * 12)` for every (DM-RS symbol, layer, antenna) row.
 * Block b = (k * nl + l) * nrx + a; the segment rows sit in `sc` at ((k * nl + l) * ns + s) * nrx + a (the chest row order), the data-ordered row in est[l * nrx + a]. */
__global__ void k_seg_compose(const short2 *sc, short2 *est, const PassBlk *pb, int ns, int nl, int nrx, int kk0 = 0) /* kk0: the first (DM-RS symbol, layer) group of a C chunk (SEG128) */
{
  const int a = blockIdx.x % nrx, kk = kk0 + blockIdx.x / nrx, l = kk % nl, k = kk / nl;
  short2 *dst = est + (size_t)(l * nrx + a) * NSYM * N + (size_t)pb->dsym[k] * N;
  for (int i = threadIdx.x; i < N; i += blockDim.x) {
    short2 v = make_short2(0, 0);
    for (int s = 0; s < ns; s++) {
      const int d = pb->seg_d[s];
      if (i >= d && i < d + (int)pb->seg_n[s]) {
        v = sc[((size_t)((kk - kk0) * ns + s) * nrx + a) * N + (i - d)];
        break;
      }
    }
    dst[i] = v;
  }
}
/* nr_llr_norm_shift_nl (nr_llr_norm.h, keep identical): nl == 2 and Qm 2/4/6/8: every 16th RE block (2 Qm LLRs, Qm per layer) of llr[0, span), the per-layer
 * means, k from the smaller; else nr_llr_norm_shift: the mean |LLR| of every 16th of llr[0, span), smallest k <= 8 with (mean >> k) <= 40 */
__global__ void k_norm_sum(const int16_t *llr, const PassBlk *pb, ResBlk *rb)
{
  __shared__ unsigned long long red[1024 / 32][2];
  unsigned long long acc = 0, acc1 = 0;
  const uint32_t span = pb->span;
  const bool l2 = pb->nl == 2 && pb->qm >= 4 && pb->qm <= 8 && !(pb->qm & 1);
  const uint32_t qm = (uint32_t)pb->qm, blk = 2u * qm;
  uint32_t nb = 0;
  if (l2) {
    nb = span >= blk ? (span / blk - 1u) / 16u + 1u : 0u; /* blocks b = 16 m with (b + 1) * blk <= span */
    if (pb->norm)
      for (uint32_t m = threadIdx.x; m < nb; m += blockDim.x) {
        const int16_t *p = llr + (size_t)m * 16u * blk;
        for (uint32_t q = 0; q < qm; q++) {
          acc += (uint32_t)abs((int)p[q]);
          acc1 += (uint32_t)abs((int)p[qm + q]);
        }
      }
  } else if (pb->norm)
    for (uint32_t i = threadIdx.x * 16u; i < span; i += blockDim.x * 16u) acc += (uint32_t)abs((int)llr[i]);
  for (int o = 16; o; o >>= 1) {
    acc += __shfl_down_sync(0xffffffffu, acc, o);
    acc1 += __shfl_down_sync(0xffffffffu, acc1, o);
  }
  if ((threadIdx.x & 31) == 0) { red[threadIdx.x >> 5][0] = acc; red[threadIdx.x >> 5][1] = acc1; }
  __syncthreads();
  if (threadIdx.x == 0) {
    unsigned long long a = 0, a1 = 0;
    for (unsigned w = 0; w < blockDim.x / 32; w++) { a += red[w][0]; a1 += red[w][1]; }
    uint32_t k = 0;
    if (pb->norm && l2 && nb) {
      const unsigned long long wk = a < a1 ? a : a1, st = a < a1 ? a1 : a;
      const uint32_t mean = (uint32_t)((wk + st) / (2ull * nb * qm));
      while (k < 8 && (mean >> k) > (uint32_t)pb->l2t) k++;
      int e = 0;
      while (e < pb->l2dk && (wk << (e + 1)) <= st) e++;
      k = (int)k > e ? k - (uint32_t)e : 0u;
    } else if (pb->norm && (span + 15) / 16) {
      const uint32_t cnt = (span + 15) / 16;
      const uint32_t mean = (uint32_t)(a / cnt);
      while (k < 8 && (mean >> k) > 40u) k++;
    }
    rb->shift = k;
  }
}
__global__ void k_norm_apply(int16_t *llr, const PassBlk *pb, const ResBlk *rb)
{
  const uint32_t k = rb->shift, G = pb->G;
  if (!k) return;
  for (uint32_t i = blockIdx.x * blockDim.x + threadIdx.x; i < G; i += gridDim.x * blockDim.x) llr[i] = (int16_t)(llr[i] >> k);
}
/* OAI's stop rule per code block, in order (gpu_tb of nr_pdsch_gpu_ldpc_host.c): the first snapshot whose CRC passes is the decoded block; none = failure, c zeroed,
 * every later block fails (abort); a failing block the GPU stopped on at a zero syndrome (and not already aborted) needs the CPU decoder. */
__global__ void k_pick(const uint8_t *snaps, const int *res, const PassBlk *pb, ResBlk *rb)
{
  __shared__ int aborted, need_cpu, err, hit, all_ok;
  const int C = pb->ncb, KB = pb->KB;
  if (threadIdx.x == 0) { aborted = need_cpu = err = 0; all_ok = 1; }
  __syncthreads();
  for (int r = 0; r < C; r++) {
    if (threadIdx.x == 0) {
      int h = -1;
      const int was_aborted = aborted;
      const int ns = res[2 * r], syn0 = res[2 * r + 1];
      if (ns < 0 || ns > NSNAP) { err = 1; } /* the decoder did not write this block */
      else if (!aborted)
        for (int s = 0; s < ns && h < 0; s++)
          if (rb->cand_ok[r * NSNAP + s]) h = s;
      if (h < 0) {
        if (!aborted && syn0 != 0 && !err) need_cpu = 1;
        aborted = 1;
        all_ok = 0;
      }
      hit = h;
      rb->seg_ok[r] = h >= 0;
      rb->it[r] = (uint8_t)(h >= 0 ? h + 1 : (was_aborted ? 0 : 255));
    }
    __syncthreads();
    for (int i = threadIdx.x; i < KB; i += blockDim.x) rb->c[r][i] = hit >= 0 ? snaps[((size_t)r * NSNAP + hit) * KBMAX + i] : 0;
    __syncthreads();
  }
  if (threadIdx.x == 0) { rb->need_cpu = need_cpu; rb->err = err; rb->all_ok = all_ok; }
}

/* ---- class keys ---- */
struct Key {
  uint16_t nrb, nl, ndmrs, ncb;
  uint8_t fp32, evm, qmode, align;
  uint8_t nseg; /* 0 = a contiguous grant (the estimate rows go straight into the estimate buffer); 1, 2, 4, 8, 16 (32, 64, 128: split mode) = segmented grant of up to that many segments (scratch rows + compose) */
  uint8_t ul = 0; /* Task 7: a PUSCH pass class (nl = 1; nrb, ndmrs and ncb are its whole structure) */
  uint8_t sub = 0; /* v2 Task 3: 0 = the whole pass in one graph; SUB_C / SUB_D / SUB_L = a sub-graph, keyed by the fields it depends on only (key_sub) */
  uint8_t nz4 = 0; /* the LDPC kernel family baked into the graph: 1 = the generic kernel (a lifting size Z that is not a multiple of 4: Z in 2, 3, 5, 6, 7, 9, 10, 11, 13, 14, 15, 18, 22, 26, 30, hence one code block only), 0 = the 4-lane kernel */
  static_assert(NR_GPU_PASS_MAX_SEG <= 128, "pack(): the segment class takes 4 bits (1 + log2 128 = 8)");
  uint64_t pack() const
  {
    return (uint64_t)nrb | (uint64_t)nl << 9 | (uint64_t)ndmrs << 12 | (uint64_t)ncb << 15 | (uint64_t)(fp32 & 1) << 48 | (uint64_t)(fp32 >> 1) << 32 | (uint64_t)evm << 49 | (uint64_t)qmode << 24 |
           (uint64_t)align << 28 | (uint64_t)(nseg ? 1 + __builtin_ctz((unsigned)nseg) : 0) << 44 | (uint64_t)ul << 40 | (uint64_t)sub << 41 | (uint64_t)nz4 << 47; /* ncb takes bits 15..23 (up to 160) */
  }
};
/* v2 Task 3 (split): C = FEP + chest + [seg compose] + [nvar D2H]: depends on (nrb, nl, ndmrs, nseg); the parameter-block H2D precedes the first sub-graph of a launch
 * (h2d_params, Task 4); D = chest post + demod (+ EVM fork) + descramble + LLR stats + LLR norm + [LLR / est / rxF D2H]: (nrb, nl, fp32, evm, qmode, align); L = rate dematch + LDPC + CRC + pick + CRC +
 * [result D2H]: (ncb). The count of graphs is the sum of the three key sets, not their product. */
enum { SUB_FULL = 0, SUB_C = 1, SUB_D = 2, SUB_L = 3 };
static Key key_sub(const Key &k, int sub)
{
  Key r{};
  if (sub == SUB_C) r = Key{k.nrb, k.nl, k.ndmrs, 0, 0, 0, 0, 0, k.nseg};
  else if (sub == SUB_D) r = Key{k.nrb, k.nl, 0, 0, k.fp32, k.evm, k.qmode, k.align, 0};
  else if (sub == SUB_L) { r = Key{0, 0, 0, k.ncb, 0, 0, 0, 0, 0}; r.nz4 = k.nz4; }
  else return k;
  r.sub = (uint8_t)sub;
  return r;
}

struct Inst {
  int id = 0;
  cudaStream_t s = nullptr, s2 = nullptr, s_lo = nullptr; /* s_lo: v2 Task 2, the low-priority twin of s for probes (ISAC_PDSCH_GPU_PRIO=1), else null */
  cudaEvent_t done = nullptr, ev0 = nullptr, evh = nullptr; /* ev0: stream marker before the launches; evh: external event node at the head of the (first sub-)graph; done: after the last. Timing on: gstart = ev0->evh, gspan = evh->done */
  cudaEvent_t evC = nullptr, evD = nullptr; /* measurement only: stream markers after the C and after the D sub-graph launch (split DL pass): cspan = evh->evC, dspan = evC->evD, lspan = evD->done */
  nr_gpu_buf_t rxF = {}, est = {}, nvar = {}, geo = {}, scratch = {}, llr = {}, lin = {}, bits = {}, pil = {}, scr = {}, pb = {}, res = {}, segsc = {};
  nr_gpu_buf_t gath = {}; /* DISCRETE DL: k_gather's output */
  nr_gpu_buf_t uli = {}, ulb = {}, ulwin = {}; /* Task 7: the FEP items + geometry, the UlBlk, the DISCRETE IQ window copy */
  nr_gpu_chest_job_t *cj = nullptr;
  nr_gpu_fep_job_t fj = {};
  const int16_t *rx[4] = {};
  nr_gpu_ldpc_job_t *lj = nullptr; /* [MAXCB]: only BG/R/Z/K/it_max are used */
  nr_gpu_demod_job_t dj = {};
  size_t bn = 0;
  /* v2 Task 4: the C inputs (c_sig) of the last two launches on this instance, double-buffered; c_ok = the estimate on the device is that of sig[sig_cur] */
  std::vector<uint8_t> sig[2];
  size_t sig_n[2] = {};
  int sig_cur = 0;
  bool c_ok = false;
  uint16_t fep_run = 0; /* symbols the last C of this instance transformed (grant-sized FEP): a held retry skips C only when its own set is inside */
  PassBlk *PB() const { return (PassBlk *)pb.host; }
  ResBlk *RB() const { return (ResBlk *)res.host; }
  const PassBlk *PBd() const { return (const PassBlk *)pb.dev; }
  ResBlk *RBd() const { return (ResBlk *)res.dev; }
  nrul::UlBlk *UB() const { return (nrul::UlBlk *)ulb.host; }
  const nrul::UlBlk *UBd() const { return (const nrul::UlBlk *)ulb.dev; }
};

struct Entry { cudaGraphExec_t exec[NI] = {}; };

static Inst g_inst[NI];
static int16_t *g_hq = nullptr;             /* DL HARQ soft store: NR_GPU_HQ_BLK blocks x NR_GPU_HQ_SEG int16 (device memory, allocated in nr_gpu_pass_init) */
static nr_gpu_buf_t g_ring[4], g_rot;       /* the warm replay's dummy IQ ring and rotation */
static std::map<uint64_t, Entry> g_cache;
static std::mutex g_mu;
static nr_gpu_pass_cfg_t g_cfg = {};
static bool g_cfg_set = false;
static std::atomic<int> g_ready{0}, g_dead{0};
static std::atomic<unsigned> g_busy{0};
static std::atomic<uint64_t> g_gate_skips{0}; /* passes the in-graph no-chance gate skipped */
static std::atomic<uint64_t> g_ovf_takes{0}, g_ovf_waits{0}; /* real grants that took the overflow (probe-pool) instance; that had to wait for a running probe */
static std::atomic<uint64_t> g_seg_launches{0}, g_seg_classes{0}, g_used_mb{0}, g_base_mb{0}, g_fb_unseen_seg{0}; /* g_used_mb: GPU memory in use by the device after the last pre-fence build */
static std::atomic<uint64_t> g_fb_busy_probe{0}; /* v2 Task 2 review: probes refused an instance (the reserve below), not counted in g_fb_busy */
static std::atomic<uint64_t> g_launches{0}, g_hits{0}, g_fb_unseen{0}, g_fb_busy{0}, g_fb_unsup{0}, g_fb_err{0}, g_need_cpu{0}, g_tb_fail{0}, g_allocs{0}, g_fb_ldpckey{0};
static std::atomic<uint64_t> g_wait_hist[32]; /* host wait, 2^i us buckets */
/* Task 8b stamps: host prep (run entry to launch), launch to done, post (done to commit end); [0] DL, [1] UL; 2^i us buckets */
static std::atomic<uint64_t> g_stamp_hist[2][5][32]; /* + [3] launch -> GPU start (ev0 -> head), [4] GPU span (head -> done) */
static void hist_add(std::atomic<uint64_t> *h, uint64_t ns)
{
  const uint64_t us = ns / 1000;
  int b = 0;
  while (b < 31 && (1ull << b) < us) b++;
  ++h[b];
}
static uint64_t hist_q(const std::atomic<uint64_t> *h, int pct) /* bucket upper bound in us */
{
  uint64_t tot = 0, acc = 0;
  for (int i = 0; i < 32; i++) tot += h[i];
  for (int i = 0; i < 32 && tot; i++) {
    acc += h[i];
    if (acc * 100 >= tot * pct) return 1ull << i;
  }
  return 0;
}
static void stamp_add(int ul, int k, uint64_t ns) { hist_add(g_stamp_hist[ul][k], ns); }
static uint64_t stamp_q(int ul, int k, int pct) { return hist_q(g_stamp_hist[ul][k], pct); }
/* GPU-side stamps of the pass that just completed (event timing): launch -> first node, first node -> done; the host wake is wall - both. Not for a pass whose C was skipped (no head node ran). */
static void gpu_stamps(int ul, const Inst &I, bool head, nr_gpu_pass_result_t *r)
{
  float a = 0, b = 0;
  r->gstart_ns = r->gspan_ns = 0;
  if (!head || cudaEventElapsedTime(&a, I.ev0, I.evh) != cudaSuccess || cudaEventElapsedTime(&b, I.evh, I.done) != cudaSuccess) return;
  stamp_add(ul, 3, (uint64_t)(a * 1e6f));
  stamp_add(ul, 4, (uint64_t)(b * 1e6f));
  r->gstart_ns = (uint32_t)(a * 1e6f);
  r->gspan_ns = (uint32_t)(b * 1e6f);
}
/* measurement only: the split DL pass's sub-graph spans (C absent when skipped: cspan 0, D from the marker after the parameter H2D) */
static void split_stamps(const Inst &I, bool c_ran, nr_gpu_pass_result_t *r)
{
  float c = 0, d = 0, l = 0;
  r->cspan_ns = r->dspan_ns = r->lspan_ns = 0;
  if ((c_ran && cudaEventElapsedTime(&c, I.evh, I.evC) != cudaSuccess) || cudaEventElapsedTime(&d, I.evC, I.evD) != cudaSuccess || cudaEventElapsedTime(&l, I.evD, I.done) != cudaSuccess) {
    cudaGetLastError();
    return;
  }
  r->cspan_ns = (uint32_t)(c * 1e6f);
  r->dspan_ns = (uint32_t)(d * 1e6f);
  r->lspan_ns = (uint32_t)(l * 1e6f);
}
/* measurement only: the L sub-graph's LDPC launch from its per-code-block verdicts (res) and %globaltimer stamps (ts); host-readable in the DIRECT model in place, in the DISCRETE model after bits_tail_d2h. nd = the TB's blocks,
 * ncb = the class's (padding blocks repeat block 0 on zero LLRs and stop at the first check). */
static void ldpc_info(const Inst &I, int nd, int ncb, nr_gpu_pass_result_t *r)
{
  memset(&r->li, 0, sizeof(r->li));
  if (!I.bits.host || nd < 1 || ncb < nd || ncb > NR_GPU_PASS_LDPC_JOBS) return;
  const int *res = (const int *)((const char *)I.bits.host + NR_GPU_PASS_LDPC_RES_OFF);
  const unsigned long long *ts = (const unsigned long long *)((const char *)I.bits.host + NR_GPU_PASS_LDPC_TS_OFF);
  unsigned long long e0 = ~0ull, e1 = 0, x1 = 0;
  for (int c = 0; c < ncb; c++) {
    if (res[2 * c] < 0 || !ts[2 * c] || ts[2 * c + 1] < ts[2 * c]) return; /* a block that did not run in this launch: no verdict */
    e0 = std::min(e0, ts[2 * c]);
    e1 = std::max(e1, ts[2 * c]);
    x1 = std::max(x1, ts[2 * c + 1]);
  }
  uint32_t dc = 0, dl = 0;
  int nconv = 0, nlast = 0, itmax = 0, itsum = 0;
  for (int c = 0; c < nd; c++) {
    const uint32_t d = (uint32_t)(ts[2 * c + 1] - ts[2 * c]);
    if (res[2 * c + 1]) { nconv++; itmax = std::max(itmax, res[2 * c]); itsum += res[2 * c]; dc = std::max(dc, d); }
    else { nlast++; dl = std::max(dl, d); }
  }
  uint32_t dp = 0;
  int npad = 0;
  for (int c = nd; c < ncb; c++) { /* the class's padding blocks (zero LLRs) */
    dp = std::max(dp, (uint32_t)(ts[2 * c + 1] - ts[2 * c]));
    npad += !res[2 * c + 1];
  }
  const ResBlk &rb = *I.RB();
  r->li.ncb = (uint16_t)nd; r->li.nconv = (uint16_t)nconv; r->li.nlast = (uint16_t)nlast; r->li.it_conv_max = (uint16_t)itmax; r->li.it_conv_sum = (uint16_t)std::min(itsum, 65535);
  r->li.kspan_ns = (uint32_t)(x1 - e0);
  r->li.swait_ns = (uint32_t)(e1 - e0);
  r->li.dconv_max_ns = dc;
  r->li.dlast_max_ns = dl;
  r->li.g2l_ns = rb.tg[3] && e0 > rb.tg[3] ? (uint32_t)(e0 - rb.tg[3]) : 0;
  r->li.d2l_ns = rb.tg[1] && rb.tg[2] > rb.tg[1] ? (uint32_t)(rb.tg[2] - rb.tg[1]) : 0;
  r->li.npad = (uint16_t)(ncb - nd);
  r->li.npad_last = (uint16_t)npad;
  r->li.dpad_max_ns = dp;
  r->li.valid = 1;
}
static std::atomic<uint32_t> g_pass_seq{0};
/* v2 Task 4: the instance this thread's decode call holds (-1 none), the call nesting depth, when it was taken; entries [C, D], held calls, dirty invalidations, hold time */
static thread_local int t_hold = 0, t_held = -1;
static thread_local uint64_t t_hold_ns = 0;
static std::atomic<uint64_t> g_entry[2], g_held{0}, g_dirty{0}, g_hold_hist[32];
constexpr size_t SIG_CAP = 256 * 1024; /* c_sig bytes: pilots 4 DM-RS x 4 layers x 6 x 554 RBs x 4 B = 213 KB + the rest */
constexpr uint32_t RING_LEN = 80000;
static int16_t g_zero_pil[6 * 273 * 2];
static uint32_t g_zero_scr[LLR_CAP / 32 + 2];

static bool guard_alloc() { return nr_gpu_allocations_allowed() != 0; }

/* ---- the synthetic job of a class (capture and warm replay): a well-formed grant of the class with an empty signal ---- */
static void synth_job(const Key &k, nr_gpu_pass_job_t *j, std::vector<int16_t> &geo)
{
  memset(j, 0, sizeof(*j));
  j->shape = {k.nrb, 13, k.nl, 2, k.ndmrs, k.ncb, k.fp32, k.nseg};
  for (int a = 0; a < 4; a++) j->rxdata[a] = (const int16_t *)g_ring[a].host;
  j->ring_len = RING_LEN;
  j->symbol_rot = (const int16_t *)g_rot.host;
  j->nbRx = g_cfg.nbRx;
  j->nl = k.nl;
  j->ndmrs = k.ndmrs;
  static const int sy[4] = {2, 6, 9, 12};
  for (int i = 0; i < k.ndmrs; i++) j->dmrs_sym[i] = sy[i];
  for (int i = 0; i < k.ndmrs; i++)
    for (int l = 0; l < k.nl; l++) j->pil[i][l] = g_zero_pil;
  j->nrb = k.nrb;
  j->k0 = 0;
  if (k.nseg >= 1) { /* nseg segments of w PRBs side by side */
    const int w = std::max(1, (int)k.nrb / k.nseg);
    j->nseg = k.nseg;
    j->nrb = w * k.nseg;
    j->seg_pil_nrb = j->nrb;
    for (int s = 0; s < k.nseg; s++) j->seg[s] = {(uint16_t)w, (uint16_t)(s * w), s * w, 0};
  }
  j->number_symbols = 13;
  j->branch = -1;
  j->fo_first = k.ndmrs > 1 ? 0 : -1;
  j->fo_last = k.ndmrs > 1 ? k.ndmrs - 1 : -1;
  j->dt_d = 1.0e-4;
  j->df_d = 30000.0;
  nr_gpu_demod_job_t &d = j->dj;
  const int ext = (12 * k.nrb + 15) & ~15;
  d.nbRx = g_cfg.nbRx; d.nl = k.nl; d.qm = 2; d.N = N;
  d.start_symbol = 1; d.nb_symbols = 13;
  uint32_t off = 0;
  bool first = true;
  for (int m = 1; m < NSYM; m++) {
    bool isd = false;
    for (int i = 0; i < k.ndmrs; i++) isd |= sy[i] == m;
    d.llr_off[m] = off;
    if (isd) continue;
    d.sym_mask |= (uint16_t)(1u << m);
    d.nre[m] = (uint16_t)(12 * k.nrb);
    off += (uint32_t)(12 * k.nrb) * 2 * k.nl;
    if (first) { d.first_symbol = m; first = false; }
  }
  d.rx_stride = NSYM * N; d.est_stride = NSYM * N; d.buf = ext; d.ext = ext;
  d.thr_coef = pow(10.0, -1.2);
  d.align = k.align;
  d.mag_per_layer = k.nl >= 2 && k.qmode >= 2;
  d.qam_mode = k.nl >= 2 ? k.qmode : 0;
  d.m3_nvfac = pow(10.0, 2.1);
  d.want_evm = k.evm;
  d.evm_fp64 = !(k.fp32 & 1);
  d.demod_fp32 = (k.fp32 >> 1) & 1;
  geo.assign((size_t)2 * NSYM * ext * 2, 0); /* geo_y then geo_h: c16 [14][ext] */
  for (int m = 0; m < NSYM; m++)
    for (int i = 0; i < ext; i++) {
      int near = 0;
      for (int q = 0; q < k.ndmrs; q++)
        if (sy[q] <= m) near = q;
      geo[2 * ((size_t)m * ext + i)] = (int16_t)((N - 273 * 6 + i) % N);
      geo[2 * ((size_t)m * ext + i) + 1] = 1;
      geo[2 * (((size_t)NSYM + m) * ext + i)] = (int16_t)i;
      geo[2 * (((size_t)NSYM + m) * ext + i) + 1] = (int16_t)(sy[near] + 1);
    }
  j->geo_y = geo.data();
  j->geo_h = geo.data() + (size_t)2 * NSYM * ext;
  j->scr = g_zero_scr;
  j->G = off;
  j->norm = 1;
  j->norm_span = off;
  j->A = 500; j->C = 1; j->K = 640; j->Z = 64; j->F = 640 - (500 + 16); j->E = off; j->E2 = off; j->first_rE2 = 1;
  if (k.nz4) { j->A = 200; j->K = 300; j->Z = 30; j->F = 300 - (200 + 16); } /* BG2 Kb 10: the generic LDPC kernel is captured */
  j->BG = 2; j->R = 15; j->R2 = 15; j->rv = 0; j->Qm = 2; j->it_max = 8;
}

/* the transport block part of the PassBlk (descrambling words, rate-dematching / CRC tables, LDPC jobs); shared by the DL and the UL pass */
static int prepare_tb(Inst &I, const nr_gpu_pass_job_t *j)
{
  PassBlk &pb = *I.PB();
  /* v2 Task 2: a probe decodes the first n_decode blocks with the parameters (K', CRC type, E, offsets) of the whole TB */
  const uint32_t nd = j->n_decode && j->n_decode < j->C ? j->n_decode : j->C;
  pb.G = j->G; pb.span = j->norm_span; pb.norm = j->norm; pb.tbs = j->A;
  {
    static int old_rule = -1, target = 0, dk = 2; /* NR_TEST_L2NORM_OLD / _TARGET: the same test seams as nr_llr_norm_shift_nl (host) */
    if (old_rule < 0) {
      old_rule = getenv("NR_TEST_L2NORM_OLD") ? 1 : 0;
      target = getenv("NR_TEST_L2NORM_TARGET") ? atoi(getenv("NR_TEST_L2NORM_TARGET")) : 40;
      dk = getenv("NR_TEST_L2NORM_DK") ? atoi(getenv("NR_TEST_L2NORM_DK")) : 2;
    }
    pb.nl = old_rule ? 0 : j->dj.nl; pb.qm = j->Qm; pb.l2t = target; pb.l2dk = dk;
  }
  pb.Gd = j->probe ? j->G_dem : j->G_dem && j->G_dem < j->G ? j->G_dem : j->G; /* a probe: the LLRs up to its horizon (> 0, checked in dl_try) */
  pb.ncb = (int)nd; pb.KB = (int)((j->K + 7) >> 3);
  if (nr_gpu_model() == NR_GPU_MODEL_DIRECT) {
    pb.scr = j->scr;
  } else {
    memcpy(I.scr.host, j->scr, (size_t)((j->G + 31) / 32) * 4);
    pb.scr = (const uint32_t *)I.scr.dev;
  }
  const int Kprime = (j->C > 1) ? (int)((j->A + 24 + 24 * j->C) / j->C) : (int)(j->A + (j->A > 3824 ? 24 : 16));
  const int ctype = (j->C > 1) ? 1 : (j->A > 3824 ? 0 : 2);
  const int pay = (int)((j->K - j->F) >> 3) - 3; /* K' / 8 (K - F = K' for every lifting size, byte aligned for every TBS) */
  const int ncb = j->shape.ncb_class;
  for (int s = 0; s < ncb; s++) {
    nr_gpu_rdm_seg_t &g = pb.rdm[s];
    memset(&g, 0, sizeof(g));
    if ((uint32_t)s < nd) {
      const bool first = (uint32_t)s < j->first_rE2;
      g.E = first ? j->E : j->E2;
      g.llr_off = first ? (uint32_t)s * j->E : j->first_rE2 * j->E + ((uint32_t)s - j->first_rE2) * j->E2;
      g.Qm = j->Qm; g.BG = j->BG; g.Z = j->Z; g.K = j->K; g.F = j->F; g.Kprime = (uint32_t)Kprime; g.Tbslbrm = j->tbslbrm; g.C = j->C; g.rv = j->rv;
      if (j->hq_mode && g_hq && j->hq_blk[s] < NR_GPU_HQ_BLK && !j->probe) {
        g.hq_mode = j->hq_mode;
        g.hq = g_hq + (size_t)j->hq_blk[s] * NR_GPU_HQ_SEG;
      }
    } /* padding blocks: C == 0 is "refused" -> zeros */
    for (int q = 0; q < NSNAP; q++) {
      nr_gpu_crc_seg_t &c = pb.cand[s * NSNAP + q];
      c = {(uint32_t)((s * NSNAP + q) * KBMAX), (uint32_t)((uint32_t)s < nd ? Kprime : 0), 0u, (uint32_t)ctype};
    }
    pb.sel[s] = {(uint32_t)(offsetof(ResBlk, c) + (size_t)s * KBMAX), (uint32_t)((uint32_t)s < nd ? Kprime : 0), (uint32_t)(pay > 0 ? pay : 0), (uint32_t)ctype};
  }
  /* LDPC jobs: the padding blocks repeat block 0 (a zero input stops them at the first syndrome check) */
  for (int s = 0; s < ncb; s++) {
    const uint32_t q = (uint32_t)s < nd ? (uint32_t)s : 0u;
    nr_gpu_ldpc_job_t &l = I.lj[s];
    l.BG = j->BG; l.R = q < j->first_rE2 ? j->R : j->R2; l.Z = (uint16_t)j->Z; l.K = (uint16_t)j->K; l.it_max = j->it_max; l.llr = nullptr; l.pad = g_sized.load(std::memory_order_relaxed) && (uint32_t)s >= nd;
  }
  if (nr_pdsch_gpu_ldpc_pass_set(I.id, I.lj, ncb, &I.lin) != 0) return -2; /* an LDPC (BG, Z, R) the init did not build */
  return 0;
}

/* The result block back to the host (DISCRETE only; a no-op in the shared model): the verdicts, then the code blocks of the class (c[ncb] of up to 160 x 1056 bytes), then the fields behind c[]. */
static int res_d2h(Inst &I, int ncb, cudaStream_t s)
{
  return nr_gpu_buf_d2h_async(&I.res, 0, offsetof(ResBlk, c) + (size_t)ncb * KBMAX, s) || nr_gpu_buf_d2h_async(&I.res, offsetof(ResBlk, ul), sizeof(ResBlk) - offsetof(ResBlk, ul), s);
}

/* DISCRETE only: the estimate and equaliser rows the host reads, gathered into one compact buffer (3 * nrx rows of N c16, then the qm row) so that a rank > 1 pass copies them instead of the whole
 * estimate (layers 1.. never reached the host) and the equalised symbol row needs no copy of the scratch. Block b < 3 * nrx: row (b / nrx, antenna b % nrx); block 3 * nrx: the qm row. */
__global__ void k_gather(const short2 *est, const short2 *comp, short2 *out, const PassBlk *pb, int nrx)
{
  const int b = blockIdx.x;
  const short2 *src;
  int n;
  if (b < 3 * nrx) {
    const int row = pb->ge_row[b / nrx];
    if (row < 0) return;
    src = est + (size_t)row + (size_t)(b % nrx) * NSYM * N;
    n = N;
  } else {
    n = pb->qm_n;
    if (n <= 0) return;
    src = comp + pb->qm_off;
  }
  short2 *dst = out + (size_t)b * N;
  for (int i = threadIdx.x; i < n; i += blockDim.x) dst[i] = src[i];
}
/* DISCRETE (DL): the tail of the LDPC result buffer the measurement-only ldpc_info reads: the iteration counts / verdicts and the %globaltimer stamps of the class's code blocks. A no-op in the shared model. */
static int bits_tail_d2h(Inst &I, int ncb, cudaStream_t s)
{
  return nr_gpu_buf_d2h_async(&I.bits, NR_GPU_PASS_LDPC_RES_OFF, (size_t)2 * ncb * sizeof(int), s) ||
         nr_gpu_buf_d2h_async(&I.bits, NR_GPU_PASS_LDPC_TS_OFF, (size_t)2 * ncb * sizeof(unsigned long long), s);
}
/* DISCRETE (DL): the rows of the estimate / equaliser output the host consumers read (brfo, DM-RS SNR at layer nl - 1, the qm row), gathered by k_gather and copied; DIRECT: the host reads the
 * mapped estimate in place and the node is the former (no-op) copy, so that graph is unchanged. */
static int est_d2h(Inst &I, int nbRx, cudaStream_t s)
{
  if (nr_gpu_model() == NR_GPU_MODEL_DIRECT) return nr_gpu_buf_d2h_async(&I.est, 0, (size_t)nbRx * NSYM * N * 4, s);
  k_gather<<<3 * nbRx + 1, 256, 0, s>>>((const short2 *)I.est.dev, (const short2 *)I.scratch.dev, (short2 *)I.gath.dev, I.PBd(), nbRx);
  return nr_gpu_buf_d2h_async(&I.gath, 0, (size_t)(3 * nbRx + 1) * N * 4, s);
}

/* the host side of a pass: writes every block the kernels read. Used for the synthetic job (capture / warm replay) and for real grants. `cls` = the key the
 * graph structure comes from (the job must fit it). 0 or -1. */
/* the symbols of the slot this job's FEP transforms (prepare: I.fj.run_mask); 0 = a job that did not size it (the synthetic job of a class, tests): all 14 */
static inline uint16_t fep_need(const nr_gpu_pass_job_t *j) { return g_sized.load(std::memory_order_relaxed) && j->copy_mask ? (uint16_t)(j->copy_mask & 0x3fff) : (uint16_t)0x3fff; }
static Key key_of(const nr_gpu_pass_job_t *j);
static int c_groups_per_chunk(const Key &k);
static int prepare(Inst &I, const nr_gpu_pass_job_t *j)
{
  const int nbRx = g_cfg.nbRx, nl = j->nl, nd = j->ndmrs, ext = (int)j->dj.ext;
  const bool segd = j->shape.nseg_class >= 1;                     /* segmented class: scratch rows + compose */
  const int ns = segd ? j->shape.nseg_class : 1;                   /* chest row sets per (DM-RS symbol, layer): the class's segment count */
  if (j->nseg > ns || j->nseg < 0 || (j->nseg == 0) == segd) return -1;
  const bool redirect = nr_gpu_model() == NR_GPU_MODEL_DIRECT && j->rxdataF;
  const int16_t *rxbase = redirect ? (const int16_t *)j->rxdataF : (const int16_t *)I.rxF.dev;
  const size_t rxstride = redirect ? j->rx_stride : (size_t)NSYM * N; /* c16 between antennas */
  /* FEP block */
  for (int a = 0; a < 4; a++) I.rx[a] = j->rxdata[a];
  I.fj = {I.rx, j->ring_len, j->ring_offset, j->symbol_rot, 0x3fff, redirect ? (int16_t *)j->rxdataF : nullptr, redirect ? (uint32_t)j->rx_stride : 0u, 0};
  I.fj.run_mask = fep_need(j); /* grant-sized: the captured grid stays 14 symbols, the blocks of the other symbols return at once */
  if (nr_pdsch_gpu_fep_stage_at(&I.fj, I.id) != 0) return -1;
  /* chest rows: one per (DM-RS symbol, layer, segment, antenna), the order of nr_pdsch_gpu_chest_grant (ns == 1: one segment = the grant). A class with several
   * segments estimates into scratch rows (the estimator writes a whole row) and k_seg_compose lays them out in data order. */
  nr_gpu_chest_job_t &cj = *I.cj;
  cj.type = 1; /* the instance's job is shared with the PUSCH pass (type 3) */
  int r = 0;
  const int pn = j->nseg ? j->seg_pil_nrb : j->nrb; /* RBs of the pilot sequence behind pil[][] */
  const int dtype = j->dmrs_type == 2 ? 2 : 1, ppr = dtype == 2 ? 4 : 6; /* DMRS2: type-2 rows (k_ls reads the row's type), 4 pilots (c16) per RB */
  const int gpc = c_groups_per_chunk(key_of(j)), crows = std::max(1, gpc * ns * nbRx); /* rows of one C chunk */
  if (gpc < 1) return -1;
  for (int k = 0; k < nd; k++)
    for (int l = 0; l < nl; l++) {
      int16_t *pil = (int16_t *)I.pil.host + (size_t)(k * 4 + l) * PIL_STRIDE * 2;
      memcpy(pil, j->pil[k][l], (size_t)ppr * pn * sizeof(int16_t) * 2);
      for (int sg = 0; sg < ns; sg++)
        for (int a = 0; a < nbRx; a++, r++) {
          nr_gpu_chest_row_t &row = cj.rows[r];
          const bool real = j->nseg && sg < j->nseg;
          row.rxF = rxbase + 2 * ((size_t)a * rxstride + (size_t)N * j->dmrs_sym[k] + j->delta[l]);
          row.pil = (const int16_t *)I.pil.dev + (size_t)(k * 4 + l) * PIL_STRIDE * 2 + (real ? 2 * (size_t)ppr * j->seg[sg].pil_rb : 0);
          row.type = dtype;
          row.out = segd ? (int16_t *)I.segsc.dev + 2 * (size_t)(r % crows) * N /* the row inside its C chunk (SEG128) */
                           : (int16_t *)I.est.dev + 2 * ((size_t)(l * nbRx + a) * NSYM * N + (size_t)j->dmrs_sym[k] * N);
          row.k0 = j->nseg ? (real ? j->seg[sg].k0 : 0) : j->k0;
          row.nb_rb = j->nseg ? (real ? j->seg[sg].n_prb : 1) : j->nrb; /* a padding row estimates one PRB nobody reads */
        }
    }
  cj.n_rows = r;
  if (nr_pdsch_gpu_chest_stage_at(&cj, r, I.id) != 0) return -1;
  /* demod geometry and job */
  uint8_t *geo = (uint8_t *)I.geo.host;
  memcpy(geo, j->geo_y, (size_t)NSYM * ext * 4);
  memcpy(geo + (size_t)NSYM * ext * 4, j->geo_h, (size_t)NSYM * ext * 4);
  nr_gpu_demod_job_t &dj = I.dj;
  memcpy(&dj, &j->dj, sizeof(dj));
  dj.rxdataF = rxbase;
  dj.est = (const int16_t *)I.est.dev;
  dj.rx_stride = (uint32_t)rxstride;
  dj.est_stride = NSYM * N;
  dj.buf = ext;
  dj.geo_y = (const int16_t *)I.geo.dev;
  dj.geo_h = (const int16_t *)((char *)I.geo.dev + (size_t)NSYM * ext * 4);
  int16_t *sc = (int16_t *)I.scratch.dev;
  dj.comp = sc; dj.mag = sc + I.bn; dj.magb = sc + 2 * I.bn; dj.magr = sc + 3 * I.bn;
  dj.llr = (int16_t *)I.llr.dev;
  dj.nvar = 0;
  dj.nvar_dev = (const uint32_t *)((char *)I.res.dev + offsetof(ResBlk, post.nvar));
  dj.evm_defer = 0;
  if (nr_pdsch_gpu_demod_pass_set(I.id, &dj) != 0) return -1;
  /* PassBlk */
  PassBlk &pb = *I.PB();
  memset(&pb.post, 0, sizeof(pb.post));
  pb.post.nvar_rows = (const uint32_t *)((char *)I.nvar.dev + 3 * NR_GPU_CHEST_MAX_ROWS * sizeof(int));
  pb.post.nrx = nbRx; pb.post.nl = nl; pb.post.n_dmrs_sym = nd;
  if (ns > 1) {
    pb.post.nseg = ns;
    pb.post.nrb_total = j->nrb;
  }
  pb.post.number_symbols = j->number_symbols; pb.post.nvfix = j->nvfix; pb.post.branch = j->branch;
  if (j->fo_first >= 0 && j->fo_last > j->fo_first) {
    pb.post.h0 = (const int16_t *)I.est.dev + 2 * (size_t)N * j->dmrs_sym[j->fo_first];
    pb.post.h1 = (const int16_t *)I.est.dev + 2 * (size_t)N * j->dmrs_sym[j->fo_last];
  }
  pb.post.N = N; pb.post.n_rb_dl = g_cfg.n_rb_dl;
  pb.post.dt_d = j->dt_d; pb.post.df_d = j->df_d; pb.post.dt_f = (float)j->dt_d; pb.post.df_f = (float)j->df_d;
  for (int sg = 0; sg < NR_GPU_PASS_MAX_SEG; sg++) {
    const bool real = sg < j->nseg;
    pb.post.seg_w[sg] = real && ns > 1 ? j->seg[sg].n_prb : 0;
    pb.seg_n[sg] = real ? (uint16_t)(12 * j->seg[sg].n_prb) : 0;
    pb.seg_d[sg] = real ? (uint16_t)(12 * j->seg[sg].data_index) : 0;
  }
  for (int k = 0; k < NR_GPU_PASS_MAX_DMRS; k++) pb.dsym[k] = k < nd ? j->dmrs_sym[k] : 0;
  {
    PassBlk &gb = *I.PB();
    const int mlast = j->dmrs_sym[nd - 1], llast = nl - 1;
    gb.gate_on = j->gate_on && g_cfg.split && !j->probe && std::isfinite(j->gate_thr_db);
    gb.gate_thr_db = j->gate_thr_db;
    gb.gate_nbrx = nbRx; gb.gate_n = 12 * j->nrb; gb.gate_type = j->dmrs_type == 2 ? 2 : 1; gb.gate_seg = j->nseg > 1;
    gb.gate_est = (const short2 *)I.est.dev + ((size_t)(llast * nbRx) * NSYM * N + (size_t)mlast * N);
    gb.gate_astride = (size_t)NSYM * N;
    gb.gate_nvar = (const uint32_t *)((const char *)I.nvar.dev + 3 * NR_GPU_CHEST_MAX_ROWS * sizeof(int)) + (size_t)(nd * nl - 1) * nbRx;
    gb.ge_row[0] = gb.ge_row[1] = -1;
    if (j->fo_first >= 0 && j->fo_last > j->fo_first) { /* the rows brfo_rows copies: layer 0 */
      gb.ge_row[0] = j->dmrs_sym[j->fo_first] * N;
      gb.ge_row[1] = j->dmrs_sym[j->fo_last] * N;
    }
    gb.ge_row[2] = (llast * nbRx) * NSYM * N + mlast * N; /* the DM-RS SNR rows: layer nl - 1 */
    gb.qm_n = 0;
    gb.qm_off = 0;
    if (j->qm_sym >= 0 && j->qm_sym < NSYM && j->qm_n > 0 && j->qm_n <= (uint32_t)N) { /* comp[(m * NR_GPU_DEMOD_MAX_NL + layer) * buf + i], layer 0 */
      gb.qm_off = (int)((size_t)j->qm_sym * NR_GPU_DEMOD_MAX_NL * ext);
      gb.qm_n = (int)j->qm_n;
    }
  }
  return prepare_tb(I, j);
}

/* the parameter blocks (DISCRETE: H2D; DIRECT: no-op, the kernels read the mapped memory). The single graph (v1) carries them as its first nodes; in split mode they
 * are copied before the first sub-graph of every launch, whatever the entry (C, or D on a held instance), so that D and L never read a previous body's PassBlk / geometry /
 * scrambling words (review T4-1). The pilots are read by C only. */
static int h2d_params(Inst &I, const Key &k, cudaStream_t s, bool pil)
{
  const int ext = (12 * k.nrb + 15) & ~15;
  return nr_gpu_buf_h2d_async(&I.pb, 0, sizeof(PassBlk), s) || nr_gpu_buf_h2d_async(&I.geo, 0, (size_t)2 * NSYM * ext * 4, s) ||
         (pil && nr_gpu_buf_h2d_async(&I.pil, 0, (size_t)NR_GPU_PASS_MAX_DMRS * 4 * PIL_STRIDE * 4, s)) ||
         nr_gpu_buf_h2d_async(&I.scr, 0, (size_t)(((size_t)12 * k.nrb * 13 * 8 * k.nl + 31) / 32) * 4, s);
}

/* the pass (sub = SUB_FULL: the v1 node order, unchanged) or one sub-graph of it (v2 split, key_sub). Split: C ends at the estimate (its inputs are the slot, the
 * DM-RS geometry and the pilots only); the chest post (nvar normalisation reads number_symbols, the per-branch choice) heads D, so a retry body that changes S or the RE
 * set re-enters at D on a held instance (Task 4). The post only writes ResBlk.post, so re-running it on the same estimate is idempotent. */
static int enqueue(Inst &I, const Key &k, const nr_gpu_pass_job_t *sj, cudaStream_t s, int sub = SUB_FULL, int part = 0)
{
  const int ncb = k.ncb, nbRx = g_cfg.nbRx;
  const bool full = sub == SUB_FULL;
  if ((full || sub == SUB_C) && cudaEventRecordWithFlags(I.evh, s, cudaEventRecordExternal) != cudaSuccess) return -1; /* the head of the graph: GPU-side start stamp */
  const size_t llr_bytes = std::min<size_t>((size_t)LLR_CAP, (size_t)12 * k.nrb * 13 * 8 * k.nl) * 2;
  const size_t nvar_bytes = (size_t)k.ndmrs * k.nl * (k.nseg ? k.nseg : 1) * nbRx * sizeof(int);
  if (full && h2d_params(I, k, s, true)) return -1;
  if (full || sub == SUB_C) {
    if (nr_pdsch_gpu_fep_dev(&I.fj, &I.rxF, I.id, s) != 0) return -1;
    /* the chest rows in C chunks of whole (DM-RS symbol, layer) groups (SEG128; one chunk otherwise): chest then compose per chunk, the scratch rows reused in stream order */
    const int gpc = c_groups_per_chunk(k), ng = k.ndmrs * k.nl, grp = (k.nseg ? k.nseg : 1) * nbRx;
    if (gpc < 1 || (gpc < ng && !k.nseg)) return -1;
    for (int g0 = 0; g0 < ng; g0 += gpc) {
      const int gn = std::min(gpc, ng - g0);
      if (nr_pdsch_gpu_chest_dev_at(I.cj, g0 * grp, gn * grp, k.nseg ? &I.segsc : &I.est, &I.nvar, I.id, s) != 0) return -1;
      if (k.nseg) k_seg_compose<<<gn * nbRx, 256, 0, s>>>((const short2 *)I.segsc.dev, (short2 *)I.est.dev, I.PBd(), k.nseg, k.nl, nbRx, g0);
    }
    if (full && nr_gpu_pass_launch_chest_post_p(I.PBd() ? &I.PBd()->post : nullptr, &I.RBd()->post, s) != 0) return -1;
    if (!full && nr_gpu_buf_d2h_async(&I.nvar, 3 * NR_GPU_CHEST_MAX_ROWS * sizeof(int), nvar_bytes, s)) return -1;
  }
  /* part (Fix A+D gate): 0 = the whole sub-graph; for D: 1 = the chest post (before the gate), 2 = the rest (the IF body); for L: 2 = the body, 3 = the result copy (after the IF, so the verdict of a skipped grant reaches the host) */
  if (sub == SUB_D && part == 3) return 0;
  if (full || sub == SUB_D) {
    if (!full && part != 2 && nr_gpu_pass_launch_chest_post_p(&I.PBd()->post, &I.RBd()->post, s) != 0) return -1;
    if (part == 1) return cudaPeekAtLastError() == cudaSuccess ? 0 : -1;
    if (nr_pdsch_gpu_demod_pass_dev(I.id, &sj->dj, &I.llr, I.dj.nvar_dev, s, I.s2) != 0) return -1;
    k_unscr<<<256, 256, 0, s>>>((int16_t *)I.llr.dev, I.PBd());
    if (g_cfg.v2) k_llr_stats<<<1, 1024, 0, s>>>((const int16_t *)I.llr.dev, I.PBd(), I.RBd()); /* v2 only: the v1 graph is unchanged */
    k_norm_sum<<<1, 1024, 0, s>>>((const int16_t *)I.llr.dev, I.PBd(), I.RBd());
    k_norm_apply<<<256, 256, 0, s>>>((int16_t *)I.llr.dev, I.PBd(), I.RBd());
    if (!full && (nr_gpu_buf_d2h_async(&I.llr, 0, llr_bytes, s) || est_d2h(I, nbRx, s) ||
                  nr_gpu_buf_d2h_async(&I.rxF, 0, (size_t)nbRx * NSYM * N * 4, s)))
      return -1;
  }
  if (sub == SUB_L && part == 3) return res_d2h(I, k.ncb, s) || bits_tail_d2h(I, k.ncb, s) ? -1 : (cudaPeekAtLastError() == cudaSuccess ? 0 : -1);
  if (sub == SUB_L && part == 1) return 0;
  if (full || sub == SUB_L) {
    if (nr_gpu_pass_launch_rdm((const int16_t *)I.llr.dev, I.PBd()->rdm, ncb, (int8_t *)I.lin.dev, s) != 0) return -1;
    if (nr_pdsch_gpu_ldpc_pass_dev(I.id, I.lj, ncb, &I.lin, &I.bits, s) != 0) return -1;
    const uint8_t *snaps = (const uint8_t *)I.bits.dev;
    const int *dres = (const int *)((const char *)I.bits.dev + NR_GPU_PASS_LDPC_RES_OFF);
    if (nr_gpu_pass_launch_crc_p(snaps, I.PBd()->cand, ncb * NSNAP, &I.PBd()->tbs, I.RBd()->cand_ok, &I.RBd()->dummy_tb, s) != 0) return -1;
    k_pick<<<1, 128, 0, s>>>(snaps, dres, I.PBd(), I.RBd());
    if (nr_gpu_pass_launch_crc_p((const uint8_t *)I.res.dev, I.PBd()->sel, ncb, &I.PBd()->tbs, I.RBd()->seg_crc, &I.RBd()->tb_crc, s) != 0) return -1;
    if (!full && part == 0 && (res_d2h(I, k.ncb, s) || bits_tail_d2h(I, k.ncb, s))) return -1;
  }
  if (full && (nr_gpu_buf_d2h_async(&I.llr, 0, llr_bytes, s) || nr_gpu_buf_d2h_async(&I.nvar, 3 * NR_GPU_CHEST_MAX_ROWS * sizeof(int), nvar_bytes, s) ||
               res_d2h(I, k.ncb, s) || est_d2h(I, nbRx, s) ||
               nr_gpu_buf_d2h_async(&I.rxF, 0, (size_t)nbRx * NSYM * N * 4, s)))
    return -1;
  return cudaPeekAtLastError() == cudaSuccess ? 0 : -1;
}


/* ===================================================================== Task 7: the PUSCH pass ===================================================================== */
static std::atomic<uint64_t> g_ul_launches{0}, g_ul_hits{0}, g_ul_fb_unseen{0}, g_ul_fb_busy{0}, g_ul_fb_unsup{0}, g_ul_fb_err{0}, g_ul_fb_ldpckey{0}, g_ul_tb_fail{0}, g_ul_need_cpu{0};
static std::atomic<uint64_t> g_ul_wait_hist[32];

/* a well-formed grant of the class with an empty signal: the capture and the warm replay */
static void synth_job_ul(const Key &k, nr_gpu_pass_job_t *j)
{
  memset(j, 0, sizeof(*j));
  j->shape = {k.nrb, 13, 1, 2, k.ndmrs, k.ncb, 0, 0, 1};
  j->nbRx = g_cfg.nbRx;
  j->nl = 1;
  j->ndmrs = k.ndmrs;
  j->nrb = k.nrb;
  nr_gpu_pass_ul_t &u = j->ul;
  u.on = 1;
  for (int a = 0; a < 4; a++) u.iq[a] = (const int16_t *)g_ring[a].host;
  u.iq_len = RING_LEN;
  u.rot_nb_rb = 273;
  u.rot_fco = N - 273 * 6;
  static const int sy[4] = {2, 6, 9, 12};
  u.ndmrs = k.ndmrs;
  for (int i = 0; i < k.ndmrs; i++) {
    u.dmrs_sym[i] = sy[i];
    u.pil[i] = g_zero_pil;
  }
  u.nrb = k.nrb;
  u.k0 = 0;
  u.keep = -1;
  uint32_t off = 0;
  bool first = true;
  for (int m = 0; m < NSYM; m++) {
    u.win[m] = m >= 1 ? (m - 1) * 4448 : -1;
    bool isd = false;
    for (int i = 0; i < k.ndmrs; i++) isd |= sy[i] == m;
    u.is_dmrs[m] = isd;
    int near = 0;
    for (int q = 0; q < k.ndmrs; q++)
      if (sy[q] <= m) near = q;
    u.est_sym[m] = (int8_t)sy[near];
    u.llr_off[m] = off;
    if (m < 1 || isd) continue;
    u.nre[m] = (uint16_t)(12 * k.nrb);
    off += (uint32_t)(12 * k.nrb) * 2;
    if (first) { u.meas_sym = m; first = false; }
  }
  j->scr = g_zero_scr;
  j->G = off;
  j->A = 500; j->C = 1; j->K = 640; j->Z = 64; j->F = 640 - (500 + 16); j->E = off; j->E2 = off; j->first_rE2 = 1;
  j->BG = 2; j->R = 15; j->R2 = 15; j->rv = 0; j->Qm = 2; j->it_max = 8;
}

/* the host side of a PUSCH pass: writes every block the kernels read (the FEP items, the chest rows, the UlBlk, the transport block tables) */
static int prepare_ul(Inst &I, const nr_gpu_pass_job_t *j)
{
  const nr_gpu_pass_ul_t &u = j->ul;
  const int nbRx = g_cfg.nbRx, nd = j->ndmrs;
  const int ext = (12 * (int)j->shape.nrb_class + 15) & ~15;
  if (!u.on || j->nl != 1 || nd < 1 || nd > NR_GPU_PASS_MAX_DMRS || u.nrb < 1 || u.nrb > (int)j->shape.nrb_class || u.meas_sym < 0 || u.meas_sym >= NSYM || !u.nre[u.meas_sym]) return -1;
  const bool direct = nr_gpu_model() == NR_GPU_MODEL_DIRECT;
  bool redirect = direct;
  for (int a = 0; a < nbRx; a++) redirect = redirect && u.rx[a];
  const int16_t *rxp[4] = {};
  for (int a = 0; a < nbRx; a++) rxp[a] = redirect ? u.rx[a] : (const int16_t *)I.rxF.dev + 2 * (size_t)a * NSYM * N;
  /* FEP items */
  nr_pusch_gpu_fep_item_t *it = (nr_pusch_gpu_fep_item_t *)I.uli.host;
  int *par = (int *)(it + NSYM);
  memset(it, 0, sizeof(*it) * NSYM);
  int32_t base = INT32_MAX, top = 0;
  for (int m = 0; m < NSYM; m++)
    if (u.win[m] >= 0) {
      base = std::min(base, u.win[m]);
      top = std::max(top, u.win[m] + N);
    }
  if (base == INT32_MAX) return -1;
  if (!direct) {
    if (top - base > ULW_LEN || (uint32_t)top > u.iq_len) return -1;
    for (int a = 0; a < nbRx; a++) memcpy((int16_t *)I.ulwin.host + 2 * (size_t)a * ULW_LEN, u.iq[a] + 2 * (size_t)base, (size_t)(top - base) * 4);
  } else if ((uint32_t)top > u.iq_len)
    return -1;
  for (int m = 0; m < NSYM; m++) {
    if (u.win[m] < 0) continue;
    for (int a = 0; a < nbRx; a++) {
      it[m].src[a] = direct ? u.iq[a] + 2 * (size_t)u.win[m] : (const int16_t *)I.ulwin.dev + 2 * ((size_t)a * ULW_LEN + (size_t)(u.win[m] - base));
      it[m].dst[a] = (int16_t *)rxp[a] + 2 * (size_t)m * N;
    }
    it[m].rot[0] = u.rot[m][0];
    it[m].rot[1] = u.rot[m][1];
    it[m].rotate = 1;
  }
  par[0] = u.rot_nb_rb;
  par[1] = u.rot_fco;
  /* chest rows: row = dmrs_index * nbRx + antenna */
  nr_gpu_chest_job_t &cj = *I.cj;
  cj.type = 3;
  int r = 0;
  for (int k = 0; k < nd; k++) {
    int16_t *pil = (int16_t *)I.pil.host + (size_t)(k * 4) * PIL_STRIDE * 2;
    memcpy(pil, u.pil[k], (size_t)6 * u.nrb * 4);
    for (int a = 0; a < nbRx; a++, r++) {
      nr_gpu_chest_row_t &row = cj.rows[r];
      row.rxF = rxp[a] + 2 * ((size_t)N * u.dmrs_sym[k] + u.delta);
      row.pil = (const int16_t *)I.pil.dev + (size_t)(k * 4) * PIL_STRIDE * 2;
      row.out = (int16_t *)I.est.dev + 2 * ((size_t)a * NSYM * N + (size_t)u.dmrs_sym[k] * N);
      row.k0 = u.k0;
      row.nb_rb = u.nrb;
    }
  }
  cj.n_rows = r;
  if (nr_pdsch_gpu_chest_stage_at(&cj, r, I.id) != 0) return -1;
  /* the equaliser block */
  nrul::UlBlk &b = *I.UB();
  memset(&b, 0, sizeof(b));
  b.nrb = u.nrb; b.nre12 = 12 * u.nrb; b.qm = (int)j->Qm; b.nbRx = nbRx; b.keep = u.keep; b.delta = u.delta; b.nd = nd;
  b.meas_sym = u.meas_sym; b.meas_est = u.est_sym[u.meas_sym]; b.meas_len = (u.nre[u.meas_sym] + 15) & ~15; b.ext = ext;
  for (int k = 0; k < 4; k++) b.dsym[k] = k < nd ? u.dmrs_sym[k] : 0;
  for (int m = 0; m < NSYM; m++) {
    b.nre[m] = u.nre[m];
    b.est_sym[m] = u.est_sym[m];
    b.is_dmrs[m] = u.is_dmrs[m];
    b.llr_off[m] = u.llr_off[m];
    if (u.nre[m] > (uint16_t)ext || u.est_sym[m] < 0 || u.est_sym[m] >= NSYM) return -1;
  }
  b.start_re = u.k0;
  b.gate_on = j->gate_on && !j->probe && std::isfinite(j->gate_thr_db);
  b.gate_thr_db = j->gate_thr_db;
  for (int a = 0; a < nbRx; a++) b.rx[a] = (const short2 *)rxp[a];
  b.est = (const short2 *)I.est.dev;
  int16_t *sc = (int16_t *)I.scratch.dev;
  b.comp = (short2 *)sc; b.mag = (short2 *)(sc + I.bn); b.magb = (short2 *)(sc + 2 * I.bn); b.magc = (short2 *)(sc + 3 * I.bn);
  b.llr = (int16_t *)I.llr.dev;
  PassBlk &pb = *I.PB();
  memset(&pb.post, 0, sizeof(pb.post));
  pb.span = 0; pb.norm = 0; pb.nl = pb.qm = 0; /* the UL decoder takes the equaliser's LLRs as they are (no K38 normalisation) */
  const int rc = prepare_tb(I, j);
  b.scr = pb.scr;
  return rc;
}

static int enqueue_ul(Inst &I, const Key &k, cudaStream_t s, int part = 0)
{
  const int nbRx = g_cfg.nbRx, nd = k.ndmrs, ncb = k.ncb, rows = nd * nbRx;
  const int ext = (12 * k.nrb + 15) & ~15;
  const size_t nllr = std::min<size_t>((size_t)LLR_CAP, (size_t)12 * k.nrb * 13 * 8);
  const size_t accoff = ((size_t)3 * NR_GPU_CHEST_MAX_ROWS + 1024) * sizeof(int);
  /* part (Fix A+D gate): 0 = everything; 1 = FEP, chest, k_ul_level and the chest results copy (before the gate); 2 = the IF body (equaliser ... CRC); 3 = the result copy (after the IF) */
  if (part == 3) return res_d2h(I, k.ncb, s) ? -1 : (cudaPeekAtLastError() == cudaSuccess ? 0 : -1);
  if (part != 2 && cudaEventRecordWithFlags(I.evh, s, cudaEventRecordExternal) != cudaSuccess) return -1;
  if (part != 2 && (nr_gpu_buf_h2d_async(&I.pb, 0, sizeof(PassBlk), s) || nr_gpu_buf_h2d_async(&I.ulb, 0, sizeof(nrul::UlBlk), s) ||
                    nr_gpu_buf_h2d_async(&I.pil, 0, (size_t)NR_GPU_PASS_MAX_DMRS * 4 * PIL_STRIDE * 4, s) ||
                    nr_gpu_buf_h2d_async(&I.scr, 0, (size_t)((nllr + 31) / 32) * 4, s) ||
                    nr_gpu_buf_h2d_async(&I.uli, 0, NSYM * sizeof(nr_pusch_gpu_fep_item_t) + 2 * sizeof(int), s) ||
                    nr_gpu_buf_h2d_async(&I.ulwin, 0, (size_t)nbRx * ULW_LEN * 4, s)))
    return -1;
  nrul::UlOut *uo = &I.RBd()->ul;
  if (part != 2) {
    if (nr_gpu_pass_ul_fep_dev(I.uli.dev, (const int *)((const char *)I.uli.dev + NSYM * sizeof(nr_pusch_gpu_fep_item_t)), nbRx, s) != 0) return -1;
    if (nr_pdsch_gpu_chest_dev(I.cj, I.cj->n_rows, &I.est, &I.nvar, I.id, s) != 0) return -1;
    nrul::k_ul_level<<<rows + nbRx, 256, 0, s>>>(I.UBd(), uo);
    if (part == 1) return nr_gpu_buf_d2h_async(&I.nvar, 0, (size_t)rows * 3 * sizeof(int), s) || nr_gpu_buf_d2h_async(&I.nvar, accoff, (size_t)rows * sizeof(uint64_t), s) ? -1 : (cudaPeekAtLastError() == cudaSuccess ? 0 : -1);
  }
  nrul::k_ul_decide<<<1, 32, 0, s>>>(I.UBd(), uo);
  nrul::k_ul_eq<<<dim3((ext + 127) / 128, NSYM), 128, 0, s>>>(I.UBd(), uo);
  nrul::k_ul_llr<<<dim3((ext + 127) / 128, NSYM), 128, 0, s>>>(I.UBd());
  if (nr_gpu_pass_launch_rdm((const int16_t *)I.llr.dev, I.PBd()->rdm, ncb, (int8_t *)I.lin.dev, s) != 0) return -1;
  if (nr_pdsch_gpu_ldpc_pass_dev(I.id, I.lj, ncb, &I.lin, &I.bits, s) != 0) return -1;
  const uint8_t *snaps = (const uint8_t *)I.bits.dev;
  const int *dres = (const int *)((const char *)I.bits.dev + NR_GPU_PASS_LDPC_RES_OFF);
  if (nr_gpu_pass_launch_crc_p(snaps, I.PBd()->cand, ncb * NSNAP, &I.PBd()->tbs, I.RBd()->cand_ok, &I.RBd()->dummy_tb, s) != 0) return -1;
  k_pick<<<1, 128, 0, s>>>(snaps, dres, I.PBd(), I.RBd());
  if (nr_gpu_pass_launch_crc_p((const uint8_t *)I.res.dev, I.PBd()->sel, ncb, &I.PBd()->tbs, I.RBd()->seg_crc, &I.RBd()->tb_crc, s) != 0) return -1;
  if (nr_gpu_buf_d2h_async(&I.llr, 0, nllr * 2, s) || (part == 0 && (nr_gpu_buf_d2h_async(&I.nvar, 0, (size_t)rows * 3 * sizeof(int), s) ||
      nr_gpu_buf_d2h_async(&I.nvar, accoff, (size_t)rows * sizeof(uint64_t), s) || res_d2h(I, k.ncb, s))) ||
      nr_gpu_buf_d2h_async(&I.est, 0, (size_t)nbRx * NSYM * N * 4, s) || nr_gpu_buf_d2h_async(&I.rxF, 0, (size_t)nbRx * NSYM * N * 4, s))
    return -1;
  return cudaPeekAtLastError() == cudaSuccess ? 0 : -1;
}

static bool key_ok_ul(const Key &k)
{
  return g_cfg_set && k.ul && nr_gpu_pass_nrb_class(k.nrb) == k.nrb && nr_gpu_pass_ncb_class(k.ncb) == k.ncb && k.nl == 1 && k.ndmrs >= 1 && k.ndmrs <= NR_GPU_PASS_MAX_DMRS &&
         (int)k.ndmrs * g_cfg.nbRx <= std::min(SEG_ROWS, nr_pdsch_gpu_pass_ls_rows());
}

/* Completion (no polling loop, no busy-spin for the whole pass): the thread sleeps ONCE until shortly before the pass's predicted end (an EMA of the GPU-measured launch->done of this class),
 * spins at most g_spin_us for the tail, and then blocks on the instance's blocking-sync event, whose wake is the driver's. Measured on the DGX (nice-19 bench thread): the blocking wake alone
 * costs ~170 us p50, which is why the prediction sleep keeps the blocking wait as a fallback only. g_spin_us == 0: blocking wait only; < 0: the former poll + 20 us naps (bench only). */
static std::atomic<int> g_spin_us{60};
static std::atomic<uint32_t> g_pred_ns[2][256];
/* bench / test hook, not a runtime switch */
extern "C" void nr_gpu_pass_set_spin_us(int us) { g_spin_us = us; }
static void pred_update(int ul, uint64_t key, uint32_t obs_ns)
{
  std::atomic<uint32_t> &p = g_pred_ns[ul][(key * 0x9E3779B97F4A7C15ull) >> 56];
  const uint32_t o = p.load(std::memory_order_relaxed);
  p.store(o ? (uint32_t)(((uint64_t)o * 7 + obs_ns) / 8) : obs_ns, std::memory_order_relaxed);
}
static void wait_done(Inst &I, int ul, uint64_t key, uint64_t t_launch_ns)
{
  cudaError_t e = cudaEventQuery(I.done);
  const int sp = g_spin_us.load();
  if (e == cudaErrorNotReady && sp < 0) {
    const struct timespec nap = {0, 20000};
    const uint64_t t0 = nr_gpu_pass_now_ns();
    while ((e = cudaEventQuery(I.done)) == cudaErrorNotReady)
      if (nr_gpu_pass_now_ns() - t0 >= (uint64_t)(-sp) * 1000ull) nanosleep(&nap, nullptr);
  } else if (e == cudaErrorNotReady) {
    static thread_local bool slack = false;
    if (!slack) { prctl(PR_SET_TIMERSLACK, 1000UL, 0, 0, 0); slack = true; } /* 1 us instead of the default 50 us wake slack of this thread's timers */
    const uint32_t pred = sp > 0 ? g_pred_ns[ul][(key * 0x9E3779B97F4A7C15ull) >> 56].load(std::memory_order_relaxed) : 0;
    if (sp > 0) {
      /* sleep once to sp us before the predicted end, spin to 100 us after it (an unseen class: spin up to 500 us), then block on the event for outliers */
      const uint64_t now = nr_gpu_pass_now_ns();
      const uint64_t tgt = pred ? t_launch_ns + pred - (uint64_t)sp * 1000ull : 0, end = t_launch_ns + (pred ? (uint64_t)pred + 100000ull : 500000ull);
      if (tgt > now + 20000) {
        const struct timespec d = {(time_t)((tgt - now) / 1000000000ull), (long)((tgt - now) % 1000000000ull)};
        nanosleep(&d, nullptr);
      }
      while ((e = cudaEventQuery(I.done)) == cudaErrorNotReady && nr_gpu_pass_now_ns() < end) {}
    }
    if (e == cudaErrorNotReady) e = cudaEventSynchronize(I.done);
  }
  if (e != cudaSuccess) g_dead = 1;
}

static Key key_of(const nr_gpu_pass_job_t *j)
{
  const nr_gpu_demod_job_t &d = j->dj;
  return Key{j->shape.nrb_class, j->shape.nl, j->shape.ndmrs, j->shape.ncb_class, j->shape.fp32, (uint8_t)(d.want_evm != 0),
             (uint8_t)(j->shape.nl >= 2 ? d.qam_mode : 0), (uint8_t)(j->shape.nl == 1 && d.align != 0),
             (uint8_t)j->shape.nseg_class, 0, 0, (uint8_t)((j->Z & 3) != 0)};
}

/* the segment geometry of a job: segments tile [0, nrb) in data order, their pilots and first subcarriers lie inside the buffers */
static bool seg_ok(const nr_gpu_pass_job_t *j)
{
  if (!j->nseg) return true;
  if (j->seg_pil_nrb < 1 || 6 * j->seg_pil_nrb > PIL_STRIDE) return false;
  int tot = 0;
  for (int s = 0; s < j->nseg; s++) {
    const auto &g = j->seg[s];
    if (g.n_prb < 1 || g.data_index != tot || g.pil_rb < 0 || g.pil_rb + g.n_prb > j->seg_pil_nrb || g.k0 < 0 || g.k0 >= N || 12 * (tot += g.n_prb) > N) return false;
  }
  return tot == j->nrb;
}

/* the chest rows of a class in C chunks (v2 SEG128): rows are (DM-RS symbol, layer) groups of nseg x nbRx rows; a chunk takes whole groups, at most min(SEG_ROWS, LS rows)
 * rows (the scratch rows and the LS scratch are reused per chunk), the whole job at most NR_GPU_CHEST_MAX_ROWS rows (staging, delay / nvar area). One chunk = the pre-SEG128 C.
 * Chunks > 1 only in split mode. Returns the groups per chunk, 0 = the class does not fit. */
static int c_groups_per_chunk(const Key &k)
{
  const int grp = (k.nseg ? k.nseg : 1) * g_cfg.nbRx, ng = (int)k.ndmrs * k.nl, cap = std::min(SEG_ROWS, nr_pdsch_gpu_pass_ls_rows());
  if (ng * grp <= cap) return ng;
  if (!g_cfg.split || grp > cap || ng * grp > NR_GPU_CHEST_MAX_ROWS) return 0;
  return cap / grp;
}
static bool key_ok(const Key &k)
{
  return g_cfg_set && nr_gpu_pass_nrb_class(k.nrb) == k.nrb && nr_gpu_pass_ncb_class(k.ncb) == k.ncb && k.nl >= 1 && k.nl <= 4 && k.nl <= g_cfg.nbRx &&
         k.ndmrs >= 1 && k.ndmrs <= NR_GPU_PASS_MAX_DMRS && k.qmode <= 3 && (k.nseg == 0 || (nr_gpu_pass_nseg_class(k.nseg) == k.nseg && k.nseg <= (g_cfg.split ? NR_GPU_PASS_MAX_SEG : NR_GPU_PASS_MAX_SEG_FULL))) &&
         c_groups_per_chunk(k) > 0;
}

/* v2 Task 3 (split) bookkeeping: sub-graphs cached per kind, pass memory (instance buffers + split graphs, cudaMemGetInfo deltas), warm time, cap refusals, unseen per kind */
/* g_split_unseen[sub]: one count per missing sub-graph of a refused call (a class missing two sub-graphs counts in both; review N5).
 * Memory (review S3), an ESTIMATE: g_inst_mb = the explicit sum of the instance buffers' sizes (other modules' per-instance contexts not included); g_split_kb = the
 * cudaMemGetInfo deltas around graph instantiation + first replay only (device-wide, racy with other allocators). g_sub_kb_max: the largest cost of one sub-graph of each kind
 * (all instances), the worst case a class is checked against before any of its sub-graphs is built (review S4: no orphan sub-graph when the cap is hit mid-class). */
static std::atomic<uint64_t> g_split_n[4], g_split_unseen[4], g_split_mb{0}, g_inst_mb{0}, g_split_warm_us{0}, g_split_capped{0}, g_split_kb{0};
static uint64_t g_sub_kb_max[4];
/* The memory cap of the split graphs: nr_gpu_pass_cap_mb (a quarter of the device's memory between 2048 and 3072 MB, at most 30 % of the memory free at the platform probe; 8 GB card: 2048; the GB10's unified memory: 3072). */
static size_t g_pinned_b; /* g_pinned_b: pinned host mirrors of the instance buffers (DISCRETE only) */
static int split_cap_auto()
{
  size_t fr = 0, tb = 0;
  if (cudaMemGetInfo(&fr, &tb) != cudaSuccess) tb = 0;
  int cap = nr_gpu_pass_cap_mb(tb ? tb : (size_t)8 << 30, nr_gpu_platform_free_at_probe()); /* no device info: the 8 GB card's value */
  /* T6b: the shared budget. The cap covers the instances plus the graphs; the graphs get their share plus what the other modules left unused
   * (unlimited / large budgets: SIZE_MAX or above the first term, so GB10 keeps 3072). */
  const size_t gb = nr_gpu_membudget_graph_bytes();
  if (gb != SIZE_MAX) {
    const uint64_t b = (gb >> 20) + g_inst_mb.load();
    if (b < (uint64_t)cap) cap = (int)(b ? b : 1);
  }
  return cap;
}
static int split_cap_mb() { return g_cfg.split_mb > 0 ? g_cfg.split_mb : split_cap_auto(); /* measured 0.55 MB per graph executable (the gated D / L wrappers included): 5 DL-shape executables (4 real + overflow) per class need ~1.5 GB for the full warm set of the unit test (was 1008 MB with 4); production warms fewer classes */ }
static bool split_cap_hit(uint64_t extra_kb)
{
  if (((g_inst_mb << 10) + g_split_kb + extra_kb) >> 10 < (uint64_t)split_cap_mb()) return false;
  if (g_split_capped++ == 0) fprintf(stderr, "GPUGRAPH split: memory cap %d MB reached (instances %lu + graphs %lu MB, estimate): warming stops\n", split_cap_mb(), (unsigned long)g_inst_mb.load(), (unsigned long)g_split_mb.load());
  return true;
}

/* capture + instantiate + one replay (pre-fence) of a class (sub = SUB_FULL) or of one sub-graph of it on every instance. Under g_mu. */
/* ---- Fix A+D: graphs with the in-graph no-chance gate. The driver entry points (no -lcuda) for a conditional node in a green-context graph: the handle and the node need the
 * green context's CUcontext (with the primary context cudaGraphInstantiate refuses the graph). ---- */
struct GateDrv {
  void *stream_ctx = nullptr; /* cuStreamGetCtx, called through nr_stream_ctx */
  CUresult (*from_green)(CUcontext *, CUgreenCtx) = nullptr;
  CUresult (*hcreate)(CUgraphConditionalHandle *, CUgraph, CUcontext, unsigned, unsigned) = nullptr;
  CUresult (*addnode)(CUgraphNode *, CUgraph, const CUgraphNode *, const CUgraphEdgeData *, size_t, CUgraphNodeParams *) = nullptr;
  bool ok = false;
  const char *missing = nullptr; /* the first entry point the driver lacks */
};
static const GateDrv &gate_drv()
{
  static const GateDrv d = [] {
    GateDrv g;
    void *f[4] = {};
    static const char *const nm[4] = {"cuStreamGetCtx", "cuCtxFromGreenCtx", "cuGraphConditionalHandleCreate", "cuGraphAddNode"};
    static const int ver[4] = {NR_DRV_VER_STREAM_CTX, NR_DRV_VER_STREAM_CTX, NR_DRV_VER_COND_HANDLE, NR_DRV_VER_GRAPH_ADD_NODE};
    for (int i = 0; i < 4; i++)
      if (!(f[i] = nr_drv_sym(nm[i], ver[i])) && !g.missing && i != 1) g.missing = nm[i];
    g.stream_ctx = f[0]; g.from_green = (decltype(g.from_green))f[1]; g.hcreate = (decltype(g.hcreate))f[2]; g.addnode = (decltype(g.addnode))f[3];
    g.ok = f[0] && f[2] && f[3]; /* cuCtxFromGreenCtx (12.4+) is only needed for a stream in a green context, which the toolkit may not have */
    return g;
  }();
  return d;
}
/* test hook (nr_gpu_pass_test_no_gate_drv): behave as a driver without the conditional-node entry points */
static std::atomic<int> g_test_no_gate{0};
static bool gate_ok() { return !g_test_no_gate && gate_drv().ok; }
/* sub: SUB_D / SUB_L (a split DL sub-graph) or -1 (the PUSCH pass). Captures  [part 1] -> gate kernel -> IF(run){ [part 2] } -> [part 3]  into one graph; the IF body is a captured child graph. */
static int g_gate_step = 0;
static int capture_gated(Inst &I, const Key &k, const nr_gpu_pass_job_t *sj, int sub, cudaGraph_t *out)
{
  const GateDrv &d = gate_drv();
  g_gate_step = 0;
  if (!gate_ok()) { g_gate_step = 20; return -1; }
  cudaStream_t s = I.s;
  auto part = [&](int p) { return sub < 0 ? enqueue_ul(I, k, s, p) : enqueue(I, k, sj, s, sub, p); };
  cudaGraph_t body = nullptr, w = nullptr;
  if (cudaStreamBeginCapture(s, cudaStreamCaptureModeRelaxed) != cudaSuccess) return -1;
  const int e2 = part(2);
  if (cudaStreamEndCapture(s, &body) != cudaSuccess || e2 != 0 || !body) { if (body) cudaGraphDestroy(body); g_gate_step = 21; return -1; }
  if (cudaStreamBeginCapture(s, cudaStreamCaptureModeRelaxed) != cudaSuccess) { cudaGraphDestroy(body); return -1; }
  int rc = -1;
  do {
    cudaStreamCaptureStatus st;
    unsigned long long id;
    cudaGraph_t cg = nullptr;
    const cudaGraphNode_t *deps = nullptr;
    const cudaGraphEdgeData *ed = nullptr;
    size_t nd = 0;
    CUcontext cx = nullptr;
    CUgreenCtx gc = nullptr;
    if (nr_capture_info(s, &st, &id, &cg, &deps, &ed, &nd) != cudaSuccess || nr_stream_ctx(d.stream_ctx, (CUstream)s, &cx, &gc) != CUDA_SUCCESS) { g_gate_step = 1; break; }
    if (gc && (!d.from_green || d.from_green(&cx, gc) != CUDA_SUCCESS)) { g_gate_step = 2; break; }
    CUgraphConditionalHandle h;
    if (d.hcreate(&h, (CUgraph)cg, cx, 1, CU_GRAPH_COND_ASSIGN_DEFAULT) != CUDA_SUCCESS) { g_gate_step = 3; break; }
    if (part(1) != 0) { g_gate_step = 4; break; }
    if (sub == SUB_D) k_gate_d<<<1, 256, 0, s>>>(I.PBd(), I.RBd(), (cudaGraphConditionalHandle)h);
    else if (sub == SUB_L) k_gate_l<<<1, 32, 0, s>>>(I.RBd(), (cudaGraphConditionalHandle)h);
    else k_gate_ul<<<1, 32, 0, s>>>(I.UBd(), I.RBd(), (const uint64_t *)((const int *)I.nvar.dev + 3 * NR_GPU_CHEST_MAX_ROWS) + 512, (cudaGraphConditionalHandle)h);
    if (cudaGetLastError() != cudaSuccess || nr_capture_info(s, &st, &id, &cg, &deps, &ed, &nd) != cudaSuccess) { g_gate_step = 5; break; }
    CUgraphNodeParams np = {};
    np.type = CU_GRAPH_NODE_TYPE_CONDITIONAL;
    np.conditional.handle = h;
    np.conditional.type = CU_GRAPH_COND_TYPE_IF;
    np.conditional.size = 1;
    np.conditional.ctx = cx;
    CUgraphNode cn;
    if (d.addnode(&cn, (CUgraph)cg, (const CUgraphNode *)deps, (const CUgraphEdgeData *)ed, nd, &np) != CUDA_SUCCESS) { g_gate_step = 6; break; }
    cudaGraphNode_t cb;
    if (cudaGraphAddChildGraphNode(&cb, (cudaGraph_t)np.conditional.phGraph_out[0], nullptr, 0, body) != cudaSuccess) { g_gate_step = 7; break; }
    cudaGraphNode_t cn2 = (cudaGraphNode_t)cn;
    if (nr_capture_set_deps(s, &cn2, nullptr, 1) != cudaSuccess) { g_gate_step = 8; break; }
    if (part(3) != 0) { g_gate_step = 9; break; }
    rc = 0;
  } while (0);
  const cudaError_t ee = cudaStreamEndCapture(s, &w);
  cudaGraphDestroy(body);
  if (rc != 0 || ee != cudaSuccess || !w) { if (w) cudaGraphDestroy(w); return -1; }
  *out = w;
  return 0;
}

static int build(const Key &k, int sub = SUB_FULL)
{
  NrMsScope ms_(NR_MS_PASS_GRAPH, NR_MS_SHARED); /* GPUMEM: graph executables are class-shaped, shared by all cells */
  if (!guard_alloc()) return -1;
  if (sub != SUB_FULL && split_cap_hit(0)) return -1; /* the memory cap: stop warming, the key stays unseen (stage path, counted) */
  const auto tb0 = std::chrono::steady_clock::now();
  uint64_t kb = 0; /* split: cudaMemGetInfo deltas around instantiation + first replay (an estimate) */
  Entry e;
  nr_gpu_pass_job_t *sj = (nr_gpu_pass_job_t *)malloc(sizeof(*sj));
  if (!sj) return -1;
  std::vector<int16_t> geo;
  if (k.ul) synth_job_ul(k, sj);
  else synth_job(k, sj, geo);
  int rc = 0;
  const unsigned pools = k.ul ? (M_UL | M_PROBE) : (M_DL | M_PROBE); /* a class gets executables only on the instances of the pools that may run it */
  for (int i = 0; i < NI && !rc; i++) {
    if (!(pools >> i & 1u)) continue;
    Inst &I = g_inst[i];
    memset(I.res.host, 0, sizeof(ResBlk));
    if ((k.ul ? prepare_ul(I, sj) : prepare(I, sj)) != 0) { rc = -1; break; }
    cudaGraph_t g = nullptr;
    int er = 0;
    cudaError_t ee = cudaSuccess;
    if (k.ul || sub == SUB_D || sub == SUB_L) { /* gated: the no-chance verdict decides on the device whether the equaliser / demod, LDPC and CRC nodes run */
      er = capture_gated(I, k, sj, k.ul ? -1 : sub, &g);
    } else {
      if (cudaStreamBeginCapture(I.s, cudaStreamCaptureModeRelaxed) != cudaSuccess) { rc = -1; break; }
      er = enqueue(I, k, sj, I.s, sub);
      ee = cudaStreamEndCapture(I.s, &g);
    }
    if (er != 0 || ee != cudaSuccess || !g) {
      fprintf(stderr, "nr_gpu_pass: capture of class nrb %u nl %u ndmrs %u ncb %u nseg %u ul %u sub %d failed (enqueue %d, gate step %d, end %s, last %s)\n", k.nrb, k.nl, k.ndmrs, k.ncb, k.nseg, k.ul, sub, er, g_gate_step,
              cudaGetErrorString(ee), cudaGetErrorString(cudaGetLastError()));
      if (g) cudaGraphDestroy(g);
      rc = -1;
      break;
    }
    ++g_allocs;
    size_t f0 = 0, f1 = 0, tb_ = 0;
    if (sub != SUB_FULL) cudaMemGetInfo(&f0, &tb_);
    if (cudaGraphInstantiate(&e.exec[i], g, 0) != cudaSuccess) rc = -1;
    cudaGraphDestroy(g);
    if (rc) break;
    /* the first replay allocates inside the driver / loads the kernels: do it now (pre-fence). Split: the parameter blocks are not in the sub-graphs (h2d_params) */
    if (cudaEventRecord(I.ev0, I.s) != cudaSuccess || (sub != SUB_FULL && h2d_params(I, k, I.s, true)) || cudaGraphLaunch(e.exec[i], I.s) != cudaSuccess || cudaEventRecord(I.done, I.s) != cudaSuccess ||
        cudaEventSynchronize(I.done) != cudaSuccess /* the blocking wait and the event timing once, pre-fence: their driver state is allocated here, not on the pass path */ || cudaStreamSynchronize(I.s) != cudaSuccess) {
      fprintf(stderr, "nr_gpu_pass: warm replay of class nrb %u nl %u failed: %s\n", k.nrb, k.nl, cudaGetErrorString(cudaGetLastError()));
      rc = -1;
    }
    { float ms; cudaEventElapsedTime(&ms, I.ev0, I.done); cudaGetLastError(); }
    if (sub != SUB_FULL && cudaMemGetInfo(&f1, &tb_) == cudaSuccess && f0 > f1) kb += (uint64_t)(f0 - f1) >> 10;
  }
  free(sj);
  if (rc) {
    for (int i = 0; i < NI; i++)
      if (e.exec[i]) cudaGraphExecDestroy(e.exec[i]);
    return -1;
  }
  g_cache[key_sub(k, sub).pack()] = e;
  nr_gpu_memstat_note_graph();
  if (sub != SUB_FULL && kb) nr_gpu_memstat_note(NR_MS_DEV, (size_t)kb << 10); /* the cudaMemGetInfo delta of the instantiate + first replay (a whole-pass graph is not measured) */
  if (k.nseg && (sub == SUB_FULL || sub == SUB_C)) ++g_seg_classes;
  {
    size_t fr = 0, tot_b = 0; /* build() runs before the fence only (guard_alloc) */
    if (cudaMemGetInfo(&fr, &tot_b) == cudaSuccess) {
      g_used_mb = ((tot_b - fr) >> 20) - g_base_mb; /* delta from the start of the warm-up (the device is shared) */
    }
  }
  if (sub != SUB_FULL) { /* summed as KB so that small graphs are not lost to MB rounding */
    g_split_kb += kb;
    g_split_mb = g_split_kb >> 10;
    g_sub_kb_max[sub] = std::max(g_sub_kb_max[sub], kb);
    ++g_split_n[sub];
    g_split_warm_us += (uint64_t)std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - tb0).count();
  }
  return 0;
}

/* the green-context stream index of an instance: the DL pool (id), the UL pool (NR_GREEN_UL_IDX + id), the probe pool (NR_GREEN_PROBE_IDX + id) */
static int pass_green_idx(int id) { return id < NR_GPU_PASS_DL ? id : id < NR_GPU_PASS_DL + NR_GPU_PASS_UL ? NR_GREEN_UL_IDX + id : NR_GREEN_PROBE_IDX + id; }
static int alloc_inst(int id)
{
  Inst &I = g_inst[id];
  I.id = id;
  int lo = 0, hi = 0;
  cudaDeviceGetStreamPriorityRange(&lo, &hi);
  const size_t ext = EXTMAX;
  I.bn = (size_t)2 * NSYM * NR_GPU_DEMOD_MAX_NL * ext * 2;
  const int prio = id < NR_GPU_PASS_DL ? hi : lo; /* real DL grants highest; UL and probe pools lowest (see the pool comment at the top) */
  if ((!nr_gpu_green_stream(&I.s, prio, true, pass_green_idx(id)) && cudaStreamCreateWithPriority(&I.s, cudaStreamNonBlocking, prio) != cudaSuccess) ||
      (!nr_gpu_green_stream(&I.s2, prio, true, pass_green_idx(id)) && cudaStreamCreateWithPriority(&I.s2, cudaStreamNonBlocking, prio) != cudaSuccess) ||
      cudaEventCreateWithFlags(&I.done, cudaEventBlockingSync) != cudaSuccess || cudaEventCreateWithFlags(&I.ev0, cudaEventDefault) != cudaSuccess ||
      cudaEventCreateWithFlags(&I.evh, cudaEventDefault) != cudaSuccess || cudaEventCreateWithFlags(&I.evC, cudaEventDefault) != cudaSuccess ||
      cudaEventCreateWithFlags(&I.evD, cudaEventDefault) != cudaSuccess) /* blocking sync: the wake is the driver's, not a poll loop */
    return -1;
  if (g_cfg.v2 && nr_pdsch_gpu_prio_on() && !nr_pdsch_gpu_low_stream(&I.s_lo, lo, hi, pass_green_idx(id))) I.s_lo = nullptr; /* v2 Task 2: probes on the twin, in the instance's own green context (a graph captured on I.s cannot launch in another); none = the RT stream */
  if (nr_gpu_buf_alloc(&I.rxF, (size_t)4 * NSYM * N * 4) || nr_gpu_buf_alloc(&I.est, (size_t)16 * NSYM * N * 4) ||
      nr_gpu_buf_alloc(&I.nvar, NR_GPU_CHEST_DEV_BYTES) || nr_gpu_buf_alloc(&I.geo, (size_t)2 * NSYM * ext * 4) ||
      nr_gpu_buf_alloc_dev(&I.scratch, 4 * I.bn * 2) || nr_gpu_buf_alloc(&I.llr, (size_t)LLR_CAP * 2) || /* scratch, lin, segsc: device intermediates the host never reads (DISCRETE: no pinned mirror) */
      nr_gpu_buf_alloc_dev(&I.lin, NR_GPU_PASS_LDPC_IN_BYTES) || nr_gpu_buf_alloc(&I.bits, NR_GPU_PASS_LDPC_BITS_BYTES) ||
      nr_gpu_buf_alloc(&I.pil, (size_t)NR_GPU_PASS_MAX_DMRS * 4 * PIL_STRIDE * 4) || nr_gpu_buf_alloc(&I.scr, (size_t)(LLR_CAP / 32 + 2) * 4) ||
      nr_gpu_buf_alloc(&I.pb, sizeof(PassBlk)) || nr_gpu_buf_alloc(&I.res, sizeof(ResBlk)) ||
      nr_gpu_buf_alloc_dev(&I.segsc, (size_t)SEG_ROWS * N * 4) ||
      (nr_gpu_model() == NR_GPU_MODEL_DISCRETE && nr_gpu_buf_alloc(&I.gath, (size_t)(3 * 4 + 1) * N * 4)) ||
      nr_gpu_buf_alloc(&I.uli, NSYM * sizeof(nr_pusch_gpu_fep_item_t) + 64) || nr_gpu_buf_alloc(&I.ulb, sizeof(nrul::UlBlk)) ||
      nr_gpu_buf_alloc(&I.ulwin, (size_t)4 * ULW_LEN * 4))
    return -1;
  for (auto &v : I.sig) v.resize(SIG_CAP);
  I.cj = (nr_gpu_chest_job_t *)calloc(1, sizeof(nr_gpu_chest_job_t));
  I.lj = (nr_gpu_ldpc_job_t *)calloc(MAXCB, sizeof(nr_gpu_ldpc_job_t));
  if (!I.cj || !I.lj) return -1;
  nr_gpu_chest_job_t &cj = *I.cj;
  cj.type = 1; cj.N = N; cj.dt_stride = g_cfg.dt_stride; cj.max_delay_comp = g_cfg.max_delay_comp; cj.delay_table = g_cfg.delay_table;
  memcpy(cj.filt[0], filt16_ul_p0, 32);
  memcpy(cj.filt[1], filt16_ul_p1p2, 32);
  memcpy(cj.filt[2], filt16_ul_middle, 32);
  memcpy(cj.filt[3], filt16_ul_last, 32);
  return 0;
}

/* Task 5: the FP32 demod instantiation is the default (RTX 4070/4060 Ti: FP64 at 1/64 rate); the tests switch to the bit-exact double one */
static int g_fp32 = 1;
static int g_fp32_default() { return g_fp32; }
extern "C" void nr_gpu_pass_test_fp32(int on) { g_fp32 = on; }
extern "C" void nr_gpu_pass_test_sized(int on) { g_sized = on; }
extern "C" void nr_gpu_pass_test_no_gate_drv(int on) { g_test_no_gate = on; }
extern "C" int nr_gpu_pass_fp32_default(void) { return g_fp32_default(); }

/* default warm set (max_classes): nrb 24/48/96/217 x nl 1..4, 2 DM-RS symbols, a typical padded block count, float */
static Key default_class(int i)
{
  static const int nrb[4] = {24, 48, 96, 217};
  const int n = nrb[i / 4], l = i % 4 + 1;
  int c = n * l / 12;
  if (c < 1) c = 1;
  return Key{(uint16_t)nr_gpu_pass_nrb_class(n), (uint16_t)l, 2, (uint16_t)nr_gpu_pass_ncb_class(c > 32 ? 32 : c), (uint8_t)(1 | (g_fp32_default() ? NR_GPU_PASS_FP32_DEMOD : 0)), 0, (uint8_t)(l >= 2 ? 3 : 0), 0, 0};
}
} // namespace nrpass

using namespace nrpass;

extern "C" int nr_gpu_pass_config(const nr_gpu_pass_cfg_t *c)
{
  if (!c || c->nbRx < 1 || c->nbRx > 4 || !c->delay_table || c->dt_stride < N || c->max_delay_comp < 1) return -1;
  std::lock_guard<std::mutex> lk(g_mu);
  if (g_ready) return 0;
  g_cfg = *c;
  g_cfg_set = true;
  return 0;
}

/* HARQ blocks <-> the host layout of the CPU store (block r at d + r * kcz, n = N entries): rare fallbacks between the GPU and the CPU decoder; the blocks are owned by the caller (its entry is busy) */
static int hq_copy(const uint16_t *blk, int16_t *d, int C, int kcz, int n, bool up)
{
  if (!g_hq || !blk || !d || C < 1 || C > MAXCB || n < 1 || n > NR_GPU_HQ_SEG) return -1;
  for (int r = 0; r < C; r++) {
    if (blk[r] >= NR_GPU_HQ_BLK) return -1;
    int16_t *dev = g_hq + (size_t)blk[r] * NR_GPU_HQ_SEG;
    if ((up ? cudaMemcpy(dev, d + (size_t)r * kcz, (size_t)n * 2, cudaMemcpyHostToDevice) : cudaMemcpy(d + (size_t)r * kcz, dev, (size_t)n * 2, cudaMemcpyDeviceToHost)) != cudaSuccess) return -1;
  }
  return 0;
}
extern "C" int nr_gpu_pass_hq_up(const uint16_t *blk, const int16_t *d, int C, int kcz, int n) { return hq_copy(blk, const_cast<int16_t *>(d), C, kcz, n, true); }
extern "C" int nr_gpu_pass_hq_down(const uint16_t *blk, int16_t *d, int C, int kcz, int n) { return hq_copy(blk, d, C, kcz, n, false); }

extern "C" int nr_gpu_pass_init(int max_classes)
{
  if (!nr_gpu_module_ready()) return -1; // CPU tier (no device, arch mismatch or NR_GPU_FORCE_MODEL=cpu): refuse, the host keeps the CPU path
  std::lock_guard<std::mutex> lk(g_mu);
  if (g_ready) return 0;
  { size_t fr = 0, tb = 0; if (cudaMemGetInfo(&fr, &tb) == cudaSuccess) g_base_mb = (tb - fr) >> 20; }
  if (!g_cfg_set) return -1;
  if (!gate_ok()) { /* no conditional graph nodes (driver or toolkit): the pass is off, once; the receiver runs the non-pass GPU / CPU path */
    static std::atomic<int> logged{0};
    if (!logged.exchange(1)) fprintf(stderr, "GPUPASS: graph conditional nodes unavailable (driver/toolkit%s%s) -- pass off\n", gate_drv().missing ? ", no " : "", gate_drv().missing ? gate_drv().missing : "");
    return -1;
  }
  if (!guard_alloc()) return -1;
  NrMsScope ms_(NR_MS_PASS, NR_MS_SHARED); /* GPUMEM: pass instances, rings; the HARQ store below is PER_CELL */
  for (int a = 0; a < 4; a++)
    if (nr_gpu_buf_alloc(&g_ring[a], (size_t)RING_LEN * 4)) return -1;
  if (nr_gpu_buf_alloc(&g_rot, 64 * 4)) return -1;
  static_assert(NR_GPU_HQ_BLK == NR_GPU_CELL_HQ_BLK && NR_GPU_HQ_SEG == NR_GPU_CELL_HQ_SEG, "nr_gpu_cell.h HARQ sizing");
  {
    NrMsScope hq_(NR_MS_PASS, NR_MS_PER_CELL); /* the HARQ soft store holds the state of one cell (G3: one per cell) */
    if (nr_ms_malloc(&g_hq, (size_t)NR_GPU_HQ_BLK * NR_GPU_HQ_SEG * 2) != cudaSuccess) return -1; /* 104 MB: the HARQ soft store counts against the pass's VRAM */
  }
  cudaMemset(g_hq, 0, (size_t)NR_GPU_HQ_BLK * NR_GPU_HQ_SEG * 2);
  SEG_ROWS = g_cfg.split ? 1024 : 512; /* split: 32 segments x 2 DM-RS x 4 layers x 4 RX */
  uint64_t ib = 0; /* review S3: the explicit sum of the pass's own buffers (an estimate: the stage modules' per-instance contexts are not in it) */
  for (int i = 0; i < NI; i++) {
    if (alloc_inst(i) != 0) return -1;
    const Inst &I = g_inst[i];
    for (const nr_gpu_buf_t *b : {&I.rxF, &I.est, &I.nvar, &I.geo, &I.scratch, &I.llr, &I.lin, &I.bits, &I.pil, &I.scr, &I.pb, &I.res, &I.segsc, &I.uli, &I.ulb, &I.ulwin}) {
      ib += b->bytes;
      if (b->host && b->model == NR_GPU_MODEL_DISCRETE) g_pinned_b += b->bytes;
    }
  }
  g_inst_mb = ib >> 20;
  nr_gpu_membudget_commit(NR_GPU_MEM_PASS, ib + (size_t)NR_GPU_HQ_BLK * NR_GPU_HQ_SEG * 2, 1.0);
  if (g_cfg.split && g_inst_mb >= (uint64_t)split_cap_mb()) { /* review S5: no room for any sub-graph: the single-graph (v1 class) mode; the host reads nr_gpu_pass_split_on() */
    fprintf(stderr, "GPUGRAPH split: the pass instances alone (%lu MB) reach the cap %d MB: split mode off\n", (unsigned long)g_inst_mb.load(), split_cap_mb());
    g_cfg.split = 0;
    SEG_ROWS = 512;
  }
  g_ready = 1;
  const int n = max_classes > 16 ? 16 : max_classes;
  for (int i = 0; i < n; i++) {
    const Key k = default_class(i);
    if (k.nl > g_cfg.nbRx || g_cache.count(k.pack())) continue;
    if (build(k) != 0) return -1;
  }
  return 0;
}

extern "C" int nr_gpu_pass_warm(const nr_gpu_pass_shape_t *sh, int qmode, int align, int evm)
{
  std::lock_guard<std::mutex> lk(g_mu);
  if (!g_ready || !sh) return -1;
  if (sh->ul) { /* Task 7: a PUSCH class */
    const Key ku{sh->nrb_class, 1, sh->ndmrs, sh->ncb_class, 0, 0, 0, 0, 0, 1};
    if (!key_ok_ul(ku)) return -1;
    if (g_cache.count(ku.pack())) return 0;
    return build(ku);
  }
  const Key k{sh->nrb_class, sh->nl, sh->ndmrs, sh->ncb_class, sh->fp32, (uint8_t)(evm != 0), (uint8_t)(sh->nl >= 2 ? qmode : 0), (uint8_t)(sh->nl == 1 && align),
                (uint8_t)sh->nseg_class, 0, 0, (uint8_t)(sh->nz4 != 0)};
  if (k.nz4 && k.ncb != 1) return -1; /* a lifting size that is not a multiple of 4 only occurs below 700 bits: one code block */
  if (!key_ok(k) || (int)k.nseg > (int)k.nrb) return -1;
  if (g_cfg.split) { /* the three sub-graphs of the class; the ones another class already built are shared */
    uint64_t need = 0; /* review S4: the worst case of the missing ones, checked before any is built (no orphan sub-graph at the cap) */
    bool missing = false;
    for (int sub = SUB_C; sub <= SUB_L; sub++)
      if (!g_cache.count(key_sub(k, sub).pack())) {
        missing = true;
        need += g_sub_kb_max[sub];
      }
    if (missing && split_cap_hit(need)) return -1;
    for (int sub = SUB_C; sub <= SUB_L; sub++)
      if (!g_cache.count(key_sub(k, sub).pack()) && build(k, sub) != 0) return -1;
    return 0;
  }
  if (g_cache.count(k.pack())) return 0;
  return build(k);
}

/* split mode after nr_gpu_pass_init (0 = off: not requested, or the instances alone reach the memory cap, review S5) */
extern "C" int nr_gpu_pass_split_on(void) { return g_cfg.split; }

/* test hook only (review N4): switch the split mode (the sub-graph caches and the single-graph cache are separate, so both modes can be compared in one process); not safe while
 * passes run in other threads */
extern "C" void nr_gpu_pass_test_split(int on)
{
  std::lock_guard<std::mutex> lk(g_mu);
  g_cfg.split = on;
}

extern "C" void nr_gpu_pass_counters(uint64_t o[16])
{
  memset(o, 0, 16 * sizeof(uint64_t));
  o[0] = g_launches; o[1] = g_hits; o[2] = g_fb_unseen; o[3] = g_fb_busy; o[4] = g_fb_unsup; o[5] = g_fb_err; o[6] = g_need_cpu; o[7] = g_tb_fail;
  o[8] = NI; o[9] = g_cache.size(); o[10] = g_allocs; o[11] = g_fb_ldpckey; o[12] = g_seg_launches; o[13] = g_seg_classes; o[14] = g_fb_unseen_seg; o[15] = g_fb_busy_probe;
}

extern "C" void nr_gpu_pass_stats(char *buf, int n)
{
  uint64_t o[16];
  nr_gpu_pass_counters(o);
  uint64_t tot = 0, acc = 0;
  for (auto &h : g_wait_hist) tot += h;
  int p50 = 0;
  for (int i = 0; i < 32; i++) {
    acc += g_wait_hist[i];
    if (acc * 2 >= tot && tot) { p50 = i; break; }
  }
  snprintf(buf, n, "GPUPASS launches=%lu graph_hits=%lu fallback=%lu (real grants; refused probes are probe_busy) (unseen=%lu (seg %lu) busy=%lu unsup=%lu err=%lu ldpc_key=%lu) need_cpu=%lu tb_fail=%lu classes=%lu (seg %lu) seg_launches=%lu instances=%lu wait_us_p50<=%d gpu_delta_mb=%lu prep_us[p50<=%lu p99<=%lu] run_us[p50<=%lu p99<=%lu] post_us[p50<=%lu p99<=%lu] probe_busy=%lu ovf_takes=%lu ovf_waits=%lu gate_skips=%lu gstart_us[p50<=%lu p99<=%lu] gspan_us[p50<=%lu p99<=%lu]",
           (unsigned long)o[0], (unsigned long)o[1], (unsigned long)(o[2] + o[3] + o[4] + o[5] + o[11]), (unsigned long)o[2], (unsigned long)o[14], (unsigned long)o[3], (unsigned long)o[4],
           (unsigned long)o[5], (unsigned long)o[11], (unsigned long)o[6], (unsigned long)o[7], (unsigned long)o[9], (unsigned long)o[13], (unsigned long)o[12], (unsigned long)o[8],
           tot ? 1 << p50 : 0, (unsigned long)g_used_mb.load(), (unsigned long)stamp_q(0, 0, 50), (unsigned long)stamp_q(0, 0, 99), (unsigned long)stamp_q(0, 1, 50),
           (unsigned long)stamp_q(0, 1, 99), (unsigned long)stamp_q(0, 2, 50), (unsigned long)stamp_q(0, 2, 99), (unsigned long)o[15], (unsigned long)g_ovf_takes.load(), (unsigned long)g_ovf_waits.load(), (unsigned long)g_gate_skips.load(), (unsigned long)stamp_q(0, 3, 50), (unsigned long)stamp_q(0, 3, 99), (unsigned long)stamp_q(0, 4, 50), (unsigned long)stamp_q(0, 4, 99));
  if (g_cfg.split) { /* v2 Task 3: mb = instance buffers + split graphs (cudaMemGetInfo deltas) */
    const size_t l = strlen(buf);
    snprintf(buf + l, n > (int)l ? n - l : 0, " GPUGRAPH split C=%lu D=%lu L=%lu execs=%lu mb=%lu (inst %lu graphs %lu, estimate) warm_ms=%lu capped=%lu unseen[C,D,L]=%lu,%lu,%lu",
             (unsigned long)g_split_n[SUB_C], (unsigned long)g_split_n[SUB_D], (unsigned long)g_split_n[SUB_L],
             (unsigned long)((NR_GPU_PASS_DL + NR_GPU_PASS_PROBE) * (g_split_n[SUB_C] + g_split_n[SUB_D] + g_split_n[SUB_L])), (unsigned long)(g_inst_mb + g_split_mb), (unsigned long)g_inst_mb.load(),
             (unsigned long)g_split_mb.load(), (unsigned long)(g_split_warm_us / 1000), (unsigned long)g_split_capped.load(), (unsigned long)g_split_unseen[SUB_C],
             (unsigned long)g_split_unseen[SUB_D], (unsigned long)g_split_unseen[SUB_L]);
    const size_t l2 = strlen(buf); /* v2 Task 4: entries of split passes (D = C skipped on the held instance), held calls, hold time */
    snprintf(buf + l2, n > (int)l2 ? n - l2 : 0, " retry[held=%lu entry_C=%lu entry_D=%lu dirty=%lu hold_us_p50<=%lu p99<=%lu]", (unsigned long)g_held.load(),
             (unsigned long)g_entry[0].load(), (unsigned long)g_entry[1].load(), (unsigned long)g_dirty.load(), (unsigned long)hist_q(g_hold_hist, 50), (unsigned long)hist_q(g_hold_hist, 99));
  }
}

/* test hook: marks the instances of `mask` busy (1) or frees them (0) so that a test can run on one chosen instance */
static void release_mask(unsigned mask);
extern "C" void nr_gpu_pass_test_hold(unsigned mask, int hold)
{
  if (hold) g_busy.fetch_or(mask);
  else release_mask(mask);
}

/* Pool acquisition (compare-and-swap on the busy mask). A real grant takes an instance of its own pool at once. If the pool is full it takes the OVERFLOW instance, the probe-pool
 * instance: probes are admitted only while at least two instances of the DL pool are free and no real grant waits, so the overflow instance is normally idle and a real grant gets it
 * immediately (a running probe is never preempted; the grant waits for it at most OVF_WAIT_US, ~ one probe, then the old path decodes it, counted busy). No real grant ever waits for
 * another real grant. Counters: overflow takes, overflow waits. */
static std::mutex g_pool_mu;
static std::condition_variable g_pool_cv;
static std::atomic<int> g_real_waiting{0};
static constexpr int OVF_WAIT_US = 400;
static int try_acquire(unsigned pool)
{
  for (unsigned m = g_busy.load();;) {
    const unsigned fr = ~m & pool;
    if (!fr) return -1;
    const int i = __builtin_ctz(fr);
    if (g_busy.compare_exchange_weak(m, m | 1u << i)) return i;
  }
}
static void release_mask(unsigned mask)
{
  g_busy.fetch_and(~mask);
  if (g_real_waiting.load() > 0) {
    std::lock_guard<std::mutex> lk(g_pool_mu);
    g_pool_cv.notify_all();
  }
}
static void release(int id) { release_mask(1u << id); }
static int acquire_real(unsigned pool)
{
  int id = try_acquire(pool);
  if (id >= 0) return id;
  id = try_acquire(M_PROBE);
  if (id >= 0) { ++g_ovf_takes; return id; }
  ++g_real_waiting; /* the overflow instance runs a probe (short): wait for it, not for a real grant */
  ++g_ovf_waits;
  const auto dl = std::chrono::steady_clock::now() + std::chrono::microseconds(OVF_WAIT_US);
  {
    std::unique_lock<std::mutex> lk(g_pool_mu);
    while ((id = try_acquire(pool | M_PROBE)) < 0 && g_pool_cv.wait_until(lk, dl) != std::cv_status::timeout) {}
    if (id < 0) id = try_acquire(pool | M_PROBE);
  }
  --g_real_waiting;
  if (id >= 0 && (M_PROBE >> id & 1u)) ++g_ovf_takes;
  return id;
}
static int acquire_probe()
{
  if (g_real_waiting.load() > 0 || __builtin_popcount(~g_busy.load() & M_DL) < 2) return -1;
  return try_acquire(M_PROBE);
}

/* v2 Task 4 (ISAC_PDSCH_GPU_GRAPH_RETRY): a decode call (nr_pdsch_passive_decode: first body, retry arms, worker arms) holds the instance its first pass takes until
 * nr_gpu_pass_call_end, so that est stays on the device across its bodies. Nested calls keep the outer hold. */
static void hold_release()
{
  if (t_held < 0) return;
  g_inst[t_held].c_ok = false;
  hist_add(g_hold_hist, nr_gpu_pass_now_ns() - t_hold_ns);
  const int h = t_held;
  t_held = -1;
  release(h);
}
/* review nit: a thread that exits inside a call (no call_end) gives its held instance back */
struct HoldGuard { ~HoldGuard() { hold_release(); t_hold = 0; } };
static thread_local HoldGuard t_hold_guard;
extern "C" void nr_gpu_pass_call_begin(void)
{
  (void)&t_hold_guard; /* odr-use: constructs the guard, so its destructor runs at thread exit */
  ++t_hold;
}
extern "C" void nr_gpu_pass_call_end(void)
{
  if (t_hold <= 0 || --t_hold > 0) return;
  hold_release();
}
/* the host may have changed what D reads besides the job (a stage-path body rewrote the caller's rxdataF, which D reads in place on DIRECT): the next body re-runs C.
 * Review S3: the instance is released too (a stage-path body, or the RECOVERY_ASYNC worker chain, would otherwise keep it from other consumers); the call stays open, so the
 * next pass of the call acquires an instance again and holds it. */
extern "C" void nr_gpu_pass_call_dirty(void)
{
  if (t_held < 0) return;
  ++g_dirty;
  hold_release();
}
/* test / census: C entries, D entries (C skipped), held calls, dirty invalidations */
extern "C" void nr_gpu_pass_retry_counters(uint64_t o[4])
{
  o[0] = g_entry[0]; o[1] = g_entry[1]; o[2] = g_held; o[3] = g_dirty;
}

/* every input of sub-graph C of a job (what prepare() turns into the FEP block, the chest rows, the pilots and the segment tables) as bytes; > cap = not comparable.
 * The IQ ring is identified by its pointers and offset: its samples do not change within one decode call (the recovery worker decodes its own snapshot). */
static size_t c_sig(const nr_gpu_pass_job_t *j, const Key &k, uint8_t *o, size_t cap)
{
  size_t n = 0;
  auto put = [&](const void *p, size_t b) { if (n + b <= cap) memcpy(o + n, p, b); n += b; };
  const uint64_t kc = key_sub(k, SUB_C).pack();
  const bool redirect = nr_gpu_model() == NR_GPU_MODEL_DIRECT && j->rxdataF; /* DIRECT: the FEP writes the caller's rxdataF and C / D read it there */
  const int16_t *rxf = redirect ? j->rxdataF : nullptr;
  const uint32_t rst = redirect ? j->rx_stride : 0;
  put(&kc, sizeof(kc)); put(j->rxdata, sizeof(j->rxdata)); put(&j->ring_len, 4); put(&j->ring_offset, 4); put(&j->symbol_rot, sizeof(j->symbol_rot));
  put(&rxf, sizeof(rxf)); put(&rst, 4); /* the FEP symbol set (copy_mask) is not part of it: skip_c checks it against the symbols C last transformed (Inst::fep_run) */
  put(&j->dmrs_type, 1); put(&j->nl, 4); put(&j->ndmrs, 4); put(&j->nrb, 4); put(&j->k0, 4); put(&j->nseg, 4); put(&j->seg_pil_nrb, 4);
  put(j->dmrs_sym, sizeof(int) * j->ndmrs); put(j->delta, sizeof(int) * j->nl);
  for (int s = 0; s < j->nseg; s++) { put(&j->seg[s].n_prb, 2); put(&j->seg[s].data_index, 2); put(&j->seg[s].pil_rb, 4); put(&j->seg[s].k0, 4); }
  const size_t pb = (size_t)(j->dmrs_type == 2 ? 4 : 6) * (j->nseg ? j->seg_pil_nrb : j->nrb) * 4;
  for (int q = 0; q < j->ndmrs; q++)
    for (int l = 0; l < j->nl; l++) put(j->pil[q][l], pb);
  return n;
}

/* T6: the memory census of the pass for the SENSING: GPU_MEM line: out = {device total MB, pass instance buffers MB, split graphs MB (estimate), cap MB, HARQ store MB, pinned host MB}. */
extern "C" void nr_gpu_pass_mem(uint64_t out[6])
{
  size_t fr = 0, tb = 0;
  out[0] = cudaMemGetInfo(&fr, &tb) == cudaSuccess ? tb >> 20 : 0;
  out[1] = g_inst_mb;
  out[2] = g_split_mb;
  out[3] = (uint64_t)split_cap_mb();
  out[4] = g_hq ? ((uint64_t)NR_GPU_HQ_BLK * NR_GPU_HQ_SEG * 2) >> 20 : 0;
  out[5] = g_pinned_b >> 20;
}
extern "C" int nr_gpu_pass_dl_try(const nr_gpu_pass_job_t *j, nr_gpu_pass_result_t *r)
{
  const uint64_t ts_e = nr_gpu_pass_now_ns();
  if (!g_ready || g_dead || !j || !r || !r->tb) { ++g_fb_unsup; return 1; }
  const Key k = key_of(j);
  const nr_gpu_demod_job_t &d = j->dj;
  const uint32_t ext = d.ext;
  const uint32_t nd = j->n_decode && j->n_decode < j->C ? j->n_decode : j->C; /* v2 Task 2: blocks decoded (a probe: block 0) */
  if (j->hq_mode && (j->hq_mode > 2 || j->probe || !g_hq)) { ++g_fb_unsup; return 1; } /* the host would count a combine that never happened */
  if (!key_ok(k) || (int)k.nl != j->nl || (int)k.ndmrs != j->ndmrs || j->nbRx != g_cfg.nbRx || (int)j->nrb > (int)k.nrb || j->C < 1 || nd > k.ncb || j->G_dem > j->G || (j->probe && !j->G_dem) ||
      (j->Z & 3 && j->C != 1) || ((j->K + 7) >> 3) > (uint32_t)KBMAX || ((j->K - j->F) & 7) || j->G > LLR_CAP || j->G == 0 || ext > (uint32_t)EXTMAX || (d.want_evm && (k.fp32 & 1) == (uint8_t)d.evm_fp64) || ((k.fp32 >> 1) != (uint8_t)(d.demod_fp32 != 0)) ||
      j->dmrs_type > 2 || j->nseg < 0 || j->nseg > (int)k.nseg || (j->nseg == 0) != (k.nseg == 0) || !seg_ok(j) ||
      j->it_max < 1 || j->it_max > NSNAP + 1 || !j->scr || !j->geo_y || !j->geo_h || !j->symbol_rot || !j->rxdata[0]) {
    ++g_fb_unsup;
    return 1;
  }
  Entry *e = nullptr, *es[3] = {}; /* split: the C, D and L sub-graphs */
  const bool split = g_cfg.split != 0;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (split) {
      for (int q = 0; q < 3; q++) {
        auto f = g_cache.find(key_sub(k, SUB_C + q).pack());
        if (f != g_cache.end()) es[q] = &f->second;
      }
      if (es[0] && es[1] && es[2]) e = es[0];
    } else {
      auto f = g_cache.find(k.pack());
      if (f != g_cache.end()) e = &f->second;
    }
  }
  if (!e) {
    if (k.nseg) ++g_fb_unseen_seg;
    for (int q = 0; split && q < 3; q++)
      if (!es[q]) ++g_split_unseen[SUB_C + q];
    ++g_fb_unseen;
    return 1;
  }
  /* the instance: the one this thread's decode call holds (Task 4), else a free one. A probe never takes one of the last probe_reserve() free instances: real grants that
   * find none free fall back to the slow stage path. In a held call the first acquisition keeps the instance until nr_gpu_pass_call_end. */
  const bool held = t_held >= 0;
  const int id = held ? t_held : j->probe ? acquire_probe() : acquire_real(M_DL);
  if (id < 0) {
    static std::atomic<int> once{0};
    if (!j->probe && !once.exchange(1)) fprintf(stderr, "GPUPASS first busy fallback: no DL instance and the overflow instance stayed on a probe for %d us (the old path decodes this grant)\n", OVF_WAIT_US);
    ++(j->probe ? g_fb_busy_probe : g_fb_busy);
    return 1;
  }
  Inst &I = g_inst[id];
  if (!held) {
    I.c_ok = false; /* a fresh instance: whatever estimate it holds is not this call's */
    if (t_hold > 0) { t_held = id; t_hold_ns = nr_gpu_pass_now_ns(); ++g_held; }
  }
  struct Rel { int id; ~Rel() { if (id >= 0) release(id); } } rel{t_held == id ? -1 : id};
  NrGpuRtScope rt;
  if (!rt.admitted) { ++g_fb_unsup; return 1; }
  const int pr = prepare(I, j);
  if (pr != 0) { ++(pr == -2 ? g_fb_ldpckey : g_fb_err); return 1; }
  /* Task 4 entry: on the held instance, C is skipped when its inputs equal those of the C the instance last ran (the estimate on the device is this job's); D and L
   * always run, after a fresh H2D of the parameter blocks (h2d_params). Data-driven, so an arm that changes the DM-RS hypothesis (CDM, pilots) can never reuse an estimate. */
  bool skip_c = false;
  size_t sig_n = SIG_CAP + 1;
  if (split && t_held == id) {
    sig_n = c_sig(j, k, I.sig[I.sig_cur ^ 1].data(), SIG_CAP);
    skip_c = I.c_ok && sig_n <= SIG_CAP && sig_n == I.sig_n[I.sig_cur] && !memcmp(I.sig[I.sig_cur ^ 1].data(), I.sig[I.sig_cur].data(), sig_n) && !(fep_need(j) & ~I.fep_run);
    I.sig_cur ^= 1;
    I.sig_n[I.sig_cur] = sig_n;
  }
  I.c_ok = false; /* until this launch completes */
  if (!skip_c) I.fep_run = fep_need(j);
  const uint64_t ts_l = nr_gpu_pass_now_ns();
  const auto t0 = std::chrono::steady_clock::now();
  cudaStream_t ls = j->probe && I.s_lo ? I.s_lo : I.s; /* a captured graph launches on either stream */
  const uint32_t seq = ++g_pass_seq;
  {
    nvtxEventAttributes_t na = {};
    na.version = NVTX_VERSION;
    na.size = NVTX_EVENT_ATTRIB_STRUCT_SIZE;
    na.messageType = NVTX_MESSAGE_TYPE_ASCII;
    na.message.ascii = "gp";
    na.payloadType = NVTX_PAYLOAD_TYPE_UNSIGNED_INT64;
    na.payload.ullValue = seq;
    nvtxMarkEx(&na);
  }
  const uint64_t ts_l1 = nr_gpu_pass_now_ns();
  const bool launched = cudaEventRecord(I.ev0, ls) == cudaSuccess && (split ? !h2d_params(I, k, ls, !skip_c) && (skip_c || cudaGraphLaunch(es[0]->exec[id], ls) == cudaSuccess) &&
                                    cudaEventRecord(I.evC, ls) == cudaSuccess && cudaGraphLaunch(es[1]->exec[id], ls) == cudaSuccess && cudaEventRecord(I.evD, ls) == cudaSuccess &&
                                    cudaGraphLaunch(es[2]->exec[id], ls) == cudaSuccess /* back to back: stream order, no host wait between (evC / evD: measurement markers) */
                              : cudaGraphLaunch(e->exec[id], ls) == cudaSuccess);
  const bool recorded = launched && cudaEventRecord(I.done, ls) == cudaSuccess;
  const uint64_t ts_l2 = nr_gpu_pass_now_ns();
  if (!recorded) {
    ++g_fb_err;
    g_dead = 1;
    cudaStreamSynchronize(ls); /* error path (review N1): drain what was launched before the instance is released; g_dead keeps every later pass off */
    return -1;
  }
  if (split) ++g_entry[skip_c];
  wait_done(I, 0, k.pack(), ts_l1);
  const uint64_t ts_d = nr_gpu_pass_now_ns();
  const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
  if (g_dead) { ++g_fb_err; return -1; }
  I.c_ok = t_held == id && sig_n <= SIG_CAP;
  ++g_launches;
  ++g_hits;
  if (k.nseg) ++g_seg_launches;
  {
    int b = 0;
    while (b < 31 && (1u << b) < (uint32_t)us) b++;
    ++g_wait_hist[b];
  }
  const ResBlk &rb = *I.RB();
  memset(r->seg_ok, 0, sizeof(r->seg_ok));
  memset(r->iters, 0, sizeof(r->iters));
  r->tbs = j->A;
  r->gpu_us = (uint32_t)us;
  r->instance = (int8_t)id;
  r->need_cpu = rb.need_cpu || rb.err;
  /* the nvar the demod actually used. nvar_valid is 0 only when no DM-RS estimate exists (never for an accepted job); then the device value is the instance's last one
   * (0 after init: the result block is zero-initialised), deterministic per instance */
  r->nvar = rb.post.nvar;
  r->post = rb.post;
  r->llr_stats = rb.llr_stats;
  r->tb_ok = 0;
  r->seq = seq;
  memset(&r->li, 0, sizeof(r->li));
  r->cspan_ns = r->dspan_ns = r->lspan_ns = 0;
  if (split) split_stamps(I, !skip_c, r);
  r->skip = split && rb.skip ? 1 : 0; /* Fix A+D: the in-graph gate fired (D's IF body and L did not run); a full (non-split) graph has no gate and leaves rb.skip stale */
  r->gate_db = rb.gate_db;
  if (r->skip) {
    r->need_cpu = 0;
    ++g_gate_skips;
    return 0;
  }
  for (uint32_t s = 0; s < nd; s++) r->seg_ok[s] = rb.seg_ok[s];
  for (uint32_t s = 0; s < nd; s++) r->iters[s] = rb.it[s] == 255 ? (uint8_t)j->it_max : rb.it[s];
  if (split) ldpc_info(I, (int)nd, (int)k.ncb, r);
  if (rb.need_cpu) ++g_need_cpu;
  if (!r->need_cpu) {
    const bool crc_ok = j->C > 1 ? rb.tb_crc != 0 : rb.seg_crc[0] != 0;
    r->tb_ok = rb.all_ok && crc_ok && nd == j->C && !j->probe; /* a probe is never a decoded TB: its LLRs go to llr_out below like a CRC failure's */
    /* reassemble the TB exactly as passive_ldpc_decode_core: seg_bytes of every block (the CRC24B bytes of a multi-block TB are dropped) */
    const uint32_t seg_bytes = ((j->K - j->F) >> 3) - (j->C > 1 ? 3 : 0);
    uint32_t off = 0;
    for (uint32_t s = 0; s < nd; s++, off += seg_bytes) memcpy(r->tb + off, rb.c[s], seg_bytes);
    if (!r->tb_ok && !j->probe) ++g_tb_fail;
  }
  nr_gpu_demod_job_t *dj = r->demod_out ? r->demod_out : &I.dj; /* the instance is ours until it is released */
  memcpy(dj, &j->dj, offsetof(nr_gpu_demod_job_t, evm));
  nr_pdsch_gpu_demod_pass_out(id, dj);
  r->log2_maxh = dj->log2_maxh;
  r->nvar_db = r->nvar ? 10.0f * log10f((float)r->nvar) : -99.0f;
  r->evm_db = r->snr_db = 0.0f;
  {
    const uint32_t *q = (const uint32_t *)((const char *)I.nvar.host + 3 * NR_GPU_CHEST_MAX_ROWS * sizeof(int)) + (size_t)(j->ndmrs * j->nl - 1) * g_cfg.nbRx;
    for (int a = 0; a < 4; a++) r->nvar_ant[a] = k.nseg > 1 ? rb.post.nvar_ant[a] : (a < g_cfg.nbRx ? q[a] : 0); /* segmented: the width-weighted mean the post kernel computed */
  }
  nr_dmrs_snr_init(&r->dsnr);
  if (j->ndmrs > 0 && j->nl > 0) { /* last DM-RS symbol and layer, as nvar_ant; a segmented estimate is composed in data order, so [0, 12 nrb) is the grant's */
    const int m = j->dmrs_sym[j->ndmrs - 1], l = j->nl - 1;
    const int16_t *rows[4] = {0, 0, 0, 0};
    const bool gd = nr_gpu_model() == NR_GPU_MODEL_DISCRETE; /* DISCRETE: the compact rows k_gather copied (block 2 * nbRx + a); DIRECT: the mapped estimate */
    for (int a = 0; a < g_cfg.nbRx && a < 4; a++)
      rows[a] = gd ? (const int16_t *)I.gath.host + 2 * ((size_t)(2 * g_cfg.nbRx + a) * N)
                   : (const int16_t *)I.est.host + 2 * ((size_t)(l * g_cfg.nbRx + a) * NSYM * N + (size_t)m * N);
    nr_dmrs_snr_fill(&r->dsnr, rows, g_cfg.nbRx, 12 * j->nrb, r->nvar_ant, j->dmrs_type, 1.0);
    if (!j->nseg) r->dsnr.ref_db = nr_dmrs_ref_db(nr_gpu_model() == NR_GPU_MODEL_DISCRETE || !j->rxdataF ? (const int16_t *)I.rxF.host + 2 * (size_t)m * N : j->rxdataF + 2 * (size_t)m * N, N, j->car_k0, j->car_n, j->k0, 12 * j->nrb);
  }
  if (!r->need_cpu && (!r->tb_ok || j->llr_out_all) && j->llr_out)
    memcpy(j->llr_out, I.llr.host, (size_t)j->G * 2); /* the first body's LLRs for the caller's retry arms */
  r->qm_valid = 0;
  if (r->qm_row && j->qm_sym >= 0 && j->qm_sym < NSYM && j->qm_n > 0 && j->qm_n <= (uint32_t)N) {
    /* comp[(m * NR_GPU_DEMOD_MAX_NL + layer) * buf + i], c16; DISCRETE: the row k_gather copied (block 3 * nbRx) */
    memcpy(r->qm_row,
           nr_gpu_model() == NR_GPU_MODEL_DISCRETE ? (const int16_t *)I.gath.host + 2 * (size_t)3 * g_cfg.nbRx * N
                                                   : (const int16_t *)I.scratch.host + 2 * (size_t)((size_t)j->qm_sym * NR_GPU_DEMOD_MAX_NL * ext),
           (size_t)j->qm_n * 4);
    r->qm_valid = 1;
  }
  if (r->brfo_rows && j->fo_first >= 0 && j->fo_last > j->fo_first) {
    const int16_t *e0 = (const int16_t *)I.est.host;
    const bool gd = nr_gpu_model() == NR_GPU_MODEL_DISCRETE; /* DISCRETE: blocks a and nbRx + a of k_gather */
    for (int a = 0; a < g_cfg.nbRx; a++) {
      memcpy(r->brfo_rows + ((size_t)a * 2) * N * 2,
             gd ? (const int16_t *)I.gath.host + 2 * ((size_t)a * N) : e0 + 2 * ((size_t)(0 * g_cfg.nbRx + a) * NSYM * N + (size_t)j->dmrs_sym[j->fo_first] * N), (size_t)N * 4);
      memcpy(r->brfo_rows + ((size_t)a * 2 + 1) * N * 2,
             gd ? (const int16_t *)I.gath.host + 2 * ((size_t)(g_cfg.nbRx + a) * N) : e0 + 2 * ((size_t)(0 * g_cfg.nbRx + a) * NSYM * N + (size_t)j->dmrs_sym[j->fo_last] * N), (size_t)N * 4);
    }
  }
  if (nr_gpu_model() == NR_GPU_MODEL_DISCRETE && j->rxdataF) { /* the symbols the caller needs, from the pinned copy of the instance's rxdataF */
    for (int a = 0; a < g_cfg.nbRx; a++)
      for (int m = 0; m < NSYM; m++)
        if (j->copy_mask >> m & 1) memcpy(j->rxdataF + 2 * ((size_t)a * j->rx_stride + (size_t)m * N), (const int16_t *)I.rxF.host + 2 * ((size_t)a * NSYM * N + (size_t)m * N), (size_t)N * 4);
  }
  {
    const uint64_t ts_c = nr_gpu_pass_now_ns();
    if (j->t_entry_ns && j->t_entry_ns <= ts_l) stamp_add(0, 0, ts_l - j->t_entry_ns);
    stamp_add(0, 1, ts_d - ts_l);
    stamp_add(0, 2, ts_c - ts_d);
    gpu_stamps(0, I, !(split && skip_c), r);
    if (r->gstart_ns) pred_update(0, k.pack(), r->gstart_ns + r->gspan_ns);
    r->prep_ns = (uint32_t)(ts_l1 - ts_e);
    r->launch_ns = (uint32_t)(ts_l2 - ts_l1);
  }
  return 0;
}

/* ---- Task 7: the PUSCH pass entry points ---- */
/* counters: launches, graph hits, fallbacks[unseen, busy, unsupported, error, ldpc key], tb_fail, need_cpu */
extern "C" void nr_gpu_pass_ul_counters(uint64_t o[16])
{
  memset(o, 0, 16 * sizeof(uint64_t));
  o[0] = g_ul_launches; o[1] = g_ul_hits; o[2] = g_ul_fb_unseen; o[3] = g_ul_fb_busy; o[4] = g_ul_fb_unsup; o[5] = g_ul_fb_err; o[6] = g_ul_fb_ldpckey; o[7] = g_ul_tb_fail; o[8] = g_ul_need_cpu;
  uint64_t tot = 0, acc = 0;
  for (auto &h : g_ul_wait_hist) tot += h;
  int p50 = 0, p99 = 0;
  for (int i = 0; i < 32; i++) {
    acc += g_ul_wait_hist[i];
    if (acc * 2 >= tot && tot && !p50) p50 = i + 1;
    if (acc * 100 >= tot * 99 && tot && !p99) p99 = i + 1;
  }
  o[9] = tot ? 1ull << (p50 - 1) : 0; /* host wait of a pass, p50 <= o[9] us (power-of-two bucket) */
  o[10] = tot ? 1ull << (p99 - 1) : 0;
  o[11] = 0;
  for (const auto &e : g_cache) o[11] += (e.first >> 40) & 1;
  for (int k = 0; k < 3; k++) o[12 + k] = stamp_q(1, k, 50) | stamp_q(1, k, 99) << 32; /* Task 8b: prep, run, post (p50 | p99 << 32, us) */
}

extern "C" int nr_gpu_pass_ul_try(const nr_gpu_pass_job_t *j, nr_gpu_pass_result_t *r)
{
  const uint64_t ts_e = nr_gpu_pass_now_ns();
  if (!g_ready || g_dead || !j || !r || !r->tb || !j->ul.on) { ++g_ul_fb_unsup; return 1; }
  const nr_gpu_pass_ul_t &u = j->ul;
  const Key k{j->shape.nrb_class, 1, j->shape.ndmrs, j->shape.ncb_class, 0, 0, 0, 0, 0, 1};
  uint32_t gsum = 0;
  for (int m = 0; m < NSYM; m++) gsum += (uint32_t)u.nre[m] * j->Qm;
  if (!j->shape.ul || !key_ok_ul(k) || j->nl != 1 || j->ndmrs != (int)k.ndmrs || j->nbRx != g_cfg.nbRx || u.ndmrs != j->ndmrs || u.nrb < 1 || u.nrb > (int)k.nrb ||
      12 * u.nrb > EXTMAX || j->C < 1 || j->C > k.ncb || (j->Z & 3) || (j->K & 7) || (j->K >> 3) > (uint32_t)KBMAX || j->G > LLR_CAP || j->G == 0 || gsum != j->G ||
      (j->Qm != 2 && j->Qm != 4 && j->Qm != 6 && j->Qm != 8) || u.delta < 0 || u.delta > 1 || u.keep >= g_cfg.nbRx || !j->scr || j->it_max < 1 || j->it_max > NSNAP + 1) {
    ++g_ul_fb_unsup;
    return 1;
  }
  Entry *e = nullptr;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    auto f = g_cache.find(k.pack());
    if (f != g_cache.end()) e = &f->second;
  }
  if (!e) {
    ++g_ul_fb_unseen;
    return 1;
  }
  const int id = j->probe ? acquire_probe() : acquire_real(M_UL);
  if (id < 0) { ++g_ul_fb_busy; return 1; }
  Inst &I = g_inst[id];
  struct Rel { int id; ~Rel() { release(id); } } rel{id};
  /* no NrGpuRtScope: the UL pass runs on lowest-priority streams of the RT context and must not hold off a CSI-RS / NID discovery slice (it only counts as an RT user for DL) */
  const int pr = prepare_ul(I, j);
  if (pr != 0) { ++(pr == -2 ? g_ul_fb_ldpckey : g_ul_fb_err); return 1; }
  const uint64_t ts_l = nr_gpu_pass_now_ns();
  const auto t0 = std::chrono::steady_clock::now();
  const uint64_t ts_l1 = nr_gpu_pass_now_ns();
  const bool ul_launched = cudaEventRecord(I.ev0, I.s) == cudaSuccess && cudaGraphLaunch(e->exec[id], I.s) == cudaSuccess && cudaEventRecord(I.done, I.s) == cudaSuccess;
  const uint64_t ts_l2 = nr_gpu_pass_now_ns();
  if (!ul_launched) {
    ++g_ul_fb_err;
    g_dead = 1;
    return -1;
  }
  wait_done(I, 1, k.pack(), ts_l1);
  const uint64_t ts_d = nr_gpu_pass_now_ns();
  const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
  if (g_dead) { ++g_ul_fb_err; return -1; }
  ++g_ul_launches;
  ++g_ul_hits;
  {
    int b = 0;
    while (b < 31 && (1u << b) < (uint32_t)us) b++;
    ++g_ul_wait_hist[b];
  }
  const ResBlk &rb = *I.RB();
  memset(r->seg_ok, 0, sizeof(r->seg_ok));
  r->tbs = j->A;
  r->gpu_us = (uint32_t)us;
  r->instance = (int8_t)id;
  r->need_cpu = rb.need_cpu || rb.err;
  r->tb_ok = 0;
  r->ul_tb_crc = rb.tb_crc;
  r->skip = rb.skip ? 1 : 0;
  r->gate_db = rb.gate_db;
  if (r->skip) { r->need_cpu = 0; ++g_gate_skips; }
  for (uint32_t s = 0; s < j->C; s++) r->seg_ok[s] = r->skip ? 0 : rb.seg_ok[s];
  if (rb.need_cpu) ++g_ul_need_cpu;
  if (!r->need_cpu && !r->skip) {
    const bool crc_ok = j->C > 1 ? rb.tb_crc != 0 : rb.seg_crc[0] != 0;
    r->tb_ok = rb.all_ok && crc_ok;
    const uint32_t seg_bytes = (j->K >> 3) - (j->F >> 3) - (j->C > 1 ? 3 : 0);
    uint32_t off = 0;
    for (uint32_t s = 0; s < j->C; s++, off += seg_bytes) memcpy(r->tb + off, rb.c[s], seg_bytes);
    if (!r->tb_ok) ++g_ul_tb_fail;
  }
  const int rows = j->ndmrs * g_cfg.nbRx;
  r->ul_nrows = (uint32_t)rows;
  r->ul_log2_maxh = rb.ul.log2_maxh;
  for (int a = 0; a < 4; a++) r->ul_avg[a] = rb.ul.avg[a];
  {
    const int *dly = (const int *)I.nvar.host;
    const uint64_t *acc = (const uint64_t *)((const int *)I.nvar.host + 3 * NR_GPU_CHEST_MAX_ROWS) + 512;
    for (int q = 0; q < rows; q++) {
      memcpy(r->ul_delay[q], dly + 3 * q, 3 * sizeof(int));
      r->ul_acc[q] = acc[q];
      r->ul_hsum[q] = rb.ul.hsum[q];
    }
  }
  if (!r->need_cpu && (!r->tb_ok || j->llr_out_all) && j->llr_out) memcpy(j->llr_out, I.llr.host, (size_t)j->G * 2);
  if (u.est_out)
    for (int q = 0; q < rows; q++)
      memcpy(u.est_out + (size_t)q * N * 2, (const int16_t *)I.est.host + 2 * ((size_t)(q % g_cfg.nbRx) * NSYM * N + (size_t)u.dmrs_sym[q / g_cfg.nbRx] * N), (size_t)N * 4);
  if (nr_gpu_model() == NR_GPU_MODEL_DISCRETE) { /* the transformed symbols the host consumers read */
    for (int a = 0; a < g_cfg.nbRx; a++)
      if (u.rx[a])
        for (int m = 0; m < NSYM; m++)
          if (u.win[m] >= 0) memcpy(u.rx[a] + 2 * (size_t)m * N, (const int16_t *)I.rxF.host + 2 * ((size_t)a * NSYM + m) * N, (size_t)N * 4);
  }
  {
    const uint64_t ts_c = nr_gpu_pass_now_ns();
    if (j->t_entry_ns && j->t_entry_ns <= ts_l) stamp_add(1, 0, ts_l - j->t_entry_ns);
    stamp_add(1, 1, ts_d - ts_l);
    stamp_add(1, 2, ts_c - ts_d);
    gpu_stamps(1, I, true, r);
    if (r->gstart_ns) pred_update(1, k.pack(), r->gstart_ns + r->gspan_ns);
    r->prep_ns = (uint32_t)(ts_l1 - ts_e);
    r->launch_ns = (uint32_t)(ts_l2 - ts_l1);
  }
  return 0;
}
