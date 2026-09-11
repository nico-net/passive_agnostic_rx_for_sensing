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
  bool check_sample_lifetime; ///< deferred IQ must survive the complete FEP
  long source_absolute_slot; ///< original RF slot, never re-stamped by consumers
  uint16_t rnti;      ///< CRC-recovered RNTI: the PDSCH scrambling sequence depends on it
  uint8_t  mcs;       ///< MCS index from the DCI
  uint8_t  rv;        ///< redundancy version from the DCI
  uint8_t  mcs_table; ///< 0 = qam64, 1 = qam256, 2 = qam64LowSE (the gNB's PDSCH-Config mcs-Table)
  uint16_t nb_rb_oh;  ///< xOverhead_PDSCH in REs per PRB (0/6/12/18); 0 when not configured
  int8_t   mcs_table_lbrm; ///< MCS table whose MAXIMUM modulation order sizes TBS_LBRM. TS 38.214
                       ///< 5.1.3.2 makes this a CELL property -- Qm = 8 if the UE is configured with
                       ///< mcs-Table = qam256 on ANY BWP of the serving cell, else 6 -- NOT the table
                       ///< this particular grant indexes. The two differ for every format 1_0 grant on
                       ///< a qam256 cell, since 1_0 always uses table 1 (qam64) for its own MCS while
                       ///< TBS_LBRM still uses 8. <0 = fall back to mcs_table (previous behaviour).
  uint16_t bw_tbslbrm; ///< PRBs to size TBS_LBRM from (TS 38.212 5.4.2.1: the MAXIMUM number of PRBs
                       ///< across the carrier's configured DL BWPs, which is NOT the allocation's own
                       ///< frequency reference). 0 = fall back to dlsch_config->BWPSize, the previous
                       ///< behaviour and correct whenever the two coincide. They do NOT coincide for a
                       ///< format 1_0 grant in a CORESET#0 common search space, where BWPSize is the
                       ///< 48-RB CORESET rather than the 273-RB carrier.
  uint8_t  tb_scaling; ///< TS 38.214 Table 5.1.3.2-2 index: scaling factor S = {1, 0.5, 0.25}
                       ///< applied to the TBS intermediate value. Non-zero ONLY for a DCI format 1_0
                       ///< with CRC scrambled by RA-RNTI or P-RNTI (Msg2/RAR and paging), where the
                       ///< field exists in place of NDI/HARQ. 0 everywhere else, which is the
                       ///< identity, so a caller that never sets it is unchanged.
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
  /// Static string naming WHY the status is what it is (an UNSUPPORTED sub-reason, or "crc_ok"/
  /// "crc_fail"/"error"); never NULL after the call. For the ISAC_PDSCH_VERDICT_TRACE line.
  const char          *reason;
} nr_pdsch_passive_decode_result_t;

/* ---- Independent (single-branch) DL view -- adaptive_RX_pipeline.md P07 --------------------------
 * The decode chain reads its antenna count from ue->frame_parms.nb_antennas_rx in every callee
 * (docs/passive_dl_branch_view_audit.md sec 1), so a branch view is delivered as a thread-local
 * SHALLOW COPY of *ue with nb_antennas_rx = 1 and common_vars.rxdata = {rxdata[phys]}: every
 * cross-branch mechanism (MRC, selection-diversity retry, subset scan, BRANCHFO, planned-branch
 * nvar) then switches itself off through its own existing nb_antennas_rx / nbRx == 4 gate, and the
 * noise estimate is that channel's alone. Default (no view) returns ue itself: bit-identical. */

/// Resolves the view for one job. ISAC_DL_BRANCH_VIEW=<phys> (replay tests) wins; else, when P03's
/// branch set has n_active > 1, the job's physical_channel if it names an active branch and
/// otherwise the lowest-id active branch (pre-P06 producers tag every job 0/0); else -1 = legacy.
/// *branch_id receives the branch identity (0 in legacy mode). An env value outside
/// [0, nb_antennas_rx) is rejected with LOG_E and resolves to legacy -- visible in the trace.
int nr_pdsch_passive_branch_view_resolve(const PHY_VARS_NR_UE *ue, int8_t job_physical_channel,
                                          uint8_t *branch_id);

/// Returns ue for phys < 0; otherwise the calling thread's shadow UE for that physical channel
/// (first call on a thread copies *ue once; later calls refresh only the mutable scalars the chain
/// reads). Pass the SAME pointer to nr_pdsch_passive_decode() AND nr_isac_pdsch_data_aided_submit()
/// so both see the same single antenna. Also arms the view for the counters/gates in the decoder.
PHY_VARS_NR_UE *nr_pdsch_passive_branch_view(PHY_VARS_NR_UE *ue, int phys, uint8_t branch_id);

/// Process-wide count of Nl > 1 grants REJECTED because a one-antenna branch view cannot separate
/// layers (plan sec 2.3 retraction). Reported separately from CRC failure.
uint64_t nr_pdsch_passive_view_unsupported_multilayer(void);

/// P10a: the branch identity armed on THIS thread by nr_pdsch_passive_branch_view(), or
/// NR_ISAC_BRANCH_NONE when no view is armed (the attached-UE and legacy 4-antenna paths). Lets the
/// data-aided CFR tap name its branch without a second thread-local to keep in step with this one.
uint8_t nr_pdsch_passive_view_branch(void);

/// P06a: the same counter split per branch, formatted as "br0=<n> br1=<n> ..." for the periodic
/// PDSCHQ-BRANCH log line. Returns a pointer to a static buffer (one periodic caller).
const char *nr_pdsch_passive_view_unsupported_multilayer_str(void);

/// ISAC_PDSCH_VERDICT_TRACE=1: one stdout line per DL job, "PDSCH-VERDICT job=.. rnti=.. crc=..
/// reason=.. Nl=.. branch=.. phys=..". Off by default (no output, no cost beyond one getenv).
void nr_pdsch_passive_verdict_trace(uint64_t job_idx, uint16_t rnti, uint8_t branch_id, int phys,
                                     const nr_pdsch_passive_decode_result_t *out);

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

/// Print the distinct decode-parameter tuples seen this run, with counts. Diffing this between a
/// 90 %-CRC run and a 0 %-CRC run is what identifies a wrong parameter -- see section 23.3.
void nr_pdsch_passive_parmset_dump(void);

/// Split the CRC failures into "LDPC did not converge" (LLRs wrong) versus "segments decoded but the
/// TB CRC failed" (reassembly/TBS wrong). Those point at different code -- see §29.1.
void nr_pdsch_passive_ldpc_stats_dump(void);

#ifdef __cplusplus
}
#endif

#endif // NR_PDSCH_PASSIVE_DECODE_H
