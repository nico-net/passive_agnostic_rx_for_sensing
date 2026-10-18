#include "common/utils/nr_core_plan.h"
#include "nr_passive_sample_lifetime.h"
/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this
 * file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.openairinterface.org/?page_id=698
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_passive_queue.c
 * \brief Consumer pool for the deferred blind-PDCCH scan. See the header for the BTIM measurement
 * that motivates it and for the admission policy.
 *
 * Deliberately a near-copy of nr_pdsch_passive_queue.c rather than a shared generic queue: the two
 * differ in job payload, in staleness margin rationale and in what a consumer calls, and the amount
 * that would actually be shared is the ~30 lines of ring mechanics. A common abstraction here would
 * couple two lifetimes that are independently tuned.
 */

#include "nr_pdcch_passive_queue.h"
#include "PHY/MODULATION/modulation_UE.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "common/utils/LOG/log.h"
#include "common/utils/system.h"
#include "nr_pdcch_blind_monitor_rt.h"
#include "nr_passive_cfg_epoch.h"
#include "nr_passive_job_epoch.h"

/* Monotonic slot counter published by the RF producer; the same source nr_pdsch_passive_queue uses,
 * so the two staleness checks are on one clock. */
extern _Atomic long nr_ue_diag_producer_absolute_slot;

/* Margin, in slots, held back from the full rxdata wrap. Same value and same reasoning as the PDSCH
 * queue: a job must be finished comfortably before the producer laps it, not on the last slot. */
#define NR_PDCCH_PASSIVE_QUEUE_MARGIN_SLOTS 4

