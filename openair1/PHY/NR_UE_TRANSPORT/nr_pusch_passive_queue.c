#include "nr_passive_sample_lifetime.h"
#include "nr_pdcch_ul_discovery.h"
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

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_queue.c
 * \brief Deferred passive PUSCH decode. See the header for the measurement that motivates it.
 *
 * Structurally a copy of nr_pdsch_passive_queue.c -- deliberately, so the two behave identically
 * under overload and a reader who knows one knows the other. Same drop-oldest admission, same
 * staleness policing against the RF producer's published position, same below-RT consumer priority.
 * The differences are stated in the header: a per-consumer DECODE CONTEXT rather than a per-consumer
 * buffer, and one extra slot of staleness margin for the timing-advance backward read.
 */

#include "PHY/NR_UE_TRANSPORT/nr_pusch_passive_queue.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "common/utils/LOG/log.h"
#include "common/utils/system.h" // threadCreate

/* The RF producer's own position, published in executables/nr-ue.c immediately BEFORE nrue_ru_read()
 * fills that slot's region of rxdata. */
extern _Atomic long nr_ue_diag_producer_absolute_slot;

static nr_pusch_passive_job_t g_ring[NR_PUSCH_PASSIVE_QUEUE_MAX_DEPTH];
static int  g_depth = 0;
static int  g_head  = 0;
static int  g_tail  = 0;
static int  g_count = 0;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv   = PTHREAD_COND_INITIALIZER;

static _Atomic uint64_t g_queued        = 0;
static _Atomic uint64_t g_decoded       = 0;
static _Atomic uint64_t g_crc_ok        = 0;
static _Atomic uint64_t g_dropped_full  = 0;
static _Atomic uint64_t g_dropped_stale = 0;
static _Atomic uint64_t g_max_lag       = 0;

static _Atomic int g_running  = 0;
static _Atomic int g_stop     = 0;
static int         g_nthreads = 0;
static pthread_t   g_threads[NR_PUSCH_PASSIVE_QUEUE_MAX_CONSUMERS];
static PHY_VARS_NR_UE *g_ue = NULL;

typedef struct {
  int idx;
} consumer_arg_t;
static consumer_arg_t g_args[NR_PUSCH_PASSIVE_QUEUE_MAX_CONSUMERS];

static void *nr_pusch_passive_queue_thread(void *arg)
{
  /* The consumer index IS the decode-context index. That identity is what keeps two consumers off
   * one PHY_VARS_gNB, so it is not an incidental convenience -- do not renumber one without the
   * other. */
  const int idx = ((consumer_arg_t *)arg)->idx;
  PHY_VARS_NR_UE *ue = g_ue;
  const long slots_per_frame = ue->frame_parms.slots_per_frame;

  LOG_I(PHY, "SENSING: passive PUSCH decode consumer %d started\n", idx);

  while (1) {
    nr_pusch_passive_job_t job;

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

    /* ---- STALENESS. A job's raw IQ lives in rxdata only until the producer reaches the same slot
     * index one frame later; decoding after that reads the NEXT frame's samples and fails CRC
     * indistinguishably from a weak channel. Count it, never "decode anyway and hope". ---- */
    const long prod = atomic_load_explicit(&nr_ue_diag_producer_absolute_slot, memory_order_relaxed);
    const long lag  = prod - job.absolute_slot;
    if (lag > (long)atomic_load_explicit(&g_max_lag, memory_order_relaxed)) {
      atomic_store_explicit(&g_max_lag, (uint64_t)(lag > 0 ? lag : 0), memory_order_relaxed);
    }
    if (!nr_passive_samples_valid(prod, job.absolute_slot, slots_per_frame)) {
      atomic_fetch_add_explicit(&g_dropped_stale, 1, memory_order_relaxed);
      continue;
    }

    nr_pusch_passive_out_t out;
    nr_pusch_passive_decode(ue, idx, (uint32_t)job.frame_rx, (uint8_t)job.nr_slot_rx, &job.grant,
                            job.ta_offset_samples, (uint64_t)job.absolute_slot, job.cfr_only, job.fo_hz, &out);
    if(!job.cfr_only && (out.status==NR_PUSCH_PASSIVE_OK ||
        out.status==NR_PUSCH_PASSIVE_CRC_FAIL || out.status==NR_PUSCH_PASSIVE_ZERO_TB))
      nr_pdcch_ul_discovery_feedback(&job.grant,out.status==NR_PUSCH_PASSIVE_OK);


    if (out.status != NR_PUSCH_PASSIVE_UNSUPPORTED && out.status != NR_PUSCH_PASSIVE_ERROR) {
      atomic_fetch_add_explicit(&g_decoded, 1, memory_order_relaxed);
      if (out.status == NR_PUSCH_PASSIVE_OK) {
        atomic_fetch_add_explicit(&g_crc_ok, 1, memory_order_relaxed);
      }
    }
  }

  LOG_I(PHY, "SENSING: passive PUSCH decode consumer %d exiting\n", idx);
  return NULL;
}

