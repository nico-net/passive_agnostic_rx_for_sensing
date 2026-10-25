/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_GPU_PLATFORM_H
#define NR_GPU_PLATFORM_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum { NR_GPU_NONE, NR_GPU_SHARED, NR_GPU_DISCRETE } nr_gpu_platform_model_t;
typedef enum { NR_GPU_DEVICE, NR_GPU_STAGING, NR_GPU_SHARED_RW } nr_gpu_kind_t;
typedef enum { NR_GPU_ALLOC_UNAVAILABLE, NR_GPU_ALLOC_DEVICE, NR_GPU_ALLOC_MAPPED } nr_gpu_storage_t;
typedef struct {
  int available, device, major, minor, integrated;
  int pageable_memory_access, host_page_tables, can_map_host_memory;
  int concurrent_managed_access, host_native_atomic_supported, unified_addressing;
  size_t total_memory, free_memory;
  char name[256];
  nr_gpu_platform_model_t model;
  int probe;              /* NR_GPU_PROBE_*: the SHARED-access proof (only run for a SHARED candidate) */
  int forced;             /* 0 automatic, else the forced model + 1 (diagnostic override: NR_GPU_FORCE_MODEL or the test hook) */
  const char *reason;     /* why this tier: integrated|ats|hmm_pcie|no_pageable|probe_fail|probe_fault|no_device|no_uva|no_mapped|forced */
} nr_gpu_caps_t;
enum { NR_GPU_PROBE_SKIPPED = 0, NR_GPU_PROBE_OK = 1, NR_GPU_PROBE_FAIL = 2, NR_GPU_PROBE_TIMEOUT = 3, NR_GPU_PROBE_FAULT = 4 };
/* Pure policy functions also used by the mocked-capability tests.
 * nr_gpu_choose_model is the property-based decision (before the SHARED proof probe):
 *   NONE (CPU tier) without a device, UVA or mapped host memory; SHARED candidate iff pageable_memory_access && (integrated || host_page_tables);
 *   else DISCRETE (HMM over PCIe, integrated without pageable access, ...). nr_gpu_choose_reason names the rule that decided it. */
nr_gpu_platform_model_t nr_gpu_choose_model(const nr_gpu_caps_t *);
const char *nr_gpu_choose_reason(const nr_gpu_caps_t *);
/* NR_GPU_FORCE_MODEL parser: "discrete" -> NR_GPU_DISCRETE, "cpu" -> NR_GPU_NONE, NULL/"" -> -1 (automatic), anything else -> -2 (ignored, warned). */
int nr_gpu_platform_parse_force(const char *value);
nr_gpu_storage_t nr_gpu_storage(nr_gpu_platform_model_t, nr_gpu_kind_t);
int nr_gpu_arch_compatible(int major, int minor, const char *built_archs);
/* One process-wide instance, shared by the core and dlopen modules. No CUDA link dependency.
 * Probe is idempotent; call before module warm-up. The first GPU module may probe earlier.
 * Logger is called with warning=0 for the capability line, 1 for fallback diagnostics. */
void nr_gpu_set_logger(void (*logger)(int warning, const char *message));
const nr_gpu_caps_t *nr_gpu_platform_probe(void);
int nr_gpu_platform_ready(void);
/* 1 iff the tier is SHARED: the device proved direct, fast host access (pageable access through the host page tables or an integrated
 * GPU, confirmed by a one-time probe kernel). DISCRETE and CPU tiers return 0. */
int nr_gpu_platform_shared_direct(void);
/* Free device memory seen at probe time (bytes; 0 without a device). */
size_t nr_gpu_platform_free_at_probe(void);
/* Diagnostic override of the tier, taking effect at (or before) the next probe: NR_GPU_DISCRETE or NR_GPU_NONE (CPU tier); -1 or NR_GPU_SHARED
 * restore automatic selection. Overrides the NR_GPU_FORCE_MODEL environment variable (read once by the probe, same values).
 * A forced tier is logged as FORCED in the GPU_PLATFORM line; SHARED can never be forced. */
void nr_gpu_platform_test_force_model(int model);
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
