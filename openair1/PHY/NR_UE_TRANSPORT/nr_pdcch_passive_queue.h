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

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_passive_queue.h
 * \brief Consumer pool for the deferred blind-PDCCH SCAN.
 *
 * WHY THIS EXISTS, MEASURED -- not inferred.
 *
 * nr_pdsch_passive_queue already moved the passive PDSCH DECODE off the PHY receive thread. What
 * stayed behind was the scan itself: a full-slot FEP over the CORESET symbols, PDCCH channel
 * estimation + equalisation (nr_pdcch_generate_llr), demapping, and the per-candidate polar
 * decodes. BTIM, mean per monitoring occasion, on this deployment:
 *
 *     load        fep_llr   demap   prepass   cand-decode   chest    TOTAL    RT duty (500us slot)
 *     165 gr/s    54.7us    3.2us   2.6us     5.4us         --       69.3us   13.9 %
 *    1530 gr/s    59-62us   3.5us   3.0us     9.6-19.2us    ~100us   80-102us 16-20 %
 *
 * PASSIVE_RX_ONLY_HANDOVER.md section 15 measured this receiver holding PBCH lock below ~18 % RT
 * duty and losing it above. Low load sits under that; high load STRADDLES it -- which is the
 * mechanism behind the 0 %/91 % CRC bimodality closed in BRANCH_IMBALANCE_HARQ_PLAN.md section 12
 * (grant-rate driven, 8/8 healthy at 165 gr/s, 17/32 dead at ~1500 gr/s).
 *
 * fep_llr alone is 54-62us, i.e. 60-79 % of the occasion. Moving the whole occasion off the receive
 * thread takes RT duty to roughly the residual gate cost and clears the threshold with margin. That
 * is what this queue does.
 *
 * SIZING. Occasions arrive once per monitoring periodicity; at periodicity 1 and 30 kHz SCS that is
 * one per 500 us slot (measured n=50000 occasions per ~25 s of lock). One consumer at 69-102 us per
 * 500 us is 14-20 % of ONE core and keeps up with a full slot-rate occasion stream, so the default
 * consumer count is 1 -- see the thread-safety note on nr_pdcch_passive_queue_start().
 *
 * ADMISSION. Identical policy to nr_pdsch_passive_queue, and for the same measured reason: the job
 * carries no samples, only a slot reference, so the raw IQ it will re-FEP lives in
 * ue->common_vars.rxdata only until the producer wraps to the same slot. Under overload the OLDEST
 * job is the one closest to being overwritten, so a full ring evicts the oldest rather than
 * refusing the newest. For sensing a dropped occasion costs coverage; a STALE one costs a wrong
 * answer, which is strictly worse.
 */

#ifndef NR_PDCCH_PASSIVE_QUEUE_H
#define NR_PDCCH_PASSIVE_QUEUE_H

#include <stdbool.h>
#include <stdint.h>

#include "PHY/defs_nr_UE.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NR_PDCCH_PASSIVE_QUEUE_MAX_DEPTH     64
#define NR_PDCCH_PASSIVE_QUEUE_MAX_CONSUMERS 4

/// One deferred monitoring occasion. Deliberately tiny: the consumer re-derives everything else
/// from the config and re-FEPs the samples itself, exactly as the PDSCH consumer does. Copying the
/// CORESET's rxdataF here instead would put the FEP back on the producer, which is the cost being
/// removed.
typedef struct {
  int  frame_rx;
  int  nr_slot_rx;
  int  gNB_id;
  long absolute_slot; ///< producer's slot counter at enqueue; drives the staleness check
  /// FO (Hz) sampled on the RECEIVE thread with these samples; replayed by the consumer
  /// via nr_slot_fep_fo_override_hz. NAN would mean "read live", which is the bug.
  double fo_hz;
} nr_pdcch_passive_job_t;

typedef struct {
  uint64_t queued;
  uint64_t processed;
  uint64_t dropped_full;  ///< evicted at admission because the ring was full
  uint64_t dropped_stale; ///< discarded by the consumer: rxdata already overwritten
  uint64_t max_lag_slots; ///< worst producer-consumer gap observed, in slots
} nr_pdcch_passive_queue_stats_t;

/**
 * @brief Start the consumer pool. One-shot; safe to call repeatedly (later calls are no-ops).
 *
 * THREAD SAFETY, and why the default is one consumer. The occasion body reached from here keeps
 * per-run state that is currently NOT synchronised: the adaptive energy floor, the cross-CPI RNTI
 * persistence table, and the occasion/accept/submit counters. With a single consumer these are
 * touched by exactly one thread, as they were when the body ran on the receive thread, so the
 * change is behaviour-preserving. Asking for more than one consumer is accepted but WARNS, because
 * one consumer already sustains a full slot-rate occasion stream (see the sizing note above) --
 * there is no measured reason to add threads, and doing so would need those statics made
 * thread-safe first.
 *
 * @return true if the pool is running and nr_pdcch_passive_queue_enqueue() may be used.
 */
bool nr_pdcch_passive_queue_start(PHY_VARS_NR_UE *ue, int depth, int n_consumers, int affinity);

/// True once the pool is running.
bool nr_pdcch_passive_queue_running(void);

/// Enqueue one occasion. Never blocks; evicts the oldest job if the ring is full.
bool nr_pdcch_passive_queue_enqueue(const nr_pdcch_passive_job_t *job);

void nr_pdcch_passive_queue_get_stats(nr_pdcch_passive_queue_stats_t *out);

void nr_pdcch_passive_queue_stop(void);

#ifdef __cplusplus
}
#endif

#endif // NR_PDCCH_PASSIVE_QUEUE_H
