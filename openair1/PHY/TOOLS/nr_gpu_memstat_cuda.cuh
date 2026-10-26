/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* CUDA-side booking helpers of the GPU memory ledger (nr_gpu_memstat.h): drop-in replacements of the raw allocation calls of the receiver's GPU modules.
 * Each books the bytes under the calling thread's scope (NrMsScope) after the allocation succeeded; the _to variants take an explicit tag.
 * No behaviour change: same arguments, same return value. */
#ifndef NR_GPU_MEMSTAT_CUDA_CUH
#define NR_GPU_MEMSTAT_CUDA_CUH
#include <cuda_runtime.h>
#include "nr_gpu_memstat.h"
template <class T> static inline cudaError_t nr_ms_malloc(T **p, size_t n)
{
  const cudaError_t e = cudaMalloc((void **)p, n);
  if (e == cudaSuccess) nr_gpu_memstat_note(NR_MS_DEV, n);
  return e;
}
template <class T> static inline cudaError_t nr_ms_malloc_async(T **p, size_t n, cudaStream_t s)
{
  const cudaError_t e = cudaMallocAsync((void **)p, n, s);
  if (e == cudaSuccess) nr_gpu_memstat_note(NR_MS_DEV, n);
  return e;
}
template <class T> static inline cudaError_t nr_ms_host_alloc(T **p, size_t n, unsigned flags)
{
  const cudaError_t e = cudaHostAlloc((void **)p, n, flags);
  if (e == cudaSuccess) nr_gpu_memstat_note(NR_MS_PIN, n);
  return e;
}
template <class T> static inline cudaError_t nr_ms_malloc_host(T **p, size_t n)
{
  const cudaError_t e = cudaMallocHost((void **)p, n);
  if (e == cudaSuccess) nr_gpu_memstat_note(NR_MS_PIN, n);
  return e;
}
template <class T> static inline cudaError_t nr_ms_malloc_managed(T **p, size_t n, unsigned flags = cudaMemAttachGlobal)
{
  const cudaError_t e = cudaMallocManaged((void **)p, n, flags);
  if (e == cudaSuccess) nr_gpu_memstat_note(NR_MS_MGD, n);
  return e;
}
/* cudaFree + the matching release (null-safe); bytes = what the allocation booked */
template <class T> static inline void nr_ms_free(T *p, size_t n)
{
  if (!p) return;
  cudaFree((void *)p);
  nr_gpu_memstat_release_tag(nr_gpu_memstat_current(), NR_MS_DEV, n);
}
/* books a free of `n` bytes of the given kind against the calling thread's scope (the module's destroy path) */
static inline void nr_ms_freed(int kind, size_t n) { nr_gpu_memstat_release_tag(nr_gpu_memstat_current(), kind, n); }
#endif
