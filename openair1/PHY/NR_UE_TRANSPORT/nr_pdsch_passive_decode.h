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

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.h
 * \brief Decode a PDSCH the receiver was never granted -- the missing half of the passive
 * data-aided source (PASSIVE_PDSCH_DATA_AIDED_HANDOVER.md Part B).
 *
 * The attached-UE data-aided tap re-encodes `harq->b`, a transport block its own DL-SCH decode
 * already verified. A --passive-rx receiver never attaches, has no HARQ process and no decoded TB,
 * so before it can reuse that chain it must decode the overheard grant ITSELF. That is all this
 * module does: given the allocation the blind PDCCH monitor recovered, it runs the ordinary receive
 * chain (channel estimation -> nr_rx_pdsch LLRs -> descramble -> LDPC decode) and reports whether
 * the CRC passed.
 *
 * TWO DESIGN POINTS worth stating up front, because both look like unnecessary duplication:
 *
 * 1. It does NOT call nr_dlsch_decoding(). That function indexes
 *    `ue->dl_harq_processes[cw_idx][harq_pid]` and writes decodeResult/status/DLround/C/K/Z/F into
 *    it. A passive receiver's HARQ array is unused, so it would "work" -- but the blind monitor is
 *    not structurally prevented from running on an ATTACHED UE, where scribbling a foreign UE's
 *    HARQ state into our own process array would corrupt real reception in a way that would be very
 *    hard to trace back here. The LDPC plumbing is mirrored instead, against a private HARQ context.
 *
 * 2. The CRC pass rate is the ENTIRE go/no-go question for this feature (handover doc §B.5): a
 *    passive receiver sits at a different position than the UE the grant was for and sees a worse
 *    channel, so it is genuinely unknown whether most TBs decode at all. That is why decoding and
 *    SUBMITTING are separately switchable ([sensing] pdcch_blind_monitor_pdsch's `decode` field):
 *    measuring the rate must be possible without any reconstructed X reaching the sensing engine.
 *    A CRC-failed TB re-encoded anyway produces a wrong X and therefore high-power garbage smeared
 *    across every range bin -- strictly worse than contributing nothing.
 */

#ifndef NR_PDSCH_PASSIVE_DECODE_H
#define NR_PDSCH_PASSIVE_DECODE_H

#include <stdbool.h>
#include <stdint.h>

#include "common/utils/bits.h" // freq_alloc_bitmap_t
#include "PHY/defs_nr_UE.h"
#include "nfapi/open-nFAPI/nfapi/public_inc/fapi_nr_ue_interface.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Transport-block parameters a DCI carries that the receive chain cannot infer from the
/// allocation alone, plus the two deployment constants (MCS table, xOverhead) a blind receiver must
/// be told rather than read off the air.
typedef struct {
  uint16_t rnti;      ///< CRC-recovered RNTI: the PDSCH scrambling sequence depends on it
  uint8_t  mcs;       ///< MCS index from the DCI
  uint8_t  rv;        ///< redundancy version from the DCI
  uint8_t  mcs_table; ///< 0 = qam64, 1 = qam256, 2 = qam64LowSE (the gNB's PDSCH-Config mcs-Table)
  uint16_t nb_rb_oh;  ///< xOverhead_PDSCH in REs per PRB (0/6/12/18); 0 when not configured
} nr_pdsch_passive_grant_t;

/// Outcome of one passive decode attempt.
typedef enum {
  NR_PDSCH_PASSIVE_DECODE_CRC_OK = 0,   ///< decoded and CRC-verified; `tb` is usable
  NR_PDSCH_PASSIVE_DECODE_CRC_FAIL,     ///< chain ran end to end, CRC failed (the expected common case)
  NR_PDSCH_PASSIVE_DECODE_UNSUPPORTED,  ///< grant outside this module's scope; nothing was attempted
  NR_PDSCH_PASSIVE_DECODE_ERROR,        ///< internal failure (allocation, demodulation)
} nr_pdsch_passive_decode_status_t;

/// Everything the data-aided submit needs afterwards, so the caller never has to recompute it.
typedef struct {
  nr_pdsch_passive_decode_status_t status;
  const uint8_t       *tb;    ///< decoded payload, B = A + TB-CRC bits; NULL unless status is CRC_OK
  fapi_nr_dl_cw_info_t cw;    ///< codeword parameters this decode derived from the grant
  uint32_t             G;     ///< coded bits available on the allocation
  uint32_t             nvar;  ///< noise variance from channel estimation (fusion weight downstream)
} nr_pdsch_passive_decode_result_t;

/**
 * @brief Decode an overheard PDSCH.
 *
 * FEPs the allocation's symbols into the caller's `rxdataF` (so the caller can hand the SAME
 * frequency-domain samples to nr_isac_pdsch_data_aided_submit() without a second transform),
 * estimates the channel on the DM-RS symbols, demodulates to LLRs, descrambles with `grant->rnti`
 * and `dlsch_config->dlDataScramblingId`, and LDPC-decodes.
 *
 * `dlsch_config` must already describe the allocation (BWP, symbols, DM-RS mask/ports/nscid,
 * scrambling ids, harq_process_nbr); this function fills in its `cw_info[0]`, `tbslbrm` and
 * `n_codewords` from `grant`, so the caller can pass the same struct straight on to the
 * data-aided submit.
 *
 * Scope (returns NR_PDSCH_PASSIVE_DECODE_UNSUPPORTED, having done nothing): PTRS present, CSI-RS
 * rate-matching overlap, more than one DM-RS port, or an MCS/allocation that yields no valid TBS.
 * These match the data-aided submit's own scope guards -- decoding a grant it could not use anyway
 * would only burn CPU.
 *
 * @param ue           UE PHY instance
 * @param proc         Current slot's RX processing context
 * @param dlsch_config Allocation description; cw_info/tbslbrm/n_codewords are written by this call
 * @param freq_alloc   Resolved PRB allocation
 * @param grant        Transport-block parameters from the DCI + deployment constants
 * @param rxdataF      Caller-owned per-antenna frequency-domain slot buffer, filled by this call
 * @param[out] out     Decode outcome; always written
 * @return             out->status
 */
nr_pdsch_passive_decode_status_t nr_pdsch_passive_decode(PHY_VARS_NR_UE *ue,
                                                         const UE_nr_rxtx_proc_t *proc,
                                                         fapi_nr_dl_config_dlsch_pdu_rel15_t *dlsch_config,
                                                         const freq_alloc_bitmap_t *freq_alloc,
                                                         const nr_pdsch_passive_grant_t *grant,
                                                         c16_t rxdataF[][ue->frame_parms.samples_per_slot_wCP],
                                                         nr_pdsch_passive_decode_result_t *out);

#ifdef __cplusplus
}
#endif

#endif // NR_PDSCH_PASSIVE_DECODE_H
