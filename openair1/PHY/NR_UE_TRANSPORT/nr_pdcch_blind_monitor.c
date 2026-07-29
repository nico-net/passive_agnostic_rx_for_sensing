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

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c
 * \brief Blind PDCCH/DCI-1_1 decode + field extraction. See the header for scope.
 *
 * REUSE VS DUPLICATION, stated once here rather than at every call site:
 *  - polar_decoder_int16() / nr_polar_params() (openair1/PHY/CODING/nrPolar_tools): reused as-is.
 *    Freestanding, mutex-protected internal parameter cache, no RT-path coupling.
 *  - get_dl_tda_info() / fill_dmrs_mask() (openair2/LAYER2/NR_MAC_COMMON/nr_mac_common.c): reused
 *    as-is, called with dl_BWP=NULL / pdsch_Config=NULL -- both are freestanding functions with an
 *    EXPLICIT, already-existing branch for exactly this "no dedicated RRC context" case (get_dl_tda_info
 *    falls to the spec-default TDRA table; fill_dmrs_mask's own comment: "in case of DCI FORMAT 1_0
 *    or dedicated pdsch config not received additionposition = pos2, len1 should be used"). Both
 *    compile into the lean MAC_NR_COMMON target.
 *  - NRRIV2BW() / NRRIV2PRBOFFSET() (common/utils/nr/nr_common.c): reused as-is (lean nr_common
 *    target, zero ASN.1 dependency).
 *  - nr_ue_process_dci_freq_dom_resource_assignment() (openair2/LAYER2/NR_MAC_UE/nr_ue_procedures.c):
 *    NOT reused, despite doing exactly the RIV math we need -- it compiles into NR_L2_UE, which
 *    additionally links nr_rlc/nr_nas/full ASN.1 RRC processing, far heavier than this module or its
 *    offline test should need to pull in for a ~10-line RIV computation. Reimplemented locally
 *    (nrriv_to_prb_alloc() below), calling the same two lean primitives that function itself calls.
 *  - Antenna-port Table 7.3.1.2.2-1 (openair2/LAYER2/NR_MAC_UE/mac_tables.c's
 *    set_antenna_port_parameters()): NOT reused, same reasoning (that file compiles into NR_L2_UE
 *    too). The table itself is a 3GPP spec constant, not deployment logic -- duplicated below,
 *    restricted to the single row this module's fixed DMRS assumption set ever uses
 *    (dmrs-Type=1, maxLength=1, one codeword).
 *  - nr_dci_size() (nr_mac_common.c): NOT called -- see nr_pdcch_blind_dci_size()'s own comment.
 *  - nr_ue_process_dci_dl_11() (nr_ue_procedures.c): NOT reused, and must never be -- it writes into
 *    the live per-slot dl_config_list via get_dl_config_request(mac, slot), shared mutable MAC
 *    scheduler state this offline/blind path has no business touching.
 *
 * FIELD BIT-WIDTHS: nr_pdcch_blind_dci_size() computes these directly from nr_mac_common.c's
 * NR_DL_DCI_FORMAT_1_1 case in nr_dci_size(), NOT independently re-derived from the TS 38.212 spec
 * text -- every fixed-width assumption below is traceable to a specific confirmed fact about this
 * codebase's gNB (see the header's file-level comment and this project's TOTAL_PASSIVE_UE_HANDOVER.md
 * Phase 3 section), with ONE unverified-this-session assumption flagged where it occurs
 * (pdsch_CGB_Transmission assumed NULL/off -- not independently re-checked, only inferred from no
 * assignment being found in nr_radio_config.c).
 */

#include "nr_pdcch_blind_monitor.h"
#include "nr_pdcch_blind_monitor_rt.h" // nr_pdcch_blind_monitor_cfg_t + get_cfg() accessor (implemented
                                       // below); the RT tap itself lives in nr_pdcch_blind_monitor_rt.c
                                       // -- see that header's file comment for why the split exists.

#include <math.h>
#include <string.h>

