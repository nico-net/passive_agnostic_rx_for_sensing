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

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.h
 * \brief Phase 3 (TOTAL_PASSIVE_UE_HANDOVER.md): offline blind PDCCH/DCI decode + field
 * extraction for a passive receiver with no RRC context of its own.
 *
 * MVP SCOPE (see /home/sens/.claude/plans/zesty-baking-thompson.md for the full design record):
 * this module decodes DCI format 1_1 candidates blindly -- accepting any CRC-recovered RNTI in a
 * plausible range instead of requiring it to match our own -- and extracts the PRB allocation and
 * DMRS config a foreign UE's PDSCH grant carries. It is exercised OFFLINE ONLY in this slice
 * (synthetic/injected LLRs, see tests/nr_pdcch_blind_monitor_test.cc); it is NOT wired into the
 * live receive path, does not schedule PDCCH monitoring occasions, and touches no file in the
 * existing RT PDCCH decode path (dci_nr.c, nr_ue_procedures.c, nr_ue_dci_configuration.c).
 *
 * WHY FORMAT 1_1, not the simpler broadcast-derivable 1_0: this codebase's gNB
 * (openair2/LAYER2/NR_MAC_gNB/nr_radio_config.c:262) switches every RRC-connected UE to DCI
 * format 1_1 for DL scheduling as soon as RRC Reconfiguration completes -- before any real DL
 * traffic flows -- with no config override. Format 1_1's bit-width and field semantics are
 * nonetheless static, config-file-derived constants, identical for every UE on a cell in this
 * codebase (verified: resourceAllocationType always Type-1/RIV, pdsch_HARQ_ACK_Codebook always
 * dynamic, DMRS dmrs_Type/maxLength always spec-default, exactly 1 BWP, no cross-carrier
 * scheduling, no supplementary uplink -- none of these come from per-UE RRC capability
 * negotiation). This lets a blind listener size and interpret DCI-1_1 for ANY UE on a known
 * deployment without ever attaching -- the same "trust the known deployment config" shortcut
 * nr_csirs_monitor.{h,c} already uses for CSI-RS resource parameters.
 */

#ifndef NR_PDCCH_BLIND_MONITOR_H
#define NR_PDCCH_BLIND_MONITOR_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Default plausible dynamic C-RNTI range (TS 38.331: 0x0001-0xFFEF), used when
/// [sensing] pdcch_blind_monitor_rnti_range is unset.
#define NR_PDCCH_BLIND_RNTI_MIN_DEFAULT 0x0001
#define NR_PDCCH_BLIND_RNTI_MAX_DEFAULT 0xFFEF

// ---------------------------------------------------------------------------------------------
// DCI format 1_0 support (TS 38.212 7.3.1.2.1). Added 2026-08-21.
//
// WHY IT EXISTS: format 1_1 is what an RRC-connected UE gets for ordinary DL traffic, but every
// piece of information a blind receiver needs to CONFIGURE ITSELF travels on 1_0 -- SIB1 (SI-RNTI),
// Msg2/RAR (RA-RNTI), and Msg4/RRCSetup (TC-RNTI), plus the fallback grants a connected UE receives
// on C-RNTI before/outside its dedicated search space. Rejecting `format_indicator != 1` made all
// of that unreachable.
//
// THE STRUCTURAL FACT THAT MAKES THIS CHEAP: TS 38.212 7.3.1.2.1 defines FIVE different field lists
// for format 1_0, selected by which RNTI scrambled the CRC, and all five sum to exactly the same
// 28 bits on top of the frequency-domain assignment. So ONE polar decode produces a payload that
// can be re-interpreted under several RNTI hypotheses with no extra decode cost -- which is what
// this module does, because a blind receiver learns the RNTI only from the CRC and cannot know in
// advance which class it belongs to.
// ---------------------------------------------------------------------------------------------

/// Which DCI format a result was decoded under.
typedef enum {
  NR_BLIND_DCI_FORMAT_1_1 = 0, ///< the module's original (and still default) format
  NR_BLIND_DCI_FORMAT_1_0 = 1,
} nr_blind_dci_format_t;

/// RNTI class a format-1_0 payload was interpreted under. NOT cosmetic: it selects the field list
/// (TS 38.212 7.3.1.2.1), the TDRA list (TS 38.214 Table 5.1.2.1.1-1) and which plausibility checks
/// apply, so getting it wrong yields a confident, completely wrong allocation.
///
/// C vs TC is NOT decidable from the air: TC-RNTI is drawn from the same dynamic range as C-RNTI
/// and the two field lists are bit-identical (TS 38.212 gives TC-RNTI's DAI as 2 RESERVED bits in
/// the same position). This module labels by search space -- common => TC, UE-specific => C -- which
/// is a label only; the parse and every downstream consequence are identical either way EXCEPT the
/// TDRA list choice, and that choice is correct under this labelling (TS 38.214's own rule keys the
/// dedicated list on "UE-specific search space", not on the RNTI).
typedef enum {
  NR_BLIND_RNTI_CLASS_C  = 0, ///< C-RNTI / CS-RNTI / MCS-C-RNTI
  NR_BLIND_RNTI_CLASS_TC = 1, ///< TC-RNTI (Msg4 / RRCSetup)
  NR_BLIND_RNTI_CLASS_SI = 2, ///< SI-RNTI, fixed 0xFFFF (SIB1 and other SI messages)
  NR_BLIND_RNTI_CLASS_RA = 3, ///< RA-RNTI (Msg2 / RAR)
  NR_BLIND_RNTI_CLASS_P  = 4, ///< P-RNTI, fixed 0xFFFE (paging)
  NR_BLIND_RNTI_CLASS_COUNT
} nr_blind_rnti_class_t;

/// Search-space kind a candidate came from. A required input, not a hint: TS 38.212 7.3.1.0 sizes
/// format 1_0's frequency-domain field from CORESET#0 in a COMMON search space and from the active
/// DL BWP in a UE-specific one, TS 38.214 5.1.2.2.2 counts its PRBs from a different origin in each,
/// and TS 38.214 Table 5.1.2.1.1-1 picks a different TDRA list. Three independent things change.
typedef enum {
  NR_BLIND_SS_UE_SPECIFIC = 0,
  NR_BLIND_SS_COMMON      = 1,
} nr_blind_ss_type_t;

/// Largest value TS 38.321 5.1.3's RA-RNTI formula can produce:
/// 1 + s_id + 14*t_id + 14*80*f_id + 14*80*8*ul_carrier_id with s_id<14, t_id<80, f_id<8,
/// ul_carrier_id<2 => 1 + 13 + 14*79 + 1120*7 + 8960 = 17920. A CRC-recovered value above this
/// CANNOT be an RA-RNTI, which is the one spec-derived discriminator separating an RA-RNTI DCI from
/// a C-RNTI/TC-RNTI one when both are admissible in the same common search space.
#define NR_PDCCH_BLIND_RA_RNTI_MAX 17920

/* CRC-decoded SIB1 common facts, kept separate from dedicated-UE hypotheses.
 * In particular, a common TDA list does not prove the dedicated list is identical. */
typedef struct {
  uint16_t pci, dl_bwp_start, dl_bwp_size, ul_bwp_start, ul_bwp_size;
  uint8_t dl_mu, ul_mu, dl_count, ul_count;
  uint8_t dl_start[16], dl_length[16], dl_mapping[16], dl_k0[16];
  uint8_t ul_start[16], ul_length[16], ul_mapping[16], ul_k2[16];
} nr_pdcch_blind_common_config_t;
bool nr_pdcch_blind_publish_common(const nr_pdcch_blind_common_config_t *facts);
bool nr_pdcch_blind_get_common(uint16_t pci, nr_pdcch_blind_common_config_t *facts);
void nr_pdcch_blind_reset_common(void);

/* ---- Precomputed polar result (GPU batch path, nr_polar_gpu.h) -------------------------------
 * The blind scan's cost is one polar SC decode per candidate, and every candidate of an occasion is
 * independent -- so they can all be decoded as ONE GPU batch before the per-candidate decode/gate
 * path runs. Rather than give all four decode entry points a new argument, the batch hands its
 * result to the NEXT decode on the CALLING THREAD, which is the worker about to run that candidate.
 * Consumed once, and only when (llr, dci_length, aggregation_level) all match, so a stale or
 * mismatched hand-off silently falls back to the CPU decode instead of returning another
 * candidate's payload. With nothing set (llr == NULL) this is the CPU path, byte for byte. */
