/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* DL GPU data path v2, Task 1 (ISAC_PDSCH_GPU_GRAPH=1): a pool of per-thread host buffer sets of the graph pass, allocated and prefaulted before the RX fence.
 * A thread that reaches the pass after the fence (the async recovery worker, other late threads) claims a set instead of being refused by the nomem gate.
 * Sets are never returned: a thread keeps its set for life. Header-only so that test_nr_gpu_pass exercises the same code as nr_gpu_pass_dl.c. */
#ifndef NR_GPU_GRAPH_POOL_H
#define NR_GPU_GRAPH_POOL_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define NR_GPU_GRAPH_POOL 8                     /* sets: the late threads of t26gE (recovery worker and helpers) with headroom */
#define NR_GPU_GRAPH_SHADOW_CAP (262144u + 8u)  /* the TB copy of a shadow-checked pass (GP_SHADOW_CAP of the body) */
#define NR_GPU_GRAPH_LLR_CAP 1500000u           /* int16 LLRs of one pass (nr_gpu_pass_dl_prewarm) */

typedef struct {
  void *out, *job; /* nr_gpu_pass_dl_out_t, nr_gpu_pass_job_t */
  int16_t *llr;    /* [NR_GPU_GRAPH_LLR_CAP]: out->llr */
  uint8_t *shadow; /* [NR_GPU_GRAPH_SHADOW_CAP] */
} nr_gpu_graph_set_t;
typedef struct {
  nr_gpu_graph_set_t set[NR_GPU_GRAPH_POOL];
  int n, left;             /* sets made, sets not yet claimed */
  uint64_t claimed, empty; /* claims served, claims refused (pool empty) */
} nr_gpu_graph_pool_t;

static inline void *nr_gpu_graph_zalloc(size_t b)
{
  const size_t r = (b + 63) & ~(size_t)63;
  void *p = aligned_alloc(64, r);
  if (p)
    memset(p, 0, r); /* prefault now, not on the pass path */
  return p;
}
/* pre-fence: up to n sets (<= NR_GPU_GRAPH_POOL); returns the sets made */
static inline int nr_gpu_graph_pool_fill(nr_gpu_graph_pool_t *p, int n, size_t out_bytes, size_t job_bytes)
{
  memset(p, 0, sizeof(*p));
  for (int i = 0; i < n && i < NR_GPU_GRAPH_POOL; i++) {
    nr_gpu_graph_set_t s = {nr_gpu_graph_zalloc(out_bytes), nr_gpu_graph_zalloc(job_bytes), (int16_t *)nr_gpu_graph_zalloc(NR_GPU_GRAPH_LLR_CAP * sizeof(int16_t)),
                            (uint8_t *)nr_gpu_graph_zalloc(NR_GPU_GRAPH_SHADOW_CAP)};
    if (!s.out || !s.job || !s.llr || !s.shadow) {
      free(s.out); free(s.job); free(s.llr); free(s.shadow);
      break;
    }
    p->set[p->n++] = s;
  }
  __atomic_store_n(&p->left, p->n, __ATOMIC_RELEASE);
  return p->n;
}
/* any thread, any time: one set, or NULL when the pool is empty (counted). Never allocates. */
static inline nr_gpu_graph_set_t *nr_gpu_graph_pool_take(nr_gpu_graph_pool_t *p)
{
  int l = __atomic_load_n(&p->left, __ATOMIC_ACQUIRE);
  while (l > 0)
    if (__atomic_compare_exchange_n(&p->left, &l, l - 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
      __atomic_fetch_add(&p->claimed, 1, __ATOMIC_RELAXED);
      return &p->set[l - 1];
    }
  __atomic_fetch_add(&p->empty, 1, __ATOMIC_RELAXED);
  return NULL;
}
#endif
