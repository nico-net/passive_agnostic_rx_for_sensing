/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* ULFEP (csd/ulfep): GPU front end of the passive PUSCH decoder, always on when libpdsch_gpu.so loads.
 *
 * Included at the end of nr_pdsch_gpu_fep.cu (same translation unit: green contexts, nr_exact FFT, poll helper) and
 * exported from libpdsch_gpu.so; the host glue is nr_pusch_gpu_fep.h. It runs the SAME bit-exact int16 DFT as the PDSCH
 * exact FEP (nr_exact::fft4096 + apply_nr_rotation_symbol_RX), but on UL windows: the caller (nr_pusch_passive_decode.c)
 * resolves every window itself (slot, symbol, timing advance -> pointer into the UL snapshot) and hands a batch of
 * items {4 antenna window pointers, 4 output row pointers, rotation}. One block per (item, antenna); a batch is one
 * launch, so the timing-search candidates and the grant's symbols cost one launch each instead of 4 CPU FFTs apiece.
 *
 * Isolation from the DL pipeline (nothing here touches the PDSCH streams, contexts, buffers or budget scopes):
 *   - own streams: on the real-time SM partition at the least priority, so PDSCH keeps its SMs; without a partition lowest priority, and a call is refused while any RT GPU user is in flight (nr_gpu_budget_rt_active);
 *   - duty cap: at most the caller-given pct percent of every 100 ms window may be spent in UL launches; over
 *     the cap, or all contexts busy, the call returns an error and the caller runs the CPU FEP (counted);
 *   - no spin: the wait polls cudaStreamQuery, spins 60 us, then nanosleeps 25 us per poll;
 *   - the duty cap is charged with the kernel's own span (first block start to last block end, %globaltimer), not the wall
 *     time that includes waiting behind DL kernels (VL2: one starved 26 ms call tripped the 30 % cap and sent UL to the CPU
 *     for the rest of the window);
 *   - everything (tables, streams, item tables, first launch) is created in warm-up, before the RX allocation fence. */
#include <unistd.h>
#include <time.h>
#include <thread>
#include "nr_pusch_gpu_diag.h"
#include "nr_gpu_stage_census.h"
#include "PHY/MODULATION/nr_sfo_rot.h" /* nr_sfo_re / nr_sfo_bin_k: the per-RE SFO step, bit-identical to nr_sfo_rotate_symbol */

