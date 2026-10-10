/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_DL_GPU_BUDGET_H
#define NR_DL_GPU_BUDGET_H
#include <stdint.h>
#include <time.h>
/* Per module, no catch-up bursts. Four DL modules together admit at most
 * 400 MB/s of charged coherent traffic. Charge CPU reads+writes as well as DMA.
 * A refused item follows the existing CPU fallback. Caller serializes access. */
typedef struct { uint64_t next_ns, bytes, refused; } nr_dl_gpu_budget_t;
static inline int nr_dl_gpu_budget_take_at(nr_dl_gpu_budget_t *b, uint64_t bytes, uint64_t now)
{
  if (now < b->next_ns) { __atomic_add_fetch(&b->refused, 1, __ATOMIC_RELAXED); return 0; }
  b->next_ns = now + bytes * 10; /* 100,000,000 bytes/s */
  __atomic_add_fetch(&b->bytes, bytes, __ATOMIC_RELAXED);
  return 1;
}
static inline int nr_dl_gpu_budget_take(nr_dl_gpu_budget_t *b, uint64_t bytes)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return nr_dl_gpu_budget_take_at(b, bytes, (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec);
}
#endif
