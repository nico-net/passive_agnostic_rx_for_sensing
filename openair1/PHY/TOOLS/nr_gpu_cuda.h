/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* CUDA-side typed adapter; policy and ownership live in nr_gpu_platform.c. */
#ifndef NR_GPU_CUDA_H
#define NR_GPU_CUDA_H
#include <cuda_runtime.h>
#include "nr_gpu_platform.h"
extern "C" int nr_gpu_module_ready(void);
template<typename T> static inline cudaError_t nr_gpu_allocate(nr_gpu_kind_t kind, T **ptr, size_t bytes)
{
  *ptr = static_cast<T *>(nr_gpu_alloc(kind, bytes));
  return *ptr ? cudaSuccess : cudaErrorMemoryAllocation;
}
/* nr_gpu_copy: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: none found in tree.
 */
static inline cudaError_t nr_gpu_copy(void *dst, const void *src, size_t bytes)
{
  int rc = nr_gpu_copy_async(dst, src, bytes, nullptr);
  return static_cast<cudaError_t>(rc ? rc : nr_gpu_sync(nullptr));
}
/* nr_gpu_copy_2d_async: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: none found in tree.
 */
static inline cudaError_t nr_gpu_copy_2d_async(void *dst, size_t dp, const void *src, size_t sp,
                                             size_t width, size_t height, void *stream)
{
  for (size_t row = 0; row < height; ++row) {
    int rc = nr_gpu_copy_async(static_cast<char *>(dst) + row * dp,
                              static_cast<const char *>(src) + row * sp, width, stream);
    if (rc) return static_cast<cudaError_t>(rc);
  }
  return cudaSuccess;
}
#endif
