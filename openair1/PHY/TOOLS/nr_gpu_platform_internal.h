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
  int (*memset_device)(void *, int, size_t); /* last: optional for test tables (NULL = device-only buffers are not zeroed) */
} nr_gpu_runtime_t;
#ifdef __cplusplus
extern "C" {
#endif
/* The runtime table once a tier other than CPU is selected, else NULL. */
const nr_gpu_runtime_t *nr_gpu_platform_runtime(void);
/* The SHARED-access proof: a kernel (libcuda driver API, embedded PTX) sums a malloc'd host buffer and writes the checksum back into it, in a private
 * context of `device` (a fault never reaches the primary context; the caller's current context is restored).
 * NR_GPU_PROBE_OK / FAIL (wrong checksum) / TIMEOUT (1 s, no verdict) / FAULT (launch or stream error), NR_GPU_PROBE_SKIPPED when the probe tooling
 * is unavailable (no libcuda symbol, context creation, PTX JIT error). Real GPU only; probe_locked() calls it for a SHARED candidate. */
int nr_gpu_platform_run_probe(int device);
/* One message through the logger installed by nr_gpu_set_logger() (stderr without one): used by nr_gpu_path.c. */
void nr_gpu_platform_log(int warning, const char *text);
#ifdef __cplusplus
}
#endif
#ifdef NR_GPU_PLATFORM_TEST
#ifdef __cplusplus
extern "C" {
#endif
void nr_gpu_test_reset(const nr_gpu_runtime_t *runtime);
/* Replaces the SHARED-access proof (NULL = SKIPPED); cleared by nr_gpu_test_reset(). Also clears the forced tier. */
void nr_gpu_test_set_probe(int (*probe)(void));
#ifdef __cplusplus
}
#endif
#endif
#endif
