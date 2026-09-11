#include "PHY/NR_UE_TRANSPORT/nr_passive_replay_capture.h"
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

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c
 * \brief Consumer pool for the deferred passive PDSCH decode. See the header for the measurements
 * that forced this design.
 *
 * TWO CHANGES over the original single-consumer SPSC version, both forced by measurement at
 * ~1500 offered grants/s (2026-08-24):
 *
 * 1. N CONSUMERS. One consumer sustained 789 decodes/s against ~1570 offered, so 80 % of jobs were
 *    dropped on a full ring (`dropped[full=51647]` of 64835 queued). The PHY receive thread cannot
 *    be parallelised -- it is a sequential read-FEP-decode loop -- but the PDSCH decode behind it
 *    can be, and this box has eight idle cores (the thread pool holds 0,1,4,5,6,7 of 14).
 *
 * 2. DROP-OLDEST, not drop-newest. This is the change that matters for correctness, not throughput.
 *    Under overload a FIFO hands the consumer the OLDEST job -- which is precisely the one whose
 *    raw samples in ue->common_vars.rxdata are most likely to have been overwritten already. The
 *    measured consequence was decoded=58384 with crc_ok=0: the queue was working hard and producing
 *    nothing but stale decodes. For sensing, completeness is worthless and freshness is everything
 *    (a dropped grant costs one CFR row; a stale one costs a wrong row), so the producer now evicts
 *    the oldest entry to make room for the newest.
 *
 * Locking: a plain mutex, not the previous lock-free SPSC indices. The critical section is a ~600 B
 * struct copy; the work outside it is ~775 us per job. Contention is therefore negligible, and a
 * lock-free MULTI-consumer ring with eviction is far easier to get subtly wrong than to make fast.
 */

#include "PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.h"
#include "PHY/NR_UE_TRANSPORT/nr_pdsch_config_sweep.h" // Technique D scoring

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "common/utils/LOG/log.h"
#include "common/utils/system.h" // threadCreate
#include "PHY/NR_UE_TRANSPORT/nr_pdsch_data_aided.h"
#include "PHY/NR_UE_ISAC/nr_isac.h"
#include "PHY/MODULATION/modulation_UE.h"

/* The RF producer's own position, published in executables/nr-ue.c immediately BEFORE nrue_ru_read()
 * fills that slot's region of rxdata. */
extern _Atomic long nr_ue_diag_producer_absolute_slot;

static nr_pdsch_passive_job_t g_ring[NR_PDSCH_PASSIVE_QUEUE_MAX_DEPTH];
static int  g_depth = 0;
static int  g_head  = 0; // next write slot
static int  g_tail  = 0; // next read slot
static int  g_count = 0; // entries currently held

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv   = PTHREAD_COND_INITIALIZER;

static _Atomic uint64_t g_queued        = 0;
static _Atomic uint64_t g_decoded       = 0;
static _Atomic uint64_t g_crc_ok        = 0;
static _Atomic uint64_t g_dropped_full  = 0; // evicted oldest to admit a newer job
static _Atomic uint64_t g_dropped_stale = 0;
static _Atomic uint64_t g_max_lag       = 0;

/* P06a per-branch census, indexed by P03 branch_id. Grants are shared (one discovery, fanned out),
 * payloads/CRC are per branch. */
static _Atomic uint64_t g_br_queued[NR_RX_BRANCH_MAX];
static _Atomic uint64_t g_br_decoded[NR_RX_BRANCH_MAX];
static _Atomic uint64_t g_br_crc_ok[NR_RX_BRANCH_MAX];
static _Atomic uint64_t g_br_stale_epoch[NR_RX_BRANCH_MAX];

static _Atomic int g_running   = 0;
static _Atomic int g_stop      = 0;
static int         g_nthreads  = 0;
static pthread_t   g_threads[NR_PDSCH_PASSIVE_QUEUE_MAX_CONSUMERS];
static PHY_VARS_NR_UE *g_ue = NULL;

