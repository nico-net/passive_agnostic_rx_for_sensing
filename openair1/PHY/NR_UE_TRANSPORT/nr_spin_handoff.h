/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/*
 * Timer-then-spin hand-off for the PDCCH critical path (RX/scan -> scan consumer -> PDSCH queue).
 *
 * WHY. A consumer blocked in pthread_cond_timedwait() is woken by a futex + scheduler wake: ~100 us p50 on a cross-core hand-off
 * (tests/pdcch_redesign/results/handoff_bench_cpu1_cpu2.txt). The work of a slot queue arrives at a predictable time (slot period P,
 * anchored on the slot arrival the RX thread already publishes), so the waiting thread sleeps until shortly before that time, spins on
 * the queue's count for a bounded window, and only then blocks as before. A producer's enqueue is unchanged (it still signals), so a
 * consumer that is not spinning is woken exactly as today.
 *
 * BOUNDS. Per waiting thread a token bucket earns NR_SPIN_BUDGET_NUM/DEN (6 %) of wall time in spin credit, capped at one window; a spin
 * consumes credit and ends when it is gone. The long-run spin share of every involved core is therefore <= 6 % (the 8 % ceiling less
 * the clock reads). At most one thread per queue spins at a time (claim), so N idle consumers do not each burn the budget.
 * No thread, no core, no scheduling class is added. The sleep part uses the consumer's normal timed wait.
 * Clock: CLOCK_REALTIME ns, the same base as nr_ue_diag_producer_wall_ns and the queues' cond waits.
 */
#ifndef NR_SPIN_HANDOFF_H
#define NR_SPIN_HANDOFF_H
#ifndef NR_ATOMIC
#ifdef __cplusplus
#include <atomic>
using std::atomic_compare_exchange_strong;
using std::atomic_compare_exchange_weak;
using std::atomic_fetch_add;
using std::atomic_fetch_add_explicit;
using std::atomic_fetch_sub;
using std::atomic_load;
using std::atomic_load_explicit;
using std::atomic_store;
using std::atomic_store_explicit;
using std::memory_order_acq_rel;
using std::memory_order_acquire;
using std::memory_order_relaxed;
using std::memory_order_release;
#define NR_ATOMIC(t) std::atomic<t>
#else
#include <stdatomic.h>
#define NR_ATOMIC(t) _Atomic(t)
#endif
#endif
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define NR_SPIN_BUDGET_NUM 6   /* percent of wall time a thread may spend spinning */
#define NR_SPIN_LEAD_MIN_NS 15000
#define NR_SPIN_ABS_MAX_NS 600000000ull