#include "common/config/config_userapi.h"
#include "common/utils/LOG/log.h"
#include "common/utils/nr/nr_common.h"

#include "PHY/CODING/nrPolar_tools/nr_polar_dci_defs.h"
#include "PHY/CODING/nrPolar_tools/nr_polar_defs.h"

#include "openair2/LAYER2/NR_MAC_COMMON/nr_mac_common.h"

// ---------------------------------------------------------------------------------------------
// [sensing] pdcch_blind_monitor_* config surface. Parsed but NOT consumed by
// nr_pdcch_blind_dci_size()/nr_pdcch_blind_decode_and_extract() (both pure functions, driven
// directly by their arguments -- see the offline test); consumed by the RT tap in
// nr_pdcch_blind_monitor_rt.c via nr_pdcch_blind_monitor_get_cfg() below.
// ---------------------------------------------------------------------------------------------
static nr_pdcch_blind_monitor_cfg_t g_cfg;
static int                          g_parsed  = 0;
static int                          g_enabled = 0;

const nr_pdcch_blind_monitor_cfg_t* nr_pdcch_blind_monitor_get_cfg(void)
{
  return &g_cfg;
}

static int parse_coreset(const char* s)
{
  return sscanf(s,
               "%d:%d:%d:%d:%d:%hu",
               &g_cfg.coreset_freq_domain,
               &g_cfg.coreset_duration,
               &g_cfg.coreset_reg_bundle_size,
               &g_cfg.coreset_interleaver_size,
               &g_cfg.coreset_shift_index,
               &g_cfg.coreset_pdcch_dmrs_scrambling_id)
         == 6;
}

static int parse_ss(const char* s)
{
  return sscanf(s,
               "%d:%d:%d:%d:%d:%d:%d:%d",
               &g_cfg.ss_monitoring_slot_periodicity,
               &g_cfg.ss_monitoring_slot_offset,
               &g_cfg.ss_duration,
               &g_cfg.ss_first_symbol,
               &g_cfg.ss_al_candidates[0],
               &g_cfg.ss_al_candidates[1],
               &g_cfg.ss_al_candidates[2],
               &g_cfg.ss_al_candidates[3])
         == 8;
}

static int parse_bwp(const char* s)
{
  g_cfg.dci_length_override = 0; // optional trailing field; sscanf below only overwrites it if present
  const int n = sscanf(s, "%d:%d:%d:%d", &g_cfg.bwp_start, &g_cfg.bwp_size, &g_cfg.dmrs_typeA_position,
                       &g_cfg.dci_length_override);
  return n == 3 || n == 4;
}

static int parse_rnti_range(const char* s)
{
  return sscanf(s, "%hu:%hu", &g_cfg.rnti_min, &g_cfg.rnti_max) == 2;
}

// energy_min:persist_k:persist_window_ms:min_snr_lin -- see nr_pdcch_blind_monitor_rt.h's field
// comments for what each gate does and why the RNTI range alone (parse_rnti_range above) cannot
// carry this on its own.
static int parse_noise_gates(const char* s)
{
  return sscanf(s, "%f:%d:%d:%f", &g_cfg.energy_min, &g_cfg.rnti_persist_k, &g_cfg.rnti_persist_window_ms,
               &g_cfg.min_snr_lin)
         == 4;
}

