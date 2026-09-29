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

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.h
 * \brief Defer the passive PDSCH decode off the PHY receive thread.
 *
 * WHY THIS EXISTS -- MEASURED, not assumed (PASSIVE_RX_ONLY_HANDOVER.md §15, 2026-08-24).
 *
 * nr_pdsch_passive_decode() costs 775 us mean / 2689 us max on the PHY receive thread, against a
 * SLOT BUDGET of 500 us at mu=1. A monitoring occasion that runs one costs ~946 us = 189 % of the
 * slot, so every single one overruns its deadline. A three-arm ablation at ~1500 offered grants/s
 * (the same load §14 measured) pins it as the cause rather than a correlate:
 *
 *   tap on,  decode ON   -> PBCH lock lost in 3 s, max_pos_acc runs away 375->583->794, exit(3)
 *   tap on,  decode OFF  -> lock held 88 s, max_pos_acc DEAD FLAT 546-550, pbch_ok 50/50
 *   tap OFF, decode OFF  -> lock held 105 s (to timeout), max_pos_acc flat 547-556
 *
 * The middle arm differs from the first in exactly one stage. And it is NOT aggregate CPU: the arm
 * that SURVIVED ran at 17.9 % duty while the arm that died ran at 25 %. What separates them is the
 * TAIL -- occasions over the 500 us deadline went 431/4816 (9.0 %) with the decode to 21/186000
 * (0.011 %) without it. One 2.7 ms decode inside a 500 us slot walks the FFT window.
 *
 * WHY THE CONSUMER RE-FEPs INSTEAD OF BEING HANDED TRANSFORMED DATA. The 775 us splits as
 * fep 215.7 / chest 235.0 / alloc 74.4 / demod 84.8 / ldpc 163.4 us. Handing over already-
 * transformed samples would move only alloc+demod+ldpc = 323 us (42 %) and leave 451 us on the
 * receive thread -- still 90 % of the slot budget, still overrunning. So the consumer must re-FEP,
 * which means it reads ue->common_vars.rxdata and inherits that buffer's lifetime.
 *
 * THE DEADLINE THIS BUYS, AND WHY IT IS POLICED RATHER THAN TRUSTED. rxdata is allocated
 * 2*samples_per_frame but the steady-state write is `firstSymSamp + get_samples_slot_timestamp(fp,
 * slot_nr)` (nr-ue.c:1121) -- within ONE frame. A slot's raw IQ is therefore overwritten exactly
 * slots_per_frame slots later: 20 slots / 10 ms at mu=1, SILENTLY, with no guard anywhere in the
 * tree. Queue depth 8 x 775 us = 6.2 ms < 10 ms, so a bounded queue makes it structurally safe --
 * but arithmetic is not evidence, so every job is checked at dequeue against the RF producer's own
 * published position (nr_ue_diag_producer_absolute_slot, nr-ue.c:43) and DISCARDED if stale. A
 * decode of overwritten samples would fail CRC and look exactly like a bad channel.
 */

#ifndef NR_PDSCH_PASSIVE_QUEUE_H
#define NR_PDSCH_PASSIVE_QUEUE_H

#include <stdbool.h>
#include <stdint.h>
#include "nr_pdsch_config_sweep.h"

#include "common/utils/bits.h" // freq_alloc_bitmap_t
#include "PHY/defs_nr_UE.h"
#include "nfapi/open-nFAPI/nfapi/public_inc/fapi_nr_ue_interface.h"
#include "PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.h" // nr_pdsch_passive_grant_t

