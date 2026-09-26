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

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_ul_ids.h
 * \brief Blind UL (PUSCH) scrambling-identity DECISION STATE: pure storage + bookkeeping for the
 * per-nSCID DM-RS identity estimate and the cell-wide data-scrambling-ID sweep/CRC-stall counter.
 * Moved out of nr_pusch_passive_decode.c (Task 13's UL scrambling-ID fallback; see task-13-report.md
 * and fix2-report.md): nr_pdcch_blind_monitor.c's blind_ul_apply_scrambling_ids() only ever READS
 * the decided identity/sweep result, but the state used to live in nr_pusch_passive_decode.c, which
 * is compiled into PHY_NR_PASSIVE_UL -- a heavy, gNB-receive-chain library the offline
 * test_nr_pdcch_blind_monitor gtest binary cannot and must not link. That made
 * nr_pdcch_blind_monitor.c's own library (nr_pdcch_blind_monitor, deliberately RT/PHY-independent so
 * the offline test can link it) depend upward on PHY_NR_PASSIVE_UL, exactly the class of defect
 * nr_pdcch_coreset_bank.{h,c} fixed for the CORESET bank (see fix-link-report.md, R14).
 *
 * The actual estimation work stays where it was: nr_dmrs_id_accumulate()/_decide() need live IQ and
 * the gNB channel-estimation output, and nr_scrambling_id_sweep_* is only ever exercised by a real TB
 * CRC, so both remain called from nr_pusch_passive_decode.c. This file owns only the storage, the
 * per-nSCID mutex/init-flag/stage-2-throttle bookkeeping around it, and the cell-wide data-ID sweep
 * object -- nr_pusch_passive_decode.c now reaches all of it through the accessors below instead of
 * touching shared globals directly, and nr_pdcch_blind_monitor.c's read side is unaffected (same
 * function names/signatures it already called). Same mutex objects / same non-blocking
 * trylock-and-skip-this-grant pattern as before the move -- only the storage moved.
 */

#ifndef NR_PUSCH_PASSIVE_UL_IDS_H
#define NR_PUSCH_PASSIVE_UL_IDS_H

#include <stdint.h>
#include <stdbool.h>
#include "PHY/NR_UE_TRANSPORT/nr_dmrs_id_estimate.h" // nr_dmrs_id_state_t (type only; the estimator
                                                       // functions themselves are NOT called here)

#ifdef __cplusplus
extern "C" {
#endif

/* scramblingID0/scramblingID1 are independent RRC fields -- one DM-RS estimator state per nSCID. */
#define NR_UL_DMRS_NSCID 2

/* Read-only view of the UL DM-RS identity estimate for one nSCID (0 or 1; diagnostic, plain ints,
 * racy by design) -- unchanged public contract from before the move. */
const nr_dmrs_id_state_t *nr_pusch_passive_ul_dmrs_id(int nscid);

/* UL decode path only. Non-blocking: mirrors the old pthread_mutex_trylock-and-skip-this-grant
 * behaviour on contention exactly -- returns NULL rather than waiting. On success, returns nscid's
 * (now-locked) state, always non-NULL; *was_initialized (if non-NULL) reports whether this state had
 * already been nr_dmrs_id_init()'d by an earlier call (== the prior separate g_ul_dmrs_id_init[]
 * check) -- the caller nr_dmrs_id_init()s it itself on false, since that call lives in
 * nr_dmrs_id_estimate.c, which this pure module deliberately does not depend on. Must be paired with
 * nr_pusch_passive_ul_dmrs_unlock(). */
nr_dmrs_id_state_t *nr_pusch_passive_ul_dmrs_trylock(int nscid, bool *was_initialized);
void nr_pusch_passive_ul_dmrs_unlock(int nscid);

/* Stage-2 (widened-range) accumulate throttle for nscid: true once every NR_UL_DMRS_STAGE2_THROTTLE
 * calls (post-increment modulo), same constant/reasoning as the pre-move code and the DL twin in
 * nr_pdsch_passive_queue.c. */
bool nr_pusch_passive_ul_dmrs_stage2_tick(int nscid);

/* dataScramblingIdentityPUSCH sweep (Task 13), cell-wide (this deployment has one UL BWP, so there is
 * nothing to key it by yet; unlike the DL side there is no per-RNTI Technique D convergence signal to
 * gate on, so nr_pusch_passive_ul_crc_stalled()'s own eligibility counter is the whole gate).
 * `current()`/`feed()` follow the same contract as the DL nr_pdsch_passive_data_id_* pair. */
uint16_t nr_pusch_passive_data_id_current(uint16_t pci, int dmrs_id, bool advance_ok);
void     nr_pusch_passive_data_id_feed(bool tb_crc_ok);

/* True when at least min_tries UL decodes have been attempted since the last CRC pass and NONE of
 * them passed CRC (windowed/resettable -- see nr_pusch_passive_ul_crc_note(); review fix round 1,
 * finding 1's fix, carried over verbatim by this move). */
bool nr_pusch_passive_ul_crc_stalled(uint32_t min_tries);
/* UL decode path only: record one TB CRC outcome for the stalled-window counter above -- resets it
 * to 0 on a pass, increments it on a fail. Same atomic object/semantics as the pre-move direct
 * atomic_store_explicit/atomic_fetch_add_explicit pair. */
void nr_pusch_passive_ul_crc_note(bool tb_crc_ok);

#ifdef __cplusplus
}
#endif
#endif // NR_PUSCH_PASSIVE_UL_IDS_H