typedef struct {
  const int16_t *llr; ///< the exact unscrambled vector the batch decoded; NULL = nothing precomputed
  uint32_t crc;       ///< polar_decoder_int16()'s return value
  uint64_t payload;   ///< polar_decoder_int16()'s out[0]
  uint16_t dci_length;
  uint8_t aggregation_level;
} nr_pdcch_blind_polar_pre_t;
void nr_pdcch_blind_polar_pre_set(const nr_pdcch_blind_polar_pre_t *pre);

/* Raw length evidence is independent of every RRC interpretation field. */
typedef struct {
  uint64_t payload;
  uint16_t rnti;
  uint16_t mismatched_bits;
  const char *reject_reason;
} nr_pdcch_blind_raw_result_t;
/* Format-neutral polar/CRC core. SI/RA/P DCI 1_0 have no DL/UL indicator. */
bool nr_pdcch_blind_decode_raw(const int16_t *llr, uint8_t aggregation_level,
                               uint16_t dci_length, uint16_t rnti_min, uint16_t rnti_max,
                               bool require_dl_indicator, nr_pdcch_blind_raw_result_t *out);
bool nr_pdcch_blind_decode_raw_11(const int16_t *llr, uint8_t aggregation_level,
                                 uint16_t dci_length, uint16_t rnti_min, uint16_t rnti_max,
                                 nr_pdcch_blind_raw_result_t *out);

/// Result of one blind decode+extract attempt. Fields beyond `plausible`/`reject_reason` are
/// only meaningful when `plausible == true`.
typedef struct {
  uint16_t    rnti;               ///< CRC-recovered RNTI (plausibility range-checked)
  uint16_t    start_rb;           ///< PRB allocation start (resource-allocation-type-1/RIV)
  uint16_t    num_rb;              ///< PRB allocation size
  uint8_t     start_symbol;       ///< PDSCH start symbol (from the default TDRA table)
  uint8_t     num_symbols;        ///< PDSCH symbol count
  uint16_t    dl_dmrs_symb_pos;   ///< DMRS symbol bitmap (same encoding as fapi_nr_dl_config_dlsch_pdu_rel15_t)
  uint8_t     n_dmrs_cdm_groups;  ///< DMRS CDM groups without data (Table 7.3.1.2.2-1)
  uint16_t    dmrs_ports;         ///< DMRS port bitmask (Tables 7.3.1.2.2-1..4)
  uint8_t     dmrs_config_type;   ///< 0 = type 1, 1 = type 2 (the hypothesis the ports were read under)
  uint8_t     nscid;              ///< DMRS scrambling sequence initialization (fixed 0 for format 1_1 blind MVP)
  // ---- Transport-block parameters. Decoded since 2026-07-30 (previously read and discarded):
  // needed ONLY by the passive data-aided path (nr_pdsch_passive_decode.{h,c}), which must
  // reconstruct the exact TB the gNB sent before it can re-encode it. Ignored by the DM-RS-only
  // CFR tap, which never looks past the allocation. ----
  uint8_t     mcs;                ///< MCS index (interpretation depends on the deployment's mcs-Table)
  uint8_t     rv;                 ///< redundancy version
  uint8_t     ndi;                ///< new-data indicator
  uint8_t     harq_pid;           ///< HARQ process number
  uint8_t     tda_index;          ///< raw time-domain-assignment index the DCI carried
  uint8_t     mapping_type;       ///< PDSCH mapping type resolved from the TDRA (0 = typeA, 1 = typeB)
  // ---- Migrated from NRSniffer's dci_nr.c (nr_dci_false_detection / dci_thres), 2026-08-05: a
  // CRC match alone is a 1/65536 chance false accept on a candidate that never carried real PDCCH.
  // Re-encoding the decoded bits with the recovered RNTI and counting mismatches against the
  // original soft LLR polarity is a far stronger discriminator than CRC alone. Only meaningful
  // when the CRC-plausibility check (Step 2) already passed. See mismatched_bits_gate() in the .c.
  uint16_t    mismatched_bits;    ///< bit mismatches between re-encoded payload and input LLR polarity
  // ---- Format 1_0 (2026-08-21). On a 1_1 result these read: dci_format = NR_BLIND_DCI_FORMAT_1_1,
  // rnti_class = NR_BLIND_RNTI_CLASS_C, vrb_to_prb = 0, mcs_table = 0, everything else 0 -- i.e. a
  // caller that ignores them behaves exactly as before. ----
  uint8_t     dci_format;         ///< nr_blind_dci_format_t
  uint8_t     rnti_class;         ///< nr_blind_rnti_class_t (meaningful for format 1_0)
  uint8_t     vrb_to_prb;         ///< 0 = non-interleaved, 1 = interleaved (format 1_0 always carries it)
  uint8_t     mcs_table;          ///< MCS table this grant's MCS indexes: 0 = Table 5.1.3.1-1 (qam64),
                                  ///< 1 = 5.1.3.1-2 (qam256), 2 = 5.1.3.1-3 (qam64LowSE). Format 1_0
                                  ///< is ALWAYS 0 per TS 38.214 5.1.3.1 regardless of the gNB's
                                  ///< mcs-Table -- a deployment-wide mcs_table setting must NOT be
                                  ///< applied to a 1_0 grant.
  uint8_t     tb_scaling;         ///< RA-/P-RNTI only: index into TS 38.214 Table 5.1.3.2-2's
                                  ///< scaling factor S = {1, 0.5, 0.25}; 3 is reserved and rejected
  uint8_t     si_indicator;       ///< SI-RNTI only: 0 = SIB1, 1 = SI message (TS 38.212 7.3.1.2.1)
  uint8_t     short_messages_ind; ///< P-RNTI only (TS 38.331): 2 = paging + short message, 3 = both
  uint8_t     short_messages;     ///< P-RNTI only: the 8-bit short message
  bool        plausible;          ///< false => caller MUST discard this result
  const char* reject_reason;      ///< non-NULL iff !plausible; static string, do not free
  /// The polar-decoded payload word, right-aligned to dci_length. Filled by the format-1_0 entry
  /// point BEFORE any field interpretation, so it is valid even when `plausible == false` -- the
  /// same convention nr_pdcch_blind_ul_result_t::raw_payload already uses, and for the same reason:
  /// a REJECTED 1_0 payload with identifier=0 is a format-0_0 UL grant, and re-reading it costs no
  /// second polar decode. Zero on the format-1_1 paths, which have their own raw result struct.
  uint64_t    payload;
} nr_pdcch_blind_result_t;

