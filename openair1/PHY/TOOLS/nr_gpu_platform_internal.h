/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_GPU_PLATFORM_INTERNAL_H
#define NR_GPU_PLATFORM_INTERNAL_H
#include "nr_gpu_platform.h"
/* Stable scalar CUDA runtime ABI only: no cudaDeviceProp layout dependency. */
typedef struct {
  int (*get_count)(int *);
  int (*get_device)(int *);
  int (*attribute)(int *, int, int);
  int (*mem_info)(size_t *, size_t *);
  int (*malloc_device)(void **, size_t);
  int (*malloc_host)(void **, size_t, unsigned);
  int (*malloc_managed)(void **, size_t, unsigned);
  int (*free_device)(void *);
  int (*free_host)(void *);
  int (*host_device_ptr)(void **, void *, unsigned);
  int (*register_host)(void *, size_t, unsigned);
  int (*unregister_host)(void *);
  int (*copy_async)(void *, const void *, size_t, int, void *);
  int (*stream_sync)(void *);
  int (*device_sync)(void);
  int (*set_flags)(unsigned);
  int (*get_last_error)(void);
} nr_gpu_runtime_t;
#ifdef NR_GPU_PLATFORM_TEST
#ifdef __cplusplus
extern "C" {
#endif
void nr_gpu_test_reset(const nr_gpu_runtime_t *runtime);
#ifdef __cplusplus
}
#endif
#endif
#endif