namespace ulfep {
struct Item {
  const short2 *src[4]; /* window start of each antenna (host memory, direct mode) */
  short2 *dst[4];       /* N-sample output row of each antenna */
  short2 rot;           /* fp->symbol_rotation[link_type_ul][symbol + (slot % slots_per_subframe) * symbols_per_slot] */
  int rotate;           /* 1 = apply_nr_rotation_symbol_RX, 0 = bare DFT (layout probe) */
  uint32_t sfo_dphi;    /* SFO step after the rotation on the same bins (nr_sfo_rotate_symbol), 0 = none */
  int sfo_kref;         /* its reference subcarrier (nr_sfo_kref) */
};
static const int MAX_ITEMS = 64, NCTX = 4;

/* the body of one (item, antenna) block; src == null (a pass item that is not used) leaves the block without effect.
 * P: the nr_isa profile (DFT and rotation twins of the host CPU, nr_exact::isa_profile) */
template <int P>
__device__ __forceinline__ void ul_fep_blk(const Item &it, int ant, const uint16_t *__restrict__ perm, const short *__restrict__ tw1024,
                                           const short *__restrict__ tw4096, const short2 *__restrict__ ts, int nb_rb, int fco)
{
  using namespace nr_exact;
  __shared__ short2 s[smem_c16(P)];
  const int t = threadIdx.x;
  const short2 *in = it.src[ant];
  if (!in) return; /* uniform over the block */
  for (int n = t; n < N; n += THREADS) s[n] = in[__ldg(&perm[n])];
  __syncthreads();
  fft_twin<P, false>(s, t, tw1024, tw4096);
  const int len = (nb_rb & 1) ? (nb_rb + 1) * 6 : nb_rb * 6, start2 = (nb_rb & 1) ? fco - 6 : fco;
  const int ar = it.rot.x, ai = nr_isa::xneg(it.rot.y);
  short2 *o = it.dst[ant];
  for (int k = t; k < N; k += THREADS) {
    short2 v = s[k];
    if (it.rotate && (k < len || (k >= start2 && k < start2 + len))) {
      v = rot_ts<P>(v, ar, ai, __ldg(&ts[k])); /* identical arithmetic to k_fep_exact (rotation twin of P, then mult_cpx_vector) */
      if (it.sfo_dphi) {
        const nr_sfo_c16_t r = nr_sfo_re(nr_sfo_c16_t{v.x, v.y}, nr_sfo_bin_k(k, N) - it.sfo_kref, it.sfo_dphi);
        v = make_short2(r.r, r.i);
      }
    }
    o[k] = v;
  }
}

__device__ __forceinline__ unsigned long long ul_gtimer()
{
  unsigned long long t;
  asm volatile("mov.u64 %0, %%globaltimer;" : "=l"(t));
  return t;
}
/* span (ULST KCHARGE, may be null): {start, end} ns of every block at [2 * (y * gridDim.x + x)] */
template <int P>
__global__ void __launch_bounds__(nr_exact::THREADS) k_ul_fep(const Item *__restrict__ items, const uint16_t *__restrict__ perm,
                                                              const short *__restrict__ tw1024, const short *__restrict__ tw4096,
                                                              const short2 *__restrict__ ts, int nb_rb, int fco, unsigned long long *span)
{
  const int b = 2 * (blockIdx.y * gridDim.x + blockIdx.x);
  if (span && threadIdx.x == 0) span[b] = ul_gtimer();
  ul_fep_blk<P>(items[blockIdx.x], blockIdx.y, perm, tw1024, tw4096, ts, nb_rb, fco);
  if (span) {
    __syncthreads();
    if (threadIdx.x == 0) span[b + 1] = ul_gtimer();
  }
}
/* GPUPASS Task 7: the same block for the graph pass: the rotation geometry (nb_rb, fco) is read from device memory when the kernel runs, and an item with src[0] == null is skipped */
template <int P>
__global__ void __launch_bounds__(nr_exact::THREADS) k_ul_fep_p(const Item *__restrict__ items, const int *__restrict__ par, const uint16_t *__restrict__ perm,
                                                                const short *__restrict__ tw1024, const short *__restrict__ tw4096, const short2 *__restrict__ ts)
{
  ul_fep_blk<P>(items[blockIdx.x], blockIdx.y, perm, tw1024, tw4096, ts, par[0], par[1]);
}

struct State {
  bool ok = false, part = false;
  int nant = 0, kind = 0, isa = 0; /* isa: nr_isa profile of the kernels */
  uint16_t *d_perm = nullptr;
  short *d_tw1024 = nullptr, *d_tw4096 = nullptr;
  short2 *d_ts = nullptr;
  cudaStream_t st[NCTX] = {};
  cudaEvent_t ev0[NCTX] = {}, ev1[NCTX] = {}; /* stall split (uldiag) */
  Item *h_items[NCTX] = {}, *d_items[NCTX] = {};
  std::atomic<unsigned> busy{0};
  unsigned long long *h_span[NCTX] = {}, *d_span[NCTX] = {}; /* MAX_ITEMS x 4 x {start, end} ns  */
  bool staged = false;        /* Q4: DISCRETE tier: the kernels read device copies of the windows (below), not the caller's host IQ */
  int nctx = NCTX;            /* contexts in use (staged: as many as the shared memory budget allows) */
  nr_gpu_buf_t in[NCTX] = {}, out[NCTX] = {}, spn[NCTX] = {}; /* staged: [items][merged windows] / [output rows] / [kernel spans]: pinned mirror + device */
  unsigned pct = 20;
  std::atomic<uint64_t> win_start_us{0}, win_used_us{0};
  std::atomic<uint64_t> calls{0}, items{0}, gpu_us{0}, max_us{0}, n_busy{0}, n_budget{0}, n_rt{0}, n_late{0}, n_err{0};
};
static State U;

static uint64_t now_us()
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
}  // namespace ulfep

