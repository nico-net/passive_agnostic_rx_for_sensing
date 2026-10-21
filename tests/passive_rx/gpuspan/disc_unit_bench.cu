/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* How long does ONE CSI-RS discovery chunk take (cudaEvent time), alone and next to a synthetic load? Bench only, no production change: the
 * production .cu is #included so its anonymous-namespace kernels, streams and green contexts are the real ones.
 * Sites: c_sweep and c_refine (nr_csirs_disc_gpu_sweep.cu, -DBENCH_SWEEP), c_seg (nr_csirs_disc_gpu.cu + cluster.cuh, -DBENCH_SEG).
 * Build: nvcc -O2 -std=c++17 -arch=native -DBENCH_SWEEP|-DBENCH_SEG -I<repo>/openair1/PHY/NR_UE_TRANSPORT -I<repo>/openair1/PHY -I<repo>/openair1 -I<repo>/common/utils -I<repo>
*       -o disc_unit_bench_{sweep,seg} disc_unit_bench.cu [sweep: also .../nr_csirs_disc_sweep.c; seg: -lcufft    Run: disc_unit_bench_sweep [reps] */
#ifdef BENCH_SWEEP
#include "nr_csirs_disc_gpu_sweep.cu"
#else
#include "nr_csirs_disc_gpu.cu"
#endif
#include <chrono>
#include <thread>
#include <vector>
#include <algorithm>
#ifdef BENCH_SWEEP
/* link stubs: nr_csirs_disc_sweep.c needs the MAC mapping helpers, which this bench never calls (it builds its map by hand) */
extern "C" void get_csi_mapping_parms() {}
extern "C" void get_cdm_group_size() {}
#else
int nr_disc_fp_valid(const nr_csirs_disc_ring_t *) { return 1; }
int nr_disc_fp_cluster(const nr_csirs_disc_ring_t *, const nr_csirs_disc_fp_cfg_t *, nr_csirs_disc_fp_t *, int, nr_disc_fold_fn, void *) { return 0; } /* host fallbacks of the footprint module: never called here */
#endif

static bool g_quick = false; /* argv[2] = "quick": no load modes */
#ifdef BENCH_SWEEP
#define MODES (g_quick ? std::vector<int>{0} : std::vector<int>{0, 1, 2, 3, 4})
#else
#define MODES (g_quick ? std::vector<int>{0} : std::vector<int>{0, 1, 2})
#endif
#define LNAME(m) ((m) == 0 ? "none" : (m) == 1 ? "RTgrn" : (m) == 2 ? "plain" : (m) == 3 ? "disc100" : "discLM")
#define CK(x) do { cudaError_t e_ = (x); if (e_ != cudaSuccess) { fprintf(stderr, "%s:%d %s\n", __FILE__, __LINE__, cudaGetErrorString(e_)); exit(2); } } while (0)
/* load: memory-streaming kernel (copy + FMA), about 0.5-1 ms per launch on the GB10 */
__global__ static void load_k(float4 *a, const float4 *b, size_t n)
{
  for (size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x; i < n; i += (size_t)gridDim.x * blockDim.x) {
    float4 v = b[i]; v.x = v.x * 1.0001f + 1; v.y = v.y * 1.0001f + 1; v.z = v.z * 1.0001f + 1; v.w = v.w * 1.0001f + 1; a[i] = v;
  }
}
/* spin: long kernel of 8 CTAs x 32 threads (one wave on the 8 discovery SMs), like a c_seg / c_refine chunk of another discovery thread; lmem = with a 21 KB per-thread local frame as segment() */
template<bool lmem> __global__ static void spin_k(long long ns, int *o)
{
  volatile char frame[lmem ? 21000 : 1]; frame[0] = (char)threadIdx.x;
  unsigned long long t0, t; asm volatile("mov.u64 %0, %%globaltimer;" : "=l"(t0));
  do { asm volatile("mov.u64 %0, %%globaltimer;" : "=l"(t)); } while ((long long)(t - t0) < ns);
  if (o && frame[0] == 99) *o = 1;
}
static cudaStream_t g_ls[5] = {};
static cudaStream_t g_load = nullptr;
static float4 *g_la, *g_lb;
static int g_load_mode = 0; /* 0 none, 1 RT green context (memory stream), 2 plain stream, 3 discovery green ctx: 100 ms spin kernel, 4 same with 21 KB local frame */
static void load_setup(int mode)
{
  g_load_mode = mode;
  if (!mode) return;
  if (!g_ls[mode]) {
    if (mode == 1 || mode == 3 || mode == 4) { if (!nr_gpu_green_stream(&g_ls[mode], 0, mode == 1)) { fprintf(stderr, "no green stream\n"); exit(2); } }
    else CK(cudaStreamCreateWithFlags(&g_ls[mode], cudaStreamNonBlocking));
    if (!g_la) { CK(cudaMalloc(&g_la, 128 << 20)); CK(cudaMalloc(&g_lb, 128 << 20)); CK(cudaMemset(g_lb, 0, 128 << 20)); }
  }
  g_load = g_ls[mode];
}
static void load_start(int ms)
{
  if (g_load_mode == 3) { spin_k<false><<<8, 32, 0, g_load>>>(100000000LL, nullptr); std::this_thread::sleep_for(std::chrono::milliseconds(5)); return; }
  if (g_load_mode == 4) { spin_k<true><<<8, 32, 0, g_load>>>(100000000LL, nullptr); std::this_thread::sleep_for(std::chrono::milliseconds(5)); return; }
  for (int i = 0; i < ms; ++i) load_k<<<512, 256, 0, g_load>>>(g_la, g_lb, (128 << 20) / 16);
}
static void load_end() { if (g_load) CK(cudaStreamSynchronize(g_load)); }
static std::vector<double> med(std::vector<double> v) { std::sort(v.begin(), v.end()); return {v[v.size() / 2], v[(size_t)(v.size() * 0.9)], v.back()}; }
/* time a stream section by events; load (if any) is queued first and kept running; returns ms */
template<class F> static double ev_ms(cudaStream_t s, F f)
{
  cudaEvent_t a, b; CK(cudaEventCreate(&a)); CK(cudaEventCreate(&b));
  CK(cudaEventRecord(a, s)); f(); CK(cudaEventRecord(b, s)); CK(cudaEventSynchronize(b));
  float ms = 0; CK(cudaEventElapsedTime(&ms, a, b)); cudaEventDestroy(a); cudaEventDestroy(b); return ms;
}
template<class F> static void report(const char *site, const char *what, int reps, cudaStream_t s, F f)
{
  std::vector<double> v;
  for (int m : MODES) {
    v.clear(); load_setup(m);
    for (int i = 0; i < reps + 3; ++i) {
      if (m) load_start(60);
      double t = ev_ms(s, f);
      load_end();
      if (i >= 3) v.push_back(t);
    }
    auto r = med(v);
    printf("%-9s %-34s load=%-5s median %9.3f ms  p90 %9.3f  max %9.3f\n", site, what, LNAME(m), r[0], r[1], r[2]);
    fflush(stdout);
  }
}

#ifdef BENCH_SWEEP
int main(int argc, char **argv)
{
  g_quick = argc > 2 && !strcmp(argv[2], "quick");
  const int reps = argc > 1 ? atoi(argv[1]) : 15, RB = 217, ANT = 4, SLOTS = 8, NC = 1024;
  CK(cudaSetDevice(0));
  pick_stream();
  if (prepare(RB, ANT, 32) < 0) { fprintf(stderr, "prepare failed\n"); return 2; }
  auto &w = workspace; cudaStream_t s = w.stream;
  /* ---- sweep: row-1-like map, 1 port, 651 REs, 1 symbol, 4 RX, 8 slots, 1024 IDs ---- */
  std::vector<nr_csirs_disc_re_t> re(651);
  for (int i = 0; i < 651; ++i) re[i] = {1 * 12 * RB + 4 * (i / 3) + i % 3, (uint16_t)i, 1, 1};
  for (int i = 0; i < 651; ++i) re[i].grid_index = (i / 3) * 12 + 4 * (i % 3);
  CK(cudaMemcpy(w.dm.p, re.data(), re.size() * sizeof(re[0]), cudaMemcpyHostToDevice));
  nr_csirs_disc_sweep_map_t map = {}; map.ports = 1; map.n_re = 651; map.words = (6 * RB + 31) / 32; map.sym_mask = 1u << 1; map.begin[1] = 651; map.re = w.dm.p;
  std::vector<short2> x((size_t)SLOTS * ANT * 651); for (size_t i = 0; i < x.size(); ++i) x[i] = {(short)(rand() % 2000 - 1000), (short)(rand() % 2000 - 1000)};
  CK(cudaMemcpy(w.dx.p, x.data(), x.size() * sizeof(short2), cudaMemcpyHostToDevice));
  int sl[SLOTS]; for (int i = 0; i < SLOTS; ++i) sl[i] = i * 2; CK(cudaMemcpy(w.ds.p, sl, sizeof(sl), cudaMemcpyHostToDevice));
  nr_csirs_disc_sweep_cfg_t c = {0, NC, 1, 32}; const int spf = 20;
  auto launch = [&](long f, long n) { sweep<<<dim3(n, SLOTS), 128, 0, s>>>(map, w.dx.p, w.ds.p, ANT, spf, c, w.dz.p, (int)f, NC); };
  printf("== c_sweep: %d IDs, %d slots, %d RX, 651 RE, 128-thread CTAs; stream = discovery green ctx\n", NC, SLOTS, ANT);
  report("c_sweep", "unit x1 (1 id x 8 slots = 8 CTAs)", reps, s, [&] { launch(0, 1); });
  report("c_sweep", "chunk x16 (OTA per_end, 128 CTAs)", reps, s, [&] { launch(0, 16); });
  report("c_sweep", "ONE launch x1024 (main, 8192 CTAs)", reps, s, [&] { launch(0, NC); });
  { /* (b) the whole call chunked as production: static chunker adapts across calls; own load handling */
    for (int m : MODES) {
      load_setup(m); static NrGpuChunker ck{1, 0, "c_sweep"}; std::vector<double> wall, gsum; uint64_t nch = 0; long per_end = 0;
      for (int i = 0; i < reps + 4; ++i) {
        if (m) load_start(1500);
        auto t0 = std::chrono::steady_clock::now(); uint64_t k0 = ck.chunks;
        CK(chunks(ck, NC, launch));
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        load_end();
        if (i >= 4) { wall.push_back(ms); nch = ck.chunks - k0; per_end = ck.per; }
      }
      auto r = med(wall);
      printf("c_sweep   %-34s load=%-5s median %9.3f ms  p90 %9.3f  max %9.3f  (chunks/call %lu, per_end %ld, last_chunk_gpu %lu us)\n", "whole call CHUNKED (F)", LNAME(m), r[0], r[1], r[2], (unsigned long)nch, per_end, (unsigned long)ck.gpu_us);
    }
  }
  /* ---- refine: row 4, 4 ports, 32 valid slots, 4 RX = 512 units, single-thread kernel, ring in mapped host memory (as production) ---- */
  const int NS = 32, PORTS = 4, NRE = 217;
  nr_csirs_disc_ring_t ring = {}; ring.n_ant = ANT; ring.n_rb = RB; ring.slots_per_frame = spf; ring.capacity = NS; ring.slot_stride = (size_t)ANT * 14 * 24 * RB;
  CK(cudaHostAlloc((void **)&ring.grid_base, ring.slot_stride * NS * 2, cudaHostAllocMapped | cudaHostAllocPortable));
  CK(cudaHostAlloc((void **)&ring.meta, NS * sizeof(*ring.meta), cudaHostAllocMapped | cudaHostAllocPortable));
  for (size_t i = 0; i < ring.slot_stride * NS; ++i) ring.grid_base[i] = (int16_t)(rand() % 2000 - 1000);
  for (int i = 0; i < NS; ++i) ring.meta[i] = {(uint32_t)i, 0, (uint8_t)(i % spf), 1};
  nr_csirs_disc_hyp_t h = {}; h.row = 4; h.density = 1; h.period = 1; h.offset = 0; h.rb_start = 0; h.n_rb = RB; h.symb_l0 = 1;
  nr_csirs_disc_win_t win = {}; win.scramb_id = 415;
  std::vector<nr_csirs_disc_re_t> re2(PORTS * NRE);
  for (int p = 0; p < PORTS; ++p) for (int i = 0; i < NRE; ++i) re2[p * NRE + i] = {1 * 12 * RB + 12 * i + p, (uint16_t)i, 1, 1};
  for (auto &r : re2) r.grid_index = r.grid_index % (12 * RB);
  CK(cudaMemcpy(w.dm.p, re2.data(), re2.size() * sizeof(re2[0]), cudaMemcpyHostToDevice));
  nr_csirs_disc_sweep_map_t m2 = {}; m2.ports = PORTS; m2.n_re = PORTS * NRE; m2.words = 14; m2.sym_mask = 1u << 1; m2.re = w.dm.p; for (int p = 0; p <= PORTS; ++p) m2.begin[p] = p * NRE;
  const long TOTAL = (long)NS * ANT * PORTS;
  auto rl = [&](long f, long n) { const bool last = f + n >= TOTAL; refine_kernel<<<1, 1, 0, s>>>(ring, h, win, m2, w.refined.p, w.result.p, w.acc.p, f, last ? LONG_MAX : f + n, last); };
  printf("== c_refine: %ld units (32 slots x 4 RX x 4 ports), refine_kernel<<<1,1>>> (single thread), ring in mapped host memory\n", TOTAL);
  report("c_refine", "unit x1 (first)", reps, s, [&] { rl(0, 1); });
  report("c_refine", "chunk x8 (OTA per_end 8)", reps, s, [&] { rl(0, 8); });
  report("c_refine", "ONE launch x512 (main)", reps, s, [&] { refine_kernel<<<1, 1, 0, s>>>(ring, h, win, m2, w.refined.p, w.result.p, w.acc.p, 0, LONG_MAX, true); });
  for (int m : MODES) {
    load_setup(m); static NrGpuChunker ck{1, 0, "c_refine"}; std::vector<double> wall; uint64_t nch = 0; long per_end = 0;
    for (int i = 0; i < 4 + (reps < 8 ? reps : 8); ++i) {
      if (m) load_start(3000);
      auto t0 = std::chrono::steady_clock::now(); uint64_t k0 = ck.chunks;
      CK(chunks(ck, TOTAL, rl));
      double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      load_end();
      if (i >= 4) { wall.push_back(ms); nch = ck.chunks - k0; per_end = ck.per; }
    }
    auto r = med(wall);
    printf("c_refine  %-34s load=%-5s median %9.3f ms  p90 %9.3f  max %9.3f  (chunks/call %lu, per_end %ld, last_chunk_gpu %lu us)\n", "whole call CHUNKED (F)", LNAME(m), r[0], r[1], r[2], (unsigned long)nch, per_end, (unsigned long)ck.gpu_us);
  }
  return 0;
}
#else
__global__ static void fill64(uint64_t *p, size_t n, uint64_t base, uint64_t spread, uint32_t seed)
{
  for (size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x; i < n; i += (size_t)gridDim.x * blockDim.x) {
    uint32_t h = (uint32_t)i * 2654435761u + seed; h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
    p[i] = base + h % spread;
  }
}
int main(int argc, char **argv)
{
  g_quick = argc > 2 && !strcmp(argv[2], "quick");
  const int reps = argc > 1 ? atoi(argv[1]) : 15, RB = 217, TILES = (RB + 15) / 16, NR = 14 * 12 * RB + 168 * TILES, POOLED = 168 * TILES;
  CK(cudaSetDevice(0));
  if (!discovery_stream.init()) { fprintf(stderr, "stream init failed\n"); return 2; }
  lmem_prereserve();
  cudaStream_t s = disc_stream();
  const int NT = 100;
  uint64_t *sum, *s80, *s64, *tot, *totq; resident::candidate_t *cand; unsigned *count; uint64_t *dc; uint16_t *masks;
  CK(cudaMalloc(&sum, (size_t)640 * NR * 8)); CK(cudaMalloc(&s80, (size_t)80 * NR * 8)); CK(cudaMalloc(&s64, (size_t)64 * NR * 8));
  CK(cudaMalloc(&tot, (size_t)NR * 8)); CK(cudaMalloc(&totq, (size_t)NR * 8));
  CK(cudaMalloc(&cand, sizeof(resident::candidate_t) * resident::GPU_CAND_CAP)); CK(cudaMalloc(&count, 4)); CK(cudaMalloc(&dc, 640 * 8)); CK(cudaMalloc(&masks, (size_t)640 * 14 * TILES * 6 * 2));
  fill64<<<256, 256>>>(sum, (size_t)640 * NR, 5 * 4096, 2048, 1); fill64<<<256, 256>>>(s80, (size_t)80 * NR, 8 * 4096, 4096, 2); fill64<<<256, 256>>>(s64, (size_t)64 * NR, 8 * 4096, 4096, 3);
  fill64<<<256, 256>>>(tot, NR, NT * 4096, 20000, 4); fill64<<<256, 256>>>(totq, NR, (uint64_t)NT * 4096 * 4096 * 2, 1 << 20, 5);
  { std::vector<uint64_t> cnt(640, 5); CK(cudaMemcpy(dc, cnt.data(), 640 * 8, cudaMemcpyHostToDevice)); }
  CK(cudaDeviceSynchronize());
  for (int variant : {0, 1}) {
    if (g_quick && variant) break;
    /* variant 0: one of six component masks set per (period-offset, symbol, tile) = lightest segmentation; 1: all six set = heaviest */
    for (int p : {20, 80, 640}) {
      std::vector<uint16_t> mk((size_t)p * 14 * TILES * 6, 0);
      for (size_t i = 0; i < mk.size(); ++i) if (variant ? 1 : (i % 6 == 2)) mk[i] = 0xfff;
      CK(cudaMemcpy(masks, mk.data(), mk.size() * 2, cudaMemcpyHostToDevice));
      const long TOTAL = (p * 14 + 31) / 32; char name[64];
      auto launch = [&](long f, long k) { resident::segment<<<k, 32, 0, s>>>(RB, p, NT, dc, sum, s80, s64, tot, totq, masks, 0, 2.f, cand, count, (int)f); };
      printf("== c_seg variant=%s p=%d: %ld units (32-thread CTAs, 1 thread per (offset,symbol) carries a ~21 KB local frame)\n", variant ? "all6masks" : "one-mask", p, TOTAL);
      snprintf(name, sizeof name, "unit x1 of %ld", TOTAL);
      report("c_seg", name, reps, s, [&] { CK(cudaMemsetAsync(count, 0, 4, s)); launch(0, 1); });
      snprintf(name, sizeof name, "chunk x%ld (8 SMs x residency)", std::min(32L, TOTAL));
      report("c_seg", name, reps, s, [&] { CK(cudaMemsetAsync(count, 0, 4, s)); launch(0, std::min(32L, TOTAL)); });
      snprintf(name, sizeof name, "ONE launch x%ld (main = whole call)", TOTAL);
      report("c_seg", name, reps, s, [&] { CK(cudaMemsetAsync(count, 0, 4, s)); launch(0, TOTAL); });
      for (int m : MODES) {
        load_setup(m); static NrGpuChunker ck{1, 0, "c_seg"}; std::vector<double> wall; uint64_t nch = 0; long per_end = 0;
        for (int i = 0; i < 4 + (reps < 8 ? reps : 8); ++i) {
          if (m) load_start(1500);
          auto t0 = std::chrono::steady_clock::now(); uint64_t k0 = ck.chunks;
          CK(cudaMemsetAsync(count, 0, 4, s));
          CK(disc_chunks(ck, TOTAL, launch));
          double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
          load_end();
          if (i >= 4) { wall.push_back(ms); nch = ck.chunks - k0; per_end = ck.per; }
        }
        auto r = med(wall);
        printf("c_seg     %-34s load=%-5s median %9.3f ms  p90 %9.3f  max %9.3f  (chunks/call %lu, per_end %ld, last_chunk_gpu %lu us)\n", "whole call CHUNKED (F)", LNAME(m), r[0], r[1], r[2], (unsigned long)nch, per_end, (unsigned long)ck.gpu_us);
      }
    }
  }
  return 0;
}
#endif
