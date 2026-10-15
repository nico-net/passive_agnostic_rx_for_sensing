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
#include "../TOOLS/nr_gpu_poll.h"
#include "PHY/TOOLS/nr_gpu_platform.h"
#include "PHY/NR_UE_ESTIMATION/filt16a_32.h"
#include <cuda_runtime.h>
#include "nr_gpu_green.h"
#include "PHY/TOOLS/nr_gpu_budget_cuda.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>
#include <mutex>
#include <thread>

/* entry points of the other translation units of libpdsch_gpu.so */
extern "C" {
int nr_pdsch_gpu_fep_stage_at(const nr_gpu_fep_job_t *j, int slot);
int nr_pdsch_gpu_fep_dev(const nr_gpu_fep_job_t *j, nr_gpu_buf_t *rxF, int slot, void *stream);
int nr_pdsch_gpu_chest_stage_at(const nr_gpu_chest_job_t *j, int n, int slot);
int nr_pdsch_gpu_chest_dev(const nr_gpu_chest_job_t *j, int n, nr_gpu_buf_t *est_dev, nr_gpu_buf_t *nvar_dev, int slot, void *stream);
int nr_pdsch_gpu_demod_pass_set(int inst, const nr_gpu_demod_job_t *j);
int nr_pdsch_gpu_demod_pass_dev(int inst, const nr_gpu_demod_job_t *jcap, nr_gpu_buf_t *llr_dev, const uint32_t *nvar_dev, void *stream, void *stream2);
void nr_pdsch_gpu_demod_pass_out(int inst, nr_gpu_demod_job_t *j);
int nr_pdsch_gpu_ldpc_pass_set(int inst, const nr_gpu_ldpc_job_t *jobs, int n, const nr_gpu_buf_t *llr_dev);
int nr_pdsch_gpu_ldpc_pass_dev(int inst, const nr_gpu_ldpc_job_t *jobs, int n, const nr_gpu_buf_t *llr_dev, nr_gpu_buf_t *bits_dev, void *stream);
}

