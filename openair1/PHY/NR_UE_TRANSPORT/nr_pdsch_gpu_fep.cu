/* GPU FEP + DM-RS channel estimation + MMSE + LLR for the passive PDSCH decoder. See the header. */
#include "nr_pdsch_gpu_fep.h"
#include "../TOOLS/nr_gpu_poll.h" // P99: ISAC_GPU_POLL_SLEEP_US
#include "nr_pdsch_gpu_chest.h"
#include "PHY/TOOLS/nr_gpu_mem.h"
#include "PHY/TOOLS/nr_gpu_membudget.h"
#include "nr_gpu_stage_census.h"
#include <cuda_runtime.h>
#include "PHY/TOOLS/nr_gpu_budget_cuda.h"
#include "nr_dl_gpu_memory.h"
#include "nr_dl_gpu_budget.h"
#include "nr_gpu_green.h"
#include <cufft.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include <atomic>
#include <mutex>
#include <thread>

static int g_init_state;

#define CK(x)                                                                                   \
  do {                                                                                          \
    cudaError_t e_ = (x);                                                                       \
    if (e_ != cudaSuccess) { g_init_state = -1;                                                                    \
      fprintf(stderr, "nr_pdsch_gpu_fep: %s at %s:%d\n", cudaGetErrorString(e_), __FILE__, __LINE__); \
      return -1;                                                                                \
    }                                                                                           \
  } while (0)

static const int MAX_ANT = 4, MAX_SYM = 14, MAX_PORTS = 4, MAX_DMRS_SYM = 4;
static const int GOLD_WORDS = 256; /* 8192 gold bits per (job, DM-RS symbol): m up to 4096 */
static const int MAX_PILOTS = 3 * 275; /* type-1 pilot pairs across a 275-RB carrier */
static const int MAX_JOBS = 65; /* passive queue: head + up to 64 same-slot jobs */

/* Gold is a 31-bit linear recurrence. Jump images let each lane start at its
 * own 32-bit word instead of serializing every word in one CUDA thread. */
__constant__ uint32_t g_gold_x1_word[GOLD_WORDS];
__constant__ uint32_t g_gold_x2_jump[14][31];
/* gold_step: one step of the 31-bit Gold LFSR (x2 polynomial when x2 is set).
 *   How it works: Feedback bit is inserted at bit 30 while the state shifts right.
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_fep.cu:57, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_fep.cu:62, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_fep.cu:65.
 */
static uint32_t gold_step(uint32_t v, bool x2)
{
  const uint32_t b = x2 ? (v ^ (v >> 1) ^ (v >> 2) ^ (v >> 3)) & 1u
                        : (v ^ (v >> 3)) & 1u;
  return (v >> 1) | (b << 30);
}
/* gold_apply: applies a 31x31 GF(2) jump matrix to a state.
 *   How it works: XOR of the matrix rows selected by the set state bits.
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_fep.cu:60.
 */
static uint32_t gold_apply(const uint32_t image[31], uint32_t state)
{
  uint32_t out = 0;
  while (state) {
    const int bit = __builtin_ctz(state);
    out ^= image[bit];
    state &= state - 1;
  }
  return out;
}
/* gold_init_jumps: precomputes the Gold sequence jump tables and the x1 word starts, uploads them to the device.
 *   How it works: jump[p] is the x2 matrix for 2^p steps. The x1 state is advanced 1600 steps, then 32 per word. Both tables are copied with cudaMemcpyToSymbol; returns the first error.
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_fep.cu:626.
 */
static cudaError_t gold_init_jumps(void)
{
  uint32_t jump[14][31], x1_word[GOLD_WORDS];
  for (int bit = 0; bit < 31; ++bit) jump[0][bit] = gold_step(1u << bit, true);
  for (int power = 1; power < 14; ++power)
    for (int bit = 0; bit < 31; ++bit)
      jump[power][bit] = gold_apply(jump[power - 1], jump[power - 1][bit]);
  uint32_t x1 = 1;
  for (int i = 0; i < 1600; ++i) x1 = gold_step(x1, false);
  for (int w = 0; w < GOLD_WORDS; ++w) {
    x1_word[w] = x1;
    for (int b = 0; b < 32; ++b) x1 = gold_step(x1, false);
  }
  cudaError_t e = cudaMemcpyToSymbol(g_gold_x1_word, x1_word, sizeof(x1_word));
  return e == cudaSuccess ? cudaMemcpyToSymbol(g_gold_x2_jump, jump, sizeof(jump)) : e;
}

static nr_gpu_fep_cfg_t g_cfg;
static int g_samples_per_slot;
static int16_t *g_h_rx;      /* pinned [ant][samples_per_slot + N] */
static int16_t *g_h_rot;     /* pinned symbol rotation, 14 c16 */
static short2 *g_d_rx;
static float2 *g_d_fft_in;   /* [ant*14][N] */
static float2 *g_d_rxF;      /* resident rxdataF, [ant][14][N] */
static short2 *g_d_rot;
static cufftHandle g_plan;
static cudaStream_t g_stream;
static float2 *g_h_check;
/* per-launch job buffers */
static nr_gpu_pdsch_job_t *g_h_jobs, *g_d_jobs;
static uint32_t *g_d_gold;   /* [job][dmrs_sym][GOLD_WORDS] */
static float2 *g_d_ls;       /* [job][dmrs_sym][port][ant][MAX_PILOTS] */
static float *g_d_sigma;     /* [job][ant] noise variance */
static int16_t *g_h_llr, *g_d_llr;
static size_t g_llr_cap;
static double g_fep_us, g_llr_us;
static int g_last_chunks;
static const float g_llr_scale = 4.0f;
static const int g_force_ant = -1; /* no antenna forced */
/* GPU6 bit-exact FEP (nr_pdsch_gpu_exact_fep.cuh): tables + per-slot int16 rxdataF */
static uint16_t *g_d_perm;
static short *g_d_tw1024, *g_d_tw4096;
static short2 *g_d_ts, *g_d_exact, *g_h_exact;
static bool g_exact_ok, g_exact_direct, g_exact_pending;
static int g_isa; /* nr_isa profile of the bit-exact kernels (nr_exact::isa_profile(), chosen at init); P_NONE = exact set off */
static double g_exact_us;
static const double g_exact_deadline_us = 2000;
static std::atomic<uint64_t> g_exact_late, g_exact_busy;
/* Exact-path contexts: one stream and one small mapped output each. Rotation and symbol list ride in the kernel
 * argument and the kernels read/write the caller's memory, so concurrent consumers never share a mutable buffer
 * and never serialize on a lock (direct mode). The staged mode reuses the module's single upload buffers and
 * keeps a lock. */
static const int EXACT_CTX = 8;
static cudaStream_t g_ctx_stream[EXACT_CTX];
/* ISAC_E2E_TRACE stage split: this module (dlopen RTLD_LOCAL) has its own copy of the per-thread wait accumulators; the host reads them here */
extern "C" uint64_t *nr_pdsch_gpu_ss_ptr(void) { return nr_ss_ns; }
extern "C" void nr_pdsch_gpu_ss_enable(int on) { nr_ss_on = on; }
static int *g_ctx_delay[EXACT_CTX];
static short2 *g_ctx_ls[EXACT_CTX];    /* GPU-DEMOD chest: LS rows [64][4096] */
static int *g_ctx_cout[EXACT_CTX];     /* GPU-DEMOD chest: delay triples + nvar (staging) */
static nr_gpu_chest_row_t *g_ctx_rows[EXACT_CTX]; /* GPU-DEMOD chest: the job's rows (staging) */
static std::atomic<unsigned> g_ctx_busy{0};
/* GPUPASS (launch-only _dev entry points): 4-slot IQ window ring for the DISCRETE model (pinned host + device, allocated before
 * the RX fence) and the chest LS scratch of the single pass in flight. */
static const int PASS_RING = NR_GPU_PASS_RING;
static const int PASS_SLOTS = NR_GPU_PASS_RING + 1; /* [0, RING): graph-pass instances; [RING]: the test-only round-robin _stage helpers (never a live instance's slot) */
static short2 *g_pass_h[PASS_SLOTS], *g_pass_d[PASS_SLOTS], *g_pass_ls[PASS_SLOTS];
static const int PASS_LS_ROWS = 1024; /* v2 split: 32 segments (nr_gpu_pass_graph.cu SEG_ROWS); LS rows per slot: a graph pass needs nd x nl x nrx x segments rows (nr_gpu_pass_graph.cu SEG_ROWS): 2 x 4 x 4 x 16 or 3 x 4 x 4 x 8 */
static uint32_t *g_pass_par[PASS_SLOTS];              /* per slot staging block read by the kernel (nr_exact::ExactIO::par layout) */
static nr_gpu_chest_row_t *g_pass_rh[PASS_SLOTS], *g_pass_rd[PASS_SLOTS]; /* chest rows: pinned staging, device copy */
static cudaEvent_t g_pass_fev[PASS_SLOTS], g_pass_cev[PASS_SLOTS];        /* recorded when a pass is done with the slot */
static std::atomic<unsigned> g_pass_next{0}, g_pass_cnext{0};
/* the done-events are recorded with cudaEventRecordExternal: inside a stream capture that makes a real event-record node of the graph, so
 * cudaEventQuery on the event stays legal after the capture (a plain captured record would turn it into a capture-only event) */
static cudaError_t pass_slot_done(cudaEvent_t ev, cudaStream_t s)
{
  cudaStreamCaptureStatus st = cudaStreamCaptureStatusNone;
  cudaStreamIsCapturing(s, &st);
  return st == cudaStreamCaptureStatusActive ? cudaEventRecordWithFlags(ev, s, cudaEventRecordExternal) : cudaEventRecord(ev, s);
}
static void pass_slot_wait(cudaEvent_t ev) /* host: the slot's previous pass has finished */
{
  uint64_t pt = 0;
  while (cudaEventQuery(ev) == cudaErrorNotReady) nr_gpu_poll_relax(&pt);
}
static std::mutex g_exact_staged_lock;
/* Q4: DISCRETE staging of the standalone chest / delay entries. Their kernels read caller (host) memory in place on DIRECT; on DISCRETE each exact context owns
 * one pinned+device block pair: `in` = [row descriptors][the rx span of each row][the pilot span of each distinct pilot row], one async H2D of exactly the
 * bytes used; `out` = [estimate rows][delay triples, nvar], one async D2H. The delay table is uploaded once at init (nr_pdsch_gpu_exact_stage_init). A job above
 * stg::rows runs as consecutive chunks: every row is independent, so the result is bit-identical. */
namespace stg {
static int rows;                        /* rows per chunk; 0 = staging off (the entries refuse, the caller runs the CPU) */
static const size_t RXE = 3312, PILE = 1656; /* c16 per row, 16 B multiples: 12 * 275 PRB, 6 * 275 */
struct Ctx { nr_gpu_buf_t in, out; };
static Ctx c[EXACT_CTX];
static nr_gpu_buf_t dt;                 /* device copy of the caller's delay table */
static const int16_t *dt_host;
static int dt_stride, dt_mdc;
static uint64_t dt_fp;                  /* sampled fingerprint of the table at upload: a table changed in place makes the chest refuse (CPU) */
static uint64_t fingerprint(const int16_t *t, int stride, int mdc)
{
  const uint32_t *w = (const uint32_t *)t;
  const size_t n = (size_t)(2 * mdc + 1) * stride;
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; i += 61) {
    h ^= w[i];
    h *= 1099511628211ull;
  }
  return h;
}
static size_t rowb(int n) { return ((size_t)n * sizeof(nr_gpu_chest_row_t) + 255) & ~(size_t)255; }
static size_t in_bytes(int r) { return rowb(r) + (size_t)r * (RXE + PILE) * sizeof(short2); }
static size_t out_bytes(int r) { return (size_t)r * 4096 * sizeof(short2) + (size_t)4 * r * sizeof(int); }
static bool ready(void) { return rows > 0; }
} // namespace stg
/* ctx_acquire: claims a free bit-exact FEP context (bitmask compare-and-swap over EXACT_CTX).
 *   How it works: try_only returns -1 when all are busy; otherwise it spins with yield.
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_fep.cu:894, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_fep.cu:974.
 */
static int ctx_acquire(bool try_only)
{
  const uint64_t ss0 = nr_ss_now();
  for (;;) {
    unsigned m = g_ctx_busy.load();
    for (int i = 0; i < nr_gpu_ctx_limit(EXACT_CTX); i++)
      if (!(m >> i & 1u) && g_ctx_busy.compare_exchange_strong(m, m | 1u << i)) {
        nr_ss_add(NRSS_LK, ss0);
        return i;
      }
    if (try_only) return -1;
    std::this_thread::yield();
  }
}
/* ctx_release: releases FEP context i.
 *   How it works: Clears bit i of g_ctx_busy.
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_fep.cu:897, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_fep.cu:909, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_fep.cu:976, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_fep.cu:985.
 */
static void ctx_release(int i) { g_ctx_busy.fetch_and(~(1u << i)); }
/* Wait for a context's stream; returns 0, or a CUDA error. Counts calls past the deadline. */
/* ctx_wait: waits for the stream of context c to finish, polling cudaStreamQuery.
 *   How it works: Counts a late completion once when the wait passes g_exact_deadline_us. Returns 0 or the CUDA error.
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_fep.cu:908, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_fep.cu:983.
 */
static int ctx_wait(int c, std::chrono::steady_clock::time_point t0, int sb)
{
  bool late = false;
  uint64_t pt = 0;
  const uint64_t ss0 = nr_ss_now();
  for (;;) {
    const cudaError_t q = cudaStreamQuery(g_ctx_stream[c]);
    if (q != cudaErrorNotReady) nr_ss_add(sb, ss0);
    if (q == cudaSuccess) return 0;
    if (q != cudaErrorNotReady) return (int)q;
    nr_gpu_poll_relax(&pt); /* P99: ISAC_GPU_POLL_SLEEP_US */
    if (!late && std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() > g_exact_deadline_us) {
      late = true;
      ++g_exact_late;
    }
  }
}

/* ---------------------------------------------------------------- FEP ---- */
/* Gather one OFDM symbol per (ant, sym): CP removal with the 1/8-CP early window, ring wrap, FO
 * de-rotation (phase origin = abs_sample, as nr_fo_compensation), int16 -> float. */