/// Deployment facts a blind receiver cannot read off the air but CAN read off the gNB's own
/// configuration (the same "trust the known deployment config" shortcut csirs_monitor and
/// dci_length_override already use). All-zero = the spec-default behaviour
/// nr_pdcch_blind_decode_and_extract() has always used, so passing NULL is exactly the old path.
///
/// WHY THIS EXISTS (found 2026-07-30 while building the passive data-aided decode): the defaults
/// are WRONG for this project's gNB, and quietly so.
///  - TDRA: nr_radio_config.c's nr_rrc_config_dl_tda() installs a CUSTOM
///    pdsch-TimeDomainAllocationList in pdsch_ConfigCommon (index 0 = S:len_coreset,
///    L:14-len_coreset, i.e. S=1/L=13 for a 1-symbol CORESET), NOT the spec default table
///    (index 0 = S=2/L=12). The front-loaded DM-RS symbol happens to land on symbol 2 either way
///    -- which is why the existing DM-RS-only CFR tap works despite this -- but the PDSCH symbol
///    span, and hence G and every data RE position, differ.
///  - DM-RS additional position: fill_dmrs_mask() falls back to pos2 with no dedicated
///    pdsch_Config; this gNB configures pos1. Same story: same front-load symbol, different
///    additional-DM-RS symbols, hence a different data-RE set.
/// Both are harmless for a DM-RS-only tap and fatal for a decode, so they are set here rather than
/// silently assumed.
///
/// The `*_bits` members are the third and most consequential group. Field WIDTHS in DCI 1_1 are not
/// spec constants -- nr_dci_size()'s NR_DL_DCI_FORMAT_1_1 case derives most of them from RRC. Every
/// one of them defaults to this module's original hard-coded assumption, so leaving them at -1 is
/// exactly the old behaviour; set them to what the deployment's own config implies.
///
/// FOUND THE HARD WAY (2026-07-30): getting the TOTAL length right via `dci_length_override` is NOT
/// enough. On this project's gNB the real DCI is 45 bits at 106 PRB where the module's formula says
/// 48 -- and the 3-bit gap is `bwp_indicator` 1->0 (no additional BWP configured, so
/// sc_info->n_dl_bwp == 0) plus `time_domain_assignment` 4->2 (the deployment's own 3-entry TDRA
/// list, ceil(log2(3)), instead of the 16-entry default table). Both sit near the FRONT of the
/// payload, so with the right total but the wrong widths every field AFTER the frequency-domain
/// assignment was read from the wrong bit offset. That was invisible for a year of use because the
/// only fields anyone consumed -- RNTI (from the CRC) and the RIV allocation (which precedes the
/// misaligned ones) -- were still correct. It surfaced the moment the MCS/RV fields were needed:
/// 74 % of accepted grants decoded as rv != 0 on a link that has essentially no retransmissions.
typedef struct {
  int     tda_count;        ///< 0 = use the spec default TDRA table (old behaviour)
  uint8_t tda_start[16];    ///< per-index PDSCH start symbol S
  uint8_t tda_length[16];   ///< per-index PDSCH symbol count L
  uint8_t tda_mapping[16];  ///< per-index mapping type, 0 = typeA, 1 = typeB
  // TS 38.214 Table 5.1.2.1.1-1 picks the PDSCH TDRA list from the RNTI type AND the search space,
  // and a deployment can carry a DIFFERENT list in pdsch-ConfigCommon -- used by SI-/RA-/TC-RNTI and
  // by C-RNTI in a CORESET#0 common search space -- than in the dedicated pdsch-Config, used by
  // C-RNTI in a UE-specific search space. `tda_*` above is the DEDICATED list (what format 1_1
  // always uses). This is the COMMON one. Leave tda_common_count at 0 to reuse the dedicated list,
  // which is correct whenever the gNB derives both from the same pdsch-ConfigCommon -- this
  // project's does: the SIB1-derived list and pdcch_blind_monitor_tda are identical, reconciled
  // over the air 2026-08-21 (BWPSize=273, 2 entries, SLIV 40 -> S=1/L=13 and 85 -> S=1/L=7).
  int     tda_common_count;
  uint8_t tda_common_start[16];
  uint8_t tda_common_length[16];
  uint8_t tda_common_mapping[16];
  int     dmrs_add_pos;     ///< dmrs-AdditionalPosition as fill_dmrs_mask()'s column index (0..3); <0 = default (2 = pos2)
  int     dmrs_max_length;  ///< DM-RS maxLength (1 or 2); <=0 = default (1)
  int     dmrs_config_type; ///< DM-RS type: 0 = type 1 (default), 1 = type 2 -- selects the antenna-ports table

  // Per-field bit widths; -1 = this module's built-in assumption. Listed in TS 38.212 payload
  // order, matching nr_dci_size()'s own accumulation order one-for-one.
  // NOTE `time_domain_assignment` has NO entry here on purpose: nr_dci_size() computes it as
  // ceil(log2(tdaList->count)), so when `tda_count` above is set it is DERIVED from that rather
  // than being a second, independently-wrong knob.
  int bwp_indicator_bits;    ///< default 1; 0 when the gNB configures no additional DL BWP
  int vrb_to_prb_bits;       ///< default 0
  int prb_bundling_bits;     ///< default 0 (staticBundling); 1 for dynamicBundling
  int rate_matching_bits;    ///< default 0
  int zp_csirs_bits;         ///< default 0
  int tb2_bits;              ///< default 0 (one codeword); 8 for maxNrofCodeWordsScheduledByDCI n2
  int harq_pid_bits;         ///< default 4; 5 with harq-ProcessNumberSizeDCI-1-1
  int dai_bits;              ///< default 2 (dynamic HARQ-ACK codebook); 0 for semi-static
  int pdsch_to_harq_bits;    ///< default 3 = ceil(log2(dl-DataToUL-ACK count))
  int antenna_ports_bits;    ///< default 4 = getAntPortBitWidth() for dmrs-Type1/maxLength1
  int tci_bits;              ///< default 0; 3 when tci-PresentInDCI is configured
  int srs_request_bits;      ///< default 2; 3 with a supplementary uplink
  int cbg_bits;              ///< default 0 (CBGTI + CBGFI combined)
} nr_pdcch_blind_extract_opts_t;

/// Everything a format-1_0 decode needs beyond the payload itself. Unlike format 1_1 -- whose field
/// widths are RRC-derived deployment constants -- format 1_0's interpretation depends on WHERE the
/// candidate was found, so this is per-search-space context rather than per-deployment config.
typedef struct {
  uint8_t  ss_type;             ///< nr_blind_ss_type_t
  uint16_t n_rb_riv;            ///< N_RB^DL,BWP for the frequency-domain assignment, per TS 38.212
                                ///< 7.3.1.0: the CORESET#0 size in a common search space (or the
                                ///< initial DL BWP when CORESET#0 is not configured), the ACTIVE DL
                                ///< BWP size in a UE-specific one. Also bounds the decoded allocation.
  uint16_t rb_offset;           ///< PRB origin the decoded start_rb is counted from, per TS 38.214
                                ///< 5.1.2.2.2: the lowest RB of the CORESET the DCI arrived in for a
                                ///< common search space, the BWP start otherwise. Reported back in
                                ///< the result as-is; the caller adds it where it builds the PDSCH
                                ///< extraction, exactly as it already adds bwp_start for 1_1.
  uint8_t  dmrs_typeA_position; ///< MIB dmrs-TypeA-Position (blind-obtainable)
  uint8_t  mux_pattern;         ///< SS/PBCH-to-CORESET#0 multiplexing pattern (1/2/3), selecting the
                                ///< default TDRA table for SIB1. 0 is treated as 1. Only consulted
                                ///< when no TDRA list is configured AND sib1 is set.
  uint8_t  sib1;                ///< 1 = these SI-RNTI DCIs schedule SIB1 itself, so no
                                ///< pdsch-ConfigCommon TDRA list can exist yet and the default table
                                ///< selected by mux_pattern applies (TS 38.214 Table 5.1.2.1.1-1)
  uint32_t rnti_class_mask;     ///< bitmask of (1u << nr_blind_rnti_class_t) to attempt; 0 = derive
                                ///< from ss_type (common => SI|P|RA|TC, UE-specific => C). Narrowing
                                ///< it is the cheapest false-accept reduction available here: each
                                ///< class carries its own reserved-bit and range checks.
} nr_pdcch_blind_dci10_ctx_t;

/// DCI format 1_0 payload width: 28 fixed bits (identical for all five RNTI variants -- see the
/// nr_blind_rnti_class_t comment) plus the frequency-domain assignment,
/// ceil(log2(N_RB*(N_RB+1)/2)). `n_rb_riv` is TS 38.212 7.3.1.0's N_RB^DL,BWP, NOT necessarily the
/// active BWP -- see nr_pdcch_blind_dci10_ctx_t::n_rb_riv. Returns 0 for n_rb_riv < 1.
///
/// This is a genuine spec formula with no deployment-dependent terms, so unlike
/// nr_pdcch_blind_dci_size() it does NOT need a live-verified override -- the only thing that can
/// make it wrong is passing the wrong n_rb_riv.
uint16_t nr_pdcch_blind_dci10_size(uint16_t n_rb_riv);

/// DCI format 0_0 payload width: 20 fixed bits + ceil(log2(N_RB^UL,BWP*(N_RB^UL,BWP+1)/2)) + 1 when
/// a supplementary uplink is configured. Not decoded by this module -- it exists because TS 38.212
/// 7.3.1.0 zero-pads whichever of 0_0/1_0 is SMALLER in a UE-specific search space, so sizing 1_0
/// there requires knowing 0_0's size. Pass the result as the dci_length when it exceeds
/// nr_pdcch_blind_dci10_size(); the extractor treats the excess as trailing padding and requires it
/// to be zero.
uint16_t nr_pdcch_blind_dci00_size(uint16_t n_rb_riv, int supplementary_uplink);

/**
 * @brief Decode one PDCCH candidate blindly as DCI format 1_0 and extract its PDSCH grant.
 *
 * Same contract as nr_pdcch_blind_decode_and_extract_ex() -- one RNTI-independent polar decode, the
 * CRC-recovered value range-checked rather than compared, `out` filled unconditionally with
 * `plausible` saying whether to trust it -- but for format 1_0, with the RNTI class resolved from
 * the recovered RNTI and the search space (see nr_blind_rnti_class_t).
 *
 * Because all five 1_0 variants are the same width, the classes admissible for the recovered RNTI
 * are tried in order (SI/P are unambiguous by RNTI value; RA is tried before TC/C and gated on
 * NR_PDCCH_BLIND_RA_RNTI_MAX plus its 16 reserved bits) and the first whose checks all pass wins.
 *
 * @param llr         descrambled/demapped LLRs, length dci_length (as the 1_1 entry point)
 * @param aggregation_level  candidate aggregation level
 * @param dci_length  payload width in use: nr_pdcch_blind_dci10_size(ctx->n_rb_riv), or the DCI 0_0
 *                    width when that is larger in a UE-specific search space
 * @param ctx         search-space context; must not be NULL
 * @param rnti_min/max plausible DYNAMIC RNTI range (the broadcast RNTIs 0xFFFF/0xFFFE are admitted
 *                    independently of it when their class is enabled -- they sit outside it by
 *                    construction)
 * @param opts        deployment facts (TDRA lists, DM-RS); NULL = spec defaults throughout
 * @param[out] out    filled unconditionally; check out->plausible
 */
