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

static nr_pdcch_passive_job_t g_ring[NR_PDCCH_PASSIVE_QUEUE_MAX_DEPTH];
static int g_depth = 0;
static int g_head  = 0; // next write slot
static int g_tail  = 0; // next read slot
static int g_count = 0;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv   = PTHREAD_COND_INITIALIZER;

static _Atomic int      g_running = 0;
static _Atomic int      g_stop    = 0;
static _Atomic uint64_t g_queued        = 0;
static _Atomic uint64_t g_processed     = 0;
static _Atomic uint64_t g_dropped_full  = 0;
static _Atomic uint64_t g_dropped_stale = 0;
static _Atomic uint64_t g_dropped_epoch = 0;
static _Atomic uint64_t g_max_lag       = 0;

static pthread_t g_threads[NR_PDCCH_PASSIVE_QUEUE_MAX_CONSUMERS];
static int       g_nthreads = 0;
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
    while (g_count == 0 && atomic_load_explicit(&g_stop, memory_order_relaxed) == 0) {
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
    job    = g_ring[g_tail];
    g_tail = (g_tail + 1) % g_depth;
    g_count--;
    pthread_mutex_unlock(&g_lock);

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

    nr_pdcch_blind_monitor_run_occasion(ue, &proc, true /* already off the RT thread */, job.absolute_slot);
    nr_slot_fep_fo_override_hz = saved_fo;
    atomic_fetch_add_explicit(&g_processed, 1, memory_order_relaxed);
  }

  LOG_I(PHY, "SENSING: blind PDCCH scan consumer %d exiting\n", idx);
  return NULL;
}

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
                 (affinity >= 0) ? (affinity + i) : -1, 50);
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
  g_head         = (g_head + 1) % g_depth;
  g_count++;
  pthread_cond_signal(&g_cv);
  pthread_mutex_unlock(&g_lock);
  atomic_fetch_add_explicit(&g_queued, 1, memory_order_relaxed);
  return true;
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