/* rx[ant] holds the slot window starting `early` samples BEFORE the slot's CP, so symbol l's FFT window
 * (OAI: its data start minus early) is at buffer index ncp0 + l * (N + ncp); abs_sample0 is the
 * absolute index of buffer index 0. */
__global__ void k_fep_gather(const short2 *__restrict__ rx, long long abs_sample0, float phase_inc, int N, int ncp,
                             int ncp0, int nsym, int slot_len, float2 *__restrict__ out)
{
  const int sym = blockIdx.y, ant = blockIdx.z;
  const int n = blockIdx.x * blockDim.x + threadIdx.x;
  if (n >= N)
    return;
  const int sym_start = ncp0 + sym * (N + ncp); /* symbol 0 carries the long CP, the rest the normal one */
  const int i = sym_start + n;
  const short2 s = rx[(size_t)ant * (slot_len + N) + i];
  float2 v = make_float2(s.x, s.y);
  if (phase_inc != 0.f) {
    /* phase = (abs_sample + i) * phase_inc turns; keep the fractional part in double precision */
    double ph = (double)(abs_sample0 + i) * (double)phase_inc;
    ph -= floor(ph);
    float sn, cs;
    sincosf((float)(2.0 * M_PI * ph), &sn, &cs);
    v = make_float2(v.x * cs - v.y * sn, v.x * sn + v.y * cs);
  }
  out[((size_t)ant * nsym + sym) * N + n] = v;
}

/* OAI's apply_nr_rotation_symbol_RX: multiply by conj(symbol_rotation[sym]) and the timeshift ramp
 * exp(+j 2 pi k early / N) (index k of the FFT output, as OAI applies it). */
/* k_fep_rotate: GPU kernel: applies the per-symbol rotation and the early-CP phase ramp to the FFT output, then scales it.
 *   How it works: One thread per (subcarrier, symbol, antenna). Optional rot[sym] multiply, then exp(-j 2 pi k early / N), then a 1/sqrt(N) scale to match the CPU dft scaling.
 *   ISAC_* flags read: none.
 *   Callers: api_fep_slot (<<<>>> launch).
 */
__global__ void k_fep_rotate(float2 *__restrict__ rxF, const short2 *__restrict__ rot, int N, int nsym, int early)
{
  const int sym = blockIdx.y, ant = blockIdx.z;
  const int k = blockIdx.x * blockDim.x + threadIdx.x;
  if (k >= N)
    return;
  float2 *p = &rxF[((size_t)ant * nsym + sym) * N + k];
  float2 v = *p;
  if (rot) {
    const float rr = rot[sym].x / 32767.f, ri = -rot[sym].y / 32767.f;
    v = make_float2(v.x * rr - v.y * ri, v.x * ri + v.y * rr);
  }
  float sn, cs;
  sincosf(2.f * (float)M_PI * (float)k * (float)early / (float)N, &sn, &cs);
  /* OAI's ARM dft(..., scale=1) scales each radix stage, for a net 1/sqrt(N).
   * cuFFT is unnormalised. Keep the FFT output in the same dynamic range as the
   * CPU rxdataF before LS estimation and diagnostic int16 readback. */
  const float fft_scale = rsqrtf((float)N);
  *p = make_float2((v.x * cs - v.y * sn) * fft_scale,
                   (v.x * sn + v.y * cs) * fft_scale);
}

/* ----------------------------------------------------------- DM-RS gold ---- */
/* gold_jump_x2: device helper: advances the x2 Gold state by steps using the jump tables.
 *   How it works: Binary decomposition of steps; each set bit XORs the matching jump matrix applied to the state.
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_fep.cu:231.
 */
__device__ inline uint32_t gold_jump_x2(uint32_t state, int steps)
{
  for (int power = 0; steps; ++power, steps >>= 1)
    if (steps & 1) {
      uint32_t next = 0;
      while (state) {
        const int bit = __ffs((int)state) - 1;
        next ^= g_gold_x2_jump[power][bit];
        state &= state - 1;
      }
      state = next;
    }
  return state;
}

/* 38.211 5.2.1, one block per (job, DM-RS symbol), one lane per Gold word. */
/* k_gold: GPU kernel: generates the DM-RS Gold words for one (job, DM-RS symbol).
 *   How it works: Finds the target DM-RS symbol in dmrs_mask. cinit comes from slot, symbol, dmrs_scrambling_id and nscid. Words needed = 4*ppr per RB up to end_rb. Each thread builds one 32-bit word with the x1 and x2 streams.
 *   ISAC_* flags read: none.
 *   Callers: run_chunk (<<<>>> launch).
 */
__global__ void k_gold(const nr_gpu_pdsch_job_t *__restrict__ jobs, uint32_t *__restrict__ gold, int nsym)
{
  const int j = blockIdx.x, target_di = blockIdx.y;
  const nr_gpu_pdsch_job_t job = jobs[j];
  int di = 0, l = -1;
  for (int s = 0, seen = 0; s < nsym; s++)
    if (job.dmrs_mask >> s & 1) {
      if (seen++ == target_di) {
        di = seen - 1;
        l = s;
      }
    }
  if (l < 0)
    return;
  const uint32_t nid = job.dmrs_scrambling_id;
  uint32_t cinit = ((1u << 17) * (14u * job.slot + l + 1) * (2u * nid + 1) + 2u * nid + job.nscid) & 0x7fffffffu;
  uint32_t *g = gold + ((size_t)j * MAX_DMRS_SYM + di) * GOLD_WORDS;
  /* The old loop always generated 8192 bits. The largest pilot index this
   * allocation can read needs only 4*ppr bits per RB from dmrs_ref_rb. On the
   * recorded 5/48-RB jobs this removes most of the serial Gold work. */
  const int ppr = job.dmrs_type == 1 ? 3 : 2;
  const int end_rb = job.start_rb + job.nb_rb - job.dmrs_ref_rb;
  const int words = (4 * ppr * end_rb + 31) / 32;
  if (end_rb <= 0 || words > GOLD_WORDS) return;
  for (int w = threadIdx.x; w < words; w += blockDim.x) {
    uint32_t x1 = g_gold_x1_word[w];
    uint32_t x2 = gold_jump_x2(cinit, 1600 + 32 * w);
    uint32_t word = 0;
    for (int b = 0; b < 32; b++) {
      word |= ((x1 ^ x2) & 1u) << b;
      x1 = (x1 >> 1) | (((x1 ^ (x1 >> 3)) & 1u) << 30);
      x2 = (x2 >> 1) | (((x2 ^ (x2 >> 1) ^ (x2 >> 2) ^ (x2 >> 3)) & 1u) << 30);
    }
    g[w] = word;
  }
}

/* ------------------------------------------------------------- LS chest ---- */
__device__ inline int dmrs_delta(int type, int port) { return type == 1 ? ((port >> 1) & 1) : 2 * ((port >> 1) % 3); }
__device__ inline int dmrs_wf(int type, int port) { return (port & 1) ? -1 : 1; }
__device__ inline int dmrs_double(int type, int port) { return type == 1 ? port >= 4 : port >= 6; }
__device__ inline int pilots_per_rb(int type) { return type == 1 ? 3 : 2; }
/* pilot pair n of a port: REs (rb-relative to start of the carrier grid) k0 = base and k1 = k0 + gap */
__device__ inline int pilot_k0(int type, int n, int delta) { return type == 1 ? 4 * n + delta : 6 * n + delta; }
__device__ inline int pilot_gap(int type) { return type == 1 ? 2 : 1; }
__device__ inline float2 gold_qpsk(const uint32_t *g, int m)
{
  const uint32_t c0 = (g[(2 * m) >> 5] >> ((2 * m) & 31)) & 1u, c1 = (g[(2 * m + 1) >> 5] >> ((2 * m + 1) & 31)) & 1u;
  return make_float2((1.f - 2.f * c0) * 0.70710678f, (1.f - 2.f * c1) * 0.70710678f);
}
__device__ inline float2 cmul(float2 a, float2 b) { return make_float2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x); }
__device__ inline float2 cmulc(float2 a, float2 b) { return make_float2(a.x * b.x + a.y * b.y, a.y * b.x - a.x * b.y); } /* a * conj(b) */

/* One thread per (job, dmrs symbol, port, ant, pilot pair): despread the fd-OCC pair to one LS estimate. */
__global__ void k_ls(const nr_gpu_pdsch_job_t *__restrict__ jobs, const uint32_t *__restrict__ gold,
                     const float2 *__restrict__ rxF, int N, int nsym, int nant, int fco, float2 *__restrict__ ls)
{
  const int j = blockIdx.y;
  const nr_gpu_pdsch_job_t job = jobs[j];
  const int ppr = pilots_per_rb(job.dmrs_type);
  const int npil = job.nb_rb * ppr;
  const int t = blockIdx.x * blockDim.x + threadIdx.x;
  const int n = t % npil, rest = t / npil;
  if (rest >= nant * job.Nl * MAX_DMRS_SYM)
    return;
  const int ant = rest % nant, port_i = (rest / nant) % job.Nl, di = rest / (nant * job.Nl);
  int l = -1, seen = 0;
  for (int s = 0; s < nsym; s++)
    if (job.dmrs_mask >> s & 1) {
      if (seen == di)
        l = s;
      seen++;
    }
  if (l < 0)
    return;
  const int port = job.ports[port_i];
  const int delta = dmrs_delta(job.dmrs_type, port), wf = dmrs_wf(job.dmrs_type, port);
  /* wt: the second symbol of a double-symbol pair is negated for the upper ports */
  int wt = 1;
  if (dmrs_double(job.dmrs_type, port) && (di & 1))
    wt = -1;
  const uint32_t *g = gold + ((size_t)j * MAX_DMRS_SYM + di) * GOLD_WORDS;
  /* sequence index of this pilot pair: m = 2*(ppr*(rb - ref) + n_local) + k' */
  const int rb_abs = job.start_rb + n / ppr;
  const int m0 = 2 * (ppr * (rb_abs - job.dmrs_ref_rb) + n % ppr);
  const int k0 = fco + 12 * job.start_rb + pilot_k0(job.dmrs_type, n, delta);
  const int k1 = k0 + pilot_gap(job.dmrs_type);
  const float2 *sym = rxF + ((size_t)ant * nsym + l) * N;
  const float2 y0 = sym[k0 % N], y1 = sym[k1 % N];
  const float2 r0 = gold_qpsk(g, m0), r1 = gold_qpsk(g, m0 + 1);
  float2 h0 = cmulc(y0, r0), h1 = cmulc(y1, r1);
  /* TS 38.214 table 4.1-1: PDSCH EPRE to DM-RS EPRE is 0 / -3 / -4.77 dB for 1 / 2 / 3 CDM groups
   * without data, so the data sees the DM-RS-measured channel divided by sqrt(groups). Measured
   * missing on the rank-4 bed (2 groups): every |x| 0.707x, LLR bits b2/b4/b6 75/62/50 % agreement. */
  const float beta = rsqrtf((float)(job.n_cdm_groups_no_data > 0 ? job.n_cdm_groups_no_data : 1));
  float2 h = make_float2(0.5f * wt * beta * (h0.x + wf * h1.x), 0.5f * wt * beta * (h0.y + wf * h1.y));
  ls[(((size_t)j * MAX_DMRS_SYM + di) * MAX_PORTS + port_i) * MAX_ANT * MAX_PILOTS + (size_t)ant * MAX_PILOTS + n] = h;
}

/* Per (job, ant): noise variance from adjacent-pilot differences of layer 0 on the first DM-RS symbol. */
__global__ void k_sigma(const nr_gpu_pdsch_job_t *__restrict__ jobs, const float2 *__restrict__ ls, float *__restrict__ sigma)
{
  const int j = blockIdx.x, ant = blockIdx.y;
  const nr_gpu_pdsch_job_t job = jobs[j];
  const int npil = job.nb_rb * pilots_per_rb(job.dmrs_type);
  const float2 *h = ls + (((size_t)j * MAX_DMRS_SYM + 0) * MAX_PORTS + 0) * MAX_ANT * MAX_PILOTS + (size_t)ant * MAX_PILOTS;
  __shared__ float sd[256], sp[256];
  float d = 0.f, p = 0.f;
  for (int n = threadIdx.x; n + 1 < npil; n += blockDim.x) {
    const float dx = h[n + 1].x - h[n].x, dy = h[n + 1].y - h[n].y;
    d += 0.5f * (dx * dx + dy * dy);
    p += h[n].x * h[n].x + h[n].y * h[n].y;
  }
  sd[threadIdx.x] = d;
  sp[threadIdx.x] = p;
  __syncthreads();
  for (int s = blockDim.x / 2; s > 0; s >>= 1) {
    if (threadIdx.x < s) {
      sd[threadIdx.x] += sd[threadIdx.x + s];
      sp[threadIdx.x] += sp[threadIdx.x + s];
    }
    __syncthreads();
  }
  if (threadIdx.x == 0) {
    const float var = sd[0] / fmaxf(1.f, (float)(npil - 1));
    const float pw = sp[0] / fmaxf(1.f, (float)(npil - 1));
    sigma[j * MAX_ANT + ant] = fmaxf(var, 1e-4f * pw + 1e-12f); /* floor: never whiten by ~0 */
  }
}

/* --------------------------------------------------------- MMSE + LLR ---- */
/* Data-RE bookkeeping in nr_rx_pdsch()'s order: symbols in order, REs in increasing k, DM-RS REs of
 * the CDM groups without data skipped. */
__device__ inline int dmrs_re_is_data(int type, int ngrp, int kr /* k within RB */)
{
  if (type == 1)
    return ngrp >= 2 ? 0 : (kr & 1);
  return (kr % 6) >= 2 * ngrp;
}
__device__ inline int data_re_per_rb(int type, int ngrp, int is_dmrs)
{
  if (!is_dmrs)
    return 12;
  return type == 1 ? (ngrp >= 2 ? 0 : 6) : 12 - 4 * ngrp;
}
/* index of data RE kr in a DM-RS symbol's RB, given it is data */
__device__ inline int dmrs_data_index(int type, int ngrp, int kr)
{
  if (type == 1)
    return kr >> 1;
  const int per = 6 - 2 * ngrp;
  return (kr / 6) * per + (kr % 6 - 2 * ngrp);
}