void nr_pdcch_blind_monitor_init(void)
{
  if (g_parsed) {
    return;
  }
  g_parsed = 1;

  memset(&g_cfg, 0, sizeof(g_cfg));
  g_cfg.rnti_min = NR_PDCCH_BLIND_RNTI_MIN_DEFAULT;
  g_cfg.rnti_max = NR_PDCCH_BLIND_RNTI_MAX_DEFAULT;
  // Noise-floor gates default ON (not opt-in): the RNTI range above is, by necessity, nearly the
  // whole space (see its own comment) and cannot alone keep the false-accept rate down at this
  // scan's trial volume -- see nr_pdcch_blind_monitor_rt.h's field comments for what each number
  // means. Values are a reasonable starting point, NOT independently tuned against a live deployment
  // this session -- re-verify against measured accept/RNTI-cross-check rates the same way
  // dci_length_override was, if they turn out too strict (losing real grants) or too loose.
  g_cfg.energy_min             = 2.0f;
  g_cfg.rnti_persist_k         = 2;
  g_cfg.rnti_persist_window_ms = 500;
  g_cfg.min_snr_lin            = 4.0f; // ~6 dB

  char*     p_coreset = NULL;
  char*     p_ss       = NULL;
  char*     p_bwp       = NULL;
  char*     p_rnti_range = NULL;
  char*     p_noise_gates = NULL;
  paramdef_t params[] = {
      {"pdcch_blind_monitor_coreset",
        "Dedicated CORESET geometry for blind PDCCH monitoring; "
        "num_freq_groups:duration:reg_bundle_size:interleaver_size:shift_index:pdcch_dmrs_scrambling_id "
        "(num_freq_groups = contiguous 6-PRB groups from group 0, e.g. 16 for a 96-PRB CORESET)",
        0, .strptr = &p_coreset, .defstrval = "", TYPE_STRING, 0},
      {"pdcch_blind_monitor_ss",
        "Dedicated SearchSpace geometry; "
        "monitoring_slot_periodicity:monitoring_slot_offset:duration:first_symbol:"
        "al2_cand:al4_cand:al8_cand:al16_cand",
        0, .strptr = &p_ss, .defstrval = "", TYPE_STRING, 0},
      {"pdcch_blind_monitor_bwp",
        "Active DL BWP for sizing; bwp_start:bwp_size:dmrs_typeA_position[:dci_length_override] "
        "(dci_length_override optional, 0/omitted = use the computed formula; see "
        "nr_pdcch_blind_monitor_rt.h's dci_length_override comment for why a live-verified override "
        "is often needed)",
        0, .strptr = &p_bwp, .defstrval = "", TYPE_STRING, 0},
      {"pdcch_blind_monitor_rnti_range", "Plausible RNTI range; rnti_min:rnti_max", 0,
        .strptr = &p_rnti_range, .defstrval = "", TYPE_STRING, 0},
      {"pdcch_blind_monitor_noise_gates",
        "False-accept reduction gates; energy_min:rnti_persist_k:rnti_persist_window_ms:min_snr_lin "
        "(any field <=0/<=1 as documented in nr_pdcch_blind_monitor_rt.h disables that specific gate; "
        "omit the whole line to use the compiled-in defaults, not to disable all gates)",
        0, .strptr = &p_noise_gates, .defstrval = "", TYPE_STRING, 0},
  };
  config_get(config_get_if(), params, (int)(sizeof(params) / sizeof(params[0])), "sensing");

  if (p_coreset == NULL || p_coreset[0] == '\0' || p_ss == NULL || p_ss[0] == '\0' || p_bwp == NULL
      || p_bwp[0] == '\0') {
    return; // monitor disabled -- coreset/ss/bwp are all required together
  }
  if (!parse_coreset(p_coreset)) {
    LOG_E(PHY, "SENSING: malformed pdcch_blind_monitor_coreset '%s'\n", p_coreset);
    return;
  }
  if (!parse_ss(p_ss)) {
    LOG_E(PHY, "SENSING: malformed pdcch_blind_monitor_ss '%s'\n", p_ss);
    return;
  }
  if (!parse_bwp(p_bwp)) {
    LOG_E(PHY, "SENSING: malformed pdcch_blind_monitor_bwp '%s'\n", p_bwp);
    return;
  }
  if (p_rnti_range != NULL && p_rnti_range[0] != '\0' && !parse_rnti_range(p_rnti_range)) {
    LOG_E(PHY, "SENSING: malformed pdcch_blind_monitor_rnti_range '%s'; using default %u-%u\n", p_rnti_range,
          NR_PDCCH_BLIND_RNTI_MIN_DEFAULT, NR_PDCCH_BLIND_RNTI_MAX_DEFAULT);
  }
  if (p_noise_gates != NULL && p_noise_gates[0] != '\0' && !parse_noise_gates(p_noise_gates)) {
    LOG_E(PHY, "SENSING: malformed pdcch_blind_monitor_noise_gates '%s'; using compiled-in defaults\n",
          p_noise_gates);
  }

  g_enabled = 1;
  LOG_I(PHY,
        "SENSING: blind PDCCH monitor configured: coreset(num_groups=%d duration=%d reg_bundle=%d "
        "interleaver=%d shift=%d scramb=%u) ss(period=%d offset=%d duration=%d first_symb=%d "
        "al_cand=[%d,%d,%d,%d]) bwp=[%d..%d) dmrs_typeA_pos=%d rnti_range=[%u..%u] "
        "noise_gates(energy_min=%.2f persist_k=%d persist_window_ms=%d min_snr_lin=%.2f)\n",
        g_cfg.coreset_freq_domain, g_cfg.coreset_duration, g_cfg.coreset_reg_bundle_size,
        g_cfg.coreset_interleaver_size, g_cfg.coreset_shift_index, g_cfg.coreset_pdcch_dmrs_scrambling_id,
        g_cfg.ss_monitoring_slot_periodicity, g_cfg.ss_monitoring_slot_offset, g_cfg.ss_duration,
        g_cfg.ss_first_symbol, g_cfg.ss_al_candidates[0], g_cfg.ss_al_candidates[1], g_cfg.ss_al_candidates[2],
        g_cfg.ss_al_candidates[3], g_cfg.bwp_start, g_cfg.bwp_start + g_cfg.bwp_size,
        g_cfg.dmrs_typeA_position, g_cfg.rnti_min, g_cfg.rnti_max, g_cfg.energy_min, g_cfg.rnti_persist_k,
        g_cfg.rnti_persist_window_ms, g_cfg.min_snr_lin);
}

