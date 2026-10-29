/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Does a long discovery kernel (or a CUDA call of the discovery thread) block cudaGraphLaunch of the real-time threads?
 * Real-time side: 4 DL threads (rt green context) + 1 UL thread (UL context), each launching a small graph (4 kernels + a
 * memcpy node) every ~300 us, timing the cudaGraphLaunch call and launch->done.  Discovery side: one thread, one of the
 * modes below, repeating for the whole run.  Usage: launchblock_bench MODE [seconds] [kernel_ms]
 * Build: nvcc -O2 -std=c++17 -I<repo>/openair1/PHY/NR_UE_TRANSPORT -o launchblock_bench launchblock_bench.cu -lpthread */
#include "nr_gpu_green.h"
#include "../TOOLS/nr_gpu_chunk.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#define CK(x) do { cudaError_t e_ = (x); if (e_ != cudaSuccess) { fprintf(stderr, "%s:%d %s\n", __FILE__, __LINE__, cudaGetErrorString(e_)); exit(2); } } while (0)
__global__ static void spin(long long ns, int *flag)
{
  unsigned long long t0, t;
  asm volatile("mov.u64 %0, %%globaltimer;" : "=l"(t0));
  do { asm volatile("mov.u64 %0, %%globaltimer;" : "=l"(t)); } while ((long long)(t - t0) < ns);
  if (flag) *flag = 1;
}
/* refine_kernel look-alikes: one thread, long. hostread = zero-copy reads of mapped pinned memory (the ring grid is cudaHostAllocMapped); lmem = a large per-thread local frame. */
__global__ static void spin_host(long long ns, const volatile short *g, int *flag)
{
  unsigned long long t0, t; int acc = 0; size_t i = 0;
  asm volatile("mov.u64 %0, %%globaltimer;" : "=l"(t0));
  do { acc += g[(i * 4099) & 0xfffff]; ++i; asm volatile("mov.u64 %0, %%globaltimer;" : "=l"(t)); } while ((long long)(t - t0) < ns);
  if (flag) *flag = acc;
}
__global__ static void spin_lmem(long long ns, int *flag)
{
  volatile double frame[3000]; /* 24 KB local */
  unsigned long long t0, t; unsigned n = 0;
  asm volatile("mov.u64 %0, %%globaltimer;" : "=l"(t0));
  do { frame[n % 3000] = n; n += (unsigned)frame[(n * 7) % 3000] % 5 + 1; asm volatile("mov.u64 %0, %%globaltimer;" : "=l"(t)); } while ((long long)(t - t0) < ns);
  if (flag) *flag = (int)n;
}
__global__ static void tiny(int *x) { atomicAdd(x, 1); }
__global__ static void gate(int *x, cudaGraphConditionalHandle h) { cudaGraphSetConditional(h, (*x & 1) == 0 ? 1 : 0); }
/* Production-like graph: tiny -> gate -> IF{ 3 tiny kernels } -> tiny, the IF node built as nr_gpu_pass_graph.cu capture_gated() does. */
static bool build_cond(cudaStream_t s, int *d, int *hp, bool ce, cudaGraphExec_t *ge)
{
  void *f[4] = {}; const char *nm[4] = {"cuStreamGetCtx", "cuCtxFromGreenCtx", "cuGraphConditionalHandleCreate", "cuGraphAddNode"};
  const int ver[4] = {12050, 12050, 12030, 13000}; cudaDriverEntryPointQueryResult q;
  for (int i = 0; i < 4; i++) if (cudaGetDriverEntryPointByVersion(nm[i], &f[i], ver[i], cudaEnableDefault, &q) != cudaSuccess || q != cudaDriverEntryPointSuccess) return false;
  auto stream_ctx = (CUresult(*)(CUstream, CUcontext *, CUgreenCtx *))f[0]; auto from_green = (CUresult(*)(CUcontext *, CUgreenCtx))f[1];
  auto hcreate = (CUresult(*)(CUgraphConditionalHandle *, CUgraph, CUcontext, unsigned, unsigned))f[2];
  auto addnode = (CUresult(*)(CUgraphNode *, CUgraph, const CUgraphNode *, const CUgraphEdgeData *, size_t, CUgraphNodeParams *))f[3];
  cudaGraph_t body, w;
  CK(cudaStreamBeginCapture(s, cudaStreamCaptureModeRelaxed)); for (int k = 0; k < 3; k++) tiny<<<1, 32, 0, s>>>(d); CK(cudaStreamEndCapture(s, &body));
  CK(cudaStreamBeginCapture(s, cudaStreamCaptureModeRelaxed));
  cudaStreamCaptureStatus st; unsigned long long id; cudaGraph_t cg; const cudaGraphNode_t *deps; const cudaGraphEdgeData *ed; size_t nd; CUcontext cx = nullptr; CUgreenCtx gc = nullptr;
  CK(cudaStreamGetCaptureInfo(s, &st, &id, &cg, &deps, &ed, &nd));
  if (stream_ctx((CUstream)s, &cx, &gc) != CUDA_SUCCESS) return false;
  if (gc && from_green(&cx, gc) != CUDA_SUCCESS) return false;
  CUgraphConditionalHandle h; if (hcreate(&h, (CUgraph)cg, cx, 1, CU_GRAPH_COND_ASSIGN_DEFAULT) != CUDA_SUCCESS) return false;
  if (ce) CK(cudaMemcpyAsync(d + 128, hp, 256, cudaMemcpyHostToDevice, s));
  tiny<<<1, 32, 0, s>>>(d); gate<<<1, 1, 0, s>>>(d, (cudaGraphConditionalHandle)h);
  CK(cudaStreamGetCaptureInfo(s, &st, &id, &cg, &deps, &ed, &nd));
  CUgraphNodeParams np = {}; np.type = CU_GRAPH_NODE_TYPE_CONDITIONAL; np.conditional.handle = h; np.conditional.type = CU_GRAPH_COND_TYPE_IF; np.conditional.size = 1; np.conditional.ctx = cx;
  CUgraphNode cn; if (addnode(&cn, (CUgraph)cg, (const CUgraphNode *)deps, (const CUgraphEdgeData *)ed, nd, &np) != CUDA_SUCCESS) return false;
  cudaGraphNode_t cb; CK(cudaGraphAddChildGraphNode(&cb, (cudaGraph_t)np.conditional.phGraph_out[0], nullptr, 0, body));
  cudaGraphNode_t cn2 = (cudaGraphNode_t)cn; CK(cudaStreamUpdateCaptureDependencies(s, &cn2, nullptr, 1, cudaStreamSetCaptureDependencies));
  tiny<<<1, 32, 0, s>>>(d);
  if (ce) CK(cudaMemcpyAsync(hp + 64, d, 256, cudaMemcpyDeviceToHost, s));
  CK(cudaStreamEndCapture(s, &w));
  CK(cudaGraphInstantiate(ge, w, 0)); return true;
}
static double now_us() { return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

struct Rt {
  cudaStream_t s; cudaGraphExec_t ge; int *d; int *hp; int chain = 1;
  std::vector<float> call, span; std::vector<double> at;
};
extern "C" int nr_gpu_budget_rt_begin(void) __attribute__((weak)); /* link openair1/PHY/TOOLS/nr_gpu_budget.c and run with ISAC_GPU_BUDGET=1 to turn the chunk admission on */
extern "C" void nr_gpu_budget_rt_end(void) __attribute__((weak));
static std::atomic<bool> stop{false};
static std::atomic<int> disc_active{0};

static void rt_thread(Rt *r)
{
  CK(cudaSetDevice(0));
  while (!stop) {
    const double t0 = now_us();
    if (nr_gpu_budget_rt_begin) nr_gpu_budget_rt_begin(); /* a real DL grant in flight, as NrGpuRtScope does around a pass */
    CK(cudaGraphLaunch(r->ge, r->s));
    for (int k = 1; k < r->chain; k++) CK(cudaGraphLaunch(r->ge, r->s)); /* "chain": C, D, L launched back to back on one stream, no host wait between */
    const double t1 = now_us();
    CK(cudaStreamSynchronize(r->s));
    const double t2 = now_us();
    if (nr_gpu_budget_rt_end) nr_gpu_budget_rt_end();
    r->at.push_back(t0); r->call.push_back((float)(t1 - t0)); r->span.push_back((float)(t2 - t0));
    std::this_thread::sleep_for(std::chrono::microseconds(300));
  }
}
static void pct(const char *n, std::vector<float> v)
{
  if (v.empty()) { printf("  %-10s empty\n", n); return; }
  std::sort(v.begin(), v.end());
  auto p = [&](double q) { return v[std::min(v.size() - 1, (size_t)(q * v.size()))]; };
  printf("  %-10s n=%zu p50=%.0f p99=%.0f p99.9=%.0f max=%.0f us\n", n, v.size(), p(.5), p(.99), p(.999), v.back());
}

int main(int argc, char **argv)
{
  const std::string mode = argc > 1 ? argv[1] : "none";
  const double secs = argc > 2 ? atof(argv[2]) : 6;
  const int kms = argc > 3 ? atoi(argv[3]) : 100;
  CK(cudaSetDevice(0));
  CK(cudaFree(0));
  cudaStream_t disc = nullptr, lo = nullptr;
  const bool plain = mode.find("plainctx") != std::string::npos; /* discovery stream in the primary context, not a green one */
  int least = 0, greatest = 0; cudaDeviceGetStreamPriorityRange(&least, &greatest);
  const bool prio = mode.find("prio") != std::string::npos; /* RT streams at the greatest priority, discovery at the least */
  if (plain || !nr_gpu_green_stream(&disc, 0, false)) CK(cudaStreamCreateWithFlags(&disc, cudaStreamNonBlocking));
  (void)lo;
  const int NR = 5;
  Rt rt[NR];
  for (int i = 0; i < NR; i++) {
    if (!nr_gpu_green_stream(&rt[i].s, prio ? greatest : (i == 0 ? -1 : 0), true, i == NR - 1 ? NR_GREEN_UL_IDX : 0)) CK(cudaStreamCreateWithFlags(&rt[i].s, cudaStreamNonBlocking));
    CK(cudaMalloc(&rt[i].d, 4096)); CK(cudaHostAlloc(&rt[i].hp, 4096, cudaHostAllocPortable));
    if (mode.find("cond") != std::string::npos) { fprintf(stderr, "build %d\n", i); if (!build_cond(rt[i].s, rt[i].d, rt[i].hp, mode.find("ce") != std::string::npos, &rt[i].ge)) { fprintf(stderr, "cond graph failed\n"); return 2; } fprintf(stderr, "built %d\n", i); CK(cudaGraphLaunch(rt[i].ge, rt[i].s)); CK(cudaStreamSynchronize(rt[i].s)); fprintf(stderr, "ran %d\n", i); continue; }
    cudaGraph_t g;
    CK(cudaStreamBeginCapture(rt[i].s, cudaStreamCaptureModeThreadLocal));
    for (int k = 0; k < 4; k++) tiny<<<1, 32, 0, rt[i].s>>>(rt[i].d);
    CK(cudaMemcpyAsync(rt[i].d + 64, rt[i].d, 64, cudaMemcpyDeviceToDevice, rt[i].s));
    if (mode.find("ce") != std::string::npos) { /* production graphs carry host<->device memcpy nodes (parameter blocks, results): copy engine */
      CK(cudaMemcpyAsync(rt[i].d + 128, rt[i].hp, 256, cudaMemcpyHostToDevice, rt[i].s));
      tiny<<<1, 32, 0, rt[i].s>>>(rt[i].d);
      CK(cudaMemcpyAsync(rt[i].hp + 64, rt[i].d, 256, cudaMemcpyDeviceToHost, rt[i].s)); }
    CK(cudaStreamEndCapture(rt[i].s, &g));
    CK(cudaGraphInstantiate(&rt[i].ge, g, 0));
    CK(cudaGraphLaunch(rt[i].ge, rt[i].s)); CK(cudaStreamSynchronize(rt[i].s));
  }
  int *dd, *hpin; CK(cudaMalloc(&dd, 1 << 20)); CK(cudaHostAlloc(&hpin, 1 << 20, cudaHostAllocPortable));
  void *hmap; CK(cudaHostAlloc(&hmap, 8 << 20, cudaHostAllocMapped | cudaHostAllocPortable)); memset(hmap, 1, 8 << 20);
  if (mode.find("stack") != std::string::npos) CK(cudaDeviceSetLimit(cudaLimitStackSize, 24 * 1024));
  cudaEvent_t evb, evs; CK(cudaEventCreateWithFlags(&evb, cudaEventBlockingSync | cudaEventDisableTiming)); CK(cudaEventCreateWithFlags(&evs, cudaEventDisableTiming));
  std::vector<cudaStream_t> many;
  if (mode.find("many") != std::string::npos) for (int i = 0; i < 64; i++) { cudaStream_t s; if (!nr_gpu_green_stream(&s, 0, false)) CK(cudaStreamCreateWithFlags(&s, cudaStreamNonBlocking)); many.push_back(s); }
  for (int i = 0; i < NR; i++) rt[i].chain = mode.find("chain") != std::string::npos ? 3 : 1;
  std::vector<std::thread> th;
  for (int i = 0; i < NR; i++) th.emplace_back(rt_thread, &rt[i]);
  std::vector<std::pair<double, double>> kwin; /* [start,end] of every discovery kernel episode, host clock */
  const double tend = now_us() + secs * 1e6;
  const long long kns = (long long)kms * 1000000;
  const bool usepoll = mode.find("wpoll") != std::string::npos; /* wait by cudaEventQuery + 50 us sleeps instead of a blocking cudaEventSynchronize */
  auto poll_ev = [&](cudaStream_t s) { CK(cudaEventRecord(evs, s)); while (cudaEventQuery(evs) == cudaErrorNotReady) std::this_thread::sleep_for(std::chrono::microseconds(50)); };
  const bool usespin = mode.find("wspin") != std::string::npos; /* cudaEventSynchronize on a default-flag event (device schedule auto: spin/yield, not interrupt) */
  cudaEvent_t evd; CK(cudaEventCreateWithFlags(&evd, cudaEventDisableTiming));
  auto sync_ev = [&](cudaStream_t s) { if (usepoll) { poll_ev(s); return; } if (usespin) { CK(cudaEventRecord(evd, s)); CK(cudaEventSynchronize(evd)); return; } CK(cudaEventRecord(evb, s)); CK(cudaEventSynchronize(evb)); };
  while (mode != "none" && now_us() < tend) {
    const double a = now_us();
    if (mode.rfind("hostread", 0) == 0) { spin_host<<<1, 1, 0, disc>>>(kns, (const volatile short *)hmap, nullptr); sync_ev(disc); }
    else if (mode.rfind("lmem", 0) == 0) { spin_lmem<<<1, 1, 0, disc>>>(kns, nullptr); sync_ev(disc); }
    else if (mode.rfind("kernel", 0) == 0) { if (mode.find("wide") != std::string::npos) spin<<<64, 128, 0, disc>>>(kns, nullptr); else spin<<<1, 1, 0, disc>>>(kns, nullptr); sync_ev(disc); }               /* kernel + blocking event sync (production wait) */
    else if (mode.rfind("poll", 0) == 0) { spin<<<1, 1, 0, disc>>>(kns, nullptr); poll_ev(disc); }               /* kernel, host never enters a wait call */
    else if (mode.rfind("streamsync", 0) == 0) { spin<<<1, 1, 0, disc>>>(kns, nullptr); CK(cudaStreamSynchronize(disc)); }
    else if (mode.rfind("copyq", 0) == 0) { spin<<<1, 1, 0, disc>>>(kns, nullptr); CK(cudaMemcpyAsync(dd, hpin, 4096, cudaMemcpyHostToDevice, disc)); CK(cudaMemcpyAsync(hpin, dd, 4096, cudaMemcpyDeviceToHost, disc)); sync_ev(disc); } /* pinned copies queued behind the kernel */
    else if (mode.rfind("malloc", 0) == 0) { spin<<<1, 1, 0, disc>>>(kns, nullptr); int *p; CK(cudaMalloc(&p, 1 << 16)); CK(cudaFree(p)); sync_ev(disc); } /* alloc/free while the kernel runs */
    else if (mode.rfind("hostalloc", 0) == 0) { spin<<<1, 1, 0, disc>>>(kns, nullptr); void *p; CK(cudaHostAlloc(&p, 1 << 16, cudaHostAllocMapped | cudaHostAllocPortable)); CK(cudaFreeHost(p)); sync_ev(disc); }
    else if (mode.rfind("many", 0) == 0) { for (auto s : many) spin<<<1, 1, 0, s>>>(kns, nullptr); for (auto s : many) CK(cudaStreamSynchronize(s)); }
    else if (mode.rfind("adapt", 0) == 0) { /* AFTER: the production chunker, kms ms of work as kms*10 units of 100 us, stream drained between chunks */
      static NrGpuChunker ck;
      nr_gpu_chunked(ck, (long)kms * 10, [&](long, long n) { spin<<<1, 1, 0, disc>>>(n * 100000LL, nullptr); return (int)cudaGetLastError(); },
                     [&]() { sync_ev(disc); return 0; }, []() { return (uint64_t)0; }); } /* no GPU span: the host time of the chunk is its run time */
    else if (mode.rfind("chunkq", 0) == 0) { for (int i = 0; i < kms; i++) spin<<<1, 1, 0, disc>>>(1000000, nullptr); sync_ev(disc); }
    else if (mode.rfind("chunk", 0) == 0) { const int n = kms; for (int i = 0; i < n; i++) { spin<<<1, 1, 0, disc>>>(1000000, nullptr); sync_ev(disc); } } /* same total time in 1 ms kernels, wait after each */
    kwin.push_back({a, now_us()});
  }
  if (mode == "none") std::this_thread::sleep_for(std::chrono::duration<double>(secs));
  stop = true;
  for (auto &t : th) t.join();
  printf("mode=%s kernel_ms=%d episodes=%zu\n", mode.c_str(), kms, kwin.size());
  std::vector<float> call, span, callin, callout;
  for (int i = 0; i < NR; i++) for (size_t j = 0; j < rt[i].call.size(); j++) {
    call.push_back(rt[i].call[j]); span.push_back(rt[i].span[j]);
    bool in = false; for (auto &w : kwin) if (rt[i].at[j] >= w.first && rt[i].at[j] <= w.second) { in = true; break; }
    (in ? callin : callout).push_back(rt[i].call[j]);
  }
  pct("launch", call); pct("launch_in", callin); pct("launch_out", callout); pct("launch>done", span);
  for (int i = 0; i < NR; i++) for (size_t j = 0; j < rt[i].span.size(); j++) if (rt[i].span[j] > 2000) {
    double off = -1; for (auto &w : kwin) if (rt[i].at[j] >= w.first && rt[i].at[j] <= w.second) off = (rt[i].at[j] - w.first) / 1000;
    printf("  STALL rt%d at +%.1f ms into episode: call %.0f us, launch->done %.0f us\n", i, off, rt[i].call[j], rt[i].span[j]); }
  size_t over = 0; for (float v : call) over += v > 1000; printf("  launch calls > 1 ms: %zu of %zu\n", over, call.size());
  size_t sover = 0; for (float v : span) sover += v > 1000; printf("  launch->done > 1 ms: %zu of %zu\n", sover, span.size());
  return 0;
}
