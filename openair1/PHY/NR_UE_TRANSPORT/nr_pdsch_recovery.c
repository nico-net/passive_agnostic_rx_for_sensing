#define _GNU_SOURCE
#include "nr_pdsch_recovery.h"
#include "common/utils/nr_cell.h" /* mpci A5: per-cell state */
#include "nr_pdsch_cell_prealloc.h"
#include "common/utils/nr_core_plan.h"
#include "common/utils/nr_core_layout.h"
#pragma weak nr_core_layout /* unit tests link this file without the utils library: no plan */
#include "common/utils/nr_core_pool.h"
#include "nr_recov_yield.h"
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/syscall.h>

#define RQ_MAX 4096
#define LATE_BINS 256 /* ms */
typedef struct { nr_recov_key_t k; void *job; uint64_t t_ns; } entry_t;
/* Priority-inheritance mutex: the SCHED_FIFO consumers and the SCHED_OTHER worker share it, so a preempted worker must not hold a consumer up.
 * Critical sections are a few array operations (never a decode, an allocation or a callback). */
NR_CELL_STATIC_INIT(pthread_mutex_t, g_mu, PTHREAD_MUTEX_INITIALIZER);
#define g_mu NR_CELL_REF(g_mu)
NR_CELL_STATIC_INIT(pthread_cond_t, g_cv, PTHREAD_COND_INITIALIZER);
#define g_cv NR_CELL_REF(g_cv)
NR_CELL_STATIC(entry_t *, g_q);
#define g_q NR_CELL_REF(g_q)
NR_CELL_STATIC(char *, g_slots);
#define g_slots NR_CELL_REF(g_slots) /* depth job slots of g_slot_bytes, allocated once at start: nothing is allocated on submit */
NR_CELL_STATIC(int *, g_free);
#define g_free NR_CELL_REF(g_free) /* stack of free slot indices */
NR_CELL_STATIC(int, g_nfree);
#define g_nfree NR_CELL_REF(g_nfree)
NR_CELL_STATIC_INIT(size_t, g_slot_bytes, 64);
#define g_slot_bytes NR_CELL_REF(g_slot_bytes)
NR_CELL_STATIC(_Atomic uint64_t, g_allocs);
#define g_allocs NR_CELL_REF(g_allocs)
NR_CELL_STATIC(int, g_depth);
#define g_depth NR_CELL_REF(g_depth)
NR_CELL_STATIC(int, g_head);
#define g_head NR_CELL_REF(g_head)
NR_CELL_STATIC(int, g_n);
#define g_n NR_CELL_REF(g_n)
NR_CELL_STATIC(bool, g_run);
#define g_run NR_CELL_REF(g_run)
NR_CELL_STATIC(bool, g_stop);
#define g_stop NR_CELL_REF(g_stop)
NR_CELL_STATIC(pthread_t, g_th);
#define g_th NR_CELL_REF(g_th)
typedef int (*recov_run_fn_t)(void *, const nr_recov_key_t *);
typedef bool (*recov_exp_fn_t)(const nr_recov_key_t *);
typedef void (*recov_fb_fn_t)(void *, const nr_recov_key_t *, int);
typedef void (*recov_done_fn_t)(void *);
typedef bool (*recov_busy_fn_t)(void);
NR_CELL_STATIC(recov_run_fn_t, g_run_fn);
#define g_run_fn NR_CELL_REF(g_run_fn)
NR_CELL_STATIC(recov_exp_fn_t, g_exp_fn);
#define g_exp_fn NR_CELL_REF(g_exp_fn)
NR_CELL_STATIC(recov_fb_fn_t, g_fb_fn);
#define g_fb_fn NR_CELL_REF(g_fb_fn)
NR_CELL_STATIC(recov_done_fn_t, g_done_fn);
#define g_done_fn NR_CELL_REF(g_done_fn)
NR_CELL_STATIC(recov_busy_fn_t, g_busy_fn);
#define g_busy_fn NR_CELL_REF(g_busy_fn)
NR_CELL_STATIC_INIT(uint64_t, g_idle_wait_ns, 2000000ull);
#define g_idle_wait_ns NR_CELL_REF(g_idle_wait_ns)
NR_CELL_STATIC(_Atomic uint64_t, g_yielded);
#define g_yielded NR_CELL_REF(g_yielded)
NR_CELL_STATIC(_Atomic uint64_t, g_sub);
#define g_sub NR_CELL_REF(g_sub)
NR_CELL_STATIC(_Atomic uint64_t, g_resc);
#define g_resc NR_CELL_REF(g_resc)
NR_CELL_STATIC(_Atomic uint64_t, g_stale);
#define g_stale NR_CELL_REF(g_stale)
NR_CELL_STATIC(_Atomic uint64_t, g_expd);
#define g_expd NR_CELL_REF(g_expd)
NR_CELL_STATIC(_Atomic uint64_t, g_full);
#define g_full NR_CELL_REF(g_full)
NR_CELL_STATIC(_Atomic uint64_t, g_dropped_old);
#define g_dropped_old NR_CELL_REF(g_dropped_old)
typedef _Atomic uint64_t g_late_arr_t[LATE_BINS];
NR_CELL_STATIC(g_late_arr_t, g_late);
#define g_late NR_CELL_REF(g_late)
NR_CELL_STATIC(bool, g_nice);
#define g_nice NR_CELL_REF(g_nice)
NR_CELL_STATIC(bool, g_mu_init);
#define g_mu_init NR_CELL_REF(g_mu_init)
static __thread uint64_t t_cur_ns; /* submit time of the job the worker is running */
static __thread void *t_cur_job; /* job the worker is on */
static __thread bool t_fb_done;