#ifdef __cplusplus
extern "C" {
#endif

#define NR_PDSCH_PASSIVE_QUEUE_MAX_DEPTH 64
/// Consumers. One consumer sustains ~1/775us = ~1290 decodes/s; the live cell at ~1500 offered
/// grants/s presented ~1000 accepts/s in bursts and a single consumer dropped 31 % of them at the
/// ring (measured 2026-08-24). More consumers is the right lever -- a deeper queue buys staleness,
/// not throughput, because the rxdata lifetime is fixed.
#define NR_PDSCH_PASSIVE_QUEUE_MAX_CONSUMERS 6
/// Slots of headroom left between a job's own slot and the point the producer overwrites it. 4 slots
/// = 2 ms at mu=1. Not tuned: it is the margin that keeps a job from being decoded in the same slot
/// the producer is filling, and it is reported (max_lag_slots) so the choice can be checked rather
/// than believed.
#define NR_PDSCH_PASSIVE_QUEUE_MARGIN_SLOTS 4

/// One deferred decode. Carries everything the chain needs that is NOT derivable from `ue`, by
/// VALUE -- the producer must not leave the consumer holding pointers into its own stack frame.
typedef struct {
  fapi_nr_dl_config_dlsch_pdu_rel15_t dlsch_pdu; ///< allocation; the decode writes cw_info/tbslbrm into it
  freq_alloc_bitmap_t                 freq_alloc;
  nr_pdsch_passive_grant_t            grant;
  int      frame_rx;      ///< the only three UE_nr_rxtx_proc_t fields the chain reads (verified by
  int      nr_slot_rx;    ///< inspection of nr_dl_channel_estimation.c / nr_dlsch_demodulation.c /
  int      gNB_id;        ///< nr_pdsch_data_aided.c: frame_rx, nr_slot_rx, gNB_id and nothing else)
  long     absolute_slot; ///< producer clock at capture: what the staleness check compares against
  uint16_t rnti;
  /// nr_blind_rnti_class_t of the DCI that scheduled this PDSCH. Only the consumer's MAC-TA parse
  /// reads it: a RAR (RA-RNTI) and a dedicated DL-SCH PDU carry timing advance in different places,
  /// and guessing from the payload alone would mis-parse noise.
  uint8_t  rnti_class;
  /// nrLDPC_coding_interface harq_unique_pid, ALREADY namespaced by the producer (3000 + harq_pid
  /// for this path). uint32_t, not uint8_t: the base is 3000, and truncating it to 8 bits would
  /// alias the attached-UE (1000+) and CSI (2000+) namespaces that must stay disjoint so a hardware
  /// LDPC accelerator cannot mix two receivers' contexts.
  uint32_t harq_pid_tag;
  bool     want_data;     ///< submit the reconstructed CFR (pdsch_decode >= 2 and the source enabled)
  /// FO (Hz) sampled on the RECEIVE thread with these samples; replayed by the consumer
  /// via nr_slot_fep_fo_override_hz. NAN would mean "read live", which is the bug.
  double fo_hz;
  /// Value-only RNTI/TDA/context-generation ticket; zero generation means manual/unscored.
  /// Resets/evictions cannot redirect an old queued outcome into a new hypothesis context.
  nr_pdsch_sweep_ticket_t sweep_ticket;
  /// >0: DM-RS coherence probe for an UNRESOLVED passive BWP entry (nr_passive_bwp.h) -- no decode;
  /// the consumer transforms the DM-RS symbol, scores the carrier and hands back bwp_probe_payload.
  int8_t   bwp_probe_entry;
  uint8_t  layout_probe;   ///< 1 = DCI-layout trial: decode code block 0 only, its CRC is the arm outcome
  /// Passive BWP entry the grant was decoded against (>0): its TB CRC is fed back to the tracker.
  int8_t   bwp_entry;
  uint64_t bwp_probe_payload;
  /// True when dlsch_pdu.dlDataScramblingId came from this RNTI's data-ID sweep (Task 13) rather
  /// than the PCI fallback -- gates whether this job's CRC outcome should be fed back into that
  /// sweep (nr_pdsch_passive_data_id_feed), so an attempt that used the PCI never perturbs a sweep
  /// it did not use.
  bool     data_id_advance;
} nr_pdsch_passive_job_t;

/// Per-run census. Every field is a reason a job did NOT become a decode, so a shortfall in
/// `decoded` is always attributable rather than merely visible.
typedef struct {
  uint64_t queued;
  uint64_t decoded;
  uint64_t crc_ok;
  uint64_t dropped_full;   ///< producer found the ring full: the consumers are not keeping up
  uint64_t dropped_narrow; ///< budget: narrow grant refused while the ring was >= 90 % full
  uint64_t dropped_stale;  ///< dequeued too late; rxdata for that slot was already overwritten
  uint64_t max_lag_slots;  ///< worst observed producer-minus-job lag, in slots
  uint64_t slot_groups;    ///< dequeues that took >1 grant of one slot (FEP/chest shared)
  uint64_t batches;        ///< producer slot batches pushed
  uint64_t batches_multi;  ///< of which carried >1 grant (slots the cell shares between UEs)
} nr_pdsch_passive_queue_stats_t;

/**
 * @brief Start the consumer thread. Idempotent; safe to call when disabled.
 * @param ue        UE PHY instance the consumer decodes against (read-only apart from benign
 *                  ue->phy_cpu_stats counters -- see §15.3's state audit)
 * @param depth     ring depth; clamped to [2, NR_PDSCH_PASSIVE_QUEUE_MAX_DEPTH]. Worst-case job
 *                  latency is (depth / n_consumers) x 775 us and MUST stay inside the one-frame
 *                  rxdata lifetime, so depth and n_consumers are chosen together, not separately.
 * @param n_consumers number of decoder threads; clamped to [1, NR_PDSCH_PASSIVE_QUEUE_MAX_CONSUMERS]
 * @param affinity  first core to pin to (consumer i gets affinity+i), or <0 for none. MUST NOT
 *                  overlap the RT thread-pool cores.
 * @return true if the consumer is running and nr_pdsch_passive_queue_enqueue() may be used.
 */
bool nr_pdsch_passive_queue_start(PHY_VARS_NR_UE *ue, int depth, int n_consumers, int affinity);

/// True once the consumer is running. Callers use this to choose enqueue vs in-line decode, so a
/// failed start degrades to the previous behaviour rather than dropping every grant.
bool nr_pdsch_passive_queue_running(void);

/**
 * @brief Producer side. Copies the job into the ring and returns immediately.
 * @return false if the ring was full (counted as dropped_full); the caller should NOT then decode
 *         in-line -- doing so would reintroduce exactly the deadline overrun this exists to remove.
 */
bool nr_pdsch_passive_queue_enqueue(const nr_pdsch_passive_job_t *job);
/// Push the producer's held-back slot batch (call at each occasion start / before stop).
void nr_pdsch_passive_queue_flush(void);

void nr_pdsch_passive_queue_get_stats(nr_pdsch_passive_queue_stats_t *out);

/// DM-RS symbol oracle for the IN-LINE decode path (no consumer running): the same per-symbol
/// coherence measurement the consumer makes, on the DCI's own slot, fed to the ticket's Technique-D
/// context (mask + last data symbol, k0 = 0). Call AFTER that ticket's CRC feedback -- a resulting
/// prune retires outstanding tickets. `scratch` receives antenna-0 FFTs (samples_per_slot_wCP entries).
/// No-op for a settled/unscored ticket or a grant narrower than 4 PRBs.
void nr_pdsch_passive_oracle_inline(PHY_VARS_NR_UE *ue, const nr_pdsch_sweep_ticket_t *ticket,
                                    const fapi_nr_dl_config_dlsch_pdu_rel15_t *pdu, const freq_alloc_bitmap_t *fa,
                                    int nr_slot, c16_t *scratch);

/// Stop and join the consumer. Called from the monitor's own teardown.
void nr_pdsch_passive_queue_stop(void);

#ifdef __cplusplus
}
#endif

#include "PHY/NR_UE_TRANSPORT/nr_dmrs_id_estimate.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Read-only view of the DL DM-RS identity estimate for one nSCID (0 or 1). Read the decision with
 * nr_dmrs_id_2stage_decided() (acquire); the other fields are diagnostic and racy by design. */
const nr_dmrs_id_2stage_t *nr_pdsch_passive_dl_dmrs_id(int nscid);
/* Up to 6 RNTIs with >= 50 decodes, as " 0xRNTI:ok/decoded(pct)" items. */
void nr_pdsch_passive_queue_rnti_census(char *buf, size_t n);
#ifdef __cplusplus
}
#endif
#endif // NR_PDSCH_PASSIVE_QUEUE_H