/* One frequency-domain slot buffer PER CONSUMER: they decode concurrently, and
 * nr_pdsch_passive_decode() both fills this and hands the same samples to the data-aided submit.
 * ~917 kB each at 273 PRB x 4 antennas. Heap, not thread-local storage -- the AoA work in
 * PASSIVE_RX_ONLY_HANDOVER.md records a shifted __thread layout producing an AVX alignment fault,
 * and this is far larger than the buffer that did it. */
static c16_t *g_rxdataF[NR_PDSCH_PASSIVE_QUEUE_MAX_CONSUMERS];

typedef struct {
  int idx;
} consumer_arg_t;
static consumer_arg_t g_args[NR_PDSCH_PASSIVE_QUEUE_MAX_CONSUMERS];

static void *nr_pdsch_passive_queue_thread(void *arg)
{
  const int idx = ((consumer_arg_t *)arg)->idx;
  PHY_VARS_NR_UE *ue        = g_ue;
  NR_DL_FRAME_PARMS *fp     = &ue->frame_parms;
  const uint32_t rxdataF_sz = fp->samples_per_slot_wCP;
  const long slots_per_frame = fp->slots_per_frame;

  LOG_I(PHY, "SENSING: passive PDSCH decode consumer %d started\n", idx);

  while (1) {
    nr_pdsch_passive_job_t job;

    pthread_mutex_lock(&g_lock);
    while (g_count == 0 && atomic_load_explicit(&g_stop, memory_order_relaxed) == 0) {
      /* Timed wait so a stop cannot be missed if it lands between the predicate and the wait. */
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

    /* ---- STALENESS CHECK. A job's raw IQ lives in rxdata only until the producer reaches the SAME
     * slot index one frame later, so decoding after that reads the NEXT frame's samples: the CRC
     * fails and it is indistinguishable from a weak channel. Count it as a drop, never "decode
     * anyway and hope". With drop-oldest admission this should now be rare -- if dropped_stale stays
     * high, the consumer pool is still too small for the offered rate. ---- */
    const long prod = atomic_load_explicit(&nr_ue_diag_producer_absolute_slot, memory_order_relaxed);
    const long lag  = prod - job.absolute_slot;
    if (lag > (long)atomic_load_explicit(&g_max_lag, memory_order_relaxed)) {
      atomic_store_explicit(&g_max_lag, (uint64_t)(lag > 0 ? lag : 0), memory_order_relaxed);
    }
    if (!nr_passive_samples_valid(prod, job.absolute_slot, slots_per_frame)) {
      atomic_fetch_add_explicit(&g_dropped_stale, 1, memory_order_relaxed);
      continue;
    }

    /* Only these three proc fields are read downstream -- verified by inspecting every proc->
     * reference in nr_dl_channel_estimation.c, nr_dlsch_demodulation.c and nr_pdsch_data_aided.c. */
    UE_nr_rxtx_proc_t proc = {0};
    proc.frame_rx   = job.frame_rx;
    proc.nr_slot_rx = job.nr_slot_rx;
    proc.gNB_id     = job.gNB_id;

    c16_t (*rxdataF)[rxdataF_sz] = (c16_t (*)[rxdataF_sz])g_rxdataF[idx];

    /* Replay the offset captured with these samples (see nr_slot_fep_fo_override_hz). */
    const double saved_fo = nr_slot_fep_fo_override_hz;
    nr_slot_fep_fo_override_hz = job.fo_hz;
    job.grant.check_sample_lifetime = true;
    job.grant.source_absolute_slot = job.absolute_slot;

    /* P06a: the branch may have lost lock or hit an RF discontinuity between this job's fan-out
     * and now. Its samples then belong to an epoch this branch has moved past, so the result must
     * not be mixed into the current one -- P05's "no old/new mixing", counted per branch rather
     * than silently decoded. Jobs carrying epoch 0/0 from a legacy producer are exempt: a set that
     * never ticked has nothing to be stale against. */
    {
      const nr_rx_branch_set_t *bset = nr_isac_rx_branches();
      const nr_rx_branch_dispatch_t d = {.branch_id = job.branch_id,
                                         .physical_channel = job.physical_channel,
                                         .lock_epoch = job.lock_epoch,
                                         .acq_epoch = job.acq_epoch};
      if (bset != NULL && bset->n_active > 1 && nr_rx_branch_dispatch_is_stale(bset, &d)) {
        atomic_fetch_add_explicit(&g_br_stale_epoch[job.branch_id & (NR_RX_BRANCH_MAX - 1)], 1,
                                  memory_order_relaxed);
        continue;
      }
    }

    /* P07 independent mode: resolve which branch this job is decoded FOR, and hand the whole chain
     * (decode AND data-aided submit) the same single-antenna view of the UE. Legacy: vue == ue. */
    const int view_phys = nr_pdsch_passive_branch_view_resolve(ue, job.physical_channel, &job.branch_id);
    job.physical_channel = (int8_t)view_phys;
    PHY_VARS_NR_UE *vue = nr_pdsch_passive_branch_view(ue, view_phys, job.branch_id);

    nr_pdsch_passive_decode_result_t dec;
    const nr_pdsch_passive_decode_status_t st =
        nr_pdsch_passive_decode(vue, &proc, &job.dlsch_pdu, &job.freq_alloc, &job.grant, rxdataF, &dec);

    nr_passive_replay_dl(&job, &dec);
    {
      static _Atomic uint64_t s_job_idx = 0; // trace index only (ISAC_PDSCH_VERDICT_TRACE), never a key
      nr_pdsch_passive_verdict_trace(atomic_fetch_add_explicit(&s_job_idx, 1, memory_order_relaxed),
                                     job.rnti, job.branch_id, view_phys, &dec);
    }
    nr_slot_fep_fo_override_hz = saved_fo;
    if (st == NR_PDSCH_PASSIVE_DECODE_UNSUPPORTED && !nr_passive_samples_valid(
            atomic_load_explicit(&nr_ue_diag_producer_absolute_slot, memory_order_relaxed),
            job.absolute_slot, slots_per_frame))
      atomic_fetch_add_explicit(&g_dropped_stale, 1, memory_order_relaxed);

    if (st != NR_PDSCH_PASSIVE_DECODE_ERROR && st != NR_PDSCH_PASSIVE_DECODE_UNSUPPORTED) {
      atomic_fetch_add_explicit(&g_decoded, 1, memory_order_relaxed);
      atomic_fetch_add_explicit(&g_br_decoded[job.branch_id & (NR_RX_BRANCH_MAX - 1)], 1,
                                memory_order_relaxed);
      /* Technique D scoring: the TB CRC is the only oracle that can tell a right payload
       * interpretation from a wrong one, and this is the one place it is known. */
      nr_pdsch_cfg_hypothesis_t winner;
      if (nr_pdsch_config_sweep_feedback(&job.sweep_ticket, st == NR_PDSCH_PASSIVE_DECODE_CRC_OK, &winner))
        LOG_A(PHY, "SENSING: Technique D CONVERGED rnti=0x%x tda=%u S=%u L=%u mask=0x%x table=%u\n",
              job.sweep_ticket.rnti, job.sweep_ticket.tda_index, winner.tda_start, winner.tda_length,
              winner.dmrs_mask, winner.mcs_table);
      if (st == NR_PDSCH_PASSIVE_DECODE_CRC_OK) {
        atomic_fetch_add_explicit(&g_crc_ok, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_br_crc_ok[job.branch_id & (NR_RX_BRANCH_MAX - 1)], 1,
                                  memory_order_relaxed);
        if (job.want_data) {
          /* Publish THIS job's monotonic slot so the CPI grid indexes it correctly. Without this the
           * submit derives the index from proc->frame_rx, which wraps at 1024 -- harmless in order,
           * fatal once several consumers submit concurrently across a wrap. */
          nr_isac_abs_slot_override = (uint64_t)job.absolute_slot;
          /* Same view as the decode: a branch's reconstruction consumes ONLY its own accepted TB and
           * its own antenna's Y. TODO(P10): carry job.branch_id/physical_channel into the CFR ABI. */
          nr_isac_pdsch_data_aided_submit(vue, &proc, &dec.cw, &job.dlsch_pdu, &job.freq_alloc, job.rnti,
                                          dec.tb, job.harq_pid_tag, rxdataF, (double)dec.nvar);
          nr_isac_abs_slot_override = 0;
        }
      }
    }
  }

  LOG_I(PHY, "SENSING: passive PDSCH decode consumer %d exiting\n", idx);
  return NULL;
}

static void report_sweep(const nr_pdsch_sweep_report_t *r)
{
  if (r->operational)
    LOG_I(PHY,"Technique D operational rnti=0x%x config=%lx tda=%u crc=%lu/%lu\n",
          r->rnti,(unsigned long)r->configuration,r->tda,(unsigned long)r->passes,(unsigned long)r->trials);
  else
    LOG_I(PHY,"Technique D evidence rnti=0x%x config=%lx tda=%u outcomes=%lu min_trials=%u "
              "best=%lu/%lu S=%u L=%u mask=0x%x table=%u winner=%d\n",
          r->rnti,(unsigned long)r->configuration,r->tda,(unsigned long)r->outcomes,r->minimum,
          (unsigned long)r->passes,(unsigned long)r->trials,r->hypothesis.tda_start,
          r->hypothesis.tda_length,r->hypothesis.dmrs_mask,r->hypothesis.mcs_table,r->winner);
}

bool nr_pdsch_passive_queue_start(PHY_VARS_NR_UE *ue, int depth, int n_consumers, int affinity)
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
  if (n_consumers > NR_PDSCH_PASSIVE_QUEUE_MAX_CONSUMERS) n_consumers = NR_PDSCH_PASSIVE_QUEUE_MAX_CONSUMERS;
  if (depth < 2) depth = 2;
  if (depth > NR_PDSCH_PASSIVE_QUEUE_MAX_DEPTH) depth = NR_PDSCH_PASSIVE_QUEUE_MAX_DEPTH;

  const uint32_t rxdataF_sz = ue->frame_parms.samples_per_slot_wCP;
  for (int i = 0; i < n_consumers; i++) {
    g_rxdataF[i] = (c16_t *)malloc16_clear((size_t)ue->frame_parms.nb_antennas_rx * rxdataF_sz * sizeof(c16_t));
    if (g_rxdataF[i] == NULL) {
      LOG_E(PHY, "SENSING: passive PDSCH queue: rxdataF allocation failed for consumer %d\n", i);
      for (int j = 0; j < i; j++) { free(g_rxdataF[j]); g_rxdataF[j] = NULL; }
      return false;
    }
  }

  nr_pdsch_config_sweep_set_reporter(report_sweep);
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
    snprintf(name, sizeof(name), "passivePdsch%d", i);
    /* Priority deliberately BELOW the RT receive thread's 97: if a consumer ever competes with the
     * receive path it must lose -- losing here costs one deferred decode, losing there costs time
     * sync, which is the entire problem this module exists to fix. One core each from base_core;
     * <0 leaves them unpinned. */
    threadCreate(&g_threads[i], nr_pdsch_passive_queue_thread, &g_args[i], name,
                 (affinity >= 0) ? (affinity + i) : -1, 50);
  }
  LOG_I(PHY, "SENSING: passive PDSCH decode pool: %d consumer(s), depth %d, cores %d..%d, drop-oldest\n",
        n_consumers, depth, affinity, (affinity >= 0) ? affinity + n_consumers - 1 : -1);
  return true;
}