/* newest (rnti, harq_pid) slot seen: 65536 hashed cells; a collision only ever discards a rescue (never delivers a wrong one) */
typedef _Atomic uint64_t g_seen_arr_t[65536];
NR_CELL_STATIC_HEAP(g_seen_arr_t, g_seen);
#define g_seen NR_CELL_HEAP_REF(g_seen_arr_t, g_seen) /* (abs_slot + 1) << 8 | ndi; 0 = none */
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
void nr_pdsch_recovery_set_busy_hook(bool (*busy)(void)) { g_busy_fn = busy; }
void nr_pdsch_recovery_test_idle_wait_ns(uint64_t ns) { g_idle_wait_ns = ns; }
static void nap_100us(void)
{
  struct timespec t = {0, 100000};
  nanosleep(&t, NULL);
}
void nr_pdsch_recovery_set_hooks(int (*run)(void *, const nr_recov_key_t *), bool (*expired)(const nr_recov_key_t *),
                                 void (*feedback)(void *, const nr_recov_key_t *, int), void (*done)(void *))
{
  g_done_fn = done;
  g_run_fn = run;
  g_exp_fn = expired;
  g_fb_fn = feedback;
}
/* The deferred job's learning feedback is delivered exactly once, whatever happens to it (see the header). */
static void feedback(const nr_recov_key_t *k, int outcome)
{
  if (g_fb_fn && t_cur_job && !t_fb_done) {
    t_fb_done = true;
    g_fb_fn(t_cur_job, k, outcome);
  }
}
bool nr_pdsch_recovery_enabled(void) { return true; }
static bool stale(const nr_recov_key_t *k)
{
  return (atomic_load_explicit(&g_seen[seen_ix(k->rnti, k->harq_pid)], memory_order_relaxed) >> 8) > (uint64_t)k->abs_slot + 1;
}
int nr_pdsch_recovery_publish(const nr_recov_key_t *k, const uint8_t *tb, uint32_t tbs)
{
  (void)tb; (void)tbs;
  if (g_exp_fn && g_exp_fn(k)) {
    atomic_fetch_add_explicit(&g_expd, 1, memory_order_relaxed);
    feedback(k, NR_RECOV_FB_FAIL_NOCREDIT);
    return 2;
  }
  if (stale(k)) {
    atomic_fetch_add_explicit(&g_stale, 1, memory_order_relaxed);
    feedback(k, NR_RECOV_FB_FAIL);
    return 1;
  }
  atomic_fetch_add_explicit(&g_resc, 1, memory_order_relaxed);
  feedback(k, NR_RECOV_FB_RESCUED);
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
static void add_cores(cpu_set_t *set, const char *list)
{
  for (const char *p = list; *p;) {
    char *end;
    long c = strtol(p, &end, 10);
    if (end == p) break;
    if (c >= 0 && c < CPU_SETSIZE) CPU_SET((int)c, set);
    p = *end == ',' ? end + 1 : end;
  }
}
/* cores given (arg): exactly those. Otherwise the core plan's recovery CPUs (legacy: little cores 10-14) minus every core the launcher
 * pins a critical thread to (same env keys); if none is left the worker is unpinned at nice 10. */
static void pick_cores(cpu_set_t *set, const char *cores)
{
  CPU_ZERO(set);
  if (cores) {
    add_cores(set, cores);
    return;
  }
  cpu_set_t busy;
  CPU_ZERO(&busy);
  static const char *const keys[] = {"ISAC_PDSCH_CORES", "ISAC_USRP_RING_CORE", "ISAC_UE_RT_CORE", "ISAC_PUSCH_CORES", "ISAC_PDCCH_USS_CORE",
                                     "ISAC_PDCCH_JOINT_ANON_CORE", "ISAC_PDCCH_PAR_CORE", "ISAC_CSIRS_DISC_CORE", "ISAC_CSIRS_DISC_DETECT_CORES",
                                     "ISAC_GPU_WORKER_CORE", "ISAC_UHD_CTRL_CORES"};
  for (unsigned i = 0; i < sizeof keys / sizeof *keys; i++)
    if (nr_env_core(keys[i])) add_cores(&busy, nr_env_core(keys[i]));
  const nr_core_layout_t *L = nr_core_layout ? nr_core_layout() : NULL; /* host core plan: its recovery CPUs; else the legacy DGX little cores 10-14 */
  for (int c = 0; c < CPU_SETSIZE && c < NR_CL_MAX_CPUS; c++)
    if ((L ? L->recov[c] != 0 : (c >= 10 && c <= 14)) && !CPU_ISSET(c, &busy)) CPU_SET(c, set);
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
  snprintf(buf, (size_t)n, "RECOV submitted=%lu rescued=%lu stale=%lu expired=%lu dropped_full=%lu dropped_old=%lu yielded=%lu late_ms_p50/p99=%d/%d",
           (unsigned long)atomic_load(&g_sub), (unsigned long)atomic_load(&g_resc), (unsigned long)atomic_load(&g_stale),
           (unsigned long)atomic_load(&g_expd), (unsigned long)atomic_load(&g_full), (unsigned long)atomic_load(&g_dropped_old), (unsigned long)atomic_load(&g_yielded), p50, p99);
}

void nr_pdsch_recovery_set_job_bytes(size_t n) { g_slot_bytes = n < 64 ? 64 : (n + 63) & ~(size_t)63; }
uint64_t nr_pdsch_recovery_allocs(void) { return atomic_load(&g_allocs); }
void nr_pdsch_recovery_test_lock(int on)
{
  if (on) pthread_mutex_lock(&g_mu);
  else pthread_mutex_unlock(&g_mu);
}

static void *worker(void *arg)
{
  nr_cell_bind((int)(intptr_t)arg); /* mpci A5: one worker per cell, bound to the cell that started it: run/expired/feedback hooks see that cell's queue state */
  if (g_nice) setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
  extern void nr_pdsch_recovery_thread_prewarm(void) __attribute__((weak)); /* nr_pdsch_passive_queue.c */
  if (nr_pdsch_recovery_thread_prewarm) nr_pdsch_recovery_thread_prewarm();
  pthread_mutex_lock(&g_mu);
  for (;;) {
    while (g_n == 0 && !g_stop) pthread_cond_wait(&g_cv, &g_mu);
    if (g_n == 0) break;
    entry_t e = g_q[g_head];
    g_head = (g_head + 1) % g_depth;
    g_n--;
    pthread_mutex_unlock(&g_mu);
    t_cur_job = e.job;
    t_fb_done = false;
    if (g_exp_fn && g_exp_fn(&e.k)) {
      atomic_fetch_add_explicit(&g_expd, 1, memory_order_relaxed);
      feedback(&e.k, NR_RECOV_FB_FAIL_NOCREDIT);
    } else if (stale(&e.k)) { /* a newer slot of this process is known: do not even run the arms */
      atomic_fetch_add_explicit(&g_stale, 1, memory_order_relaxed);
      feedback(&e.k, NR_RECOV_FB_FAIL);
    } else if (!nr_recov_wait_idle(g_busy_fn, now_ns, nap_100us, g_idle_wait_ns)) {
      /* a first-pass decode still waits after the bounded wait: the lane yields, no arm runs; the failure verdict stands and its feedback is delivered once */
      atomic_fetch_add_explicit(&g_yielded, 1, memory_order_relaxed);
      feedback(&e.k, NR_RECOV_FB_FAIL);
    } else if (g_run_fn) {
      t_cur_ns = e.t_ns;
      nr_pool_site_acquire(NR_POOL_RETRY, 0, 0); /* core pool: async rescue arms, after the first-pass decodes */
      const int r = g_run_fn(e.job, &e.k);
      nr_pool_site_release();
      if (r == 2) {
        atomic_fetch_add_explicit(&g_expd, 1, memory_order_relaxed);
        feedback(&e.k, NR_RECOV_FB_FAIL_NOCREDIT);
      } else { /* nothing rescued (0) / abandoned (3, config epoch changed) / handled: the verdict is delivered unless publish already did */
        feedback(&e.k, NR_RECOV_FB_FAIL);
      }
      t_cur_ns = 0;
    } else {
      feedback(&e.k, NR_RECOV_FB_FAIL);
    }
    t_cur_job = NULL;
    if (g_done_fn) g_done_fn(e.job);
    pthread_mutex_lock(&g_mu);
    g_free[g_nfree++] = (int)(((char *)e.job - g_slots) / (ptrdiff_t)g_slot_bytes);
  }
  pthread_mutex_unlock(&g_mu);
  return NULL;
}
int nr_pdsch_recovery_start(int depth, const char *cores)
{
  if (!g_mu_init) { /* first call: priority-inheritance mutex */
    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_setprotocol(&ma, PTHREAD_PRIO_INHERIT);
    pthread_mutex_init(&g_mu, &ma);
    pthread_mutexattr_destroy(&ma);
    g_mu_init = true;
  }
  pthread_mutex_lock(&g_mu);
  if (g_run) {
    pthread_mutex_unlock(&g_mu);
    return 0;
  }
  if (depth <= 0) {
    depth = 32;
  }
  if (depth < 1) depth = 1;
  if (depth > RQ_MAX) depth = RQ_MAX;
  free(g_q); free(g_free); free(g_slots); /* left over from a previous run */
  g_q = calloc((size_t)depth, sizeof(entry_t));
  g_free = calloc((size_t)depth, sizeof(int));
  g_slots = aligned_alloc(64, (size_t)depth * g_slot_bytes);
  atomic_fetch_add(&g_allocs, 3);
  if (!g_q || !g_free || !g_slots) {
    free(g_q); free(g_free); free(g_slots);
    g_q = NULL; g_free = NULL; g_slots = NULL;
    pthread_mutex_unlock(&g_mu);
    return -1;
  }
  memset(g_slots, 0, (size_t)depth * g_slot_bytes);
  g_nfree = depth;
  for (int i = 0; i < depth; i++) g_free[i] = depth - 1 - i;
  cpu_set_t set;
  pick_cores(&set, cores);
  g_nice = CPU_COUNT(&set) == 0;
  g_depth = depth; g_head = g_n = 0; g_stop = false;
  pthread_attr_t at; /* explicit SCHED_OTHER: do not inherit an RT policy from the starting thread */
  pthread_attr_init(&at);
  pthread_attr_setinheritsched(&at, PTHREAD_EXPLICIT_SCHED);
  pthread_attr_setschedpolicy(&at, SCHED_OTHER);
  const int prc = pthread_create(&g_th, &at, worker, (void *)(intptr_t)nr_cell_idx());
  pthread_attr_destroy(&at);
  if (prc) {
    free(g_q); free(g_free); free(g_slots);
    g_q = NULL; g_free = NULL; g_slots = NULL;
    pthread_mutex_unlock(&g_mu);
    return -1;
  }
  g_run = true;
  pthread_mutex_unlock(&g_mu);
  pthread_setname_np(g_th, "pdschRecov");
  if (CPU_COUNT(&set)) pthread_setaffinity_np(g_th, sizeof(set), &set); /* policy stays SCHED_OTHER nice 0 */
  return 0;
}
void nr_pdsch_recovery_stop(void)
{
  pthread_mutex_lock(&g_mu);
  if (!g_run) {
    pthread_mutex_unlock(&g_mu);
    return;
  }
  g_stop = true; /* acquire/submit refuse from now on */
  pthread_cond_broadcast(&g_cv);
  pthread_mutex_unlock(&g_mu);
  pthread_join(g_th, NULL);
  pthread_mutex_lock(&g_mu);
  while (g_n) { /* the worker drains before it exits; anything a racing submit left gets its failure verdict here */
    entry_t e = g_q[g_head];
    g_head = (g_head + 1) % g_depth;
    g_n--;
    if (g_fb_fn) g_fb_fn(e.job, &e.k, NR_RECOV_FB_FAIL);
    if (g_done_fn) g_done_fn(e.job);
  }
  g_run = false; /* the arrays stay allocated (freed by the next start): a slot still held by a caller can be released safely */
  pthread_mutex_unlock(&g_mu);
}
void *nr_pdsch_recovery_acquire(void)
{
  if (pthread_mutex_trylock(&g_mu) != 0) { /* busy: never wait on the worker, the caller keeps the job and feeds the failure itself */
    atomic_fetch_add_explicit(&g_full, 1, memory_order_relaxed);
    return NULL;
  }
  void *p = NULL;
  entry_t old = {0};
  bool evicted = false;
  if (g_run && !g_stop) {
    if (g_nfree > 0) {
      p = g_slots + (size_t)g_free[--g_nfree] * g_slot_bytes;
    } else if (g_n > 0) { /* full: the OLDEST queued job is the least likely to still matter (a newer slot of its HARQ process may already exist): it is dropped, its slot reused */
      old = g_q[g_head];
      g_head = (g_head + 1) % g_depth;
      g_n--;
      evicted = true;
      p = old.job;
    }
  }
  pthread_mutex_unlock(&g_mu);
  if (evicted) { /* its failure verdict is delivered exactly once, here, as for any job the worker does not rescue */
    atomic_fetch_add_explicit(&g_dropped_old, 1, memory_order_relaxed);
    if (g_fb_fn) g_fb_fn(old.job, &old.k, NR_RECOV_FB_FAIL);
    if (g_done_fn) g_done_fn(old.job);
  }
  if (!p) atomic_fetch_add_explicit(&g_full, 1, memory_order_relaxed);
  return p;
}
void nr_pdsch_recovery_release(void *slot)
{
  pthread_mutex_lock(&g_mu);
  if (!g_free) { pthread_mutex_unlock(&g_mu); return; }
  g_free[g_nfree++] = (int)(((char *)slot - g_slots) / (ptrdiff_t)g_slot_bytes);
  pthread_mutex_unlock(&g_mu);
}
int nr_pdsch_recovery_submit(const nr_recov_key_t *k, void *slot)
{
  pthread_mutex_lock(&g_mu); /* a few array operations; priority-inheriting */
  if (!g_run || g_stop || g_n == g_depth) { /* cannot happen for an acquired slot while running; stop refuses */
    pthread_mutex_unlock(&g_mu);
    return 1;
  }
  g_q[(g_head + g_n++) % g_depth] = (entry_t){*k, slot, now_ns()};
  atomic_fetch_add_explicit(&g_sub, 1, memory_order_relaxed);
  pthread_cond_signal(&g_cv);
  pthread_mutex_unlock(&g_mu);
  return 0;
}

/* mpci A5: allocates cell `cell`'s heap object (zeroed) before the RX fence; see nr_pdsch_cell_prealloc.h */
void nr_pdsch_recovery_cell_prealloc(int cell)
{
  NR_CELL_HEAP_PREALLOC(g_seen_arr_t, g_seen, cell);
}
