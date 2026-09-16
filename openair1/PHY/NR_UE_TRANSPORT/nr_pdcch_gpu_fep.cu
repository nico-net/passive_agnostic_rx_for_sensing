/* GPU DM-RS LS chest + LLR for the blind PDCCH CORESET-extent search. See the header for status,
 * scope, and why decode/CRC (Polar SCL) is deliberately NOT here.
 *
 * PDCCH DM-RS, unlike PDSCH's, is present in EVERY CORESET symbol (not a subset flagged by a mask),
 * is single-port with no fd-OCC/CDM (38.211 7.4.1.3.1: one sequence, mapped directly, no despreading
 * pair), and the payload is always QPSK (38.212 7.3) -- so this kernel set is structurally simpler
 * than nr_pdsch_gpu_fep.cu's: no ports/layers loop, no MMSE, no PAM-level table, one gold sequence
 * per (job, symbol) rather than per (job, dmrs-symbol-index, port).
 *
 * RB-quantity contract (mirrors dci_nr.c's nr_rx_pdcch_symbol()/nr_pdcch_channel_estimation() call
 * exactly -- see nr_gpu_pdcch_job_t's field comments): the RE position uses
 * (bwp_start_rb + rb_offset), the pilot/Gold sequence index uses (dmrs_ref_rb + rb_offset). The two
 * CPU functions keep these separate on purpose (a common CORESET's dmrs_ref_rb is 0 even when its
 * RE position still needs bwp_start_rb added for a UE-specific BWP), and collapsing them here would
 * silently reintroduce a bug that function signature was already written to avoid.
 */
#define NR_GPU_FEP_NO_LOADER
#include "nr_pdcch_gpu_fep.h"
#include <cuda_runtime.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#define CK(x)                                                                                     \
  do {                                                                                             \
    cudaError_t e_ = (x);                                                                          \
    if (e_ != cudaSuccess) {                                                                       \
      fprintf(stderr, "nr_pdcch_gpu_fep: %s at %s:%d\n", cudaGetErrorString(e_), __FILE__, __LINE__); \
      return -1;                                                                                   \
    }                                                                                               \
  } while (0)

static const int MAX_ANT = 4;
static const int MAX_DUR = 3;         /* CORESET duration: 1..3 symbols */
static const int MAX_PILOTS_PER_JOB = 3 * 275; /* 3 DM-RS REs/RB, up to a 275-RB CORESET width */
static const int GOLD_WORDS = 64;     /* covers pilot index m up to ~1024 (2 bits/pilot); see header */
static const int MAX_JOBS = 64;       /* one launch-set chunk; chunked below like nr_pdsch_gpu_fep */
static const int RE_PER_RB_OUT_DMRS = 9;

static nr_gpu_pdcch_fep_cfg_t g_cfg;
static float2 *g_d_rxF;                 /* resident CORESET rxdataF: [ant][symbol][N] */
static float2 *g_h_rxF_stage;           /* pinned staging buffer for the int16 -> float2 upload */
static nr_gpu_pdcch_job_t *g_h_jobs, *g_d_jobs;
static uint32_t *g_d_gold;              /* [job][symbol][GOLD_WORDS] */
static float2 *g_d_ls;                  /* [job][symbol][ant][MAX_PILOTS_PER_JOB] */
static int16_t *g_h_llr, *g_d_llr;
static size_t g_llr_cap;
static cudaStream_t g_stream;
static double g_upload_us, g_llr_us;
static float g_llr_scale = 6.0f; /* int16 LLR headroom; same order as nr_pdsch_gpu_fep's g_llr_scale */

/* ---------------------------------------------------------------- Gold sequence (38.211 5.2.1) --
 * c_init = (2^17*(14*n_s,f + l + 1)*(2*N_ID+1) + 2*N_ID) mod 2^31 -- 38.211 7.4.1.3.1, no nscid term
 * (that is a PDSCH DM-RS-only quantity; PDCCH's sequence has none). One thread per (job, symbol);
 * each runs the length-31 Gold LFSR to bit GOLD_WORDS*32, identical recurrence to nr_pdsch_gpu_fep's
 * k_gold (that recurrence is 3GPP-fixed, not PDSCH-specific, so it is copied verbatim on purpose). */
