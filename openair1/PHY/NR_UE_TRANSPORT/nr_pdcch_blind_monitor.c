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
  // 7th field (coreset_type) is OPTIONAL so every existing 6-field config keeps working unchanged.
  g_cfg.coreset_type = 0; // 0 = PDCCH-Config (dedicated), 1 = MIB/SIB1 (CORESET0)
  const int n = sscanf(s,
               "%d:%d:%d:%d:%d:%hu:%d",
               &g_cfg.coreset_freq_domain,
               &g_cfg.coreset_duration,
               &g_cfg.coreset_reg_bundle_size,
               &g_cfg.coreset_interleaver_size,
               &g_cfg.coreset_shift_index,
               &g_cfg.coreset_pdcch_dmrs_scrambling_id,
               &g_cfg.coreset_type);
  return n == 6 || n == 7;
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
  // 5th field (energy_adapt_factor) is OPTIONAL so existing 4-field configs keep working unchanged.
  // When present and > 0 it selects the adaptive energy gate and overrides energy_min -- see
  // nr_pdcch_blind_monitor_rt.h for why an absolute energy threshold is not portable.
  g_cfg.energy_adapt_factor = 0.0f;
  const int n = sscanf(s, "%f:%d:%d:%f:%f", &g_cfg.energy_min, &g_cfg.rnti_persist_k,
                       &g_cfg.rnti_persist_window_ms, &g_cfg.min_snr_lin, &g_cfg.energy_adapt_factor);
  return n == 4 || n == 5;
}

// "S:L[:map],S:L[:map],..." -- the deployment's real pdsch-TimeDomainAllocationList, indexed by the
// DCI's time-domain-assignment field. map: 0 = typeA (default), 1 = typeB.
static int parse_tda(const char* s)
{
  int n = 0;
  const char* p = s;
  while (*p != '\0' && n < 16) {
    int start = 0, len = 0, map = 0;
    const int got = sscanf(p, "%d:%d:%d", &start, &len, &map);
    if (got < 2) {
      return 0;
    }
    if (start < 0 || start > 13 || len < 1 || start + len > 14 || map < 0 || map > 1) {
      return 0;
    }
    g_cfg.extract.tda_start[n]   = (uint8_t)start;
    g_cfg.extract.tda_length[n]  = (uint8_t)len;
    g_cfg.extract.tda_mapping[n] = (uint8_t)map;
    n++;
    const char* comma = strchr(p, ',');
    if (comma == NULL) {
      break;
    }
    p = comma + 1;
  }
  g_cfg.extract.tda_count = n;
  return n > 0;
}

// "add_pos:max_length" -- dmrs-AdditionalPosition as fill_dmrs_mask()'s column index (0=pos0,
// 1=pos1, 2=pos2, 3=pos3) and DM-RS maxLength (1 or 2).
static int parse_dmrs(const char* s)
{
  return sscanf(s, "%d:%d", &g_cfg.extract.dmrs_add_pos, &g_cfg.extract.dmrs_max_length) == 2;
}

// Per-field DCI-1_1 bit widths, in TS 38.212 payload order (nr_dci_size()'s own accumulation
// order). -1 in any position keeps this module's built-in assumption for that field. See
// nr_pdcch_blind_extract_opts_t for why getting the TOTAL right via dci_length_override is not
// sufficient.
static int parse_dci_bits(const char* s)
{
  nr_pdcch_blind_extract_opts_t* o = &g_cfg.extract;
  return sscanf(s, "%d:%d:%d:%d:%d:%d:%d:%d:%d:%d:%d:%d:%d", &o->bwp_indicator_bits, &o->vrb_to_prb_bits,
                &o->prb_bundling_bits, &o->rate_matching_bits, &o->zp_csirs_bits, &o->tb2_bits,
                &o->harq_pid_bits, &o->dai_bits, &o->pdsch_to_harq_bits, &o->antenna_ports_bits,
                &o->tci_bits, &o->srs_request_bits, &o->cbg_bits)
         == 13;
}

