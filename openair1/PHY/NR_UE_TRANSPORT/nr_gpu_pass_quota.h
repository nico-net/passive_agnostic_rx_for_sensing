/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* MPCI G3: per-cell instance quota of the GPU pass pools. Pure C, no CUDA, no nr_cell.c (the dlopen'd GPU module includes it as well as the unit test).
 *
 * The pass instances (DL, UL) are shared by all cells; a cell is REGISTERED once (nr_gpu_pass_cell_init) and from then on the pool keeps a floor of ONE instance for every
 * registered cell that holds none of that pool at the moment ("quiet" cell): a burst cell may take free instances only while the number of free instances is larger than the number
 * of other registered cells that hold none. A cell that holds none takes any free instance (its floor). Nothing is reserved while at most one cell is registered, so a single
 * cell sees exactly the pool of today (N = 1 equivalence), and the rule is work conserving: an idle cell reserves one instance, never more, and a pool with fewer instances than
 * cells simply cannot honour every floor. Sizes come from the caller (the pool size is a parameter, never a constant of this header).
 * The caller serialises all calls on one nr_pass_quota_t (the module holds a mutex while more than one cell is registered). */
#ifndef NR_GPU_PASS_QUOTA_H
#define NR_GPU_PASS_QUOTA_H
#include <string.h>

#define NR_PQ_CELLS 16 /* = NR_CELL_MAX (common/utils/nr_cell.h); the unit test asserts the equality */
#define NR_PQ_CLASSES 2 /* 0 = DL real grants, 1 = UL real grants (probes are not under quota: their pool is separate) */

typedef struct {
  int reg[NR_PQ_CELLS];                    /* 1 = the cell is registered */
  int held[NR_PQ_CLASSES][NR_PQ_CELLS];    /* instances of the class currently held by the cell */
  int nreg;
} nr_pass_quota_t;

static inline void nr_pq_reset(nr_pass_quota_t *q) { memset(q, 0, sizeof(*q)); }
static inline int nr_pq_register(nr_pass_quota_t *q, int cell)
{
  if (cell < 0 || cell >= NR_PQ_CELLS)
    return -1;
  if (!q->reg[cell]) {
    q->reg[cell] = 1;
    q->nreg++;
  }
  return 0;
}
static inline void nr_pq_unregister(nr_pass_quota_t *q, int cell)
{
  if (cell >= 0 && cell < NR_PQ_CELLS && q->reg[cell]) {
    q->reg[cell] = 0;
    q->nreg--;
  }
}
/* the quota is in force only with two or more registered cells */
static inline int nr_pq_active(const nr_pass_quota_t *q) { return q->nreg > 1; }

/* 1 = `cell` may take one of the n_free free instances of class cls now. */
static inline int nr_pq_may_take(const nr_pass_quota_t *q, int cls, int cell, int n_free)
{
  if (n_free <= 0 || cls < 0 || cls >= NR_PQ_CLASSES || cell < 0 || cell >= NR_PQ_CELLS)
    return 0;
  if (!nr_pq_active(q) || !q->reg[cell] || q->held[cls][cell] == 0)
    return 1; /* alone, unregistered (no claim on the floors), or on its own floor */
  int quiet = 0; /* other registered cells holding none: each keeps one free instance */
  for (int c = 0; c < NR_PQ_CELLS; c++)
    if (c != cell && q->reg[c] && q->held[cls][c] == 0)
      quiet++;
  return n_free > quiet;
}
static inline void nr_pq_take(nr_pass_quota_t *q, int cls, int cell)
{
  if (cls >= 0 && cls < NR_PQ_CLASSES && cell >= 0 && cell < NR_PQ_CELLS)
    q->held[cls][cell]++;
}
static inline void nr_pq_release(nr_pass_quota_t *q, int cls, int cell)
{
  if (cls >= 0 && cls < NR_PQ_CLASSES && cell >= 0 && cell < NR_PQ_CELLS && q->held[cls][cell] > 0)
    q->held[cls][cell]--;
}
#endif