__global__ void k_gold_pdcch(const nr_gpu_pdcch_job_t *__restrict__ jobs, int n, uint32_t *__restrict__ gold)
{
  const int j = blockIdx.x, s = threadIdx.x;
  if (j >= n || s >= jobs[j].duration)
    return;
  const nr_gpu_pdcch_job_t job = jobs[j];
  const int l = job.start_symbol + s;
  const uint32_t nid = job.dmrs_scrambling_id;
  uint32_t cinit = ((1u << 17) * (14u * job.slot + (uint32_t)l + 1u) * (2u * nid + 1u) + 2u * nid) & 0x7fffffffu;
  uint32_t x1 = 1, x2 = cinit;
  for (int n_ = 0; n_ < 1600; n_++) {
    x1 = (x1 >> 1) | (((x1 ^ (x1 >> 3)) & 1u) << 30);
    x2 = (x2 >> 1) | (((x2 ^ (x2 >> 1) ^ (x2 >> 2) ^ (x2 >> 3)) & 1u) << 30);
  }
  uint32_t *g = gold + ((size_t)j * MAX_DUR + s) * GOLD_WORDS;
  for (int w = 0; w < GOLD_WORDS; w++) {
    uint32_t word = 0;
    for (int b = 0; b < 32; b++) {
      word |= ((x1 ^ x2) & 1u) << b;
      x1 = (x1 >> 1) | (((x1 ^ (x1 >> 3)) & 1u) << 30);
      x2 = (x2 >> 1) | (((x2 ^ (x2 >> 1) ^ (x2 >> 2) ^ (x2 >> 3)) & 1u) << 30);
    }
    g[w] = word;
  }
}

__device__ inline float2 gold_qpsk(const uint32_t *g, int m)
{
  /* nr_pdcch_dmrs_ref() returns conj(X) already (dci_nr.c comment); mirrored here directly rather
   * than conjugating downstream, same convention nr_pdsch_gpu_fep's gold_qpsk avoids for PDSCH. */
  const uint32_t c0 = (g[(2 * m) >> 5] >> ((2 * m) & 31)) & 1u, c1 = (g[(2 * m + 1) >> 5] >> ((2 * m + 1) & 31)) & 1u;
  return make_float2((1.f - 2.f * c0) * 0.70710678f, -(1.f - 2.f * c1) * 0.70710678f);
}
__device__ inline float2 cmulc(float2 a, float2 b) { return make_float2(a.x * b.x + a.y * b.y, a.y * b.x - a.x * b.y); } /* a * conj(b) */

/* ---------------------------------------------------------------- LS chest ----
 * Mirrors nr_dl_channel_estimation.c's nr_pdcch_channel_estimation() non-CH_INTERP branch: LS at
 * each of the 3 pilot REs/RB (k = 1 + 4*p within the RB), no fd-OCC despread (PDCCH has none). One
 * thread per (job, symbol, ant, pilot index n = 0..3*n_rb-1). */
__global__ void k_ls_pdcch(const nr_gpu_pdcch_job_t *__restrict__ jobs, const uint32_t *__restrict__ gold,
                           const float2 *__restrict__ rxF, int N, int max_dur, int nant, int fco,
                           float2 *__restrict__ ls)
{
  const int j = blockIdx.y;
  const nr_gpu_pdcch_job_t job = jobs[j];
  const int npil = 3 * job.n_rb;
  const int t = blockIdx.x * blockDim.x + threadIdx.x;
  const int n = t % npil, rest = t / npil;
  if (rest >= nant * job.duration)
    return;
  const int ant = rest % nant, s = rest / nant;
  const int rb_local = n / 3, p = n % 3;
  const int k = (fco + (job.bwp_start_rb + job.rb_offset + rb_local) * 12 + 1 + 4 * p) % N;
  const int m = 3 * (job.dmrs_ref_rb + job.rb_offset + rb_local) + p;
  const uint32_t *g = gold + ((size_t)j * MAX_DUR + s) * GOLD_WORDS;
  const float2 y = rxF[(((size_t)ant * max_dur) + s) * N + k]; /* CORESET-relative row: see upload_slot()'s contract */
  const float2 x = gold_qpsk(g, m);
  ls[(((size_t)j * MAX_DUR + s) * nant + ant) * MAX_PILOTS_PER_JOB + n] = cmulc(y, x); /* LS: y * conj(x) */
}