// "decode:mcs_table:xoverhead:rv0_only:max_per_slot" -- see the field comments in
// nr_pdcch_blind_monitor_rt.h.
static int parse_pdsch(const char* s)
{
  const int n = sscanf(s, "%d:%d:%d:%d:%d", &g_cfg.pdsch_decode, &g_cfg.pdsch_mcs_table, &g_cfg.pdsch_xoverhead,
                       &g_cfg.pdsch_rv0_only, &g_cfg.pdsch_max_per_slot);
  return n >= 1;
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
  // energy_min defaults OFF: it is an ABSOLUTE threshold in receiver-dependent units, so shipping a
  // default for it was always wrong -- 2.0 was calibrated at 106 PRB and does not carry to 273 PRB
  // or to any real OTA gain setting, which is why it ended up disabled in the configs rather than
  // retuned. The adaptive gate replaces it and CAN safely carry a default, because a multiple of the
  // measured noise floor is dimensionless.
  g_cfg.energy_min             = 0.0f;
  g_cfg.energy_adapt_factor    = 2.0f;
  g_cfg.rnti_persist_k         = 2;
  g_cfg.rnti_persist_window_ms = 500;
  g_cfg.min_snr_lin            = 4.0f; // ~6 dB
  // Spec defaults for the deployment-fact overrides: tda_count=0 keeps the default TDRA table and
  // dmrs_add_pos<0 keeps fill_dmrs_mask()'s pos2 fallback -- i.e. exactly the pre-2026-07-30
  // behaviour unless the corresponding config lines are present.
  g_cfg.extract.tda_count      = 0;
  g_cfg.extract.dmrs_add_pos   = -1;
  g_cfg.extract.dmrs_max_length = 0;
  // -1 everywhere = "use the built-in assumption", i.e. the pre-2026-07-30 hard-coded widths.
  g_cfg.extract.bwp_indicator_bits = -1;
  g_cfg.extract.vrb_to_prb_bits    = -1;
  g_cfg.extract.prb_bundling_bits  = -1;
  g_cfg.extract.rate_matching_bits = -1;
  g_cfg.extract.zp_csirs_bits      = -1;
  g_cfg.extract.tb2_bits           = -1;
  g_cfg.extract.harq_pid_bits      = -1;
  g_cfg.extract.dai_bits           = -1;
  g_cfg.extract.pdsch_to_harq_bits = -1;
  g_cfg.extract.antenna_ports_bits = -1;
  g_cfg.extract.tci_bits           = -1;
  g_cfg.extract.srs_request_bits   = -1;
  g_cfg.extract.cbg_bits           = -1;
  g_cfg.pdsch_decode           = 0;
  g_cfg.pdsch_mcs_table        = 0;
  g_cfg.pdsch_xoverhead        = 0;
  g_cfg.pdsch_rv0_only         = 1;
  g_cfg.pdsch_max_per_slot     = 1;

  char*     p_coreset = NULL;
  char*     p_ss       = NULL;
  char*     p_bwp       = NULL;
  char*     p_rnti_range = NULL;
  char*     p_noise_gates = NULL;
  char*     p_tda        = NULL;
  char*     p_dmrs       = NULL;
  char*     p_pdsch      = NULL;
  char*     p_dci_bits   = NULL;
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
      {"pdcch_blind_monitor_tda",
        "The gNB's real pdsch-TimeDomainAllocationList, indexed by the DCI's time-domain-assignment "
        "field; S:L[:mapping],S:L[:mapping],... (mapping 0=typeA default, 1=typeB). Omit to use the "
        "3GPP default TDRA table -- which is WRONG for any gNB that configures its own list, and "
        "must be set before the passive PDSCH decode can work",
        0, .strptr = &p_tda, .defstrval = "", TYPE_STRING, 0},
      {"pdcch_blind_monitor_dmrs",
        "PDSCH DM-RS config the gNB's dedicated pdsch-Config carries; add_pos:max_length "
        "(add_pos 0=pos0,1=pos1,2=pos2,3=pos3). Omit for the no-dedicated-config default (pos2, len 1)",
        0, .strptr = &p_dmrs, .defstrval = "", TYPE_STRING, 0},
      {"pdcch_blind_monitor_dci_bits",
        "Per-field DCI-1_1 bit widths for this deployment, TS 38.212 payload order: "
        "bwp_ind:vrb_to_prb:prb_bundling:rate_match:zp_csirs:tb2:harq_pid:dai:pdsch_to_harq:"
        "ant_ports:tci:srs_req:cbg (-1 = built-in default). Getting dci_length_override right is "
        "NOT enough on its own -- see nr_pdcch_blind_extract_opts_t",
        0, .strptr = &p_dci_bits, .defstrval = "", TYPE_STRING, 0},
      {"pdcch_blind_monitor_pdsch",
        "Passive data-aided PDSCH; decode:mcs_table:xoverhead:rv0_only:max_per_slot "
        "(decode 0=off, 1=decode+count CRC pass rate only, 2=also submit the reconstructed CFR)",
        0, .strptr = &p_pdsch, .defstrval = "", TYPE_STRING, 0},
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
  if (p_tda != NULL && p_tda[0] != '\0' && !parse_tda(p_tda)) {
    g_cfg.extract.tda_count = 0;
    LOG_E(PHY, "SENSING: malformed pdcch_blind_monitor_tda '%s'; falling back to the default TDRA table\n", p_tda);
  }
  if (p_dmrs != NULL && p_dmrs[0] != '\0' && !parse_dmrs(p_dmrs)) {
    g_cfg.extract.dmrs_add_pos    = -1;
    g_cfg.extract.dmrs_max_length = 0;
    LOG_E(PHY, "SENSING: malformed pdcch_blind_monitor_dmrs '%s'; falling back to pos2/len1\n", p_dmrs);
  }
  if (p_dci_bits != NULL && p_dci_bits[0] != '\0' && !parse_dci_bits(p_dci_bits)) {
    LOG_E(PHY, "SENSING: malformed pdcch_blind_monitor_dci_bits '%s'; using built-in field widths\n", p_dci_bits);
  }
  if (p_pdsch != NULL && p_pdsch[0] != '\0' && !parse_pdsch(p_pdsch)) {
    g_cfg.pdsch_decode = 0;
    LOG_E(PHY, "SENSING: malformed pdcch_blind_monitor_pdsch '%s'; passive PDSCH decode disabled\n", p_pdsch);
  }
  if (g_cfg.pdsch_max_per_slot <= 0) {
    g_cfg.pdsch_max_per_slot = 1;
  }
  // A decode built on the spec-default TDRA/DM-RS assumptions is near-certain to fail CRC on any
  // gNB that configures its own list (this project's does -- see nr_pdcch_blind_extract_opts_t).
  // Warn rather than refuse: "0% CRC pass rate" is itself a legitimate measurement to take, but it
  // must not be mistaken for a statement about the CHANNEL.
  // ---- Reconcile the per-field widths against the payload length actually in use. This is the
  // check that would have caught 2026-07-30's misalignment on day one: dci_length_override made the
  // TOTAL right while the per-field widths stayed wrong, so every field after the frequency-domain
  // assignment was read from the wrong offset and nobody noticed, because the only fields consumed
  // (RNTI, RIV allocation) happen to precede the damage. ----
  {
    const uint16_t used_len = g_cfg.dci_length_override > 0 ? (uint16_t)g_cfg.dci_length_override
                                                            : nr_pdcch_blind_dci_size((uint16_t)g_cfg.bwp_size);
    const uint16_t implied  = nr_pdcch_blind_dci_size_ex((uint16_t)g_cfg.bwp_size, &g_cfg.extract);
    if (used_len != implied) {
      LOG_W(PHY,
            "SENSING: blind PDCCH DCI field widths imply %u bits but the payload in use is %u -- every field "
            "after the frequency-domain assignment is being read from the WRONG bit offset (RNTI and the PRB "
            "allocation are still correct, which is why this can look healthy). Set "
            "pdcch_blind_monitor_dci_bits / pdcch_blind_monitor_tda to this deployment's real widths; see "
            "nr_pdcch_blind_extract_opts_t\n",
            implied, used_len);
    } else {
      LOG_I(PHY, "SENSING: blind PDCCH DCI field widths reconcile with the %u-bit payload\n", used_len);
    }
  }

  if (g_cfg.pdsch_decode > 0 && g_cfg.extract.tda_count == 0) {
    LOG_W(PHY,
          "SENSING: passive PDSCH decode enabled with NO pdcch_blind_monitor_tda -- the 3GPP default "
          "TDRA table will be assumed, which is wrong for any gNB carrying its own "
          "pdsch-TimeDomainAllocationList; expect a ~0%% CRC pass rate that says nothing about the channel\n");
  }

  g_enabled = 1;
  LOG_I(PHY,
        "SENSING: blind PDCCH monitor configured: coreset(num_groups=%d duration=%d reg_bundle=%d "
        "interleaver=%d shift=%d scramb=%u) ss(period=%d offset=%d duration=%d first_symb=%d "
        "al_cand=[%d,%d,%d,%d]) bwp=[%d..%d) dmrs_typeA_pos=%d rnti_range=[%u..%u] "
        "noise_gates(energy_min=%.2f energy_adapt_factor=%.2f persist_k=%d persist_window_ms=%d "
        "min_snr_lin=%.2f) tda_entries=%d dmrs(add_pos=%d max_len=%d) "
        "pdsch(decode=%d mcs_table=%d xoverhead=%d rv0_only=%d max_per_slot=%d)\n",
        g_cfg.coreset_freq_domain, g_cfg.coreset_duration, g_cfg.coreset_reg_bundle_size,
        g_cfg.coreset_interleaver_size, g_cfg.coreset_shift_index, g_cfg.coreset_pdcch_dmrs_scrambling_id,
        g_cfg.ss_monitoring_slot_periodicity, g_cfg.ss_monitoring_slot_offset, g_cfg.ss_duration,
        g_cfg.ss_first_symbol, g_cfg.ss_al_candidates[0], g_cfg.ss_al_candidates[1], g_cfg.ss_al_candidates[2],
        g_cfg.ss_al_candidates[3], g_cfg.bwp_start, g_cfg.bwp_start + g_cfg.bwp_size,
        g_cfg.dmrs_typeA_position, g_cfg.rnti_min, g_cfg.rnti_max, g_cfg.energy_min,
        g_cfg.energy_adapt_factor, g_cfg.rnti_persist_k, g_cfg.rnti_persist_window_ms,
        g_cfg.min_snr_lin, g_cfg.extract.tda_count, g_cfg.extract.dmrs_add_pos, g_cfg.extract.dmrs_max_length,
        g_cfg.pdsch_decode, g_cfg.pdsch_mcs_table, g_cfg.pdsch_xoverhead, g_cfg.pdsch_rv0_only,
        g_cfg.pdsch_max_per_slot);
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

// Per-field widths actually used by the extraction, resolving each override against this module's
// built-in assumption. Kept in ONE place so nr_pdcch_blind_dci_size_ex() (which validates a config)
// and nr_pdcch_blind_decode_and_extract_ex() (which reads the payload) can never disagree about the
// layout -- the two disagreeing is precisely the bug the override exists to fix.
typedef struct {
  int bwp_ind, riv, tda, vrb, prb_bundling, rate_match, zp_csirs;
  int tb2, harq_pid, dai, pdsch_to_harq, ant_ports, tci, srs, cbg;
} blind_field_bits_t;

static int pick_bits(int override_val, int dflt)
{
  return (override_val >= 0) ? override_val : dflt;
}

static blind_field_bits_t blind_field_bits(uint16_t bwp_size, const nr_pdcch_blind_extract_opts_t* opts)
{
  const double riv_span = ((double)bwp_size * (double)(bwp_size + 1)) / 2.0;
  blind_field_bits_t f;
  f.riv = (int)ceil(log2(riv_span));
  // time_domain_assignment: nr_dci_size() uses ceil(log2(tdaList->count)), so a configured TDRA
  // list determines this width -- it is not a separate knob. Default 4 = the 16-entry default table.
  if (opts != NULL && opts->tda_count > 0) {
    int b = 0;
    while ((1 << b) < opts->tda_count) {
      b++;
    }
    f.tda = b;
  } else {
    f.tda = 4;
  }
  f.bwp_ind       = opts ? pick_bits(opts->bwp_indicator_bits, 1) : 1;
  f.vrb           = opts ? pick_bits(opts->vrb_to_prb_bits, 0) : 0;
  f.prb_bundling  = opts ? pick_bits(opts->prb_bundling_bits, 0) : 0;
  f.rate_match    = opts ? pick_bits(opts->rate_matching_bits, 0) : 0;
  f.zp_csirs      = opts ? pick_bits(opts->zp_csirs_bits, 0) : 0;
  f.tb2           = opts ? pick_bits(opts->tb2_bits, 0) : 0;
  f.harq_pid      = opts ? pick_bits(opts->harq_pid_bits, 4) : 4;
  f.dai           = opts ? pick_bits(opts->dai_bits, 2) : 2;
  f.pdsch_to_harq = opts ? pick_bits(opts->pdsch_to_harq_bits, 3) : 3;
  f.ant_ports     = opts ? pick_bits(opts->antenna_ports_bits, 4) : 4;
  f.tci           = opts ? pick_bits(opts->tci_bits, 0) : 0;
  f.srs           = opts ? pick_bits(opts->srs_request_bits, 2) : 2;
  f.cbg           = opts ? pick_bits(opts->cbg_bits, 0) : 0;
  return f;
}

uint16_t nr_pdcch_blind_dci_size_ex(uint16_t bwp_size, const nr_pdcch_blind_extract_opts_t* opts)
{
  if (bwp_size < 1) {
    return 0;
  }
  const blind_field_bits_t f = blind_field_bits(bwp_size, opts);
  // Constant-width fields: format identifier (1) + MCS/NDI/RV (8) + TPC PUCCH (2) +
  // PUCCH resource indicator (3) + DM-RS sequence initialisation (1). Carrier indicator is 0 here
  // (no cross-carrier scheduling is representable in this module's fixed assumption set).
  return (uint16_t)(1 + 8 + 2 + 3 + 1 + f.bwp_ind + f.riv + f.tda + f.vrb + f.prb_bundling + f.rate_match
                    + f.zp_csirs + f.tb2 + f.harq_pid + f.dai + f.pdsch_to_harq + f.ant_ports + f.tci + f.srs
                    + f.cbg);
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

// ---------------------------------------------------------------------------------------------
// TS 38.211 Tables 7.4.1.1.2-3 / -4 (PDSCH DM-RS positions l' within a slot). Duplicated from
// nr_mac_common.c, where both are file-static, for the SAME reason the antenna-port table above is
// duplicated: they are 3GPP spec constants, not deployment logic. What is NOT duplicated is
// fill_dmrs_mask()'s policy layer -- that function derives dmrs_AdditionalPosition from an ASN.1
// pdsch_Config a blind receiver does not have, and AssertFatal()s (i.e. aborts the softmodem) on
// inputs this module must merely reject. blind_fill_dmrs_mask() below takes the column directly and
// returns -1 instead. Columns 0-3 = mapping type A, 4-7 = type B; l' == l0 is encoded as bit 0.
// ---------------------------------------------------------------------------------------------
static const int32_t g_table_7_4_1_1_2_3[13][8] = {
    {-1, -1, -1, -1, 1, 1, 1, 1},          // ld = 2
    {0, 0, 0, 0, 1, 1, 1, 1},              // ld = 3
    {0, 0, 0, 0, 1, 1, 1, 1},              // ld = 4
    {0, 0, 0, 0, 1, 17, 17, 17},           // ld = 5
    {0, 0, 0, 0, 1, 17, 17, 17},           // ld = 6
    {0, 0, 0, 0, 1, 17, 17, 17},           // ld = 7
    {0, 128, 128, 128, 1, 65, 73, 73},     // ld = 8
    {0, 128, 128, 128, 1, 129, 145, 145},  // ld = 9
    {0, 512, 576, 576, 1, 129, 145, 145},  // ld = 10
    {0, 512, 576, 576, 1, 257, 273, 585},  // ld = 11
    {0, 512, 576, 2336, 1, 513, 545, 585}, // ld = 12
    {0, 2048, 2176, 2336, 1, 513, 545, 585}, // ld = 13
    {0, 2048, 2176, 2336, -1, -1, -1, -1}, // ld = 14
};
static const int32_t g_table_7_4_1_1_2_4[12][8] = {
    {-1, -1, -1, -1, -1, -1, -1, -1}, // ld < 4
    {0, 0, -1, -1, -1, -1, -1, -1},   // ld = 4
    {0, 0, -1, -1, 3, 3, -1, -1},     // ld = 5
    {0, 0, -1, -1, 3, 3, -1, -1},     // ld = 6
    {0, 0, -1, -1, 3, 3, -1, -1},     // ld = 7
    {0, 0, -1, -1, 3, 99, -1, -1},    // ld = 8
    {0, 0, -1, -1, 3, 99, -1, -1},    // ld = 9
    {0, 768, -1, -1, 3, 387, -1, -1}, // ld = 10
    {0, 768, -1, -1, 3, 387, -1, -1}, // ld = 11
    {0, 768, -1, -1, 3, 771, -1, -1}, // ld = 12
    {0, 3072, -1, -1, 3, 771, -1, -1},// ld = 13
    {0, 3072, -1, -1, -1, -1, -1, -1},// ld = 14
};

/// DM-RS symbol bitmap, with dmrs_AdditionalPosition/maxLength supplied explicitly rather than
/// derived from an ASN.1 pdsch_Config. Mirrors fill_dmrs_mask()'s arithmetic exactly; returns -1
/// (reject) where that function would AssertFatal.
static int32_t blind_fill_dmrs_mask(int dmrs_TypeA_Position,
                                    int NrOfSymbols,
                                    int startSymbol,
                                    mappingType_t mappingtype,
                                    int add_pos,
                                    int length)
{
  if (add_pos < 0 || add_pos > 3 || (length != 1 && length != 2)) {
    return -1;
  }
  int l0 = 0; // type B
  if (mappingtype == typeA) {
    if (dmrs_TypeA_Position == NR_ServingCellConfigCommon__dmrs_TypeA_Position_pos2) {
      l0 = 2;
    } else if (dmrs_TypeA_Position == NR_ServingCellConfigCommon__dmrs_TypeA_Position_pos3) {
      l0 = 3;
    } else {
      return -1;
    }
    // fill_dmrs_mask()'s three AssertFatal conditions, as rejections.
    if (l0 == 3 && add_pos == 3) {
      return -1;
    }
    if (startSymbol > l0) {
      return -1;
    }
  }
  const int column = (mappingtype == typeA) ? add_pos : (add_pos + 4);
  const int ld     = (mappingtype == typeA) ? (NrOfSymbols + startSymbol) : NrOfSymbols;
  if (ld <= 1 || ld >= 15 || (NrOfSymbols + startSymbol) >= 15) {
    return -1;
  }
  if (mappingtype == typeA && l0 == 3 && (ld == 3 || ld == 4)) {
    return -1;
  }

  int32_t l_prime;
  int     l0_shift;
  if (length == 1) {
    l_prime  = g_table_7_4_1_1_2_3[ld - 2][column];
    l0_shift = 1 << l0;
  } else {
    const int row = (ld < 4) ? 0 : (ld - 3);
    if (row >= 12) {
      return -1;
    }
    l_prime  = g_table_7_4_1_1_2_4[row][column];
    l0_shift = (1 << l0) | (1 << (l0 + 1));
  }
  if (l_prime < 0) {
    return -1;
  }
  return (mappingtype == typeA) ? (l_prime | l0_shift) : (l_prime << startSymbol);
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
  return nr_pdcch_blind_decode_and_extract_ex(llr, aggregation_level, dci_length, bwp_size, dmrs_typeA_position,
                                              rnti_min, rnti_max, NULL /* spec defaults */, out);
}

bool nr_pdcch_blind_decode_and_extract_ex(const int16_t* llr,
                                          uint8_t         aggregation_level,
                                          uint16_t        dci_length,
                                          uint16_t        bwp_size,
                                          uint8_t         dmrs_typeA_position,
                                          uint16_t        rnti_min,
                                          uint16_t        rnti_max,
                                          const nr_pdcch_blind_extract_opts_t* opts,
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

  /* FULLCRC probe: polar_decoder_int16() returns a 24-bit CRC, and only a genuine match has its
   * upper bits zero (the live path relies on exactly that when it does `crc == n_rnti`). Logging
   * out->rnti instead -- which is (uint16_t)crc -- makes a false decode whose LOW 16 bits happen to
   * equal the target look like a success. Print the untruncated value. */
  {
    static int s_fullcrc = -1;
    if (s_fullcrc < 0)
      s_fullcrc = (getenv("ISAC_PDCCH_FULLCRC") != NULL) ? 1 : 0;
    /* RNTI-AGNOSTIC detector: a genuine polar decode has the upper 8 bits of the 24-bit CRC zero.
     * Keying this on a PINNED rnti_min was a mistake -- the C-RNTI churns on every re-attach, so a
     * pinned probe only sees the window where the guess happened to be live. Logging every crc with
     * upper==0 finds real DCIs no matter which RNTI they carry. */
    if (s_fullcrc && (crc >> 16) == 0)
      printf("FULLCRC L=%u dci_len=%u crc=0x%x upper=0x%x in_range=%d\n",
             (unsigned)aggregation_level, (unsigned)dci_length, crc, crc >> 16,
             (crc >= rnti_min && crc <= rnti_max) ? 1 : 0);
  }

  // ---- Step 2: RNTI plausibility -- range check instead of the live path's equality check. This
  // is the entire "blind" widening; see dci_nr.c:538-541 (reference only, not modified). ----
  if (crc < rnti_min || crc > rnti_max) {
    out->rnti = (uint16_t)crc;
    out->reject_reason = "CRC-recovered value outside plausible RNTI range";
    return false;
  }
  out->rnti = (uint16_t)crc;

  // ---- Step 2b: mismatched-bits false-detection check. Migrated from NRSniffer's dci_nr.c
  // (nr_dci_false_detection): re-encode the decoded payload with the just-recovered RNTI and count
  // bit mismatches against the ORIGINAL soft LLR polarity. A CRC match is a 1/65536 chance false
  // accept even on a candidate that never carried real PDCCH; this is a far stronger discriminator,
  // since a genuine decode's re-encoded codeword should agree with almost every soft-bit sign. The
  // caller (nr_pdcch_blind_monitor_rt.c) owns the actual accept/reject threshold decision -- this
  // function only measures and reports the count, staying consistent with its existing contract of
  // returning `plausible=true` results for the caller's own gates to filter further. */
  {
    uint32_t encoder_output[NR_MAX_DCI_SIZE_DWORD];
    polar_encoder_fast(dci_estimation, (void*)encoder_output, (int)crc, 1,
                       NR_POLAR_DCI_MESSAGE_TYPE, dci_length, aggregation_level);
    const uint8_t *enout_p = (const uint8_t*)encoder_output;
    const int encoded_length = (int)aggregation_level * 108;
    uint16_t mismatches = 0;
    for (int i = 0; i < encoded_length/8; i++) {
      for (int b = 0; b < 8; b++)
        mismatches += ((enout_p[i] >> b) & 1) ^ ((llr[i*8+b] >> 15) & 1);
    }
    out->mismatched_bits = mismatches;
  }

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
  const blind_field_bits_t f = blind_field_bits(bwp_size, opts);
  // read_field() walks DOWN from dci_length, so a field list wider than the payload would shift by
  // a negative count (undefined behaviour) -- and, long before that, would mean every field is
  // being read from the wrong offset anyway. Reject rather than produce confident garbage.
  if (nr_pdcch_blind_dci_size_ex(bwp_size, opts) > dci_length) {
    out->reject_reason = "configured DCI field widths exceed dci_length";
    return false;
  }

  int            pos     = (int)dci_length;
  const uint64_t payload = dci_estimation[0];

  const uint32_t format_indicator = read_field(payload, &pos, 1);
  (void)read_field(payload, &pos, 0);           // carrier indicator (no cross-carrier scheduling)
  (void)read_field(payload, &pos, f.bwp_ind);   // bwp indicator (consumed, not gated on)
  const uint32_t freq_domain_assignment = read_field(payload, &pos, f.riv);
  const uint32_t time_domain_assignment = read_field(payload, &pos, f.tda);
  (void)read_field(payload, &pos, f.vrb);          // vrb-to-prb mapping
  (void)read_field(payload, &pos, f.prb_bundling); // prb bundling size indicator
  (void)read_field(payload, &pos, f.rate_match);   // rate matching indicator
  (void)read_field(payload, &pos, f.zp_csirs);     // zp csi-rs trigger
  const uint32_t mcs = read_field(payload, &pos, 5);
  const uint32_t ndi = read_field(payload, &pos, 1);
  const uint32_t rv  = read_field(payload, &pos, 2);
  (void)read_field(payload, &pos, f.tb2);          // TB2
  const uint32_t harq_pid = read_field(payload, &pos, f.harq_pid);
  (void)read_field(payload, &pos, f.dai);          // DAI, unused by this extraction
  (void)read_field(payload, &pos, 2);              // TPC PUCCH
  (void)read_field(payload, &pos, 3);              // PUCCH resource indicator
  (void)read_field(payload, &pos, f.pdsch_to_harq); // PDSCH-to-HARQ feedback timing indicator
  const uint32_t antenna_ports = read_field(payload, &pos, f.ant_ports);
  (void)read_field(payload, &pos, f.tci);          // TCI
  (void)read_field(payload, &pos, f.srs);          // SRS request
  (void)read_field(payload, &pos, f.cbg);          // CBGTI + CBGFI
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

  // TDRA: the deployment's own pdsch-TimeDomainAllocationList when supplied (see
  // nr_pdcch_blind_extract_opts_t's comment for why the spec default is wrong here), otherwise the
  // spec default table exactly as before.
  NR_tda_info_t tda = {0};
  if (opts != NULL && opts->tda_count > 0) {
    if ((int)time_domain_assignment >= opts->tda_count) {
      out->reject_reason = "time_domain_assignment index beyond the configured TDRA list";
      return false;
    }
    tda.valid_tda         = true;
    tda.startSymbolIndex  = opts->tda_start[time_domain_assignment];
    tda.nrOfSymbols       = opts->tda_length[time_domain_assignment];
    tda.mapping_type      = opts->tda_mapping[time_domain_assignment] ? typeB : typeA;
    // fill_dmrs_mask() AssertFatal()s on an out-of-range span rather than returning an error, so
    // range-check the configured entry here instead of letting a typo abort the softmodem.
    if (tda.nrOfSymbols < 1 || tda.startSymbolIndex + tda.nrOfSymbols > 14
        || (tda.mapping_type == typeA && tda.startSymbolIndex + tda.nrOfSymbols < 2)) {
      out->reject_reason = "configured TDRA entry spans an illegal symbol range";
      return false;
    }
  } else {
    tda = get_dl_tda_info(NULL /* dl_BWP */, 0 /* ss_type, unused when dl_BWP is NULL */, (int)time_domain_assignment,
                          dmrs_typeA_position, 1 /* mux_pattern */, TYPE_C_RNTI_, 0 /* coresetid */, false /* sib1 */);
    if (!tda.valid_tda) {
      out->reject_reason = "time_domain_assignment index invalid for the default TDRA table";
      return false;
    }
  }

  // fill_dmrs_mask()'s dmrs_AdditionalPosition/maxLength come from the dedicated pdsch_Config,
  // which a blind receiver has not seen. Passing pdsch_Config=NULL makes it assume pos2/len1; when
  // the deployment's real values are configured, apply them by driving the same table lookup
  // through a synthetic column instead (fill_dmrs_mask takes no override argument, and adding one
  // would touch the shared MAC path -- see nr_pdcch_blind_extract_opts_t).
  const int add_pos = (opts != NULL && opts->dmrs_add_pos >= 0) ? opts->dmrs_add_pos : 2;
  const int max_len = (opts != NULL && opts->dmrs_max_length > 0) ? opts->dmrs_max_length : 1;
  const int16_t dmrs_mask =
      blind_fill_dmrs_mask(dmrs_typeA_position, tda.nrOfSymbols, tda.startSymbolIndex, tda.mapping_type, add_pos, max_len);
  if (dmrs_mask <= 0) {
    out->reject_reason = "DM-RS symbol mask undefined for this TDRA entry / additional-position";
    return false;
  }

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
  out->mcs                = (uint8_t)mcs;
  out->rv                 = (uint8_t)rv;
  out->ndi                = (uint8_t)ndi;
  out->harq_pid           = (uint8_t)harq_pid;
  out->tda_index          = (uint8_t)time_domain_assignment;
  out->mapping_type       = (tda.mapping_type == typeB) ? 1 : 0;
  out->plausible          = true;
  out->reject_reason      = NULL;
  return true;
}

