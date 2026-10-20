/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* DL HARQ soft combining and the GPU pass. The soft store of a failed transport block lives on the device (nr_gpu_pass.h, NR_GPU_HQ_BLK blocks of one code block each) and a retransmission is combined in
 * the pass's rate dematching (k_rdm hq_mode 2), so a real grant with combining on takes the pass. Only the deferred SI soft combining (SIB1 repeats, ISAC_SI_SOFTCOMBINE) keeps its grants on the old path.
 * Host side, no CUDA: the gate and the block pool. The caller (nr_pdsch_passive_decode.c) serialises the pool with the lock of its HARQ store. */
#ifndef NR_GPU_PASS_HQ_H
#define NR_GPU_PASS_HQ_H
#include <stdint.h>
#include "nr_gpu_pass.h"

/* 1 = the grant keeps off the pass because of HARQ: it is armed for soft combining AND is an SI / SIB1 repetition */
static inline int nr_gpu_pass_hq_refuses(int hq_armed, int is_si)
{
  return hq_armed && is_si;
}

/* the device blocks one HARQ entry holds (one per code block of its transport block) and the pool they come from */
typedef struct {
  uint16_t n;
  uint16_t id[NR_GPU_PASS_MAX_CB];
} nr_hq_blocks_t;
typedef struct {
  int ready, nfree;
  uint16_t free_id[NR_GPU_HQ_BLK];
} nr_hq_pool_t;
static inline void nr_hq_pool_init(nr_hq_pool_t *p)
{
  if (p->ready)
    return;
  for (int i = 0; i < NR_GPU_HQ_BLK; i++)
    p->free_id[i] = (uint16_t)(NR_GPU_HQ_BLK - 1 - i);
  p->nfree = NR_GPU_HQ_BLK;
  p->ready = 1;
}
/* b gives all its blocks back */
static inline void nr_hq_pool_put(nr_hq_pool_t *p, nr_hq_blocks_t *b)
{
  nr_hq_pool_init(p);
  while (b->n > 0)
    p->free_id[p->nfree++] = b->id[--b->n];
}
/* another holder whose sum may be dropped (the caller marks it invalid), never `except`; NULL = none */
typedef nr_hq_blocks_t *(*nr_hq_victim_fn)(void *ctx, const nr_hq_blocks_t *except);
/* b holds >= C blocks afterwards (0), taking from the pool and, when it is short, the blocks of the victims one by one (*evicted counts them); -1 = not enough even then (b is left empty).
 * An entry that already holds C or more keeps them (a live sum is never moved). */
static inline int nr_hq_pool_ensure(nr_hq_pool_t *p, nr_hq_blocks_t *b, int C, nr_hq_victim_fn victim, void *ctx, int *evicted)
{
  nr_hq_pool_init(p);
  if (C < 1 || C > NR_GPU_PASS_MAX_CB)
    return -1;
  if (b->n >= C)
    return 0;
  nr_hq_pool_put(p, b);
  while (p->nfree < C) {
    nr_hq_blocks_t *v = victim ? victim(ctx, b) : 0;
    if (!v)
      return -1;
    nr_hq_pool_put(p, v);
    if (evicted)
      ++*evicted;
  }
  for (int i = 0; i < C; i++)
    b->id[i] = p->free_id[--p->nfree];
  b->n = (uint16_t)C;
  return 0;
}
#endif