/* ---------------------------------------------------------------- Equalise + QPSK LLR ----
 * Per-RB channel = the mean of its 3 pilot LS estimates (nr_pdcch_channel_estimation()'s own
 * averaging, spread flat across the RB -- the CH_INTERP=0 branch, which is what this cell's build
 * runs; see the .h file's known-simplification note re: the branch-roughness antenna gate this
 * skips). MRC combine over antennas (power-weighted, i.e. matched-filter sum h*conj(y)/sum|h|^2 --
 * equivalent to per-branch SNR-weighted combining, the textbook substitute for the CPU's
 * roughness-based branch zeroing). One thread per (job, symbol, data RE). */
__global__ void k_llr_pdcch(const nr_gpu_pdcch_job_t *__restrict__ jobs, const float2 *__restrict__ ls,
                            const float2 *__restrict__ rxF, int N, int max_dur, int nant, int fco,
                            int16_t *__restrict__ llr, float scale)
{
  const int j = blockIdx.y;
  const nr_gpu_pdcch_job_t job = jobs[j];
  if (job.n_llr == 0)
    return;
  const int re = blockIdx.x * blockDim.x + threadIdx.x; /* 0 .. duration*n_rb*RE_PER_RB_OUT_DMRS - 1 */
  const int nre = job.n_llr / 2;
  if (re >= nre)
    return;
  const int per_sym = job.n_rb * RE_PER_RB_OUT_DMRS;
  const int s = re / per_sym, local = re % per_sym;
  const int rb_local = local / RE_PER_RB_OUT_DMRS, re_in_rb = local % RE_PER_RB_OUT_DMRS;
  /* 9 data REs of a 12-RE RB, in increasing order, with the 3 DM-RS REs (1,5,9) removed: 0,2,3,4,6,7,8,10,11 */
  static const int DATA_KR[9] = {0, 2, 3, 4, 6, 7, 8, 10, 11};
  const int kr = DATA_KR[re_in_rb];
  const int k = (fco + (job.bwp_start_rb + job.rb_offset + rb_local) * 12 + kr) % N;

  float2 h_mean[MAX_ANT];
  for (int a = 0; a < nant; a++) {
    const float2 *row = ls + (((size_t)j * MAX_DUR + s) * nant + a) * MAX_PILOTS_PER_JOB + 3 * rb_local;
    h_mean[a] = make_float2((row[0].x + row[1].x + row[2].x) / 3.f, (row[0].y + row[1].y + row[2].y) / 3.f);
  }
  float2 z = make_float2(0.f, 0.f);
  float hp = 0.f;
  for (int a = 0; a < nant; a++) {
    const float2 y = rxF[(((size_t)a * max_dur) + s) * N + k]; /* CORESET-relative row */
    const float2 zc = cmulc(y, h_mean[a]); /* y * conj(h) */
    z.x += zc.x;
    z.y += zc.y;
    hp += h_mean[a].x * h_mean[a].x + h_mean[a].y * h_mean[a].y;
  }
  hp = fmaxf(hp, 1e-8f);
  /* QPSK, Gray: b0 <-> Re(z), b1 <-> Im(z), soft LLR proportional to the matched-filter output
   * normalised by the channel power (an SNR-scaled soft bit, not a hard slicer). Sign convention
   * (which polarity means bit 0) is NOT independently re-derived from nr_pdcch_demapping_deinterleaving
   * / nr_pdcch_unscrambling's downstream bit convention here -- that agreement is exactly what the
   * mandatory self-check (see the .h file's STATUS note) verifies before this path is trusted live;
   * a sign error would show up there as consistent CRC disagreement, not as a subtle bug. */
  const float g = scale / hp;
  int16_t *o = llr + job.llr_offset + 2 * re;
  o[0] = (int16_t)fmaxf(-32000.f, fminf(32000.f, z.x * g));
  o[1] = (int16_t)fmaxf(-32000.f, fminf(32000.f, z.y * g));
}

