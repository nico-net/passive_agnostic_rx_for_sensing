#define _GNU_SOURCE
#include "nr_pdsch_recovery.h"
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RQ_MAX 4096
#define LATE_BINS 256 /* ms */
typedef struct { nr_recov_key_t k; void *job; uint64_t t_ns; } entry_t;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static entry_t *g_q;
static int g_depth, g_head, g_n;
static bool g_run, g_stop;
static pthread_t g_th;
static int (*g_run_fn)(void *, const nr_recov_key_t *);
static bool (*g_exp_fn)(const nr_recov_key_t *);
static _Atomic uint64_t g_sub, g_resc, g_stale, g_expd, g_full, g_late[LATE_BINS];
static __thread uint64_t t_cur_ns; /* submit time of the job the worker is running */

/* newest (rnti, harq_pid) slot seen: 65536 hashed cells; a collision only ever discards a rescue (never delivers a wrong one) */
static _Atomic uint64_t g_seen[65536]; /* (abs_slot + 1) << 8 | ndi; 0 = none */
static unsigned seen_ix(uint16_t rnti, uint8_t pid) { return ((unsigned)rnti * 16u + (pid & 15u)) & 0xFFFFu; }
void nr_pdsch_recovery_note_newer(uint16_t rnti, uint8_t pid, uint8_t ndi, uint32_t slot)
{
  _Atomic uint64_t *c = &g_seen[seen_ix(rnti, pid)];
  const uint64_t v = ((uint64_t)slot + 1) << 8 | ndi;
  uint64_t o = atomic_load_explicit(c, memory_order_relaxed);
  while ((o >> 8) < ((uint64_t)slot + 1) && !atomic_compare_exchange_weak_explicit(c, &o, v, memory_order_relaxed, memory_order_relaxed)) {}
}

