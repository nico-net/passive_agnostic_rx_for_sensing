/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Live observer for nr_dmrs_nid_solve (see the header). Adapted from commit 571bd335d4, reference = Point A (the same pilot grid the GPU NID sweep
 * uses: subcarrier pa + 12 c + 1 + 4 q, sequence index m = 3 c + q). Observe only: it votes per N_ID and logs; nothing consumes the result.
 * Env: ISAC_DMRS_NID_SOLVE=1 observe | 2 observe + leader hint | 3 primary (top-2 hint, GPU sweep on demand, see nr_pdcch_nid_gpu.c) (default off), _EVERY=N (offer 1 symbol in N, default 32), _CORE=C (pin the worker, default unpinned),
 * _OPT=0 disables the cost cuts (energy gate on windows, fast solver, 1-in-4 once the leader is stable; default on). */
#define _GNU_SOURCE
#include "nr_dmrs_nid_solve.h"
#include "nr_pdcch_nid_gpu.h"
#include "common/utils/LOG/log.h"
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RING 4
#define MAXRB 276
typedef struct { int16_t r, i; } c16s_t;
typedef struct { int slot, sym, c0, nc; float yr[3 * MAXRB], yi[3 * MAXRB]; } job_t; /* pilot (c, q) of RB c0 + c at 3c + q; 0+0j = absent */
typedef struct { int nid; uint32_t votes; uint64_t win; int lo_c, hi_c; uint32_t symmask; } vote_t; /* lo_c/hi_c: RBs (rel. Point A) of the window votes */

static job_t g_ring[RING];
static int g_head, g_count;
static pthread_mutex_t g_q = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static vote_t g_votes[64];
static int g_leader = -1;
static pthread_mutex_t g_vq = PTHREAD_MUTEX_INITIALIZER; /* votes: worker writes, the hint reader reads */
static _Atomic int g_pa_rb = 0, g_pa_ok = 0;
static _Atomic uint64_t g_offered, g_dropped, g_done;
static struct timespec g_t0;
static int g_every = 32, g_mode = 1, g_opt = 1;
static _Atomic uint64_t g_ns_sym, g_ns_win, g_win_solved, g_win_skipped, g_jobs_skipped;

static int enabled(void)
{
  static _Atomic int v = -1;
  int x = atomic_load_explicit(&v, memory_order_relaxed);
  if (x < 0) {
    const char *e = getenv("ISAC_DMRS_NID_SOLVE"), *ev = getenv("ISAC_DMRS_NID_SOLVE_EVERY");
    x = (e != NULL && (atoi(e) >= 1 && atoi(e) <= 3));
    g_mode = e != NULL ? atoi(e) : 0;
    if (getenv("ISAC_DMRS_NID_SOLVE_OPT") != NULL) g_opt = atoi(getenv("ISAC_DMRS_NID_SOLVE_OPT")) != 0;
    if (ev != NULL && atoi(ev) >= 1) g_every = atoi(ev);
    atomic_store_explicit(&v, x, memory_order_relaxed);
  }
  return x;
}

static double since_start(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (t.tv_sec - g_t0.tv_sec) + 1e-9 * (t.tv_nsec - g_t0.tv_nsec);
}

static void vote(int nid, int win_c, int sym)
{
  pthread_mutex_lock(&g_vq);
  int k = -1, free_k = -1, min_k = 0;
  for (int i = 0; i < 64; i++) {
    if (g_votes[i].votes && g_votes[i].nid == nid) { k = i; break; }
    if (!g_votes[i].votes && free_k < 0) free_k = i;
    if (g_votes[i].votes < g_votes[min_k].votes) min_k = i;
  }
  if (k < 0) { k = free_k >= 0 ? free_k : min_k; g_votes[k] = (vote_t){nid, 0, 0, 1 << 30, -1, 0}; }
  g_votes[k].votes++;
  g_votes[k].symmask |= 1u << (sym & 15);
  if (win_c >= 0) {
    if (win_c / 6 < 64) g_votes[k].win |= 1ull << (win_c / 6);
    if (win_c < g_votes[k].lo_c) g_votes[k].lo_c = win_c;
    if (win_c + 6 > g_votes[k].hi_c) g_votes[k].hi_c = win_c + 6;
  }
  if (g_votes[k].votes == 8) {
    struct timespec rt;
    clock_gettime(CLOCK_MONOTONIC, &rt);
    LOG_A(PHY, "SENSING: DMRS_NID_SOLVE confirmed N_ID=%d elapsed=%.3f s t=%ld.%03ld\n", nid, since_start(), (long)rt.tv_sec, (long)(rt.tv_nsec / 1000000));
  }
  int lead = 0;
  for (int i = 1; i < 64; i++)
    if (g_votes[i].votes > g_votes[lead].votes) lead = i;
  if (lead != g_leader && g_votes[lead].votes >= 3 && (g_leader < 0 || g_votes[lead].votes > g_votes[g_leader].votes + 2)) {
    g_leader = lead;
    struct timespec rt;
    clock_gettime(CLOCK_MONOTONIC, &rt); /* same clock as the PDCCH_NID_GPU pass lines */
    LOG_A(PHY, "SENSING: DMRS_NID_SOLVE leader N_ID=%d votes=%u windows=0x%llx elapsed=%.3f s t=%ld.%03ld\n", g_votes[lead].nid, g_votes[lead].votes,
          (unsigned long long)g_votes[lead].win, since_start(), (long)rt.tv_sec, (long)(rt.tv_nsec / 1000000));
  }
  pthread_mutex_unlock(&g_vq);
}