#define UCK(x)                                                                                 \
  do {                                                                                         \
    cudaError_t e_ = (x);                                                                      \
    if (e_ != cudaSuccess) {                                                                   \
      fprintf(stderr, "nr_pusch_gpu_fep: %s at %s:%d\n", cudaGetErrorString(e_), __FILE__, __LINE__); \
      return -1;                                                                               \
    }                                                                                          \
  } while (0)

/* ---- Q4: the staged DISCRETE run of a batch --------------------------------------------------------------------------------------------------------
 * Each item's windows are N-sample slices of the caller's UL snapshot. Per antenna the slices are sorted and merged where they overlap or touch (the timing search asks
 * for windows a few samples apart), every merged span is copied once into the context's pinned block behind the items, and the items are rewritten to device addresses
 * (source = merged span + offset, destination = a row of the output block). One async H2D of the used bytes, the kernel, one async D2H of the output rows and of the kernel
 * spans; the rows are scattered to the callers' destinations once the stream has finished. */
namespace ulfep {
static size_t al256(size_t b) { return (b + 255) & ~(size_t)255; }
static size_t in_bytes() { return al256(MAX_ITEMS * sizeof(Item)) + (size_t)MAX_ITEMS * 4 * nr_exact::N * sizeof(short2); }
static size_t out_bytes(int nant) { return (size_t)MAX_ITEMS * nant * nr_exact::N * sizeof(short2); }
static size_t spn_bytes(int nant) { return (size_t)MAX_ITEMS * nant * 2 * sizeof(unsigned long long); }
/* fills the context's block for items it[0..n) and enqueues copy + kernel + copy back on its stream; *h2d / *d2h = bytes copied */
static cudaError_t stage_enqueue(int c, const Item *it, int n, int nb_rb, int fco, uint64_t *h2d, uint64_t *d2h)
{
  const size_t W = (size_t)nr_exact::N * sizeof(short2);
  const int nant = U.nant;
  char *h = (char *)U.in[c].host;
  Item *di = (Item *)h;
  size_t off = al256((size_t)n * sizeof(Item));
  memcpy(di, it, (size_t)n * sizeof(Item));
  for (int a = 0; a < nant; a++) {
    int ord[MAX_ITEMS], m = 0;
    for (int i = 0; i < n; i++)
      if (it[i].src[a]) ord[m++] = i;
    for (int x = 1; x < m; x++) { /* insertion sort by address */
      const int v = ord[x];
      int y = x - 1;
      while (y >= 0 && (uintptr_t)it[ord[y]].src[a] > (uintptr_t)it[v].src[a]) { ord[y + 1] = ord[y]; y--; }
      ord[y + 1] = v;
    }
    for (int x = 0; x < m;) {
      const char *s0 = (const char *)it[ord[x]].src[a];
      const char *e0 = s0 + W;
      int y = x + 1;
      while (y < m && (const char *)it[ord[y]].src[a] <= e0) {
        const char *e1 = (const char *)it[ord[y]].src[a] + W;
        if (e1 > e0) e0 = e1;
        y++;
      }
      memcpy(h + off, s0, (size_t)(e0 - s0));
      for (int z = x; z < y; z++)
        di[ord[z]].src[a] = (const short2 *)((char *)U.in[c].dev + off + ((const char *)it[ord[z]].src[a] - s0));
      off += al256((size_t)(e0 - s0));
      x = y;
    }
  }
  for (int i = 0; i < n; i++)
    for (int a = 0; a < nant; a++) di[i].dst[a] = (short2 *)((char *)U.out[c].dev + ((size_t)i * nant + a) * W);
  const cudaStream_t st = U.st[c];
  cudaError_t e = cudaMemcpyAsync(U.in[c].dev, U.in[c].host, off, cudaMemcpyHostToDevice, st);
  *h2d = off;
  if (e != cudaSuccess) return e;
  NR_EXACT_LAUNCH(U.isa, k_ul_fep, dim3(n, nant), nr_exact::THREADS, st, (const Item *)U.in[c].dev, U.d_perm, U.d_tw1024, U.d_tw4096, U.d_ts, nb_rb, fco, U.d_span[c]);
  e = cudaPeekAtLastError();
  if (e != cudaSuccess) return e;
  e = cudaMemcpyAsync(U.out[c].host, U.out[c].dev, (size_t)n * nant * W, cudaMemcpyDeviceToHost, st);
  if (e == cudaSuccess) e = cudaMemcpyAsync(U.spn[c].host, U.spn[c].dev, (size_t)n * nant * 2 * sizeof(unsigned long long), cudaMemcpyDeviceToHost, st);
  *d2h = (size_t)n * nant * (W + 2 * sizeof(unsigned long long));
  return e;
}
/* after the stream finished: the output rows to the callers' destinations */
static void stage_collect(int c, const Item *it, int n)
{
  const size_t W = (size_t)nr_exact::N * sizeof(short2);
  for (int i = 0; i < n; i++)
    for (int a = 0; a < U.nant; a++)
      memcpy(it[i].dst[a], (const char *)U.out[c].host + ((size_t)i * U.nant + a) * W, W);
}
} // namespace ulfep

