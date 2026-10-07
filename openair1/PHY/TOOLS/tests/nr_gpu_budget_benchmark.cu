/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Device-only contention validation. Never part of automatic ctest. */
#include "nr_gpu_budget_cuda.h"
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
__global__ void load(unsigned long long cycles) {
  auto start=clock64();while(clock64()-start<cycles) {asm volatile("");}
}
int main() {
  setenv("ISAC_GPU_BUDGET","1",1);
  if(!nr_gpu_platform_probe()->available)return 77;
  cudaStream_t rt,disc;int least,greatest,khz=0;
  if(cudaDeviceGetStreamPriorityRange(&least,&greatest)
     || cudaDeviceGetAttribute(&khz,cudaDevAttrClockRate,0)
     || cudaStreamCreateWithPriority(&rt,cudaStreamNonBlocking,greatest)
     || cudaStreamCreateWithPriority(&disc,cudaStreamNonBlocking,least))return 2;
  NrGpuDiscoverySlice slice;if(!slice.init())return 2;
  load<<<1,32,0,rt>>>(1);load<<<32,32,0,disc>>>(1);
  if(cudaDeviceSynchronize())return 2;
  nr_gpu_rx_started();
  std::atomic<bool> stop{false};std::atomic<int> errors{0};
  std::thread background([&] {
    while(!stop.load()) {
      auto rc=slice.run(disc,[&] {
        load<<<32,32,0,disc>>>((unsigned long long)khz/2);return cudaGetLastError();
      });
      if(rc!=cudaSuccess) {++errors;break;}
    }
  });
  using Clock=std::chrono::steady_clock;auto start=Clock::now();
  uint64_t jobs=0,ok=0,wall_max=0;
  for(int slot=0;slot<20000;++slot) {
    std::this_thread::sleep_until(start+std::chrono::microseconds(slot*500));
    auto before=nr_gpu_budget_now_us();
    NrGpuRtScope scope;++jobs;
    if(scope.admitted) {
      load<<<1,32,0,rt>>>((unsigned long long)khz/50);
      if(scope.wait(rt)==cudaSuccess)++ok;
      else break; // quarantine stream; never submit again after timeout
    }
    wall_max=std::max(wall_max,nr_gpu_budget_now_us()-before);
  }
  stop=true;background.join();
  nr_gpu_budget_metrics_t m;nr_gpu_budget_metrics(&m);
  printf("GPU_CONTENTION jobs=%llu ok=%llu budget_pct=%.3f yields=%llu wait_max_us=%llu fallbacks=%llu wall_max_us=%llu overruns=%llu errors=%d\n",
    (unsigned long long)jobs,(unsigned long long)ok,m.gpu_budget_used_pct,(unsigned long long)m.discovery_yields,
    (unsigned long long)m.rt_gpu_wait_max_us,(unsigned long long)m.rt_gpu_fallbacks,(unsigned long long)wall_max,
    (unsigned long long)m.discovery_overruns,errors.load());
  // Scheduler/driver latency and measured slice duration are device gates.
  const char *v=getenv("ISAC_GPU_DISCOVERY_BUDGET_PCT");int pct=v?atoi(v):30;
  if(pct<1 || pct>100)pct=30;
  return errors || m.discovery_overruns || m.gpu_budget_used_pct>pct || !ok
    || wall_max>=500 || jobs!=20000?1:0;
}