int nr_pdcch_blind_monitor_enabled(void)
{
  return g_enabled;
}

// ---------------------------------------------------------------------------------------------
// DCI-1_1 payload bit-width, MVP fixed assumption set (see file-level comment).
// ---------------------------------------------------------------------------------------------
uint16_t nr_pdcch_blind_dci_size(uint16_t bwp_size)
{
  // Fixed bits, independent of bwp_size -- each traceable to nr_mac_common.c's NR_DL_DCI_FORMAT_1_1
  // case (line numbers as read 2026-07-28; re-check if that file's field list changes):
  //   format identifier(1) + carrier indicator(0, no cross-carrier) + bwp indicator(1, n_dl_bwp=1)
  // + time domain assignment(4, default 16-row TDRA table) + vrb-to-prb(0) + prb bundling(0)
  // + rate matching(0) + zp csi-rs trigger(0) [last 4: all gated on pdsch_Config!=NULL, which this
  //   MVP's blind assumption treats as NULL, mirroring format 1_0's "dedicated config not received"
  //   regime]
  // + MCS(5)+NDI(1)+RV(2)=8 + TB2(0, one codeword) + harq pid(4, default) + DAI(2, dynamic codebook)
  // + TPC PUCCH(2) + PUCCH resource indicator(3)
  // + PDSCH-to-HARQ timing indicator(3, ceil(log2(8)) -- this cell's dl_DataToUL_ACK list has 8
  //   entries: min_rxtxtime=6 gives delays 6..13, all <=15, so the full 0..7 loop completes)
  // + antenna ports(4, ceil(log2(12)) -- Table 7.3.1.2.2-1, dmrs-Type=1/maxLength=1)
  // + TCI(0, tci_PresentInDCI=NULL) + SRS request(2, supplementaryUplink=NULL -> 2 not 3)
  // + CBGTI(0)+CBGFI(0) [pdsch_CGB_Transmission assumed NULL -- NOT independently re-verified this
  //   session, only inferred from no assignment found]
  // + DMRS sequence init(1)
  // = 1+0+1+4+0+0+0+0+8+0+4+2+2+3+3+4+0+2+0+0+1 = 35
  static const uint16_t FIXED_BITS = 35;

  if (bwp_size < 1) {
    return 0;
  }
  // Freq domain assignment (RIV, resource-allocation-type-1 -- the only branch this gNB's fixed
  // resourceAllocationType ever produces): ceil(log2(N_RB*(N_RB+1)/2)), matching nr_mac_common.c's
  // dci_pdu->frequency_domain_assignment.nbits formula for the pdsch_Config==NULL branch.
  const double riv_span = ((double)bwp_size * (double)(bwp_size + 1)) / 2.0;
  const uint16_t riv_bits = (uint16_t)ceil(log2(riv_span));

  return FIXED_BITS + riv_bits;
}