bool nr_pdcch_blind_decode_and_extract_10(const int16_t* llr,
                                          uint8_t         aggregation_level,
                                          uint16_t        dci_length,
                                          const nr_pdcch_blind_dci10_ctx_t* ctx,
                                          uint16_t        rnti_min,
                                          uint16_t        rnti_max,
                                          const nr_pdcch_blind_extract_opts_t* opts,
                                          nr_pdcch_blind_result_t* out);

/* Protocol plausibility is not physical validation. A unique candidate remains
 * UNRESOLVED until independent PDSCH/configuration evidence validates it. */
typedef enum {
  NR_DCI_UNRESOLVED, NR_DCI_AMBIGUOUS, NR_DCI_VALIDATED, NR_DCI_REJECTED,
} nr_dci_interpretation_state_t;

static inline const char *nr_dci_interpretation_state_name(nr_dci_interpretation_state_t state)
{
  switch (state) {
    case NR_DCI_UNRESOLVED: return "UNRESOLVED";
    case NR_DCI_AMBIGUOUS: return "AMBIGUOUS";
    case NR_DCI_VALIDATED: return "VALIDATED";
    case NR_DCI_REJECTED: return "REJECTED";
    default: return "INVALID_STATE";
  }
}

/* Exhaustive only over this parser's enabled RNTI classes for ONE supplied
 * frequency/TDRA context. Not proof of cell configuration or C-vs-TC identity.
 * Every attempted candidate retains its allocation or static rejection reason. */
#define NR_DCI10_MAX_CLASS_CANDIDATES 3
typedef struct {
  nr_dci_interpretation_state_t state;
  unsigned attempted, surviving;
  int unique_candidate; /* -1 unless exactly one candidate survives */
  nr_pdcch_blind_result_t candidates[NR_DCI10_MAX_CLASS_CANDIDATES];
} nr_dci10_interpretation_report_t;

/* Manual: legacy first-match behavior; report is empty and UNRESOLVED.
 * Auto: ONE polar decode, every enabled class, true only for a unique protocol-
 * plausible decoding ATTEMPT, never VALIDATED. Ambiguity clears allocation and
 * plausible but preserves payload/RNTI/mismatches, including format-0_0 input.
 * report may be NULL without disabling automatic ambiguity protection. */
bool nr_pdcch_blind_decode_10_mode(bool automatic,
                                  const int16_t *llr, uint8_t aggregation_level,
                                  uint16_t dci_length, const nr_pdcch_blind_dci10_ctx_t *ctx,
                                  uint16_t rnti_min, uint16_t rnti_max,
                                  const nr_pdcch_blind_extract_opts_t *opts,
                                  nr_pdcch_blind_result_t *out,
                                  nr_dci10_interpretation_report_t *report);

/* Bounded initial layout family. Returned grants contain a legal S/L scaffold;
 * never decode them without a Technique D selection. No layout is declared correct here. */
int nr_pdcch_blind_dl_layout_candidates(const nr_pdcch_blind_raw_result_t *raw,
                                        uint16_t len, uint16_t bwp, uint8_t typeA,
                                        nr_pdcch_blind_result_t out[3], uint8_t ids[3]);

/// Total DCI-1_1 payload width implied by `opts` at `bwp_size`. With `opts == NULL` this is
/// identical to nr_pdcch_blind_dci_size(). Its real job is to CHECK a configuration: if this
/// disagrees with the live-verified `dci_length_override`, the per-field widths are wrong and every
/// field after the frequency-domain assignment will be read from the wrong offset -- the exact
/// failure described above, which a total-length-only check cannot see.
uint16_t nr_pdcch_blind_dci_size_ex(uint16_t bwp_size, const nr_pdcch_blind_extract_opts_t* opts);

/// Interpret already CRC-verified DL bits under one explicit layout hypothesis.
/// Does not repeat polar decoding; plausibility is NOT evidence that a layout is correct.
bool nr_pdcch_blind_extract_11(const nr_pdcch_blind_raw_result_t *raw,
                               uint16_t dci_length, uint16_t bwp_size,
                               uint8_t dmrs_typeA_position,
                               const nr_pdcch_blind_extract_opts_t *opts,
                               nr_pdcch_blind_result_t *out);

/**
 * @brief Parse the [sensing] pdcch_blind_monitor_* config keys. Safe to call once at UE start-up;
 * a no-op that leaves the monitor disabled when unset. NOT required for
 * nr_pdcch_blind_decode_and_extract()/nr_pdcch_blind_dci_size(), which are pure functions callable
 * independent of this config surface (see the offline test) -- parsed here so the config surface
 * and its parser have their own coverage ahead of the (not-yet-built) live-wiring follow-up.
 */
void nr_pdcch_blind_monitor_init(void);

/// Non-zero once nr_pdcch_blind_monitor_init() has parsed a non-empty configuration.
int nr_pdcch_blind_monitor_enabled(void);

/* Self-configure the monitor for the COMMON search space from values the cell broadcasts in the
 * clear (MIB/SIB1 -> mac->type0_PDCCH_CSS_config). Lets the receiver work on an unknown cell with
 * no hand-written pdcch_blind_monitor_* config. Returns false and changes nothing if the inputs are
 * not usable. See the definition-site comment in nr_pdcch_blind_monitor.c. */
/* 1 when pdcch_blind_monitor_autoconf asked for self-configuration (default 0). */
bool nr_pdcch_blind_monitor_autoconf_wanted(void);

bool nr_pdcch_blind_monitor_autoconf_css0(int num_rbs,
                                          int num_symbols,
                                          int cset_start_rb,
                                          int ssb_offset_point_a,
                                          int ss_period_slots,
                                          int ss_slot,
                                          int ss_duration,
                                          int ss_first_symbol,
                                          int mux_pattern,
                                          int pci,
                                          int rb_offset,
                                          int dmrs_typea_position);

/* PHASE 3: recover the DEDICATED CORESET/search space by search (Techniques A + C) instead of
 * reading pdcch_blind_monitor_coreset/_ss/_bwp by hand. See the definition-site comment in
 * nr_pdcch_blind_monitor.c for the full design (why this is NOT gated on g_cfg.bwp_size == 0). */

/* Non-zero once nr_pdcch_blind_monitor_autodiscover_step() has selected a candidate footprint
 * and populated g_cfg for it. This is NOT a verification verdict. Distinct from g_cfg.bwp_size, which CSS0 autoconf above
 * may already have set for the COMMON search space by the time this is first checked. */
bool nr_pdcch_blind_monitor_autodiscover_done(void);
/* Apply /tmp/coresets_discovered.txt (stage 1-2 hand-off) when it changes; true if applied. */
bool nr_pdcch_blind_monitor_discovered_poll(void);
bool nr_pdcch_blind_monitor_bank_has_geometry(int rb_offset, int groups, int duration, int bundle, int interleaver,
                                               int shift, int nid);
/* True while every discovered CORESET is verified: the catalog walk (and its decode pass) is paused. */
bool nr_pdcch_blind_monitor_discovery_paused(void);

/** Advance the current geometry after an inconclusive length budget. The offset is not
 * blacklisted: other widths and future observations remain eligible. */
void nr_pdcch_blind_monitor_autodiscover_retry(int failed_rb_offset);
/** Compatibility query: always false; offsets are no longer blacklisted. */
bool nr_pdcch_blind_monitor_autodiscover_offset_rejected(int rb_offset);

/* Technique A (whole-carrier DM-RS correlation): call once per symbol while autodiscover is on and
 * nr_pdcch_blind_monitor_autodiscover_done() is still false. `rxdataF_symbol` is one antenna's
 * already-FEP'd frequency-domain samples for exactly this (slot, symbol), indexed [0,
 * ofdm_symbol_size) -- `const void*` rather than `const c16_t*` so this header stays free of the
 * PHY-heavy c16_t definition (nr_pdcch_coreset_map.h pulls in PHY/impl_defs_top.h; the .c file casts
 * internally). Returns true once a candidate footprint is selected and g_cfg is populated.
 * `abs_slot` feeds nr_pdcch_blind_monitor_confirmed_rnti() for the bootstrap-RNTI log line. */
void nr_pdcch_blind_monitor_autodiscover_observe_symbol1(const void* rxdataF_symbol, int ofdm_symbol_size,
                                                          int n_rb_carrier, int first_carrier_offset, uint16_t pci,
                                                          int slot);
