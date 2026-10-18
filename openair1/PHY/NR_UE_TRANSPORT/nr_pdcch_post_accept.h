/*
 * Post-accept work queue of the blind PDCCH Phase 2 (DL grant latency).
 *
 * WHY. Between "this DCI passed the CRC/RNTI gates" and "its PDSCH job is in the queue" the Phase-2 loop used to run, in line, every
 * piece of bookkeeping the accept triggers: the rnti_seen log line (clock read + formatting), the TechA window / cell-state RNTI notes
 * (two leaf mutexes), the stage-0 / search-space-registry / AL1-union / USS-hash notes, the on-accept DM-RS probe. Not one of them feeds the
 * job; all of them only update state that a LATER slot reads. They now go into this small per-iteration queue and run when the iteration
 * ends, i.e. after the job is enqueued, on the same thread and before the next candidate is looked at, on every exit path (the iteration
 * has many `continue`s; the queue is a GCC cleanup variable, so none can skip it).
 *
 * WHAT STAYS BEFORE THE ENQUEUE (inputs the job or its consumer reads): the BC9 DCI history entry (the PDSCH consumer looks up THIS DCI in it
 * as soon as it dequeues the job -- deferring it would race with that read), the RNTI bootstrap / persistence / dedupe gates, the layout
 * resolver observe and pin (they choose the layout), the solver planning, the discovery-token handling.
 *
 * An op's arguments are copied into the arena at defer time (nothing points into the iteration's locals). If the queue is full the op runs
 * at once, i.e. exactly the old behaviour.
 */
#ifndef NR_PDCCH_POST_ACCEPT_H
#define NR_PDCCH_POST_ACCEPT_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define NR_POST_ACCEPT_MAX_OPS 12
#define NR_POST_ACCEPT_ARENA 1024

typedef struct {
  void (*fn)(const void *);
  uint32_t off;
} nr_post_accept_op_t;

typedef struct {
  int n;
  uint32_t used;
  nr_post_accept_op_t op[NR_POST_ACCEPT_MAX_OPS];
  uint8_t arena[NR_POST_ACCEPT_ARENA] __attribute__((aligned(16))); /* left uninitialised on purpose: the queue is created once per candidate task */
} nr_post_accept_t;

static inline void nr_post_accept_init(nr_post_accept_t *p)
{
  p->n = 0;
  p->used = 0;
}

/* Queue fn(args) (args copied, `bytes` long). Runs it at once if the queue cannot hold it. */
static inline void nr_post_accept_defer(nr_post_accept_t *p, void (*fn)(const void *), const void *args, size_t bytes)
{
  const uint32_t need = (uint32_t)((bytes + 15) & ~(size_t)15);
  if (p->n == NR_POST_ACCEPT_MAX_OPS || p->used + need > NR_POST_ACCEPT_ARENA) {
    fn(args);
    return;
  }
  memcpy(p->arena + p->used, args, bytes);
  p->op[p->n].fn = fn;
  p->op[p->n].off = p->used;
  p->n++;
  p->used += need;
}

/* Run the queued ops in the order they were queued, once. */
static inline void nr_post_accept_run(nr_post_accept_t *p)
{
  for (int k = 0; k < p->n; k++)
    p->op[k].fn(p->arena + p->op[k].off);
  p->n = 0;
  p->used = 0;
}

#endif
