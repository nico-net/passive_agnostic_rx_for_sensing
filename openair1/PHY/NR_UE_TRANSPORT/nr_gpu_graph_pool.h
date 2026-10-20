/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* DL GPU data path v2, Task 1 (ISAC_PDSCH_GPU_GRAPH=1): a pool of per-thread host buffer sets of the graph pass, allocated and prefaulted before the RX fence.
 * A thread that reaches the pass after the fence (the async recovery worker, other late threads) claims a set instead of being refused by the nomem gate.
 * A thread keeps its set until it exits (nr_gpu_pass_dl.c returns it from a pthread key destructor). Header-only so that test_nr_gpu_pass exercises the same code
 * as nr_gpu_pass_dl.c. */
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
  int n;                             /* sets made */
  unsigned free_mask;                /* bit i = set i is free */
  uint64_t claimed, empty, released; /* claims served, claims refused (pool empty), sets returned */
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
  __atomic_store_n(&p->free_mask, p->n ? (1u << p->n) - 1u : 0u, __ATOMIC_RELEASE);
  return p->n;
}
/* any thread, any time: one set, or NULL when the pool is empty (counted). Never allocates. */
static inline nr_gpu_graph_set_t *nr_gpu_graph_pool_take(nr_gpu_graph_pool_t *p)
{
  unsigned m = __atomic_load_n(&p->free_mask, __ATOMIC_ACQUIRE);
  while (m)
    if (__atomic_compare_exchange_n(&p->free_mask, &m, m & (m - 1u), 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
      __atomic_fetch_add(&p->claimed, 1, __ATOMIC_RELAXED);
      return &p->set[__builtin_ctz(m)];
    }
  __atomic_fetch_add(&p->empty, 1, __ATOMIC_RELAXED);
  return NULL;
}
/* the claim of nr_gpu_graph_pool_claim (nr_gpu_pass_dl.c): nothing unless v2 is on (no claim, no count) */
static inline nr_gpu_graph_set_t *nr_gpu_graph_pool_claim_set(nr_gpu_graph_pool_t *p, int v2)
{
  return v2 ? nr_gpu_graph_pool_take(p) : NULL;
}
/* a set back to the pool (its thread exits) */
static inline void nr_gpu_graph_pool_put(nr_gpu_graph_pool_t *p, nr_gpu_graph_set_t *s)
{
  const long i = s - p->set;
  if (i < 0 || i >= p->n)
    return;
  __atomic_fetch_or(&p->free_mask, 1u << i, __ATOMIC_ACQ_REL);
  __atomic_fetch_add(&p->released, 1, __ATOMIC_RELAXED);
}
static inline int nr_gpu_graph_pool_avail(const nr_gpu_graph_pool_t *p) { return __atomic_load_n(&p->free_mask, __ATOMIC_ACQUIRE) != 0; }
#endif