static inline uint64_t nr_spin_now_ns(void)
{
  struct timespec t;
  clock_gettime(CLOCK_REALTIME, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* One per queue. period_ns: slot duration. anchor_ns: REALTIME arrival of a recent slot (shared, written by the RX thread, may be NULL
 * -> falls back to the last work arrival). phase_ns: EMA of (work arrival - latest slot arrival), i.e. how long after the slot the
 * queue's work shows up: ~0 for the scan queue, the scan latency for the PDSCH queue. */
typedef struct {
  uint64_t period_ns;
  const NR_ATOMIC(long) *anchor_ns;
  NR_ATOMIC(uint64_t) last_work_ns;
  NR_ATOMIC(uint64_t) phase_ns;
  NR_ATOMIC(int) claim;
} nr_spin_pred_t;

/* One per waiting thread (thread-local or in the thread's frame). */
typedef struct {
  int64_t credit_ns;
  uint64_t last_ns;
  uint64_t lead_ns;          /* EMA-based: how long before the predicted time to wake */
  uint64_t hits, misses, spun_ns, t0_ns;
} nr_spin_acct_t;

static inline void nr_spin_pred_init(nr_spin_pred_t *p, uint64_t period_ns, const NR_ATOMIC(long) *anchor_ns)
{
  p->period_ns = period_ns;
  p->anchor_ns = anchor_ns;
  atomic_store(&p->last_work_ns, 0);
  atomic_store(&p->phase_ns, 0);
  atomic_store(&p->claim, 0);
}
static inline void nr_spin_acct_init(nr_spin_acct_t *a)
{
  a->t0_ns = a->last_ns = nr_spin_now_ns();
  a->credit_ns = 0;
  a->lead_ns = 2 * NR_SPIN_LEAD_MIN_NS;
  a->hits = a->misses = a->spun_ns = 0;
}

static inline uint64_t nr_spin_anchor(const nr_spin_pred_t *p, uint64_t now)
{
  const uint64_t a = p->anchor_ns ? (uint64_t)atomic_load_explicit(p->anchor_ns, memory_order_relaxed) : 0;
  if (a && a <= now + p->period_ns) return a;
  return atomic_load_explicit(&p->last_work_ns, memory_order_relaxed);
}

/* Producer side: work just arrived (called with the enqueue). Learns the phase after the slot arrival. */
static inline void nr_spin_pred_note(nr_spin_pred_t *p, uint64_t now)
{
  const uint64_t a = p->anchor_ns ? (uint64_t)atomic_load_explicit(p->anchor_ns, memory_order_relaxed) : 0;
  if (a && now >= a && now - a < p->period_ns) {
    const uint64_t ph = atomic_load_explicit(&p->phase_ns, memory_order_relaxed);
    atomic_store_explicit(&p->phase_ns, ph ? (ph * 7 + (now - a)) / 8 : now - a, memory_order_relaxed);
  }
  atomic_store_explicit(&p->last_work_ns, now, memory_order_relaxed);
}

/* Predicted arrival of the next work item that is not already in the past by more than `tail`. 0 = unknown. */
static inline uint64_t nr_spin_pred_next(const nr_spin_pred_t *p, uint64_t now, uint64_t tail_ns)
{
  const uint64_t a = nr_spin_anchor(p, now);
  if (!a || !p->period_ns) return 0;
  const uint64_t base = a + atomic_load_explicit(&p->phase_ns, memory_order_relaxed);
  if (base + tail_ns >= now) return base;           /* arrival of the anchored slot still ahead (or just passed) */
  const uint64_t m = (now - base - tail_ns) / p->period_ns + 1;
  return base + m * p->period_ns;
}

static inline void nr_spin_credit_update(nr_spin_acct_t *a, uint64_t now, uint64_t window_ns)
{
  if (now > a->last_ns) {
    a->credit_ns += (int64_t)((now - a->last_ns) * NR_SPIN_BUDGET_NUM / 100);
    a->last_ns = now;
  }
  if (a->credit_ns > (int64_t)window_ns) a->credit_ns = (int64_t)window_ns;
}

typedef enum { NR_SPIN_BLOCK = 0, NR_SPIN_SLEEP = 1, NR_SPIN_SPIN = 2 } nr_spin_action_t;

/* What the idle consumer does next. SLEEP: timed-wait until *until_ns (then call again). SPIN: call nr_spin_run(.., *until_ns) (the
 * claim is held until nr_spin_run returns). BLOCK: ordinary wait. */
static inline nr_spin_action_t nr_spin_plan(nr_spin_pred_t *p, nr_spin_acct_t *a, uint64_t now, uint64_t *until_ns)
{
  const uint64_t tail = p->period_ns / 50; /* arrival jitter allowed after the predicted time */
  const uint64_t pred = nr_spin_pred_next(p, now, tail);
  if (!pred || pred - now > NR_SPIN_ABS_MAX_NS) return NR_SPIN_BLOCK;
  uint64_t lead = a->lead_ns;
  if (lead > p->period_ns / 10) lead = p->period_ns / 10;
  const uint64_t window = lead + tail;
  nr_spin_credit_update(a, now, window);
  if (pred > now + lead + 2000) { /* too early to spin: sleep until the window opens (never longer than the usual 20 ms wait) */
    *until_ns = pred - lead;
    return NR_SPIN_SLEEP;
  }
  if (a->credit_ns <= 0) return NR_SPIN_BLOCK;
  int exp = 0;
  if (!atomic_compare_exchange_strong(&p->claim, &exp, 1)) return NR_SPIN_BLOCK; /* another consumer spins for this queue */
  *until_ns = pred + tail;
  return NR_SPIN_SPIN;
}

static inline void nr_spin_relax(void)
{
#if defined(__aarch64__)
  __asm__ __volatile__("yield" ::: "memory");
#elif defined(__x86_64__)
  __asm__ __volatile__("pause" ::: "memory");
#endif
}

/* Spin until *count != 0, the window ends, or the credit is spent. Releases the claim. true: work arrived while spinning. */
static inline bool nr_spin_run(nr_spin_pred_t *p, nr_spin_acct_t *a, const int *count, uint64_t until_ns)
{
  const uint64_t t0 = nr_spin_now_ns();
  uint64_t now = t0;
  bool hit = false;
  for (unsigned i = 0;; i++) {
    if (__atomic_load_n(count, __ATOMIC_ACQUIRE) != 0) { hit = true; break; }
    nr_spin_relax();
    if ((i & 31) == 31) {
      now = nr_spin_now_ns();
      if (now >= until_ns || (int64_t)(now - t0) >= a->credit_ns) break;
    }
  }
  now = nr_spin_now_ns();
  const uint64_t spent = now - t0;
  a->credit_ns -= (int64_t)spent;
  a->spun_ns += spent;
  if (hit) a->hits++; else a->misses++;
  atomic_store_explicit(&p->claim, 0, memory_order_release);
  return hit;
}

/* The sleep step woke at `woke_ns` for a target of `target_ns`: adapt the lead to the timer's real lateness. */
static inline void nr_spin_note_wake(nr_spin_acct_t *a, uint64_t target_ns, uint64_t woke_ns)
{
  const uint64_t late = woke_ns > target_ns ? woke_ns - target_ns : 0;
  uint64_t want = NR_SPIN_LEAD_MIN_NS + 2 * late;
  if (want > 200000) want = 200000;
  a->lead_ns = (a->lead_ns * 3 + want) / 4;
}
#endif