bool nr_pdsch_passive_queue_running(void)
{
  return atomic_load_explicit(&g_running, memory_order_acquire) != 0;
}

bool nr_pdsch_passive_queue_enqueue(const nr_pdsch_passive_job_t *job)
{
  if (!atomic_load_explicit(&g_running, memory_order_acquire)) {
    return false;
  }
  pthread_mutex_lock(&g_lock);
  if (g_count == g_depth) {
    /* DROP-OLDEST. The evicted entry is the one whose rxdata samples are closest to being
     * overwritten, so discarding it is strictly better than refusing the fresh job -- see the file
     * header for the measurement that forced this (decoded=58384, crc_ok=0 under drop-newest). */
    g_tail = (g_tail + 1) % g_depth;
    g_count--;
    atomic_fetch_add_explicit(&g_dropped_full, 1, memory_order_relaxed);
  }
  g_ring[g_head] = *job;
  g_head         = (g_head + 1) % g_depth;
  g_count++;
  pthread_cond_signal(&g_cv);
  pthread_mutex_unlock(&g_lock);
  atomic_fetch_add_explicit(&g_queued, 1, memory_order_relaxed);
  atomic_fetch_add_explicit(&g_br_queued[job->branch_id & (NR_RX_BRANCH_MAX - 1)], 1,
                            memory_order_relaxed);
  return true;
}