/* Warm-up (before the RX allocation fence). 0 = ready, -1 = unavailable (callers keep the CPU FEP). */
static int ul_init_impl(int nant, int N, int ncp, int offset_div, int pct, int slot_us)
{
  using namespace ulfep;
  (void)slot_us; /* kept for the exported ABI */
  if (U.ok) return U.nant == nant ? 0 : -1;
  if (N != nr_exact::N || nant < 1 || nant > 4 || offset_div <= 0) return -1;
  if (!nr_gpu_module_ready()) return -1;
  U.staged = !nr_gpu_platform_shared_direct(); /* Q4: DISCRETE tier: staged copies instead of reading the host IQ in place */
  U.isa = nr_exact::isa_profile();
  if (U.isa == nr_isa::P_NONE) {
    fprintf(stderr, "nr_pusch_gpu_fep: no bit-identical GPU twin of the host CPU DFT/rotation (%s) -- CPU FEP\n", nr_exact::g_isa_desc);
    return -1;
  }
  U.nant = nant;
  U.pct = pct > 0 && pct <= 100 ? (unsigned)pct : 20u;
  std::vector<uint16_t> perm;
  std::vector<short> tw1, tw4, ts(2 * N);
  nr_exact::twin_tables(U.isa, perm, tw1, tw4);
  UCK(nr_gpu_allocate(NR_GPU_DEVICE, (void **)&U.d_perm, N * sizeof(uint16_t)));
  UCK(nr_gpu_allocate(NR_GPU_DEVICE, (void **)&U.d_tw1024, tw1.size() * sizeof(short)));
  UCK(nr_gpu_allocate(NR_GPU_DEVICE, (void **)&U.d_tw4096, tw4.size() * sizeof(short)));
  UCK(nr_gpu_allocate(NR_GPU_DEVICE, (void **)&U.d_ts, N * sizeof(short2)));
  const int so = ncp / offset_div; /* init_timeshift_rotation(): same libm, same formula as the DL exact FEP */
  for (int i = 0; i < N; i++) {
    const double poff = -i * 2.0 * M_PI * so / N;
    ts[2 * i] = (int16_t)round(cos(poff) * 32767);
    ts[2 * i + 1] = (int16_t)round(sin(-poff) * 32767);
  }
  UCK(cudaMemcpy(U.d_perm, perm.data(), N * sizeof(uint16_t), cudaMemcpyHostToDevice));
  UCK(cudaMemcpy(U.d_tw1024, tw1.data(), tw1.size() * sizeof(short), cudaMemcpyHostToDevice));
  UCK(cudaMemcpy(U.d_tw4096, tw4.data(), tw4.size() * sizeof(short), cudaMemcpyHostToDevice));
  UCK(cudaMemcpy(U.d_ts, ts.data(), ts.size() * sizeof(short), cudaMemcpyHostToDevice));
  cudaFuncAttributes attr;
  UCK(NR_EXACT_ATTR(U.isa, &attr, ulfep::k_ul_fep));
  int least = 0, greatest = 0;
  UCK(cudaDeviceGetStreamPriorityRange(&least, &greatest));
  /* host buffers the warm-up launch reads and writes: written first, so the GPU never first-touches an unwritten page */
  const size_t row = (size_t)N * sizeof(short2);
  short2 *buf = nullptr;
  if (posix_memalign((void **)&buf, 4096, row * 2)) return -1;
  memset(buf, 0, row * 2);
  if (U.staged) {
    const size_t per = in_bytes() + out_bytes(nant) + spn_bytes(nant);
    U.nctx = (int)nr_gpu_membudget_fit(nr_gpu_stage_share(), 0, per, 1, NCTX);
    if (U.nctx < 1) { free(buf); return -1; }
  }
  for (int c = 0; c < U.nctx; c++) {
    const int kind = nr_pusch_gpu_ul_stream(&U.st[c], greatest, least, c);
    if (!kind) UCK(cudaStreamCreateWithPriority(&U.st[c], cudaStreamNonBlocking, least));
    else { U.part = true; U.kind = kind; }
    if (uldiag::on()) { UCK(cudaEventCreate(&U.ev0[c])); UCK(cudaEventCreate(&U.ev1[c])); }
    if (U.staged) {
      if (nr_gpu_buf_alloc(&U.in[c], in_bytes()) != 0 || nr_gpu_buf_alloc(&U.out[c], out_bytes(nant)) != 0 || nr_gpu_buf_alloc(&U.spn[c], spn_bytes(nant)) != 0) {
        fprintf(stderr, "nr_pusch_gpu_fep: staging buffers do not fit -- CPU FEP\n");
        free(buf);
        return -1;
      }
      U.h_span[c] = (unsigned long long *)U.spn[c].host;
      U.d_span[c] = (unsigned long long *)U.spn[c].dev;
      Item w = {};
      for (int a = 0; a < 4; a++) { w.src[a] = buf; w.dst[a] = buf + N; }
      w.rotate = 1;
      uint64_t hb = 0, db = 0;
      UCK(stage_enqueue(c, &w, 1, 106, N - 636, &hb, &db)); /* the first launch, before the RX fence */
      UCK((cudaError_t)nr_gpu_sync(U.st[c]));
      continue;
    }
    UCK(nr_gpu_allocate(NR_GPU_STAGING, (void **)&U.h_items[c], MAX_ITEMS * sizeof(Item)));
    memset(U.h_items[c], 0, MAX_ITEMS * sizeof(Item));
    U.d_items[c] = (Item *)nr_gpu_device_ptr(U.h_items[c]);
    if (!U.d_items[c]) return -1;
    UCK(nr_gpu_allocate(NR_GPU_STAGING, (void **)&U.h_span[c], MAX_ITEMS * 4 * 2 * sizeof(unsigned long long)));
    memset(U.h_span[c], 0, MAX_ITEMS * 4 * 2 * sizeof(unsigned long long));
    U.d_span[c] = (unsigned long long *)nr_gpu_device_ptr(U.h_span[c]);
    if (!U.d_span[c]) return -1;
    Item &it = U.h_items[c][0];
    for (int a = 0; a < 4; a++) { it.src[a] = buf; it.dst[a] = buf + N; }
    it.rotate = 1;
    NR_EXACT_LAUNCH(U.isa, k_ul_fep, dim3(1, nant), nr_exact::THREADS, U.st[c], U.d_items[c], U.d_perm, U.d_tw1024, U.d_tw4096, U.d_ts, 106, N - 636, U.d_span[c]);
    UCK(cudaPeekAtLastError());
    UCK((cudaError_t)nr_gpu_sync(U.st[c]));
  }
  free(buf);
  U.ok = true;
  fprintf(stderr, "SENSING: UL_GPU_FEP ready: %d RX, N=%d, %s stream, cap %u%% of each 100 ms%s\n", nant, N,
          U.kind == 2 ? "real-time-partition" : U.part ? "discovery-partition" : "lowest-priority", U.pct, U.staged ? " (DISCRETE: staged copies)" : "");
  if (U.staged)
    fprintf(stderr, "SENSING: UL_GPU_FEP staged: %d contexts x (%zu KB device + %zu KB pinned)\n", U.nctx, (in_bytes() + out_bytes(nant) + spn_bytes(nant)) >> 10,
            (in_bytes() + out_bytes(nant) + spn_bytes(nant)) >> 10);
  return 0;
}