bool nr_pdcch_blind_monitor_autodiscover_step(const void* rxdataF_symbol, int ofdm_symbol_size, int n_rb_carrier,
                                              int first_carrier_offset, uint16_t pci, int slot, int symbol,
                                              uint32_t abs_slot);

/* Technique C's result: called by the RT tap once nr_pdcch_dci_length_sweep() (driven from
 * nr_pdcch_blind_monitor_rt.c, which has the real candidate LLR stream this pure/offline-testable
 * header does not) returns a winning length. g_cfg is owned by nr_pdcch_blind_monitor.c, so the RT
 * tap -- which only ever sees the const nr_pdcch_blind_monitor_get_cfg() accessor -- needs this
 * setter rather than writing g_cfg.dci_length_override directly. */
void nr_pdcch_blind_monitor_autodiscover_set_dci_length(int dci_length);
/** Archive/activate the verified geometry externally, then resume occupancy discovery for another CORESET. */
void nr_pdcch_blind_monitor_autodiscover_next(void);

/**
 * @brief DCI format 1_1 payload bit-width under this module's fixed MVP assumption set (see the
 * file-level comment). Derived field-by-field from openair2/LAYER2/NR_MAC_COMMON/nr_mac_common.c's
 * nr_dci_size() NR_DL_DCI_FORMAT_1_1 case (not a call to that function -- see the .c file for why).
 *
 * @param bwp_size  Active DL BWP size in PRBs (SIB1-derivable, hence blind-obtainable)
 * @return          Total DCI-1_1 payload bits for this bwp_size under the fixed assumption set
 */
uint16_t nr_pdcch_blind_dci_size(uint16_t bwp_size);

/**
 * @brief Decode one PDCCH candidate blindly and, if the CRC-recovered RNTI and every extracted
 * field pass their plausibility checks, extract its PRB allocation and DMRS config.
 *
 * Reuses openair1/PHY/CODING/nrPolar_tools's polar_decoder_int16() (RNTI-independent single
 * decode -- the CRC-recovered value is compared against a range, not an equality, which is the
 * entire "blind" part) and openair2/LAYER2/NR_MAC_COMMON/nr_mac_common.c's get_dl_tda_info()/
 * fill_dmrs_mask() (both freestanding, explicitly handle a NULL dedicated-BWP/PDSCH-Config input as
 * the correct MVP-assumption branch, not a workaround). PRB allocation is this module's OWN
 * ~10-line Type-1/RIV implementation (openair1/PHY/CODING's NRRIV2BW()/NRRIV2PRBOFFSET(), from the
 * lean nr_common target) rather than a call to nr_ue_process_dci_freq_dom_resource_assignment(),
 * which lives in the heavy, ASN.1-RRC-linked NR_L2_UE target -- see the .c file's top comment.
 *
 * @param llr                Already-descrambled, already-demapped LLR array, length dci_length
 *                           (this function starts one layer below PDCCH RT plumbing -- the caller
 *                           is responsible for descrambling/demapping, exactly as the RT receive
 *                           path's nr_dci_decoding_procedure() already does before its own
 *                           polar_decoder_int16() call in dci_nr.c)
 * @param aggregation_level  PDCCH aggregation level of this candidate (1/2/4/8/16)
 * @param dci_length         Payload bit-width, from nr_pdcch_blind_dci_size()
 * @param bwp_size           Active DL BWP size in PRBs (for RIV decode)
 * @param dmrs_typeA_position MIB dmrs-TypeA-Position (2 or 3; blind-obtainable from MIB)
 * @param rnti_min, rnti_max Plausible RNTI range (defaults NR_PDCCH_BLIND_RNTI_{MIN,MAX}_DEFAULT)
 * @param[out] out           Filled unconditionally; check out->plausible before trusting it
 * @return                   out->plausible (convenience — same value written to *out)
 */
bool nr_pdcch_blind_decode_and_extract(const int16_t* llr,
                                       uint8_t         aggregation_level,
                                       uint16_t        dci_length,
                                       uint16_t        bwp_size,
                                       uint8_t         dmrs_typeA_position,
                                       uint16_t        rnti_min,
                                       uint16_t        rnti_max,
                                       nr_pdcch_blind_result_t* out);

/**
 * @brief As nr_pdcch_blind_decode_and_extract(), with the deployment's real TDRA list and DM-RS
 * additional-position substituted for the spec defaults. `opts == NULL` is bit-identical to the
 * plain form above (which is implemented as exactly that call).
 */
bool nr_pdcch_blind_decode_and_extract_ex(const int16_t* llr,
                                          uint8_t         aggregation_level,
                                          uint16_t        dci_length,
                                          uint16_t        bwp_size,
                                          uint8_t         dmrs_typeA_position,
                                          uint16_t        rnti_min,
                                          uint16_t        rnti_max,
                                          const nr_pdcch_blind_extract_opts_t* opts,
                                          nr_pdcch_blind_result_t* out);

// =============================================================================================
// UPLINK: DCI formats 0_1 and 0_0 (TS 38.212 7.3.1.1.2 / 7.3.1.1.1). Added 2026-08-25.
//
// WHY: a passive receiver that decodes only DL DCI sees half the air. UL grants ride the SAME
// CORESET/CCE space as the DL ones and are already polar-decoded by the existing scan -- they were
// simply discarded at `format_indicator != 1`. Keeping them turns the receiver bidirectional:
// every UL grant names a PUSCH the receiver can then extract and measure, on a link whose
// transmitter is a UE rather than the gNB.
//
// TWO THINGS ARE STRUCTURALLY DIFFERENT FROM THE DL CASE, and both cost real work:
//
//  1. 0_0 is FREE, 0_1 is NOT. TS 38.212 7.3.1.0 size-aligns 0_0 with 1_0, so a 1_0 candidate's
//     polar decode ALREADY yields a valid 0_0 payload -- only the field-list interpretation
//     branches on the identifier bit. Format 0_1 has its own RRC-derived width (43 bits here
//     against 1_1's 47) and therefore needs its OWN polar decode, at roughly the cost of the
//     existing 1_0 scan.
//
//  2. The grant does not schedule THIS slot. A DL DCI's PDSCH is in the same slot; a UL DCI's
//     PUSCH is k2 slots later, and k2 comes from the pusch-TimeDomainAllocationList, not the
//     payload. So `k2` is carried in the TDRA list below and reported in the result -- a UL grant
//     is only actionable together with the slot it points at.
//
// ON FIELD WIDTHS -- READ THIS BEFORE SETTING ANY OF THEM. The same trap that cost the DL path a
// year of wrong bit offsets (see nr_pdcch_blind_extract_opts_t's "FOUND THE HARD WAY" note) is
// WORSE here, because 0_1 has more RRC-derived fields than 1_1 and this deployment's UL RRC config
// is not readable off the air (PUSCH-Config travels in a ciphered RRCReconfiguration). Measured on
// the live cell 2026-08-25: the gNB's own FAPI dump reports payload_size=43 at 273 PRB. The
// spec-fixed part of that is
//     format_ind 1 + FDRA 16 + MCS 5 + NDI 1 + RV 2 + HARQ 4 + TPC 2 + dmrs_seq_init 1
//   + ulsch_ind 1  =  33 bits,
// leaving exactly 10 for the RRC-dependent fields. The gNB logs `ant=2` and `dai=2` per UL DCI, so
// those two fields are >= 2 bits each, and srs_request is 2 with no supplementary uplink -- 6 of
// the 10. The remaining 4 are distributed across TDA / frequency-hopping / SRI / precoding /
// CSI-request, and MORE THAN ONE ASSIGNMENT SUMS TO 4. Arithmetic alone cannot pick the layout.
//
// The layout is therefore RECONCILED, not derived: `raw_payload` below is filled on EVERY decode,
// including rejected ones, so a capture can be replayed offline against the gNB's own logged
// h_id/ndi/rv/mcs/tpc/dai/mimo/ant values and the unique consistent width assignment solved for.
// Eight simultaneous field constraints is a far stronger test than any total-length check, and it
// is the only honest way to pin a layout that the air does not disclose.
// =============================================================================================

/// Uplink DCI formats, kept in a separate enum from nr_blind_dci_format_t so that no existing
/// switch over DL formats silently acquires a new reachable case.
typedef enum {
  NR_BLIND_UL_DCI_FORMAT_0_1 = 0,
  NR_BLIND_UL_DCI_FORMAT_0_0 = 1,
} nr_blind_ul_dci_format_t;

