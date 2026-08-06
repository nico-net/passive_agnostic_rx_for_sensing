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
  uint16_t    dmrs_ports;         ///< DMRS port bitmask (Table 7.3.1.2.2-1)
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
  bool        plausible;          ///< false => caller MUST discard this result
  const char* reject_reason;      ///< non-NULL iff !plausible; static string, do not free
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
  int     dmrs_add_pos;     ///< dmrs-AdditionalPosition as fill_dmrs_mask()'s column index (0..3); <0 = default (2 = pos2)
  int     dmrs_max_length;  ///< DM-RS maxLength (1 or 2); <=0 = default (1)

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

/// Total DCI-1_1 payload width implied by `opts` at `bwp_size`. With `opts == NULL` this is
/// identical to nr_pdcch_blind_dci_size(). Its real job is to CHECK a configuration: if this
/// disagrees with the live-verified `dci_length_override`, the per-field widths are wrong and every
/// field after the frequency-domain assignment will be read from the wrong offset -- the exact
/// failure described above, which a total-length-only check cannot see.
uint16_t nr_pdcch_blind_dci_size_ex(uint16_t bwp_size, const nr_pdcch_blind_extract_opts_t* opts);

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

#ifdef __cplusplus
}
#endif

#endif // NR_PDCCH_BLIND_MONITOR_H