/* linear frequency interpolation of a port/ant LS row at RE k (RB-relative to the allocation) */
__device__ inline float2 interp_f(const float2 *row, int type, int delta, int npil, float k)
{
  /* pilot pair n sits at k_c(n) = pilot_k0 + gap/2 */
  const float per = type == 1 ? 4.f : 6.f, off = delta + 0.5f * pilot_gap(type);
  float x = (k - off) / per;
  if (x <= 0.f)
    return row[0];
  if (x >= (float)(npil - 1))
    return row[npil - 1];
  const int n = (int)x;
  const float w = x - n;
  return make_float2(row[n].x + w * (row[n + 1].x - row[n].x), row[n].y + w * (row[n + 1].y - row[n].y));
}

__device__ inline float pam_level(int qm, int code)
{
  /* 38.211 5.1: I = (1-2b0)[2^(h-1) - (1-2b2)[... - (1-2b_{2h-2})]] / sqrt(norm); bits of the axis packed
   * LSB-first in `code` (bit i of code = b_{2i}). */
  /* innermost first: 2 pairs with b_{Qm-2} (code bit h-1), 4 with the next, ..., 2^(h-1) with b2 (code bit 1) */
  const int h = qm / 2;
  float a = 1.f;
  for (int i = 1; i < h; i++)
    a = (float)(1 << i) - (1.f - 2.f * ((code >> (h - i)) & 1)) * a;
  a *= (1.f - 2.f * (code & 1));
  const float norm = qm == 2 ? 2.f : qm == 4 ? 10.f : qm == 6 ? 42.f : 170.f;
  return a * rsqrtf(norm);
}

/* One thread per (job, data RE). */
/* k_llr: GPU kernel: PDSCH soft LLRs, one thread per data RE.
 *   How it works: Locates (symbol, subcarrier) of the RE from the job layout. Uses the DM-RS symbols bracketing it for time interpolation of the LS estimate, and the per-antenna sigma. force_ant (NR_GPU_FORCE_ANT) restricts it to one antenna. TODO(doc): the demapping tail after the time-interpolation setup was not reviewed.
 *   ISAC_* flags read: none.
 *   Callers: run_chunk (<<<>>> launch).
 */
__global__ void k_llr(const nr_gpu_pdsch_job_t *__restrict__ jobs, const float2 *__restrict__ ls, const float *__restrict__ sigma,
                      const float2 *__restrict__ rxF, int N, int nsym, int nant, int fco, int16_t *__restrict__ llr, float scale,
                      int force_ant)
{
  const int j = blockIdx.y;
  const nr_gpu_pdsch_job_t job = jobs[j];
  if (job.n_llr == 0)
    return;
  const int re = blockIdx.x * blockDim.x + threadIdx.x;
  const int nre = job.n_llr / (job.Nl * job.Qm);
  if (re >= nre)
    return;
  /* locate (symbol, k) of data RE `re` */
  int sym = job.start_symbol, idx = re;
  for (;; sym++) {
    const int is_d = (job.dmrs_mask >> sym) & 1;
    const int per = data_re_per_rb(job.dmrs_type, job.n_cdm_groups_no_data, is_d) * job.nb_rb;
    if (idx < per)
      break;
    idx -= per;
  }
  const int is_d = (job.dmrs_mask >> sym) & 1;
  int k; /* allocation-relative subcarrier */
  if (!is_d)
    k = idx;
  else if (job.dmrs_type == 1)
    k = 2 * idx + 1;
  else {
    const int per = 6 - 2 * job.n_cdm_groups_no_data;
    k = (idx / per) * 6 + 2 * job.n_cdm_groups_no_data + idx % per;
  }
  /* DM-RS symbols bracketing `sym` for time interpolation */
  int lp = -1, ln = -1, dp = -1, dn = -1;
  for (int s = 0, d = 0; s < nsym; s++)
    if (job.dmrs_mask >> s & 1) {
      if (s <= sym) { lp = s; dp = d; }
      else if (ln < 0) { ln = s; dn = d; }
      d++;
    }
  float wt = 0.f;
  if (job.time_interp && lp >= 0 && ln >= 0)
    wt = (float)(sym - lp) / (float)(ln - lp);
  if (lp < 0) { lp = ln; dp = dn; wt = 0.f; }
  const int npil = job.nb_rb * pilots_per_rb(job.dmrs_type);
  const int Nl = job.Nl;
  /* whitened H (nant x Nl) and y (nant) */
  float2 H[MAX_ANT][MAX_PORTS], y[MAX_ANT];
  const float2 *symp = rxF + (size_t)sym * N;
  const int kk = (fco + 12 * job.start_rb + k) % N;
  for (int a = 0; a < nant; a++) {
    const float inv = force_ant >= 0 && a != force_ant ? 0.f : rsqrtf(sigma[j * MAX_ANT + a]);
    const float2 ya = symp[(size_t)a * nsym * N + kk];
    y[a] = make_float2(ya.x * inv, ya.y * inv);
    for (int l = 0; l < Nl; l++) {
      const int delta = dmrs_delta(job.dmrs_type, job.ports[l]);
      const float2 *rp = ls + (((size_t)j * MAX_DMRS_SYM + dp) * MAX_PORTS + l) * MAX_ANT * MAX_PILOTS + (size_t)a * MAX_PILOTS;
      float2 h = interp_f(rp, job.dmrs_type, delta, npil, (float)k);
      if (wt > 0.f) {
        const float2 *rn = ls + (((size_t)j * MAX_DMRS_SYM + dn) * MAX_PORTS + l) * MAX_ANT * MAX_PILOTS + (size_t)a * MAX_PILOTS;
        const float2 hn = interp_f(rn, job.dmrs_type, delta, npil, (float)k);
        h = make_float2(h.x + wt * (hn.x - h.x), h.y + wt * (hn.y - h.y));
      }
      H[a][l] = make_float2(h.x * inv, h.y * inv);
    }
  }
  /* A = H^H H + I (noise whitened to unit variance), b = H^H y */
  float2 A[MAX_PORTS][MAX_PORTS], Ai[MAX_PORTS][MAX_PORTS], b[MAX_PORTS];
  for (int r = 0; r < Nl; r++) {
    b[r] = make_float2(0.f, 0.f);
    for (int a = 0; a < nant; a++) {
      const float2 t = cmulc(y[a], H[a][r]);
      b[r].x += t.x;
      b[r].y += t.y;
    }
    for (int c = 0; c < Nl; c++) {
      float2 s = make_float2(r == c ? 1.f : 0.f, 0.f);
      for (int a = 0; a < nant; a++) {
        const float2 t = cmulc(H[a][c], H[a][r]);
        s.x += t.x;
        s.y += t.y;
      }
      A[r][c] = s;
      Ai[r][c] = make_float2(r == c ? 1.f : 0.f, 0.f);
    }
  }
  /* Gauss-Jordan inverse (Hermitian positive definite: no pivoting needed) */
  for (int p = 0; p < Nl; p++) {
    const float2 d = A[p][p];
    const float id = 1.f / (d.x * d.x + d.y * d.y);
    const float2 dinv = make_float2(d.x * id, -d.y * id);
    for (int c = 0; c < Nl; c++) {
      A[p][c] = cmul(A[p][c], dinv);
      Ai[p][c] = cmul(Ai[p][c], dinv);
    }
    for (int r = 0; r < Nl; r++) {
      if (r == p)
        continue;
      const float2 f = A[r][p];
      for (int c = 0; c < Nl; c++) {
        const float2 t1 = cmul(f, A[p][c]), t2 = cmul(f, Ai[p][c]);
        A[r][c].x -= t1.x; A[r][c].y -= t1.y;
        Ai[r][c].x -= t2.x; Ai[r][c].y -= t2.y;
      }
    }
  }
  int16_t *out = llr + job.llr_offset + (size_t)re * Nl * job.Qm;
  const int hbits = job.Qm / 2, ncode = 1 << hbits;
  for (int l = 0; l < Nl; l++) {
    float2 x = make_float2(0.f, 0.f);
    for (int c = 0; c < Nl; c++) {
      const float2 t = cmul(Ai[l][c], b[c]);
      x.x += t.x;
      x.y += t.y;
    }
    /* rho = 1 - sigma^2 (A^-1)_ll with sigma^2 = 1 after whitening; unbiased x/rho, SNR rho/(1-rho) */
    const float rho = fminf(fmaxf(1.f - Ai[l][l].x, 1e-4f), 1.f - 1e-4f);
    const float snr = rho / (1.f - rho);
    const float xi = x.x / rho, xq = x.y / rho;
    for (int axis = 0; axis < 2; axis++) {
      const float v = axis ? xq : xi;
      float d0[4], d1[4];
      for (int i = 0; i < hbits; i++) { d0[i] = 1e30f; d1[i] = 1e30f; }
      for (int code = 0; code < ncode; code++) {
        const float e = v - pam_level(job.Qm, code);
        const float d = e * e;
        for (int i = 0; i < hbits; i++) {
          if ((code >> i) & 1) d1[i] = fminf(d1[i], d);
          else d0[i] = fminf(d0[i], d);
        }
      }
      for (int i = 0; i < hbits; i++) {
        float v_llr = snr * (d1[i] - d0[i]) * scale;
        v_llr = fminf(fmaxf(v_llr, -32767.f), 32767.f);
        out[(size_t)l * job.Qm + 2 * i + axis] = (int16_t)rintf(v_llr);
      }
    }
  }
}

#include "nr_pdsch_gpu_exact_fep.cuh"

/* ------------------------------------------------------------- host API ---- */
static nr_dl_gpu_budget_t g_budget;
extern "C" void nr_dl_gpu_transfer_stats(uint64_t *bytes, uint64_t *refused)
{ *bytes = __atomic_load_n(&g_budget.bytes, __ATOMIC_RELAXED); *refused = __atomic_load_n(&g_budget.refused, __ATOMIC_RELAXED); }
/* "dft_twin=<neon|avx2|none> rot_twin=<qrdmx|v80|madd|none>": the ISA twins of the bit-exact kernels, chosen by the
 * self-test against the host CPU at the first init (nr_exact::isa_profile); "none" before it or when no twin matches */
extern "C" const char *nr_gpu_isa_twin(void) { return nr_exact::g_isa_desc; }

/* allocate_init: sets up the PDSCH GPU front end: config check, highest-priority stream, buffers, cuFFT plan and the bit-exact working set.
 *   How it works: Validates the config, then creates the stream with the top priority. Allocates staging, device and shared buffers for rx, rotation, FFT input and output, jobs, Gold, LS, sigma and LLRs (32 Mi LLRs). Creates the cuFFT C2C plan for N with nant*ns batches. NR_GPU_LLR_SCALE and NR_GPU_FORCE_ANT are read here. The bit-exact working set (N = 4096) is set up at the end. TODO(doc): the tail after the bit-exact setup was not reviewed.
 *   ISAC_* flags read: ISAC_PDSCH_GPU_BATCH_ASYNC, ISAC_PDSCH_GPU_EXACT_DEADLINE_US, ISAC_PDSCH_GPU_EXACT_DIRECT.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_fep.cu:641.
 */