/// Result of one blind UL decode+extract attempt. Deliberately a SEPARATE struct from
/// nr_pdcch_blind_result_t rather than extra members on it: the DL result is consumed by the
/// PDSCH decode, the CFR tap and the queue job (which copies it by value), and the overriding
/// constraint on this work is that the DL path does not regress. Nothing DL-side can be perturbed
/// by a struct it does not contain.
///
/// `raw_payload`/`dci_length`/`crc_rnti` are filled even when `plausible == false` -- they are the
/// reconciliation instrument described above, and a rejected payload is exactly the case worth
/// dumping when a width assignment is still being pinned.
typedef struct {
  // ---- always filled, valid even when !plausible ----
  uint64_t raw_payload;      ///< the polar-decoded payload word, right-aligned to dci_length
  uint16_t dci_length;       ///< payload width this decode was attempted at
  uint16_t crc_rnti;         ///< CRC-recovered RNTI (NOT range-checked; see `rnti` for the checked one)
  uint16_t mismatched_bits;  ///< re-encode-vs-LLR-polarity false-detection measure (as the DL path)

  int width_hyp_class;       ///< -1 when no width search owns this grant
  int interp_hyp_class;      ///< -1 when no interpretation search owns this grant
  uint64_t hyp_generation;   ///< reject feedback from an earlier discovery context
  uint8_t carrier_indicator, ul_sul_indicator; ///< preserve carrier identity for search equivalence

  // ---- valid only when plausible ----
  uint16_t rnti;             ///< CRC-recovered RNTI, range-checked
  uint8_t  ul_dci_format;    ///< nr_blind_ul_dci_format_t

  // Frequency domain (resource allocation type 1 / RIV -- the only type this deployment uses)
  uint32_t freq_domain_assignment; ///< the raw RIV, kept for ground-truth comparison
  uint16_t start_rb;         ///< PRB allocation start, counted from bwp_start
  uint16_t num_rb;           ///< PRB allocation size
  uint16_t bwp_start;        ///< UL BWP start the allocation is relative to (echoed from opts)
  uint16_t bwp_size;         ///< UL BWP size (echoed from opts)
  uint8_t  bwp_indicator;    ///< decoded BWP indicator field (0 when the field is 0 bits wide)

  // Time domain. NOTE `k2` is NOT in the payload -- it is the TDRA list entry's own k2, and it is
  // what makes the grant actionable: the PUSCH is in slot (DCI slot + k2).
  uint8_t  tda_index;        ///< raw time-domain-assignment index the DCI carried
  uint8_t  start_symbol;     ///< PUSCH start symbol S
  uint8_t  num_symbols;      ///< PUSCH symbol count L
  uint8_t  mapping_type;     ///< 0 = typeA, 1 = typeB
  uint8_t  k2;               ///< slot offset from this DCI's slot to the PUSCH

  // Transport block
  uint8_t  mcs;              ///< MCS index (indexes mcs_table below)
  uint8_t  mcs_table;        ///< 0 = qam64 (Table 6.1.4.1-1), 1 = qam256, 2 = qam64LowSE,
                             ///< 3..5 = the transform-precoding variants. From opts, not the payload.
  uint8_t  rv;               ///< redundancy version
  uint8_t  ndi;              ///< new-data indicator
  uint8_t  harq_pid;         ///< HARQ process number
  uint8_t  nrOfLayers;       ///< from the precoding-information field (1 when that field is 0 bits)

  // DM-RS. Resolved the same way the DL path resolves its own: the payload carries an index, the
  // deployment config carries dmrs-Type/maxLength/AdditionalPosition, and the symbol mask is
  // computed from both.
  uint16_t ul_dmrs_symb_pos;    ///< DM-RS symbol bitmap
  uint8_t  dmrs_config_type;    ///< 0 = type1, 1 = type2
  uint8_t  n_dmrs_cdm_groups;   ///< CDM groups without data
  uint16_t dmrs_ports;          ///< DM-RS port bitmask
  uint8_t  nscid;               ///< DM-RS sequence initialisation (the dmrs_seq_init payload bit)
  uint8_t  antenna_ports_field; ///< the raw antenna-ports code point, for ground-truth comparison

  // Waveform / scrambling
  uint8_t  transform_precoding; ///< 0 = disabled (CP-OFDM), 1 = enabled (DFT-s-OFDM). From opts.
  uint8_t  frequency_hopping;   ///< decoded hopping flag (0 when the field is 0 bits wide)
  uint16_t data_scrambling_id;  ///< nid_pusch; from opts, else the blind sweep, else the PCI
  uint16_t ul_dmrs_scrambling_id; ///< from opts, else the blind DM-RS estimate, else the PCI
  /// True when data_scrambling_id came from the per-cell data-ID sweep (Task 13), not opts/PCI --
  /// gates whether this grant's decode outcome should be fed back into that sweep
  /// (nr_pusch_passive_data_id_feed), so an attempt that used the PCI never perturbs it.
  bool     data_id_advance;

  // Fields decoded only so they can be reconciled against the gNB's own log. Not consumed
  // downstream -- but they are half the evidence that the layout is right, so they are reported
  // rather than discarded.
  uint8_t  tpc;                 ///< TPC command for the scheduled PUSCH
  uint8_t  dai;                 ///< 1st downlink assignment index
  uint8_t  srs_request;         ///< SRS request
  uint8_t  csi_request;         ///< CSI request
  uint8_t  precoding_info;      ///< precoding information and number of layers (raw code point)
  uint8_t  ulsch_indicator;     ///< UL-SCH indicator; 0 = CSI only, NO PUSCH data (rejected)

  bool        plausible;        ///< false => caller MUST discard everything above the raw fields
  const char* reject_reason;    ///< non-NULL iff !plausible; static string, do not free
} nr_pdcch_blind_ul_result_t;

/// Deployment facts needed to size and interpret an UL DCI. Same contract as the DL
/// nr_pdcch_blind_extract_opts_t: every `*_bits` member defaults to a documented assumption when
/// left at -1, and the TDA width is DERIVED from `tda_count` rather than being a second
/// independently-wrong knob.
///
/// Unlike the DL case there is no "spec default" that is likely to be right, because 0_1's widths
/// depend on PUSCH-Config, SRS-Config and CSI-MeasConfig -- none of which a passive receiver can
/// read. Treat every default here as a STARTING POINT for the reconciliation described at the top
/// of this section, not as a value to trust.
typedef struct {
  // ---- UL BWP ----
  uint16_t bwp_start;        ///< UL BWP start in CRBs
  uint16_t bwp_size;         ///< UL BWP size in PRBs; also the RIV reference (resource alloc type 1)

  uint8_t numerology;       ///< actual UL SCS index; drives the default-table k2 offset
  uint8_t dmrs_typeA_position; ///< MIB ASN.1 enum: 0=pos2, 1=pos3

  // ---- pusch-TimeDomainAllocationList. k2 is here and nowhere else. ----
  int     tda_count;         ///< 0 = use the TS 38.214 Table 6.1.2.1.1-2 default table (16 entries)
  uint8_t tda_start[16];     ///< per-index PUSCH start symbol S
  uint8_t tda_length[16];    ///< per-index PUSCH symbol count L
  uint8_t tda_mapping[16];   ///< per-index mapping type, 0 = typeA, 1 = typeB
  uint8_t tda_k2[16];        ///< per-index slot offset k2

  // ---- DM-RS (UL) ----
  int dmrs_config_type;      ///< 0 = type1 (default), 1 = type2
  int dmrs_add_pos;          ///< dmrs-AdditionalPosition column 0..3; <0 = default (2 = pos2)
  int dmrs_max_length;       ///< maxLength 1 or 2; <=0 = default (1)

  // ---- waveform / identities ----
  int transform_precoding;   ///< 0 = disabled (default, CP-OFDM), 1 = enabled (DFT-s-OFDM)
  int mcs_table;             ///< 0 = qam64 (default), 1 = qam256, 2 = qam64LowSE, 3..5 = TP variants
  int data_scrambling_id;    ///< nid_pusch; <0 = use the PCI
  int ul_dmrs_scrambling_id; ///< <0 = use the PCI
  uint16_t phy_cell_id;      ///< PCI, the fallback for both identities above

  // ---- per-field bit widths, in TS 38.212 7.3.1.1.2 PACKER order (see the note below); -1 = the
  // documented default. The packer order is fill_dci_pdu_rel15()'s NR_UL_DCI_FORMAT_0_1 case
  // (gNB_scheduler_primitives.c), NOT nr_dci_size()'s accumulation order -- the two differ (size
  // adds HARQ before the carrier indicator; the packer emits the carrier indicator first). The
  // totals agree; the per-field OFFSETS follow the packer, and offsets are what a decoder needs. ----
  int carrier_indicator_bits;   ///< default 0 (no cross-carrier scheduling)
  int ul_sul_bits;              ///< default 0 (no supplementary uplink)
  int bwp_indicator_bits;       ///< default 0 (a single configured UL BWP)
  int freq_hopping_bits;        ///< default 0 (frequencyHopping not configured)
  int harq_pid_bits;            ///< default 4; 5 with harq-ProcessNumberSizeDCI-0-1-r17
  int dai1_bits;                ///< default 2 (dynamic HARQ-ACK codebook)
  int dai2_bits;                ///< default 0 (no CBG transmission)
  int sri_bits;                 ///< default 0 (a single SRS resource in the set)
  int precoding_info_bits;      ///< default 0 (single UE TX port / rank 1)
  int antenna_ports_bits;       ///< default 2 (dmrs type1, maxLength1, CP-OFDM)
  int srs_request_bits;         ///< default 2; 3 with a supplementary uplink
  int csi_request_bits;         ///< default 0 (= reportTriggerSize)
  int cbg_bits;                 ///< default 0 (CBGTI)
  int ptrs_dmrs_bits;           ///< default 0 (no PTRS, or maxRank 1, or transform precoding on)
  int beta_offset_bits;         ///< default 0 (semi-static betaOffsets)
  int dmrs_seq_init_bits;       ///< default 1 (transform precoding disabled)
} nr_pdcch_blind_ul_opts_t;

