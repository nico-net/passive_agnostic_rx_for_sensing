/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_GPU_WAIT_H
#define NR_GPU_WAIT_H
/* Host wait for a discovery GPU job by polling, never by an interrupt-based wait. MEASURED (tests/passive_rx/gpuspan/launchblock_bench.cu, GB10): while one
 * thread sits in a blocking cudaEventSynchronize / cudaStreamSynchronize on a long kernel (cudaEventBlockingSync, or a device scheduled with
 * cudaDeviceScheduleBlockingSync as nrLDPC_cuda does), the conditional-node body launch of other threads' graphs is held until that wait returns (the
 * on-air nsys: 5 to 9 threads' launches released 0.2-1.1 ms after the discovery kernel ended = the waiter's wake-up). Polling cudaEventQuery /
 * cudaStreamQuery with a short sleep does not hold them. Few polls spin, then 50 us sleeps (the discovery thread owns no real-time core). */
#include <cuda_runtime.h>
#include <time.h>

template<class Q> static inline cudaError_t nr_gpu_wait_poll(Q query)
{
  for (int polls = 0;; ++polls) {
    const cudaError_t r = query();
    if (r != cudaErrorNotReady) return r;
    if (polls >= 8) {
      const struct timespec ts = {0, 50000};
      nanosleep(&ts, nullptr);
    }
  }
}
static inline cudaError_t nr_gpu_wait_event(cudaEvent_t e) { return nr_gpu_wait_poll([&] { return cudaEventQuery(e); }); }
static inline cudaError_t nr_gpu_wait_stream(cudaStream_t s) { return nr_gpu_wait_poll([&] { return cudaStreamQuery(s); }); }
#endif
