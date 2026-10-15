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
/* DIRECT iff NR_GPU_SHARED && pageable access && !ISAC_GPU_FORCE_DISCRETE (the env forces DISCRETE for tests). */
nr_gpu_model_t nr_gpu_model(void);
/* Pre-fence only (before nr_gpu_rx_started); host side pinned and zeroed. Returns 0 or -1. */
int nr_gpu_buf_alloc(nr_gpu_buf_t *b, size_t bytes);
/* No-ops on DIRECT. stream is a cudaStream_t (NULL = default). Return 0 or -1. */
int nr_gpu_buf_h2d_async(nr_gpu_buf_t *b, size_t off, size_t n, void *stream);
int nr_gpu_buf_d2h_async(nr_gpu_buf_t *b, size_t off, size_t n, void *stream);
void nr_gpu_buf_free(nr_gpu_buf_t *b);
#ifdef __cplusplus
}
#endif
#endif
