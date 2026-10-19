/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#define _GNU_SOURCE
#include "nr_pusch_ul_bgdisc.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>

#define BG_MAX_DEPTH 16
enum { FREE, FILLING, READY };
typedef struct {
  int state;
  nr_ul_bgdisc_job_t j;
  c16_t *buf[4];
  size_t cap; /* samples per antenna */
} bgslot_t;

static bgslot_t g_slot[BG_MAX_DEPTH];
static int g_depth, g_run_active, g_stop;
static nr_ul_bgdisc_run_fn g_run;
static uint64_t g_max_age_ns;
static pthread_t g_thr;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static nr_ul_bgdisc_stats_t g_st;

static uint64_t now_ns(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static void log_stats(void)
{
  fprintf(stderr, "SENSING: UL_BGDISC nohyp=%lu submitted=%lu dup=%lu full=%lu bg_expired=%lu done=%lu\n", (unsigned long)g_st.nohyp,
          (unsigned long)g_st.submitted, (unsigned long)g_st.dup, (unsigned long)g_st.full, (unsigned long)g_st.expired,
          (unsigned long)g_st.done);
}

static void *worker(void *arg)
{
  (void)arg;
  setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10); /* the discovery lane never competes with the grant consumers */
  const char *ce = getenv("ISAC_UL_BGDISC_CORE");
  if (ce && atoi(ce) >= 0) {
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET(atoi(ce), &cs);
    sched_setaffinity(0, sizeof(cs), &cs);
  }
  uint64_t last_log = now_ns(), logged_done = 0;
  pthread_mutex_lock(&g_lock);
  while (!g_stop) {
    int pick = -1;
    for (int i = 0; i < g_depth; i++) /* oldest first */
      if (g_slot[i].state == READY && (pick < 0 || g_slot[i].j.submit_ns < g_slot[pick].j.submit_ns)) pick = i;
    if (pick < 0) {
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_nsec += 20 * 1000 * 1000;
      if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
      pthread_cond_timedwait(&g_cv, &g_lock, &ts);
      const uint64_t t = now_ns();
      if (t - last_log > 5000000000ull && g_st.done + g_st.expired != logged_done) {
        logged_done = g_st.done + g_st.expired;
        last_log = t;
        log_stats();
      }
      continue;
    }
    bgslot_t *s = &g_slot[pick];
    pthread_mutex_unlock(&g_lock);
    const int expired = now_ns() - s->j.submit_ns > g_max_age_ns;
    int rc = 0;
    if (!expired && !g_stop) rc = g_run(&s->j);
    pthread_mutex_lock(&g_lock);
    if (expired || rc) g_st.expired++;
    else g_st.done++;
    s->state = FREE;
  }
  pthread_mutex_unlock(&g_lock);
  return NULL;
}

int nr_ul_bgdisc_start(int depth, nr_ul_bgdisc_run_fn run)
{
  pthread_mutex_lock(&g_lock);
  if (!g_depth) { /* the first call sets the queue up; a later one with a runner only starts the worker */
    g_depth = depth < 1 ? 1 : depth > BG_MAX_DEPTH ? BG_MAX_DEPTH : depth;
    g_max_age_ns = 100 * 1000000ull;
    g_stop = 0;
  }
  const int go = run && !g_run_active;
  pthread_mutex_unlock(&g_lock);
  if (!go) return 0;
  g_run = run;
  if (pthread_create(&g_thr, NULL, worker, NULL)) return -1;
  pthread_setname_np(g_thr, "ulBgDisc");
  g_run_active = 1;
  return 0;
}