static int allocate_init(const nr_gpu_fep_cfg_t *cfg)
{
  if (!nr_gpu_module_ready()) return -1;
  g_cfg = *cfg;
  if (cfg->nant < 1 || cfg->nant > MAX_ANT || cfg->symbols_per_slot < 1 || cfg->symbols_per_slot > MAX_SYM
      || cfg->ofdm_symbol_size <= 0 || cfg->ofdm_offset_divisor <= 0 || cfg->samples_per_ms <= 0
      || cfg->n_rb_dl < 1 || cfg->n_rb_dl > 275)
    return -1;
  const int N = cfg->ofdm_symbol_size, ns = cfg->symbols_per_slot;
  g_samples_per_slot = cfg->nb_prefix_samples0 + N + (ns - 1) * (N + cfg->nb_prefix_samples);
  /* PDSCH FEP always runs on the highest-priority stream: its IQ has a live deadline, other GPU users (CSI-RS/nID
   * discovery) do not. Buffers come from the platform memory-model API (T18). */
  {
    int least = 0, greatest = 0;
    CK(cudaDeviceGetStreamPriorityRange(&least, &greatest));
    if (!nr_gpu_green_stream(&g_stream, greatest, true)) CK(cudaStreamCreateWithPriority(&g_stream, cudaStreamNonBlocking, greatest));
  }
  CK(nr_gpu_allocate(NR_GPU_STAGING, (void **)&g_h_rx, (size_t)cfg->nant * (g_samples_per_slot + N) * sizeof(short2)));
  CK(nr_gpu_allocate(NR_GPU_STAGING, (void **)&g_h_rot, MAX_SYM * sizeof(short2)));
  CK(nr_gpu_allocate(NR_GPU_DEVICE, (void **)&g_d_rx, (size_t)cfg->nant * (g_samples_per_slot + N) * sizeof(short2)));
  CK(nr_gpu_allocate(NR_GPU_DEVICE, (void **)&g_d_fft_in, (size_t)cfg->nant * ns * N * sizeof(float2)));
  CK(nr_gpu_allocate(NR_GPU_DEVICE, (void **)&g_d_rxF, (size_t)cfg->nant * ns * N * sizeof(float2)));
  CK(nr_gpu_allocate(NR_GPU_DEVICE, (void **)&g_d_rot, MAX_SYM * sizeof(short2)));
  CK(nr_gpu_allocate(NR_GPU_SHARED_RW, &g_h_check, (size_t)ns * N * sizeof(float2)));
  if (cufftPlan1d(&g_plan, N, CUFFT_C2C, cfg->nant * ns) != CUFFT_SUCCESS)
    return -1;
  cufftSetStream(g_plan, g_stream);
  CK(nr_gpu_allocate(NR_GPU_STAGING, (void **)&g_h_jobs, MAX_JOBS * sizeof(nr_gpu_pdsch_job_t)));
  CK(nr_gpu_allocate(NR_GPU_DEVICE, (void **)&g_d_jobs, MAX_JOBS * sizeof(nr_gpu_pdsch_job_t)));
  CK(nr_gpu_allocate(NR_GPU_DEVICE, (void **)&g_d_gold, (size_t)MAX_JOBS * MAX_DMRS_SYM * GOLD_WORDS * sizeof(uint32_t)));
  CK(nr_gpu_allocate(NR_GPU_DEVICE, (void **)&g_d_ls, (size_t)MAX_JOBS * MAX_DMRS_SYM * MAX_PORTS * MAX_ANT * MAX_PILOTS * sizeof(float2)));
  CK(nr_gpu_allocate(NR_GPU_DEVICE, (void **)&g_d_sigma, (size_t)MAX_JOBS * MAX_ANT * sizeof(float)));
  g_llr_cap = 32u << 20; /* matches the receiver's per-consumer output capacity */
  CK(nr_gpu_allocate(NR_GPU_SHARED_RW, (void **)&g_h_llr, g_llr_cap * sizeof(int16_t)));
  CK(nr_gpu_allocate(NR_GPU_DEVICE, (void **)&g_d_llr, g_llr_cap * sizeof(int16_t)));
  cudaFuncAttributes attr;
  /* every FFT size the host's twin reproduces (nr_exact::fep_size_ok: NEON 4096; x86 powers of two 256..4096, each size self-tested here
   * against the host CPU). The chest / delay / graph-pass users of this working set stay N = 4096 only (their own checks). */
  if (cfg->nant <= MAX_ANT && (g_isa = nr_exact::isa_profile()) != nr_isa::P_NONE && nr_exact::fep_size_ok(g_isa, N)
      && nr_exact::fep_size_selftest(N)) { /* no bit-identical twin: CPU FEP */
    /* Bit-exact FEP working set: allocated with the rest before the RX fence. The timeshift table is
     * computed exactly as init_timeshift_rotation() does (same libm, same formula). */
    const size_t exact = (size_t)cfg->nant * MAX_SYM * N * sizeof(short2);
    std::vector<uint16_t> perm;
    std::vector<short> tw1, tw4, ts(2 * N);
    nr_exact::twin_tables(g_isa, perm, tw1, tw4, N); /* NEON: build_perm + init_rad4_host(1024 / 4096); x86: its permutation and twiddle blob for N */
    CK(nr_gpu_allocate(NR_GPU_DEVICE, (void **)&g_d_perm, N * sizeof(uint16_t)));
    CK(nr_gpu_allocate(NR_GPU_DEVICE, (void **)&g_d_tw1024, tw1.size() * sizeof(short)));
    CK(nr_gpu_allocate(NR_GPU_DEVICE, (void **)&g_d_tw4096, tw4.size() * sizeof(short)));
    CK(nr_gpu_allocate(NR_GPU_DEVICE, (void **)&g_d_ts, N * sizeof(short2)));
    CK(nr_gpu_allocate(NR_GPU_DEVICE, (void **)&g_d_exact, exact));
    CK(nr_gpu_allocate(NR_GPU_STAGING, (void **)&g_h_exact, exact));
    const int so = cfg->nb_prefix_samples / cfg->ofdm_offset_divisor;
    for (int i = 0; i < N; i++) {
      const double poff = -i * 2.0 * M_PI * so / N;
      ts[2 * i] = (int16_t)round(cos(poff) * 32767);
      ts[2 * i + 1] = (int16_t)round(sin(-poff) * 32767);
    }
    CK(cudaMemcpy(g_d_perm, perm.data(), N * sizeof(uint16_t), cudaMemcpyHostToDevice));
    CK(cudaMemcpy(g_d_tw1024, tw1.data(), tw1.size() * sizeof(short), cudaMemcpyHostToDevice));
    CK(cudaMemcpy(g_d_tw4096, tw4.data(), tw4.size() * sizeof(short), cudaMemcpyHostToDevice));
    CK(cudaMemcpy(g_d_ts, ts.data(), ts.size() * sizeof(short), cudaMemcpyHostToDevice));
    CK(NR_EXACT_ATTR(g_isa, &attr, nr_exact::k_fep_exact));
    CK(NR_EXACT_ATTR(g_isa, &attr, nr_exact::k_delay_exact));
    {
      int least = 0, greatest = 0;
      CK(cudaDeviceGetStreamPriorityRange(&least, &greatest));
      for (int c = 0; c < EXACT_CTX; c++) {
        if (!nr_gpu_green_stream(&g_ctx_stream[c], greatest, true, c)) CK(cudaStreamCreateWithPriority(&g_ctx_stream[c], cudaStreamNonBlocking, greatest));
        CK(nr_gpu_allocate(NR_GPU_STAGING, &g_ctx_delay[c], 3 * nr_exact::DELAY_MAX_ROWS * sizeof(int)));
        CK(nr_gpu_allocate(NR_GPU_DEVICE, (void **)&g_ctx_ls[c], NR_GPU_CHEST_MAX_ROWS * 4096 * sizeof(short2)));
        CK(nr_gpu_allocate(NR_GPU_STAGING, (void **)&g_ctx_cout[c], 4 * NR_GPU_CHEST_MAX_ROWS * sizeof(int)));
        CK(nr_gpu_allocate(NR_GPU_STAGING, (void **)&g_ctx_rows[c], NR_GPU_CHEST_MAX_ROWS * sizeof(nr_gpu_chest_row_t)));
      }
      const size_t ring = (size_t)cfg->nant * (g_samples_per_slot + N) * sizeof(short2);
      for (int k = 0; k < PASS_SLOTS; k++) {
        CK(nr_gpu_allocate(NR_GPU_STAGING, (void **)&g_pass_h[k], ring));
        CK(nr_gpu_allocate(NR_GPU_DEVICE, (void **)&g_pass_d[k], ring));
        CK(nr_gpu_allocate(NR_GPU_STAGING, (void **)&g_pass_par[k], nr_exact::PAR_WORDS * sizeof(uint32_t)));
        CK(nr_gpu_allocate(NR_GPU_DEVICE, (void **)&g_pass_ls[k], (size_t)PASS_LS_ROWS * 4096 * sizeof(short2)));
        CK(nr_gpu_allocate(NR_GPU_STAGING, (void **)&g_pass_rh[k], NR_GPU_CHEST_MAX_ROWS * sizeof(nr_gpu_chest_row_t)));
        CK(nr_gpu_allocate(NR_GPU_DEVICE, (void **)&g_pass_rd[k], NR_GPU_CHEST_MAX_ROWS * sizeof(nr_gpu_chest_row_t)));
        CK(cudaEventCreateWithFlags(&g_pass_fev[k], cudaEventDisableTiming));
        CK(cudaEventCreateWithFlags(&g_pass_cev[k], cudaEventDisableTiming));
      }
    }
    /* Direct mode: the kernel reads the receiver's host IQ ring and writes its rxdataF in place. Only on a
     * coherent shared-memory platform (GB10: pageable access through the host page tables). */
    g_exact_direct = nr_gpu_platform_shared_direct();
    g_exact_ok = true;
  }
  CK(cudaFuncGetAttributes(&attr, k_fep_gather));
  CK(cudaFuncGetAttributes(&attr, k_fep_rotate));
  CK(cudaFuncGetAttributes(&attr, k_gold));
  CK(gold_init_jumps());
  CK(cudaFuncGetAttributes(&attr, k_ls));
  CK(cudaFuncGetAttributes(&attr, k_sigma));
  CK(cudaFuncGetAttributes(&attr, k_llr));
  CK(cudaMemsetAsync(g_d_fft_in, 0, (size_t)cfg->nant * ns * N * sizeof(float2), g_stream));
  if (cufftExecC2C(g_plan, (cufftComplex *)g_d_fft_in, (cufftComplex *)g_d_rxF, CUFFT_FORWARD) != CUFFT_SUCCESS) return -1;
  CK((cudaError_t)nr_gpu_sync(g_stream));
  return 0;
}

/* api_init: API entry: initialises the PDSCH GPU front end once.
 *   How it works: Returns 0 when already initialised with the same cfg. A failed attempt cannot retry while RX runs (g_init_state = -1). Calls allocate_init and seals the DL GPU memory (nr_dl_gpu_sealed).
 *   ISAC_* flags read: none.
 *   Callers: none found in tree.
 */
static int api_init(const nr_gpu_fep_cfg_t *cfg)
{
  if (!cfg) return -1;
  if (g_init_state) return g_init_state == 1 && !memcmp(cfg, &g_cfg, sizeof(*cfg)) ? 0 : -1;
  g_init_state = -1; /* a failed attempt cannot retry while RX is running */
  const int rc = allocate_init(cfg);
  if (!rc) g_init_state = 1;
  nr_dl_gpu_sealed = true;
  return rc;
}


extern "C" int nr_gpu_fep_admit(const nr_gpu_pdsch_job_t *jobs, int n, int check)
{
  if (g_init_state != 1 || n < 1 || n > MAX_JOBS) return 0;
  uint64_t bytes = 3 * (uint64_t)g_cfg.nant * (g_samples_per_slot + g_cfg.ofdm_symbol_size) * sizeof(short2);
  for (int i = 0; i < n; i++) {
    uint64_t llr = (uint64_t)jobs[i].nb_rb * 12 * jobs[i].nb_symbols * jobs[i].Nl * jobs[i].Qm;
    if (jobs[i].max_llr && llr > jobs[i].max_llr) llr = jobs[i].max_llr;
    bytes += 3 * llr * sizeof(int16_t) + 3 * sizeof(jobs[i]);
  }
  if (check) {
    int first_sym = jobs[0].dmrs_mask ? __builtin_ctz((unsigned)jobs[0].dmrs_mask) : -1;
    for (int i = 1; i < n; ++i)
      if (!jobs[i].dmrs_mask || __builtin_ctz((unsigned)jobs[i].dmrs_mask) != first_sym) first_sym = -1;
    if (first_sym >= g_cfg.symbols_per_slot) first_sym = -1;
    const int read_symbols = first_sym >= 0 ? 1 : g_cfg.symbols_per_slot;
    bytes += (uint64_t)read_symbols * g_cfg.ofdm_symbol_size * (2 * sizeof(float2) + sizeof(short2));
  }
  return !nr_dl_gpu_radio_mode || nr_dl_gpu_budget_take(&g_budget, bytes);
}

/* Copy the slot window (from `early` samples before the slot's CP, ring wrap) to the device. */
/* upload_window: copies the rxdata ring window for all antennas to the device.
 *   How it works: The window starts early samples before the slot's CP and wraps at ring_len. Copies in one or two pieces into g_h_rx, then starts an async H2D copy.
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_fep.cu:695, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_fep.cu:930.
 */
static cudaError_t upload_window(const int16_t *const *rxdata, uint32_t ring_len, uint32_t ring_offset)
{
  const int N = g_cfg.ofdm_symbol_size, nant = g_cfg.nant;
  const int early = g_cfg.nb_prefix_samples / g_cfg.ofdm_offset_divisor;
  const size_t len = (size_t)g_samples_per_slot + N; /* the last symbol + slack */
  /* the window starts `early` samples before the slot's CP: origin = ring_offset - early */
  const uint32_t origin = (ring_offset + ring_len - early) % ring_len;
  for (int a = 0; a < nant; a++) {
    short2 *dst = (short2 *)g_h_rx + (size_t)a * len;
    const short2 *src = (const short2 *)rxdata[a];
    const size_t first = std::min<size_t>(len, ring_len - origin);
    memcpy(dst, src + origin, first * sizeof(short2));
    if (first < len)
      memcpy(dst + first, src, (len - first) * sizeof(short2));
  }
  return (cudaError_t)nr_gpu_copy_async(g_d_rx, g_h_rx, (size_t)nant * len * sizeof(short2), g_stream);
}

/* Q4 (DISCRETE staged exact FEP): copies only the window span the kernel reads for the masked symbols lo..hi, [ncp0 + lo (N + ncp), ncp0 + hi (N + ncp) + N) of every
 * antenna (ring wrap), into g_h_rx at the window coordinates and one 2D async H2D of exactly that span for all antennas. Returns the CUDA error; *bytes = bytes copied. */
static cudaError_t upload_span(const int16_t *const *rxdata, uint32_t ring_len, uint32_t ring_offset, int lo, int hi, uint64_t *bytes)
{
  const int N = g_cfg.ofdm_symbol_size, nant = g_cfg.nant;
  const int early = g_cfg.nb_prefix_samples / g_cfg.ofdm_offset_divisor;
  const size_t len = (size_t)g_samples_per_slot + N;
  const size_t s0 = (size_t)g_cfg.nb_prefix_samples0 + (size_t)lo * (N + g_cfg.nb_prefix_samples);
  const size_t s1 = (size_t)g_cfg.nb_prefix_samples0 + (size_t)hi * (N + g_cfg.nb_prefix_samples) + N, span = s1 - s0;
  const uint32_t origin = (ring_offset + ring_len - early) % ring_len;
  const size_t start = ((size_t)origin + s0) % ring_len;
  for (int a = 0; a < nant; a++) {
    short2 *dst = (short2 *)g_h_rx + (size_t)a * len + s0;
    const short2 *src = (const short2 *)rxdata[a];
    const size_t first = std::min<size_t>(span, ring_len - start);
    memcpy(dst, src + start, first * sizeof(short2));
    if (first < span)
      memcpy(dst + first, src, (span - first) * sizeof(short2));
  }
  *bytes = (uint64_t)nant * span * sizeof(short2);
  return cudaMemcpy2DAsync(g_d_rx + s0, len * sizeof(short2), (short2 *)g_h_rx + s0, len * sizeof(short2), span * sizeof(short2), nant, cudaMemcpyHostToDevice, g_stream);
}

/* api_fep_slot: API entry: runs the FEP for one slot on the GPU.
 *   How it works: Admits the RT budget scope. Uploads the window and the optional symbol rotation. k_fep_gather applies the sample gather and the frequency-offset phase. cuFFT forward, then k_fep_rotate. Waits for the stream. Records g_fep_us.
 *   ISAC_* flags read: none.
 *   Callers: PDSCH GPU API table (function pointer).
 */