bool nr_pusch_passive_queue_start(PHY_VARS_NR_UE *ue, int depth, int n_consumers, int affinity)
{
  if (atomic_load_explicit(&g_running, memory_order_acquire)) {
    return true;
  }
  if (ue == NULL) {
    return false;
  }
  /* The --cont-fo-comp refusal that used to live here is GONE, and the hazard it named is fixed
   * rather than avoided. It read: a deferred nr_slot_fep() would de-rotate using
   * ue->dl_Doppler_shift + ue->freq_offset, both mutated by the RECEIVE thread, hence newer than
   * the job's own samples. True -- so the producer now SAMPLES that offset on the receive thread,
   * in the same call that captured the samples, and carries it in nr_pusch_passive_job_t::fo_hz;
   * the consumer passes it to nr_pusch_passive_decode() instead of reading ue->.
   *
   * Why this mattered enough to fix: the fallback was not benign. --cont-fo-comp is required for
   * CFO to converge at sensing grade, so every run had it on, so the queue never started, so the
   * decode (measured 1065 us mean against a 500 us slot) ran in-line and overran its deadline on
   * 99.5 % of grants. Under bidirectional iperf that timing runaway lost PBCH lock and the
   * supervisor killed the capture at 73 s. The two features were mutually exclusive by accident.
   */

  if (n_consumers < 1) n_consumers = 1;
  if (n_consumers > NR_PUSCH_PASSIVE_QUEUE_MAX_CONSUMERS) n_consumers = NR_PUSCH_PASSIVE_QUEUE_MAX_CONSUMERS;
  if (depth < 2) depth = 2;
  if (depth > NR_PUSCH_PASSIVE_QUEUE_MAX_DEPTH) depth = NR_PUSCH_PASSIVE_QUEUE_MAX_DEPTH;

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
    snprintf(name, sizeof(name), "passivePusch%d", i);
    /* Priority BELOW the RT receive thread's 97: if a consumer ever competes with the receive path
     * it must lose. Losing here costs one deferred decode; losing there costs time sync, which is
     * the whole problem this module exists to fix. */
    threadCreate(&g_threads[i], nr_pusch_passive_queue_thread, &g_args[i], name,
                 (affinity >= 0) ? (affinity + i) : -1, 50);
  }
  LOG_I(PHY, "SENSING: passive PUSCH decode pool: %d consumer(s), depth %d, cores %d..%d, drop-oldest\n",
        n_consumers, depth, affinity, (affinity >= 0) ? affinity + n_consumers - 1 : -1);
  return true;
}

bool nr_pusch_passive_queue_running(void)
{
  return atomic_load_explicit(&g_running, memory_order_acquire) != 0;
}

bool nr_pusch_passive_queue_enqueue(const nr_pusch_passive_job_t *job)
{
  if (!atomic_load_explicit(&g_running, memory_order_acquire)) {
    return false;
  }
  pthread_mutex_lock(&g_lock);
  if (g_count == g_depth) {
    /* DROP-OLDEST: the evicted entry is the one whose samples are closest to being overwritten, so
     * discarding it beats refusing the fresh job. The downlink measured the alternative --
     * decoded=58384 with crc_ok=0, a queue working hard and producing only stale decodes. */
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
  return true;
}

void nr_pusch_passive_queue_get_stats(nr_pusch_passive_queue_stats_t *out)
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
}

void nr_pusch_passive_queue_stop(void)
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
