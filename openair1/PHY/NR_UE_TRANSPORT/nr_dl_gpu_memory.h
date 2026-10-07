/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* CUDA-module-local allocation fence. Seal only after successful warm-up (or a
 * failed attempt). Counters are exported per module for device regression tests.
 * Driver-internal allocation is also avoided by launching kernels/cuFFT at init. */
#ifndef NR_DL_GPU_MEMORY_H
#define NR_DL_GPU_MEMORY_H
#include <stdint.h>
static bool nr_dl_gpu_sealed, nr_dl_gpu_radio_mode;
extern "C" void nr_dl_gpu_enable_radio_mode(void) { nr_dl_gpu_radio_mode = true; }
static uint64_t nr_dl_gpu_allocs, nr_dl_gpu_denied;
extern "C" void nr_dl_gpu_memory_stats(uint64_t *allocs, uint64_t *denied)
{
  *allocs = __atomic_load_n(&nr_dl_gpu_allocs, __ATOMIC_RELAXED);
  *denied = __atomic_load_n(&nr_dl_gpu_denied, __ATOMIC_RELAXED);
}
#define NR_DL_GPU_ALLOC(call) ((!nr_gpu_allocation_guard() || nr_dl_gpu_sealed) ? (__atomic_add_fetch(&nr_dl_gpu_denied, 1, __ATOMIC_RELAXED), cudaErrorNotReady) : (++nr_dl_gpu_allocs, (call)))
#define nr_gpu_allocate(...) NR_DL_GPU_ALLOC(nr_gpu_allocate(__VA_ARGS__))
#define cudaStreamCreate(...) NR_DL_GPU_ALLOC(cudaStreamCreate(__VA_ARGS__))
#define cudaStreamCreateWithFlags(...) NR_DL_GPU_ALLOC(cudaStreamCreateWithFlags(__VA_ARGS__))
#define cudaStreamCreateWithPriority(...) NR_DL_GPU_ALLOC(cudaStreamCreateWithPriority(__VA_ARGS__))
#define cudaEventCreateWithFlags(...) NR_DL_GPU_ALLOC(cudaEventCreateWithFlags(__VA_ARGS__))
#define cudaGraphInstantiate(...) NR_DL_GPU_ALLOC(cudaGraphInstantiate(__VA_ARGS__))
#endif