static int api_init(const nr_gpu_pdcch_fep_cfg_t *cfg)
{
  g_cfg = *cfg;
  const int N = cfg->ofdm_symbol_size, nant = cfg->nant;
  CK(cudaStreamCreate(&g_stream));
  CK(cudaMalloc(&g_d_rxF, (size_t)nant * MAX_DUR * N * sizeof(float2)));
  CK(cudaMallocHost(&g_h_rxF_stage, (size_t)nant * MAX_DUR * N * sizeof(float2)));
  CK(cudaMallocHost(&g_h_jobs, MAX_JOBS * sizeof(nr_gpu_pdcch_job_t)));
  CK(cudaMalloc(&g_d_jobs, MAX_JOBS * sizeof(nr_gpu_pdcch_job_t)));
  CK(cudaMalloc(&g_d_gold, (size_t)MAX_JOBS * MAX_DUR * GOLD_WORDS * sizeof(uint32_t)));
  CK(cudaMalloc(&g_d_ls, (size_t)MAX_JOBS * MAX_DUR * MAX_ANT * MAX_PILOTS_PER_JOB * sizeof(float2)));
  /* one chunk's worth: MAX_JOBS candidates at a full 273-RB*3-symbol CORESET, generous headroom */
  g_llr_cap = (size_t)MAX_JOBS * 273 * RE_PER_RB_OUT_DMRS * MAX_DUR * 2;
  CK(cudaMallocHost(&g_h_llr, g_llr_cap * sizeof(int16_t)));
  CK(cudaMalloc(&g_d_llr, g_llr_cap * sizeof(int16_t)));
  return 0;
}

/* Upload rxdataF_symb[ant]: max_duration * N c16 samples, CORESET-RELATIVE (row 0 = the CORESET's
 * OWN first symbol, i.e. the caller slices/copies starting at start_symbol before calling this --
 * NOT a full 14-symbol slot buffer). This is why every live job in one upload MUST share the same
 * (start_symbol, duration): the buffer only has room for one CORESET time-window, which is exactly
 * today's search structure (the extent search varies CORESET FREQUENCY range across candidates; the
 * CORESET time-domain window is the separate "mapping" dimension, tried one at a time -- see the
 * integration note in nr_pdcch_blind_monitor.c). k_ls_pdcch/k_llr_pdcch address this buffer by the
 * plain relative row `s`; job.start_symbol is used ONLY inside k_gold_pdcch, where the ABSOLUTE
 * symbol number is a real input to the 38.211 c_init formula, not a buffer offset -- conflating
 * those two uses was an actual bug caught while writing this (buffer index and c_init's `l` differ
 * by start_symbol and must not share one variable). */
static int api_upload_slot(const int16_t *const *rxdataF_symb, int max_duration)
{
  const auto t0 = std::chrono::steady_clock::now();
  const int N = g_cfg.ofdm_symbol_size, nant = g_cfg.nant;
  const int dur = std::min(max_duration, MAX_DUR);
  for (int a = 0; a < nant; a++)
    for (int s = 0; s < dur; s++)
      for (int k = 0; k < N; k++) {
        const int16_t re = rxdataF_symb[a][2 * (s * N + k)], im = rxdataF_symb[a][2 * (s * N + k) + 1];
        g_h_rxF_stage[(a * MAX_DUR + s) * N + k] = make_float2((float)re, (float)im);
      }
  CK(cudaMemcpyAsync(g_d_rxF, g_h_rxF_stage, (size_t)nant * MAX_DUR * N * sizeof(float2), cudaMemcpyHostToDevice, g_stream));
  CK(cudaStreamSynchronize(g_stream));
  g_upload_us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
  return 0;
}

static inline uint32_t job_total_llr(const nr_gpu_pdcch_job_t *j) { return 2u * j->n_rb * RE_PER_RB_OUT_DMRS * j->duration; }