int nr_pdsch_passive_queue_enqueue_fanout(const nr_pdsch_passive_job_t *job)
{
  if (job == NULL)
    return 0;
  nr_rx_branch_dispatch_t d[NR_RX_BRANCH_MAX];
  const nr_rx_branch_set_t *bset = nr_isac_rx_branches();
  const int n = nr_rx_branch_set_dispatch(bset, d, NR_RX_BRANCH_MAX);
  if (n <= 1) {
    /* No branch set, or exactly one active branch: the legacy single job, with the identity the
     * single branch (or the caller) already carries. Bit-identical to the pre-P06a producer. */
    nr_pdsch_passive_job_t one = *job;
    if (n == 1) {
      one.branch_id = d[0].branch_id;
      one.physical_channel = d[0].physical_channel;
      one.lock_epoch = d[0].lock_epoch;
      one.acq_epoch = d[0].acq_epoch;
    }
    return nr_pdsch_passive_queue_enqueue(&one) ? 1 : 0;
  }
  /* One decode per branch of the SAME grant. The queue is a fixed ring, so N branches cost N slots
   * and the drop-oldest eviction is N times more likely -- that is a real, measured cost of
   * independent mode, reported through dropped[full], never hidden by growing the ring. */
  int accepted = 0;
  for (int i = 0; i < n; i++) {
    nr_pdsch_passive_job_t copy = *job;
    copy.branch_id = d[i].branch_id;
    copy.physical_channel = d[i].physical_channel;
    copy.lock_epoch = d[i].lock_epoch;
    copy.acq_epoch = d[i].acq_epoch;
    if (nr_pdsch_passive_queue_enqueue(&copy))
      accepted++;
  }
  return accepted;
}