static int api_fep_slot(const int16_t *const *rxdata, uint32_t ring_len, uint32_t ring_offset, uint32_t abs_sample,
                        double fo_hz, const int16_t *symbol_rot)
{
  if (g_init_state != 1) return -1;
  NrGpuRtScope rt; if(!rt.admitted)return -1;
  const auto t0 = std::chrono::steady_clock::now();
  const int N = g_cfg.ofdm_symbol_size, ns = g_cfg.symbols_per_slot, nant = g_cfg.nant;
  const int early = g_cfg.nb_prefix_samples / g_cfg.ofdm_offset_divisor;
  CK(upload_window(rxdata, ring_len, ring_offset));
  if (symbol_rot) {
    memcpy(g_h_rot, symbol_rot, (size_t)ns * sizeof(short2));
    CK((cudaError_t)nr_gpu_copy_async(g_d_rot, g_h_rot, (size_t)ns * sizeof(short2), g_stream));
  }
  const float phase_inc = (float)(-fo_hz / (g_cfg.samples_per_ms * 1000.0));
  dim3 blk(256), grd((N + 255) / 256, ns, nant);
  k_fep_gather<<<grd, blk, 0, g_stream>>>(g_d_rx, (long long)abs_sample - early, phase_inc, N, g_cfg.nb_prefix_samples,
                                          g_cfg.nb_prefix_samples0, ns, g_samples_per_slot, g_d_fft_in);
  if (cufftExecC2C(g_plan, (cufftComplex *)g_d_fft_in, (cufftComplex *)g_d_rxF, CUFFT_FORWARD) != CUFFT_SUCCESS) {
    g_init_state=-1;nr_gpu_budget_fault();return -1;
  }
  k_fep_rotate<<<grd, blk, 0, g_stream>>>(g_d_rxF, symbol_rot ? g_d_rot : nullptr, N, ns, early);
  CK(rt.wait(g_stream));
  g_fep_us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
  return 0;
}

static uint32_t job_total_llr(const nr_gpu_pdsch_job_t *j)
{
  uint32_t nre = 0;
  for (int s = j->start_symbol; s < j->start_symbol + j->nb_symbols; s++) {
    const int is_d = (j->dmrs_mask >> s) & 1;
    int per = is_d ? (j->dmrs_type == 1 ? (j->n_cdm_groups_no_data >= 2 ? 0 : 6) : 12 - 4 * j->n_cdm_groups_no_data) : 12;
    nre += per * j->nb_rb;
  }
  return nre * j->Nl * j->Qm;
}

/* run_chunk: runs one chunk of PDSCH jobs on the GPU and returns the number of LLRs written to llr_out.
 *   How it works: Copies the jobs to the device, then launches k_gold, k_ls, k_sigma and, when max_re > 0, k_llr. Copies the LLRs back, waits on rt and checks the last CUDA error.
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_fep.cu:791.
 */
static int64_t run_chunk(nr_gpu_pdsch_job_t *jobs, int n, int16_t *llr_out, uint32_t total, NrGpuRtScope &rt)
{
  const int N = g_cfg.ofdm_symbol_size, ns = g_cfg.symbols_per_slot, nant = g_cfg.nant, fco = g_cfg.first_carrier_offset;
  memcpy(g_h_jobs, jobs, (size_t)n * sizeof(nr_gpu_pdsch_job_t));
  CK((cudaError_t)nr_gpu_copy_async(g_d_jobs, g_h_jobs, (size_t)n * sizeof(nr_gpu_pdsch_job_t), g_stream));
  k_gold<<<dim3(n, MAX_DMRS_SYM), 128, 0, g_stream>>>(g_d_jobs, g_d_gold, ns);
  int max_ls = 0, max_re = 0;
  for (int i = 0; i < n; i++) {
    const int npil = jobs[i].nb_rb * (jobs[i].dmrs_type == 1 ? 3 : 2);
    max_ls = std::max(max_ls, npil * nant * jobs[i].Nl * MAX_DMRS_SYM);
    if (jobs[i].n_llr)
      max_re = std::max(max_re, (int)(jobs[i].n_llr / (jobs[i].Nl * jobs[i].Qm)));
  }
  k_ls<<<dim3((max_ls + 255) / 256, n), 256, 0, g_stream>>>(g_d_jobs, g_d_gold, g_d_rxF, N, ns, nant, fco, g_d_ls);
  k_sigma<<<dim3(n, nant), 256, 0, g_stream>>>(g_d_jobs, g_d_ls, g_d_sigma);
  if (max_re > 0)
    k_llr<<<dim3((max_re + 127) / 128, n), 128, 0, g_stream>>>(g_d_jobs, g_d_ls, g_d_sigma, g_d_rxF, N, ns, nant, fco, g_d_llr,
                                                             g_llr_scale, g_force_ant);
  CK((cudaError_t)nr_gpu_copy_async(g_h_llr, g_d_llr, (size_t)total * sizeof(int16_t), g_stream));
  CK(rt.wait(g_stream));
  CK(cudaGetLastError());
  memcpy(llr_out, g_h_llr, (size_t)total * sizeof(int16_t));
  return total;
}

/* api_pdsch_llr: API entry: computes PDSCH LLRs for n jobs on the GPU.
 *   How it works: Admits the RT budget scope. Packs jobs into launch chunks of at most MAX_JOBS jobs and g_llr_cap LLRs. Each job is validated (layers, RBs, DM-RS type, Qm, symbols, pilots). Its LLR count is capped by max_llr and the caller's capacity, and rounded to whole REs. Calls run_chunk per chunk and returns the total LLRs written, or a negative error.
 *   ISAC_* flags read: none.
 *   Callers: PDSCH GPU API table (function pointer).
 */