/* The PDSCH init of this module seals its allocation fence (nr_dl_gpu_sealed) when it finishes; the UL front end is
 * created later (UL monitor start-up), still before the platform-wide RX seal (nr_gpu_allocation_guard), so it lifts the
 * module fence for the duration of its own init only. */
extern "C" int nr_pusch_gpu_fep_init(int nant, int N, int ncp, int offset_div, int pct, int slot_us)
{
  const bool was = nr_dl_gpu_sealed;
  nr_dl_gpu_sealed = false;
  const int rc = ul_init_impl(nant, N, ncp, offset_div, pct, slot_us);
  nr_dl_gpu_sealed = was;
  return rc;
}

namespace ulfep {
/* kernel span of the last launch on context c (n items x nant blocks), us; 0 = not measurable */
static uint64_t span_us(int c, int nblk)
{
  unsigned long long lo = ~0ull, hi = 0;
  for (int b = 0; b < nblk; b++) {
    lo = std::min(lo, U.h_span[c][2 * b]);
    hi = std::max(hi, U.h_span[c][2 * b + 1]);
  }
  return hi > lo ? (hi - lo) / 1000 : 0;
}
}  // namespace ulfep

/* One batch (<= MAX_ITEMS items x nant antennas). 0 = rows written; -1 all contexts busy; -2 over the duty cap / RT in flight;
 * -3 module off or CUDA error. On non-zero
 * nothing was written and the caller runs the CPU FEP. */