static int64_t run_chunk(nr_gpu_pdcch_job_t *jobs, int n, int16_t *llr_out, uint32_t total)
{
  if (n == 0)
    return 0;
  memcpy(g_h_jobs, jobs, n * sizeof(nr_gpu_pdcch_job_t));
  CK(cudaMemcpyAsync(g_d_jobs, g_h_jobs, n * sizeof(nr_gpu_pdcch_job_t), cudaMemcpyHostToDevice, g_stream));
  const int N = g_cfg.ofdm_symbol_size, nant = g_cfg.nant, fco = g_cfg.first_carrier_offset;
  int max_ls = 0, max_re = 0;
  for (int i = 0; i < n; i++) {
    max_ls = std::max(max_ls, 3 * (int)jobs[i].n_rb * nant * (int)jobs[i].duration);
    max_re = std::max(max_re, (int)job_total_llr(&jobs[i]) / 2);
  }
  k_gold_pdcch<<<n, MAX_DUR, 0, g_stream>>>(g_d_jobs, n, g_d_gold);
  if (max_ls > 0)
    k_ls_pdcch<<<dim3((max_ls + 255) / 256, n), 256, 0, g_stream>>>(g_d_jobs, g_d_gold, g_d_rxF, N, MAX_DUR, nant, fco, g_d_ls);
  if (max_re > 0)
    k_llr_pdcch<<<dim3((max_re + 127) / 128, n), 128, 0, g_stream>>>(g_d_jobs, g_d_ls, g_d_rxF, N, MAX_DUR, nant, fco, g_d_llr,
                                                                     g_llr_scale);
  CK(cudaMemcpyAsync(g_h_llr, g_d_llr, (size_t)total * sizeof(int16_t), cudaMemcpyDeviceToHost, g_stream));
  CK(cudaStreamSynchronize(g_stream));
  CK(cudaGetLastError());
  memcpy(llr_out, g_h_llr, (size_t)total * sizeof(int16_t));
  return total;
}

static int64_t api_pdcch_llr(nr_gpu_pdcch_job_t *jobs, int n, int16_t *llr_out, size_t llr_cap)
{
  const auto t0 = std::chrono::steady_clock::now();
  int64_t written = 0;
  int i = 0;
  while (i < n) {
    uint32_t total = 0;
    int m = 0;
    while (i + m < n && m < MAX_JOBS) {
      nr_gpu_pdcch_job_t *j = &jobs[i + m];
      uint32_t nl = 0;
      const bool ok = j->n_rb > 0 && j->n_rb <= 273 && j->duration >= 1 && j->duration <= MAX_DUR
                      && j->start_symbol + j->duration <= g_cfg.symbols_per_slot && 3 * (int)j->n_rb <= MAX_PILOTS_PER_JOB;
      if (ok)
        nl = job_total_llr(j);
      if (m > 0 && total + nl > g_llr_cap)
        break;
      if ((size_t)written + total + nl > llr_cap)
        nl = 0; /* no room: unsupported for the caller, decode on the CPU */
      j->llr_offset = (uint32_t)(written + total);
      j->n_llr = nl;
      total += nl;
      m++;
    }
    for (int k = 0; k < m; k++)
      jobs[i + k].llr_offset -= (uint32_t)written;
    const int64_t r = run_chunk(&jobs[i], m, llr_out + written, total);
    for (int k = 0; k < m; k++)
      jobs[i + k].llr_offset += (uint32_t)written;
    if (r < 0)
      return r;
    written += r;
    i += m;
  }
  g_llr_us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
  return written;
}

static void api_timing(double *upload_us, double *llr_us)
{
  if (upload_us) *upload_us = g_upload_us;
  if (llr_us) *llr_us = g_llr_us;
}

static const nr_gpu_pdcch_fep_api_t g_api = {api_init, api_upload_slot, api_pdcch_llr, api_timing};
extern "C" const nr_gpu_pdcch_fep_api_t *nr_gpu_pdcch_fep_api(void) { return &g_api; }