static uint64_t now_ns(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
void nr_pdsch_recovery_set_hooks(int (*run)(void *, const nr_recov_key_t *), bool (*expired)(const nr_recov_key_t *))
{
  g_run_fn = run;
  g_exp_fn = expired;
}
bool nr_pdsch_recovery_enabled(void)
{
  static _Atomic int v = -1;
  if (v < 0) {
    const char *e = getenv("ISAC_PDSCH_RECOVERY_ASYNC");
    v = e && atoi(e) != 0;
  }
  return v;
}
static bool stale(const nr_recov_key_t *k)
{
  return (atomic_load_explicit(&g_seen[seen_ix(k->rnti, k->harq_pid)], memory_order_relaxed) >> 8) > (uint64_t)k->abs_slot + 1;
}
int nr_pdsch_recovery_publish(const nr_recov_key_t *k, const uint8_t *tb, uint32_t tbs)
{
  (void)tb; (void)tbs;
  if (g_exp_fn && g_exp_fn(k)) {
    atomic_fetch_add_explicit(&g_expd, 1, memory_order_relaxed);
    return 2;
  }
  if (stale(k)) {
    atomic_fetch_add_explicit(&g_stale, 1, memory_order_relaxed);
    return 1;
  }
  atomic_fetch_add_explicit(&g_resc, 1, memory_order_relaxed);
  if (t_cur_ns) {
    uint64_t ms = (now_ns() - t_cur_ns) / 1000000ull;
    atomic_fetch_add_explicit(&g_late[ms < LATE_BINS ? ms : LATE_BINS - 1], 1, memory_order_relaxed);
  }
  return 0;
}
void nr_pdsch_recovery_count_stale(void) { atomic_fetch_add_explicit(&g_stale, 1, memory_order_relaxed); }
int nr_pdsch_recovery_arm_order(unsigned mask, int out[31])
{
  int n = 0;
  if (mask & 2) out[n++] = 2;
  for (int a = 1; a < 32; a++)
    if (((unsigned)a & ~mask) == 0 && !(a == 2 && n)) /* a lone 2 was emitted first */
      out[n++] = a;
  return n;
}
static void *worker(void *arg)
{
  (void)arg;
  pthread_mutex_lock(&g_mu);
  for (;;) {
    while (g_n == 0 && !g_stop) pthread_cond_wait(&g_cv, &g_mu);
    if (g_n == 0) break;
    entry_t e = g_q[g_head];
    g_head = (g_head + 1) % g_depth;
    g_n--;
    pthread_mutex_unlock(&g_mu);
    if (g_exp_fn && g_exp_fn(&e.k)) {
      atomic_fetch_add_explicit(&g_expd, 1, memory_order_relaxed);
    } else if (stale(&e.k)) { /* a newer slot of this process is known: do not even run the arms */
      atomic_fetch_add_explicit(&g_stale, 1, memory_order_relaxed);
    } else if (g_run_fn) {
      t_cur_ns = e.t_ns;
      if (g_run_fn(e.job, &e.k) == 2) atomic_fetch_add_explicit(&g_expd, 1, memory_order_relaxed);
      t_cur_ns = 0;
    }
    free(e.job);
    pthread_mutex_lock(&g_mu);
  }
  pthread_mutex_unlock(&g_mu);
  return NULL;
}
int nr_pdsch_recovery_start(int depth, const char *cores)
{
  pthread_mutex_lock(&g_mu);
  if (g_run) {
    pthread_mutex_unlock(&g_mu);
    return 0;
  }
  if (depth <= 0) {
    const char *e = getenv("ISAC_PDSCH_RECOVERY_Q");
    depth = e ? atoi(e) : 32;
  }
  if (depth < 1) depth = 1;
  if (depth > RQ_MAX) depth = RQ_MAX;
  g_q = calloc((size_t)depth, sizeof(entry_t));
  if (!g_q) {
    pthread_mutex_unlock(&g_mu);
    return -1;
  }
  g_depth = depth; g_head = g_n = 0; g_stop = false;
  if (pthread_create(&g_th, NULL, worker, NULL)) {
    free(g_q); g_q = NULL;
    pthread_mutex_unlock(&g_mu);
    return -1;
  }
  g_run = true;
  pthread_mutex_unlock(&g_mu);
  pthread_setname_np(g_th, "pdschRecov");
  if (!cores) cores = getenv("ISAC_PDSCH_RECOVERY_CORES");
  if (!cores) cores = "3,4";
  cpu_set_t set;
  CPU_ZERO(&set);
  for (const char *p = cores; *p;) {
    char *end;
    long c = strtol(p, &end, 10);
    if (end == p) break;
    if (c >= 0 && c < CPU_SETSIZE) CPU_SET((int)c, &set);
    p = *end == ',' ? end + 1 : end;
  }
  if (CPU_COUNT(&set)) pthread_setaffinity_np(g_th, sizeof(set), &set); /* SCHED_OTHER nice 0: the thread keeps the creator's policy only if it is not RT */
  return 0;
}
void nr_pdsch_recovery_stop(void)
{
  pthread_mutex_lock(&g_mu);
  if (!g_run) {
    pthread_mutex_unlock(&g_mu);
    return;
  }
  g_stop = true;
  pthread_cond_broadcast(&g_cv);
  pthread_mutex_unlock(&g_mu);
  pthread_join(g_th, NULL);
  pthread_mutex_lock(&g_mu);
  while (g_n) { free(g_q[g_head].job); g_head = (g_head + 1) % g_depth; g_n--; } /* worker drained already; defensive */
  free(g_q); g_q = NULL; g_run = false;
  pthread_mutex_unlock(&g_mu);
}
int nr_pdsch_recovery_submit(const nr_recov_key_t *k, void *job)
{
  pthread_mutex_lock(&g_mu); /* held for a ring slot copy only, never across the worker's decode */
  if (!g_run || g_n == g_depth) {
    pthread_mutex_unlock(&g_mu);
    atomic_fetch_add_explicit(&g_full, 1, memory_order_relaxed);
    return 1;
  }
  g_q[(g_head + g_n++) % g_depth] = (entry_t){*k, job, now_ns()};
  atomic_fetch_add_explicit(&g_sub, 1, memory_order_relaxed);
  pthread_cond_signal(&g_cv);
  pthread_mutex_unlock(&g_mu);
  return 0;
}
void nr_pdsch_recovery_stats(char *buf, int n)
{
  uint64_t tot = 0, h[LATE_BINS];
  for (int i = 0; i < LATE_BINS; i++) tot += h[i] = atomic_load_explicit(&g_late[i], memory_order_relaxed);
  int p50 = 0, p99 = 0;
  uint64_t c = 0;
  for (int i = 0; i < LATE_BINS && tot; i++) {
    c += h[i];
    if (!p50 && c * 2 >= tot) p50 = i;
    if (!p99 && c * 100 >= tot * 99) { p99 = i; break; }
  }
  snprintf(buf, (size_t)n, "RECOV submitted=%lu rescued=%lu stale=%lu expired=%lu dropped_full=%lu late_ms_p50/p99=%d/%d",
           (unsigned long)atomic_load(&g_sub), (unsigned long)atomic_load(&g_resc), (unsigned long)atomic_load(&g_stale),
           (unsigned long)atomic_load(&g_expd), (unsigned long)atomic_load(&g_full), p50, p99);
}