extern "C" int nr_pusch_gpu_fep_run(const void *items, int n, int nb_rb, int fco)
{
  using namespace ulfep;
  if (!U.ok || n < 1 || n > MAX_ITEMS || nb_rb < 1 || nb_rb > 275 || fco < 0 || fco >= nr_exact::N) return -3;
  int c = -1;
  for (int i = 0; i < U.nctx && c < 0; i++)
    if (!(U.busy.fetch_or(1u << i) >> i & 1u)) c = i;
  if (c < 0) { ++U.n_busy; return -1; }
  const uint64_t t0 = now_us();
  uint64_t w0 = U.win_start_us.load();
  if (t0 - w0 >= 100000 && U.win_start_us.compare_exchange_strong(w0, t0)) U.win_used_us.store(0);
  if (U.win_used_us.load() >= (uint64_t)U.pct * 1000) {
    ++U.n_budget;
    U.busy.fetch_and(~(1u << c));
    return -2;
  }
  if (!U.part && nr_gpu_budget_rt_active() > 0) {
    ++U.n_rt;
    U.busy.fetch_and(~(1u << c));
    return -2;
  }
  const bool dg = uldiag::on();
  const uldiag::Snap r0 = dg ? uldiag::snap() : uldiag::Snap();
  uint64_t st_h2d = 0, st_d2h = 0;
  cudaError_t e = cudaSuccess;
  if (U.staged) { /* Q4: DISCRETE tier: copy, kernel and copy back are enqueued together (the event pair brackets all three) */
    if (dg) cudaEventRecord(U.ev0[c], U.st[c]);
    e = stage_enqueue(c, (const Item *)items, n, nb_rb, fco, &st_h2d, &st_d2h);
  } else {
    memcpy(U.h_items[c], items, (size_t)n * sizeof(Item));
    if (dg) cudaEventRecord(U.ev0[c], U.st[c]);
    NR_EXACT_LAUNCH(U.isa, k_ul_fep, dim3(n, U.nant), nr_exact::THREADS, U.st[c], U.d_items[c], U.d_perm, U.d_tw1024, U.d_tw4096, U.d_ts, nb_rb, fco,
                                                                  U.d_span[c]);
    e = cudaPeekAtLastError();
  }
  if (dg) cudaEventRecord(U.ev1[c], U.st[c]);
  const uint64_t t_launched = now_us();
  while (e == cudaSuccess) { /* the kernel writes the caller's rows, so a late kernel is waited for, never abandoned */
    e = cudaStreamQuery(U.st[c]);
    if (e == cudaErrorNotReady) {
      e = cudaSuccess;
      const uint64_t el = now_us() - t0;
      if (el > 60) {
        const struct timespec ts = {0, 25000};
        nanosleep(&ts, NULL);
      }
    } else
      break;
  }
  const uint64_t dt = now_us() - t0;
  if (dg && e == cudaSuccess) {
    float ms = 0.f;
    if (cudaEventElapsedTime(&ms, U.ev0[c], U.ev1[c]) != cudaSuccess) ms = 0.f;
    uldiag::report("fep", dt, t_launched - t0, 1000.0 * ms, n, r0, uldiag::snap());
  }
  if (e != cudaSuccess) {
    U.busy.fetch_and(~(1u << c));
    U.ok = false; /* the caller's CPU FEP takes over for good */
    ++U.n_err;
    fprintf(stderr, "nr_pusch_gpu_fep: %s -- UL GPU FEP off\n", cudaGetErrorString(e));
    return -3;
  }
  const uint64_t ks = span_us(c, n * U.nant);
  if (U.staged) {
    stage_collect(c, (const Item *)items, n);
    nr_stage::note(NR_GPU_STG_ULFEP, st_h2d, 1, st_d2h, 2);
  }
  U.busy.fetch_and(~(1u << c));
  ++U.calls;
  U.items += (uint64_t)n;
  U.gpu_us += dt;
  U.win_used_us += ks ? ks : dt;
  if (dt > 2000) ++U.n_late;
  uint64_t m = U.max_us.load();
  while (dt > m && !U.max_us.compare_exchange_weak(m, dt)) {}
  return 0;
}