/// Total DCI-0_1 payload width implied by `opts`. Its job is to CHECK a configuration against the
/// live-verified length, exactly as nr_pdcch_blind_dci_size_ex() does for 1_1: if the two disagree
/// the per-field widths are wrong and every field after the frequency-domain assignment is read
/// from the wrong offset. Returns 0 when opts is NULL or bwp_size < 1.
/* Non-aborting standards-table lookup for joint interpretation hypotheses. */
int32_t nr_pdcch_blind_ul_dmrs_mask(uint8_t num_symbols, uint8_t start_symbol,
    int mapping_type_is_b, int add_pos, int max_length, uint8_t dmrs_typeA_position);

uint16_t nr_pdcch_blind_dci01_size(const nr_pdcch_blind_ul_opts_t* opts);

bool nr_pdcch_blind_decode_01_mode(bool automatic, const int16_t *llr, uint8_t aggregation_level,
                                   uint16_t dci_length, const nr_pdcch_blind_ul_opts_t *opts,
                                   uint16_t rnti_min, uint16_t rnti_max,
                                   nr_pdcch_blind_ul_result_t *out);

/* Full CRC/UL-identifier recovery, independent of field widths and PUSCH semantics.
 * Success is raw-bit evidence, not a usable grant. */
bool nr_pdcch_blind_decode_raw_01(const int16_t *llr, uint8_t aggregation_level,
                                uint16_t dci_length, uint16_t rnti_min, uint16_t rnti_max,
                                nr_pdcch_blind_ul_result_t *out);
bool nr_pdcch_blind_extract_01(uint64_t payload, uint16_t dci_length, uint16_t rnti,
                             const nr_pdcch_blind_ul_opts_t *opts, nr_pdcch_blind_ul_result_t *out);

/**
 * @brief Decode one PDCCH candidate blindly as DCI format 0_1 and extract its PUSCH grant.
 *
 * Same contract as the DL entry points: ONE RNTI-independent polar decode, the CRC-recovered value
 * range-checked rather than compared, `out` filled unconditionally with `plausible` saying whether
 * to trust it. `out->raw_payload` is filled even on rejection (see the section comment).
 *
 * @param llr          descrambled/demapped LLRs, length dci_length
 * @param aggregation_level candidate aggregation level
 * @param dci_length   payload width in use -- the live-verified length for the deployment, which is
 *                     NOT assumed equal to nr_pdcch_blind_dci01_size(opts); the two are cross-checked
 *                     by the caller at start-up, and this function trusts the caller.
 * @param opts         deployment facts; must not be NULL (there is no useful spec default here)
 * @param rnti_min/max plausible dynamic RNTI range
 * @param[out] out     filled unconditionally; check out->plausible
 */
bool nr_pdcch_blind_decode_and_extract_01(const int16_t* llr,
                                          uint8_t        aggregation_level,
                                          uint16_t       dci_length,
                                          const nr_pdcch_blind_ul_opts_t* opts,
                                          uint16_t       rnti_min,
                                          uint16_t       rnti_max,
                                          nr_pdcch_blind_ul_result_t* out);

/**
 * @brief Interpret an ALREADY-DECODED format-1_0-sized payload as DCI format 0_0.
 *
 * Formats 0_0 and 1_0 are size-aligned by TS 38.212 7.3.1.0, so the 1_0 scan's polar decode already
 * produced this payload -- there is no second decode, and this function does not do one. It is the
 * branch the 1_0 path takes when the identifier bit reads 0 instead of rejecting outright.
 *
 * @param payload      the decoded payload word, right-aligned to dci_length
 * @param dci_length   the width it was decoded at
 * @param crc_rnti     the CRC-recovered RNTI (already range-checked by the caller)
 * @param opts         deployment facts; must not be NULL
 * @param[out] out     filled unconditionally; check out->plausible
 */
bool nr_pdcch_blind_extract_00(uint64_t       payload,
                               uint16_t       dci_length,
                               uint16_t       crc_rnti,
                               const nr_pdcch_blind_ul_opts_t* opts,
                               nr_pdcch_blind_ul_result_t* out);


/* Phase 3 Technique B: bootstrap the dedicated C-RNTI from sightings the accept path already
 * classifies (nr_blind_rnti_class_t). Recording is idempotent and RT-safe (no allocation, no
 * lock) -- call it from the same point the accept path already logs DCIGT. */
/** One admissible CORESET extent hypothesis, in 6-RB window indices (inclusive). */
typedef struct { int first_w; int last_w; } nr_pdcch_extent_cand_t;

/** Enumerate contiguous CORESET extents containing the observed occupancy.
 * Offset and width both vary; the legacy snap is only the first hypothesis.
 * Returns the number written, capped by caller capacity (runtime reserves the complete catalog). */
/** TS 38.211 7.4.1.1.2 DM-RS symbol mask, or -1 if the combination is illegal. Exported for
 * Technique D: a payload-interpretation hypothesis changes the TDA, so the mask that goes with it
 * has to be recomputed rather than reused from the DCI extraction. */
int32_t nr_pdcch_blind_dmrs_mask(int dmrs_TypeA_Position, int NrOfSymbols, int startSymbol,
                                 int mapping_type_is_b, int add_pos, int length);

int nr_pdcch_extent_candidates(int first_w, int last_w, int nw_total,
                               nr_pdcch_extent_cand_t* out, int max_out);

/** Build a complete extent catalogue from independent recurrent occupancy peaks. Peaks are
 * hypotheses for separate CORESETs, so no candidate is forced to span all of them. The recurrent
 * single-window and common-width hypotheses are ordered first; every contiguous interval remains
 * in the fallback, making an imperfect oracle unable to exclude the true geometry. */
int nr_pdcch_extent_candidates_multi(const int *seed_w, int nseed, int nw_total,
                                     nr_pdcch_extent_cand_t *out, int max_out);

/** One CCE-to-REG mapping hypothesis for a dedicated CORESET (TS 38.211 7.3.2.2): bundle = 0 is
 *  non-interleaved; otherwise (L, R, n_shift) with n_shift already reduced modulo N_REG/L. */
typedef struct { uint8_t bundle; uint8_t interleaver; uint16_t shift; } nr_pdcch_map_cand_t;

/** Enumerate the legal CCE-to-REG mappings of a CORESET of span_rb x duration, decided by DCI CRC
 *  evidence exactly like the extent hypotheses. Order: non-interleaved first, then for each legal
 *  (L, R) -- L in {2,6} (duration 1,2) / {3,6} (duration 3), R in {2,3,6}, N_REG divisible by L*R --
 *  every distinct shift modulo N_REG/L, the PCI's residue first and 0 second: the PCI is one
 *  hypothesis, never the seeded answer. Returns the number written (capped by max_out). */
int nr_pdcch_map_candidates(int span_rb, int duration, int pci, nr_pdcch_map_cand_t *out, int max_out);

/** Phase 3 Technique A, extent verification. Call once per candidate-bearing occasion after the
 * footprint is found: it scores the currently applied CORESET extent by whether Technique B
 * confirms a C-RNTI under it, and advances to the next admissible extent hypothesis if not.
 * Returns true when it has just changed the applied extent. */
bool nr_pdcch_blind_monitor_autodiscover_extent_step(uint32_t abs_slot);

/** Maximum simultaneously tracked UEs. A passive receiver hears every UE on the cell, so this is
 * the ceiling on how many it can follow at once, not a property of the deployment. */
#define NR_PDCCH_BLIND_MAX_UE 16