// ---------------------------------------------------------------------------------------------
// Antenna-port Table 7.3.1.2.2-1 (TS 38.212): 1 codeword, dmrs-Type=1, maxLength=1 -- the only row
// this module's fixed DMRS assumption set (dmrs_Type=NULL, maxLength=NULL) ever indexes.
// Columns: {n_dmrs_cdm_groups, port0_active, port1_active, port2_active, port3_active}.
// Duplicated from openair2/LAYER2/NR_MAC_UE/mac_tables.c's table_7_3_2_3_3_1 (that file compiles
// into the heavy NR_L2_UE target -- see the file-level comment for why this is duplicated rather
// than linked).
// ---------------------------------------------------------------------------------------------
static const uint8_t g_table_7_3_2_3_3_1[12][5] = {
    {1, 1, 0, 0, 0}, {1, 0, 1, 0, 0}, {1, 1, 1, 0, 0}, {2, 1, 0, 0, 0},
    {2, 0, 1, 0, 0}, {2, 0, 0, 1, 0}, {2, 0, 0, 0, 1}, {2, 1, 1, 0, 0},
    {2, 0, 0, 1, 1}, {2, 1, 1, 1, 0}, {2, 1, 1, 1, 1}, {2, 1, 0, 1, 0},
};

/// Read `nbits` starting at the bit position just below `*pos` (spec/TS-38.212-field order, MSB
/// first) out of a single 64-bit payload word, then advance `*pos` past them. Payloads sized by
/// nr_pdcch_blind_dci_size() are always well under 64 bits (46-48 for the BWP sizes this project
/// uses), so a single uint64_t word (matching polar_decoder_int16()'s out[0]) is sufficient --
/// mirrors openair2/LAYER2/NR_MAC_UE/nr_ue_procedures.c's readBits()/EXTRACT_DCI_ITEM exactly,
/// just operating on a uint64_t directly instead of a byte-pointer-cast-to-uint64_t.
static uint32_t read_field(uint64_t payload, int* pos, int nbits)
{
  if (nbits == 0) {
    return 0;
  }
  const uint32_t mask = (nbits >= 32) ? 0xFFFFFFFFu : ((1U << nbits) - 1);
  *pos -= nbits;
  return (uint32_t)((payload >> *pos) & mask);
}

/// Resource-allocation-type-1 (RIV) decode -- the only branch this gNB's fixed resourceAllocationType
/// ever uses (see file-level comment). Mirrors nr_ue_procedures.c's
/// nr_ue_process_dci_freq_dom_resource_assignment()'s Type-1 branch exactly (same two primitives,
/// same bound check), without linking the heavy target that function lives in.
static bool riv_to_prb_alloc(uint32_t riv, uint16_t n_RB_DLBWP, uint16_t* start_rb, uint16_t* num_rb)
{
  *num_rb   = (uint16_t)NRRIV2BW((int)riv, n_RB_DLBWP);
  *start_rb = (uint16_t)NRRIV2PRBOFFSET((int)riv, n_RB_DLBWP);
  if (*num_rb < 1 || *num_rb > n_RB_DLBWP - *start_rb) {
    return false;
  }
  return true;
}