void nr_pdsch_passive_queue_get_stats(nr_pdsch_passive_queue_stats_t *out)
{
  if (out == NULL) {
    return;
  }
  out->queued        = atomic_load_explicit(&g_queued, memory_order_relaxed);
  out->decoded       = atomic_load_explicit(&g_decoded, memory_order_relaxed);
  out->crc_ok        = atomic_load_explicit(&g_crc_ok, memory_order_relaxed);
  out->dropped_full  = atomic_load_explicit(&g_dropped_full, memory_order_relaxed);
  out->dropped_stale = atomic_load_explicit(&g_dropped_stale, memory_order_relaxed);
  out->max_lag_slots = atomic_load_explicit(&g_max_lag, memory_order_relaxed);
  for (int b = 0; b < NR_RX_BRANCH_MAX; b++) {
    out->per_branch[b].queued  = atomic_load_explicit(&g_br_queued[b], memory_order_relaxed);
    out->per_branch[b].decoded = atomic_load_explicit(&g_br_decoded[b], memory_order_relaxed);
    out->per_branch[b].crc_ok  = atomic_load_explicit(&g_br_crc_ok[b], memory_order_relaxed);
    out->per_branch[b].dropped_stale_epoch =
        atomic_load_explicit(&g_br_stale_epoch[b], memory_order_relaxed);
  }
}

void nr_pdsch_passive_queue_stop(void)
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
    free(g_rxdataF[i]);
    g_rxdataF[i] = NULL;
  }
  atomic_store_explicit(&g_running, 0, memory_order_release);
}
