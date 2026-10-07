/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_GPU_BUDGET_CUDA_H
#define NR_GPU_BUDGET_CUDA_H
#include "nr_gpu_cuda.h"
#include "nr_gpu_budget.h"
#include <cmath>
#include <cstdio>
#include <thread>
#include <chrono>
/* Events must be created in module warm-up, before the T18 RX fence. */
struct NrGpuDiscoverySlice {
  cudaEvent_t first=nullptr,last=nullptr;
  bool init() {
    if(!nr_gpu_budget_enabled())return true;
    if(first && last)return true;
    if(!nr_gpu_allocation_guard() || cudaEventCreate(&first)!=cudaSuccess
       || cudaEventCreate(&last)!=cudaSuccess)return false;
    // First use of events must happen before the RX allocation fence.
    return cudaEventRecord(first)==cudaSuccess && cudaEventRecord(last)==cudaSuccess
      && cudaEventSynchronize(last)==cudaSuccess;
  }
  template<class F> cudaError_t run(cudaStream_t stream,F submit,const char *name="transfer") {
    if(!nr_gpu_budget_enabled() || nr_gpu_allocations_allowed())return submit();
    int rc;
    while((rc=nr_gpu_budget_discovery_begin(nr_gpu_budget_now_us()))==0) {
      uint64_t delay=nr_gpu_budget_discovery_delay_us(nr_gpu_budget_now_us());
      // nanosleep(1 us) commonly costs 50-100 us. Never round tiny event
      // charges to the scheduler's sleep granularity on every launch/copy.
      if(delay>100)std::this_thread::sleep_for(std::chrono::microseconds(delay-50));
      else std::this_thread::yield();
    }
    if(rc<0)return cudaErrorNotReady;
    cudaError_t e=cudaEventRecord(first,stream);
    if(e==cudaSuccess)e=submit();
    if(e==cudaSuccess)e=cudaEventRecord(last,stream);
    if(e==cudaSuccess) {
      e=cudaEventSynchronize(last);
    }
    float ms=0;
    if(e==cudaSuccess)e=cudaEventElapsedTime(&ms,first,last);
    if(e!=cudaSuccess || ms>5.f)
      fprintf(stderr,"GPU_DISCOVERY_SLICE operation=%s cuda=%s elapsed_ms=%.6f\n",name,cudaGetErrorString(e),ms);
    if(e!=cudaSuccess)nr_gpu_budget_fault();
    nr_gpu_budget_discovery_end(nr_gpu_budget_now_us(),(uint64_t)ceil(ms*1000.0));
    return e;
  }
};
/* A scope owns RT admission through all copies/kernels and checks one absolute
 * deadline. On failure its module is permanently quarantined: queued DMA uses
 * only module-owned buffers, never CPU fallback output or recycled input. */
struct NrGpuRtScope {
  uint64_t start=nr_gpu_budget_rt_start_us();
  bool admitted=nr_gpu_budget_rt_begin();
  ~NrGpuRtScope() {if(admitted)nr_gpu_budget_rt_end();}
  cudaError_t wait(cudaStream_t stream) {
    if(!nr_gpu_budget_enabled() || nr_gpu_allocations_allowed())return (cudaError_t)nr_gpu_sync(stream);
    return (cudaError_t)nr_gpu_budget_rt_wait([](void *p) {
      return (int)cudaStreamQuery((cudaStream_t)p);
    },stream,start,nullptr,nullptr);
  }
};
#endif
