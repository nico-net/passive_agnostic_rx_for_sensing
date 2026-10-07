/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_GPU_BUDGET_CUDA_H
#define NR_GPU_BUDGET_CUDA_H
#include "nr_gpu_cuda.h"
#include "nr_gpu_budget.h"
#include <cmath>
#include <thread>
#include <chrono>
/* Events must be created in module warm-up, before the T18 RX fence. */
struct NrGpuDiscoverySlice {
  cudaEvent_t first=nullptr,last=nullptr;
  bool init() {
    if(!nr_gpu_budget_enabled())return true;
    if(first && last)return true;
    return nr_gpu_allocation_guard() && cudaEventCreate(&first)==cudaSuccess
      && cudaEventCreate(&last)==cudaSuccess;
  }
  template<class F> cudaError_t run(cudaStream_t stream,F submit) {
    if(!nr_gpu_budget_enabled() || nr_gpu_allocations_allowed())return submit();
    int rc;
    while((rc=nr_gpu_budget_discovery_begin(nr_gpu_budget_now_us()))==0)
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    if(rc<0)return cudaErrorNotReady;
    cudaError_t e=cudaEventRecord(first,stream);
    if(e==cudaSuccess)e=submit();
    if(e==cudaSuccess)e=cudaEventRecord(last,stream);
    if(e==cudaSuccess) {
      while((e=cudaEventQuery(last))==cudaErrorNotReady)
        std::this_thread::sleep_for(std::chrono::microseconds(25));
    }
    float ms=0;
    if(e==cudaSuccess)e=cudaEventElapsedTime(&ms,first,last);
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
