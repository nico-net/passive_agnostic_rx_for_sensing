#define _GNU_SOURCE
#include "nr_pdsch_recovery.h"
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
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static entry_t *g_q;
static char *g_slots;      /* depth job slots of g_slot_bytes, allocated once at start: nothing is allocated on submit */
static int *g_free;        /* stack of free slot indices */
static int g_nfree;
static size_t g_slot_bytes = 64;
static _Atomic uint64_t g_allocs;
static int g_depth, g_head, g_n;
static bool g_run, g_stop;
static pthread_t g_th;
static int (*g_run_fn)(void *, const nr_recov_key_t *);
static bool (*g_exp_fn)(const nr_recov_key_t *);
static void (*g_fb_fn)(void *, const nr_recov_key_t *, int);
static void (*g_done_fn)(void *);
static _Atomic uint64_t g_sub, g_resc, g_stale, g_expd, g_full, g_late[LATE_BINS];
static bool g_nice, g_mu_init;
static __thread uint64_t t_cur_ns; /* submit time of the job the worker is running */
static __thread void *t_cur_job; /* job the worker is on */
static __thread bool t_fb_done;

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
/* cores given (arg or ISAC_PDSCH_RECOVERY_CORES): exactly those. Otherwise the background little cores 10-14 minus every core the launcher
 * pins a critical thread to (same env keys); if none is left the worker is unpinned at nice 10. */
static void pick_cores(cpu_set_t *set, const char *cores)
{
  CPU_ZERO(set);
  if (!cores) cores = getenv("ISAC_PDSCH_RECOVERY_CORES");
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
    if (getenv(keys[i])) add_cores(&busy, getenv(keys[i]));
  for (int c = 10; c <= 14; c++)
    if (!CPU_ISSET(c, &busy)) CPU_SET(c, set);
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

void nr_pdsch_recovery_set_job_bytes(size_t n) { g_slot_bytes = n < 64 ? 64 : (n + 63) & ~(size_t)63; }
uint64_t nr_pdsch_recovery_allocs(void) { return atomic_load(&g_allocs); }
void nr_pdsch_recovery_test_lock(int on)
{
  if (on) pthread_mutex_lock(&g_mu);
  else pthread_mutex_unlock(&g_mu);
}

static void *worker(void *arg)
{
  (void)arg;
  if (g_nice) setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
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
    } else if (g_run_fn) {
      t_cur_ns = e.t_ns;
      const int r = g_run_fn(e.job, &e.k);
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
    const char *e = getenv("ISAC_PDSCH_RECOVERY_Q");
    depth = e ? atoi(e) : 32;
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
  const int prc = pthread_create(&g_th, &at, worker, NULL);
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
  if (g_run && !g_stop && g_nfree > 0) p = g_slots + (size_t)g_free[--g_nfree] * g_slot_bytes;
  pthread_mutex_unlock(&g_mu);
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
