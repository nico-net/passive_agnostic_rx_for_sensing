/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Small bounded drop-oldest job queue (fixed-size items, copied in). The producer never blocks (a full queue drops the OLDEST job and
 * counts it); one consumer blocks in nr_dq_pop() while empty. Used to move discovery work off the scan consumer onto a DISC-lease thread. */
#ifndef NR_DROP_QUEUE_H
#define NR_DROP_QUEUE_H
#include <pthread.h>
#include <stdint.h>
#include <string.h>

#define NR_DQ_CAP 8
typedef struct {
  pthread_mutex_t mu;
  pthread_cond_t cv;
  int head, n;
  uint64_t drops;
  unsigned char item[NR_DQ_CAP][256]; /* sizeof(job) <= 256 */
} nr_dq_t;
#define NR_DQ_INIT {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, 0, {{0}}}

/* Returns the number of jobs dropped by this push (0 or 1). */
static inline int nr_dq_push(nr_dq_t *q, const void *job, size_t sz)
{
  int dropped = 0;
  pthread_mutex_lock(&q->mu);
  if (q->n == NR_DQ_CAP) {
    q->head = (q->head + 1) % NR_DQ_CAP;
    q->n--;
    q->drops++;
    dropped = 1;
  }
  memcpy(q->item[(q->head + q->n) % NR_DQ_CAP], job, sz);
  q->n++;
  pthread_cond_signal(&q->cv);
  pthread_mutex_unlock(&q->mu);
  return dropped;
}
/* Blocks while empty. */
static inline void nr_dq_pop(nr_dq_t *q, void *job, size_t sz)
{
  pthread_mutex_lock(&q->mu);
  while (q->n == 0)
    pthread_cond_wait(&q->cv, &q->mu);
  memcpy(job, q->item[q->head], sz);
  q->head = (q->head + 1) % NR_DQ_CAP;
  q->n--;
  pthread_mutex_unlock(&q->mu);
}
#endif
