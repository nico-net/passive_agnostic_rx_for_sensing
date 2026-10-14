#pragma once
/* FATTAILS: bounded hand-off queue from the RT decode threads to one low-priority background worker (the GPU bit-exact
 * self-checks, ISAC_GPU_CHECK_ASYNC). The producer never blocks: a trylock that fails or a full queue drops the item and
 * counts it, so a grant is never delayed by the queue. Header-only so the unit test (test_nr_pdsch_retry_gate) links it alone. */
#include <pthread.h>
#include <stdint.h>

#define NR_BGQ_MAX 8
typedef void (*nr_bgq_fn)(void *arg); /* runs on the worker; owns (frees) arg */
typedef struct {
  pthread_mutex_t mu;
  pthread_cond_t cv;
  nr_bgq_fn fn[NR_BGQ_MAX];
  void *arg[NR_BGQ_MAX];
  unsigned head, n, run, cap; /* n queued + run (0/1) in progress <= cap <= NR_BGQ_MAX */
  uint64_t queued, dropped, done; /* __atomic builtins (the header is also compiled as C++ by its test) */
} nr_bgq_t;

static inline void nr_bgq_init(nr_bgq_t *q, unsigned cap)
{
  pthread_mutex_init(&q->mu, NULL);
  pthread_cond_init(&q->cv, NULL);
  q->head = q->n = q->run = 0;
  q->cap = cap < 1 ? 1 : cap > NR_BGQ_MAX ? NR_BGQ_MAX : cap;
  q->queued = q->dropped = q->done = 0;
}

/* cheap pre-test before the producer snapshots its inputs (racy by design; nr_bgq_push decides) */
static inline int nr_bgq_room(nr_bgq_t *q)
{
  return __atomic_load_n(&q->n, __ATOMIC_RELAXED) + __atomic_load_n(&q->run, __ATOMIC_RELAXED) < q->cap;
}

/* 0 = queued (the worker owns arg); -1 = dropped (busy lock or full queue; the caller still owns arg) */
static inline int nr_bgq_push(nr_bgq_t *q, nr_bgq_fn fn, void *arg)
{
  if (pthread_mutex_trylock(&q->mu) != 0) {
    __atomic_fetch_add(&q->dropped, 1, __ATOMIC_RELAXED);
    return -1;
  }
  if (q->n + q->run >= q->cap) {
    pthread_mutex_unlock(&q->mu);
    __atomic_fetch_add(&q->dropped, 1, __ATOMIC_RELAXED);
    return -1;
  }
  const unsigned i = (q->head + q->n) % NR_BGQ_MAX;
  q->fn[i] = fn;
  q->arg[i] = arg;
  __atomic_store_n(&q->n, q->n + 1, __ATOMIC_RELAXED);
  __atomic_fetch_add(&q->queued, 1, __ATOMIC_RELAXED);
  pthread_cond_signal(&q->cv);
  pthread_mutex_unlock(&q->mu);
  return 0;
}

/* worker side: blocks until an item is available, runs it outside the lock. Returns after one item. */
static inline void nr_bgq_run_one(nr_bgq_t *q)
{
  pthread_mutex_lock(&q->mu);
  while (q->n == 0)
    pthread_cond_wait(&q->cv, &q->mu);
  nr_bgq_fn fn = q->fn[q->head];
  void *arg = q->arg[q->head];
  q->head = (q->head + 1) % NR_BGQ_MAX;
  __atomic_store_n(&q->n, q->n - 1, __ATOMIC_RELAXED);
  __atomic_store_n(&q->run, 1, __ATOMIC_RELAXED); /* "busy" includes the item in progress */
  pthread_mutex_unlock(&q->mu);
  fn(arg);
  pthread_mutex_lock(&q->mu);
  __atomic_store_n(&q->run, 0, __ATOMIC_RELAXED);
  pthread_mutex_unlock(&q->mu);
  __atomic_fetch_add(&q->done, 1, __ATOMIC_RELAXED);
}
