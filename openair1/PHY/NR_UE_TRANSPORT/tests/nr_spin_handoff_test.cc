// SPDX-License-Identifier: LicenseRef-CSSL-1.0
// nr_spin_handoff.h: predictor arithmetic, and a two-thread bench of the exact consumer loop used by the scan and PDSCH queues
// (cond_timedwait baseline vs timer-then-spin). Cores 1 (producer) and 2 (consumer): both inside the 0-4 bench set.
#include <gtest/gtest.h>
#include <pthread.h>
#include <sched.h>
#include <sys/prctl.h>
#include <algorithm>
#include <atomic>
#include <vector>
#include <cstdio>
#include "../nr_spin_handoff.h"

TEST(SpinHandoff, PredictsNextSlotAfterAnchor)
{
  nr_spin_pred_t p;
  std::atomic<long> anchor{1000000};
  nr_spin_pred_init(&p, 500000, &anchor);
  EXPECT_EQ(nr_spin_pred_next(&p, 900000, 10000), 1000000u);          // anchored slot still ahead
  EXPECT_EQ(nr_spin_pred_next(&p, 1005000, 10000), 1000000u);         // just passed, within the tail
  EXPECT_EQ(nr_spin_pred_next(&p, 1100000, 10000), 1500000u);         // next period
  EXPECT_EQ(nr_spin_pred_next(&p, 3300000, 10000), 3500000u);
  atomic_store(&p.phase_ns, 200000);                                  // work shows up 200 us after the slot
  EXPECT_EQ(nr_spin_pred_next(&p, 1100000, 10000), 1200000u);
  EXPECT_EQ(nr_spin_pred_next(&p, 1300000, 10000), 1700000u);
}

TEST(SpinHandoff, PhaseLearningAndUnknown)
{
  nr_spin_pred_t p;
  std::atomic<long> anchor{0};
  nr_spin_pred_init(&p, 500000, &anchor);
  EXPECT_EQ(nr_spin_pred_next(&p, 123, 10), 0u);                      // no anchor, no work seen: unknown
  atomic_store(&anchor, 1000000);
  for (int i = 0; i < 40; i++) nr_spin_pred_note(&p, 1000000 + 100000);
  EXPECT_NEAR((double)atomic_load(&p.phase_ns), 100000.0, 1.0);
}

TEST(SpinHandoff, PlanSleepsThenSpinsThenBlocks)
{
  nr_spin_pred_t p; nr_spin_acct_t a;
  std::atomic<long> anchor{0};
  nr_spin_pred_init(&p, 500000, &anchor);
  nr_spin_acct_init(&a);
  const uint64_t now = nr_spin_now_ns();
  atomic_store(&anchor, (long)(now + 400000));
  a.credit_ns = 1000000;
  uint64_t until = 0;
  EXPECT_EQ(nr_spin_plan(&p, &a, now, &until), NR_SPIN_SLEEP);        // far from the window: sleep until it opens
  EXPECT_LT(until, now + 400000);
  EXPECT_EQ(nr_spin_plan(&p, &a, now + 395000, &until), NR_SPIN_SPIN);
  EXPECT_EQ(nr_spin_plan(&p, &a, now + 395000, &until), NR_SPIN_BLOCK); // claim held: a second consumer does not spin
  atomic_store(&p.claim, 0);
  a.credit_ns = 0;
  EXPECT_EQ(nr_spin_plan(&p, &a, now + 395000, &until), NR_SPIN_BLOCK); // no credit: no spin
}

