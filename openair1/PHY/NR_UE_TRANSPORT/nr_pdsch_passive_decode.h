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
#include "PHY/NR_UE_TRANSPORT/nr_pdsch_prb_set.h" // nr_pdsch_passive_alloc_normalise (pure, lives there)
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
  uint8_t  ndi;       ///< new-data indicator: unchanged for a HARQ process = retransmission
  uint8_t  harq_pid;  ///< HARQ process number: keys the soft-combining buffer
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
  uint8_t  vrb_l;      ///< DCI 1_1 interleaved VRB-to-PRB bundle size (2 or 4) the caller's PRB
                       ///< list was built with (nr_pdsch_vrbl_pick()), fed back to the per-RNTI
                       ///< sweep once this decode's TB CRC is known. 0 = not interleaved, or a
                       ///< DCI 1_0 grant (L=2 is fixed by spec there, not a hypothesis) -- nothing
                       ///< to feed back, and the previous behaviour for every caller that never
                       ///< sets it.
  uint8_t  rnti_class; ///< nr_blind_rnti_class_t of the scheduling DCI (0 = C-RNTI). SI/RA/P-RNTI
                       ///< grants never get per-RNTI sweep state (final review I4).
  bool     dci11;      ///< scheduled by DCI 1_1: the only grant with a PRB-bundling (PRG) hypothesis
                       ///< (TS 38.214 5.1.2.3 fixes it for 1_0).
  bool     scr_dedicated; ///< decoded with the RRC-dedicated scrambling identities
                          ///< (nr_scrambling_dedicated(), final review I2)
} nr_pdsch_passive_grant_t;

/// Pick this RNTI's current DCI 1_1 interleaved VRB-to-PRB bundle-size hypothesis (2 or 4). The
/// RRC field that decides it (vrb-ToPRB-Interleaver) is invisible to a passive receiver, so it is a
/// minimal per-RNTI 2-arm sweep decided by the TB CRC, mirroring the PT-RS density sweep above --
/// pick an arm, build the grant's PRB list under it (copy the return value into `vrb_l`), decode,
/// and the TB CRC outcome feeds back automatically inside nr_pdsch_passive_decode(). Does NOT apply
/// to DCI 1_0, whose L=2 is fixed by spec and never swept.
int nr_pdsch_vrbl_pick(uint16_t rnti);

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
  uint8_t              qm_measured; ///< modulation order from the equalised symbols (nr_pdsch_qm_oracle.h), 0 = abstained
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
 * Rate matching: G (out->G) excludes the PT-RS REs of the swept/configured density, the CSI-RS REs of
 * the blind CSI-RS search's confirmed resource, and the REs of an SSB OBSERVED in this slot (PSS x SSS
 * of the acquired PCI on this slot's own FFT, TS 38.214 5.1.4 -- never a configured or projected SSB).
 * On the CPU path the demodulator must hand exactly G LLRs to the decoder (sum of the per-symbol
 * valid REs x Qm x Nl == G) or the call returns NR_PDSCH_PASSIVE_DECODE_ERROR.
 *
 * Scope (returns NR_PDSCH_PASSIVE_DECODE_UNSUPPORTED): PT-RS with no density to compute G from, a
 * segmented grant with PT-RS or with CSI-RS on a parity-changing segment, more DM-RS ports than
 * receive antennas, an MCS/allocation that yields no valid TBS, or an observed SSB the grant
 * overlaps with a DM-RS or PT-RS RE, or under SI-RNTI.
 *
 * The data-aided submit re-encodes onto every non-DM-RS RE (no SSB/CSI-RS/PT-RS hole): pass it out->G
 * and it refuses any decode whose G differs from its own RE model.
 *
 * @param ue           UE PHY instance
 * @param proc         Current slot's RX processing context
 * @param dlsch_config Allocation description; cw_info/tbslbrm/n_codewords are written by this call
 * @param freq_alloc   Resolved PRB allocation. n_prb_list > 0 = a DATA-ORDERED, possibly non-contiguous
 *                     BWP-relative PRB list (first_rb/last_rb/num_rbs/bitmap are re-derived from it);
 *                     prg > 0 = PRB bundling size. Either may split the grant into several channel-
 *                     estimation segments; one segment with prg == 0 is the unchanged contiguous path.
 *                     A segmented grant with PT-RS, or with CSI-RS rate matching on a segment whose
 *                     data position and PRB differ in parity, returns UNSUPPORTED.
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

/// nr_pdsch_passive_alloc_normalise() (make a PRB-list allocation self-consistent) moved to
/// nr_pdsch_prb_set.{h,c} -- it depends only on freq_alloc_bitmap_t and nr_prb_list_normalise(),
/// both already pure/dependency-free there, unlike this header. Declared via the include above.

/// Print the distinct decode-parameter tuples seen this run, with counts. Diffing this between a
/// 90 %-CRC run and a 0 %-CRC run is what identifies a wrong parameter -- see section 23.3.
void nr_pdsch_passive_parmset_dump(void);

/// Split the CRC failures into "LDPC did not converge" (LLRs wrong) versus "segments decoded but the
/// TB CRC failed" (reassembly/TBS wrong). Those point at different code -- see §29.1.
void nr_pdsch_passive_ldpc_stats_dump(void);

#ifdef __cplusplus
}
#endif

