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
/* UL reads the preceding slot tail. Keep five slots before ring overwrite. */
static inline bool nr_pusch_queue_snapshot_fresh(long producer,long source,long slots_per_frame)
{
  return source>=0 && producer>=source && slots_per_frame>5 &&
         producer-source<slots_per_frame-5;
}
#endif