namespace nrpass {
constexpr int NI = NR_GPU_PASS_RING;
constexpr int MAXCB = NR_GPU_PASS_MAX_CB;
constexpr int NSYM = 14;
constexpr int N = 4096;
constexpr int NSNAP = NR_GPU_LDPC_MAX_SNAP;
constexpr int KBMAX = NR_GPU_LDPC_KB_MAX;
constexpr int EXTMAX = NR_GPU_DEMOD_MAX_EXT;
constexpr uint32_t LLR_CAP = 1500000;      /* LLRs of one pass: 273 PRB x 13 symbols x 12 x Qm 8 x 4 layers = 1362816 */
constexpr int PIL_STRIDE = 3328;           /* c16 per (DM-RS symbol, layer) pilot row */

/* ---- device-accessible blocks ---- */
struct PassBlk { /* written by the host before every replay */
  nr_gpu_chest_post_par_t post;
  uint32_t G, span, tbs;
  int norm, ncb, KB;
  const uint32_t *scr;
  nr_gpu_rdm_seg_t rdm[MAXCB];
  nr_gpu_crc_seg_t cand[MAXCB * NSNAP]; /* every snapshot of every code block */
  nr_gpu_crc_seg_t sel[MAXCB];          /* the chosen block of every code block */
};
struct ResBlk { /* written by the kernels, read by the host after the pass */
  nr_gpu_chest_post_out_t post;
  uint32_t shift;
  int need_cpu, err, all_ok;
  uint8_t tb_crc, dummy_tb;
  uint8_t seg_ok[MAXCB];
  uint8_t seg_crc[MAXCB];
  uint8_t cand_ok[MAXCB * NSNAP];
  uint8_t c[MAXCB][KBMAX];
};

/* ---- kernels of the LLR post and the stop rule ---- */
__global__ void k_unscr(int16_t *llr, const PassBlk *pb)
{
  const uint32_t G = pb->G;
  const uint32_t *scr = pb->scr;
  for (uint32_t i = blockIdx.x * blockDim.x + threadIdx.x; i < G; i += gridDim.x * blockDim.x)
    if (scr[i >> 5] >> (i & 31) & 1u) llr[i] = (int16_t)(-llr[i]);
}
/* nr_llr_norm_shift: the mean |LLR| of every 16th of llr[0, span), smallest k <= 8 with (mean >> k) <= 40 */
__global__ void k_norm_sum(const int16_t *llr, const PassBlk *pb, ResBlk *rb)
{
  __shared__ unsigned long long red[1024 / 32];
  unsigned long long acc = 0;
  const uint32_t span = pb->span;
  if (pb->norm)
    for (uint32_t i = threadIdx.x * 16u; i < span; i += blockDim.x * 16u) acc += (uint32_t)abs((int)llr[i]);
  for (int o = 16; o; o >>= 1) acc += __shfl_down_sync(0xffffffffu, acc, o);
  if ((threadIdx.x & 31) == 0) red[threadIdx.x >> 5] = acc;
  __syncthreads();
  if (threadIdx.x == 0) {
    unsigned long long a = 0;
    for (unsigned w = 0; w < blockDim.x / 32; w++) a += red[w];
    const uint32_t cnt = (span + 15) / 16;
    uint32_t k = 0;
    if (pb->norm && cnt) {
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
  uint64_t pack() const
  {
    return (uint64_t)nrb | (uint64_t)nl << 9 | (uint64_t)ndmrs << 12 | (uint64_t)ncb << 15 | (uint64_t)fp32 << 22 | (uint64_t)evm << 23 | (uint64_t)qmode << 24 |
           (uint64_t)align << 28;
  }
};

struct Inst {
  int id = 0;
  cudaStream_t s = nullptr, s2 = nullptr;
  cudaEvent_t done = nullptr;
  nr_gpu_buf_t rxF = {}, est = {}, nvar = {}, geo = {}, scratch = {}, llr = {}, lin = {}, bits = {}, pil = {}, scr = {}, pb = {}, res = {};
  nr_gpu_chest_job_t *cj = nullptr;
  nr_gpu_fep_job_t fj = {};
  const int16_t *rx[4] = {};
  nr_gpu_ldpc_job_t *lj = nullptr; /* [MAXCB]: only BG/R/Z/K/it_max are used */
  nr_gpu_demod_job_t dj = {};
  size_t bn = 0;
  PassBlk *PB() const { return (PassBlk *)pb.host; }
  ResBlk *RB() const { return (ResBlk *)res.host; }
  const PassBlk *PBd() const { return (const PassBlk *)pb.dev; }
  ResBlk *RBd() const { return (ResBlk *)res.dev; }
};

struct Entry { cudaGraphExec_t exec[NI] = {}; };

static Inst g_inst[NI];
static nr_gpu_buf_t g_ring[4], g_rot;       /* the warm replay's dummy IQ ring and rotation */
static std::map<uint64_t, Entry> g_cache;
static std::mutex g_mu;
static nr_gpu_pass_cfg_t g_cfg = {};
static bool g_cfg_set = false;
static std::atomic<int> g_ready{0}, g_dead{0};
static std::atomic<unsigned> g_busy{0};
static std::atomic<uint64_t> g_launches{0}, g_hits{0}, g_fb_unseen{0}, g_fb_busy{0}, g_fb_unsup{0}, g_fb_err{0}, g_need_cpu{0}, g_tb_fail{0}, g_allocs{0};
static std::atomic<uint64_t> g_wait_hist[32]; /* host wait, 2^i us buckets */
constexpr uint32_t RING_LEN = 80000;
static int16_t g_zero_pil[6 * 273 * 2];
static uint32_t g_zero_scr[LLR_CAP / 32 + 2];

static bool guard_alloc() { return nr_gpu_allocations_allowed() != 0; }

/* ---- the synthetic job of a class (capture and warm replay): a well-formed grant of the class with an empty signal ---- */
static void synth_job(const Key &k, nr_gpu_pass_job_t *j, std::vector<int16_t> &geo)
{
  memset(j, 0, sizeof(*j));
  j->shape = {k.nrb, 13, k.nl, 2, k.ndmrs, k.ncb, k.fp32};
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
  d.evm_fp64 = !k.fp32;
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
  j->BG = 2; j->R = 15; j->R2 = 15; j->rv = 0; j->Qm = 2; j->it_max = 8;
}

/* the host side of a pass: writes every block the kernels read. Used for the synthetic job (capture / warm replay) and for real grants. `cls` = the key the
 * graph structure comes from (the job must fit it). 0 or -1. */
static int prepare(Inst &I, const nr_gpu_pass_job_t *j)
{
  const int nbRx = g_cfg.nbRx, nl = j->nl, nd = j->ndmrs, ext = (int)j->dj.ext;
  const bool redirect = nr_gpu_model() == NR_GPU_MODEL_DIRECT && j->rxdataF;
  const int16_t *rxbase = redirect ? (const int16_t *)j->rxdataF : (const int16_t *)I.rxF.dev;
  const size_t rxstride = redirect ? j->rx_stride : (size_t)NSYM * N; /* c16 between antennas */
  /* FEP block */
  for (int a = 0; a < 4; a++) I.rx[a] = j->rxdata[a];
  I.fj = {I.rx, j->ring_len, j->ring_offset, j->symbol_rot, 0x3fff, redirect ? (int16_t *)j->rxdataF : nullptr, redirect ? (uint32_t)j->rx_stride : 0u};
  if (nr_pdsch_gpu_fep_stage_at(&I.fj, I.id) != 0) return -1;
  /* chest rows: one per (DM-RS symbol, layer, antenna), the order of nr_pdsch_gpu_chest_grant */
  nr_gpu_chest_job_t &cj = *I.cj;
  int r = 0;
  for (int k = 0; k < nd; k++)
    for (int l = 0; l < nl; l++) {
      int16_t *pil = (int16_t *)I.pil.host + (size_t)(k * 4 + l) * PIL_STRIDE * 2;
      memcpy(pil, j->pil[k][l], (size_t)6 * j->nrb * sizeof(int16_t) * 2);
      for (int a = 0; a < nbRx; a++, r++) {
        nr_gpu_chest_row_t &row = cj.rows[r];
        row.rxF = rxbase + 2 * ((size_t)a * rxstride + (size_t)N * j->dmrs_sym[k] + j->delta[l]);
        row.pil = (const int16_t *)I.pil.dev + (size_t)(k * 4 + l) * PIL_STRIDE * 2;
        row.out = (int16_t *)I.est.dev + 2 * ((size_t)(l * nbRx + a) * NSYM * N + (size_t)j->dmrs_sym[k] * N);
        row.k0 = j->k0;
        row.nb_rb = j->nrb;
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
  pb.post.number_symbols = j->number_symbols; pb.post.nvfix = j->nvfix; pb.post.branch = j->branch;
  if (j->fo_first >= 0 && j->fo_last > j->fo_first) {
    pb.post.h0 = (const int16_t *)I.est.dev + 2 * (size_t)N * j->dmrs_sym[j->fo_first];
    pb.post.h1 = (const int16_t *)I.est.dev + 2 * (size_t)N * j->dmrs_sym[j->fo_last];
  }
  pb.post.N = N; pb.post.n_rb_dl = g_cfg.n_rb_dl;
  pb.post.dt_d = j->dt_d; pb.post.df_d = j->df_d; pb.post.dt_f = (float)j->dt_d; pb.post.df_f = (float)j->df_d;
  pb.G = j->G; pb.span = j->norm_span; pb.norm = j->norm; pb.tbs = j->A;
  pb.ncb = (int)j->C; pb.KB = (int)(j->K >> 3);
  if (nr_gpu_model() == NR_GPU_MODEL_DIRECT) {
    pb.scr = j->scr;
  } else {
    memcpy(I.scr.host, j->scr, (size_t)((j->G + 31) / 32) * 4);
    pb.scr = (const uint32_t *)I.scr.dev;
  }
  const int Kprime = (j->C > 1) ? (int)((j->A + 24 + 24 * j->C) / j->C) : (int)(j->A + (j->A > 3824 ? 24 : 16));
  const int ctype = (j->C > 1) ? 1 : (j->A > 3824 ? 0 : 2);
  const int pay = (int)(j->K >> 3) - (int)(j->F >> 3) - 3;
  const int ncb = j->shape.ncb_class;
  for (int s = 0; s < ncb; s++) {
    nr_gpu_rdm_seg_t &g = pb.rdm[s];
    memset(&g, 0, sizeof(g));
    if ((uint32_t)s < j->C) {
      const bool first = (uint32_t)s < j->first_rE2;
      g.E = first ? j->E : j->E2;
      g.llr_off = first ? (uint32_t)s * j->E : j->first_rE2 * j->E + ((uint32_t)s - j->first_rE2) * j->E2;
      g.Qm = j->Qm; g.BG = j->BG; g.Z = j->Z; g.K = j->K; g.F = j->F; g.Kprime = (uint32_t)Kprime; g.Tbslbrm = j->tbslbrm; g.C = j->C; g.rv = j->rv;
    } /* padding blocks: C == 0 is "refused" -> zeros */
    for (int q = 0; q < NSNAP; q++) {
      nr_gpu_crc_seg_t &c = pb.cand[s * NSNAP + q];
      c = {(uint32_t)((s * NSNAP + q) * KBMAX), (uint32_t)((uint32_t)s < j->C ? Kprime : 0), 0u, (uint32_t)ctype};
    }
    pb.sel[s] = {(uint32_t)(offsetof(ResBlk, c) + (size_t)s * KBMAX), (uint32_t)((uint32_t)s < j->C ? Kprime : 0), (uint32_t)(pay > 0 ? pay : 0), (uint32_t)ctype};
  }
  /* LDPC jobs: the padding blocks repeat block 0 (a zero input stops them at the first syndrome check) */
  for (int s = 0; s < ncb; s++) {
    const uint32_t q = (uint32_t)s < j->C ? (uint32_t)s : 0u;
    nr_gpu_ldpc_job_t &l = I.lj[s];
    l.BG = j->BG; l.R = q < j->first_rE2 ? j->R : j->R2; l.Z = (uint16_t)j->Z; l.K = (uint16_t)j->K; l.it_max = j->it_max; l.llr = nullptr;
  }
  if (nr_pdsch_gpu_ldpc_pass_set(I.id, I.lj, ncb, &I.lin) != 0) return -1;
  return 0;
}

static int enqueue(Inst &I, const Key &k, const nr_gpu_pass_job_t *sj, cudaStream_t s)
{
  const int ncb = k.ncb, nbRx = g_cfg.nbRx;
  const int ext = (12 * k.nrb + 15) & ~15;
  /* the parameter blocks (DISCRETE: H2D; DIRECT: the kernels read the mapped memory) */
  if (nr_gpu_buf_h2d_async(&I.pb, 0, sizeof(PassBlk), s) || nr_gpu_buf_h2d_async(&I.geo, 0, (size_t)2 * NSYM * ext * 4, s) ||
      nr_gpu_buf_h2d_async(&I.pil, 0, (size_t)NR_GPU_PASS_MAX_DMRS * 4 * PIL_STRIDE * 4, s) ||
      nr_gpu_buf_h2d_async(&I.scr, 0, (size_t)(((size_t)12 * k.nrb * 13 * 8 * k.nl + 31) / 32) * 4, s))
    return -1;
  if (nr_pdsch_gpu_fep_dev(&I.fj, &I.rxF, I.id, s) != 0) return -1;
  if (nr_pdsch_gpu_chest_dev(I.cj, I.cj->n_rows, &I.est, &I.nvar, I.id, s) != 0) return -1;
  if (nr_gpu_pass_launch_chest_post_p(I.PBd() ? &I.PBd()->post : nullptr, &I.RBd()->post, s) != 0) return -1;
  if (nr_pdsch_gpu_demod_pass_dev(I.id, &sj->dj, &I.llr, I.dj.nvar_dev, s, I.s2) != 0) return -1;
  k_unscr<<<256, 256, 0, s>>>((int16_t *)I.llr.dev, I.PBd());
  k_norm_sum<<<1, 1024, 0, s>>>((const int16_t *)I.llr.dev, I.PBd(), I.RBd());
  k_norm_apply<<<256, 256, 0, s>>>((int16_t *)I.llr.dev, I.PBd(), I.RBd());
  if (nr_gpu_pass_launch_rdm((const int16_t *)I.llr.dev, I.PBd()->rdm, ncb, (int8_t *)I.lin.dev, s) != 0) return -1;
  if (nr_pdsch_gpu_ldpc_pass_dev(I.id, I.lj, ncb, &I.lin, &I.bits, s) != 0) return -1;
  const uint8_t *snaps = (const uint8_t *)I.bits.dev;
  const int *dres = (const int *)((const char *)I.bits.dev + NR_GPU_LDPC_DEV_RES_OFF);
  if (nr_gpu_pass_launch_crc_p(snaps, I.PBd()->cand, ncb * NSNAP, &I.PBd()->tbs, I.RBd()->cand_ok, &I.RBd()->dummy_tb, s) != 0) return -1;
  k_pick<<<1, 128, 0, s>>>(snaps, dres, I.PBd(), I.RBd());
  if (nr_gpu_pass_launch_crc_p((const uint8_t *)I.res.dev, I.PBd()->sel, ncb, &I.PBd()->tbs, I.RBd()->seg_crc, &I.RBd()->tb_crc, s) != 0) return -1;
  if (nr_gpu_buf_d2h_async(&I.nvar, 3 * NR_GPU_CHEST_MAX_ROWS * sizeof(int), (size_t)k.ndmrs * k.nl * nbRx * sizeof(int), s) ||
      nr_gpu_buf_d2h_async(&I.res, 0, sizeof(ResBlk), s) || nr_gpu_buf_d2h_async(&I.est, 0, (size_t)nbRx * NSYM * N * 4, s) ||
      nr_gpu_buf_d2h_async(&I.rxF, 0, (size_t)nbRx * NSYM * N * 4, s))
    return -1;
  return cudaPeekAtLastError() == cudaSuccess ? 0 : -1;
}

static void wait_done(Inst &I)
{
  uint64_t pt = 0;
  cudaError_t e;
  while ((e = cudaEventQuery(I.done)) == cudaErrorNotReady) nr_gpu_poll_relax(&pt);
  if (e != cudaSuccess) g_dead = 1;
}

static Key key_of(const nr_gpu_pass_job_t *j)
{
  const nr_gpu_demod_job_t &d = j->dj;
  return Key{j->shape.nrb_class, j->shape.nl, j->shape.ndmrs, j->shape.ncb_class, j->shape.fp32, (uint8_t)(d.want_evm != 0),
             (uint8_t)(j->shape.nl >= 2 ? d.qam_mode : 0), (uint8_t)(j->shape.nl == 1 && d.align != 0)};
}

static bool key_ok(const Key &k)
{
  return g_cfg_set && nr_gpu_pass_nrb_class(k.nrb) == k.nrb && nr_gpu_pass_ncb_class(k.ncb) == k.ncb && k.nl >= 1 && k.nl <= 4 && k.nl <= g_cfg.nbRx &&
         k.ndmrs >= 1 && k.ndmrs <= NR_GPU_PASS_MAX_DMRS && k.qmode <= 3;
}

/* capture + instantiate + one replay (pre-fence) of a class on every instance. Under g_mu. */
static int build(const Key &k)
{
  if (!guard_alloc()) return -1;
  Entry e;
  nr_gpu_pass_job_t *sj = (nr_gpu_pass_job_t *)malloc(sizeof(*sj));
  if (!sj) return -1;
  std::vector<int16_t> geo;
  synth_job(k, sj, geo);
  int rc = 0;
  for (int i = 0; i < NI && !rc; i++) {
    Inst &I = g_inst[i];
    memset(I.res.host, 0, sizeof(ResBlk));
    if (prepare(I, sj) != 0) { rc = -1; break; }
    cudaGraph_t g = nullptr;
    if (cudaStreamBeginCapture(I.s, cudaStreamCaptureModeRelaxed) != cudaSuccess) { rc = -1; break; }
    const int er = enqueue(I, k, sj, I.s);
    const cudaError_t ee = cudaStreamEndCapture(I.s, &g);
    if (er != 0 || ee != cudaSuccess || !g) {
      fprintf(stderr, "nr_gpu_pass: capture of class nrb %u nl %u ndmrs %u ncb %u failed (enqueue %d, end %s, last %s)\n", k.nrb, k.nl, k.ndmrs, k.ncb, er,
              cudaGetErrorString(ee), cudaGetErrorString(cudaGetLastError()));
      if (g) cudaGraphDestroy(g);
      rc = -1;
      break;
    }
    ++g_allocs;
    if (cudaGraphInstantiate(&e.exec[i], g, 0) != cudaSuccess) rc = -1;
    cudaGraphDestroy(g);
    if (rc) break;
    /* the first replay allocates inside the driver / loads the kernels: do it now (pre-fence) */
    if (cudaGraphLaunch(e.exec[i], I.s) != cudaSuccess || cudaEventRecord(I.done, I.s) != cudaSuccess || cudaStreamSynchronize(I.s) != cudaSuccess) {
      fprintf(stderr, "nr_gpu_pass: warm replay of class nrb %u nl %u failed: %s\n", k.nrb, k.nl, cudaGetErrorString(cudaGetLastError()));
      rc = -1;
    }
  }
  free(sj);
  if (rc) {
    for (int i = 0; i < NI; i++)
      if (e.exec[i]) cudaGraphExecDestroy(e.exec[i]);
    return -1;
  }
  g_cache[k.pack()] = e;
  return 0;
}

static int alloc_inst(int id)
{
  Inst &I = g_inst[id];
  I.id = id;
  int lo = 0, hi = 0;
  cudaDeviceGetStreamPriorityRange(&lo, &hi);
  const size_t ext = EXTMAX;
  I.bn = (size_t)2 * NSYM * NR_GPU_DEMOD_MAX_NL * ext * 2;
  if ((!nr_gpu_green_stream(&I.s, hi, true, id) && cudaStreamCreateWithPriority(&I.s, cudaStreamNonBlocking, hi) != cudaSuccess) ||
      (!nr_gpu_green_stream(&I.s2, hi, true, id) && cudaStreamCreateWithPriority(&I.s2, cudaStreamNonBlocking, hi) != cudaSuccess) ||
      cudaEventCreateWithFlags(&I.done, cudaEventDisableTiming) != cudaSuccess)
    return -1;
  if (nr_gpu_buf_alloc(&I.rxF, (size_t)4 * NSYM * N * 4) || nr_gpu_buf_alloc(&I.est, (size_t)16 * NSYM * N * 4) ||
      nr_gpu_buf_alloc(&I.nvar, NR_GPU_CHEST_DEV_BYTES) || nr_gpu_buf_alloc(&I.geo, (size_t)2 * NSYM * ext * 4) ||
      nr_gpu_buf_alloc(&I.scratch, 4 * I.bn * 2) || nr_gpu_buf_alloc(&I.llr, (size_t)LLR_CAP * 2) ||
      nr_gpu_buf_alloc(&I.lin, NR_GPU_LDPC_DEV_IN_BYTES) || nr_gpu_buf_alloc(&I.bits, NR_GPU_LDPC_DEV_BITS_BYTES) ||
      nr_gpu_buf_alloc(&I.pil, (size_t)NR_GPU_PASS_MAX_DMRS * 4 * PIL_STRIDE * 4) || nr_gpu_buf_alloc(&I.scr, (size_t)(LLR_CAP / 32 + 2) * 4) ||
      nr_gpu_buf_alloc(&I.pb, sizeof(PassBlk)) || nr_gpu_buf_alloc(&I.res, sizeof(ResBlk)))
    return -1;
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

/* default warm set (max_classes): nrb 24/48/96/217 x nl 1..4, 2 DM-RS symbols, a typical padded block count, float */
static Key default_class(int i)
{
  static const int nrb[4] = {24, 48, 96, 217};
  const int n = nrb[i / 4], l = i % 4 + 1;
  int c = n * l / 12;
  if (c < 1) c = 1;
  return Key{(uint16_t)nr_gpu_pass_nrb_class(n), (uint16_t)l, 2, (uint16_t)nr_gpu_pass_ncb_class(c > 32 ? 32 : c), 1, 0, (uint8_t)(l >= 2 ? 3 : 0), 0};
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

extern "C" int nr_gpu_pass_init(int max_classes)
{
  std::lock_guard<std::mutex> lk(g_mu);
  if (g_ready) return 0;
  if (!g_cfg_set || !guard_alloc()) return -1;
  for (int a = 0; a < 4; a++)
    if (nr_gpu_buf_alloc(&g_ring[a], (size_t)RING_LEN * 4)) return -1;
  if (nr_gpu_buf_alloc(&g_rot, 64 * 4)) return -1;
  for (int i = 0; i < NI; i++)
    if (alloc_inst(i) != 0) return -1;
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
  const Key k{sh->nrb_class, sh->nl, sh->ndmrs, sh->ncb_class, sh->fp32, (uint8_t)(evm != 0), (uint8_t)(sh->nl >= 2 ? qmode : 0), (uint8_t)(sh->nl == 1 && align)};
  if (!key_ok(k)) return -1;
  if (g_cache.count(k.pack())) return 0;
  return build(k);
}

extern "C" void nr_gpu_pass_counters(uint64_t o[16])
{
  memset(o, 0, 16 * sizeof(uint64_t));
  o[0] = g_launches; o[1] = g_hits; o[2] = g_fb_unseen; o[3] = g_fb_busy; o[4] = g_fb_unsup; o[5] = g_fb_err; o[6] = g_need_cpu; o[7] = g_tb_fail;
  o[8] = NI; o[9] = g_cache.size(); o[10] = g_allocs;
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
  snprintf(buf, n, "GPUPASS launches=%lu graph_hits=%lu fallback=%lu (unseen=%lu busy=%lu unsup=%lu err=%lu) need_cpu=%lu tb_fail=%lu classes=%lu instances=%lu wait_us_p50<=%d",
           (unsigned long)o[0], (unsigned long)o[1], (unsigned long)(o[2] + o[3] + o[4] + o[5]), (unsigned long)o[2], (unsigned long)o[3], (unsigned long)o[4],
           (unsigned long)o[5], (unsigned long)o[6], (unsigned long)o[7], (unsigned long)o[9], (unsigned long)o[8], tot ? 1 << p50 : 0);
}

/* test hook: marks the instances of `mask` busy (1) or frees them (0) so that a test can run on one chosen instance */
extern "C" void nr_gpu_pass_test_hold(unsigned mask, int hold)
{
  if (hold) g_busy.fetch_or(mask);
  else g_busy.fetch_and(~mask);
}

extern "C" int nr_gpu_pass_dl_try(const nr_gpu_pass_job_t *j, nr_gpu_pass_result_t *r)
{
  if (!g_ready || g_dead || !j || !r || !r->tb) { ++g_fb_unsup; return 1; }
  const Key k = key_of(j);
  const nr_gpu_demod_job_t &d = j->dj;
  const uint32_t ext = d.ext;
  if (!key_ok(k) || (int)k.nl != j->nl || (int)k.ndmrs != j->ndmrs || j->nbRx != g_cfg.nbRx || (int)j->nrb > (int)k.nrb || j->C < 1 || j->C > k.ncb ||
      (j->Z & 3) || (j->K & 7) || (j->K >> 3) > (uint32_t)KBMAX || j->G > LLR_CAP || j->G == 0 || ext > (uint32_t)EXTMAX || (d.want_evm && k.fp32 == (uint8_t)d.evm_fp64) ||
      j->it_max < 1 || j->it_max > NSNAP + 1 || !j->scr || !j->geo_y || !j->geo_h || !j->symbol_rot || !j->rxdata[0]) {
    ++g_fb_unsup;
    return 1;
  }
  Entry *e = nullptr;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    auto f = g_cache.find(k.pack());
    if (f != g_cache.end()) e = &f->second;
  }
  if (!e) {
    if (g_fb_unseen++ < 40 && getenv("ISAC_GPU_PASS_LOG_UNSEEN"))
      fprintf(stderr, "GPUPASS unseen class: nrb %u (grant %d) nl %u ndmrs %u ncb %u (C %u) evm %u qmode %u align %u fp32 %u\n", k.nrb, j->nrb, k.nl, k.ndmrs, k.ncb, j->C, k.evm, k.qmode,
              k.align, k.fp32);
    return 1;
  }
  int id = -1;
  for (unsigned m = g_busy.load(); id < 0;) {
    int i = 0;
    while (i < NI && (m >> i & 1u)) i++;
    if (i == NI) break;
    if (g_busy.compare_exchange_weak(m, m | 1u << i)) id = i;
  }
  if (id < 0) { ++g_fb_busy; return 1; }
  Inst &I = g_inst[id];
  struct Rel { int id; ~Rel() { g_busy.fetch_and(~(1u << id)); } } rel{id};
  NrGpuRtScope rt;
  if (!rt.admitted) { ++g_fb_unsup; return 1; }
  if (prepare(I, j) != 0) { ++g_fb_err; return 1; }
  const auto t0 = std::chrono::steady_clock::now();
  if (cudaGraphLaunch(e->exec[id], I.s) != cudaSuccess || cudaEventRecord(I.done, I.s) != cudaSuccess) {
    ++g_fb_err;
    g_dead = 1;
    return -1;
  }
  wait_done(I);
  const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
  if (g_dead) { ++g_fb_err; return -1; }
  ++g_launches;
  ++g_hits;
  {
    int b = 0;
    while (b < 31 && (1u << b) < (uint32_t)us) b++;
    ++g_wait_hist[b];
  }
  const ResBlk &rb = *I.RB();
  memset(r->seg_ok, 0, sizeof(r->seg_ok));
  r->tbs = j->A;
  r->gpu_us = (uint32_t)us;
  r->instance = (int8_t)id;
  r->need_cpu = rb.need_cpu || rb.err;
  r->nvar = rb.post.nvar_valid ? rb.post.nvar : 0;
  r->post = rb.post;
  r->tb_ok = 0;
  for (uint32_t s = 0; s < j->C; s++) r->seg_ok[s] = rb.seg_ok[s];
  if (rb.need_cpu) ++g_need_cpu;
  if (!r->need_cpu) {
    const bool crc_ok = j->C > 1 ? rb.tb_crc != 0 : rb.seg_crc[0] != 0;
    r->tb_ok = rb.all_ok && crc_ok;
    /* reassemble the TB exactly as passive_ldpc_decode_core: seg_bytes of every block (the CRC24B bytes of a multi-block TB are dropped) */
    const uint32_t seg_bytes = (j->K >> 3) - (j->F >> 3) - (j->C > 1 ? 3 : 0);
    uint32_t off = 0;
    for (uint32_t s = 0; s < j->C; s++, off += seg_bytes) memcpy(r->tb + off, rb.c[s], seg_bytes);
    if (!r->tb_ok) ++g_tb_fail;
  }
  nr_gpu_demod_job_t *dj = r->demod_out ? r->demod_out : &I.dj; /* the instance is ours until it is released */
  memcpy(dj, &j->dj, offsetof(nr_gpu_demod_job_t, evm));
  nr_pdsch_gpu_demod_pass_out(id, dj);
  r->log2_maxh = dj->log2_maxh;
  r->nvar_db = r->nvar ? 10.0f * log10f((float)r->nvar) : -99.0f;
  r->evm_db = r->snr_db = 0.0f;
  {
    const uint32_t *q = (const uint32_t *)((const char *)I.nvar.host + 3 * NR_GPU_CHEST_MAX_ROWS * sizeof(int)) + (size_t)(j->ndmrs * j->nl - 1) * g_cfg.nbRx;
    for (int a = 0; a < 4; a++) r->nvar_ant[a] = a < g_cfg.nbRx ? q[a] : 0;
  }
  r->qm_valid = 0;
  if (r->qm_row && nr_gpu_model() == NR_GPU_MODEL_DIRECT && j->qm_sym >= 0 && j->qm_sym < NSYM && j->qm_n > 0 && j->qm_n <= (uint32_t)N) {
    /* comp[(m * NR_GPU_DEMOD_MAX_NL + layer) * buf + i], c16 */
    memcpy(r->qm_row, (const int16_t *)I.scratch.host + 2 * (size_t)((size_t)j->qm_sym * NR_GPU_DEMOD_MAX_NL * ext), (size_t)j->qm_n * 4);
    r->qm_valid = 1;
  }
  if (r->brfo_rows && j->fo_first >= 0 && j->fo_last > j->fo_first) {
    const int16_t *e0 = (const int16_t *)I.est.host;
    for (int a = 0; a < g_cfg.nbRx; a++) {
      memcpy(r->brfo_rows + ((size_t)a * 2) * N * 2, e0 + 2 * ((size_t)(0 * g_cfg.nbRx + a) * NSYM * N + (size_t)j->dmrs_sym[j->fo_first] * N), (size_t)N * 4);
      memcpy(r->brfo_rows + ((size_t)a * 2 + 1) * N * 2, e0 + 2 * ((size_t)(0 * g_cfg.nbRx + a) * NSYM * N + (size_t)j->dmrs_sym[j->fo_last] * N), (size_t)N * 4);
    }
  }
  if (nr_gpu_model() == NR_GPU_MODEL_DISCRETE && j->rxdataF) { /* the symbols the caller needs, from the pinned copy of the instance's rxdataF */
    for (int a = 0; a < g_cfg.nbRx; a++)
      for (int m = 0; m < NSYM; m++)
        if (j->copy_mask >> m & 1) memcpy(j->rxdataF + 2 * ((size_t)a * j->rx_stride + (size_t)m * N), (const int16_t *)I.rxF.host + 2 * ((size_t)a * NSYM * N + (size_t)m * N), (size_t)N * 4);
  }
  return 0;
}