/* calls, items, gpu_us, max_us, busy, over_cap, rt_defer, late(>2 ms), errors, partitioned, staged runs, h2d bytes, d2h bytes, staged in use */
extern "C" void nr_pusch_gpu_fep_stats(uint64_t *o)
{
  using namespace ulfep;
  o[0] = U.calls; o[1] = U.items; o[2] = U.gpu_us; o[3] = U.max_us; o[4] = U.n_busy; o[5] = U.n_budget; o[6] = U.n_rt;
  o[7] = U.n_late; o[8] = U.n_err; o[9] = U.part;
  uint64_t q[6];
  for (int k = 0; k < 6; k++) q[k] = nr_stage::g_c[NR_GPU_STG_ULFEP][k].load(std::memory_order_relaxed);
  o[10] = q[0]; o[11] = q[1]; o[12] = q[2]; o[13] = U.staged; /* Q4: staged runs, H2D bytes, D2H bytes, 1 = the staged (DISCRETE) variant is in use */
}

/* GPUPASS Task 7: launch-only FEP of the PUSCH pass (graph-capturable). items_dev: 14 ulfep::Item (nr_pusch_gpu_fep_item_t) readable by the kernel, par_dev: {nb_rb, fco}
 * (apply_nr_rotation_symbol_RX geometry); both are rewritten by the host between replays. Uses the tables of the exact PDSCH FEP (the same bits as this file's own,
 * ). 0 or -1. */
extern "C" int nr_gpu_pass_ul_fep_dev(const void *items_dev, const int *par_dev, int nant, void *stream)
{
  if (g_init_state != 1 || !g_exact_ok || !g_d_perm || g_cfg.ofdm_symbol_size != nr_exact::N || !items_dev || !par_dev || nant < 1 || nant > 4) return -1; /* the DL tables of N = 4096 only */
  NR_EXACT_LAUNCH(g_isa, ulfep::k_ul_fep_p, dim3(14, nant), nr_exact::THREADS, (cudaStream_t)stream, (const ulfep::Item *)items_dev, par_dev, g_d_perm, g_d_tw1024, g_d_tw4096, g_d_ts);
  return cudaPeekAtLastError() == cudaSuccess ? 0 : -1;
}