static int64_t api_pdsch_llr(nr_gpu_pdsch_job_t *jobs, int n, int16_t *llr_out, size_t llr_cap)
{
  if (g_init_state != 1 || !jobs || !llr_out || n < 0) return -1;
  NrGpuRtScope rt; if(!rt.admitted)return -1;
  const auto t0 = std::chrono::steady_clock::now();
  int64_t written = 0;
  int i = 0;
  g_last_chunks = 0;
  while (i < n) {
    /* pack jobs into one launch set: <= MAX_JOBS, <= g_llr_cap LLRs */
    uint32_t total = 0;
    int m = 0;
    while (i + m < n && m < MAX_JOBS) {
      nr_gpu_pdsch_job_t *j = &jobs[i + m];
      uint32_t nl = 0;
      const bool ok = j->Nl >= 1 && j->Nl <= g_cfg.nant && j->Nl <= MAX_PORTS && j->nb_rb > 0 && j->start_rb + j->nb_rb <= g_cfg.n_rb_dl && j->dmrs_mask != 0
                      && (j->dmrs_type == 1 || j->dmrs_type == 2) && j->Qm >= 2 && j->Qm <= 8 && (j->Qm & 1) == 0
                      && j->start_symbol + j->nb_symbols <= g_cfg.symbols_per_slot
                      && __builtin_popcount(j->dmrs_mask) <= MAX_DMRS_SYM && j->nb_rb * 3 <= MAX_PILOTS;
      if (ok) {
        nl = job_total_llr(j);
        if (j->max_llr && j->max_llr < nl)
          nl = j->max_llr;
        nl -= nl % (j->Nl * j->Qm); /* whole REs */
      }
      if (m > 0 && total + nl > g_llr_cap)
        break;
      if ((size_t)written + total + nl > llr_cap) {
        nl = 0; /* no room: unsupported for the caller, decode on the CPU */
      }
      j->llr_offset = (uint32_t)(written + total);
      j->n_llr = nl;
      total += nl;
      m++;
    }
    /* the kernels address the chunk's LLRs from 0 */
    for (int k = 0; k < m; k++)
      jobs[i + k].llr_offset -= (uint32_t)written;
    const int64_t r = run_chunk(&jobs[i], m, llr_out + written, total, rt);
    ++g_last_chunks;
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

extern "C" int nr_gpu_fep_last_chunks(void) { return g_last_chunks; }

static int api_read_rxdataF(int ant, int16_t *out)
{
  const int N = g_cfg.ofdm_symbol_size, ns = g_cfg.symbols_per_slot;
  if (g_init_state != 1 || ant < 0 || ant >= g_cfg.nant) return -1;
  NrGpuRtScope rt; if(!rt.admitted)return -1;
  float2 *tmp = g_h_check;
  CK((cudaError_t)nr_gpu_copy_async(tmp, g_d_rxF + (size_t)ant * ns * N, (size_t)ns * N * sizeof(float2), g_stream));
  CK(rt.wait(g_stream));
  for (size_t i = 0; i < (size_t)ns * N; i++) {
    out[2 * i] = (int16_t)std::max(-32768.f, std::min(32767.f, rintf(tmp[i].x)));
    out[2 * i + 1] = (int16_t)std::max(-32768.f, std::min(32767.f, rintf(tmp[i].y)));
  }
  return 0;
}

/* api_read_rxdataF_symbol: reads one rxdataF symbol (antenna, symbol) back from the device as saturated int16.
 *   How it works: Copies N float2 from g_d_rxF to the check buffer, waits, then rounds and saturates each component.
 *   ISAC_* flags read: none.
 *   Callers: PDSCH GPU API table (function pointer).
 */
static int api_read_rxdataF_symbol(int ant, int sym, int16_t *out)
{
  const int N = g_cfg.ofdm_symbol_size, ns = g_cfg.symbols_per_slot;
  if (g_init_state != 1 || ant < 0 || ant >= g_cfg.nant || sym < 0 || sym >= ns) return -1;
  NrGpuRtScope rt; if (!rt.admitted) return -1;
  float2 *tmp = g_h_check;
  CK((cudaError_t)nr_gpu_copy_async(tmp, g_d_rxF + ((size_t)ant * ns + sym) * N, (size_t)N * sizeof(float2), g_stream));
  CK(rt.wait(g_stream));
  for (int i = 0; i < N; i++) {
    out[2 * i] = (int16_t)std::max(-32768.f, std::min(32767.f, rintf(tmp[i].x)));
    out[2 * i + 1] = (int16_t)std::max(-32768.f, std::min(32767.f, rintf(tmp[i].y)));
  }
  return 0;
}

/* api_timing: returns the durations of the last FEP and LLR calls in microseconds.
 *   How it works: Copies g_fep_us and g_llr_us into the non-NULL output pointers.
 *   ISAC_* flags read: none.
 *   Callers: PDSCH GPU API table (function pointer).
 */
static void api_timing(double *fep_us, double *llr_us)
{
  if (fep_us) *fep_us = g_fep_us;
  if (llr_us) *llr_us = g_llr_us;
}

static const nr_gpu_fep_api_t g_api = {api_init, api_fep_slot, api_pdsch_llr, api_read_rxdataF, api_timing,
                                      api_read_rxdataF_symbol};
extern "C" const nr_gpu_fep_api_t *nr_gpu_fep_api(void) { return nr_gpu_module_ready() ? &g_api : nullptr; }

/* Offline stage isolation only. Deliberately separate from the receiver API. */
extern "C" int nr_pdsch_gpu_debug_ls(int job, int di, int layer, int ant, float *out, float *noise)
{
  if (g_init_state != 1 || job < 0 || job >= MAX_JOBS || di < 0 || di >= MAX_DMRS_SYM
      || layer < 0 || layer >= MAX_PORTS || ant < 0 || ant >= g_cfg.nant) return -1;
  CK(cudaMemcpy(out, g_d_ls + (((size_t)job * MAX_DMRS_SYM + di) * MAX_PORTS + layer) * MAX_ANT * MAX_PILOTS
                              + (size_t)ant * MAX_PILOTS, MAX_PILOTS * sizeof(float2), cudaMemcpyDeviceToHost));
  CK(cudaMemcpy(noise, g_d_sigma + job * MAX_ANT + ant, sizeof(float), cudaMemcpyDeviceToHost));
  return 0;
}
extern "C" int nr_pdsch_gpu_debug_cpu_fep(const int16_t *rx)
{
  if (g_init_state != 1 || !rx) return -1;
  CK(cudaStreamSynchronize(g_stream));
  const size_t count = (size_t)g_cfg.symbols_per_slot * g_cfg.ofdm_symbol_size;
  for (int a = 0; a < g_cfg.nant; ++a) {
    for (size_t i = 0; i < count; ++i)
      g_h_check[i] = make_float2(rx[2*(a*count+i)], rx[2*(a*count+i)+1]);
    CK(cudaMemcpy(g_d_rxF + a*count, g_h_check, count*sizeof(float2), cudaMemcpyHostToDevice));
  }
  return 0;
}

/* GPU6: bit-exact CPU FEP (nr_pdsch_gpu_exact_fep.cuh) of the symbols in sym_mask, written as OAI rxdataF
 * into out[a * ant_stride + sym * N] (host c16). Same window arguments as fep_slot. fo_hz must be 0: the
 * CPU FO compensation is not reproduced. try_only: no free context -> -1 at once (the PDCCH scan never waits).
 * Returns 0, or -1 when the caller must run the CPU FEP.
 * Direct mode never returns while the kernel can still write the caller's rxdataF (a late kernel would
 * otherwise overwrite the CPU fallback or the next slot); it only counts deadline misses. Staged mode
 * returns -1 on a miss and refuses the next calls until its stream has drained. */
extern "C" int nr_pdsch_gpu_exact_fep2(const int16_t *const *rxdata, uint32_t ring_len, uint32_t ring_offset, double fo_hz,
                                       const int16_t *symbol_rot, uint16_t sym_mask, int16_t *out, size_t ant_stride, int try_only,
                                       const uint32_t *sfo_dphi, int sfo_kref)
{
  if (g_init_state != 1 || !g_exact_ok || fo_hz != 0.0 || !rxdata || !symbol_rot || !out) return -1;
  const int N = g_cfg.ofdm_symbol_size, ns = g_cfg.symbols_per_slot, nant = g_cfg.nant;
  sym_mask &= (uint16_t)((1u << ns) - 1u);
  if (!sym_mask) return -1;
  nr_exact::ExactIO io = {};
  memcpy(io.rot, symbol_rot, (size_t)ns * sizeof(short2));
  if (sfo_dphi)
    memcpy(io.sfo, sfo_dphi, (size_t)ns * sizeof(uint32_t));
  io.sfo_kref = sfo_kref;
  int nsym = 0, lo = ns, hi = -1;
  for (int l = 0; l < ns; l++)
    if (sym_mask >> l & 1) {
      io.sym_list[nsym++] = (uint16_t)l;
      lo = std::min(lo, l);
      hi = l;
    }
  const int early = g_cfg.nb_prefix_samples / g_cfg.ofdm_offset_divisor;
  if (g_exact_direct) {
    const int c = ctx_acquire(try_only);
    if (c < 0) { ++g_exact_busy; return -1; }
    NrGpuRtScope rt;
    if (!rt.admitted) { ctx_release(c); return -1; }
    const auto t0 = std::chrono::steady_clock::now();
    for (int a = 0; a < nant; a++) io.rx[a] = (const short2 *)rxdata[a];
    io.ring_len = ring_len;
    io.origin = (ring_offset + ring_len - early) % ring_len;
    io.out = (short2 *)out;
    io.ant_stride = ant_stride;
    NR_EXACT_LAUNCH(g_isa, nr_exact::k_fep_exact, dim3(nsym, nant), nr_exact::THREADS, g_ctx_stream[c], 
        io, g_cfg.nb_prefix_samples, g_cfg.nb_prefix_samples0, g_d_perm, g_d_tw1024, g_d_tw4096, g_d_ts, g_cfg.n_rb_dl,
        g_cfg.first_carrier_offset, N);
    cudaError_t e = cudaPeekAtLastError();
    if (e == cudaSuccess) e = (cudaError_t)ctx_wait(c, t0, NRSS_FEP);
    ctx_release(c);
    if (e != cudaSuccess) {
      g_init_state = -1;
      fprintf(stderr, "nr_pdsch_gpu_exact_fep: %s\n", cudaGetErrorString(e));
      return -1;
    }
    g_exact_us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    return 0;
  }
  const uint64_t ss_lk = nr_ss_now();
  if (try_only) {
    if (!g_exact_staged_lock.try_lock()) { ++g_exact_busy; return -1; }
  } else
    g_exact_staged_lock.lock();
  nr_ss_add(NRSS_LK, ss_lk);
  std::lock_guard<std::mutex> hold(g_exact_staged_lock, std::adopt_lock);
  if (g_exact_pending) {
    if (cudaStreamQuery(g_stream) == cudaErrorNotReady) { ++g_exact_busy; return -1; }
    g_exact_pending = false;
  }
  NrGpuRtScope rt;
  if (!rt.admitted) return -1;
  const auto t0 = std::chrono::steady_clock::now();
  uint64_t up_bytes = 0;
  CK(upload_span(rxdata, ring_len, ring_offset, lo, hi, &up_bytes)); /* Q4: only the span of the masked symbols */
  for (int a = 0; a < nant; a++) io.rx[a] = g_d_rx + (size_t)a * (g_samples_per_slot + N);
  io.ring_len = 0xffffffffu;
  io.origin = 0;
  io.out = g_d_exact;
  io.ant_stride = (size_t)MAX_SYM * N;
  NR_EXACT_LAUNCH(g_isa, nr_exact::k_fep_exact, dim3(nsym, nant), nr_exact::THREADS, g_stream, 
      io, g_cfg.nb_prefix_samples, g_cfg.nb_prefix_samples0, g_d_perm, g_d_tw1024, g_d_tw4096, g_d_ts, g_cfg.n_rb_dl,
      g_cfg.first_carrier_offset, N);
  CK(cudaPeekAtLastError());
  CK(cudaMemcpy2DAsync(g_h_exact + (size_t)lo * N, (size_t)MAX_SYM * N * sizeof(short2), g_d_exact + (size_t)lo * N, (size_t)MAX_SYM * N * sizeof(short2),
                       (size_t)(hi - lo + 1) * N * sizeof(short2), nant, cudaMemcpyDeviceToHost, g_stream)); /* Q4: rows lo..hi of every antenna in one copy */
  uint64_t pt = 0;
  const uint64_t ss_w = nr_ss_now();
  for (;;) {
    const cudaError_t q = cudaStreamQuery(g_stream);
    if (q == cudaSuccess) break;
    if (q != cudaErrorNotReady) CK(q);
    nr_gpu_poll_relax(&pt); /* P99: ISAC_GPU_POLL_SLEEP_US */
    if (std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() > g_exact_deadline_us) {
      g_exact_pending = true;
      ++g_exact_late;
      nr_ss_add(NRSS_FEP, ss_w);
      return -1;
    }
  }
  nr_ss_add(NRSS_FEP, ss_w);
  nr_stage::note(NR_GPU_STG_FEP, up_bytes, 1, (uint64_t)nant * (hi - lo + 1) * N * sizeof(short2), 1);
  for (int a = 0; a < nant; a++)
    for (int l = lo; l <= hi; l++)
      if (sym_mask >> l & 1)
        memcpy(out + 2 * ((size_t)a * ant_stride + (size_t)l * N), g_h_exact + ((size_t)a * MAX_SYM + l) * N,
               (size_t)N * sizeof(short2));
  g_exact_us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
  return 0;
}
/* GPUPASS host step: pick the next staging slot, wait until its previous pass is done with it, then write the per-slot origin / rotation
 * and (DISCRETE model only) copy the IQ window into the slot's pinned ring. Host only: no CUDA work is enqueued. See nr_pdsch_gpu_fep.h. */
static int fep_stage_impl(const nr_gpu_fep_job_t *j, int k)
{
  const int N = g_cfg.ofdm_symbol_size, ns = g_cfg.symbols_per_slot, nant = g_cfg.nant;
  pass_slot_wait(g_pass_fev[k]);
  const int early = g_cfg.nb_prefix_samples / g_cfg.ofdm_offset_divisor;
  const uint32_t origin = (j->ring_offset + j->ring_len - early) % j->ring_len;
  uint32_t *par = g_pass_par[k];
  memset(par, 0, nr_exact::PAR_WORDS * sizeof(uint32_t));
  memcpy(par + 1, j->symbol_rot, (size_t)ns * sizeof(short2));
  if (j->sfo_dphi)
    memcpy(par + nr_exact::PAR_SFO, j->sfo_dphi, (size_t)ns * sizeof(uint32_t));
  par[nr_exact::PAR_SFO_KREF] = (uint32_t)j->sfo_kref;
  par[21] = (uint32_t)((j->run_mask ? j->run_mask : j->sym_mask) & ((1u << ns) - 1u)); /* sized to the grant for the graph pass; fep_dev's grid is sym_mask */
  if (nr_gpu_model() == NR_GPU_MODEL_DISCRETE) {
    const size_t len = (size_t)g_samples_per_slot + N; /* the last symbol + slack, as upload_window */
    for (int a = 0; a < nant; a++) {
      short2 *dst = g_pass_h[k] + (size_t)a * len;
      const short2 *src = (const short2 *)j->rxdata[a];
      const size_t first = std::min<size_t>(len, j->ring_len - origin);
      memcpy(dst, src + origin, first * sizeof(short2));
      if (first < len) memcpy(dst + first, src, (len - first) * sizeof(short2));
      const unsigned long long p = (unsigned long long)(g_pass_d[k] + (size_t)a * len);
      memcpy(par + 22 + 2 * a, &p, sizeof(p));
    }
    par[0] = 0;
    par[16] = 0xffffffffu;
  } else {
    par[0] = origin;
    par[16] = j->ring_len;
    for (int a = 0; a < nant; a++) {
      const unsigned long long p = (unsigned long long)j->rxdata[a];
      memcpy(par + 22 + 2 * a, &p, sizeof(p));
    }
    if (j->out) { /* DIRECT only: write the caller's rxdataF in place (j->out_stride short2 per antenna) */
      const unsigned long long p = (unsigned long long)j->out;
      par[17] = 1;
      memcpy(par + 18, &p, sizeof(p));
      par[20] = j->out_stride;
    }
  }
  return 0;
}
extern "C" int nr_pdsch_gpu_fep_stage(const nr_gpu_fep_job_t *j, int *slot_out)
{
  if (g_init_state != 1 || !g_exact_ok || !j || !j->rxdata || !j->symbol_rot || !slot_out || !j->ring_len) return -1;
  const int k = PASS_RING; /* the helper's own slot: one test pass in flight, no sharing with live instances */
  fep_stage_impl(j, k);
  *slot_out = k;
  return 0;
}
/* the same for a slot chosen by the caller (a graph instance owns its slot: the graph is captured per slot index) */
extern "C" int nr_pdsch_gpu_fep_stage_at(const nr_gpu_fep_job_t *j, int slot)
{
  if (g_init_state != 1 || !g_exact_ok || !j || !j->rxdata || !j->symbol_rot || !j->ring_len || slot < 0 || slot >= PASS_SLOTS
      || (j->out && nr_gpu_model() == NR_GPU_MODEL_DISCRETE))
    return -1;
  return fep_stage_impl(j, slot);
}

/* GPUPASS: launch-only exact FEP of one slot into rxF (nr_gpu_buf_t, [ant][14][N] c16, OAI rxdataF), after nr_pdsch_gpu_fep_stage returned
 * `slot`. Enqueues on `stream` only (no wait, no host copy), so it can be captured in a graph and replayed after each _stage. rxF->model must
 * be nr_gpu_model(). DIRECT: the kernel reads the host IQ ring in place (GB10); DISCRETE: one H2D from the slot's pinned ring into its device
 * ring slot, the kernel reads that. 0 or -1. */
extern "C" int nr_pdsch_gpu_fep_dev(const nr_gpu_fep_job_t *j, nr_gpu_buf_t *rxF, int slot, void *stream)
{
  if (g_init_state != 1 || !g_exact_ok || !j || !j->rxdata || !rxF || !rxF->dev || slot < 0 || slot >= PASS_SLOTS
      || rxF->model != nr_gpu_model())
    return -1;
  const int N = g_cfg.ofdm_symbol_size, ns = g_cfg.symbols_per_slot, nant = g_cfg.nant;
  const uint16_t sym_mask = j->sym_mask & (uint16_t)((1u << ns) - 1u);
  if (N != nr_exact::N || !sym_mask || rxF->bytes < (size_t)nant * MAX_SYM * N * sizeof(short2)) return -1; /* the graph pass: N = 4096 only */
  nr_exact::ExactIO io = {};
  int nsym = 0;
  for (int l = 0; l < ns; l++)
    if (sym_mask >> l & 1) io.sym_list[nsym++] = (uint16_t)l;
  const cudaStream_t s = (cudaStream_t)stream;
  io.par = (const uint32_t *)nr_gpu_device_ptr(g_pass_par[slot]);
  if (rxF->model == NR_GPU_MODEL_DIRECT) {
    if (!nr_gpu_platform_shared_direct()) return -1;
    for (int a = 0; a < nant; a++) io.rx[a] = (const short2 *)j->rxdata[a];
    io.ring_len = j->ring_len;
  } else {
    const size_t len = (size_t)g_samples_per_slot + N;
    for (int a = 0; a < nant; a++) io.rx[a] = g_pass_d[slot] + (size_t)a * len;
    if (cudaMemcpyAsync(g_pass_d[slot], g_pass_h[slot], (size_t)nant * len * sizeof(short2), cudaMemcpyHostToDevice, s) != cudaSuccess)
      return -1;
    io.ring_len = 0xffffffffu;
  }
  io.out = (short2 *)rxF->dev;
  io.ant_stride = (size_t)MAX_SYM * N;
  NR_EXACT_LAUNCH(g_isa, nr_exact::k_fep_exact, dim3(nsym, nant), nr_exact::THREADS, s, io, g_cfg.nb_prefix_samples, g_cfg.nb_prefix_samples0, g_d_perm,
                                                                      g_d_tw1024, g_d_tw4096, g_d_ts, g_cfg.n_rb_dl,
                                                                      g_cfg.first_carrier_offset);
  if (cudaPeekAtLastError() != cudaSuccess) return -1;
  return pass_slot_done(g_pass_fev[slot], s) == cudaSuccess ? 0 : -1;
}
extern "C" int nr_pdsch_gpu_exact_fep(const int16_t *const *rxdata, uint32_t ring_len, uint32_t ring_offset, double fo_hz,
                                      const int16_t *symbol_rot, uint16_t sym_mask, int16_t *out, size_t ant_stride)
{
  return nr_pdsch_gpu_exact_fep2(rxdata, ring_len, ring_offset, fo_hz, symbol_rot, sym_mask, out, ant_stride, 0, NULL, 0);
}

/* GPU6: nr_passive_est_delay() of up to 16 LS rows (N c16 each, host memory; shared-memory platforms only).
 * out[3 r] = {est_delay, delay_max_pos, delay_max_val} of the candidate. 0, or -1 (caller computes on the CPU). */
/* Q4 (DISCRETE): the same on a staged copy: the rows are gathered into the context's pinned block, one async H2D, the kernel reads the device copy, 3 ints per row come
 * back in one D2H. Returns 0 / -1 like the DIRECT body. */
static int delay_staged(const int16_t *const *rows, int nrows, int *out)
{
  const int c = ctx_acquire(false);
  NrGpuRtScope rt;
  if (!rt.admitted) { ctx_release(c); return -1; }
  const auto t0 = std::chrono::steady_clock::now();
  const size_t rowb = (size_t)g_cfg.ofdm_symbol_size * sizeof(short2);
  stg::Ctx &x = stg::c[c];
  short2 *dev = (short2 *)x.in.dev;
  for (int r = 0; r < nrows; r++) memcpy((char *)x.in.host + (size_t)r * rowb, rows[r], rowb);
  nr_exact::DelayIO io = {};
  for (int r = 0; r < nrows; r++) io.ls[r] = dev + (size_t)r * g_cfg.ofdm_symbol_size;
  cudaStream_t s = g_ctx_stream[c];
  int *dints = (int *)x.out.dev;
  cudaError_t e = cudaMemcpyAsync(dev, x.in.host, (size_t)nrows * rowb, cudaMemcpyHostToDevice, s);
  if (e == cudaSuccess) {
    NR_EXACT_LAUNCH(g_isa, nr_exact::k_delay_exact, nrows, nr_exact::THREADS, s, io, g_d_perm, g_d_tw1024, g_d_tw4096, dints);
    e = cudaPeekAtLastError();
  }
  if (e == cudaSuccess) e = cudaMemcpyAsync(x.out.host, x.out.dev, 3 * (size_t)nrows * sizeof(int), cudaMemcpyDeviceToHost, s);
  if (e == cudaSuccess) e = (cudaError_t)ctx_wait(c, t0, NRSS_CHEST);
  if (e == cudaSuccess) {
    memcpy(out, x.out.host, 3 * (size_t)nrows * sizeof(int));
    nr_stage::note(NR_GPU_STG_DELAY, (uint64_t)nrows * rowb, 1, 3 * (uint64_t)nrows * sizeof(int), 1);
  }
  ctx_release(c);
  if (e != cudaSuccess) {
    g_init_state = -1;
    fprintf(stderr, "nr_pdsch_gpu_exact_delay: %s\n", cudaGetErrorString(e));
    return -1;
  }
  return 0;
}
extern "C" int nr_pdsch_gpu_exact_delay(const int16_t *const *rows, int nrows, int *out)
{
  if (g_init_state != 1 || !g_exact_ok || (!g_exact_direct && !stg::ready()) || g_cfg.ofdm_symbol_size != nr_exact::N || nrows < 1 || nrows > nr_exact::DELAY_MAX_ROWS || !rows || !out)
    return -1; /* the delay IDFT twin is idft4096 only */
  if (!g_exact_direct) return delay_staged(rows, nrows, out);
  const int c = ctx_acquire(false);
  NrGpuRtScope rt;
  if (!rt.admitted) { ctx_release(c); return -1; }
  const auto t0 = std::chrono::steady_clock::now();
  nr_exact::DelayIO io = {};
  for (int r = 0; r < nrows; r++) io.ls[r] = (const short2 *)rows[r];
  NR_EXACT_LAUNCH(g_isa, nr_exact::k_delay_exact, nrows, nr_exact::THREADS, g_ctx_stream[c], io, g_d_perm, g_d_tw1024, g_d_tw4096,
                                                                           (int *)nr_gpu_device_ptr(g_ctx_delay[c]));
  cudaError_t e = cudaPeekAtLastError();
  if (e == cudaSuccess) e = (cudaError_t)ctx_wait(c, t0, NRSS_CHEST);
  if (e == cudaSuccess) memcpy(out, g_ctx_delay[c], 3 * nrows * sizeof(int));
  ctx_release(c);
  if (e != cudaSuccess) {
    g_init_state = -1;
    fprintf(stderr, "nr_pdsch_gpu_exact_delay: %s\n", cudaGetErrorString(e));
    return -1;
  }
  return 0;
}

/* Q4: census of the staged DISCRETE variants of the standalone entries (nr_gpu_stage_census.h), shared by every translation unit of this module */
extern "C" void nr_gpu_stage_stats(int entry, uint64_t *o)
{
  for (int k = 0; k < 6; k++) o[k] = entry >= 0 && entry < NR_GPU_STG_N ? nr_stage::g_c[entry][k].load(std::memory_order_relaxed) : 0;
}
extern "C" void nr_pdsch_gpu_exact_stats(int *direct, uint64_t *late, uint64_t *busy)
{
  *direct = g_exact_direct;
  *late = g_exact_late;
  *busy = g_exact_busy;
}
extern "C" double nr_pdsch_gpu_exact_last_us(void) { return g_exact_us; }
/* 1 when the bit-exact FEP working set exists for this cell's FFT size (init done, twin bit-identical at N), else 0: the host then keeps the CPU FEP */
extern "C" int nr_pdsch_gpu_exact_ready(void) { return g_init_state == 1 && g_exact_ok; }
/* DECODETIME: pins a pageable host buffer the kernels read or write (cudaHostRegister), so the GPU never page-faults on
 * its first touch of it (nsys: every new 4 KB page cost ~12 us of device-wide stall in k_fir). 0 or the CUDA error. */
extern "C" int nr_pdsch_gpu_exact_pin(void *p, size_t bytes) { return (int)cudaHostRegister(p, bytes, cudaHostRegisterPortable); }
extern "C" int nr_pdsch_gpu_exact_unpin(void *p) { return (int)cudaHostUnregister(p); }

/* ---- GPU-DEMOD stage 2: bit-exact passive PDSCH channel estimation of one grant (nr_pdsch_gpu_chest.h) ------- */
namespace nrch {
struct Par { /* the job without its rows (kernel argument size) */
  int type, N, dt_stride, max_delay_comp;
  const int16_t *delay_table;
  int16_t filt[4][16];
  unsigned long long *acc; /* GPUPASS Task 7 (type 3): the noise sum of every row, exact (nvar = acc / 12 nb_rb loses the remainder the UL aggregation needs); null = not written */
};
__device__ __forceinline__ short2 mulshift(short2 a, short2 b, int s) /* c16mulShift (int arithmetic, wraps) */
{
  const int r = (int)((unsigned)((int)a.x * b.x) - (unsigned)((int)a.y * b.y));
  const int i = (int)((unsigned)((int)a.x * b.y) + (unsigned)((int)a.y * b.x));
  return make_short2((short)(r >> s), (short)(i >> s));
}
__device__ __forceinline__ short sat16(int v) { return (short)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
__device__ __forceinline__ short mulhrs(short a, short b) { return (short)(((int)a * b + 0x4000) >> 15); }
__global__ void k_ls(const Par j, const nr_gpu_chest_row_t *rows, short2 *ls)
{
  const nr_gpu_chest_row_t R = rows[blockIdx.x];
  short2 *o = ls + (size_t)blockIdx.x * 4096;
  const short2 *rx = (const short2 *)R.rxF, *pil = (const short2 *)R.pil;
  const int N = j.N, idx = 12 * R.nb_rb;
  for (int k = threadIdx.x; k < N; k += blockDim.x) {
    short2 v = make_short2(0, 0);
    if (k < idx) {
      int q, a, b;
      if ((R.type ? R.type : j.type) == 1) { q = k >> 2; a = R.k0 + 4 * q; b = a + 2; }
      else { q = k / 6; a = R.k0 + 6 * q; b = a + 1; }
      const short2 l = mulshift(pil[2 * q], rx[a % N], 15), r = mulshift(pil[2 * q + 1], rx[b % N], 15);
      v = make_short2((short)(((int)l.x + r.x) >> 1), (short)(((int)l.y + r.y) >> 1));
    }
    o[k] = v;
  }
}
__device__ __forceinline__ int fir_base(int p) { return p < 3 ? 0 : 4 * max(0, (p - 1) / 2 - 1); }
__global__ void k_fir(const Par j, const nr_gpu_chest_row_t *rows, const short2 *ls, const int *dly, unsigned *nvar)
{
  __shared__ unsigned long long red[256];
  const nr_gpu_chest_row_t R = rows[blockIdx.x];
  const short2 *L = ls + (size_t)blockIdx.x * 4096;
  short2 *out = (short2 *)R.out;
  const int N = j.N, idx = 12 * R.nb_rb, np = idx >> 1;
  const int d = dly[3 * blockIdx.x];
  int di = j.max_delay_comp + d, ii = j.max_delay_comp - d;
  di = min(max(di, 0), 2 * j.max_delay_comp);
  ii = min(max(ii, 0), 2 * j.max_delay_comp);
  const short2 *dt = (const short2 *)j.delay_table + (size_t)di * j.dt_stride;
  const short2 *it = (const short2 *)j.delay_table + (size_t)ii * j.dt_stride;
  unsigned long long acc = 0;
  for (int k = threadIdx.x; k < N; k += blockDim.x) {
    short yr = 0, yi = 0;
    const int p0 = max(0, (k - 16) / 2 - 1), p1 = min(np - 1, k / 2 + 6);
    for (int p = p0; p <= p1; p++) {
      const int b = fir_base(p);
      if (k < b || k >= b + 16)
        continue;
      const int f = p == 0 ? 0 : (p == 1 || p == 2) ? 1 : p == np - 1 ? 3 : 2;
      const short tap = j.filt[f][k - b];
      const short2 ch = mulshift(L[2 * p], dt[2 * p], 8);
      const short mr = mulhrs(ch.x, tap), mi = mulhrs(ch.y, tap);
      yr = sat16((int)yr + sat16((int)mr + mr));
      yi = sat16((int)yi + sat16((int)mi + mi));
    }
    short2 v = make_short2(yr, yi);
    if (k < idx) {
      v = mulshift(v, it[k], 8);
      const short er = (short)(L[k].x - v.x), ei = (short)(L[k].y - v.y);
      acc += (unsigned)((int)er * er) + (unsigned)((int)ei * ei);
    }
    out[k] = v;
  }
  red[threadIdx.x] = acc;
  __syncthreads();
  for (int s = blockDim.x / 2; s > 0; s >>= 1) {
    if (threadIdx.x < s)
      red[threadIdx.x] += red[threadIdx.x + s];
    __syncthreads();
  }
  if (threadIdx.x == 0) {
    nvar[blockIdx.x] = idx > 0 ? (unsigned)(red[0] / (unsigned long long)idx) : 0u;
    if (j.acc) j.acc[blockIdx.x] = red[0];
  }
}
} // namespace nrch
#define NR_GPU_PASS_UL_CHEST
#include "nr_gpu_pass_ul.cuh"
#undef NR_GPU_PASS_UL_CHEST

/* Q4 (DISCRETE): the standalone chest on staged copies. Per chunk of <= stg::rows rows: the rx span (12 nb_rb c16 from the row's k0, wrap resolved on the host, so the staged row
 * has k0 = 0) and the pilot span (6 or 4 nb_rb c16, shared pilot rows once) of every row go into the context's pinned block with the row descriptors rewritten to device
 * addresses, one async H2D of the used bytes; k_ls / k_delay_exact / k_fir run on the device copies; the estimate rows (N c16 each) and the delay / nvar ints come back in one
 * async D2H and are scattered to the caller's rows. Bit-identical to the DIRECT body: the same kernels on the same values. 0 or -1 (nothing written is consumed). */
static int chest_staged(nr_gpu_chest_job_t *j)
{
  if (j->delay_table != stg::dt_host || j->dt_stride != stg::dt_stride || j->max_delay_comp != stg::dt_mdc || j->type == 3
      || stg::fingerprint(j->delay_table, j->dt_stride, j->max_delay_comp) != stg::dt_fp) {
    nr_stage::refused(NR_GPU_STG_CHEST);
    return -1;
  }
  const int c = ctx_acquire(false);
  NrGpuRtScope rt;
  if (!rt.admitted) { ctx_release(c); return -1; }
  const auto t0 = std::chrono::steady_clock::now();
  stg::Ctx &x = stg::c[c];
  const cudaStream_t s = g_ctx_stream[c];
  const int N = 4096;
  nrch::Par par;
  par.type = j->type;
  par.N = j->N;
  par.dt_stride = j->dt_stride;
  par.max_delay_comp = j->max_delay_comp;
  par.delay_table = (const int16_t *)stg::dt.dev;
  memcpy(par.filt, j->filt, sizeof(par.filt));
  par.acc = nullptr;
  cudaError_t e = cudaSuccess;
  uint64_t h2d = 0, d2h = 0;
  int nh = 0, nd = 0;
  bool refused = false;
  for (int r0 = 0; r0 < j->n_rows && e == cudaSuccess && !refused;) {
    nr_gpu_chest_row_t srow[128];
    const int16_t *pil_ptr[128];
    size_t pil_off[128], pil_len[128];
    int npil = 0, n = 0;
    size_t off = 0;
    char *h = (char *)x.in.host;
    const int nmax = std::min(stg::rows, j->n_rows - r0);
    for (; n < nmax; n++) {
      const nr_gpu_chest_row_t &R = j->rows[r0 + n];
      const int idx = 12 * R.nb_rb, ty = R.type ? R.type : j->type;
      const size_t pn = (size_t)(ty == 1 ? 6 : 4) * R.nb_rb;
      const size_t rxb = ((size_t)idx * 4 + 15) & ~(size_t)15, pilb = (pn * 4 + 15) & ~(size_t)15;
      int pi = -1;
      for (int q = 0; q < npil; q++)
        if (pil_ptr[q] == R.pil && pil_len[q] >= pn) { pi = q; break; }
      if (off + rxb + (pi < 0 ? pilb : 0) + stg::rowb(n + 1) > x.in.bytes || (size_t)idx > stg::RXE || pn > stg::PILE) {
        if (n == 0) refused = true; /* a single row that does not fit: the CPU chest */
        break;
      }
      const int k0 = R.k0 % N; /* the kernel reads rxF[(k0 + i) % N] */
      const size_t first = std::min<size_t>((size_t)idx, (size_t)(N - k0));
      memcpy(h + off, R.rxF + 2 * (size_t)k0, first * 4);
      if (first < (size_t)idx) memcpy(h + off + first * 4, R.rxF, ((size_t)idx - first) * 4);
      srow[n] = R;
      srow[n].rxF = (const int16_t *)((char *)x.in.dev + off);
      srow[n].k0 = 0;
      off += rxb;
      if (pi < 0) {
        memcpy(h + off, R.pil, pn * 4);
        pil_ptr[npil] = R.pil; pil_off[npil] = off; pil_len[npil] = pn; pi = npil++;
        off += pilb;
      }
      srow[n].pil = (const int16_t *)((char *)x.in.dev + pil_off[pi]);
      srow[n].out = (int16_t *)x.out.dev + 2 * (size_t)N * n;
    }
    if (refused) break;
    const size_t rows_off = (off + 255) & ~(size_t)255;
    memcpy(h + rows_off, srow, (size_t)n * sizeof(nr_gpu_chest_row_t));
    const size_t in_n = rows_off + (size_t)n * sizeof(nr_gpu_chest_row_t);
    const nr_gpu_chest_row_t *rows = (const nr_gpu_chest_row_t *)((char *)x.in.dev + rows_off);
    int *const dints = (int *)((char *)x.out.dev + (size_t)n * N * 4);
    e = cudaMemcpyAsync(x.in.dev, x.in.host, in_n, cudaMemcpyHostToDevice, s);
    if (e == cudaSuccess) {
      nrch::k_ls<<<n, 256, 0, s>>>(par, rows, g_ctx_ls[c]);
      nr_exact::DelayIO io = {};
      io.base = g_ctx_ls[c];
      NR_EXACT_LAUNCH(g_isa, nr_exact::k_delay_exact, n, nr_exact::THREADS, s, io, g_d_perm, g_d_tw1024, g_d_tw4096, dints);
      nrch::k_fir<<<n, 256, 0, s>>>(par, rows, g_ctx_ls[c], dints, (unsigned *)(dints + 3 * n));
      e = cudaPeekAtLastError();
    }
    const size_t out_n = (size_t)n * N * 4 + (size_t)4 * n * sizeof(int);
    if (e == cudaSuccess) e = cudaMemcpyAsync(x.out.host, x.out.dev, out_n, cudaMemcpyDeviceToHost, s);
    if (e == cudaSuccess) e = (cudaError_t)ctx_wait(c, t0, NRSS_CHEST);
    if (e != cudaSuccess) break;
    const char *oh = (const char *)x.out.host;
    for (int i = 0; i < n; i++) memcpy(j->rows[r0 + i].out, oh + (size_t)i * N * 4, (size_t)N * 4);
    const int *hints = (const int *)(oh + (size_t)n * N * 4);
    memcpy(&j->delay[r0][0], hints, 3 * (size_t)n * sizeof(int));
    memcpy(j->nvar + r0, hints + 3 * n, (size_t)n * sizeof(int));
    h2d += in_n; d2h += out_n; nh++; nd++;
    r0 += n;
  }
  ctx_release(c);
  if (e != cudaSuccess) {
    g_init_state = -1;
    fprintf(stderr, "nr_pdsch_gpu_exact_chest: %s\n", cudaGetErrorString(e));
    return -1;
  }
  if (refused) { /* rows already scattered stay in the caller's buffers; the CPU chest rewrites every row of the job */
    nr_stage::refused(NR_GPU_STG_CHEST);
    return -1;
  }
  nr_stage::note(NR_GPU_STG_CHEST, h2d, nh, d2h, nd);
  return 0;
}

/* Q4: sets up the DISCRETE staging of the standalone chest / delay entries (host: nr_pdsch_gpu_exact_warmup, before RX starts). delay_table: the caller's table
 * (bytes long) which j->delay_table must equal on every chest job; it is uploaded once. Rows per chunk are sized from the shared memory budget (floor 16 rows);
 * -1 = off: the entries refuse and the caller runs the CPU chest. 0 = ready (also when called again with the same table). A DIRECT platform needs none: 0. */
extern "C" int nr_pdsch_gpu_exact_stage_init(const int16_t *delay_table, size_t bytes, int dt_stride, int max_delay_comp)
{
  if (g_init_state != 1 || !g_exact_ok) return -1;
  if (g_exact_direct) return 0;
  if (!delay_table || dt_stride < 4096 || max_delay_comp < 1 || bytes < (size_t)(2 * max_delay_comp + 1) * dt_stride * sizeof(short2)) return -1;
  if (stg::rows) return stg::dt_host == delay_table ? 0 : -1;
  const int nctx = nr_gpu_ctx_limit(EXACT_CTX);
  const size_t per_row = (stg::in_bytes(1) + stg::out_bytes(1)) * nctx;
  const int rows = (int)nr_gpu_membudget_fit(nr_gpu_stage_share(), bytes, per_row, 16, 128);
  if (rows < 16) return -1;
  bool ok = nr_gpu_buf_alloc(&stg::dt, bytes) == 0;
  for (int c = 0; c < nctx && ok; c++)
    ok = nr_gpu_buf_alloc(&stg::c[c].in, stg::in_bytes(rows)) == 0 && nr_gpu_buf_alloc(&stg::c[c].out, stg::out_bytes(rows)) == 0;
  if (ok) {
    memcpy(stg::dt.host, delay_table, bytes);
    /* the copy runs on the legacy default stream and the exact contexts' streams are cudaStreamNonBlocking: wait for it before the first chest job */
    ok = nr_gpu_buf_h2d_async(&stg::dt, 0, bytes, NULL) == 0 && nr_gpu_sync(NULL) == 0;
  }
  if (!ok) {
    for (int c = 0; c < nctx; c++) { nr_gpu_buf_free(&stg::c[c].in); nr_gpu_buf_free(&stg::c[c].out); }
    nr_gpu_buf_free(&stg::dt);
    return -1;
  }
  stg::dt_host = delay_table;
  stg::dt_fp = stg::fingerprint(delay_table, dt_stride, max_delay_comp);
  stg::dt_stride = dt_stride;
  stg::dt_mdc = max_delay_comp;
  stg::rows = rows;
  fprintf(stderr, "SENSING: GPU_EXACT staged chest/delay (DISCRETE): %d contexts x %d rows, %zu KB device and as much pinned host per context, delay table %zu KB\n", nctx, rows,
          (stg::in_bytes(rows) + stg::out_bytes(rows)) >> 10, bytes >> 10);
  return 0;
}

extern "C" int nr_pdsch_gpu_exact_chest(nr_gpu_chest_job_t *j)
{
  if (g_init_state != 1 || !g_exact_ok || (!g_exact_direct && !stg::ready()) || j->n_rows < 1 || j->n_rows > NR_GPU_CHEST_MAX_ROWS || j->N != 4096)
    return -1;
  for (int r = 0; r < j->n_rows; r++)
    if (j->rows[r].nb_rb < 1 || 12 * j->rows[r].nb_rb > 4096)
      return -1;
  if (!g_exact_direct) return chest_staged(j);
  const int c = ctx_acquire(false);
  NrGpuRtScope rt;
  if (!rt.admitted) { ctx_release(c); return -1; }
  const auto t0 = std::chrono::steady_clock::now();
  int *dout = (int *)nr_gpu_device_ptr(g_ctx_cout[c]);
  memcpy(g_ctx_rows[c], j->rows, (size_t)j->n_rows * sizeof(nr_gpu_chest_row_t));
  const nr_gpu_chest_row_t *rows = (const nr_gpu_chest_row_t *)nr_gpu_device_ptr(g_ctx_rows[c]);
  nrch::Par par;
  par.type = j->type;
  par.N = j->N;
  par.dt_stride = j->dt_stride;
  par.max_delay_comp = j->max_delay_comp;
  par.delay_table = j->delay_table;
  memcpy(par.filt, j->filt, sizeof(par.filt));
  par.acc = nullptr;
  nrch::k_ls<<<j->n_rows, 256, 0, g_ctx_stream[c]>>>(par, rows, g_ctx_ls[c]);
  {
    nr_exact::DelayIO io = {};
    io.base = g_ctx_ls[c]; /* every row in one launch */
    NR_EXACT_LAUNCH(g_isa, nr_exact::k_delay_exact, j->n_rows, nr_exact::THREADS, g_ctx_stream[c], io, g_d_perm, g_d_tw1024, g_d_tw4096, dout);
  }
  nrch::k_fir<<<j->n_rows, 256, 0, g_ctx_stream[c]>>>(par, rows, g_ctx_ls[c], dout, (unsigned *)(dout + 3 * NR_GPU_CHEST_MAX_ROWS));
  cudaError_t e = cudaPeekAtLastError();
  if (e == cudaSuccess) e = (cudaError_t)ctx_wait(c, t0, NRSS_CHEST);
  if (e == cudaSuccess) {
    memcpy(j->delay, g_ctx_cout[c], 3 * j->n_rows * sizeof(int));
    memcpy(j->nvar, g_ctx_cout[c] + 3 * NR_GPU_CHEST_MAX_ROWS, j->n_rows * sizeof(int));
  }
  ctx_release(c);
  if (e != cudaSuccess) {
    g_init_state = -1;
    fprintf(stderr, "nr_pdsch_gpu_exact_chest: %s\n", cudaGetErrorString(e));
    return -1;
  }
  return 0;
}

/* GPUPASS host step of the chest: next staging slot, wait for its previous pass, copy the job's first n rows to pinned staging. */
extern "C" int nr_pdsch_gpu_chest_stage(const nr_gpu_chest_job_t *j, int n, int *slot_out)
{
  if (g_init_state != 1 || !g_exact_ok || !j || !slot_out || n < 1 || n > j->n_rows || n > NR_GPU_CHEST_MAX_ROWS) return -1;
  const int k = PASS_RING;
  pass_slot_wait(g_pass_cev[k]);
  memcpy(g_pass_rh[k], j->rows, (size_t)n * sizeof(nr_gpu_chest_row_t));
  *slot_out = k;
  return 0;
}
/* the same for a slot chosen by the caller (a graph instance owns its slot) */
extern "C" int nr_pdsch_gpu_chest_stage_at(const nr_gpu_chest_job_t *j, int n, int slot)
{
  /* up to NR_GPU_CHEST_MAX_ROWS rows (the staging size); one launch takes at most PASS_LS_ROWS of them (nr_pdsch_gpu_chest_dev_at chunks) */
  if (g_init_state != 1 || !g_exact_ok || !j || n < 1 || n > j->n_rows || n > NR_GPU_CHEST_MAX_ROWS || slot < 0 || slot >= PASS_SLOTS) return -1;
  pass_slot_wait(g_pass_cev[slot]);
  memcpy(g_pass_rh[slot], j->rows, (size_t)n * sizeof(nr_gpu_chest_row_t));
  return 0;
}

/* GPUPASS: launch-only bit-exact chest of n rows of j after nr_pdsch_gpu_chest_stage returned `slot` (see nr_pdsch_gpu_chest.h). Enqueues the
 * rows H2D from the slot's pinned copy and the kernels on `stream`; graph-capturable. Delay triples and nvar go into nvar_dev, estimates to
 * row.out (all inside est_dev). One pass in flight (shared LS scratch). 0 or -1.
 * _at (v2 SEG128, the chunked C sub-graph): rows [r0, r0 + n) of a job staged whole (up to NR_GPU_CHEST_MAX_ROWS rows); the LS scratch is reused by the next chunk in stream
 * order; the delay triples and nvar of row r0 + i land at index r0 + i, as one launch of every row would write them (every row is independent: bit-exact). */
extern "C" int nr_pdsch_gpu_chest_dev_at(const nr_gpu_chest_job_t *j, int r0, int n, nr_gpu_buf_t *est_dev, nr_gpu_buf_t *nvar_dev, int slot, void *stream)
{
  if (g_init_state != 1 || !g_exact_ok || !j || !est_dev || !est_dev->dev || !nvar_dev || !nvar_dev->dev || nvar_dev->bytes < NR_GPU_CHEST_DEV_BYTES
      || r0 < 0 || n < 1 || r0 + n > j->n_rows || r0 + n > NR_GPU_CHEST_MAX_ROWS || n > PASS_LS_ROWS || (r0 && j->type == 3) || j->N != 4096 || slot < 0 || slot >= PASS_SLOTS)
    return -1;
  for (int r = r0; r < r0 + n; r++) {
    const char *o = (const char *)j->rows[r].out;
    if (j->rows[r].nb_rb < 1 || 12 * j->rows[r].nb_rb > 4096 || o < (const char *)est_dev->dev
        || o + 4096 * sizeof(short2) > (const char *)est_dev->dev + est_dev->bytes)
      return -1;
  }
  const cudaStream_t s = (cudaStream_t)stream;
  if (cudaMemcpyAsync(g_pass_rd[slot] + r0, g_pass_rh[slot] + r0, (size_t)n * sizeof(nr_gpu_chest_row_t), cudaMemcpyHostToDevice, s) != cudaSuccess) return -1;
  const nr_gpu_chest_row_t *rows = g_pass_rd[slot] + r0;
  int *const dbase = (int *)nvar_dev->dev, *dout = dbase + 3 * r0; /* delay triples of the chunk's rows; nvar below at dbase + 3 * MAX_ROWS + r0 */
  nrch::Par par;
  par.type = j->type;
  par.N = j->N;
  par.dt_stride = j->dt_stride;
  par.max_delay_comp = j->max_delay_comp;
  par.delay_table = j->delay_table;
  memcpy(par.filt, j->filt, sizeof(par.filt));
  /* type 3 (GPUPASS Task 7, the PUSCH pass): the UL least-squares step, and the exact noise sum of every row at u64 [512 + row] of the nvar area */
  par.acc = j->type == 3 ? (unsigned long long *)(dbase + 3 * NR_GPU_CHEST_MAX_ROWS) + 512 : nullptr;
  if (j->type == 3) nrch::k_ul_ls<<<n, 256, 0, s>>>(par, rows, g_pass_ls[slot]);
  else nrch::k_ls<<<n, 256, 0, s>>>(par, rows, g_pass_ls[slot]);
  {
    nr_exact::DelayIO io = {};
    io.base = g_pass_ls[slot];
    NR_EXACT_LAUNCH(g_isa, nr_exact::k_delay_exact, n, nr_exact::THREADS, s, io, g_d_perm, g_d_tw1024, g_d_tw4096, dout);
  }
  nrch::k_fir<<<n, 256, 0, s>>>(par, rows, g_pass_ls[slot], dout, (unsigned *)(dbase + 3 * NR_GPU_CHEST_MAX_ROWS) + r0);
  if (cudaPeekAtLastError() != cudaSuccess) return -1;
  return pass_slot_done(g_pass_cev[slot], s) == cudaSuccess ? 0 : -1;
}
extern "C" int nr_pdsch_gpu_chest_dev(const nr_gpu_chest_job_t *j, int n, nr_gpu_buf_t *est_dev, nr_gpu_buf_t *nvar_dev, int slot, void *stream)
{
  return nr_pdsch_gpu_chest_dev_at(j, 0, n, est_dev, nvar_dev, slot, stream);
}

/* GPUPASS: LS rows per pass the chest can take (the graph classes are limited to it) */
extern "C" int nr_pdsch_gpu_pass_ls_rows(void) { return PASS_LS_ROWS; }

/* ULFEP: GPU front end of the passive PUSCH decoder (own file, own streams; see the header comment) */
#include "nr_pusch_gpu_fep.cuh"

/* GPUPASS: device kernels for the host steps between the stages (chest post, rate dematching, CRC) */
#include "nr_gpu_pass_post.cuh"