// ---------------------------------------------------------------- two-thread bench
namespace {
struct Q {
  pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
  pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
  int count = 0;
  nr_spin_pred_t pred;
  std::atomic<long> anchor{0};
  std::atomic<uint64_t> stamp{0};
  std::atomic<int> stop{0};
  bool hybrid = false;
  int n = 0;
  std::vector<double> lat_us;
  double cpu_frac = 0, spin_frac = 0;
  uint64_t hits = 0, misses = 0;
  uint64_t period = 500000, phase = 0, jitter = 0;
};
static void pin(int c) { cpu_set_t s; CPU_ZERO(&s); CPU_SET(c, &s); sched_setaffinity(0, sizeof s, &s); }
static uint64_t thr_cpu_ns() { timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }

static void *consumer(void *arg)
{
  Q *q = (Q *)arg;
  pin(2);
  prctl(PR_SET_TIMERSLACK, 1UL);
  nr_spin_acct_t acct;
  nr_spin_acct_init(&acct);
  const uint64_t c0 = thr_cpu_ns(), w0 = nr_spin_now_ns();
  int got = 0;
  while (got < q->n) {
    pthread_mutex_lock(&q->mu);
    while (q->count == 0 && !atomic_load(&q->stop)) {
      timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      uint64_t until = 0;
      const nr_spin_action_t act = q->hybrid ? nr_spin_plan(&q->pred, &acct, nr_spin_now_ns(), &until) : NR_SPIN_BLOCK;
      if (act == NR_SPIN_SPIN) {
        pthread_mutex_unlock(&q->mu);
        nr_spin_run(&q->pred, &acct, &q->count, until);
        pthread_mutex_lock(&q->mu);
        continue;
      }
      if (act == NR_SPIN_SLEEP && until - nr_spin_now_ns() < 20000000ull) {
        ts.tv_sec = (time_t)(until / 1000000000ull); ts.tv_nsec = (long)(until % 1000000000ull);
        pthread_cond_timedwait(&q->cv, &q->mu, &ts);
        nr_spin_note_wake(&acct, until, nr_spin_now_ns());
        continue;
      }
      ts.tv_nsec += 20 * 1000 * 1000;
      if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
      pthread_cond_timedwait(&q->cv, &q->mu, &ts);
    }
    if (q->count) {
      q->count--;
      const uint64_t t = nr_spin_now_ns();
      if (got >= 100) q->lat_us.push_back((double)(t - atomic_load(&q->stamp)) / 1000.0);
      got++;
    }
    pthread_mutex_unlock(&q->mu);
  }
  q->cpu_frac = (double)(thr_cpu_ns() - c0) / (double)(nr_spin_now_ns() - w0);
  q->spin_frac = (double)acct.spun_ns / (double)(nr_spin_now_ns() - w0);
  q->hits = acct.hits; q->misses = acct.misses;
  return nullptr;
}
static void *producer(void *arg)
{
  Q *q = (Q *)arg;
  pin(1);
  uint64_t rng = 12345;
  uint64_t slot = nr_spin_now_ns() + 5000000;
  for (int i = 0; i < q->n; i++, slot += q->period) {
    timespec ts{(time_t)(slot / 1000000000ull), (long)(slot % 1000000000ull)};
    clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &ts, nullptr);   // the slot arrives (RX thread read returns)
    atomic_store(&q->anchor, (long)slot);
    uint64_t when = slot + q->phase;
    if (q->jitter) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; when += rng % q->jitter; }
    if (when > slot) {
      timespec t2{(time_t)(when / 1000000000ull), (long)(when % 1000000000ull)};
      clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &t2, nullptr);
    }
    pthread_mutex_lock(&q->mu);
    q->count++;
    atomic_store(&q->stamp, nr_spin_now_ns());
    nr_spin_pred_note(&q->pred, nr_spin_now_ns());
    pthread_mutex_unlock(&q->mu);
    pthread_cond_signal(&q->cv);
  }
  return nullptr;
}
static void run(Q &q, bool hybrid, uint64_t phase, uint64_t jitter, int n = 3000)
{
  q.hybrid = hybrid; q.n = n; q.phase = phase; q.jitter = jitter;
  nr_spin_pred_init(&q.pred, q.period, &q.anchor);
  pthread_t c, p;
  pthread_create(&c, nullptr, consumer, &q);
  pthread_create(&p, nullptr, producer, &q);
  pthread_join(p, nullptr);
  atomic_store(&q.stop, 1);
  pthread_cond_broadcast(&q.cv);
  pthread_join(c, nullptr);
  std::sort(q.lat_us.begin(), q.lat_us.end());
}
static double pct(const Q &q, double p) { return q.lat_us.empty() ? -1 : q.lat_us[(size_t)(p * (q.lat_us.size() - 1))]; }
}  // namespace

// scan queue shape: work lands at the slot arrival (phase 0). pdsch queue shape: lands ~250 us later, +-100 us jitter.
TEST(SpinHandoffBench, WakeLatencyAndCoreCost)
{
  const struct { const char *name; uint64_t phase, jitter; } shapes[] = {{"scan(phase 0)", 0, 0}, {"pdsch(phase 250us, jitter 100us)", 250000, 100000}};
  for (auto &sh : shapes) {
    Q base, hyb;
    run(base, false, sh.phase, sh.jitter);
    run(hyb, true, sh.phase, sh.jitter);
    printf("%-34s cond:   p50 %6.1f p90 %6.1f p99 %6.1f us | cpu %.1f%% of the core\n", sh.name, pct(base, .5), pct(base, .9), pct(base, .99), 100 * base.cpu_frac);
    printf("%-34s hybrid: p50 %6.1f p90 %6.1f p99 %6.1f us | cpu %.1f%% (spin %.1f%%) hits %llu misses %llu\n", "", pct(hyb, .5), pct(hyb, .9), pct(hyb, .99),
           100 * hyb.cpu_frac, 100 * hyb.spin_frac, (unsigned long long)hyb.hits, (unsigned long long)hyb.misses);
    EXPECT_LE(hyb.spin_frac, 0.08) << sh.name;                      // the budget
    EXPECT_LE(hyb.cpu_frac - base.cpu_frac, 0.08) << sh.name;       // total extra core cost
    EXPECT_LE(pct(hyb, .99), pct(base, .99) * 1.5 + 20) << sh.name; // never worse than blocking by more than noise
  }
}
