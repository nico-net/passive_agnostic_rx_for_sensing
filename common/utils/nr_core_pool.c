/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Shared core pool with priority admission: see nr_core_pool.h for the model and the bounds. */
#define _GNU_SOURCE
#include "nr_core_pool.h"
#include "nr_core_plan.h"
#include <dirent.h>
#include <errno.h>
#include <sys/prctl.h>
#include <limits.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>


/* ---- B1 (mpci-main plan B.4): per-cell fair share. Everything below derives from the pool's token count N; no host constant. ---- */
/* Half-life of the per-cell token-time usage U_c used to pick the next waiting cell inside a class (equal weights, no per-cell parameter). */
#define NR_POOL_USAGE_HALFLIFE_NS 50000000ull
/* Two cells whose usage differs by less than this are a tie (broken by the old round-robin). Absorbs clock noise; 1 us of token time. */
#define NR_POOL_USAGE_TIE_NS 1000.0
/* A cell is active while it holds a lease or has a waiter, and for this long after its last lease/wait event. */
#define NR_POOL_ACTIVE_WINDOW_NS 100000000ull
/* Below this many tokens DISC and HOUSE age promotion is off (FLOAT-LANE-POOL-20261021 section 3: at 3-4 tokens promotion cost DL p99 +2.5 ms and
 * UL miss 57 % at overload [Bench, DGX, synthetic spin jobs]; with promotion off decode is back within +0.5 ms). Applies to the default config. */
#define NR_POOL_PROMO_MIN_TOKENS 6

typedef struct waiter {
  int prio, cell, tok, last;
  bool ul;
  uint64_t deadline, t_enq, seq;
  pthread_cond_t *cv;
  struct waiter *prev, *next; /* intrusive wait list: the node lives on the waiting thread's stack, so the queue has no capacity limit and no allocation */
} waiter_t;

struct nr_pool {
  pthread_mutex_t mu;
  nr_pool_cfg_t cfg;
  int n, core[NR_POOL_MAX_TOKENS], cap[NR_POOL_MAX_TOKENS];
  bool busy[NR_POOL_MAX_TOKENS];
  int nfree;
  waiter_t *wq_head, *wq_tail; /* waiters in the order the former array held them (append; removal moves the tail into the hole, see wq_remove) */
  int nwq;
  int rr[NR_POOL_NPRIO];
  uint64_t seq;
  _Atomic int nwait[NR_POOL_NPRIO]; /* lock-free view for should_yield */
  int cnt[NR_POOL_NPRIO];           /* current leases per class */
  int cnt_dl;                       /* of the DECODE leases, the DL ones */
  int chan_demand;                  /* tokens the channelizer asked for (0 = none registered) */
  bool chan_tok[NR_POOL_MAX_TOKENS]; /* the r reserved tokens (fastest first): CHAN's own, nobody else is placed on them while r > 0 */
  cpu_set_t poolset, home;
  bool have_home;
  pid_t member[256];
  int nmember;
  nr_pool_stats_t st;
  /* per cell (index = cell): leases held, DL decode leases held, waiters queued, usage U_c (token-ns, decayed to cell_t), last event time */
  int cell_run[NR_POOL_MAX_CELLS], cell_dl[NR_POOL_MAX_CELLS], cell_wait[NR_POOL_MAX_CELLS];
  double cell_u[NR_POOL_MAX_CELLS];
  uint64_t cell_t[NR_POOL_MAX_CELLS], cell_act[NR_POOL_MAX_CELLS];
};

/* The calling thread's lease (a thread holds at most one). */
static __thread struct {
  const nr_pool_t *pool;
  int tok, prio, cell, cur_core;
  bool ul;
  uint64_t deadline, t_grant;
} tl = {NULL, -1, 0, 0, -1, false, 0, 0};
static __thread pthread_cond_t tl_cv = PTHREAD_COND_INITIALIZER;
static __thread int tl_last = -1; /* token of the previous lease: preferred when free */

/* Test hook (function, no flag): unit tests replace the clock by a virtual one (deterministic scheduling tests). NULL in production. While set, a waiter does not
 * arm the timed age wake-up (the virtual clock is not CLOCK_MONOTONIC); promotion is then evaluated on every dispatch only. */
static uint64_t (*g_test_clock)(void);
void nr_pool_set_clock_for_test(uint64_t (*fn)(void)) { g_test_clock = fn; }