/* Publish pdsch-ConfigCommon's PDSCH TDRA list, decoded to (S, L, mappingType) triples.
 * SI-RNTI's own SIB1 grants deliberately ignore it (the list travels inside SIB1), but RA-RNTI,
 * TC-RNTI and C-RNTI-in-CSS are all sized from it -- and with count 0 they silently fall back to
 * the spec default table, which is a different allocation on any cell that configures its own. */
void nr_pdcch_blind_monitor_set_tda_common(const uint8_t *start, const uint8_t *len,
                                           const uint8_t *map, int n);

void nr_pdcch_blind_rnti_bootstrap_record(uint16_t rnti, uint8_t rnti_class, uint32_t abs_slot);

/* Same, but marks the sighting TRUSTED: it came from a CORESET whose mapping is PROVEN (CORESET#0,
 * confirmed by SIB1 decodes) rather than from an unverified hypothesis. A trusted entry is live at
 * ONE sighting -- see boot_entry_live() for why the two-sighting rule cannot apply to TC-RNTI. */
/** Tag currently-lit CORESET windows with an accepted C-RNTI, so a window can be tied to a UE --
 *  the test for whether each UE has its own dedicated CORESET. Diagnostic; consumes no decision. */
void nr_pdcch_blind_monitor_note_rnti_for_windows(uint16_t rnti);

void nr_pdcch_blind_rnti_bootstrap_record_trusted(uint16_t rnti, uint8_t rnti_class, uint32_t abs_slot);
/* The RAR chain (RA-RNTI decomposition -> TB CRC -> TC-RNTI): live at ONE sighting. */
void nr_pdcch_blind_rnti_bootstrap_record_verified(uint16_t rnti, uint8_t rnti_class, uint32_t abs_slot);
/* Five changing payloads for the same (CORESET,RNTI,length), each from a distinct OTA occasion. */
void nr_pdcch_blind_rnti_bootstrap_record_corroborated(uint16_t rnti, uint8_t rnti_class,
                                                       uint32_t abs_slot);

/** All currently confirmed, non-stale C-RNTIs. Returns how many were written. */
int nr_pdcch_blind_monitor_confirmed_rnti_set(uint32_t now_abs_slot, uint16_t *out, int max_out);
/* Subset whose identity came from a CRC-valid RAR chain. Unlike recurrence-only entries, these are
 * safe inputs to the UE-specific CCE hash before a dedicated CORESET has been discovered. */
int nr_pdcch_blind_monitor_verified_rnti_set(uint32_t now_abs_slot, uint16_t *out, int max_out);
/* Confirmed C-RNTIs only. TC-RNTIs from CFRA are not operational USS identities. */
int nr_pdcch_blind_monitor_dedicated_rnti_set(uint32_t now_abs_slot, uint16_t *out, int max_out);
/** Is this RNTI a confirmed, non-stale UE? */
bool nr_pdcch_blind_monitor_rnti_confirmed(uint32_t now_abs_slot, uint16_t rnti);

/* Non-zero (true) when a persistence-confirmed C-RNTI or TC-RNTI exists and is not stale as of
 * now_abs_slot (the caller's own current absolute slot -- this function has no other way to know
 * "now"). age_slots_out (now_abs_slot - last sighting) is filled only on a true return. */
bool nr_pdcch_blind_monitor_confirmed_rnti(uint32_t now_abs_slot, uint16_t* rnti_out, uint8_t* class_out,
                                           uint32_t* age_slots_out);

/* Formats the raw bootstrap table (rnti:sightings,age plus used/live counts) into @p buf.
 * Diagnostic only: distinguishes "no accepts" from "accepts that never repeat". */
int nr_pdcch_blind_rnti_bootstrap_dump(uint32_t now_abs_slot, char *buf, int buflen);

/* Test-only: clears bootstrap state between gtest cases. Not for RT use. */
void nr_pdcch_blind_rnti_bootstrap_reset_for_test(void);
/** Producer-thread-only geometry epoch and fresh-evidence interface. */
uint64_t nr_pdcch_blind_monitor_autodiscover_generation(void);
bool nr_pdcch_blind_monitor_autodiscover_extent_verified(void);
void nr_pdcch_blind_monitor_autodiscover_observe(uint16_t rnti, uint32_t slot, uint64_t payload);
void nr_pdcch_blind_monitor_autodiscover_reset(void);

/** Multi-candidate-per-occasion lookahead (2026-09-16, PDCCH_GPU_BATCH_HANDOVER.md): the extent/
 * mapping search above tests ONE (extent, mapping) hypothesis at a time, each given
 * NR_PDCCH_EXTENT_VERIFY_OCC real occasions -- measured live, decode (Polar SCL+CRC) costs 10x
 * fep_llr and 99% of occasions already exceed the slot budget at a single hypothesis, so
 * WALL-CLOCK time to exhaust up to 1035 extents is the bottleneck, not compute. These lanes run
 * K-1 ADDITIONAL hypotheses per real occasion (rt.c reuses the one FFT/LLR pass already paid for
 * that occasion), each with its OWN NR_PDCCH_EXTENT_VERIFY_OCC dwell -- no candidate's budget
 * shrinks, K candidates just complete their full dwell in ~1/K the wall-clock time. Whichever lane
 * (or the primary) verifies first wins and its geometry becomes g_cfg, exactly as an unbatched
 * search would have found eventually. Off by default (ISAC_PDCCH_EXTENT_BATCH unset or 1); set it
 * to K to run K-1 lanes. */
/* Raised 15 -> 63 (2026-09-20) together with the per-occasion LLR cache in
 * nr_pdcch_blind_monitor_rt.c. Before that cache each lane redid the FEP, a full-symbol memcpy per
 * antenna and the channel estimation, so K lanes cost K x ~98 % of an occasion and 16 was already
 * near the RT budget (135+217 us of 632 us at K=1). With the cache, lanes sharing an extent -- which
 * is the normal case, the catalogue walks 271 mappings per extent -- share one LLR computation, so
 * the marginal lane costs only its deinterleave+decode.
 * Why it matters: the search is 133 extents x 271 mappings ~= 36k hypotheses, and convergence needs
 * (36k / K) x occasions_per_hypothesis occasions of AIR TIME. K is the only term we control. */
#define NR_PDCCH_LOOKAHEAD_MAX 127
typedef struct {
  bool valid;             // this lane owns a live candidate this call
  bool fast_length_only;  // bounded bank-length pass; exhaustive lengths follow in the next lap
  bool al1_only;          // AL1 cover lap: scan aggregation level 1 only (ISAC_AL1_COVER=1)
  int  rb_offset;
  int  freq_domain;       // span, in 6-RB windows
  int  reg_bundle_size;
  int  interleaver_size;
  int  shift_index;
} nr_pdcch_lookahead_geom_t;

/** Configured lookahead lane COUNT (ISAC_PDCCH_EXTENT_BATCH - 1, clamped to
 *  [0, NR_PDCCH_LOOKAHEAD_MAX]; 0 = feature off, the default). Read once from the environment. */
int nr_pdcch_blind_lookahead_count(void);
/** This lane's currently applied geometry. False if the extent/mapping search is not running
 *  (footprint not found yet, already verified, or the lane index is unused this run). */
bool nr_pdcch_blind_lookahead_get(int lane, nr_pdcch_lookahead_geom_t *out);
/** Same evidence contract as nr_pdcch_blind_monitor_autodiscover_observe(), scoped to one lookahead
 *  lane. Returns true iff THIS call just verified the lane's geometry -- the caller must then call
 *  nr_pdcch_blind_monitor_autodiscover_set_dci_length() with the lane's own found length, since
 *  g_cfg's dci_length_override is not touched here (rt.c owns per-lane length-sweep state). */
bool nr_pdcch_blind_lookahead_observe(int lane, uint16_t rnti, uint32_t slot, uint64_t payload);
/** Bump this lane's occasion counter and advance it to its next candidate if its
 *  NR_PDCCH_EXTENT_VERIFY_OCC dwell just expired. Call once per real occasion this lane was offered
 *  a decode attempt, whether or not it produced evidence. */
void nr_pdcch_blind_lookahead_step(int lane);
/** Immediately retire this lane's current candidate without waiting out its NR_PDCCH_EXTENT_VERIFY_OCC
 *  dwell -- mirrors nr_pdcch_blind_monitor_autodiscover_retry() for the primary, for the case where a
 *  lane's own DCI-length sweep gives up (AUTODISCOVER_LENGTH_SWEEP_MAX_OCCASIONS) well before that. */
void nr_pdcch_blind_lookahead_retry(int lane);

#ifdef __cplusplus
}
#endif

#endif // NR_PDCCH_BLIND_MONITOR_H
