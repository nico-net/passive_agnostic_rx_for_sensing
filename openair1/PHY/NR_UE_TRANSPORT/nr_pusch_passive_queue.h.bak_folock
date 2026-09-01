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

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_queue.h
 * \brief Defer the passive PUSCH decode off the PHY receive thread.
 *
 * WHY THIS EXISTS -- MEASURED (CAPTURE_OPTIMIZATION_PLAN.md, capture prof_080521, 2026-08-28).
 *
 * The uplink path was the last stage still running in-line on the receive thread, and the new UTIM
 * probe (ISAC_PUSCH_TIMING=1) measured what that costs, per grant, at 273 PRB / 4 RX:
 *
 *   fep 314 us   rx_pusch 319 us   cfr 29 us   decode 403 us   TOTAL 1065 us   (max 1459 us)
 *
 * against a 500 us slot budget at mu=1. over_slot was 16830/16907 = 99.5 %: essentially EVERY
 * uplink grant overran its deadline, by rather more than 2x. Together with the downlink scan that
 * put ~30-38 % duty on a thread this deployment was already measured to lose PBCH lock above ~18 %
 * (nr_pdcch_blind_monitor_rt.c's own header records the three-arm ablation), and the same capture
 * took four RF stalls needing full device re-inits.
 *
 * The downlink learned this first and nr_pdsch_passive_queue.{h,c} is the result. This file is that
 * design applied to the uplink; read that header for the reasoning behind DROP-OLDEST admission and
 * the staleness policy, which are inherited wholesale rather than re-derived.
 *
 * WHAT IS DIFFERENT FROM THE DOWNLINK QUEUE, and why this is a separate file rather than a
 * direction tag on the existing one:
 *
 * 1. THE CONSUMER OWNS A gNB, NOT JUST A BUFFER. The uplink chain runs the real gNB receive path,
 *    whose entire intermediate state -- the rxdataF ring, pusch_vars, the ULSCH HARQ process, its
 *    own thread pool -- hangs off one PHY_VARS_gNB. The downlink consumer needs only a private
 *    rxdataF. So the uplink needs a per-consumer CONTEXT (nr_pusch_passive_decode's `ctx`), which is
 *    a different resource model, not a different branch of the same one.
 *
 * 2. THE STALENESS WINDOW IS ONE SLOT SHORTER. An uplink grant is FEP'd with the timing advance
 *    applied, i.e. the window starts up to N_TA_offset + N_TA samples BEFORE its own slot boundary
 *    and therefore reads the tail of the PREVIOUS slot -- which the producer overwrites one slot
 *    sooner. MARGIN_SLOTS carries an extra slot for exactly that, and it is stated here rather than
 *    left to arithmetic because the failure mode is a silent CRC failure indistinguishable from a
 *    weak channel.
 *
 * Merging the two rings would have meant a union payload, two admission policies and two resource
 * models behind one lock, to save a ring and a mutex.
 */

#ifndef NR_PUSCH_PASSIVE_QUEUE_H
#define NR_PUSCH_PASSIVE_QUEUE_H

#include <stdbool.h>
#include <stdint.h>

#include "PHY/defs_nr_UE.h"
#include "PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.h"  // nr_pdcch_blind_ul_result_t
#include "PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.h" // NR_PUSCH_PASSIVE_MAX_CTX

#ifdef __cplusplus
extern "C" {
#endif

#define NR_PUSCH_PASSIVE_QUEUE_MAX_DEPTH 64
/// One consumer sustains ~1/1065us = ~940 decodes/s. The live cell offers ~178 UL grants/s, so one
/// is already ~5x the measured need -- the cap exists for headroom at higher load, not because the
/// current rate needs it. Bounded by the decode-context count: a consumer without its own context
/// would share gNB state with another.
#define NR_PUSCH_PASSIVE_QUEUE_MAX_CONSUMERS NR_PUSCH_PASSIVE_MAX_CTX
/// Slots of headroom between a job's own slot and the point the producer overwrites it. Five, not
/// the downlink's four: see point 2 in the file header -- the uplink FEP reads backwards past its
/// own slot boundary by the timing advance.
#define NR_PUSCH_PASSIVE_QUEUE_MARGIN_SLOTS 5

/// One deferred uplink decode. Everything by VALUE: the producer must not leave a consumer holding
/// a pointer into the receive thread's stack.
typedef struct {
  nr_pdcch_blind_ul_result_t grant;
  int      frame_rx;
  int      nr_slot_rx;
  int32_t  ta_offset_samples;
  /// Producer clock at capture. Two jobs: the staleness check, and -- unlike the downlink, where it
  /// is only the former -- the CFR's slow-time index, which MUST be stamped here because a consumer
  /// reading the producer counter would read a value from after its own samples were taken.
  long     absolute_slot;
  /// Mode 2: estimate and emit the CFR, skip the LLR/LDPC half. Carried per job rather than read
  /// from config in the consumer, so a config change cannot alter a job already in flight.
  bool     cfr_only;
} nr_pusch_passive_job_t;

/// Per-run census. Every field is a reason a job did NOT become a decode, so a shortfall in
/// `decoded` is attributable rather than merely visible.
typedef struct {
  uint64_t queued;
  uint64_t decoded;
  uint64_t crc_ok;
  uint64_t dropped_full;  ///< producer evicted the oldest to admit a newer job
  uint64_t dropped_stale; ///< dequeued too late; rxdata for that slot was already overwritten
  uint64_t max_lag_slots; ///< worst observed producer-minus-job lag, in slots
} nr_pusch_passive_queue_stats_t;

/**
 * @brief Start the consumer threads. Idempotent; safe to call when disabled.
 * @param ue          UE PHY instance the consumers decode against
 * @param depth       ring depth, clamped to [2, NR_PUSCH_PASSIVE_QUEUE_MAX_DEPTH]
 * @param n_consumers decoder threads, clamped to [1, NR_PUSCH_PASSIVE_QUEUE_MAX_CONSUMERS]
 * @param affinity    first core to pin to (consumer i gets affinity+i), or <0 for none. MUST NOT
 *                    overlap the RT thread-pool cores.
 * @return true if a consumer is running and enqueue() may be used.
 */
bool nr_pusch_passive_queue_start(PHY_VARS_NR_UE *ue, int depth, int n_consumers, int affinity);

/// True once a consumer is running. Callers use this to choose enqueue vs in-line decode, so a
/// refused start degrades to the previous behaviour rather than dropping every grant.
bool nr_pusch_passive_queue_running(void);

/**
 * @brief Producer side. Copies the job into the ring and returns immediately.
 * @return false if the job could not be admitted; the caller must NOT then decode in-line -- that
 *         would reintroduce the deadline overrun this exists to remove.
 */
bool nr_pusch_passive_queue_enqueue(const nr_pusch_passive_job_t *job);

void nr_pusch_passive_queue_get_stats(nr_pusch_passive_queue_stats_t *out);

/// Stop and join the consumers.
void nr_pusch_passive_queue_stop(void);

#ifdef __cplusplus
}
#endif

#endif // NR_PUSCH_PASSIVE_QUEUE_H