uint64_t nr_pool_now_ns(void)
{
  if (g_test_clock) return g_test_clock();
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

nr_pool_cfg_t nr_pool_cfg_default(void)
{
  nr_pool_cfg_t c = {0};
  /* SCAN and DECODE are never promoted by age (the caps protect them); RETRY 4 ms (a late rescue is worth little after that), DISC 20 ms, HOUSE 100 ms */
  c.age_ns[NR_POOL_RETRY] = 4000000ull;
  c.age_ns[NR_POOL_DISC] = 20000000ull;
  c.age_ns[NR_POOL_HOUSE] = 100000000ull;
  c.pin = true;
  return c;
}

/* Default config for a pool of n tokens: the classic ages, with DISC/HOUSE promotion off below NR_POOL_PROMO_MIN_TOKENS tokens. */
nr_pool_cfg_t nr_pool_cfg_default_n(int n)
{
  nr_pool_cfg_t c = nr_pool_cfg_default();
  if (n < NR_POOL_PROMO_MIN_TOKENS) c.age_ns[NR_POOL_DISC] = c.age_ns[NR_POOL_HOUSE] = 0;
  return c;
}

/* CHAN bounds (header, CHAN CLASS). Pure functions of N and the demand. */
int nr_pool_chan_cap_n(int n) { return n > 2 ? n - 2 : 1; }
int nr_pool_chan_reserved_n(int n, int demand)
{
  if (n <= 2 || demand <= 0) return 0;
  return demand < n - 2 ? demand : n - 2;
}
static void dispatch(nr_pool_t *p, uint64_t now);
static int chan_res(const nr_pool_t *p) { return nr_pool_chan_reserved_n(p->n, p->chan_demand); }
/* Urgency rank (smaller = more urgent): CHAN first, then the classes in index order. */
static const int g_order[NR_POOL_NPRIO] = {NR_POOL_CHAN, NR_POOL_SCAN, NR_POOL_DECODE, NR_POOL_RETRY, NR_POOL_DISC, NR_POOL_HOUSE};
static int urgency(int prio) { return prio == NR_POOL_CHAN ? -1 : prio; }
static bool per_cell(int prio) { return prio != NR_POOL_CHAN; }
void nr_pool_set_chan_demand(nr_pool_t *p, int tokens)
{
  if (!p) return;
  pthread_mutex_lock(&p->mu);
  p->chan_demand = tokens < 0 ? 0 : tokens;
  /* The reserved tokens are fixed cores (the fastest ones, then the lowest index): a CHAN worker keeps its core between blocks, so a lease costs no
   * affinity change (a migrating sched_setaffinity costs ~110 us on the DGX [Bench, DGX GB10, shared host, n=5000], half a block period). */
  memset(p->chan_tok, 0, sizeof(p->chan_tok));
  for (int k = chan_res(p); k > 0; k--) {
    int best = -1;
    for (int i = 0; i < p->n; i++)
      if (!p->chan_tok[i] && (best < 0 || p->cap[i] > p->cap[best])) best = i;
    if (best >= 0) p->chan_tok[best] = true;
  }
  dispatch(p, nr_pool_now_ns()); /* a lowered demand frees tokens for waiters */
  pthread_mutex_unlock(&p->mu);
}
int nr_pool_chan_demand(const nr_pool_t *p) { return p ? p->chan_demand : 0; }
int nr_pool_chan_cap(const nr_pool_t *p) { return p ? nr_pool_chan_cap_n(p->n) : 0; }
int nr_pool_chan_reserved(const nr_pool_t *p) { return p ? chan_res(p) : 0; }

nr_pool_t *nr_pool_create(const int *cores, const int *capacity, int n, const nr_pool_cfg_t *cfg)
{
  if (!cores || n < 1 || n > NR_POOL_MAX_TOKENS) return NULL;
  nr_pool_t *p = calloc(1, sizeof(*p));
  if (!p) return NULL;
  pthread_mutex_init(&p->mu, NULL);
  p->cfg = cfg ? *cfg : nr_pool_cfg_default_n(n); /* an explicit cfg is a deliberate override: used as given */
  p->n = p->nfree = n;
  for (int i = 0; i < NR_POOL_NPRIO; i++) p->rr[i] = NR_POOL_MAX_CELLS - 1; /* cell 0 is served first */
  CPU_ZERO(&p->poolset);
  for (int i = 0; i < n; i++) {
    p->core[i] = cores[i];
    p->cap[i] = capacity ? capacity[i] : 0;
    if (cores[i] >= 0 && cores[i] < CPU_SETSIZE) CPU_SET(cores[i], &p->poolset);
  }
  cpu_set_t cur;
  if (sched_getaffinity(0, sizeof(cur), &cur) == 0) {
    CPU_ZERO(&p->home);
    for (int c = 0; c < CPU_SETSIZE; c++)
      if (CPU_ISSET(c, &cur) && !CPU_ISSET(c, &p->poolset)) CPU_SET(c, &p->home);
    p->have_home = CPU_COUNT(&p->home) > 0;
  }
  return p;
}

void nr_pool_destroy(nr_pool_t *p)
{
  if (!p) return;
  pthread_mutex_destroy(&p->mu);
  free(p);
}

int nr_pool_size(const nr_pool_t *p) { return p ? p->n : 0; }
int nr_pool_core_of(const nr_pool_t *p, int t) { return p && t >= 0 && t < p->n ? p->core[t] : -1; }
bool nr_pool_core_in_pool(const nr_pool_t *p, int c) { return p && c >= 0 && c < CPU_SETSIZE && CPU_ISSET(c, &p->poolset); }
int nr_pool_total_waiting(const nr_pool_t *p)
{
  int w = 0;
  for (int i = 0; i < NR_POOL_NPRIO; i++) w += atomic_load_explicit(&((nr_pool_t *)p)->nwait[i], memory_order_relaxed);
  return w;
}

static bool overdue(const nr_pool_t *p, const waiter_t *w, uint64_t now)
{
  const uint64_t a = p->cfg.age_ns[w->prio];
  return a && now > w->t_enq && now - w->t_enq >= a;
}

/* Per-cell bookkeeping (under p->mu). U_c is the exponentially decayed token-time of cell c: dU/dt = run_c - U/tau, tau = halflife/ln 2. */
/* 2^(-dt/half-life) without libm: whole half-lives by halving, the rest by a Taylor series of exp(-x), x < ln 2 (error < 1e-8). */
static double decay_factor(uint64_t dt)
{
  const uint64_t k = dt / NR_POOL_USAGE_HALFLIFE_NS;
  if (k >= 64) return 0.0;
  const double x = (double)(dt % NR_POOL_USAGE_HALFLIFE_NS) / (double)NR_POOL_USAGE_HALFLIFE_NS * 0.6931471805599453;
  double term = 1.0, sum = 1.0;
  for (int i = 1; i <= 9; i++) term *= -x / i, sum += term;
  for (uint64_t i = 0; i < k; i++) sum *= 0.5;
  return sum;
}
static double cell_decay(const nr_pool_t *p, int c, uint64_t now)
{
  const double tau = (double)NR_POOL_USAGE_HALFLIFE_NS / 0.6931471805599453;
  const uint64_t dt = now > p->cell_t[c] ? now - p->cell_t[c] : 0;
  const double d = decay_factor(dt);
  return p->cell_u[c] * d + (double)p->cell_run[c] * tau * (1.0 - d);
}
static void cell_touch(nr_pool_t *p, int c, uint64_t now) /* call before any change of cell_run[c] */
{
  if (now > p->cell_t[c]) {
    p->cell_u[c] = cell_decay(p, c, now);
    p->cell_t[c] = now;
  }
  if (now > p->cell_act[c]) p->cell_act[c] = now;
}
static bool cell_active(const nr_pool_t *p, int c, uint64_t now)
{
  return p->cell_run[c] > 0 || p->cell_wait[c] > 0 || (p->cell_act[c] && now <= p->cell_act[c] + NR_POOL_ACTIVE_WINDOW_NS);
}
/* A = active cells, `cell` (the asker) always counted. */
static int active_cells(const nr_pool_t *p, int cell, uint64_t now)
{
  int a = 0;
  for (int c = 0; c < NR_POOL_MAX_CELLS; c++) a += c == cell || cell_active(p, c, now);
  return a;
}

/* May a class `prio` of cell `cell` be granted a token right now (ignoring who else waits)? The caps keep one token for each urgent class (see
 * header (b)). promoted (waited past its age): only the hard limit busy < N. DL additionally has a per-cell cap max(1, N-1-(A-1)) (A active cells),
 * and a cell that already holds a DL token leaves one for every other cell that waits for its first one (global sum stays <= N-1). With A = 1 both
 * reduce to the global rule: identical to the single-cell pool. */
static bool class_ok(const nr_pool_t *p, int prio, bool promoted, bool ul, int cell, uint64_t now)
{
  if (prio == NR_POOL_CHAN) return p->nfree > 0 && p->cnt[NR_POOL_CHAN] < (chan_res(p) > 0 ? chan_res(p) : nr_pool_chan_cap_n(p->n));
  /* The tokens reserved for the channelizer and not held by it right now stay free for it; every other class works on N-r tokens (r = 0: unchanged). */
  const int res = chan_res(p), idle = res > p->cnt[NR_POOL_CHAN] ? res - p->cnt[NR_POOL_CHAN] : 0;
  if (p->nfree - idle <= 0) return false;
  if (promoted) return true;
  const int n = p->n - res;
  const int busy = p->n - p->nfree - p->cnt[NR_POOL_CHAN], low = p->cnt[NR_POOL_RETRY] + p->cnt[NR_POOL_DISC] + p->cnt[NR_POOL_HOUSE];
  const int lim1 = n > 1 ? n - 1 : 1, lim2 = n > 2 ? n - 2 : 1;
  bool ulw = false; /* a UL decode is queued: DL gives up its last core (cap N-2) so the freed token goes to UL */
  if (prio == NR_POOL_DECODE && !ul)
    for (const waiter_t *q = p->wq_head; q && !ulw; q = q->next) ulw = q->prio == NR_POOL_DECODE && q->ul;
  switch (prio) {
    case NR_POOL_SCAN: return p->cnt[NR_POOL_SCAN] + low < lim1;
    /* DL: counted alone, low classes do not shrink it (they yield at their next chunk step to a waiting decode). The last free token stays
     * the scan reserve unless a scan already holds one. So DL holds min(#DL consumers, N-1) tokens, N-1 only when scan is idle and no UL decode
     * waits; while one waits DL is capped at N-2, so the next DL release goes to UL (DL is refused, UL is the only DECODE waiter grantable). */
    case NR_POOL_DECODE: {
      if (ul) return p->cnt[NR_POOL_DECODE] + low < lim1;
      if (!(p->cnt_dl < lim1 - ulw && (busy < lim1 || p->cnt[NR_POOL_SCAN] > 0))) return false;
      const int A = active_cells(p, cell, now);
      if (A <= 1) return true;
      const int cap = lim1 - (A - 1) > 1 ? lim1 - (A - 1) : 1;
      if (p->cell_dl[cell] >= cap) return false;
      if (p->cell_dl[cell] == 0) return true; /* a cell without a DL token is never held back: that is its guaranteed one */
      int zw = 0; /* other cells waiting for their first DL token keep one reachable */
      bool seen[NR_POOL_MAX_CELLS] = {false};
      for (const waiter_t *w = p->wq_head; w; w = w->next) {
        if (w->prio == NR_POOL_DECODE && !w->ul && w->cell != cell && p->cell_dl[w->cell] == 0 && !seen[w->cell]) seen[w->cell] = true, zw++;
      }
      return p->cnt_dl + zw < lim1 - ulw;
    }
    default: return busy < lim2 && low < lim2;
  }
}

/* Big cores to the urgent classes, small ones to the rest; the thread's previous token wins ties (cache). */
static int pick_token(const nr_pool_t *p, int prio, bool promoted, int last)
{
  const bool hi = prio <= NR_POOL_DECODE || prio == NR_POOL_CHAN || promoted;
  const bool chan = prio == NR_POOL_CHAN;
  int best = -1;
  long bs = 0;
  for (int pass = 0; pass < 2 && best < 0; pass++)
    for (int i = 0; i < p->n; i++) {
      if (p->busy[i]) continue;
      /* pass 0: CHAN only on the reserved tokens, every other class only off them; pass 1 (nothing there is free): any free token */
      if (pass == 0 && p->chan_tok[i] != chan) continue;
      const long s = chan ? (i == last ? 1000000L : 0) + p->cap[i] : (hi ? p->cap[i] : -p->cap[i]) * 2 + (i == last);
      if (best < 0 || s > bs) best = i, bs = s;
    }
  return best;
}

/* Same order effect as the old array swap-remove (the last waiter takes the removed one's place): keeps every scan order, hence every tie, identical. */
static void wq_remove(nr_pool_t *p, waiter_t *x)
{
  waiter_t *t = p->wq_tail;
  p->wq_tail = t->prev;
  if (p->wq_tail) p->wq_tail->next = NULL; else p->wq_head = NULL;
  p->nwq--;
  if (x == t) return;
  t->prev = x->prev, t->next = x->next;
  if (t->prev) t->prev->next = t; else p->wq_head = t;
  if (t->next) t->next->prev = t; else p->wq_tail = t;
}

static waiter_t *select_waiter(nr_pool_t *p, uint64_t now, bool *promoted)
{
  waiter_t *best = NULL;
  /* starvation bound first: the oldest overdue waiter, but never into the tokens reserved for the channelizer and not held by it right now */
  const int res = chan_res(p), idle = res > p->cnt[NR_POOL_CHAN] ? res - p->cnt[NR_POOL_CHAN] : 0;
  for (waiter_t *w = p->wq_head; w && p->nfree - idle > 0; w = w->next)
    if (overdue(p, w, now) && (!best || w->t_enq < best->t_enq)) best = w;
  if (best) {
    *promoted = true;
    return best;
  }
  *promoted = false;
  for (int oi = 0; oi < NR_POOL_NPRIO; oi++) {
    const int pr = g_order[oi];
    /* Per cell (in round-robin order after the one served last) the best grantable waiter: oldest deadline. Then, between cells, the lowest decayed
     * token-time usage U_c; a tie (within NR_POOL_USAGE_TIE_NS) goes to the cell met first in round-robin order. A capped waiter does not block others. */
    waiter_t *bw = NULL;
    int bcell = -1;
    double bu = 0;
    for (int k = 1; k <= NR_POOL_MAX_CELLS; k++) {
      const int cell = (p->rr[pr] + k) % NR_POOL_MAX_CELLS;
      best = NULL;
      for (waiter_t *w = p->wq_head; w; w = w->next) {
        if (w->prio != pr || w->cell != cell || !class_ok(p, pr, false, w->ul, cell, now)) continue;
        if (!best || w->deadline < best->deadline || (w->deadline == best->deadline && w->seq < best->seq)) best = w;
      }
      if (!best) continue;
      if (!bw) { /* first candidate: U_c is computed only if a second cell competes */
        bw = best, bcell = cell, bu = -1.0;
        continue;
      }
      if (bu < 0) bu = cell_decay(p, bcell, now);
      const double u = cell_decay(p, cell, now);
      if (u + NR_POOL_USAGE_TIE_NS < bu) bw = best, bcell = cell, bu = u;
    }
    if (bw) {
      p->rr[pr] = bcell;
      return bw;
    }
  }
  return NULL;
}

static void grant(nr_pool_t *p, waiter_t *w, int tok, uint64_t now, bool promoted)
{
  p->busy[tok] = true;
  p->nfree--;
  p->cnt[w->prio]++;
  if (w->prio == NR_POOL_DECODE && !w->ul) p->cnt_dl++, p->cell_dl[w->cell]++;
  if (per_cell(w->prio)) cell_touch(p, w->cell, now), p->cell_run[w->cell]++;
  w->tok = tok;
  const uint64_t wait = now > w->t_enq ? now - w->t_enq : 0; /* now may predate a waiter that queued while an older dispatch ran */
  p->st.leases[w->prio]++;
  if (wait > 2000) p->st.waited[w->prio]++;
  if (promoted) p->st.promoted[w->prio]++;
  if (wait > p->st.wait_max_ns[w->prio]) p->st.wait_max_ns[w->prio] = wait;
  if (per_cell(w->prio)) {
    p->st.leases_cell[w->cell]++;
    p->st.leases_cell_cls[w->cell][w->prio == NR_POOL_DECODE && w->ul ? NR_POOL_CLS_UL : w->prio]++; /* under p->mu like every counter here */
    if (wait > 2000) p->st.waited_cell[w->cell]++;
    if (wait > p->st.wait_max_ns_cell[w->cell]) p->st.wait_max_ns_cell[w->cell] = wait;
  }
  if (wait > 50000000ull) { /* a lease wait this long: show who held the tokens (rate-limited) */
    static _Atomic int n_wait;
    if (atomic_fetch_add(&n_wait, 1) < 40)
      fprintf(stderr, "SENSING: CORE_POOL long wait class=%d%s waited_ms=%lu free=%d held scan=%d decode=%d(dl %d) retry=%d disc=%d house=%d\n", w->prio,
              w->ul ? "(ul)" : "", (unsigned long)(wait / 1000000ull), p->nfree, p->cnt[0], p->cnt[1], p->cnt_dl, p->cnt[2], p->cnt[3], p->cnt[4]);
  }
  uint64_t us = wait / 1000;
  int b = 0;
  while (us && b < NR_POOL_WAIT_BINS - 1) us >>= 1, b++;
  p->st.wait_hist[w->prio][b]++;
  if (per_cell(w->prio)) p->st.wait_hist_cell[w->cell][b]++;
}

static void dispatch(nr_pool_t *p, uint64_t now)
{
  while (p->nfree > 0 && p->nwq > 0) {
    bool pro;
    waiter_t *w = select_waiter(p, now, &pro);
    if (!w) break;
    wq_remove(p, w);
    atomic_fetch_sub_explicit(&p->nwait[w->prio], 1, memory_order_relaxed);
    if (per_cell(w->prio)) p->cell_wait[w->cell]--;
    grant(p, w, pick_token(p, w->prio, pro, w->last), now, pro);
    pthread_cond_signal(w->cv); /* under the lock: w lives on the waiter's stack until it sees tok >= 0 under the same lock */
  }
}

static void pin_to(const nr_pool_t *p, int core)
{
  if (!p->cfg.pin || tl.cur_core == core) return;
  cpu_set_t cs;
  CPU_ZERO(&cs);
  CPU_SET(core, &cs);
  if (sched_setaffinity(0, sizeof(cs), &cs) == 0) tl.cur_core = core;
}

static pid_t tl_tid(void)
{
  static __thread pid_t t;
  if (!t) t = (pid_t)syscall(SYS_gettid);
  return t;
}

static bool is_member(nr_pool_t *p, pid_t tid);
static void go_home(const nr_pool_t *p)
{
  if (!p->cfg.pin || !p->have_home || tl.cur_core < 0) return;
  if (sched_setaffinity(0, sizeof(p->home), &p->home) == 0) tl.cur_core = -1;
}

static int lease(nr_pool_t *p, int prio, int cell, uint64_t deadline, bool block)
{
  if (!p) return -1;
  if (tl.pool == p) { /* already holds one (nested request: keeps the outer class; a different class is counted, see header (c)) */
    if ((prio & ~NR_POOL_F_UL) != tl.prio) { pthread_mutex_lock(&p->mu); p->st.nested++; pthread_mutex_unlock(&p->mu); }
    return p->core[tl.tok];
  }
  const bool ul = (prio & NR_POOL_F_UL) != 0;
  prio &= ~NR_POOL_F_UL;
  if (prio < 0) prio = 0;
  if (prio >= NR_POOL_NPRIO) prio = NR_POOL_NPRIO - 1;
  cell = (unsigned)cell % NR_POOL_MAX_CELLS;
  const uint64_t now = nr_pool_now_ns();
  waiter_t w = {.prio = prio, .cell = cell, .tok = -1, .ul = ul, .last = tl_last, .deadline = deadline ? deadline : now, .t_enq = now, .cv = &tl_cv};
  pthread_mutex_lock(&p->mu);
  w.seq = p->seq++;
  if (!block) { /* grant only if no more urgent waiter could take this token */
    bool ahead = false;
    for (const waiter_t *q = p->wq_head; q; q = q->next)
      if (urgency(q->prio) < urgency(prio) && class_ok(p, q->prio, false, q->ul, q->cell, now)) ahead = true;
    if (ahead || !class_ok(p, prio, false, ul, cell, now)) {
      pthread_mutex_unlock(&p->mu);
      return -1;
    }
    grant(p, &w, pick_token(p, prio, false, w.last), now, false);
  } else {
    w.prev = p->wq_tail, w.next = NULL;
    if (p->wq_tail) p->wq_tail->next = &w; else p->wq_head = &w;
    p->wq_tail = &w;
    p->nwq++;
    atomic_fetch_add_explicit(&p->nwait[prio], 1, memory_order_relaxed);
    if (per_cell(prio)) {
      p->cell_wait[cell]++;
      if (now > p->cell_act[cell]) p->cell_act[cell] = now;
    }
    dispatch(p, now); /* grants the best grantable waiter, possibly this one: a capped higher class must not hold a free token idle */
    const uint64_t age = p->cfg.age_ns[prio];
    while (w.tok < 0) {
      /* A waiter held back by a class cap while a token is free gets no release/arrival event: wake at its age limit and re-dispatch, or the
       * aging bound would only hold while somebody else happens to release. */
      const uint64_t t = nr_pool_now_ns();
      if (!age || t >= w.t_enq + age || g_test_clock) {
        pthread_cond_wait(&tl_cv, &p->mu);
        continue;
      }
      const uint64_t at = w.t_enq + age;
      const struct timespec ts = {(time_t)(at / 1000000000ull), (long)(at % 1000000000ull)};
      if (pthread_cond_clockwait(&tl_cv, &p->mu, CLOCK_MONOTONIC, &ts) == ETIMEDOUT) dispatch(p, nr_pool_now_ns());
    }
  }
  if (!is_member(p, tl_tid()) && p->nmember < (int)(sizeof(p->member) / sizeof(p->member[0]))) p->member[p->nmember++] = tl_tid(); /* a lessee is a member: enforce() leaves its mask alone */
  pthread_mutex_unlock(&p->mu);
  tl.pool = p;
  tl.tok = tl_last = w.tok;
  tl.prio = prio;
  tl.ul = ul;
  tl.cell = cell;
  tl.deadline = w.deadline;
  tl.t_grant = nr_pool_now_ns();
  pin_to(p, p->core[w.tok]);
  return p->core[w.tok];
}

int nr_pool_acquire(nr_pool_t *p, int prio, int cell, uint64_t dl) { return lease(p, prio, cell, dl, true); }
int nr_pool_try_acquire(nr_pool_t *p, int prio, int cell, uint64_t dl) { return lease(p, prio, cell, dl, false); }
bool nr_pool_holds(const nr_pool_t *p) { return p && tl.pool == p; }

void nr_pool_release(nr_pool_t *p)
{
  if (!p || tl.pool != p) return;
  const uint64_t now = nr_pool_now_ns();
  const int tok = tl.tok, prio = tl.prio, rcell = tl.cell;
  const bool was_ul = tl.ul;
  const bool sticky = prio == NR_POOL_CHAN && p->chan_tok[tok]; /* a reserved token is CHAN's alone: the worker stays on it between blocks */
  tl.pool = NULL;
  tl.tok = -1;
  if (now - tl.t_grant > 20000000ull) { /* a lease held this long means the holder blocked on something: name it (rate-limited) */
    static _Atomic int n_long;
    char nm[20] = "?";
    prctl(PR_GET_NAME, nm, 0, 0, 0);
    if (atomic_fetch_add(&n_long, 1) < 40) fprintf(stderr, "SENSING: CORE_POOL long lease %s class=%d held_ms=%lu\n", nm, prio, (unsigned long)((now - tl.t_grant) / 1000000ull));
  }
  pthread_mutex_lock(&p->mu);
  p->busy[tok] = false;
  p->nfree++;
  p->cnt[prio]--;
  if (prio == NR_POOL_DECODE && !was_ul) p->cnt_dl--, p->cell_dl[rcell]--;
  if (per_cell(prio)) cell_touch(p, rcell, now), p->cell_run[rcell]--;
  p->st.hold_ns[prio] += now - tl.t_grant;
  if (per_cell(prio)) {
    p->st.hold_ns_cell[rcell] += now - tl.t_grant;
    p->st.hold_ns_cell_cls[rcell][prio == NR_POOL_DECODE && was_ul ? NR_POOL_CLS_UL : prio] += now - tl.t_grant;
  }
  p->st.busy_ns += now - tl.t_grant;
  dispatch(p, now);
  pthread_mutex_unlock(&p->mu);
  if (!(prio == NR_POOL_CHAN && sticky)) go_home(p); /* a wake-up from now on lands on the home lane, not on a core that is somebody else's */
}

bool nr_pool_should_yield(const nr_pool_t *p, int prio)
{
  if (!p) return false;
  for (int i = 0; i < NR_POOL_NPRIO; i++)
    if (urgency(i) < urgency(prio) && atomic_load_explicit(&((nr_pool_t *)p)->nwait[i], memory_order_relaxed) > 0) return true;
  return false;
}

bool nr_pool_yield(nr_pool_t *p)
{
  if (!p || tl.pool != p || !nr_pool_should_yield(p, tl.prio)) return false;
  const int prio = tl.prio | (tl.ul ? NR_POOL_F_UL : 0), cell = tl.cell;
  const uint64_t dl = tl.deadline;
  nr_pool_release(p);
  pthread_mutex_lock(&p->mu);
  p->st.yields++;
  pthread_mutex_unlock(&p->mu);
  nr_pool_acquire(p, prio, cell, dl);
  return true;
}

void nr_pool_stats(nr_pool_t *p, nr_pool_stats_t *out)
{
  if (!p || !out) return;
  pthread_mutex_lock(&p->mu);
  *out = p->st;
  pthread_mutex_unlock(&p->mu);
}

static uint64_t hist_q_us(const uint64_t *h, double q)
{
  uint64_t tot = 0, c = 0;
  for (int b = 0; b < NR_POOL_WAIT_BINS; b++) tot += h[b];
  if (!tot) return 0;
  for (int b = 0; b < NR_POOL_WAIT_BINS; b++) {
    c += h[b];
    if ((double)c >= q * (double)tot) return b == 0 ? 1 : (1ull << b);
  }
  return 1ull << (NR_POOL_WAIT_BINS - 1);
}

static const char *const k_cls_nm[NR_POOL_NPRIO] = {"scan", "decode", "retry", "disc", "house", "chan"};

/* the process-wide part: header + one block per class */
static int pool_line_proc(const nr_pool_stats_t *sp, const nr_pool_t *p, char *buf, int len)
{
  const nr_pool_stats_t s = *sp;
  int o = snprintf(buf, (size_t)len, "SENSING: CORE_POOL n=%d yields=%lu gpu_gives=%lu nested=%lu enforced=%lu", p ? p->n : 0, (unsigned long)s.yields,
                (unsigned long)s.gpu_gives, (unsigned long)s.nested, (unsigned long)s.enforced);
  for (int i = 0; i < NR_POOL_NPRIO && o < len; i++) {
    if (i == NR_POOL_CHAN && !s.leases[i]) continue; /* no channelizer: the line is the pre-CHAN one */
    o += snprintf(buf + o, (size_t)(len - o), " %s[n=%lu waited=%lu promo=%lu wait_us p50<=%lu p99<=%lu max=%lu]", k_cls_nm[i],
                  (unsigned long)s.leases[i], (unsigned long)s.waited[i], (unsigned long)s.promoted[i],
                  (unsigned long)hist_q_us(s.wait_hist[i], 0.5), (unsigned long)hist_q_us(s.wait_hist[i], 0.99),
                  (unsigned long)(s.wait_max_ns[i] / 1000));
  }
  return o;
}

/* one per-cell block, " cell<c>[n=<leases> waited=<waited> hold_ms=<token-time> wait_us p50<=<> p99<=<> max=<>]" */
static int pool_block_cell(const nr_pool_stats_t *s, int c, char *buf, int len)
{
  return snprintf(buf, (size_t)len, " cell%d[n=%lu waited=%lu hold_ms=%lu wait_us p50<=%lu p99<=%lu max=%lu]", c, (unsigned long)s->leases_cell[c],
                  (unsigned long)s->waited_cell[c], (unsigned long)(s->hold_ns_cell[c] / 1000000ull), (unsigned long)hist_q_us(s->wait_hist_cell[c], 0.5),
                  (unsigned long)hist_q_us(s->wait_hist_cell[c], 0.99), (unsigned long)(s->wait_max_ns_cell[c] / 1000));
}

int nr_pool_stats_line(nr_pool_t *p, char *buf, int len)
{
  nr_pool_stats_t s;
  nr_pool_stats(p, &s);
  int o = pool_line_proc(&s, p, buf, len);
  /* Per-cell blocks, one per cell that has leased, only once a cell other than 0 has: with cell 0 alone the line is byte-identical to the
   * single-cell pool. Grammar: " cell<c>[n=<leases> waited=<waited> hold_ms=<token-time> wait_us p50<=<> p99<=<> max=<>]". */
  bool multi = false;
  for (int c = 1; c < NR_POOL_MAX_CELLS; c++) multi |= s.leases_cell[c] != 0;
  for (int c = 0; multi && c < NR_POOL_MAX_CELLS && o < len; c++) {
    if (!s.leases_cell[c]) continue;
    o += pool_block_cell(&s, c, buf + o, len - o);
  }
  return o;
}

int nr_pool_stats_line_proc(nr_pool_t *p, char *buf, int len)
{
  nr_pool_stats_t s;
  nr_pool_stats(p, &s);
  return pool_line_proc(&s, p, buf, len);
}

int nr_pool_stats_line_cell(nr_pool_t *p, int cell, int pci, char *buf, int len)
{
  if (cell < 0 || cell >= NR_POOL_MAX_CELLS) return 0;
  nr_pool_stats_t s;
  nr_pool_stats(p, &s);
  if (!s.leases_cell[cell]) return 0;
  int o = snprintf(buf, (size_t)len, "SENSING: CORE_POOL cell=%d pci=%d", cell, pci);
  if (o < len) o += pool_block_cell(&s, cell, buf + o, len - o);
  return o;
}

/* P1 CELL_CPU. "SENSING: CELL_CPU cell=<k> pci=<pci> win_ms=<ms> tokens_avg=<hold/win> share_pct=<cell hold / pool hold> fair_pct=<100/cells>
 * wait_p99_us=<cell, window> n=<leases in window> scan[hold_ms= n=] decode[..] retry[..] disc[..] house[..] ul[..]"; all values are window deltas of the cumulative pool counters. */
int nr_pool_cell_cpu_line(nr_pool_t *p, int cell, int pci, int n_cells, uint64_t now_ns, nr_pool_cpu_win_t *w, char *buf, int len)
{
  if (!p || !w || n_cells <= 1 || cell < 0 || cell >= NR_POOL_MAX_CELLS || len <= 0) return 0;
  nr_pool_stats_t s;
  nr_pool_stats(p, &s);
  nr_pool_cpu_win_t cur;
  memset(&cur, 0, sizeof(cur));
  cur.t_ns = now_ns;
  cur.busy_ns = s.busy_ns;
  for (int i = 0; i < NR_POOL_NCLS; i++) cur.cls_hold_ns[i] = s.hold_ns_cell_cls[cell][i], cur.cls_leases[i] = s.leases_cell_cls[cell][i];
  cur.cell_hold_ns = s.hold_ns_cell[cell];
  cur.cell_leases = s.leases_cell[cell];
  cur.cell_waited = s.waited_cell[cell];
  memcpy(cur.wait_hist, s.wait_hist_cell[cell], sizeof(cur.wait_hist));
  cur.primed = 1;
  const nr_pool_cpu_win_t prev = *w;
  *w = cur;
  if (!prev.primed || now_ns <= prev.t_ns) return 0; /* the first trigger only opens the window */
  const uint64_t win_ns = now_ns - prev.t_ns, hold = cur.cell_hold_ns - prev.cell_hold_ns, pool = cur.busy_ns - prev.busy_ns;
  uint64_t dh[NR_POOL_WAIT_BINS];
  for (int b = 0; b < NR_POOL_WAIT_BINS; b++) dh[b] = cur.wait_hist[b] - prev.wait_hist[b];
  int o = snprintf(buf, (size_t)len, "SENSING: CELL_CPU cell=%d pci=%d win_ms=%lu tokens_avg=%.2f share_pct=%.1f fair_pct=%.1f wait_p99_us=%lu n=%lu", cell, pci,
                   (unsigned long)(win_ns / 1000000ull), (double)hold / (double)win_ns, pool ? 100.0 * (double)hold / (double)pool : 0.0, 100.0 / (double)n_cells,
                   (unsigned long)hist_q_us(dh, 0.99), (unsigned long)(cur.cell_leases - prev.cell_leases));
  for (int i = 0; i < NR_POOL_NCLS && o < len; i++) {
    if (i == NR_POOL_CHAN) continue; /* the channelizer charges no cell (per_cell()): no per-cell column, the CELL_CPU grammar is the pre-CHAN one */
    o += snprintf(buf + o, (size_t)(len - o), " %s[hold_ms=%lu n=%lu]", i == NR_POOL_CLS_UL ? "ul" : k_cls_nm[i],
                  (unsigned long)((cur.cls_hold_ns[i] - prev.cls_hold_ns[i]) / 1000000ull), (unsigned long)(cur.cls_leases[i] - prev.cls_leases[i]));
  }
  return o < len ? o : len - 1;
}

uint64_t nr_pool_cell_hold_ns(nr_pool_t *p, int cell)
{
  if (!p || cell < 0 || cell >= NR_POOL_MAX_CELLS) return 0;
  pthread_mutex_lock(&p->mu);
  const uint64_t h = p->st.hold_ns_cell[cell];
  pthread_mutex_unlock(&p->mu);
  return h;
}

/* ---- exclusivity (header (a)) ---- */
static bool is_member(nr_pool_t *p, pid_t tid)
{
  for (int i = 0; i < p->nmember; i++)
    if (p->member[i] == tid) return true;
  return false;
}

void nr_pool_member_enter(nr_pool_t *p)
{
  if (!p) return;
  pthread_mutex_lock(&p->mu);
  const pid_t tid = tl_tid();
  if (!is_member(p, tid) && p->nmember < (int)(sizeof(p->member) / sizeof(p->member[0]))) p->member[p->nmember++] = tid;
  pthread_mutex_unlock(&p->mu);
  tl.cur_core = 0; /* unknown: make go_home act */
  go_home(p);
}

int nr_pool_enforce(nr_pool_t *p)
{
  if (!p) return 0;
  DIR *d = opendir("/proc/self/task");
  if (!d) return 0;
  int changed = 0;
  struct dirent *e;
  while ((e = readdir(d))) {
    char *end;
    const long tid = strtol(e->d_name, &end, 10);
    if (end == e->d_name || *end || tid <= 0 || tid > INT_MAX) continue;
    pthread_mutex_lock(&p->mu);
    const bool m = is_member(p, (pid_t)tid);
    pthread_mutex_unlock(&p->mu);
    if (m) continue;
    cpu_set_t cur, nw;
    if (sched_getaffinity((pid_t)tid, sizeof(cur), &cur)) continue;
    CPU_ZERO(&nw);
    bool hit = false;
    for (int c = 0; c < CPU_SETSIZE; c++)
      if (CPU_ISSET(c, &cur)) {
        if (CPU_ISSET(c, &p->poolset)) hit = true;
        else CPU_SET(c, &nw);
      }
    if (!hit) continue;
    if (CPU_COUNT(&nw) == 0) nw = p->home; /* pinned to pool cores only: the home lane */
    if (CPU_COUNT(&nw) > 0 && sched_setaffinity((pid_t)tid, sizeof(nw), &nw) == 0) changed++;
  }
  closedir(d);
  if (changed) {
    pthread_mutex_lock(&p->mu);
    p->st.enforced += (uint64_t)changed;
    pthread_mutex_unlock(&p->mu);
  }
  return changed;
}

extern int nr_core_inherited_mask(cpu_set_t *) __attribute__((weak));
/* ---- site pool ---- */
static nr_pool_t *g_site;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void add_list(const char *e, int *v, int *n)
{
  while (e && *e && *n < NR_POOL_MAX_TOKENS) {
    char *end;
    long a = strtol(e, &end, 10), b = a;
    if (end == e) break;
    if (*end == '-') {
      e = end + 1;
      b = strtol(e, &end, 10);
    }
    for (long c = a; c <= b && *n < NR_POOL_MAX_TOKENS && c < CPU_SETSIZE; c++) {
      bool dup = false;
      for (int i = 0; i < *n; i++) dup |= v[i] == c;
      if (!dup && c >= 0) v[(*n)++] = (int)c;
    }
    e = (*end == ',') ? end + 1 : end;
  }
}

/* The pool cores = the site core map: the PDSCH and PUSCH consumer lists (defaults built in, nr_env_core), minus the RT receive thread,
 * radio ring and UHD control cores, and minus cores the process may not use. The scan consumers no longer have cores of their own. */
static void site_init(void)
{
  int cores[NR_POOL_MAX_TOKENS], cap[NR_POOL_MAX_TOKENS], n = 0, ex[NR_POOL_MAX_TOKENS], nex = 0;
  add_list(nr_env_core("ISAC_PDSCH_CORES"), cores, &n);
  add_list(nr_env_core("ISAC_PUSCH_CORES"), cores, &n);
  add_list(getenv("ISAC_UE_RT_CORE"), ex, &nex);
  add_list(getenv("ISAC_USRP_RING_CORE"), ex, &nex);
  add_list(nr_env_core("ISAC_UHD_CTRL_CORES"), ex, &nex);
  cpu_set_t allowed; /* the mask before the float lane moved the main thread: the cores this process may really use */
  const bool have_allowed = (nr_core_inherited_mask && nr_core_inherited_mask(&allowed)) || sched_getaffinity(0, sizeof(allowed), &allowed) == 0;
  int k = 0;
  for (int i = 0; i < n; i++) {
    bool drop = have_allowed && !CPU_ISSET(cores[i], &allowed);
    for (int j = 0; j < nex; j++) drop |= ex[j] == cores[i];
    if (drop) {
      fprintf(stderr, "SENSING: CORE_POOL: core %d not usable (RT/IO core or outside the allowed set), dropped\n", cores[i]);
      continue;
    }
    cores[k++] = cores[i];
  }
  n = k;
  if (n < 3 && have_allowed) { /* a host smaller than (or only partly overlapping) the site map: every allowed core that is not RT/IO, minus one */
    int fb[NR_POOL_MAX_TOKENS], nf = 0;
    for (int c = 0; c < CPU_SETSIZE && nf < NR_POOL_MAX_TOKENS; c++) {
      bool drop = !CPU_ISSET(c, &allowed);
      for (int j = 0; j < nex; j++) drop |= ex[j] == c;
      if (!drop) fb[nf++] = c;
    }
    if (nf > 1) nf--; /* keep one core for everything else */
    if (nf > n) memcpy(cores, fb, (size_t)nf * sizeof(int)), n = nf;
  }
  if (n == 0) return;
  for (int i = 0; i < n; i++) {
    char path[96];
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpu_capacity", cores[i]);
    FILE *f = fopen(path, "r");
    cap[i] = 0;
    if (f) {
      if (fscanf(f, "%d", &cap[i]) != 1) cap[i] = 0;
      fclose(f);
    }
  }
  g_site = nr_pool_create(cores, cap, n, NULL);
  if (g_site) {
    if (have_allowed) { /* home = allowed minus pool minus RT/ring/UHD cores (not whatever mask the first caller had) */
      cpu_set_t h = allowed;
      for (int c = 0; c < CPU_SETSIZE; c++)
        if (CPU_ISSET(c, &g_site->poolset)) CPU_CLR(c, &h);
      for (int j = 0; j < nex; j++)
        if (ex[j] < CPU_SETSIZE) CPU_CLR(ex[j], &h);
      if (CPU_COUNT(&h) > 0) g_site->home = h, g_site->have_home = true;
    }
    char l[256];
    int o = snprintf(l, sizeof(l), "SENSING: CORE_POOL: %d cores:", n);
    for (int i = 0; i < n && o < (int)sizeof(l) - 8; i++) o += snprintf(l + o, sizeof(l) - (size_t)o, " %d", cores[i]);
    fprintf(stderr, "%s\n", l);
    nr_pool_enforce(g_site);
  }
}

static _Atomic bool g_site_test; /* unit tests only: g_site was injected */
void nr_pool_site_set_for_test(nr_pool_t *p)
{
  g_site = p;
  atomic_store(&g_site_test, p != NULL);
}
nr_pool_t *nr_pool_site(void)
{
  if (atomic_load(&g_site_test)) return g_site;
  pthread_once(&g_once, site_init);
  return g_site;
}
int nr_pool_site_acquire(int prio, int cell, uint64_t dl) { return nr_pool_acquire(nr_pool_site(), prio, cell, dl); }
int nr_pool_site_try_acquire(int prio, int cell, uint64_t dl) { return nr_pool_try_acquire(nr_pool_site(), prio, cell, dl); }
void nr_pool_site_release(void) { if (g_site) nr_pool_release(g_site); }
bool nr_pool_site_yield(void) { return g_site && nr_pool_yield(g_site); }
void nr_pool_site_member_enter(void) { nr_pool_member_enter(nr_pool_site()); }
bool nr_pool_float_merged_n(int tokens) { return tokens >= NR_POOL_FLOAT_MERGE_MIN_TOKENS; }
bool nr_pool_site_float_merged(void)
{
  nr_pool_t *p = nr_pool_site();
  return p && nr_pool_float_merged_n(nr_pool_size(p));
}
int nr_pool_site_float_acquire(int prio, int cell, uint64_t enq_ns)
{
  if (!nr_pool_site_float_merged()) return -1;
  if (prio != NR_POOL_DISC && prio != NR_POOL_HOUSE) prio = NR_POOL_HOUSE; /* floats are never above DISC */
  return nr_pool_acquire(g_site, prio, cell, enq_ns);
}
void nr_pool_site_float_release(void) { nr_pool_site_release(); }
bool nr_pool_site_float_yield(void) { return nr_pool_site_float_merged() && nr_pool_yield(g_site); }
int nr_pool_site_chan_acquire(int blocking)
{
  nr_pool_t *p = nr_pool_site();
  if (!p) return -1;
  return blocking ? nr_pool_acquire(p, NR_POOL_CHAN, 0, 0) : nr_pool_try_acquire(p, NR_POOL_CHAN, 0, 0);
}
void nr_pool_site_chan_release(void) { nr_pool_site_release(); }
void nr_pool_site_chan_set_demand(int tokens) { nr_pool_set_chan_demand(nr_pool_site(), tokens); }
int nr_pool_site_chan_max(void) { return nr_pool_chan_cap(nr_pool_site()); }
int nr_pool_site_filter_core(int core)
{
  if (core >= 0 && g_site && nr_pool_core_in_pool(g_site, core)) {
    static _Atomic int logged;
    if (!atomic_exchange(&logged, 1)) fprintf(stderr, "SENSING: CORE_POOL: thread pinned to pool core %d moved to the home lane\n", core);
    return -1;
  }
  return core;
}

static __thread int tl_give;
void nr_pool_site_gpu_give_begin(void) { tl_give++; }
void nr_pool_site_gpu_give_end(void) { if (tl_give > 0) tl_give--; }

/* Would some waiter be granted the token the caller releases now? (same caps as dispatch; the caller's own class count is lowered first) */
static bool released_token_wanted(nr_pool_t *p)
{
  const uint64_t now = nr_pool_now_ns();
  const int prio = tl.prio;
  const bool dl = prio == NR_POOL_DECODE && !tl.ul;
  bool want = false;
  pthread_mutex_lock(&p->mu);
  p->nfree++, p->cnt[prio]--, p->cnt_dl -= dl, p->cell_dl[tl.cell] -= dl;
  for (const waiter_t *q = p->wq_head; q && !want; q = q->next) want = class_ok(p, q->prio, overdue(p, q, now), q->ul, q->cell, now);
  p->nfree--, p->cnt[prio]++, p->cnt_dl += dl, p->cell_dl[tl.cell] += dl;
  pthread_mutex_unlock(&p->mu);
  return want;
}

void nr_pool_gpu_wait(nr_pool_t *p)
{
  const struct timespec ts = {0, 20000};
  if (!tl_give || !p || tl.pool != p || nr_pool_total_waiting(p) == 0 || !released_token_wanted(p)) {
    nanosleep(&ts, NULL);
    return;
  }
  nr_pool_park_t pk;
  nr_pool_park(p, &pk);
  pthread_mutex_lock(&p->mu);
  p->st.gpu_gives++;
  pthread_mutex_unlock(&p->mu);
  nanosleep(&ts, NULL);
  nr_pool_unpark(p, &pk);
}

void nr_pool_park(nr_pool_t *p, nr_pool_park_t *pk)
{
  pk->held = p && tl.pool == p;
  if (!pk->held) return;
  pk->prio = tl.prio | (tl.ul ? NR_POOL_F_UL : 0);
  pk->cell = tl.cell;
  pk->deadline = tl.deadline;
  nr_pool_release(p);
}
void nr_pool_unpark(nr_pool_t *p, const nr_pool_park_t *pk)
{
  if (pk->held) nr_pool_acquire(p, pk->prio, pk->cell, pk->deadline);
}

void nr_pool_note_scan_extra(nr_pool_t *p)
{
  if (!p || tl.pool != p || tl.prio != NR_POOL_SCAN) return;
  pthread_mutex_lock(&p->mu);
  p->st.nested++;
  pthread_mutex_unlock(&p->mu);
}
void nr_pool_site_gpu_wait(void) { nr_pool_gpu_wait(g_site); }
void nr_pool_site_park(nr_pool_park_t *pk) { nr_pool_park(g_site, pk); }
void nr_pool_site_unpark(const nr_pool_park_t *pk) { nr_pool_unpark(g_site, pk); }
void nr_pool_site_note_scan_extra(void) { nr_pool_note_scan_extra(g_site); }

bool nr_pool_site_home_attr(pthread_attr_t *a)
{
  nr_pool_t *p = g_site;
  return p && tl.pool == p && p->have_home && pthread_attr_setaffinity_np(a, sizeof(p->home), &p->home) == 0;
}

void nr_pool_site_enforce_tick(void)
{
  static _Atomic uint64_t next;
  const uint64_t now = nr_pool_now_ns();
  uint64_t n = atomic_load_explicit(&next, memory_order_relaxed);
  if (now < n || !g_site || !atomic_compare_exchange_strong(&next, &n, now + 1000000000ull)) return;
  nr_pool_enforce(g_site);
}

int nr_pool_free(const nr_pool_t *p) { return p ? __atomic_load_n(&p->nfree, __ATOMIC_RELAXED) : 0; }
int nr_pool_site_free(void) { return g_site ? nr_pool_free(g_site) : -1; }