void nr_ul_bgdisc_stop(void)
{
  pthread_mutex_lock(&g_lock);
  g_stop = 1;
  pthread_cond_broadcast(&g_cv);
  pthread_mutex_unlock(&g_lock);
  if (g_run_active) pthread_join(g_thr, NULL);
  if (g_run_active) log_stats();
  pthread_mutex_lock(&g_lock);
  for (int i = 0; i < BG_MAX_DEPTH; i++) {
    for (int a = 0; a < 4; a++) free(g_slot[i].buf[a]);
    memset(&g_slot[i], 0, sizeof(g_slot[i]));
  }
  g_depth = g_run_active = 0;
  memset(&g_st, 0, sizeof(g_st));
  pthread_mutex_unlock(&g_lock);
}

int nr_ul_bgdisc_submit(const nr_ul_bgdisc_job_t *j)
{
  pthread_mutex_lock(&g_lock);
  int idx = -1;
  for (int i = 0; i < g_depth; i++) {
    if (g_slot[i].state != FREE && g_slot[i].j.rnti == j->rnti) { g_st.dup++; pthread_mutex_unlock(&g_lock); return 0; }
    if (idx < 0 && g_slot[i].state == FREE) idx = i;
  }
  g_st.nohyp++; /* distinct misses: a duplicate of an RNTI in flight is not one */
  if (idx < 0) { g_st.full++; pthread_mutex_unlock(&g_lock); return 0; }
  bgslot_t *s = &g_slot[idx];
  s->state = FILLING;
  s->j.rnti = j->rnti;
  pthread_mutex_unlock(&g_lock);
  int ok = j->nant >= 1 && j->nant <= 4 && j->length > 0;
  if (ok && s->cap < j->length) { /* first use / larger window: (re)allocate outside the lock, the slot is ours while FILLING */
    for (int a = 0; a < 4; a++) { free(s->buf[a]); s->buf[a] = NULL; }
    s->cap = 0;
    for (int a = 0; ok && a < j->nant; a++) ok = (s->buf[a] = malloc((size_t)j->length * sizeof(c16_t))) != NULL;
    if (ok) s->cap = j->length;
  }
  if (ok) {
    s->j = *j;
    s->j.submit_ns = now_ns(); /* the age clock starts before the copy */
    for (int a = 0; a < j->nant; a++) {
      memcpy(s->buf[a], j->iq[a], (size_t)j->length * sizeof(c16_t));
      s->j.iq[a] = s->buf[a];
    }
  }
  pthread_mutex_lock(&g_lock);
  if (ok) {
    s->state = READY;
    g_st.submitted++;
    pthread_cond_signal(&g_cv);
  } else {
    s->state = FREE;
    g_st.full++;
  }
  pthread_mutex_unlock(&g_lock);
  return ok;
}

void nr_ul_bgdisc_stats(nr_ul_bgdisc_stats_t *s)
{
  pthread_mutex_lock(&g_lock);
  *s = g_st;
  pthread_mutex_unlock(&g_lock);
}

#define PEND_N 16
static struct { uint16_t rnti; uint32_t key; int64_t slot; int used; } g_pend[PEND_N];
static unsigned g_pend_next;

int nr_ul_bgdisc_latch(uint16_t rnti, uint32_t key, int64_t abs_slot, uint32_t cur_key)
{
  if (key == cur_key) return 1; /* cur_key 0 = nothing cached: the first pass pends too */
  pthread_mutex_lock(&g_lock);
  int ret = 0, mine = -1;
  for (int i = 0; i < PEND_N; i++) {
    if (!g_pend[i].used || g_pend[i].key != key) continue;
    if (g_pend[i].rnti == rnti) {
      if (g_pend[i].slot != abs_slot) ret = 1;
      mine = i;
    } else {
      ret = 1; /* another RNTI saw the same hypothesis */
    }
  }
  if (ret) {
    for (int i = 0; i < PEND_N; i++) if (g_pend[i].used && g_pend[i].key == key) g_pend[i].used = 0;
  } else if (mine < 0) {
    const int i = (int)(g_pend_next++ % PEND_N);
    g_pend[i].rnti = rnti; g_pend[i].key = key; g_pend[i].slot = abs_slot; g_pend[i].used = 1;
  }
  pthread_mutex_unlock(&g_lock);
  return ret;
}