static int cmpf(const void *a, const void *b) { const float x = *(const float *)a, y = *(const float *)b; return (x > y) - (x < y); }
static uint64_t cpu_ns(void)
{
  struct timespec t;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
/* A leader that is far ahead of the runner-up: discovery is done, only the second CORESET (if any) is still worth looking for. */
static int stable(void)
{
  if (g_leader < 0 || g_votes[g_leader].votes < 48) return 0;
  uint32_t second = 0;
  for (int i = 0; i < 64; i++)
    if (i != g_leader && g_votes[i].votes > second) second = g_votes[i].votes;
  return g_votes[g_leader].votes >= 3 * second;
}

/* Whole symbol (confirmation by top checks, a DCI split over scattered bundles still gives >= 16 pilots), then each 6-RB window. */
static void solve(const job_t *j)
{
  static float yr[NR_NID_SOLVE_MAX_PILOT], yi[NR_NID_SOLVE_MAX_PILOT];
  static int mm[NR_NID_SOLVE_MAX_PILOT];
  static uint32_t njob;
  nr_nid_solution_t sol[2];
  if (g_opt && stable() && (njob++ & 3) != 0) { atomic_fetch_add_explicit(&g_jobs_skipped, 1, memory_order_relaxed); return; }
  const uint64_t t0 = cpu_ns();
  int n = 0;
  for (int k = 0; k < 3 * j->nc && n < NR_NID_SOLVE_MAX_PILOT; k++) {
    if (j->yr[k] == 0.0f && j->yi[k] == 0.0f) continue;
    yr[n] = j->yr[k]; yi[n] = j->yi[k]; mm[n] = 3 * j->c0 + k;
    n++;
  }
  if (n >= 16) {
    const int ns = nr_dmrs_nid_solve_ex(yr, yi, mm, n, j->slot, j->sym, 14, -1, sol, 2, g_opt);
    for (int i = 0; i < ns; i++)
      if (sol[i].top_checks >= 4 && sol[i].top_mismatches <= sol[i].top_checks / 8) vote(sol[i].nid, -1, j->sym);
  }
  const uint64_t t1 = cpu_ns();
  /* windows with no PDCCH energy cannot hold a DCI: skip those below twice the lower-quartile pilot power */
  const int nw = j->nc / 6;
  float pw[MAXRB / 6 + 1], sorted[MAXRB / 6 + 1];
  for (int w = 0; w < nw; w++) {
    float e = 0;
    for (int i = 0; i < 18; i++) e += j->yr[18 * w + i] * j->yr[18 * w + i] + j->yi[18 * w + i] * j->yi[18 * w + i];
    sorted[w] = pw[w] = e;
  }
  float gate = 0;
  if (g_opt && nw >= 4) { qsort(sorted, (size_t)nw, sizeof(float), cmpf); gate = 2.0f * sorted[nw / 4]; }
  for (int w = 0; w < nw; w++) {
    if (pw[w] < gate) { atomic_fetch_add_explicit(&g_win_skipped, 1, memory_order_relaxed); continue; }
    atomic_fetch_add_explicit(&g_win_solved, 1, memory_order_relaxed);
    int m[18];
    for (int i = 0; i < 18; i++) m[i] = 3 * (j->c0 + 6 * w) + i;
    const int ns = nr_dmrs_nid_solve_ex(&j->yr[18 * w], &j->yi[18 * w], m, 18, j->slot, j->sym, 14, -1, sol, 2, g_opt);
    for (int i = 0; i < ns; i++)
      if (sol[i].mismatches <= 1) vote(sol[i].nid, j->c0 + 6 * w, j->sym);
  }
  atomic_fetch_add_explicit(&g_ns_sym, t1 - t0, memory_order_relaxed);
  atomic_fetch_add_explicit(&g_ns_win, cpu_ns() - t1, memory_order_relaxed);
}

static bool hint_record(const vote_t *v, nr_pdcch_nid_record_t *r)
{
  if (v->symmask == 0) return false;
  int s0 = __builtin_ctz(v->symmask), s1 = s0;
  while (s1 + 1 < 16 && (v->symmask >> (s1 + 1) & 1)) s1++;
  r->nid = v->nid; r->sym = s0; r->dur = s1 - s0 + 1 > 3 ? 3 : s1 - s0 + 1;
  r->grid_rb = atomic_load(&g_pa_rb) + v->lo_c; r->groups = (v->hi_c - v->lo_c) / 6; r->L = 0; r->R = 0; r->shift = 0; r->ties = 0;
  return true;
}

/* New cell / retune: forget every vote (the old cell's ids must not be hinted). */
void nr_dmrs_nid_live_reset(void)
{
  pthread_mutex_lock(&g_vq);
  memset(g_votes, 0, sizeof(g_votes));
  g_leader = -1;
  pthread_mutex_unlock(&g_vq);
}

/* ISAC_DMRS_NID_SOLVE=2: the leader as a one-record CORESET hint for the discovered-CORESET bank (non-interleaved, extent from its window
 * votes, symbols from where it solved). The GPU sweep's own records replace it as soon as they publish. */
bool nr_dmrs_nid_live_hint(nr_pdcch_nid_record_t *r)
{
    bool ok = false;
  if (g_mode != 2 || !atomic_load(&g_pa_ok)) return false;
  pthread_mutex_lock(&g_vq);
  if (g_leader >= 0 && g_votes[g_leader].votes >= 8 && g_votes[g_leader].hi_c > g_votes[g_leader].lo_c) {
    ok = hint_record(&g_votes[g_leader], r);
  }
  pthread_mutex_unlock(&g_vq);
  return ok;
}

/* ISAC_DMRS_NID_SOLVE=3: up to cap (<= 2) records, the top-2 ids with >= 8 votes (a DCI-less id cannot block the real one); *sig identifies the
 * id set AND footprints (changes when an id enters/leaves or its footprint widens). Returns the count. */
int nr_dmrs_nid_live_hints(nr_pdcch_nid_record_t *r, int cap, uint64_t *sig)
{
  if (g_mode != 3 || !atomic_load(&g_pa_ok) || cap < 1) return 0;
  uint32_t votes[64];
  int top[2], n = 0;
  pthread_mutex_lock(&g_vq);
  for (int i = 0; i < 64; i++) votes[i] = g_votes[i].hi_c > g_votes[i].lo_c && g_votes[i].symmask ? g_votes[i].votes : 0;
  const int c = nr_dmrs_nid_top2(votes, 64, 8, top);
  *sig = 1;
  for (int i = 0; i < c && n < cap; i++, n++) {
    if (!hint_record(&g_votes[top[i]], &r[n])) { n--; continue; }
    *sig = nr_dmrs_nid_hint_sig(*sig, r[n].nid, r[n].grid_rb, r[n].groups, r[n].sym, r[n].dur);
  }
  pthread_mutex_unlock(&g_vq);
  return n;
}

static void *worker(void *arg)
{
  (void)arg;
  static job_t job;
  uint64_t n = 0;
  for (;;) {
    pthread_mutex_lock(&g_q);
    while (g_count == 0) pthread_cond_wait(&g_cv, &g_q);
    job = g_ring[g_head];
    g_head = (g_head + 1) % RING;
    g_count--;
    pthread_mutex_unlock(&g_q);
    solve(&job);
    atomic_fetch_add_explicit(&g_done, 1, memory_order_relaxed);
    if ((++n & 255) == 0) {
      struct timespec b;
      clock_gettime(CLOCK_THREAD_CPUTIME_ID, &b);
      int t3[3] = {-1, -1, -1};
      for (int r = 0; r < 3; r++) {
        for (int i = 0; i < 64; i++) {
          int taken = 0;
          for (int q = 0; q < r; q++) taken |= (t3[q] == i);
          if (!taken && g_votes[i].votes && (t3[r] < 0 || g_votes[i].votes > g_votes[t3[r]].votes)) t3[r] = i;
        }
      }
      LOG_A(PHY, "SENSING: DMRS_NID_SOLVE summary jobs=%llu offered=%llu dropped=%llu worker_cpu=%.3f s sym_us/job=%.0f win_us/job=%.0f win_solved=%llu win_skipped=%llu jobs_skipped=%llu top: N_ID=%d votes=%u win=0x%llx | N_ID=%d votes=%u win=0x%llx | N_ID=%d votes=%u win=0x%llx\n",
            (unsigned long long)n, (unsigned long long)atomic_load(&g_offered), (unsigned long long)atomic_load(&g_dropped), b.tv_sec + 1e-9 * b.tv_nsec,
            1e-3 * atomic_load(&g_ns_sym) / (double)(n - atomic_load(&g_jobs_skipped)), 1e-3 * atomic_load(&g_ns_win) / (double)(n - atomic_load(&g_jobs_skipped)),
            (unsigned long long)atomic_load(&g_win_solved), (unsigned long long)atomic_load(&g_win_skipped), (unsigned long long)atomic_load(&g_jobs_skipped),
            t3[0] >= 0 ? g_votes[t3[0]].nid : -1, t3[0] >= 0 ? g_votes[t3[0]].votes : 0, t3[0] >= 0 ? (unsigned long long)g_votes[t3[0]].win : 0,
            t3[1] >= 0 ? g_votes[t3[1]].nid : -1, t3[1] >= 0 ? g_votes[t3[1]].votes : 0, t3[1] >= 0 ? (unsigned long long)g_votes[t3[1]].win : 0,
            t3[2] >= 0 ? g_votes[t3[2]].nid : -1, t3[2] >= 0 ? g_votes[t3[2]].votes : 0, t3[2] >= 0 ? (unsigned long long)g_votes[t3[2]].win : 0);
    }
  }
  return NULL;
}

static void start(void)
{
  clock_gettime(CLOCK_MONOTONIC, &g_t0);
  pthread_attr_t at;
  pthread_attr_init(&at);
  const char *ce = getenv("ISAC_DMRS_NID_SOLVE_CORE");
  if (ce != NULL && atoi(ce) >= 0) {
    cpu_set_t s;
    CPU_ZERO(&s);
    CPU_SET(atoi(ce), &s);
    pthread_attr_setaffinity_np(&at, sizeof(s), &s);
  }
  pthread_t th;
  if (pthread_create(&th, &at, worker, NULL) == 0) {
    pthread_setname_np(th, "dmrs_nid_solve");
    pthread_detach(th);
  }
  pthread_attr_destroy(&at);
}

void nr_dmrs_nid_live_offer(const void *rx_c16, int fft, int fco, int nsc, int pa, int slot, int sym)
{
  static _Atomic uint32_t tick;
  if (!enabled() || (atomic_fetch_add_explicit(&tick, 1, memory_order_relaxed) % (uint32_t)g_every) != 0) return;
  const c16s_t *y = rx_c16;
  const int lo = pa >= 0 ? 0 : (-pa + 11) / 12;
  int hi = (nsc - pa - 9) / 12; /* RBs c < hi have their last pilot (pa + 12 c + 9) inside the carrier */
  if (hi - lo > MAXRB) hi = lo + MAXRB;
  if (hi - lo < 1) return;
  pthread_once(&g_once, start);
  atomic_store(&g_pa_ok, pa % 12 == 0);
  atomic_store(&g_pa_rb, pa / 12);
  atomic_fetch_add_explicit(&g_offered, 1, memory_order_relaxed);
  pthread_mutex_lock(&g_q);
  if (g_count < RING) {
    job_t *j = &g_ring[(g_head + g_count) % RING];
    j->slot = slot; j->sym = sym; j->c0 = lo; j->nc = hi - lo;
    for (int c = lo; c < hi; c++)
      for (int q = 0; q < 3; q++) {
        const c16s_t s = y[(fco + pa + 12 * c + 1 + 4 * q) % fft];
        j->yr[3 * (c - lo) + q] = (float)s.r;
        j->yi[3 * (c - lo) + q] = (float)s.i;
      }
    g_count++;
    pthread_cond_signal(&g_cv);
  } else {
    atomic_fetch_add_explicit(&g_dropped, 1, memory_order_relaxed);
  }
  pthread_mutex_unlock(&g_q);
}