bool nr_pdcch_blind_decode_and_extract(const int16_t* llr,
                                       uint8_t         aggregation_level,
                                       uint16_t        dci_length,
                                       uint16_t        bwp_size,
                                       uint8_t         dmrs_typeA_position,
                                       uint16_t        rnti_min,
                                       uint16_t        rnti_max,
                                       nr_pdcch_blind_result_t* out)
{
  memset(out, 0, sizeof(*out));
  out->plausible = false;

  if (dci_length == 0 || dci_length > 63 || bwp_size < 1) {
    out->reject_reason = "invalid dci_length/bwp_size argument";
    return false;
  }

  // ---- Step 1: RNTI-independent polar decode; the CRC-recovered value IS the candidate RNTI. ----
  uint64_t dci_estimation[2] = {0};
  const uint32_t crc = polar_decoder_int16((int16_t*)llr, dci_estimation, 1, NR_POLAR_DCI_MESSAGE_TYPE,
                                           dci_length, aggregation_level);

  // ---- Step 2: RNTI plausibility -- range check instead of the live path's equality check. This
  // is the entire "blind" widening; see dci_nr.c:538-541 (reference only, not modified). ----
  if (crc < rnti_min || crc > rnti_max) {
    out->rnti = (uint16_t)crc;
    out->reject_reason = "CRC-recovered value outside plausible RNTI range";
    return false;
  }
  out->rnti = (uint16_t)crc;

  // ---- Step 3: field extraction, in TS 38.212 spec order (MSB-first, matches
  // nr_mac_common.c's nr_dci_size() accumulation order and nr_ue_procedures.c's readBits()).
  // NOTE: this does NOT re-check dci_length against nr_pdcch_blind_dci_size(bwp_size) -- it used to,
  // but that comparison is WRONG whenever the caller passed a live-verified dci_length_override
  // (see nr_pdcch_blind_monitor_rt.h's field comment): the whole point of the override is that the
  // formula's bit count is known-wrong for some deployments, so gating on formula==dci_length would
  // reject every override case outright. dci_length is trusted as the caller's ground truth. Known
  // limitation: the FIELD WIDTHS below (the "= 35" fixed-bits breakdown from nr_pdcch_blind_dci_size's
  // own comment) are only independently verified to be correct in aggregate (the total matches
  // ground truth via the override); which SPECIFIC field(s) account for the 3-bit gap on the
  // deployment that needed dci_length_override=45 has NOT been isolated, so the per-field bit
  // positions below may still be misaligned for that deployment even once decode (Step 1-2) starts
  // succeeding. Flagged, not fixed -- see the Stage 1 handover note this session leaves behind. ----
  const double   riv_span = ((double)bwp_size * (double)(bwp_size + 1)) / 2.0;
  const int      riv_bits = (int)ceil(log2(riv_span));

  int            pos     = (int)dci_length;
  const uint64_t payload = dci_estimation[0];

  const uint32_t format_indicator = read_field(payload, &pos, 1);
  (void)read_field(payload, &pos, 0); // carrier indicator (0 bits, no cross-carrier scheduling)
  (void)read_field(payload, &pos, 1); // bwp indicator (1 bit, n_dl_bwp=1 -- consumed, not gated on)
  const uint32_t freq_domain_assignment = read_field(payload, &pos, riv_bits);
  const uint32_t time_domain_assignment = read_field(payload, &pos, 4);
  (void)read_field(payload, &pos, 0); // vrb-to-prb mapping
  (void)read_field(payload, &pos, 0); // prb bundling size indicator
  (void)read_field(payload, &pos, 0); // rate matching indicator
  (void)read_field(payload, &pos, 0); // zp csi-rs trigger
  const uint32_t mcs = read_field(payload, &pos, 5);
  (void)read_field(payload, &pos, 1); // NDI, unused by this extraction
  (void)read_field(payload, &pos, 2); // RV, unused by this extraction
  (void)read_field(payload, &pos, 0); // TB2 (single codeword)
  (void)read_field(payload, &pos, 4); // HARQ process number, unused by this extraction
  (void)read_field(payload, &pos, 2); // DAI, unused by this extraction
  (void)read_field(payload, &pos, 2); // TPC PUCCH
  (void)read_field(payload, &pos, 3); // PUCCH resource indicator
  (void)read_field(payload, &pos, 3); // PDSCH-to-HARQ feedback timing indicator
  const uint32_t antenna_ports = read_field(payload, &pos, 4);
  (void)read_field(payload, &pos, 0); // TCI
  (void)read_field(payload, &pos, 2); // SRS request
  (void)read_field(payload, &pos, 0); // CBGTI
  (void)read_field(payload, &pos, 0); // CBGFI
  const uint32_t dmrs_seq_init = read_field(payload, &pos, 1);

  // ---- Step 4: plausibility filter on decoded fields. This is the false-positive control the
  // widened RNTI acceptance needs -- nothing like it exists in the live (own-RNTI-trusted) path. ----
  if (format_indicator != 1) {
    out->reject_reason = "format indicator=0 (UL grant, not a PDSCH DCI)";
    return false;
  }
  if (antenna_ports >= 12) {
    out->reject_reason = "antenna_ports field outside Table 7.3.1.2.2-1's 12 valid rows";
    return false;
  }
  // MCS 28-31 (64QAM table, Table 5.1.3.1-1) are reserved for retransmissions -- a UE already knows
  // the modulation from the initial transmission in that case, so a NEW/first grant should never
  // carry one. NOT independently re-verified against this deployment's actual mcs-Table RRC config
  // this session (same caveat as several other field assumptions in this file) -- if real grants are
  // being rejected here, check whether 256QAM or the low-SE table is configured instead, which shift
  // this boundary.
  if (mcs >= 28) {
    out->reject_reason = "MCS in the reserved retransmission-only range (28-31)";
    return false;
  }
  uint16_t start_rb, num_rb;
  if (!riv_to_prb_alloc(freq_domain_assignment, bwp_size, &start_rb, &num_rb)) {
    out->reject_reason = "RIV decodes to a PRB allocation outside the BWP";
    return false;
  }

  const NR_tda_info_t tda =
      get_dl_tda_info(NULL /* dl_BWP */, 0 /* ss_type, unused when dl_BWP is NULL */, (int)time_domain_assignment,
                      dmrs_typeA_position, 1 /* mux_pattern */, TYPE_C_RNTI_, 0 /* coresetid */, false /* sib1 */);
  if (!tda.valid_tda) {
    out->reject_reason = "time_domain_assignment index invalid for the default TDRA table";
    return false;
  }

  const int16_t dmrs_mask =
      fill_dmrs_mask(NULL /* pdsch_Config */, NR_DL_DCI_FORMAT_1_1, dmrs_typeA_position, tda.nrOfSymbols,
                     tda.startSymbolIndex, tda.mapping_type, 1 /* maxLength=1 */);

  // ---- All checks passed: fill the result. ----
  out->start_rb          = start_rb;
  out->num_rb            = num_rb;
  out->start_symbol       = (uint8_t)tda.startSymbolIndex;
  out->num_symbols        = (uint8_t)tda.nrOfSymbols;
  out->dl_dmrs_symb_pos   = (uint16_t)dmrs_mask;
  out->n_dmrs_cdm_groups  = g_table_7_3_2_3_3_1[antenna_ports][0];
  out->dmrs_ports         = (uint16_t)(g_table_7_3_2_3_3_1[antenna_ports][1] | (g_table_7_3_2_3_3_1[antenna_ports][2] << 1)
                                       | (g_table_7_3_2_3_3_1[antenna_ports][3] << 2)
                                       | (g_table_7_3_2_3_3_1[antenna_ports][4] << 3));
  out->nscid              = (uint8_t)dmrs_seq_init;
  out->plausible          = true;
  out->reject_reason      = NULL;
  return true;
}

