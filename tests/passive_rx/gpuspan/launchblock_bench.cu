/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Does a long discovery kernel (or a CUDA call of the discovery thread) block cudaGraphLaunch of the real-time threads?
 * Real-time side: 4 DL threads (rt green context) + 1 UL thread (UL context), each launching a small graph (4 kernels + a
 * memcpy node) every ~300 us, timing the cudaGraphLaunch call and launch->done.  Discovery side: one thread, one of the
 * modes below, repeating for the whole run.  Usage: launchblock_bench MODE [seconds] [kernel_ms]
 * Build: nvcc -O2 -std=c++17 -I<repo>/openair1/PHY/NR_UE_TRANSPORT -o launchblock_bench launchblock_bench.cu -lpthread */
#include "nr_gpu_green.h"
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
__global__ static void tiny(int *x) { atomicAdd(x, 1); }
static double now_us() { return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

struct Rt {
  cudaStream_t s; cudaGraphExec_t ge; int *d;
  std::vector<float> call, span; std::vector<double> at;
};
static std::atomic<bool> stop{false};
static std::atomic<int> disc_active{0};

static void rt_thread(Rt *r)
{
  CK(cudaSetDevice(0));
  while (!stop) {
    const double t0 = now_us();
    CK(cudaGraphLaunch(r->ge, r->s));
    const double t1 = now_us();
    CK(cudaStreamSynchronize(r->s));
    const double t2 = now_us();
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
  if (plain || !nr_gpu_green_stream(&disc, 0, false)) CK(cudaStreamCreateWithFlags(&disc, cudaStreamNonBlocking));
  (void)lo;
  const int NR = 5;
  Rt rt[NR];
  for (int i = 0; i < NR; i++) {
    if (!nr_gpu_green_stream(&rt[i].s, i == 0 ? -1 : 0, true, i == NR - 1 ? NR_GREEN_UL_IDX : 0)) CK(cudaStreamCreateWithFlags(&rt[i].s, cudaStreamNonBlocking));
    CK(cudaMalloc(&rt[i].d, 4096));
    cudaGraph_t g;
    CK(cudaStreamBeginCapture(rt[i].s, cudaStreamCaptureModeThreadLocal));
    for (int k = 0; k < 4; k++) tiny<<<1, 32, 0, rt[i].s>>>(rt[i].d);
    CK(cudaMemcpyAsync(rt[i].d + 64, rt[i].d, 64, cudaMemcpyDeviceToDevice, rt[i].s));
    CK(cudaStreamEndCapture(rt[i].s, &g));
    CK(cudaGraphInstantiate(&rt[i].ge, g, 0));
    CK(cudaGraphLaunch(rt[i].ge, rt[i].s)); CK(cudaStreamSynchronize(rt[i].s));
  }
  int *dd, *hpin; CK(cudaMalloc(&dd, 1 << 20)); CK(cudaHostAlloc(&hpin, 1 << 20, cudaHostAllocPortable));
  cudaEvent_t evb, evs; CK(cudaEventCreateWithFlags(&evb, cudaEventBlockingSync | cudaEventDisableTiming)); CK(cudaEventCreateWithFlags(&evs, cudaEventDisableTiming));
  std::vector<cudaStream_t> many;
  if (mode.find("many") != std::string::npos) for (int i = 0; i < 64; i++) { cudaStream_t s; if (!nr_gpu_green_stream(&s, 0, false)) CK(cudaStreamCreateWithFlags(&s, cudaStreamNonBlocking)); many.push_back(s); }
  std::vector<std::thread> th;
  for (int i = 0; i < NR; i++) th.emplace_back(rt_thread, &rt[i]);
  std::vector<std::pair<double, double>> kwin; /* [start,end] of every discovery kernel episode, host clock */
  const double tend = now_us() + secs * 1e6;
  const long long kns = (long long)kms * 1000000;
  auto sync_ev = [&](cudaStream_t s) { CK(cudaEventRecord(evb, s)); CK(cudaEventSynchronize(evb)); };
  auto poll_ev = [&](cudaStream_t s) { CK(cudaEventRecord(evs, s)); while (cudaEventQuery(evs) == cudaErrorNotReady) std::this_thread::sleep_for(std::chrono::microseconds(50)); };
  while (mode != "none" && now_us() < tend) {
    const double a = now_us();
    if (mode.rfind("kernel", 0) == 0) { spin<<<1, 1, 0, disc>>>(kns, nullptr); sync_ev(disc); }               /* kernel + blocking event sync (production wait) */
    else if (mode.rfind("poll", 0) == 0) { spin<<<1, 1, 0, disc>>>(kns, nullptr); poll_ev(disc); }               /* kernel, host never enters a wait call */
    else if (mode.rfind("streamsync", 0) == 0) { spin<<<1, 1, 0, disc>>>(kns, nullptr); CK(cudaStreamSynchronize(disc)); }
    else if (mode.rfind("copyq", 0) == 0) { spin<<<1, 1, 0, disc>>>(kns, nullptr); CK(cudaMemcpyAsync(dd, hpin, 4096, cudaMemcpyHostToDevice, disc)); CK(cudaMemcpyAsync(hpin, dd, 4096, cudaMemcpyDeviceToHost, disc)); sync_ev(disc); } /* pinned copies queued behind the kernel */
    else if (mode.rfind("malloc", 0) == 0) { spin<<<1, 1, 0, disc>>>(kns, nullptr); int *p; CK(cudaMalloc(&p, 1 << 16)); CK(cudaFree(p)); sync_ev(disc); } /* alloc/free while the kernel runs */
    else if (mode.rfind("hostalloc", 0) == 0) { spin<<<1, 1, 0, disc>>>(kns, nullptr); void *p; CK(cudaHostAlloc(&p, 1 << 16, cudaHostAllocMapped | cudaHostAllocPortable)); CK(cudaFreeHost(p)); sync_ev(disc); }
    else if (mode.rfind("many", 0) == 0) { for (auto s : many) spin<<<1, 1, 0, s>>>(kns, nullptr); for (auto s : many) CK(cudaStreamSynchronize(s)); }
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
  size_t over = 0; for (float v : call) over += v > 1000; printf("  launch calls > 1 ms: %zu of %zu\n", over, call.size());
  return 0;
}
