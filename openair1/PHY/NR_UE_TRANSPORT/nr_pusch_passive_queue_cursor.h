/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_PUSCH_PASSIVE_QUEUE_CURSOR_H
#define NR_PUSCH_PASSIVE_QUEUE_CURSOR_H
#include <stdbool.h>

/* Caller serializes. A full push evicts the oldest entry and returns its
 * index; -1 means no eviction. Slots themselves remain owned by the queue. */
typedef struct { int depth, head, tail, count; } nr_pusch_queue_cursor_t;
static inline int nr_pusch_queue_push(nr_pusch_queue_cursor_t *q, int *evicted)
{
  *evicted=-1;
  if(q->count==q->depth) {
    *evicted=q->tail;
    q->tail=(q->tail+1)%q->depth;
    q->count--;
  }
  const int slot=q->head;
  q->head=(q->head+1)%q->depth;
  q->count++;
  return slot;
}
static inline int nr_pusch_queue_pop(nr_pusch_queue_cursor_t *q)
{
  if(!q->count) return -1;
  const int slot=q->tail;
  q->tail=(q->tail+1)%q->depth;
  q->count--;
  return slot;
}
/* Shutdown discards pending work. The active consumer finishes only its one
 * already-dequeued job, so a full backlog cannot extend radio teardown. */
static inline int nr_pusch_queue_discard_all(nr_pusch_queue_cursor_t *q)
{
  const int dropped=q->count;
  q->count=0;
  q->tail=q->head;
  return dropped;
}
/* ULQ (ISAC_UL_PRIO_QUEUE): a real grant always goes first; a probe/trial only on spare
 * capacity, i.e. when no real grant waits. 0 = pop real, 1 = pop probe, -1 = nothing. */
static inline int nr_pusch_queue_pick(const nr_pusch_queue_cursor_t *real,const nr_pusch_queue_cursor_t *probe)
{
  return real->count ? 0 : (probe && probe->count) ? 1 : -1;
}
/* ULQ snapshot pool: first unreferenced buffer, -1 when every buffer is held by a job. */
static inline int nr_pusch_snap_free(const int *refs,int n)
{
  for(int i=0;i<n;i++) if(refs[i]==0) return i;
  return -1;
}
/* ULQ CPU budget (ISAC_UL_CPU_BUDGET_PCT): ns a consumer must idle so that its CPU time over
 * the window stays within pct of the wall time. 0 = within budget (or no budget). */
static inline long nr_pusch_budget_idle_ns(long cpu_ns,long wall_ns,int pct)
{
  if(pct<=0 || pct>=100 || cpu_ns<=0) return 0;
  const long need=cpu_ns*100/pct; /* wall time that makes cpu_ns exactly pct % */
  return need>wall_ns ? need-wall_ns : 0;
}
/* UL reads the preceding slot tail. Keep five slots before ring overwrite. */
static inline bool nr_pusch_queue_snapshot_fresh(long producer,long source,long slots_per_frame)
{
  return source>=0 && producer>=source && slots_per_frame>5 &&
         producer-source<slots_per_frame-5;
}
#endif
