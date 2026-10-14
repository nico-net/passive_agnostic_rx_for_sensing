/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* ULFEP (csd/ulfep): GPU front end of the passive PUSCH decoder, ISAC_UL_GPU_FEP=1 (default off).
 *
 * Included at the end of nr_pdsch_gpu_fep.cu (same translation unit: green contexts, nr_exact FFT, poll helper) and
 * exported from libpdsch_gpu.so; the host glue is nr_pusch_gpu_fep.h. It runs the SAME bit-exact int16 DFT as the PDSCH
 * exact FEP (nr_exact::fft4096 + apply_nr_rotation_symbol_RX), but on UL windows: the caller (nr_pusch_passive_decode.c)
 * resolves every window itself (slot, symbol, timing advance -> pointer into the UL snapshot) and hands a batch of
 * items {4 antenna window pointers, 4 output row pointers, rotation}. One block per (item, antenna); a batch is one
 * launch, so the timing-search candidates and the grant's symbols cost one launch each instead of 4 CPU FFTs apiece.
 *
 * Isolation from the DL pipeline (nothing here touches the PDSCH streams, contexts, buffers or budget scopes):
 *   - own streams: on the discovery SM partition when ISAC_GPU_DISC_SMS / ISAC_GPU_RT_ISOLATE give one, so PDSCH keeps its
 *     SMs; otherwise lowest priority, and a call is refused while any RT GPU user is in flight (nr_gpu_budget_rt_active);
 *   - duty cap: at most ISAC_UL_GPU_FEP_PCT (default 20) percent of every 100 ms window may be spent in UL launches; over
 *     the cap, or all contexts busy, the call returns an error and the caller runs the CPU FEP (counted);
 *   - no spin: the wait polls cudaStreamQuery, spins 60 us, then nanosleeps 25 us per poll;
 *   - everything (tables, streams, item tables, first launch) is created in warm-up, before the RX allocation fence. */
#include <unistd.h>
#include <time.h>

