/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_GPU_PLATFORM_H
#define NR_GPU_PLATFORM_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum { NR_GPU_NONE, NR_GPU_SHARED, NR_GPU_DISCRETE } nr_gpu_model_t;
typedef enum { NR_GPU_DEVICE, NR_GPU_STAGING, NR_GPU_SHARED_RW } nr_gpu_kind_t;
typedef enum { NR_GPU_ALLOC_UNAVAILABLE, NR_GPU_ALLOC_DEVICE, NR_GPU_ALLOC_MAPPED, NR_GPU_ALLOC_MANAGED } nr_gpu_storage_t;
typedef struct {
  int available, device, major, minor, integrated;
  int pageable_memory_access, host_page_tables, can_map_host_memory;
  int concurrent_managed_access, host_native_atomic_supported, unified_addressing;
  size_t total_memory, free_memory;
  char name[256];
  nr_gpu_model_t model;
} nr_gpu_caps_t;
/* Pure policy functions also used by the mocked-capability tests. Invalid overrides fail closed. */
nr_gpu_model_t nr_gpu_choose_model(const nr_gpu_caps_t *, const char *override);
nr_gpu_storage_t nr_gpu_storage(nr_gpu_model_t, nr_gpu_kind_t, int managed_opt_in);
int nr_gpu_arch_compatible(int major, int minor, const char *built_archs);
/* One process-wide instance, shared by the core and dlopen modules. No CUDA link dependency.
 * Probe is idempotent; call before module warm-up. The first GPU module may probe earlier.
 * Logger is called with warning=0 for the capability line, 1 for fallback diagnostics. */
void nr_gpu_set_logger(void (*logger)(int warning, const char *message));
const nr_gpu_caps_t *nr_gpu_platform_probe(void);
int nr_gpu_platform_ready(void);
int nr_gpu_module_check(const char *module, const char *built_archs);
/* Irreversible RX fence. Serialize with in-flight allocations; denial returns NULL/nonzero.
 * Free/unregister also denied after sealing: buffers remain resident until process exit. */
void nr_gpu_rx_started(void);
int nr_gpu_allocations_allowed(void);
/* For module-owned streams/events/graphs: logs and counts post-RX denial. */
int nr_gpu_allocation_guard(void);
void nr_gpu_memory_stats(uint64_t *allocations, uint64_t *denied);
/* DEVICE is GPU-only; STAGING/SHARED_RW return host-visible storage. Use device_ptr for
 * kernel arguments, never assume host/device addresses will alias in a future backend.
 * Keep existing explicit transfers between staging and device working sets: coherent
 * memory is not permission to repeatedly stream host buffers from GPU kernels. */
void *nr_gpu_alloc(nr_gpu_kind_t kind, size_t bytes);
void *nr_gpu_device_ptr(void *allocation);
int nr_gpu_free(void *allocation);
int nr_gpu_register_host(void *ptr, size_t bytes);
int nr_gpu_unregister_host(void *ptr);
/* Opaque stream is cudaStream_t in a CUDA module; NULL selects the default stream.
 * Return zero on success, otherwise a CUDA-compatible error code. sync(NULL) waits
 * for the default stream; sync_all waits for the device. Synchronize before CPU reuse. */
int nr_gpu_copy_async(void *dst, const void *src, size_t bytes, void *stream);
int nr_gpu_sync(void *stream);
int nr_gpu_sync_all(void);
#ifdef __cplusplus
}
#endif
#endif