/** Layout-probe mode for the calling thread: decode only code block 0; its CRC comes back through
 *  nr_pdsch_passive_probe_outcome() and the TB is never reported decoded. */
void nr_pdsch_passive_probe_mode(bool on);
bool nr_pdsch_passive_probe_outcome(void);
/** PT-RS density sweep gate for the next decode on this thread: off while the grant's layout /
 *  Technique-D context is still being searched (a wrong PT-RS arm rotates layer 0 by its bogus CPE,
 *  7-13 % EVM, and would feed that failure into the searches). Default on. */
void nr_pdsch_passive_ptrs_sweep_allow(bool on);

/** Per-RNTI dataScramblingIdentityPDSCH walk (Task 13, gate per final review I1). Call ONLY for
 *  dedicated-class grants (nr_scrambling_dedicated()). `current()` returns this RNTI's latched id if
 *  any, else `pci` when `advance_ok` is false, else its ordered TB-CRC-walk hypothesis (PCI, then the
 *  decided DM-RS id if in range, then 0..1023). `advance_ok` = nr_scrambling_walk_eligible(DM-RS id of
 *  the grant's nSCID, nr_pdsch_passive_link_healthy(rnti), nr_pdsch_passive_rnti_ded_fails(rnti)).
 *  `feed()` reports whether that decode's TB CRC passed; call it ONLY when `advance_ok` was true for
 *  that same grant. Every start/step/wrap/latch is logged. */
uint16_t nr_pdsch_passive_data_id_current(uint16_t rnti, uint16_t pci, int dmrs_id, bool advance_ok);
void nr_pdsch_passive_data_id_feed(uint16_t rnti, bool tb_crc_ok);
/** One TB outcome from ANY decode path (deferred consumer and in-line), layout probes excluded: feeds
 *  the DL link-health tracker and, for dedicated-class grants, this RNTI's resettable fail window. */
void nr_pdsch_passive_crc_note(uint16_t rnti, bool dedicated, bool crc_ok);
/** Dedicated-class CRC fails since this RNTI's last dedicated pass. */
uint32_t nr_pdsch_passive_rnti_ded_fails(uint16_t rnti);
/** nr_scr_link_healthy() over the DL outcomes noted above. */
bool nr_pdsch_passive_link_healthy(uint16_t rnti);

/* ---- GPU front end (NR_GPU_FEP=1, libpdsch_gpu.so): the queue does FEP + chest + MMSE + LLR for a
 * whole slot on the GPU and hands each job its LLRs; nr_pdsch_passive_decode() then skips its own
 * FEP/chest/demod and runs descramble + LDPC on them. ---- */
#include "nr_pdsch_gpu_fep.h"
/** Fill the GPU job for this grant with the same arithmetic the decode uses (ports -> Nl, Qm, TBS ->
 *  probe horizon). false = the decode would not attempt it either (or the GPU cannot: PT-RS, CSI-RS
 *  rate matching, Nl > antennas), so the caller leaves it on the CPU. */
bool nr_pdsch_passive_gpu_job(const PHY_VARS_NR_UE *ue, const fapi_nr_dl_config_dlsch_pdu_rel15_t *dlsch_config,
                              const freq_alloc_bitmap_t *freq_alloc, const nr_pdsch_passive_grant_t *grant, int slot_rx,
                              bool probe, nr_gpu_pdsch_job_t *job);
/** LLRs for the NEXT nr_pdsch_passive_decode() call on this thread (n = 0 clears). */
void nr_pdsch_passive_set_llr_override(const int16_t *llr, uint32_t n);
/** The (descrambled) LLR buffer and G of this thread's last decode -- the GPU self-check compares against it. */
uint32_t nr_pdsch_passive_last_llr(const int16_t **p);
/** Decoded-grant evidence on ZP rate-matching entry @p i of @p cfg (csi_type 2): energy on its REs inside the
 *  grant's PRBs (fa) on the CSI-RS symbol(s), REs of the other entries left out, against every RE of the grant on
 *  ONE data-only reference symbol (no DM-RS, no CSI-RS, not in @p skip_symbols -- the SSB symbols; the one nearest
 *  the ZP symbol); all receive antennas summed. Counted only for a @p dedicated grant whose first DM-RS symbol
 *  carries this cell's DM-RS on its PRBs (nr_csirs_blind_pilot_presence >= 0.5; slot_rx seeds the sequence). Only
 *  symbols inside the allocation and the FFT'd range [fep_s0, fep_s0 + fep_n) are read; a ZP symbol in
 *  @p skip_symbols gives no evidence. rxdataF_flat: antenna a, symbol m at [a * stride + m * ofdm_symbol_size].
 *  Returns nr_csirs_blind_zp_grant_score(), or -1 (no evidence). */
double nr_pdsch_passive_zp_grant_score(const NR_DL_FRAME_PARMS *fp, const fapi_nr_dl_config_dlsch_pdu_rel15_t *cfg,
                                       const freq_alloc_bitmap_t *fa, const c16_t *rxdataF_flat, uint32_t stride,
                                       int slot_rx, int fep_s0, int fep_n, uint16_t skip_symbols, bool dedicated, int i);

/* Metrics getter (nr_passive_metrics.c): LDPC census counters. */
void nr_pdsch_passive_ldpc_counters(uint64_t *ok, uint64_t *seg_fail, uint64_t *tb_fail, uint64_t *zero_tb);

#endif // NR_PDSCH_PASSIVE_DECODE_H
