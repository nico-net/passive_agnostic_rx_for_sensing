/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_gpu_mem.h"
#include "nr_gpu_platform_internal.h"
#include <stdlib.h>
#include <string.h>
/* Uses the platform's dlopen'd runtime so libnr_gpu_platform keeps no CUDA link dependency. */
enum { MEMCPY_H2D = 1, MEMCPY_D2H = 2, HOST_MAPPED_PORTABLE = 3 };
nr_gpu_model_t nr_gpu_model(void)
{
  const char *f = getenv("ISAC_GPU_FORCE_DISCRETE");
  if (f && atoi(f)) return NR_GPU_MODEL_DISCRETE;
  return nr_gpu_platform_shared_direct() ? NR_GPU_MODEL_DIRECT : NR_GPU_MODEL_DISCRETE;
}
int nr_gpu_buf_alloc(nr_gpu_buf_t *b, size_t bytes)
{
  const nr_gpu_runtime_t *rt = nr_gpu_platform_runtime();
  memset(b, 0, sizeof(*b));
  if (!rt || !bytes || !nr_gpu_allocations_allowed()) return -1; /* allocation fence */
  b->bytes = bytes;
  b->model = nr_gpu_model();
  if (b->model == NR_GPU_MODEL_DIRECT) {
    if (rt->malloc_host(&b->host, bytes, HOST_MAPPED_PORTABLE)) goto fail;
    memset(b->host, 0, bytes);
    if (rt->host_device_ptr(&b->dev, b->host, 0)) goto fail;
    return 0;
  }
  if (rt->malloc_host(&b->host, bytes, 0)) goto fail;
  memset(b->host, 0, bytes);
  if (rt->malloc_device(&b->dev, bytes)) goto fail;
  /* first touch of the device side: zero it from the zeroed host copy */
  if (rt->copy_async(b->dev, b->host, bytes, MEMCPY_H2D, NULL) || rt->stream_sync(NULL)) goto fail;
  return 0;
fail:
  nr_gpu_buf_free(b);
  return -1;
}
int nr_gpu_buf_h2d_async(nr_gpu_buf_t *b, size_t off, size_t n, void *s)
{
  if (b->model == NR_GPU_MODEL_DIRECT) return 0;
  return nr_gpu_platform_runtime()->copy_async((char *)b->dev + off, (char *)b->host + off, n, MEMCPY_H2D, s) ? -1 : 0;
}
int nr_gpu_buf_d2h_async(nr_gpu_buf_t *b, size_t off, size_t n, void *s)
{
  if (b->model == NR_GPU_MODEL_DIRECT) return 0;
  return nr_gpu_platform_runtime()->copy_async((char *)b->host + off, (char *)b->dev + off, n, MEMCPY_D2H, s) ? -1 : 0;
}
void nr_gpu_buf_free(nr_gpu_buf_t *b)
{
  const nr_gpu_runtime_t *rt = nr_gpu_platform_runtime();
  if (rt && b->model == NR_GPU_MODEL_DISCRETE && b->dev) rt->free_device(b->dev);
  if (rt && b->host) rt->free_host(b->host);
  memset(b, 0, sizeof(*b));
}