namespace ulfep {
struct Item {
  const short2 *src[4]; /* window start of each antenna (host memory, direct mode) */
  short2 *dst[4];       /* N-sample output row of each antenna */
  short2 rot;           /* fp->symbol_rotation[link_type_ul][symbol + (slot % slots_per_subframe) * symbols_per_slot] */
  int rotate;           /* 1 = apply_nr_rotation_symbol_RX, 0 = bare DFT (layout probe) */
};
static const int MAX_ITEMS = 64, NCTX = 4;

__global__ void __launch_bounds__(nr_exact::THREADS) k_ul_fep(const Item *__restrict__ items, const uint16_t *__restrict__ perm,
                                                              const short *__restrict__ tw1024, const short *__restrict__ tw4096,
                                                              const short2 *__restrict__ ts, int nb_rb, int fco)
{
  using namespace nr_exact;
  __shared__ short2 s[N];
  const Item it = items[blockIdx.x];
  const int ant = blockIdx.y, t = threadIdx.x;
  const short2 *in = it.src[ant];
  for (int n = t; n < N; n += THREADS) s[n] = in[__ldg(&perm[n])];
  __syncthreads();
  fft4096<false>(s, t, tw1024, tw4096);
  const int len = (nb_rb & 1) ? (nb_rb + 1) * 6 : nb_rb * 6, start2 = (nb_rb & 1) ? fco - 6 : fco;
  const int ar = it.rot.x, ai = (short)(-(int)it.rot.y);
  short2 *o = it.dst[ant];
  for (int k = t; k < N; k += THREADS) {
    short2 v = s[k];
    if (it.rotate && (k < len || (k >= start2 && k < start2 + len))) {
      /* identical arithmetic to k_fep_exact (rotate_cpx_vector QRDMX, then mult_cpx_vector with the timeshift table) */
      const int br = v.x, bi = v.y;
      const int re0 = sat16((int)((2LL * ar * br) >> 16)), im0 = sat16((int)((2LL * ar * bi) >> 16));
      const short re = sat16((int)((((long long)re0 << 16) - 2LL * ai * bi + 32768) >> 16));
      const short im = sat16((int)((((long long)im0 << 16) + 2LL * ai * br + 32768) >> 16));
      const short2 w = __ldg(&ts[k]);
      const short nim = (short)(-(int)im);
      v = make_short2(sat16(padd(re * w.x, nim * w.y) >> 15), sat16(padd(im * w.x, re * w.y) >> 15));
    }
    o[k] = v;
  }
}

struct State {
  bool ok = false, part = false;
  int nant = 0;
  uint16_t *d_perm = nullptr;
  short *d_tw1024 = nullptr, *d_tw4096 = nullptr;
  short2 *d_ts = nullptr;
  cudaStream_t st[NCTX] = {};
  Item *h_items[NCTX] = {}, *d_items[NCTX] = {};
  std::atomic<unsigned> busy{0};
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

/* Warm-up (before the RX allocation fence). 0 = ready, -1 = unavailable (callers keep the CPU FEP). */
extern "C" int nr_pusch_gpu_fep_init(int nant, int N, int ncp, int offset_div, int pct)
{
  using namespace ulfep;
  if (U.ok) return U.nant == nant ? 0 : -1;
  if (N != nr_exact::N || nant < 1 || nant > 4 || offset_div <= 0) return -1;
  if (!nr_gpu_module_ready()) return -1;
  const nr_gpu_caps_t *caps = nr_gpu_platform_probe();
  if (!(caps && caps->model == NR_GPU_SHARED && caps->pageable_memory_access && caps->host_page_tables)) {
    fprintf(stderr, "nr_pusch_gpu_fep: no coherent host memory access -- CPU FEP\n");
    return -1;
  }
  U.nant = nant;
  U.pct = pct > 0 && pct <= 100 ? (unsigned)pct : 20u;
  UCK(nr_gpu_allocate(NR_GPU_DEVICE, (void **)&U.d_perm, N * sizeof(uint16_t)));
  UCK(nr_gpu_allocate(NR_GPU_DEVICE, (void **)&U.d_tw1024, 3 * 512 * sizeof(short)));
  UCK(nr_gpu_allocate(NR_GPU_DEVICE, (void **)&U.d_tw4096, 3 * 2048 * sizeof(short)));
  UCK(nr_gpu_allocate(NR_GPU_DEVICE, (void **)&U.d_ts, N * sizeof(short2)));
  std::vector<uint16_t> perm(N);
  std::vector<short> tw1(3 * 512), tw4(3 * 2048), ts(2 * N);
  nr_exact::build_perm(perm.data());
  nr_exact::init_rad4_host(1024, tw1.data());
  nr_exact::init_rad4_host(4096, tw4.data());
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
  UCK(cudaFuncGetAttributes(&attr, ulfep::k_ul_fep));
  int least = 0, greatest = 0;
  UCK(cudaDeviceGetStreamPriorityRange(&least, &greatest));
  /* host buffers the warm-up launch reads and writes: written first, so the GPU never first-touches an unwritten page */
  const size_t row = (size_t)N * sizeof(short2);
  short2 *buf = nullptr;
  if (posix_memalign((void **)&buf, 4096, row * 2)) return -1;
  memset(buf, 0, row * 2);
  for (int c = 0; c < NCTX; c++) {
    if (!nr_gpu_green_stream(&U.st[c], 0, false)) UCK(cudaStreamCreateWithPriority(&U.st[c], cudaStreamNonBlocking, least));
    else U.part = true;
    UCK(nr_gpu_allocate(NR_GPU_STAGING, (void **)&U.h_items[c], MAX_ITEMS * sizeof(Item)));
    memset(U.h_items[c], 0, MAX_ITEMS * sizeof(Item));
    U.d_items[c] = (Item *)nr_gpu_device_ptr(U.h_items[c]);
    if (!U.d_items[c]) return -1;
    Item &it = U.h_items[c][0];
    for (int a = 0; a < 4; a++) { it.src[a] = buf; it.dst[a] = buf + N; }
    it.rotate = 1;
    k_ul_fep<<<dim3(1, nant), nr_exact::THREADS, 0, U.st[c]>>>(U.d_items[c], U.d_perm, U.d_tw1024, U.d_tw4096, U.d_ts, 106, N - 636);
    UCK(cudaPeekAtLastError());
    UCK((cudaError_t)nr_gpu_sync(U.st[c]));
  }
  free(buf);
  U.ok = true;
  fprintf(stderr, "SENSING: UL_GPU_FEP ready: %d RX, N=%d, %s stream, cap %u%% of each 100 ms\n", nant, N,
          U.part ? "discovery-partition" : "lowest-priority", U.pct);
  return 0;
}

/* One batch (<= MAX_ITEMS items x nant antennas). 0 = rows written; -1 all contexts busy; -2 over the duty cap / RT in
 * flight; -3 module off or CUDA error. On non-zero nothing was written and the caller runs the CPU FEP. */
extern "C" int nr_pusch_gpu_fep_run(const void *items, int n, int nb_rb, int fco)
{
  using namespace ulfep;
  if (!U.ok || n < 1 || n > MAX_ITEMS || nb_rb < 1 || nb_rb > 275 || fco < 0 || fco >= nr_exact::N) return -3;
  int c = -1;
  for (int i = 0; i < NCTX && c < 0; i++)
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
  memcpy(U.h_items[c], items, (size_t)n * sizeof(Item));
  k_ul_fep<<<dim3(n, U.nant), nr_exact::THREADS, 0, U.st[c]>>>(U.d_items[c], U.d_perm, U.d_tw1024, U.d_tw4096, U.d_ts, nb_rb, fco);
  cudaError_t e = cudaPeekAtLastError();
  while (e == cudaSuccess) { /* the kernel writes the caller's rows, so a late kernel is waited for, never abandoned */
    e = cudaStreamQuery(U.st[c]);
    if (e == cudaErrorNotReady) {
      e = cudaSuccess;
      if (now_us() - t0 > 60) {
        const struct timespec ts = {0, 25000};
        nanosleep(&ts, NULL);
      }
    } else
      break;
  }
  const uint64_t dt = now_us() - t0;
  U.busy.fetch_and(~(1u << c));
  if (e != cudaSuccess) {
    U.ok = false; /* the caller's CPU FEP takes over for good */
    ++U.n_err;
    fprintf(stderr, "nr_pusch_gpu_fep: %s -- UL GPU FEP off\n", cudaGetErrorString(e));
    return -3;
  }
  ++U.calls;
  U.items += (uint64_t)n;
  U.gpu_us += dt;
  U.win_used_us += dt;
  if (dt > 2000) ++U.n_late;
  uint64_t m = U.max_us.load();
  while (dt > m && !U.max_us.compare_exchange_weak(m, dt)) {}
  return 0;
}

/* calls, items, gpu_us, max_us, busy, over_cap, rt_defer, late(>2 ms), errors, partitioned */
extern "C" void nr_pusch_gpu_fep_stats(uint64_t *o)
{
  using namespace ulfep;
  o[0] = U.calls; o[1] = U.items; o[2] = U.gpu_us; o[3] = U.max_us; o[4] = U.n_busy; o[5] = U.n_budget; o[6] = U.n_rt;
  o[7] = U.n_late; o[8] = U.n_err; o[9] = U.part;
}
