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

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_data_aided.h
 * \brief ISAC data-aided PDSCH source: reconstruct the transmitted symbols X from a CRC-verified
 * transport block and submit Ĥ[k] = Y[k]/X[k] at every data RE.
 *
 * This is the body of what used to be phy_procedures_nr_ue.c's static
 * nr_isac_pdsch_data_aided_tap(), lifted verbatim into its own translation unit (2026-07-30) so the
 * PASSIVE receiver can reuse it. The attached-UE caller keeps its scope guards and calls this; the
 * passive caller (nr_pdcch_blind_monitor_rt.c, via nr_pdsch_passive_decode.{h,c}) reaches the same
 * function after decoding an overheard grant itself. See PASSIVE_PDSCH_DATA_AIDED_HANDOVER.md §B.3:
 * the whole point is that the reconstruction chain is identical, only the source of the verified
 * transport block differs.
 */

#ifndef NR_PDSCH_DATA_AIDED_H
#define NR_PDSCH_DATA_AIDED_H

#include "common/utils/bits.h" // freq_alloc_bitmap_t
#include "PHY/defs_nr_UE.h"
#include "nfapi/open-nFAPI/nfapi/public_inc/fapi_nr_ue_interface.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Slow-time index override for a DEFERRED caller. `proc->frame_rx` wraps at 1024, so deriving the
/// CPI grid index from it breaks once submissions arrive out of order across a wrap. Set this to the
/// producer's monotonic absolute slot before the submit and back to 0 after. 0 = derive from proc,
/// which is what the in-order attached-UE path wants.
extern __thread uint64_t nr_isac_abs_slot_override;

/// P07 replay-only: when true, nr_isac_pdsch_data_aided_submit() skips its "ISAC/source enabled"
/// early return and runs the whole reconstruction (re-encode, Ĥ = Y/X, counter) even though the
/// sensing engine is not started -- nr_isac_submit_cfr_multi() still drops the row when the
/// engine is absent, so nothing reaches a detector. Exists so the replay can MEASURE that a branch
/// view's reconstruction consumes exactly its own CRC-OK TBs (G2 test 3) on a fixture whose
/// receiver.conf never enabled the engine. Default false = previous behaviour everywhere.
extern __thread bool nr_isac_data_aided_force;

/**
 * @brief Re-encode a CRC-verified transport block through the real TX chain (LDPC encode + rate
 * match + scramble + modulate), then submit Ĥ = Y/X at every DATA RE of the allocation to the
 * sensing engine as NR_ISAC_SRC_PDSCH_DATA. "Data RE" includes those sharing a DM-RS symbol when
 * only one CDM group is reserved -- see the .c file, getting that wrong desynchronises the whole
 * reconstruction rather than merely losing a few REs.
 *
 * The caller owns the scope decision. This function assumes, and does NOT re-check:
 *   - `tb_bytes` is a CRC-VERIFIED payload in the TX encoder's own B = A + TB-CRC format (i.e. a
 *     decoder's `harq->b`). Feeding it a failed decode reconstructs the WRONG X and injects
 *     high-power garbage across every range bin -- the single worst failure mode this source has.
 *   - single layer (`cw->Nl == 1`), no PTRS, no CSI-RS rate-matching overlap: the RE enumeration
 *     treats every RE of a non-DM-RS symbol inside the allocation as plain data.
 *
 * No-ops if ISAC or the pdsch_data source is disabled.
 *
 * @param ue            UE PHY instance (for frame_parms, the LDPC coding interface and the tpool)
 * @param proc          Current slot's RX processing context
 * @param cw            Codeword info (TBS, mcs, rv, Qm, BG, Nl) for this transport block
 * @param dlsch_config  Allocation/DM-RS/scrambling parameters of the grant
 * @param freq_alloc    Resolved PRB allocation
 * @param rnti          RNTI the PDSCH was scrambled with
 * @param tb_bytes      CRC-verified transport block, A + CRC bits
 * @param harq_pid_tag  Disambiguates this TB on the shared nrLDPC coding interface; see the .c file
 * @param rxdataF       Frequency-domain received samples for this slot, per rx antenna
 * @param nvar          Noise variance estimate from the demodulator, for the engine's fusion weight
 */
/// Process-wide count of transport blocks whose reconstruction reached the CFR submission stage
/// (P07 G2 test 3: a branch's submissions == its own CRC-OK count). See the .c for what it counts.
uint64_t nr_isac_pdsch_data_aided_submits(void);

void nr_isac_pdsch_data_aided_submit(PHY_VARS_NR_UE *ue,
                                     const UE_nr_rxtx_proc_t *proc,
                                     const fapi_nr_dl_cw_info_t *cw,
                                     const fapi_nr_dl_config_dlsch_pdu_rel15_t *dlsch_config,
                                     const freq_alloc_bitmap_t *freq_alloc,
                                     uint16_t rnti,
                                     const uint8_t *tb_bytes,
                                     uint32_t harq_pid_tag,
                                     const c16_t rxdataF[][ue->frame_parms.samples_per_slot_wCP],
                                     double nvar);

#ifdef __cplusplus
}
#endif

#endif // NR_PDSCH_DATA_AIDED_H
