/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_GPU_MEM_H
#define NR_GPU_MEM_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* One buffer type for both platforms: DIRECT (GB10 shared memory, dev == host) and DISCRETE (PCIe GPU, pinned host + cudaMalloc, explicit copies). */
typedef enum { NR_GPU_MODEL_DIRECT = 0, NR_GPU_MODEL_DISCRETE = 1 } nr_gpu_model_t;
typedef struct { void *dev; void *host; size_t bytes; nr_gpu_model_t model; } nr_gpu_buf_t;
/* DIRECT iff the platform tier is SHARED (proven direct host access) and not forced DISCRETE by a test. */
nr_gpu_model_t nr_gpu_model(void);
void nr_gpu_mem_test_force_discrete(int on); /* test hook: 1 = DISCRETE model on any device (buffers allocated from now on) */
void nr_gpu_mem_test_fail_after(int n); /* test hook (OOM injection): after n further successful nr_gpu_buf_alloc[_dev] calls every later one fails (n = 0: the very next); -1 = off */
/* Pre-fence only (before nr_gpu_rx_started); host side pinned and zeroed. Returns 0 or -1 (also on the CPU tier: no CUDA runtime). */
int nr_gpu_buf_alloc(nr_gpu_buf_t *b, size_t bytes);
/* Device-only variant for intermediates the host never reads or writes: DIRECT = nr_gpu_buf_alloc (mapped, dev == host, unchanged);
 * DISCRETE = zeroed cudaMalloc only, host == NULL (no pinned mirror, so no h2d/d2h: both return -1). Pre-fence only. Returns 0 or -1. */
int nr_gpu_buf_alloc_dev(nr_gpu_buf_t *b, size_t bytes);
/* No-ops on DIRECT. stream is a cudaStream_t (NULL = default). Return 0 or -1. */
int nr_gpu_buf_h2d_async(nr_gpu_buf_t *b, size_t off, size_t n, void *stream);
int nr_gpu_buf_d2h_async(nr_gpu_buf_t *b, size_t off, size_t n, void *stream);
void nr_gpu_buf_free(nr_gpu_buf_t *b);
#ifdef __cplusplus
}
#endif
#endif