extern int nr_passive_e2e_trace(void);
extern uint64_t nr_passive_slot_arrival_ns(long abs_slot);
static uint64_t scanq_now_ns(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static nr_pdcch_passive_job_t g_ring[NR_PDCCH_PASSIVE_QUEUE_MAX_DEPTH];
static int g_depth = 0;
static int g_head  = 0; // next write slot
static int g_tail  = 0; // next read slot
static int g_count = 0;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv   = PTHREAD_COND_INITIALIZER;

/* SIDE WORK (ISAC_PDCCH_BANK_HANDOFF, nr_pdcch_blind_monitor_rt.c): a consumer running an occasion posts part of it (the later banked
 * CORESET passes); an IDLE consumer of this same pool takes it, so no thread or core is added. The poster reclaims it and runs it
 * itself when no consumer took it by the time its own part is done, so the work never waits for a busy consumer. Under g_lock. */
typedef struct {
  void (*fn)(void *);
  void *arg;
  int state; /* 0 free, 1 posted, 2 taken, 3 done */
} side_work_t;
static side_work_t g_side[NR_PDCCH_PASSIVE_QUEUE_MAX_CONSUMERS];
static int g_side_posted;
static pthread_cond_t g_side_done = PTHREAD_COND_INITIALIZER;
static _Atomic uint64_t g_side_n, g_side_taken;
static int g_nthreads;

static _Atomic int      g_running = 0;
static _Atomic int      g_stop    = 0;
static _Atomic uint64_t g_queued        = 0;
static _Atomic uint64_t g_processed     = 0;
static _Atomic uint64_t g_dropped_full  = 0;
static _Atomic uint64_t g_dropped_stale = 0;
static _Atomic uint64_t g_dropped_epoch = 0;
static _Atomic uint64_t g_max_lag       = 0;

/* P99: lag (producer slot - occasion slot) at dequeue, bins < 2/4/8/12/16/18/24/32/inf slots (nr_pdcch_passive_queue_lag_hist) */
static _Atomic uint64_t g_lag_hist[NR_PASSIVE_LAG_BINS];
static _Atomic uint64_t g_fresh_picks = 0;
int nr_passive_lag_bin(long lag)
{
  static const long edge[NR_PASSIVE_LAG_BINS - 1] = {2, 4, 8, 12, 16, 18, 24, 32};
  int b = 0;
  while (b < NR_PASSIVE_LAG_BINS - 1 && lag >= edge[b])
    b++;
  return b;
}
/* ISAC_SCANQ_FRESH=N (P99, default 0 = off = FIFO unchanged): when >= 2 occasions wait and the oldest is already
 * N or more slots old, serve the NEWEST first. Under a scan backlog FIFO makes every occasion late, so the PDSCH jobs it
 * creates arrive with little of their 18-slot sample lifetime left (born stale); newest-first keeps most of them fresh
 * and lets the oldest ones age out (still counted as scanq drop_stale). */
static int scanq_fresh(void)
{
  static int v = -1;
  if (v < 0) {
    const char *e = (getenv("ISAC_SCANQ_FRESH") ?: "6");
    v = e ? atoi(e) : 0;
    if (v < 0) v = 0;
  }
  return v;
}

static pthread_t g_threads[NR_PDCCH_PASSIVE_QUEUE_MAX_CONSUMERS];
static PHY_VARS_NR_UE *g_ue = NULL;

typedef struct {
  int idx;
} consumer_arg_t;
static consumer_arg_t g_args[NR_PDCCH_PASSIVE_QUEUE_MAX_CONSUMERS];

static void *nr_pdcch_passive_queue_thread(void *arg)
{
  const int idx = ((consumer_arg_t *)arg)->idx;
  PHY_VARS_NR_UE *ue = g_ue;
  const long slots_per_frame = ue->frame_parms.slots_per_frame;

  LOG_I(PHY, "SENSING: blind PDCCH scan consumer %d started\n", idx);

  while (1) {
    nr_pdcch_passive_job_t job;

    pthread_mutex_lock(&g_lock);
    if (g_side_posted > 0) { /* another consumer's posted side work: it is for an occasion already running, run it first */
      for (int k = 0; k < NR_PDCCH_PASSIVE_QUEUE_MAX_CONSUMERS; k++)
        if (g_side[k].state == 1) {
          g_side[k].state = 2;
          g_side_posted--;
          pthread_mutex_unlock(&g_lock);
          atomic_fetch_add_explicit(&g_side_taken, 1, memory_order_relaxed);
          g_side[k].fn(g_side[k].arg);
          pthread_mutex_lock(&g_lock);
          g_side[k].state = 3;
          pthread_cond_broadcast(&g_side_done);
          break;
        }
      pthread_mutex_unlock(&g_lock);
      continue;
    }
    while (g_count == 0 && g_side_posted == 0 && atomic_load_explicit(&g_stop, memory_order_relaxed) == 0) {
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_nsec += 20 * 1000 * 1000; // 20 ms
      if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
      pthread_cond_timedwait(&g_cv, &g_lock, &ts);
    }
    if (g_count == 0 && atomic_load_explicit(&g_stop, memory_order_relaxed) != 0) {
      pthread_mutex_unlock(&g_lock);
      break;
    }
    if (g_count == 0) { /* woken for side work */
      pthread_mutex_unlock(&g_lock);
      continue;
    }
    const int fresh = scanq_fresh();
    if (fresh && g_count >= 2
        && atomic_load_explicit(&nr_ue_diag_producer_absolute_slot, memory_order_relaxed) - g_ring[g_tail].absolute_slot >= fresh) {
      g_head = (g_head + g_depth - 1) % g_depth;
      job    = g_ring[g_head];
      atomic_fetch_add_explicit(&g_fresh_picks, 1, memory_order_relaxed);
    } else {
      job    = g_ring[g_tail];
      g_tail = (g_tail + 1) % g_depth;
    }
    g_count--;
    pthread_mutex_unlock(&g_lock);
    const uint64_t take_ns = job.enq_ns ? scanq_now_ns() : 0;

    NR_CFG_EPOCH_WORK(job.config_epoch, &g_dropped_epoch);
    if (!nr_cfg_epoch_work_current()) {
      continue;
    }

    /* STALENESS. The job carries no samples, so the scan re-FEPs from ue->common_vars.rxdata. Those
     * samples survive only until the producer reaches the same slot index one frame later; past
     * that the scan would demodulate the NEXT frame's IQ and report confident nonsense rather than
     * an error. Dropped and counted, never run late. */
    const long prod = atomic_load_explicit(&nr_ue_diag_producer_absolute_slot, memory_order_relaxed);
    const long lag  = prod - job.absolute_slot;
    /* CAS max: with N consumers a plain load/store pair can lose the larger lag (Task A7). */
    uint64_t cur_max = atomic_load_explicit(&g_max_lag, memory_order_relaxed);
    while (lag > (long)cur_max
           && !atomic_compare_exchange_weak_explicit(&g_max_lag, &cur_max, (uint64_t)lag, memory_order_relaxed,
                                                     memory_order_relaxed))
      ;
    atomic_fetch_add_explicit(&g_lag_hist[nr_passive_lag_bin(lag)], 1, memory_order_relaxed);
    if (!nr_passive_samples_valid(prod, job.absolute_slot, slots_per_frame)) {
      atomic_fetch_add_explicit(&g_dropped_stale, 1, memory_order_relaxed);
      continue;
    }

    /* Replay the offset captured with these samples (see nr_slot_fep_fo_override_hz). */
    const double saved_fo = nr_slot_fep_fo_override_hz;
    nr_slot_fep_fo_override_hz = job.fo_hz;

    UE_nr_rxtx_proc_t proc = {0};
    proc.frame_rx   = job.frame_rx;
    proc.nr_slot_rx = job.nr_slot_rx;
    proc.gNB_id     = job.gNB_id;

    struct timespec e0;
    if (nr_passive_e2e_trace()) clock_gettime(CLOCK_MONOTONIC, &e0);
    nr_pdcch_blind_monitor_run_occasion(ue, &proc, true /* already off the RT thread */, job.absolute_slot);
    if (nr_passive_e2e_trace()) {
      /* SYSAN SCAN_E2E: slot read -> scan start (wait), scan occasion wall (run); 50-us bins, p50/p90/p99 every 4000 occasions */
      static _Atomic uint32_t h[4][101]; /* wait, run, pre (arrival -> enqueue), q (enqueue -> taken: queue sit + wake-up) */
      static _Atomic uint64_t n_occ;
      struct timespec e1;
      clock_gettime(CLOCK_MONOTONIC, &e1);
      const uint64_t t0 = (uint64_t)e0.tv_sec * 1000000000ull + e0.tv_nsec, t1 = (uint64_t)e1.tv_sec * 1000000000ull + e1.tv_nsec;
      const uint64_t arr = nr_passive_slot_arrival_ns(job.absolute_slot);
      if (arr && t0 >= arr) {
        const uint64_t w = (t0 - arr) / 50000, r = (t1 - t0) / 50000;
        atomic_fetch_add(&h[0][w > 100 ? 100 : w], 1);
        atomic_fetch_add(&h[1][r > 100 ? 100 : r], 1);
        if (job.enq_ns >= arr && take_ns >= job.enq_ns) {
          const uint64_t p = (job.enq_ns - arr) / 10000, q = (take_ns - job.enq_ns) / 10000; /* 10 us bins here: the hand-off is tens of us */
          atomic_fetch_add(&h[2][p > 100 ? 100 : p], 1);
          atomic_fetch_add(&h[3][q > 100 ? 100 : q], 1);
        }
        if (atomic_fetch_add(&n_occ, 1) % 4000 == 3999) {
          int q[4][3];
          for (int k = 0; k < 4; k++) {
            uint64_t tot = 0, c = 0;
            for (int b = 0; b <= 100; b++) tot += h[k][b];
            const double f[3] = {0.5, 0.9, 0.99};
            const int bin = k < 2 ? 50 : 10;
            for (int j = 0, b = 0; j < 3; j++) {
              while (b < 100 && c + h[k][b] < f[j] * tot) c += h[k][b++];
              q[k][j] = (b + 1) * bin;
            }
          }
          LOG_A(PHY, "SENSING: SCAN_E2E occ=%lu wait_us p50<=%d p90<=%d p99<=%d run_us p50<=%d p90<=%d p99<=%d "
                     "pre_us p50<=%d p90<=%d p99<=%d q_us p50<=%d p90<=%d p99<=%d\n",
                (unsigned long)atomic_load(&n_occ), q[0][0], q[0][1], q[0][2], q[1][0], q[1][1], q[1][2],
                q[2][0], q[2][1], q[2][2], q[3][0], q[3][1], q[3][2]);
        }
      }
    }
    nr_slot_fep_fo_override_hz = saved_fo;
    atomic_fetch_add_explicit(&g_processed, 1, memory_order_relaxed);
  }

  LOG_I(PHY, "SENSING: blind PDCCH scan consumer %d exiting\n", idx);
  return NULL;
}

/* nr_pdcch_passive_queue_start: starts the blind PDCCH scan pool: sizes the ring (depth clamped to [2, MAX]) and launches n_consumers consumer threads at SCHED_OTHER priority 50, below the RT receive thread.
 *   Idempotent: returns true at once when the pool already runs; false when ue is NULL. Consumers take cores from nr_core_plan_cpu(NR_CP_SCAN) or affinity + index (-1 leaves them unpinned). Several consumers are supported. Caller: nr_pdcch_blind_monitor_rt.c (receive start-up).
 */
bool nr_pdcch_passive_queue_start(PHY_VARS_NR_UE *ue, int depth, int n_consumers, int affinity)
{
  if (atomic_load_explicit(&g_running, memory_order_acquire)) {
    return true;
  }
  if (ue == NULL) {
    return false;
  }
  /* The --cont-fo-comp refusal that stood here is gone: the producer now samples the
   * frequency offset on the receive thread and the consumer replays it through
   * nr_slot_fep_fo_override_hz, so the deferred FEP de-rotates with the offset that
   * belongs to its own samples. Falling back in-line was never benign -- it overran the
   * slot deadline and cost PBCH lock under load. */

  if (n_consumers < 1) n_consumers = 1;
  if (n_consumers > NR_PDCCH_PASSIVE_QUEUE_MAX_CONSUMERS) n_consumers = NR_PDCCH_PASSIVE_QUEUE_MAX_CONSUMERS;
  /* n_consumers > 1 is supported since Task A7: the occasion body's cross-occasion state is either a
   * relaxed atomic counter, under its own leaf lock (energy floor, length sweep, bank, dedupe), or in
   * Phase 2, which runs under one lock (nr_pdcch_blind_phase2.c) -- so Phase 1 (FEP/LLR/demap/decode)
   * is what N consumers parallelise. affinity >= 0 pins consumer i to core affinity + i; -1 leaves every
   * consumer unpinned. */
  if (depth < 2) depth = 2;
  if (depth > NR_PDCCH_PASSIVE_QUEUE_MAX_DEPTH) depth = NR_PDCCH_PASSIVE_QUEUE_MAX_DEPTH;

  g_ue       = ue;
  g_depth    = depth;
  g_head     = 0;
  g_tail     = 0;
  g_count    = 0;
  g_nthreads = n_consumers;
  atomic_store_explicit(&g_stop, 0, memory_order_relaxed);
  atomic_store_explicit(&g_running, 1, memory_order_release);

  for (int i = 0; i < n_consumers; i++) {
    g_args[i].idx = i;
    char name[32];
    snprintf(name, sizeof(name), "passivePdcch%d", i);
    /* Priority below the RT receive thread's 97, same rationale as the PDSCH pool: if a consumer
     * ever competes with the receive path it must lose. Losing here costs one monitoring occasion;
     * losing there costs time sync, which is the whole reason this deferral exists. */
    threadCreate(&g_threads[i], nr_pdcch_passive_queue_thread, &g_args[i], name,
                 nr_core_plan_cpu(NR_CP_SCAN, i, (affinity >= 0) ? (affinity + i) : -1), 50);
  }
  LOG_I(PHY,
        "SENSING: blind PDCCH scan pool: %d consumer(s), depth %d, cores %d..%d, drop-oldest -- "
        "FEP/LLR/demap/candidate-decode no longer run on the PHY receive thread\n",
        n_consumers, depth, affinity, (affinity >= 0) ? affinity + n_consumers - 1 : -1);
  return true;
}

bool nr_pdcch_passive_queue_running(void)
{
  return atomic_load_explicit(&g_running, memory_order_acquire) != 0;
}

bool nr_pdcch_passive_queue_enqueue(const nr_pdcch_passive_job_t *job)
{
  if (!atomic_load_explicit(&g_running, memory_order_acquire)) {
    return false;
  }
  pthread_mutex_lock(&g_lock);
  if (g_count == g_depth) {
    /* DROP-OLDEST: the evicted occasion is the one whose rxdata is closest to being overwritten, so
     * dropping it beats refusing a fresh one. Same policy and same measured reason as the PDSCH
     * queue, where drop-newest produced decoded=58384 / crc_ok=0. */
    g_tail = (g_tail + 1) % g_depth;
    g_count--;
    atomic_fetch_add_explicit(&g_dropped_full, 1, memory_order_relaxed);
  }
  g_ring[g_head] = *job;
  g_ring[g_head].config_epoch = nr_passive_job_epoch_stamp(nr_cfg_reconf_enabled(), nr_cfg_epoch_work_stamp);
  g_ring[g_head].enq_ns = nr_passive_e2e_trace() ? scanq_now_ns() : 0;
  g_head         = (g_head + 1) % g_depth;
  g_count++;
  pthread_mutex_unlock(&g_lock);
  /* Signal AFTER the unlock: signalled under the lock, the woken consumer's first act is to take g_lock, which the producer still
   * holds, so it sleeps a second time (a futex round trip on the wake path, on top of the scheduler wake-up). */
  pthread_cond_signal(&g_cv);
  atomic_fetch_add_explicit(&g_queued, 1, memory_order_relaxed);
  return true;
}

int nr_pdcch_passive_queue_side_post(void (*fn)(void *), void *arg)
{
  if (!atomic_load_explicit(&g_running, memory_order_acquire) || g_nthreads < 2)
    return -1;
  int slot = -1;
  pthread_mutex_lock(&g_lock);
  for (int k = 0; k < NR_PDCCH_PASSIVE_QUEUE_MAX_CONSUMERS && slot < 0; k++)
    if (g_side[k].state == 0) slot = k;
  if (slot >= 0) {
    g_side[slot] = (side_work_t){.fn = fn, .arg = arg, .state = 1};
    g_side_posted++;
    pthread_cond_broadcast(&g_cv);
  }
  pthread_mutex_unlock(&g_lock);
  if (slot >= 0) atomic_fetch_add_explicit(&g_side_n, 1, memory_order_relaxed);
  return slot;
}
void nr_pdcch_passive_queue_side_finish(int slot)
{
  if (slot < 0) return;
  pthread_mutex_lock(&g_lock);
  if (g_side[slot].state == 1) { /* nobody took it: run it here */
    g_side_posted--;
    g_side[slot].state = 0;
    pthread_mutex_unlock(&g_lock);
    g_side[slot].fn(g_side[slot].arg);
    return;
  }
  while (g_side[slot].state == 2) pthread_cond_wait(&g_side_done, &g_lock);
  g_side[slot].state = 0;
  pthread_mutex_unlock(&g_lock);
}
void nr_pdcch_passive_queue_side_stats(uint64_t *posted, uint64_t *taken)
{
  *posted = atomic_load_explicit(&g_side_n, memory_order_relaxed);
  *taken = atomic_load_explicit(&g_side_taken, memory_order_relaxed);
}

int nr_pdcch_passive_queue_backlog(void)
{
  if (!atomic_load_explicit(&g_running, memory_order_acquire))
    return -1;
  pthread_mutex_lock(&g_lock);
  const int n = g_count;
  pthread_mutex_unlock(&g_lock);
  return n;
}

void nr_pdcch_passive_queue_lag_hist(uint64_t out[NR_PASSIVE_LAG_BINS], uint64_t *fresh_picks)
{
  for (int b = 0; b < NR_PASSIVE_LAG_BINS; b++)
    out[b] = atomic_load_explicit(&g_lag_hist[b], memory_order_relaxed);
  *fresh_picks = atomic_load_explicit(&g_fresh_picks, memory_order_relaxed);
}

void nr_pdcch_passive_queue_get_stats(nr_pdcch_passive_queue_stats_t *out)
{
  if (out == NULL) {
    return;
  }
  out->queued        = atomic_load_explicit(&g_queued, memory_order_relaxed);
  out->processed     = atomic_load_explicit(&g_processed, memory_order_relaxed);
  out->dropped_full  = atomic_load_explicit(&g_dropped_full, memory_order_relaxed);
  out->dropped_stale = atomic_load_explicit(&g_dropped_stale, memory_order_relaxed);
  out->dropped_epoch = atomic_load_explicit(&g_dropped_epoch, memory_order_relaxed);
  out->max_lag_slots = atomic_load_explicit(&g_max_lag, memory_order_relaxed);
}

void nr_pdcch_passive_queue_stop(void)
{
  if (!atomic_load_explicit(&g_running, memory_order_acquire)) {
    return;
  }
  atomic_store_explicit(&g_stop, 1, memory_order_release);
  pthread_mutex_lock(&g_lock);
  pthread_cond_broadcast(&g_cv);
  pthread_mutex_unlock(&g_lock);
  for (int i = 0; i < g_nthreads; i++) {
    pthread_join(g_threads[i], NULL);
  }
  atomic_store_explicit(&g_running, 0, memory_order_release);
}

void *nr_pdcch_passive_queue_epoch_counter(void) { return &g_dropped_epoch; }
