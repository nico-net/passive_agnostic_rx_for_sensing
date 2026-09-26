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
#include "nr_pdcch_sib1_prior.h"
#include "nr_pdcch_blind_monitor_rt.h" // nr_pdcch_blind_monitor_cfg_t + get_cfg() accessor (implemented
                                       // below); the RT tap itself lives in nr_pdcch_blind_monitor_rt.c
                                       // -- see that header's file comment for why the split exists.

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "common/config/config_userapi.h"
#include <sys/stat.h>
#include "common/utils/LOG/log.h"
#include "common/utils/nr/nr_common.h"

#include "PHY/CODING/nrPolar_tools/nr_polar_dci_defs.h"
#include "PHY/CODING/nrPolar_tools/nr_polar_defs.h"

#include "openair2/LAYER2/NR_MAC_COMMON/nr_mac_common.h"

#include "nr_pdcch_coreset_map.h"        // Phase 3 Technique A: nr_pdcch_coreset_map_scan()
#include "nr_pdcch_dci_length_sweep.h"   // Phase 3 Technique C: nr_pdcch_dci_length_sweep()

// ---------------------------------------------------------------------------------------------
// [sensing] pdcch_blind_monitor_* config surface. Parsed but NOT consumed by
// nr_pdcch_blind_dci_size()/nr_pdcch_blind_decode_and_extract() (both pure functions, driven
// directly by their arguments -- see the offline test); consumed by the RT tap in
// nr_pdcch_blind_monitor_rt.c via nr_pdcch_blind_monitor_get_cfg() below.
// ---------------------------------------------------------------------------------------------
static nr_pdcch_blind_monitor_cfg_t g_cfg;
/* The LAST CSS0/CORESET#0 config this cell derived, kept after autodiscover overwrites g_cfg with
 * the dedicated one. Its only consumer is the CSS0/SI-RNTI interleave in nr_pdcch_blind_monitor_rt.c
 * (ISAC_CSS0_INTERLEAVE_K), which time-division-swaps it back in for one occasion at a time so a
 * single capture can answer "does SIB1 decode here?" and "is the dedicated sweep progressing?"
 * without two separate runs. Snapshotting the WHOLE struct rather than listing the fields that
 * differ is deliberate: the two config sets diverge in ~20 fields (coreset geometry+mapping, bwp,
 * ss_*, ss_al_candidates[], dci10_*, rnti range, energy gate), and any field added to one block and
 * forgotten in the other would leak silently across the swap. */
static nr_pdcch_blind_monitor_cfg_t g_css0_cfg;
static bool                         g_css0_cfg_valid;
/* CORESET#0's window range, excluded from the DEDICATED footprint decision so the search stops
 * rediscovering a CORESET whose geometry MIB/SIB1 already gave us -- measured, it dominates at
 * ~22 sigma because SIB1/RAR are transmitted there constantly. -1 = not known. Declared here
 * because autoconf_css0() sets it long before the observation code reads it. */
static int s_css0_excl_first_w = -1, s_css0_excl_last_w = -1;
static int                          g_parsed  = 0;
static bool s_css0_applied; /* CSS0 autoconf idempotency; an autodiscover reset re-arms it (a reset IS a state change) */
static int                          g_enabled = 0;

/* Set for the duration of ONE occasion by the CSS0/SI-RNTI interleave, and THREAD-LOCAL on purpose:
 * the interleaved occasion runs on the PHY receive thread while a scan consumer may be inside an
 * occasion of its own. Swapping the global g_cfg instead would hand that consumer a CORESET#0 config
 * mid-occasion -- not a crash, just confident nonsense -- and the race would be invisible in a log. */
static __thread const nr_pdcch_blind_monitor_cfg_t *t_cfg_override;

const nr_pdcch_blind_monitor_cfg_t* nr_pdcch_blind_monitor_get_cfg(void)
{
  return (t_cfg_override != NULL) ? t_cfg_override : &g_cfg;
}

static bool map_staging_enabled(void)
{
  /* Explicit 0 means exhaustive immediately; explicit 1 deliberately limits the search to the
   * pass-0 prior. Only the default staged mode promises a later exhaustive lap. */
  return getenv("ISAC_MAP_PASS0_ONLY") == NULL;
}

const nr_pdcch_blind_monitor_cfg_t* nr_pdcch_blind_monitor_css0_cfg(void)
{
  return g_css0_cfg_valid ? &g_css0_cfg : NULL;
}

bool nr_pdcch_blind_monitor_coreset0_uss_cfg(nr_pdcch_blind_monitor_cfg_t *out)
{
  const nr_pdcch_sib1_prior_t *pr = nr_pdcch_sib1_prior_get();
  if (out == NULL || !g_css0_cfg_valid || pr == NULL || !pr->dl_bwp_valid
      || pr->dl_bwp_size == 0)
    return false;

  const int physical_start = g_css0_cfg.bwp_start + g_css0_cfg.coreset_rb_offset;
  if (physical_start < (int)pr->dl_bwp_start)
    return false;

  *out = g_css0_cfg;
  out->bwp_start = pr->dl_bwp_start;
  out->bwp_size = pr->dl_bwp_size;
  out->coreset_rb_offset = physical_start - out->bwp_start;
  /* CORESET#0 keeps CoreSetType=MIB/SIB1 so its DM-RS reference remains CRB0. The SearchSpace
   * referencing it is UE-specific: monitor every DL slot/all ALs and size 1_0 from the active BWP. */
  out->ss_monitoring_slot_periodicity = 1;
  out->ss_monitoring_slot_offset = 0;
  out->ss_duration = 1;
  for (int i = 0; i < 4; ++i)
    out->ss_al_candidates[i] = 0;
  out->dci10_scan = 1;
  out->dci10_ss_type = NR_BLIND_SS_UE_SPECIFIC;
  out->dci10_n_rb_riv = out->bwp_size;
  out->dci10_rb_offset = out->bwp_start;
  out->dci10_class_mask = 1u << NR_BLIND_RNTI_CLASS_C;
  out->dci10_mux_pattern = 0;
  out->dci10_sib1 = 0;
  out->rnti_min = NR_PDCCH_BLIND_RNTI_MIN_DEFAULT;
  out->rnti_max = NR_PDCCH_BLIND_RNTI_MAX_DEFAULT;
  out->autodiscover = 0;
  return true;
}

void nr_pdcch_blind_monitor_cfg_override(const nr_pdcch_blind_monitor_cfg_t *in)
{
  t_cfg_override = in;
}

/* ---- PHASE 1: SELF-CONFIGURE FROM MIB/SIB1 (2026-09-04) ---------------------------------------
 *
 * Everything the monitor needs to watch the COMMON search space is broadcast in the clear, so a
 * receiver pointed at an unknown cell can derive it instead of being told. The UE already computes
 * it -- it has to, or SIB1 could not decode -- into mac->type0_PDCCH_CSS_config. This just copies
 * that into the monitor's config, which is the step that was missing: the runtime line read
 * `ss=ue-specific ... sib1=0` while a perfectly good CORESET#0 description sat unused in MAC.
 *
 * The values that are NOT passed in are fixed by TS 38.211 7.3.2.2 for CORESET#0 and so are set
 * here rather than derived: REG bundle 6, interleaver 2, interleaved mapping, and both the DM-RS
 * scrambling ID and the shift index equal to the PCI. That is exactly the hand-written recipe in
 * the conf file's comments, now computed.
 *
 * Returns false and changes NOTHING if the caller's inputs are not usable, so a failed derivation
 * leaves any operator-supplied config standing. */
/* Is self-configuration wanted? Default OFF: an existing deployment has a hand-written config that
 * describes its DEDICATED search space, and silently replacing it with the common one would trade a
 * dense data-aided source for a sparse SIB1 one without being asked. */
bool nr_pdcch_blind_monitor_autoconf_wanted(void)
{
  return g_cfg.autoconf != 0;
}

void nr_pdcch_blind_monitor_set_tda_common(const uint8_t *start, const uint8_t *len,
                                           const uint8_t *map, int n)
{
  if (start == NULL || len == NULL || map == NULL || n <= 0) {
    return;
  }
  if (n > 16) {
    n = 16;
  }
  for (int i = 0; i < n; i++) {
    g_cfg.extract.tda_common_start[i]   = start[i];
    g_cfg.extract.tda_common_length[i]  = len[i];
    g_cfg.extract.tda_common_mapping[i] = map[i];
    /* AND THE CORESET#0 SNAPSHOT. See this function's header note: the snapshot is taken at MIB
     * time, before SIB1 exists, so without this it keeps tda_common_count = 0 and every RA/TC/C
     * grant on a CSS0 interleave occasion is decoded from the 3GPP default table instead of the
     * cell's own list. MEASURED: that is the whole difference between 68/68 RARs and 0. */
    g_css0_cfg.extract.tda_common_start[i]   = start[i];
    g_css0_cfg.extract.tda_common_length[i]  = len[i];
    g_css0_cfg.extract.tda_common_mapping[i] = map[i];
  }
  g_cfg.extract.tda_common_count      = n;
  g_css0_cfg.extract.tda_common_count = n;
  static bool tda_logged = false;
  if (!tda_logged) {
    tda_logged = true;
    char b[256];
    int u = 0;
    for (int k = 0; k < n && u < (int)sizeof(b) - 24; k++) {
      u += snprintf(b + u, sizeof(b) - u, " [%d]S=%u,L=%u,m=%u", k, start[k], len[k], map[k]);
    }
    LOG_A(PHY, "SENSING: TDA-COMMON from SIB1: %d entries:%s -- RA/TC/C-in-CSS now use the CELL's "
               "own list instead of the spec default table\n", n, b);
  }
}

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
                                          int dmrs_typea_position)
{
  if (num_rbs <= 0 || (num_rbs % 6) != 0 || num_symbols < 1 || num_symbols > 3 || cset_start_rb < 0
      || pci < 0 || pci > 1007) {
    LOG_W(PHY,
          "SENSING: CSS0 autoconf REFUSED (num_rbs=%d num_symbols=%d cset_start_rb=%d pci=%d) -- "
          "keeping the existing config\n",
          num_rbs, num_symbols, cset_start_rb, pci);
    return false;
  }

  /* IDEMPOTENT. This is called from the MIB-derivation path, which re-runs on every
   * re-acquisition -- and re-applying it is NOT harmless: the block below overwrites LIVE discovery
   * state, pinning rnti_min/max back to SI-RNTI only and zeroing the energy gate. MEASURED
   * 2026-09-13: with ISAC_AUTO_ACQUIRE=1 the auto_timing re-acquisition loop drove this to 765,195
   * calls (vs 385 without it), so the RNTI window was slammed back to SI-RNTI continuously and the
   * receiver could never graduate from the common search space to the dedicated one -- SI-RNTI
   * accepts flowed (SI=250) while C-RNTI grants, PDSCH and Technique-D convergence stayed at zero.
   * Apply on the FIRST call and on a genuine config CHANGE; otherwise leave the running config
   * alone. Comparing the inputs (not a "done" flag) keeps a real cell reconfiguration working. */
  {
    static int  s_prev[12];
    const int now[12] = {num_rbs, num_symbols, cset_start_rb, ssb_offset_point_a, ss_period_slots,
                         ss_slot, ss_duration, ss_first_symbol, mux_pattern, pci, rb_offset,
                         dmrs_typea_position};
    if (s_css0_applied && memcmp(s_prev, now, sizeof(now)) == 0) {
      return true; // identical derivation, already live -- do not disturb discovery state
    }
    memcpy(s_prev, now, sizeof(now));
    s_css0_applied = true;
  }

  g_cfg.coreset_type                     = 1; // MIB/SIB1 CORESET#0
  g_cfg.coreset_freq_domain              = num_rbs / 6; // the monitor counts 6-RB groups
  g_cfg.coreset_duration                 = num_symbols;
  g_cfg.coreset_reg_bundle_size          = 6; // 38.211: CORESET#0 is always L=6, interleaved
  g_cfg.coreset_interleaver_size         = 2;
  g_cfg.coreset_shift_index              = pci;
  g_cfg.coreset_pdcch_dmrs_scrambling_id = (uint16_t)pci;

  g_cfg.bwp_start = cset_start_rb;
  g_cfg.bwp_size  = num_rbs;
  // MIB dmrs-TypeA-Position, the ASN.1 ENUM (pos2 = 0, pos3 = 1) exactly as mac->dmrs_TypeA_Position
  // carries it -- NOT the symbol number 2/3. Feeds blind_fill_dmrs_mask()/blind_ul_dmrs_mask().
  g_cfg.dmrs_typeA_position = dmrs_typea_position;

  g_cfg.ss_monitoring_slot_periodicity = (ss_period_slots > 0) ? ss_period_slots : 1;
  /* `ss_slot` must already carry the frame term for mux pattern 1 -- the caller computes it the
   * same way fill_searchSpaceZero() does (slot + slots_per_frame * sfn_c). Passing the bare slot
   * monitors the wrong frame on every other period at 30 kHz, which is how the first attempt
   * scanned CORESET#0 correctly and still recovered nothing. */
  g_cfg.ss_monitoring_slot_offset      = ss_slot;
  g_cfg.ss_duration                    = (ss_duration > 0) ? ss_duration : 1;
  g_cfg.ss_first_symbol                = ss_first_symbol;

  /* Align Technique A s 6-RB window grid to THIS cell s CORESET#0 start, derived just above from
   * the SSB offset. MEASURED: the grid was CRB-0 aligned while the CORESET starts at RB 1, so
   * every window straddled two REG bundles (independent precoders under sameAsREG-bundle) --
   * correcting it took the on-accept correlation 0.510 to 0.607, and evened out per-RB values
   * that had ranged 0.360 to 0.949 within one CCE. Derived, never hardcoded. */
  nr_pdcch_coreset_map_set_phase_hint(cset_start_rb);
  nr_pdcch_coreset_map_set_css0(cset_start_rb, num_rbs);
  /* And tell the DEDICATED search where CORESET#0 is, so it stops rediscovering it: measured, it
   * dominates the histogram at ~22 sigma (window 7, 41 hits vs background 3) because SIB1/RAR are
   * transmitted constantly there. Its geometry is already known from MIB/SIB1, so masking it is
   * removing a known quantity, not hiding evidence. */
  s_css0_excl_first_w = cset_start_rb / 6;
  s_css0_excl_last_w  = (cset_start_rb + num_rbs - 1) / 6;

  /* SearchSpace#0's candidate counts are FIXED by TS 38.213 Table 10.1-1, and fill_searchSpaceZero()
   * sets exactly these: AL1 = 0, AL2 = 0, AL4 = 4, AL8 = 2. Pin them rather than leaving the
   * adaptive split to discover them -- the split spends candidates on AL1/AL2, where SIB1 is never
   * sent, and CORESET#0 here is only 8 CCEs (48 REGs / 6), so AL8 already spans the whole thing.
   * ss_al_candidates[] is indexed AL 1,2,4,8; <0 disables, >0 pins, 0 is adaptive. */
  g_cfg.ss_al_candidates[0] = -1; // AL1 -- not used by SS0
  g_cfg.ss_al_candidates[1] = -1; // AL2 -- not used by SS0
  g_cfg.ss_al_candidates[2] = 4;  // AL4
  g_cfg.ss_al_candidates[3] = 2;  // AL8
  /* ISAC_CSS0_ALS="a1:a2:a4:a8" overrides the four above. The Table 10.1-1 pin is correct for
   * SEARCH SPACE #0, but RAR and Msg4 live in the ra-SearchSpace -- a different common search
   * space, with its own nrofCandidates, that happens to share CORESET#0 on this cell. Disabling
   * AL1/AL2 is therefore an SS#0 fact applied to SS#1, which is exactly the kind of assumption
   * that makes a scan cover the wrong geometry and report no error. Measured on Swisscom PCI 382:
   * 450+ RARs decoded at 100 % CRC, TC-RNTIs harvested, and ZERO Msg4 DCIs ever accepted. */
  {
    const char *e = getenv("ISAC_CSS0_ALS");
    int a[4];
    if (e != NULL && sscanf(e, "%d:%d:%d:%d", &a[0], &a[1], &a[2], &a[3]) == 4) {
      for (int i = 0; i < 4; i++)
        g_cfg.ss_al_candidates[i] = a[i];
      LOG_A(PHY, "SENSING: CSS0 AL ladder OVERRIDDEN by ISAC_CSS0_ALS: AL1=%d AL2=%d AL4=%d AL8=%d\n",
            a[0], a[1], a[2], a[3]);
    }
  }

  /* Format 1_0 ONLY. 1_1 lives in the DEDICATED search space, whose description is ciphered and
   * therefore not available to us -- scanning for it here would only manufacture false accepts. */
  g_cfg.dci10_scan        = 2;
  /* BUG FIXED 2026-09-04 (found while investigating 0 blind SI-RNTI accepts on CORESET#0, after the
   * occasion-gate ss_duration fix above got genuine CRC=0xFFFF polar decodes flowing but "accepts"
   * stayed at 0): this line read `= 0; // common`, but nr_blind_ss_type_t (nr_pdcch_blind_monitor.h)
   * defines NR_BLIND_SS_UE_SPECIFIC = 0 and NR_BLIND_SS_COMMON = 1 -- the comment was simply wrong.
   * With ss_type left at UE-specific, dci10_default_class_mask() enables ONLY the C-RNTI class for
   * every CORESET#0 candidate, so a genuine SI-RNTI (0xFFFF) or P-RNTI (0xFFFE) decode -- which
   * SS0 exists to carry -- hit `n_attempts == 0` and was rejected with "no DCI-1_0 RNTI class
   * enabled for this CRC-recovered value" every time, before ever reaching dci10_parse()'s
   * SIB1-specific field checks. Confirmed live: the periodic summary's `last_reject` only ever
   * showed the much more common "CRC-recovered value outside plausible RNTI range" because that
   * reason is overwritten by whichever candidate rejects last before each 1000-occasion snapshot,
   * and real SI-RNTI hits are rare against ~3 candidates/occasion of mostly noise -- so this defect
   * was invisible to a last-value diagnostic and needed tracing the class-mask logic directly. */
  g_cfg.dci10_ss_type     = NR_BLIND_SS_COMMON;
  g_cfg.dci10_n_rb_riv    = num_rbs; // RIV is over CORESET#0 for SI-RNTI, not the whole carrier
  g_cfg.dci10_rb_offset   = cset_start_rb; /* the origin this function just derived; auto
                                      * (bwp_start + bitmap offset) measured 0 at decode
                                      * time while cset_start_rb was 1 -- CORESET#0 at
                                      * CRB 1, so the allocation was extracted one RB low
                                      * and every SIB1 PDSCH CRC failed with otherwise
                                      * perfect parameters. */
  g_cfg.dci10_mux_pattern = (mux_pattern >= 1 && mux_pattern <= 3) ? mux_pattern : 1;
  g_cfg.dci10_sib1        = 1;

  /* SI-RNTI, and ONLY SI-RNTI. The default plausibility range is 1..0xFFEF, which EXCLUDES
   * SI-RNTI (0xFFFF) -- so without this the monitor scans CORESET#0 correctly and then rejects
   * every SIB1 grant it finds. Measured: sib1 decoded but dl_ldpc_ok=0, claimed=0.
   *
   * Pinning it to the single value is also a far stronger gate than the wide default: a false
   * accept must now hit one specific RNTI rather than any of 65519, which is worth ~16 bits of
   * additional rejection on a search whose only other check is a 24-bit CRC. */
  /* RA-RNTI IS THE ONLY VERIFIABLE SEED THIS RECEIVER CAN GET, and the SI-only pin was silently
   * throwing it away. MEASURED 2026-09-20 (Swisscom PCI 382): dci10[C=314 SI=0 RA=0] with
   * BOOTTABLE used=0 -- every RA candidate was rejected as "outside plausible RNTI range" before
   * any RA logic ran, because crc <= 17920 fails `crc >= rnti_min` when rnti_min is pinned to
   * 0xFFFF.
   *
   * Why this matters beyond one counter: the dedicated CORESET search is circular. Verifying a
   * mapping hypothesis needs a known-good RNTI, and the only RNTIs on offer come from the search
   * over 271 UNVERIFIED hypotheses -- i.e. random CCE locations yielding random CRC passes. That
   * was measured too: ~20 accepted RNTIs spread uniformly over the 16-bit space, not one of which
   * ever recurred. C-RNTI cannot break the loop from inside the search, and CORESET#0 carries no
   * C-RNTI to borrow (connected UEs are scheduled in their own USS).
   *
   * RA-RNTI can, because it is COMPUTED from the PRACH occasion rather than guessed: a CRC pass
   * against it is self-validating, and dci10_parse() additionally demands its 16 spec-fixed
   * reserved bits be zero. The RAR it schedules then carries a TC-RNTI, which is that UE's real
   * C-RNTI -- a trustworthy bootstrap seed.
   *
   * COST OF WIDENING: none for SIB1. SI-RNTI (0xFFFF) and P-RNTI (0xFFFE) are admitted by
   * dci10_extract() INDEPENDENTLY of [rnti_min, rnti_max] -- see its Step 2 comment -- so the pin
   * was never what protected them. The false-accept budget stays small: this path sees ~3
   * candidates/occasion at ~50 occasions/s (~800x fewer trials than the dedicated CORESET), the
   * bound is the spec's own RA-RNTI domain rather than the wide 1..0xFFEF default, and RA then
   * costs a further 16 bits of reserved-field rejection. */
  /* MEASURED 2026-09-21 (Swisscom PCI 382, harv2_115328): 77 RARs decoded at 100 % CRC handed out
   * TC-RNTIs 0x5962..0x732a -- ALL above NR_PDCCH_BLIND_RA_RNTI_MAX (0x4600), so every Msg4 DCI
   * (CRC scrambled by that TC-RNTI, sent in THIS common search space) was rejected as "outside
   * plausible RNTI range" before dci10_parse() ran, and the RRCSetup harvest saw only SIB1 TBs
   * (ccch_sdus=0 over 6200 TBs). The 89 "TC accepts" that run did see were all <= 0x45A8: noise
   * inside the RA range that failed RA decomposition. The comment above ("CORESET#0 carries no
   * C-RNTI") predates Msg4 harvesting. TC-RNTI is a C-RNTI-range value; admit the whole range and
   * let the mismatch gate + PDSCH TB CRC + ASN.1 decode be the verifier, as nr_passive_rrc_harvest.c
   * already argues. RA keeps its own <= RA_RNTI_MAX / decomposition test inside dci10_extract(). */
  g_cfg.rnti_min = NR_PDCCH_BLIND_RNTI_MIN_DEFAULT;
  g_cfg.rnti_max = NR_PDCCH_BLIND_RNTI_MAX_DEFAULT;

  /* Format 0_1 OFF. It is a UE-specific-search-space format and cannot appear in CORESET#0, but
   * leaving the configured scan on does real harm rather than nothing: MEASURED, it consumed every
   * candidate that survived the energy gate and rejected all of them --
   *   occasions=7000 candidates=199 dci10[accepts=0] dci01[accepts=0 rejects=199]
   * and it was doing so at THIS cell's dedicated 0_1 payload length, a value that has no meaning on
   * another gNB. Anything the operator configured for the dedicated search space has to be switched
   * off for this context. Preserve the validated operator intent: the runtime
   * nr_pdcch_blind_monitor_ul_scan_enabled() gate suppresses 0_1 on CSS0 and
   * automatically re-allows it after dedicated-USS discovery. Clearing dci01_scan
   * here permanently disabled UL even after successful dedicated acquisition. */

  /* The ADAPTIVE ENERGY GATE describes the dedicated CORESET and must be switched OFF here, for the
   * same reason dci01_scan is: leaving a dedicated-path setting on does active harm, not nothing.
   * energy_floor_update() is fed by every candidate it tests, so it only measures a NOISE floor when
   * most candidates are empty. That holds on the dedicated CORESET (45 groups / 270 RB) and fails
   * completely on CORESET#0, which is 8 CCEs with SIB1 every 20 ms: every AL4/AL8 candidate overlaps
   * the grant, the floor converges to SIGNAL level, and the threshold then sits above everything.
   * MEASURED 2026-09-04: candidates froze at 199 (== ENERGY_FLOOR_WARMUP) while held[energy] grew by
   * exactly 3 per occasion for the rest of the run -- a 100 % rejection rate that looked like a
   * decode failure and hid every downstream question, BWPStart included.
   *
   * OFF rather than retuned: the false-accept budget this gate defends is small here anyway. It was
   * sized for 8 CCE candidates x ~2000 slots/s on the dedicated CORESET; SS0 gives 3 candidates x
   * ~50 occasions/s, ~800x fewer trials, and the SI-RNTI pin above is worth ~16 bits of rejection on
   * its own. If a deployment ever needs a floor here, estimate it from CCEs OUTSIDE the monitored
   * candidates rather than from the candidates themselves -- that is the defect, not the factor. */
  g_cfg.energy_adapt_factor = 0.0f;
  g_cfg.energy_min          = 0.0f;

  LOG_A(PHY,
        "SENSING: CSS0 autoconf from MIB/SIB1 -- coreset(groups=%d dur=%d bundle=6 interleaver=2 "
        "shift=%d scramb=%d) bwp=[%d..%d) (cset_start_rb = ssb_offset_point_a %d - rb_offset %d) "
        "ss(period=%d offset=%d dur=%d symb=%d) dci10(mux=%d sib1=1)\n",
        g_cfg.coreset_freq_domain, g_cfg.coreset_duration, g_cfg.coreset_shift_index,
        g_cfg.coreset_pdcch_dmrs_scrambling_id, g_cfg.bwp_start, g_cfg.bwp_start + g_cfg.bwp_size,
        ssb_offset_point_a, rb_offset,
        g_cfg.ss_monitoring_slot_periodicity, g_cfg.ss_monitoring_slot_offset, g_cfg.ss_duration,
        g_cfg.ss_first_symbol, g_cfg.dci10_mux_pattern);

  /* Snapshot for the CSS0/SI-RNTI interleave (see g_css0_cfg). Taken HERE, not where autodiscover
   * overwrites g_cfg: autodiscover_step() also re-runs on RETRY, by which time g_cfg already holds
   * a DEDICATED config, so snapshotting there would capture the wrong one.
   * autodiscover is forced off IN THE SNAPSHOT ONLY: every dedicated-sweep bookkeeping site in
   * run_occasion() (extent_step, the length sweep, the lookahead lanes) is gated on cfg->autodiscover
   * and WRITES g_cfg when it advances a hypothesis. An interleaved occasion restores the saved
   * dedicated config when it finishes, which would silently revert such an advance while the sweep's
   * own index had already moved on -- i.e. the sweep would go on testing the previous mapping while
   * believing it was testing the next one. A CSS0 occasion is not part of that sweep anyway. */
  g_css0_cfg              = g_cfg;
  g_css0_cfg.autodiscover = 0;
  g_css0_cfg_valid        = true;
  return true;
}

/* ---- PHASE 3: RECOVER THE DEDICATED CONFIG BY SEARCH (2026-09-04) -----------------------------
 *
 * Orchestrates Technique A (nr_pdcch_coreset_map_scan(), whole-carrier DM-RS correlation) and
 * Technique C (nr_pdcch_dci_length_sweep(), histogram sweep) into a self-contained "recover the
 * dedicated CORESET" attempt, mirroring nr_pdcch_blind_monitor_autoconf_css0()'s "populate g_cfg,
 * every downstream consumer is unaware which path did it" contract.
 *
 * NOT gated on g_cfg.bwp_size == 0: g_cfg is SHARED with CSS0 autoconf above, which this feature's
 * own bootstrap-RNTI dependency REQUIRES to be running first (see the autodiscover conf knob's own
 * comment) -- by the time this function is first called, CSS0 has almost certainly already set
 * g_cfg.bwp_size to CORESET#0's span, so that field can no longer distinguish "dedicated geometry
 * still unknown" from "CSS0 already populated the common one". Tracked with its own state instead
 * (s_dedicated_found below), separate from anything CSS0 writes. */
static bool s_dedicated_found = false;

/* No offset blacklist: lack of evidence for (offset,width) says nothing about
 * other widths at that offset, and a quiet interval proves no geometry wrong. */
static bool extent_advance(void);
bool nr_pdcch_blind_monitor_autodiscover_offset_rejected(int rb_offset)
{
  (void)rb_offset;
  return false;
}
void nr_pdcch_blind_monitor_autodiscover_retry(int failed_rb_offset)
{
  if (s_dedicated_found && failed_rb_offset == g_cfg.bwp_start + g_cfg.coreset_rb_offset)
    extent_advance();
}

bool nr_pdcch_blind_monitor_autodiscover_done(void)
{
  return s_dedicated_found;
}

#define NR_PDCCH_MAX_CANDIDATE_WINDOWS (273 / 6)
static uint16_t s_hit_count[NR_PDCCH_MAX_CANDIDATE_WINDOWS];
static uint16_t s_hit_count1[NR_PDCCH_MAX_CANDIDATE_WINDOWS]; /* same, CORESET symbol 1: decides the duration */

/* LONG-TERM evidence, never reset by a dwell -- see the note on the per-UE CORESET hypothesis.
 * Diagnostic only: nothing below consumes these, so no decision changes. */
static unsigned long s_lt_hits[NR_PDCCH_MAX_CANDIDATE_WINDOWS];
static unsigned      s_lt_dwells[NR_PDCCH_MAX_CANDIDATE_WINDOWS];
static uint16_t      s_lt_rnti[NR_PDCCH_MAX_CANDIDATE_WINDOWS];
static unsigned      s_lt_ndwell;
void nr_pdcch_blind_monitor_note_rnti_for_windows(uint16_t rnti)
{
  /* Tag every currently-hot window with the RNTI just accepted. Cheap and approximate on purpose:
   * it answers "which UE was on air while this window was lit", not "which UE owns this CORESET". */
  for (int w = 0; w < NR_PDCCH_MAX_CANDIDATE_WINDOWS; w++)
    if (s_hit_count[w] > 0)
      s_lt_rnti[w] = rnti;
}
static int s_obs_calls;

/* CONVERGENCE CRITERION (rewritten 2026-09-06 -- see the handover doc's reversal section for the
 * live measurement that forced this). The original design required the SAME rb_offset to win
 * AUTODISCOVER_STABLE_VOTES=5 consecutive calls before trusting it. Live validation found the
 * correlation math itself is fine (0.9-0.999 hits, comfortably above the 0.836 bar) but on a busy,
 * WIDE (270 RB) dedicated CORESET the winning window legitimately changes call to call -- different
 * grants land on different CCEs -- so no single window dominates enough to win 5 straight (measured:
 * the single most common window still only topped ~19% of ~19700 calls in one 90s capture, several
 * others close behind). A 5-in-a-row streak at that hit rate is vanishingly unlikely, which is
 * exactly why the original design saw it "never converge" even once the underlying signal chain was
 * healthy. Fixed by ACCUMULATING which windows repeatedly clear the threshold over many calls
 * instead of requiring one to dominate a single instant -- the same shift from "instantaneous vote"
 * to "observed history" that this project's own rnti_persistence_check() already uses for RNTI
 * sightings, just with a longer dwell (a CORESET's occupied windows shift call to call; an RNTI
 * does not). */
#define AUTODISCOVER_OBS_CALLS 1000  // ~4-5s of DL-slot dwell on this cell's occasion rate --
                                     // long enough to average over occasion-to-occasion CCE hopping
/* Complete bounded catalog of contiguous intervals over at most 45 six-RB windows.
 * Exhaustion is inconclusive and starts a fresh occupancy epoch, never a verified fallback. */
#define NR_PDCCH_EXTENT_MAX_CAND (45 * 46 / 2)
/* Occasions each candidate is given to produce a Technique B confirmation before moving on. Sized
 * from this cell's own measured accept rate (~700 accepts/s at ~2000 occasions/s, and a
 * confirmation needs two sightings of the same RNTI), with a wide margin for a quieter cell. */
#define NR_PDCCH_EXTENT_VERIFY_OCC 4000

static nr_pdcch_extent_cand_t s_ext_cand[NR_PDCCH_EXTENT_MAX_CAND];
static int  s_ext_n        = 0;
static int  s_ext_idx      = 0;
static int  s_ext_phase_idx = 0;

/* The occupancy oracle samples one 6-RB grid phase, chosen from CORESET#0. A dedicated BWP may
 * start at any CRB, so that phase is only a search-order prior, never the physical phase of a
 * dedicated CORESET. Try it first, then the other five residues. */
static int extent_phase(int idx)
{
  const int hint = nr_pdcch_coreset_map_get_phase() % 6;
  if (idx <= 0)
    return hint;
  int n = 1;
  for (int phase = 0; phase < 6; ++phase) {
    if (phase != hint && n++ == idx)
      return phase;
  }
  return hint;
}
/* CCE-to-REG mapping hypotheses of the extent under test (nr_pdcch_map_candidates). Each
 * (extent, mapping) pair gets the same NR_PDCCH_EXTENT_VERIFY_OCC dwell. */
#define NR_PDCCH_MAP_MAX_CAND 1024 /* 865 legal at 216 RB x 2 symbols; 512 truncated them */
static nr_pdcch_map_cand_t s_map_cand[NR_PDCCH_MAP_MAX_CAND];
static int s_map_stage = 0;          /* 0 = pass-0 mappings only; 1 = full list (see nr_pdcch_map_candidates) */
static int s_map_pass0_n = 0;        /* pass-0 prefix length of the last nr_pdcch_map_candidates() call */
static int map_stage_truncate(int n);
static bool map_staging_enabled(void);
static int  s_map_n        = 0;
static int  s_map_idx      = 0;
static void map_apply(void)
{
  const nr_pdcch_map_cand_t *m = &s_map_cand[s_map_idx];
  g_cfg.coreset_reg_bundle_size  = m->bundle;
  g_cfg.coreset_interleaver_size = m->interleaver;
  g_cfg.coreset_shift_index      = m->shift;
}
static void map_restart(int span_rb, int duration, int pci)
{
  s_map_n = nr_pdcch_map_candidates(span_rb, duration, pci, s_map_cand, NR_PDCCH_MAP_MAX_CAND);
  s_map_n = map_stage_truncate(s_map_n);
  s_map_idx = 0;
  map_apply();
}
static bool s_ext_verified = false;
static int  s_ext_occ      = 0;
static uint64_t s_ext_generation;
typedef struct {
  uint16_t rnti;
  uint32_t slot;
  uint64_t payload;
} extent_evidence_t;
static extent_evidence_t s_ext_evidence[NR_PDCCH_BLIND_MAX_UE];

static void extent_clear_evidence(void)
{
  memset(s_ext_evidence, 0, sizeof(s_ext_evidence));
  s_ext_verified = false;
  s_ext_occ = 0;
  ++s_ext_generation;
}

/* A decoded CCE proves that its RBs belong to this CORESET, but cannot prove either edge: an unused
 * CCE emits no DM-RS. For non-interleaved mapping, extending the bitmap to the right preserves every
 * proven CCE-to-RB mapping exactly. Use that standards-valid superset as the operational scan
 * envelope, so grants scheduled at higher CCEs remain visible while the measured span stays only a
 * lower bound. Interleaved mappings cannot be extended this way because N_REG changes the mapping. */
static int extent_operational_groups(int rb_offset, int observed_groups, int reg_bundle_size)
{
  if (reg_bundle_size != 0)
    return observed_groups;
  const int available = (g_cfg.bwp_size - rb_offset) / 6;
  return available > observed_groups ? available : observed_groups;
}
uint64_t nr_pdcch_blind_monitor_autodiscover_generation(void)
{
  return s_ext_generation;
}
bool nr_pdcch_blind_monitor_autodiscover_extent_verified(void)
{
  return s_dedicated_found && s_ext_verified;
}
void nr_pdcch_blind_monitor_autodiscover_observe(uint16_t rnti, uint32_t slot, uint64_t payload)
{
  /* Called ONLY for a CRC/plausibility-accepted dedicated DL DCI in the current geometry.
   * Bootstrap history is deliberately not an input. Repeated candidates in one slot or
   * a repeated fixed payload cannot verify a geometry. */
  if (!s_dedicated_found || s_ext_verified || !rnti)
    return;
  int victim = 0;
  for (int i = 0; i < NR_PDCCH_BLIND_MAX_UE; ++i) {
    extent_evidence_t *e = &s_ext_evidence[i];
    if (e->rnti == rnti) {
      if (slot > e->slot && payload != e->payload) {
        const int observed_span = g_cfg.coreset_freq_domain * 6;
        g_cfg.coreset_freq_domain = extent_operational_groups(
            g_cfg.coreset_rb_offset, g_cfg.coreset_freq_domain, g_cfg.coreset_reg_bundle_size);
        s_ext_verified = true;
        LOG_A(PHY, "SENSING: CORESET VERIFIED by fresh dedicated DCI: offset=%d observed_span=%d "
                   "scan_span=%d rnti=0x%x\n",
              g_cfg.coreset_rb_offset, observed_span, g_cfg.coreset_freq_domain * 6, rnti);
      }
      return;
    }
    if (!e->rnti || e->slot < s_ext_evidence[victim].slot)
      victim = i;
  }
  s_ext_evidence[victim] = (extent_evidence_t){rnti, slot, payload};
}
/* RNTI TAGGING (2026-09-16): before an (extent,mapping) candidate's evidence is wiped by
 * extent_clear_evidence(), log which RNTI(s) backed its votes. This is deliberately just a
 * log line over the existing small per-candidate evidence array (NR_PDCCH_BLIND_MAX_UE=16
 * slots) -- NOT a new per-RNTI search state, see memory per-rnti-contexts-deferred.md. Lets a
 * later offline pass check whether accepted-but-unverified evidence is split across RNTIs that
 * imply genuinely different UE configs (one CORESET truth should draw votes from ONE coherent
 * set of RNTIs, not several unrelated ones), without paying for live per-RNTI contexts now. */
static void extent_log_evidence_before_clear(void)
{
  int n = 0;
  for (int i = 0; i < NR_PDCCH_BLIND_MAX_UE; ++i) {
    if (s_ext_evidence[i].rnti)
      n++;
  }
  if (n == 0)
    return;
  char buf[16 * 8];
  int off = 0;
  for (int i = 0; i < NR_PDCCH_BLIND_MAX_UE && off < (int)sizeof(buf) - 8; ++i) {
    if (!s_ext_evidence[i].rnti)
      continue;
    off += snprintf(buf + off, sizeof(buf) - off, "0x%x ", s_ext_evidence[i].rnti);
  }
  LOG_I(PHY, "SENSING: CORESET candidate %d/%d mapping %d/%d abandoned: offset=%d span=%d rnti_votes=%d [%s]\n",
        s_ext_idx + 1, s_ext_n, s_map_idx + 1, s_map_n, g_cfg.coreset_rb_offset, g_cfg.coreset_freq_domain * 6, n, buf);
}
static void discovered_restore(void);
static void discovered_verified(void);
static bool extent_advance(void)
{
  if (!s_dedicated_found || s_ext_verified || s_ext_n <= 0)
    return false;
  discovered_restore(); /* a discovered geometry that failed hands back to the walk unchanged */
  extent_log_evidence_before_clear();
  extent_clear_evidence();
  g_cfg.dci_length_override = 0;

  while (++s_ext_phase_idx < 6) {
    const int off = s_ext_cand[s_ext_idx].first_w * 6 + extent_phase(s_ext_phase_idx);
    const int span = g_cfg.coreset_freq_domain * 6;
    if (off + span > g_cfg.bwp_size)
      continue;
    g_cfg.coreset_rb_offset = off;
    LOG_I(PHY, "SENSING: CORESET candidate %d/%d phase %d/6 mapping %d/%d: offset=%d span=%d "
               "bundle=%u interleaver=%u shift=%u (unverified)\n",
          s_ext_idx + 1, s_ext_n, s_ext_phase_idx + 1, s_map_idx + 1, s_map_n,
          g_cfg.coreset_rb_offset, span, g_cfg.coreset_reg_bundle_size,
          g_cfg.coreset_interleaver_size, g_cfg.coreset_shift_index);
    return true;
  }
  s_ext_phase_idx = 0;

  if (++s_map_idx < s_map_n) {
    g_cfg.coreset_rb_offset = s_ext_cand[s_ext_idx].first_w * 6 + extent_phase(0);
    map_apply();
    LOG_I(PHY, "SENSING: CORESET candidate %d/%d phase 1/6 mapping %d/%d: offset=%d span=%d "
               "bundle=%u interleaver=%u shift=%u (unverified)\n",
          s_ext_idx + 1, s_ext_n, s_map_idx + 1, s_map_n, g_cfg.coreset_rb_offset,
          g_cfg.coreset_freq_domain * 6, g_cfg.coreset_reg_bundle_size,
          g_cfg.coreset_interleaver_size, g_cfg.coreset_shift_index);
    return true;
  }
  if (++s_ext_idx >= s_ext_n) {
    if (s_map_stage == 0 && map_staging_enabled()) {
      s_map_stage = 1;
      s_ext_idx = 0;
      LOG_A(PHY, "SENSING: autodiscover mapping stage 1: primary fast lap exhausted, widening to all shifts\n");
    } else {
      s_dedicated_found = false;
      s_ext_n = 0;
      LOG_W(PHY, "SENSING: CORESET candidates exhausted without evidence; restarting occupancy discovery\n");
      return true;
    }
  }
  g_cfg.coreset_rb_offset = s_ext_cand[s_ext_idx].first_w * 6 + extent_phase(0);
  g_cfg.coreset_freq_domain = s_ext_cand[s_ext_idx].last_w - s_ext_cand[s_ext_idx].first_w + 1;
  map_restart(g_cfg.coreset_freq_domain * 6, g_cfg.coreset_duration,
              g_cfg.coreset_pdcch_dmrs_scrambling_id);
  LOG_I(PHY, "SENSING: CORESET candidate %d/%d: offset=%d span=%d (%d mappings x 6 phases, unverified)\n",
        s_ext_idx + 1, s_ext_n, g_cfg.coreset_rb_offset, g_cfg.coreset_freq_domain * 6, s_map_n);
  return true;
}


/* STAGE 1-2 HAND-OFF (2026-09-23). The decode-free discovery process (captures/idsweep_offline --stage2,
 * GPU stage 1) writes /tmp/coresets_discovered.txt from THIS cell's own snapshots, one line per CORESET.
 * They are applied in order as the CURRENT candidate. A verified one is banked by the caller (and
 * autodiscover_next() moves on to the next line); one that fails its dwell hands back to the walk's saved
 * position and geometry and the next line is tried. When every line is verified, decode-free evidence
 * says nothing else is significant, so the catalog walk PAUSES -- one whole decode pass per occasion
 * saved -- until the file changes. If any line failed, the walk resumes as the fallback. */
#define DISC_MAX 8
typedef struct { int nid, sym, dur, rb, ng, L, R, sh; } disc_coreset_t;
static disc_coreset_t s_disc[DISC_MAX];
static int s_disc_n, s_disc_next, s_disc_failed;
static bool s_disc_active, s_disc_pending, s_disc_paused;
static int s_disc_saved[8];

bool nr_pdcch_blind_monitor_discovery_paused(void)
{
  return s_disc_paused;
}

static void discovered_restore(void) /* called from extent_advance(): the applied line failed its dwell */
{
  if (!s_disc_active)
    return;
  g_cfg.coreset_rb_offset = s_disc_saved[0];
  g_cfg.coreset_freq_domain = s_disc_saved[1];
  g_cfg.coreset_duration = s_disc_saved[2];
  g_cfg.coreset_reg_bundle_size = s_disc_saved[3];
  g_cfg.coreset_interleaver_size = s_disc_saved[4];
  g_cfg.coreset_shift_index = s_disc_saved[5];
  g_cfg.coreset_pdcch_dmrs_scrambling_id = s_disc_saved[6];
  g_cfg.ss_first_symbol = s_disc_saved[7];
  s_disc_active = false;
  s_disc_failed++;
  s_disc_next++;
  s_disc_pending = s_disc_next < s_disc_n;
  LOG_A(PHY, "SENSING: discovered CORESET %d/%d did not verify in its dwell; %s\n", s_disc_next, s_disc_n,
        s_disc_pending ? "trying the next discovered one" : "resuming the catalog walk");
}

/* called from autodiscover_next(): the applied line was verified and banked */
static void discovered_verified(void)
{
  if (!s_disc_active)
    return;
  s_disc_active = false;
  s_disc_next++;
  s_disc_pending = s_disc_next < s_disc_n;
  if (!s_disc_pending && s_disc_failed == 0) {
    s_disc_paused = true;
    LOG_A(PHY, "SENSING: all %d discovered CORESET(s) verified and banked; decode-free discovery reports nothing "
               "else significant -> catalog walk PAUSED until the discovery file changes\n", s_disc_n);
  }
}

static bool discovered_load(const char *path)
{
  FILE *f = fopen(path, "r");
  if (f == NULL)
    return false;
  char line[256];
  s_disc_n = 0;
  while (s_disc_n < DISC_MAX && fgets(line, sizeof(line), f)) {
    disc_coreset_t d;
    if (sscanf(line, "CORESET nid=%d sym=%d dur=%d grid_rb=%d groups=%d L=%d R=%d shift=%d", &d.nid, &d.sym, &d.dur,
               &d.rb, &d.ng, &d.L, &d.R, &d.sh) != 8)
      continue;
    if (d.nid < 0 || d.nid > 65535 || d.sym < 0 || d.sym > 13 || d.dur < 1 || d.dur > 3 || d.rb < 0 || d.ng < 1
        || d.rb + 6 * d.ng > (int)g_cfg.bwp_size || (d.L != 0 && d.L != 2 && d.L != 3 && d.L != 6))
      continue;
    s_disc[s_disc_n++] = d;
  }
  fclose(f);
  s_disc_next = s_disc_failed = 0;
  return s_disc_n > 0;
}

bool nr_pdcch_blind_monitor_discovered_poll(void)
{
  static uint64_t s_calls;
  static time_t s_mtime;
  if ((++s_calls & 1023) != 1)
    return false;
  const char *path = "/tmp/coresets_discovered.txt";
  struct stat st;
  if (stat(path, &st) == 0 && st.st_mtime != s_mtime) { /* new or changed discovery result */
    s_mtime = st.st_mtime;
    if (discovered_load(path)) {
      s_disc_pending = true;
      if (s_disc_paused) {
        s_disc_paused = false;
        LOG_A(PHY, "SENSING: discovery file changed; catalog walk un-paused\n");
      }
    } else {
      LOG_W(PHY, "SENSING: %s present but no usable CORESET line; ignored\n", path);
    }
  }
  /* discovered CORESETs the bank already holds are verified: skip them without a dwell */
  while (s_disc_pending && !s_disc_active) {
    const disc_coreset_t *b = &s_disc[s_disc_next];
    if (!nr_pdcch_blind_monitor_bank_has_geometry(b->rb, b->ng, b->dur, b->L, b->R, b->sh, b->nid))
      break;
    LOG_A(PHY, "SENSING: discovered CORESET %d/%d is already a verified bank entry -- no dwell needed\n",
          s_disc_next + 1, s_disc_n);
    s_disc_active = true; /* discovered_verified() consumes an active entry */
    discovered_verified();
  }
  if (!s_disc_pending || s_disc_active || !s_dedicated_found || s_ext_verified || s_ext_n <= 0)
    return false;
  const disc_coreset_t *d = &s_disc[s_disc_next];
  s_disc_saved[0] = g_cfg.coreset_rb_offset;
  s_disc_saved[1] = g_cfg.coreset_freq_domain;
  s_disc_saved[2] = g_cfg.coreset_duration;
  s_disc_saved[3] = g_cfg.coreset_reg_bundle_size;
  s_disc_saved[4] = g_cfg.coreset_interleaver_size;
  s_disc_saved[5] = g_cfg.coreset_shift_index;
  s_disc_saved[6] = g_cfg.coreset_pdcch_dmrs_scrambling_id;
  s_disc_saved[7] = g_cfg.ss_first_symbol;
  extent_log_evidence_before_clear();
  extent_clear_evidence(); /* bumps the generation: the RT path invalidates its length sweep */
  g_cfg.dci_length_override = 0;
  g_cfg.coreset_rb_offset = d->rb;
  g_cfg.coreset_freq_domain = d->ng;
  g_cfg.coreset_duration = d->dur;
  g_cfg.coreset_reg_bundle_size = d->L;
  g_cfg.coreset_interleaver_size = d->R;
  g_cfg.coreset_shift_index = d->sh;
  g_cfg.coreset_pdcch_dmrs_scrambling_id = d->nid;
  g_cfg.ss_first_symbol = d->sym;
  s_disc_active = true;
  s_disc_pending = false;
  LOG_A(PHY, "SENSING: CORESET %d/%d from decode-free discovery: nID=%d sym=%d dur=%d grid_rb=%d groups=%d "
             "bundle=%d interleaver=%d shift=%d (unverified)\n",
        s_disc_next + 1, s_disc_n, d->nid, d->sym, d->dur, d->rb, d->ng, d->L, d->R, d->sh);
  return true;
}

/* ---- Lookahead lanes: see nr_pdcch_blind_monitor.h's own comment for the design. Lanes draw
 * unique (extent,mapping) tasks from a shared producer-thread catalogue. */
typedef struct {
  bool active;
  bool fast_length_only;
  int  ext_idx;
  int  rb_offset;
  int  freq_domain;
  int  reg_bundle_size;
  int  interleaver_size;
  int  shift_index;
  nr_pdcch_map_cand_t map_cand[NR_PDCCH_MAP_MAX_CAND];
  int  map_n;
  int  map_idx;
  int  occ;
  extent_evidence_t evidence[NR_PDCCH_BLIND_MAX_UE];
} nr_pdcch_lookahead_lane_t;
static nr_pdcch_lookahead_lane_t s_lane[NR_PDCCH_LOOKAHEAD_MAX];
static int s_lane_dispatch_ext;
static int s_lane_dispatch_map;
static int s_lane_dispatch_phase;
static int s_lane_dispatch_stage;
static int s_lane_dispatch_map_max;

int nr_pdcch_blind_lookahead_count(void)
{
  static int s_k = -1;
  if (s_k < 0) {
    const char *e = getenv("ISAC_PDCCH_EXTENT_BATCH");
    int v = e ? atoi(e) : 1;
    if (v < 1) v = 1;
    if (v > NR_PDCCH_LOOKAHEAD_MAX + 1) v = NR_PDCCH_LOOKAHEAD_MAX + 1;
    s_k = v - 1;
    LOG_A(PHY, "SENSING: PDCCH_LOOKAHEAD configured ISAC_PDCCH_EXTENT_BATCH=%s -> K=%d (%d lookahead lane%s active)\n",
          e ? e : "(unset)", v, s_k, s_k == 1 ? "" : "s");
  }
  return s_k;
}

static void lane_map_apply(int lane)
{
  nr_pdcch_lookahead_lane_t *ln = &s_lane[lane];
  const nr_pdcch_map_cand_t *m = &ln->map_cand[ln->map_idx];
  ln->reg_bundle_size   = m->bundle;
  ln->interleaver_size  = m->interleaver;
  ln->shift_index       = m->shift;
}

static int lane_map_count(int ext, nr_pdcch_map_cand_t *out)
{
  const int span_rb = (s_ext_cand[ext].last_w - s_ext_cand[ext].first_w + 1) * 6;
  const int full_n = nr_pdcch_map_candidates(span_rb, g_cfg.coreset_duration,
                                             g_cfg.coreset_pdcch_dmrs_scrambling_id,
                                             out, NR_PDCCH_MAP_MAX_CAND);
  const char *e = getenv("ISAC_MAP_PASS0_ONLY");
  if (e != NULL && atoi(e) == 0)
    return full_n; /* explicitly exhaustive from the first lap */
  if (s_lane_dispatch_stage == 0 && s_map_pass0_n > 0 && s_map_pass0_n < full_n)
    return s_map_pass0_n;
  return full_n;
}

static int lane_catalog_map_max(void)
{
  nr_pdcch_map_cand_t tmp[NR_PDCCH_MAP_MAX_CAND];
  int max_n = 0;
  for (int ext = 0; ext < s_ext_n; ++ext) {
    const int n = lane_map_count(ext, tmp);
    if (n > max_n)
      max_n = n;
  }
  return max_n;
}

static bool lane_assign_next(int lane)
{
  nr_pdcch_lookahead_lane_t *ln = &s_lane[lane];
  while (s_ext_n > 0) {
    if (s_lane_dispatch_map >= s_lane_dispatch_map_max) {
      if (s_lane_dispatch_stage == 0 && map_staging_enabled()) {
        s_lane_dispatch_stage = 1;
        s_lane_dispatch_ext = 0;
        s_lane_dispatch_map = 0;
        s_lane_dispatch_phase = 0;
        s_lane_dispatch_map_max = lane_catalog_map_max();
        LOG_A(PHY, "SENSING: autodiscover mapping stage 1: fast catalog assigned, widening to "
                   "all lengths, shifts and RB phases\n");
      } else {
        ln->active = false;
        return false;
      }
    }

    /* Extent is the innermost cursor. Saturated occupancy means the edge is unknown, so spending
     * every mapping/phase on one width before touching the next one starves later physical
     * CORESETs. Visit every extent at the same mapping/phase first; mapping and phase remain fully
     * exhaustive, only their order changes. */
    if (s_lane_dispatch_ext >= s_ext_n) {
      s_lane_dispatch_ext = 0;
      if (++s_lane_dispatch_phase >= 6) {
        s_lane_dispatch_phase = 0;
        ++s_lane_dispatch_map;
      }
      continue;
    }

    const int ext = s_lane_dispatch_ext++;
    ln->ext_idx       = ext;
    ln->freq_domain   = s_ext_cand[ext].last_w - s_ext_cand[ext].first_w + 1;
    const int span_rb = ln->freq_domain * 6;
    ln->map_n = lane_map_count(ext, ln->map_cand);
    if (s_lane_dispatch_map >= ln->map_n)
      continue;

    const int map_idx = s_lane_dispatch_map;
    const int phase_idx = s_lane_dispatch_phase;
    ln->rb_offset = s_ext_cand[ext].first_w * 6 + extent_phase(phase_idx);
    if (ln->rb_offset + span_rb > g_cfg.bwp_size)
      continue;

    ln->map_idx = map_idx;
    ln->fast_length_only = (s_lane_dispatch_stage == 0 && map_staging_enabled());
    memset(ln->evidence, 0, sizeof(ln->evidence));
    ln->occ = 0;
    ln->active = true;
    lane_map_apply(lane);
    if (getenv("ISAC_DISCOVER_DIAG") != NULL)
      LOG_I(PHY, "SENSING: LOOKAHEAD_ASSIGN lane=%d extent=%d offset=%d span=%d bundle=%d interleaver=%d shift=%d fast=%d\n",
            lane, ext, ln->rb_offset, span_rb, ln->reg_bundle_size, ln->interleaver_size,
            ln->shift_index, ln->fast_length_only);
    return true;
  }
  ln->active = false;
  return false;
}

/* Called once a fresh footprint is found (nr_pdcch_blind_monitor_autodiscover_step), same moment
 * the primary's own s_ext_cand/s_ext_n catalog is (re)built. */
static void lookahead_lanes_init(void)
{
  const int k = nr_pdcch_blind_lookahead_count();
  /* The primary owns task (extent 0, mapping 0). Every lane draws a different subsequent task
   * from one shared two-dimensional catalogue. The old design spread lanes over extents but put
   * every one at mapping 0, making an L=6 interleaved CORESET wait behind thousands of occasions
   * of unrelated mappings. */
  const char *map_env = getenv("ISAC_MAP_PASS0_ONLY");
  s_lane_dispatch_stage = (map_env != NULL && atoi(map_env) == 0) ? 1 : 0;
  s_lane_dispatch_ext = 1; /* primary owns extent 0 / mapping 0 / preferred phase */
  s_lane_dispatch_map = 0;
  s_lane_dispatch_phase = 0;
  s_lane_dispatch_map_max = lane_catalog_map_max();
  for (int L = 0; L < NR_PDCCH_LOOKAHEAD_MAX; L++) {
    if (L >= k || s_ext_n <= 0) {
      s_lane[L].active = false;
      continue;
    }
    lane_assign_next(L);
  }
}

static void lane_advance(int lane)
{
  nr_pdcch_lookahead_lane_t *ln = &s_lane[lane];
  memset(ln->evidence, 0, sizeof(ln->evidence));
  ln->occ = 0;
  lane_assign_next(lane);
}

bool nr_pdcch_blind_lookahead_get(int lane, nr_pdcch_lookahead_geom_t *out)
{
  if (!out)
    return false;
  memset(out, 0, sizeof(*out));
  if (lane < 0 || lane >= NR_PDCCH_LOOKAHEAD_MAX)
    return false;
  const nr_pdcch_lookahead_lane_t *ln = &s_lane[lane];
  if (!s_dedicated_found || s_ext_verified || !ln->active)
    return false;
  out->valid            = true;
  out->fast_length_only = ln->fast_length_only;
  out->rb_offset         = ln->rb_offset;
  out->freq_domain       = ln->freq_domain;
  out->reg_bundle_size   = ln->reg_bundle_size;
  out->interleaver_size  = ln->interleaver_size;
  out->shift_index       = ln->shift_index;
  return true;
}

bool nr_pdcch_blind_lookahead_observe(int lane, uint16_t rnti, uint32_t slot, uint64_t payload)
{
  if (lane < 0 || lane >= NR_PDCCH_LOOKAHEAD_MAX || !rnti)
    return false;
  nr_pdcch_lookahead_lane_t *ln = &s_lane[lane];
  if (!s_dedicated_found || s_ext_verified || !ln->active)
    return false;
  int victim = 0;
  for (int i = 0; i < NR_PDCCH_BLIND_MAX_UE; ++i) {
    extent_evidence_t *e = &ln->evidence[i];
    if (e->rnti == rnti) {
      if (slot > e->slot && payload != e->payload) {
        /* This lane's geometry just VERIFIED: commit it as THE answer and stop every lane's search,
         * same effect as the primary's own verification (nr_pdcch_blind_monitor_autodiscover_observe).
         * dci_length_override is deliberately left to the caller (see header comment) -- rt.c owns
         * the per-lane length-sweep state this needs. */
        g_cfg.coreset_rb_offset        = ln->rb_offset;
        g_cfg.coreset_freq_domain      = extent_operational_groups(
            ln->rb_offset, ln->freq_domain, ln->reg_bundle_size);
        g_cfg.coreset_reg_bundle_size  = ln->reg_bundle_size;
        g_cfg.coreset_interleaver_size = ln->interleaver_size;
        g_cfg.coreset_shift_index      = ln->shift_index;
        s_ext_verified = true;
        ++s_ext_generation;
        LOG_A(PHY, "SENSING: CORESET VERIFIED by lookahead lane %d: offset=%d observed_span=%d "
                   "scan_span=%d rnti=0x%x\n",
              lane, ln->rb_offset, ln->freq_domain * 6, g_cfg.coreset_freq_domain * 6, rnti);
        return true;
      }
      return false;
    }
    if (!e->rnti || e->slot < ln->evidence[victim].slot)
      victim = i;
  }
  ln->evidence[victim] = (extent_evidence_t){rnti, slot, payload};
  return false;
}

void nr_pdcch_blind_lookahead_step(int lane)
{
  if (lane < 0 || lane >= NR_PDCCH_LOOKAHEAD_MAX)
    return;
  nr_pdcch_lookahead_lane_t *ln = &s_lane[lane];
  if (!s_dedicated_found || s_ext_verified || !ln->active)
    return;
  if (++ln->occ >= NR_PDCCH_EXTENT_VERIFY_OCC)
    lane_advance(lane);
}

void nr_pdcch_blind_lookahead_retry(int lane)
{
  if (lane < 0 || lane >= NR_PDCCH_LOOKAHEAD_MAX)
    return;
  if (!s_dedicated_found || s_ext_verified || !s_lane[lane].active)
    return;
  lane_advance(lane);
}

void nr_pdcch_blind_monitor_autodiscover_reset(void)
{
  s_css0_applied = false;
  s_dedicated_found = false;
  s_ext_n = s_ext_idx = s_ext_phase_idx = 0;
  s_map_stage = 0;
  s_map_n = s_map_idx = 0;
  s_lane_dispatch_ext = s_lane_dispatch_map = s_lane_dispatch_phase = 0;
  s_lane_dispatch_stage = s_lane_dispatch_map_max = 0;
  memset(s_hit_count, 0, sizeof(s_hit_count));
  memset(s_hit_count1, 0, sizeof(s_hit_count1));
  s_obs_calls = 0;
  g_cfg.dci_length_override = 0;
  extent_clear_evidence();
  memset(s_lane, 0, sizeof(s_lane));
}

int nr_pdcch_map_candidates(int span_rb, int duration, int pci, nr_pdcch_map_cand_t *out, int max_out)
{
  if (out == NULL || max_out <= 0 || span_rb <= 0 || duration < 1 || duration > 3)
    return 0;
  int n = 0;
  /* SIB1 PRIOR FIRST, for the same reason as the extent: the cell's own commonControlResourceSet
   * states its REG bundle size, interleaver size and shift index. The blind walk below tries the
   * PCI residue of every legal (L, R) and then all ~271 shifts; if the operator reuses the common
   * mapping for the dedicated CORESET, this single entry replaces that entire walk. */
  {
    const nr_pdcch_sib1_prior_t *pr = nr_pdcch_sib1_prior_get();
    if (pr != NULL && pr->coreset_valid && pr->interleaved && n < max_out) {
      out[n++] = (nr_pdcch_map_cand_t){(uint8_t)pr->reg_bundle_size,
                                       (uint8_t)pr->interleaver_size,
                                       (uint16_t)pr->shift_index};
    }
  }
  out[n++] = (nr_pdcch_map_cand_t){0, 0, 0}; /* non-interleaved: this project's every captured dedicated CORESET */
  const int N_reg = span_rb * duration;
  static const int Ls[2][2] = {{2, 6}, {3, 6}};
  const int *L = Ls[duration == 3];
  static const int Rs[3] = {2, 3, 6};
  /* Two passes (2026-09-19): pass 0 = the PCI's residue and 0 of EVERY legal (L, R); pass 1 = the
   * remaining shifts. The single-pass order walked all shifts of one (L, R) before the next, so on
   * the macro's 216 RB x 2-symbol CORESET (865 legal) the 512 cap cut the list before ANY L=6
   * mapping -- CORESET#0's own bundle size -- and no run length could ever reach it. */
  /* ISAC_MAP_PASS0_ONLY=1: emit ONLY pass 0 -- the PCI residue and shift 0 of every legal (L, R).
   * Pass 1 is the blind crawl over every remaining shift, and it is ~96 % of the 271 mappings, so
   * skipping it turns a 133 x 271 = 36,043-hypothesis catalogue into ~133 x 12 = 1,600: a full walk
   * in seconds rather than minutes. shiftIndex is overwhelmingly either 0 or PCI-derived in real
   * deployments, so this covers the likely answers first and simply MISSES an exotic shift -- run
   * without the knob to get the exhaustive walk back. Default off; nothing changes silently. */
  /* 2026-09-21 (user: shrink the search from what SIB1/PCI give): pass 0 is now the DEFAULT first
   * lap -- every extent is walked with only the PCI residue / shift 0 mappings, and pass 1 (every
   * other shift) is added only after the whole extent catalogue has been assigned once at pass 0
   * (s_map_stage, advanced by lane_assign). ISAC_MAP_PASS0_ONLY=1 never widens; =0 is the old
   * exhaustive walk from the start. */
  static int s_pass0_env = -1;
  if (s_pass0_env < 0) {
    const char *e = getenv("ISAC_MAP_PASS0_ONLY");
    s_pass0_env = (e == NULL) ? -2 : ((atoi(e) == 1) ? 1 : 0);   /* -2 = staged (default) */
  }
  const int n_pass = (s_pass0_env == 1) ? 1 : 2;   /* staging is applied by the callers, see map_stage_truncate() */
  int n_pass0 = 0;
  for (int pass = 0; pass < n_pass; pass++) {
    if (pass == 1) n_pass0 = n;
    for (int li = 0; li < 2; li++) {
      const int nb = N_reg / L[li];             /* REG bundles; the shift acts modulo this */
      if (L[li] % duration != 0)
        continue;
      const int p = pci % nb;
      for (int ri = 0; ri < 3; ri++) {
        const int R = Rs[ri];
        if (N_reg % (L[li] * R) != 0)           /* C = N_REG/(L*R) must be an integer */
          continue;
        if (pass == 0) {
          if (n < max_out)
            out[n++] = (nr_pdcch_map_cand_t){(uint8_t)L[li], (uint8_t)R, (uint16_t)p};
          if (p != 0 && n < max_out)
            out[n++] = (nr_pdcch_map_cand_t){(uint8_t)L[li], (uint8_t)R, 0};
        } else {
          for (int sh = 1; sh < nb && n < max_out; sh++)
            if (sh != p)
              out[n++] = (nr_pdcch_map_cand_t){(uint8_t)L[li], (uint8_t)R, (uint16_t)sh};
        }
      }
    }
  }
  s_map_pass0_n = (n_pass == 1) ? n : n_pass0;
  return n;
}

/* Stage 0 keeps only the pass-0 prefix (PCI residue / shift 0 of every legal (L, R)); stage 1 is
 * the full list. ISAC_MAP_PASS0_ONLY=0 disables staging (exhaustive from the start). */
static int map_stage_truncate(int n)
{
  const char *e = getenv("ISAC_MAP_PASS0_ONLY");
  if (e != NULL && atoi(e) == 0) return n;
  return (s_map_stage == 0 && s_map_pass0_n > 0 && s_map_pass0_n < n) ? s_map_pass0_n : n;
}

int nr_pdcch_extent_candidates(int first_w, int last_w, int nw_total,
                               nr_pdcch_extent_cand_t* out, int max_out)
{
  if (out == NULL || max_out <= 0 || nw_total <= 0 || first_w < 0 || last_w < first_w
      || last_w >= nw_total) {
    return 0;
  }
  /* Candidate 0 is the pre-2026-09-07 heuristic's OWN answer, so a cell where it was already right
   * locks with no added dwell and this can never regress. */
  /* SNAP ON SPAN, NOT ON first_w == 0.
   * A CORESET is CONTIGUOUS, so if the observed occupancy spans most of the carrier the CORESET is
   * full-band and the exact edges are merely unobserved -- PDCCH DM-RS exists only where a PDCCH was
   * actually transmitted, so an edge window is silent whenever the scheduler did not use it.
   * Requiring first_w == 0 made the snap depend on the single most fragile statistic in the
   * histogram: the position of the LOWEST window that happened to clear a fixed 3-hit floor.
   * MEASURED 2026-09-07 (capture val_d1_151203): traffic thinned after the UE re-attached, windows
   * 0 and 1 fell under the floor, and Technique A declared rb_offset=12 span_rb=216 against a truth
   * of 0/270. The span was 36 of 45 windows -- 80 %, comfortably over the threshold -- so snapping
   * on span alone would have returned the right answer; only the first_w == 0 guard prevented it.
   * Nothing downstream could recover, because the extent check is gated on the dci_length being
   * found and the length sweep cannot succeed under a wrong footprint. */
  /* NO SNAP (2026-09-16). Snapping a >= 75 % span to the full carrier turned a CORRECT 0/240
   * observation into 0/270 on the OAI rfsim cell (dedicated CORESET = 48-RB-quantised 240 of 273),
   * and since the walk only ever grows, the truth was never tried: 0 accepts at every candidate.
   * The observed footprint is hypothesis 1 as observed; the full carrier is hypothesis 2. */
  const int snap = 0;
  int n = 0;
  /* SIB1 PRIOR FIRST. The cell broadcasts commonControlResourceSet in SIB1, and its frequency-domain
   * bitmap is a real, DECODED CORESET footprint on THIS cell -- whereas everything below is derived
   * from where DCIs happened to land during a dwell. It is the COMMON CORESET, so it is a
   * hypothesis for the dedicated one rather than an answer; putting it at index 0 costs one dwell
   * to test and nothing if wrong, and the entire blind walk still follows it. */
  {
    int pw_first = -1, pw_last = -1;
    if (nr_pdcch_sib1_prior_window(&pw_first, &pw_last) && pw_last < nw_total && n < max_out) {
      out[n].first_w = pw_first;
      out[n].last_w  = pw_last;
      n++;
    }
  }
  out[n].first_w = first_w;
  out[n].last_w  = last_w;
  n++;
  /* The full carrier is the second hypothesis whenever it is not the first: the observed footprint
   * is only where DCIs happened to land in the dwell (rfsim OAI cell: 60 of 273 RB seen, CORESET =
   * whole BWP), and walking 322 growing dilations at 500 occasions each never reached it. */
  if (!snap && n < max_out && !(first_w == 0 && last_w == nw_total - 1)) {
    out[n].first_w = 0;
    out[n].last_w  = nw_total - 1;
    n++;
  }
  /* OFFSET AND SPAN. The offset used to be excluded because it could not be APPLIED: the FAPI
   * builder hardcoded rel15->coreset.rb_offset = 0 and the offset rode on BWPStart, which also
   * moves RIV interpretation and dci_length. That is now plumbed through its own field, and
   * dci_nr.c consumes it as `cset_start + coreset->rb_offset` -- for the RE index AND for indexing
   * the DM-RS sequence (nr_pdcch_dmrs_ref over n_rb + rb_offset), so a wrong offset corrupts the
   * pilot sequence and reads as a dead channel rather than a weak one. Hence it must be searched,
   * not assumed.
   *
   * The true CORESET must CONTAIN the observed occupancy, so admissible hypotheses are exactly
   * (f <= first_w, l >= last_w). Enumerated in order of TOTAL EXPANSION from what was observed, so
   * the nearest hypotheses are tried first and the verification dwell is spent where the answer
   * most likely is -- the histogram is a lower bound on the footprint, and it is usually a tight
   * one. */
  for (int d = 0; d <= first_w + (nw_total - 1 - last_w) && n < max_out; d++) {
    for (int df = 0; df <= d && n < max_out; df++) {
      const int dl = d - df;
      const int f = first_w - df;
      const int l = last_w + dl;
      if (f < 0 || l >= nw_total) {
        continue;
      }
      bool dup = false;
      for (int i = 0; i < n; i++) {
        if (out[i].first_w == f && out[i].last_w == l) {
          dup = true;
          break;
        }
      }
      if (dup) {
        continue;
      }
      out[n].first_w = f;
      out[n].last_w  = l;
      n++;
    }
  }

  /* SATURATED OCCUPANCY: the containment premise fails, so also search INSIDE the window.
   *
   * Everything above rests on "the true CORESET must CONTAIN the observed occupancy", which holds
   * only while the histogram lights up where PDCCH DM-RS actually is. On a loaded commercial cell
   * it does not: MEASURED 2026-09-20 on Swisscom PCI 382 (273 PRB, heavy traffic), the observation
   * saturated to the whole carrier, so first_w=0 and last_w=nw_total-1. The expansion loop then has
   * d_max = 0 and emits exactly ONE candidate -- the log reads "CORESET candidate 1/1" -- and the
   * sweep spent three full passes over its 271 bundle/interleaver/shift variations with 15 live
   * bootstrap RNTIs and verified nothing, because the true mapping was never a candidate.
   *
   * A saturated observation carries no information about the edges, so treat it as unknown rather
   * than as a lower bound and enumerate plausible CONTAINED footprints. Bounded deliberately: only
   * the common CORESET widths (24/48/96/144 RB, i.e. 4/8/16/24 windows of 6 RB) at every offset,
   * rather than all 1035 contiguous intervals -- at 271 mapping variations each, the full set would
   * be ~280k hypotheses and unreachable in any realistic dwell, whereas this adds ~100 and is
   * covered in minutes at K=16 lanes.
   *
   * Only triggered when the observation really is saturated (>= 90 % of the carrier). A tight
   * observation keeps the original containment behaviour untouched, so no cell that already
   * converged can regress. */
  {
    const int observed_w = last_w - first_w + 1;
    if (observed_w * 10 >= nw_total * 9) {
      static const int kCommonWidths[] = {4, 8, 16, 24}; /* 24, 48, 96, 144 RB */
      for (unsigned wi = 0; wi < sizeof(kCommonWidths) / sizeof(kCommonWidths[0]) && n < max_out; wi++) {
        const int w = kCommonWidths[wi];
        if (w > nw_total) {
          continue;
        }
        for (int f = 0; f + w - 1 < nw_total && n < max_out; f++) {
          const int l = f + w - 1;
          bool dup = false;
          for (int i = 0; i < n; i++) {
            if (out[i].first_w == f && out[i].last_w == l) {
              dup = true;
              break;
            }
          }
          if (dup) {
            continue;
          }
          out[n].first_w = f;
          out[n].last_w  = l;
          n++;
        }
      }
    }
  }
  return n;
}


static bool extent_catalog_add(nr_pdcch_extent_cand_t *out, int *n, int max_out,
                               int first_w, int last_w, int nw_total)
{
  if (first_w < 0 || last_w < first_w || last_w >= nw_total)
    return false;
  for (int i = 0; i < *n; ++i)
    if (out[i].first_w == first_w && out[i].last_w == last_w)
      return true;
  if (*n >= max_out)
    return false;
  out[*n] = (nr_pdcch_extent_cand_t){.first_w = first_w, .last_w = last_w};
  ++*n;
  return true;
}

int nr_pdcch_extent_candidates_multi(const int *seed_w, int nseed, int nw_total,
                                     nr_pdcch_extent_cand_t *out, int max_out)
{
  if (out == NULL || max_out <= 0 || nw_total <= 0 || nseed < 0
      || (nseed > 0 && seed_w == NULL))
    return 0;
  for (int i = 0; i < nseed; ++i)
    if (seed_w[i] < 0 || seed_w[i] >= nw_total)
      return 0;

  int n = 0;
  int prior_first = -1, prior_last = -1;
  if (nr_pdcch_sib1_prior_window(&prior_first, &prior_last) && prior_last < nw_total)
    extent_catalog_add(out, &n, max_out, prior_first, prior_last, nw_total);

  /* Treat recurrent peaks independently. Joining two peaks into one observed span made every
   * candidate contain both, which made two per-UE CORESETs undiscoverable by construction. */
  for (int i = 0; i < nseed; ++i)
    extent_catalog_add(out, &n, max_out, seed_w[i], seed_w[i], nw_total);

  /* BWP-wide CORESETs are common and occupancy only shows active CCEs, not the edges. */
  extent_catalog_add(out, &n, max_out, 0, nw_total - 1, nw_total);

  /* Put common widths near every recurrent peak ahead of the exhaustive fallback. Every legal
   * start containing the peak is retained; no centre/edge guess can exclude the answer. */
  static const int common_widths[] = {4, 8, 16, 24};
  for (unsigned wi = 0; wi < sizeof(common_widths) / sizeof(common_widths[0]); ++wi) {
    const int width = common_widths[wi];
    if (width > nw_total)
      continue;
    for (int i = 0; i < nseed; ++i) {
      const int lo = seed_w[i] - width + 1 > 0 ? seed_w[i] - width + 1 : 0;
      const int hi = seed_w[i] < nw_total - width ? seed_w[i] : nw_total - width;
      const int centre = seed_w[i] - (width - 1) / 2;
      for (int d = 0; d < nw_total; ++d) {
        const int f0 = centre - d;
        const int f1 = centre + d;
        if (f0 >= lo && f0 <= hi)
          extent_catalog_add(out, &n, max_out, f0, f0 + width - 1, nw_total);
        if (d && f1 >= lo && f1 <= hi)
          extent_catalog_add(out, &n, max_out, f1, f1 + width - 1, nw_total);
      }
    }
  }

  /* Completeness is the safety property: occupancy ranks the search but cannot veto geometry.
   * Shorter extents come first because they are cheaper to demap and distinguish separate
   * per-UE CORESETs; the full-carrier fast path above preserves the wide-CORESET case. */
  for (int width = 1; width <= nw_total && n < max_out; ++width)
    for (int first = 0; first + width <= nw_total && n < max_out; ++first)
      extent_catalog_add(out, &n, max_out, first, first + width - 1, nw_total);
  return n;
}

#define AUTODISCOVER_MIN_HITS  3
#define AUTODISCOVER_HITS_PER_WINDOW 30   // mean hits/window required before deciding --
                                          // makes MIN_HITS a real floor rather than noise
#define AUTODISCOVER_MAX_OBS_CALLS 400000 // safety stop (~3 min of DL occasions) so an
                                          // absent CORESET cannot accumulate forever     // measured: a real window clears the raw 0.836 threshold (not
                                     // just "wins outright") on a large fraction of calls; a window
                                     // that never once does so in 1000 calls is noise, not signal

/* Phase 3: orchestrate Techniques A/B/C into a self-contained "recover the dedicated CORESET"
 * attempt. Called from the RT path (nr_pdcch_blind_monitor_rt.c) once per symbol while
 * autodiscover is on AND the dedicated CORESET has not been found yet (see
 * nr_pdcch_blind_monitor_autodiscover_done() above, NOT g_cfg.bwp_size). Cheap to call
 * repeatedly -- it is a no-op once found (checked by the caller, not here, so this function's own
 * logic stays simple: "try once, report success/failure"). */
/* CORESET DURATION (2026-09-16). Technique A scanned symbol 0 only and the footprint was declared
 * with duration 1 -- an assumption a commercial cell need not satisfy. The caller also offers symbol
 * 1; a window lit there at a rate comparable to symbol 0 means the CORESET spans both symbols. */
void nr_pdcch_blind_monitor_autodiscover_observe_symbol1(const void* rxdataF_symbol, int ofdm_symbol_size,
                                                          int n_rb_carrier, int first_carrier_offset, uint16_t pci,
                                                          int slot)
{
  nr_pdcch_coreset_candidate_t candidates[NR_PDCCH_MAX_CANDIDATE_WINDOWS];
  const int n = nr_pdcch_coreset_map_scan((const c16_t*)rxdataF_symbol, ofdm_symbol_size, n_rb_carrier,
                                          first_carrier_offset, pci, slot, 1, candidates,
                                          NR_PDCCH_MAX_CANDIDATE_WINDOWS);
  for (int c = 0; c < n; c++) {
    const int w = candidates[c].rb_offset / 6;
    if (w >= 0 && w < NR_PDCCH_MAX_CANDIDATE_WINDOWS)
      s_hit_count1[w]++;
  }
}

bool nr_pdcch_blind_monitor_autodiscover_step(const void* rxdataF_symbol, int ofdm_symbol_size, int n_rb_carrier,
                                              int first_carrier_offset, uint16_t pci, int slot, int symbol,
                                              uint32_t abs_slot)
{
  nr_pdcch_coreset_candidate_t candidates[NR_PDCCH_MAX_CANDIDATE_WINDOWS];
  const int n = nr_pdcch_coreset_map_scan((const c16_t*)rxdataF_symbol, ofdm_symbol_size, n_rb_carrier,
                                          first_carrier_offset, pci, slot, symbol, candidates,
                                          NR_PDCCH_MAX_CANDIDATE_WINDOWS);
  {
    /* DIAGNOSTIC (env-gated, kept permanently -- same convention as this project's other ISAC_*
     * debug flags). Originally added 2026-09-05 to debug a then-zero-convergence result; kept
     * because it is what surfaced the real picture 2026-09-06 (see this function's convergence-
     * criterion comment above): n>0 with strong (0.9+) correlation on most calls, but the winning
     * window changing call to call. Rate-limited to avoid flooding the RT thread's own log volume. */
    static int s_diag = -1;
    if (s_diag < 0)
      s_diag = (getenv("ISAC_DISCOVER_DIAG") != NULL) ? 1 : 0;
    static int s_calls = 0;
    s_calls++;
    if (s_diag && (n > 0 || (s_calls % 200) == 1)) {
      if (n > 0) {
        printf("DISCOVERDIAG calls=%d n=%d top_rb=%d top_corr=%.3f\n", s_calls, n,
              candidates[0].rb_offset, candidates[0].corr);
      } else {
        printf("DISCOVERDIAG calls=%d n=0\n", s_calls);
      }
      fflush(stdout);
    }
  }
  // Accumulate hits toward the observation window regardless of n==0 -- a genuinely idle call is
  // itself informative (real windows stay at 0 too on an idle call), and returning early here would
  // under-count elapsed dwell against AUTODISCOVER_OBS_CALLS.
  for (int c = 0; c < n; c++) {
    const int w = candidates[c].rb_offset / 6;
    if (w >= 0 && w < NR_PDCCH_MAX_CANDIDATE_WINDOWS) {
      s_hit_count[w]++;
    }
  }
  s_obs_calls++;
  /* TERMINATION IS HIT-DRIVEN, NOT CALL-DRIVEN (fixed 2026-09-07). A fixed observation length in
   * CALLS silently changes meaning with offered load: this function is invoked on every DL
   * occasion (~2000/s) but only accumulates a hit when a PDCCH is actually present, so at 38
   * grants/s only ~2 % of calls contribute. MEASURED at that load: 1000 calls yielded total_hits
   * of 94 and 4 in two consecutive observation windows, against 45 windows to populate -- so which
   * windows cleared AUTODISCOVER_MIN_HITS was decided by noise, and three consecutive live runs
   * converged to three different footprints (84/168, 240/12, 12/240) against a truth of 0/270.
   *
   * Wait for enough EVIDENCE instead: a real full-band CORESET populates every window, so require
   * the histogram to hold AUTODISCOVER_HITS_PER_WINDOW hits per window on average before deciding.
   * That makes AUTODISCOVER_MIN_HITS a meaningful floor (a real window then expects ~10 hits, not
   * ~2) and makes the dwell self-scaling: it ends quickly on a busy cell and simply waits longer on
   * a quiet one, instead of returning a confident wrong answer. The call cap is a safety stop so a
   * dead/absent CORESET cannot spin forever; reaching it resets and retries. */
  int obs_total_hits = 0;
  for (int w = 0; w < NR_PDCCH_MAX_CANDIDATE_WINDOWS; w++) {
    obs_total_hits += s_hit_count[w];
  }
  /* Evidence needed = HITS_PER_WINDOW on the LIT windows, not on every window of the carrier: the
   * old n_windows x 30 (1380 on 273 PRB) was sized for a full-carrier CORESET lit every slot; a
   * narrow dedicated CORESET (rfsim OAI cell: 2 windows at RB 90, corr 0.88, 2026-09-16) or sparse
   * traffic never reached it and discovery sat at n=0 forever. */
  /* "Lit" is RELATIVE to the strongest window (>= top/8), not an absolute 3 hits: over a long dwell
   * noise windows cross 3 and the needed count chased the total forever (rfsim: lit 13 -> 27 while
   * hits 153 -> 411, needed always ~2x ahead). */
  int obs_top = 0;
  for (int w = 0; w < NR_PDCCH_MAX_CANDIDATE_WINDOWS; w++)
    if (s_hit_count[w] > obs_top) obs_top = s_hit_count[w];
  /* LIT = SIGNIFICANT AGAINST THE BACKGROUND, not a fraction of the peak (2026-09-21).
   * top/8 is a RELATIVE rule: it cannot distinguish "everything lit" from "nothing lit", which is
   * exactly how an ungated, duty-cycle-diluted histogram saturated to 0..44 and triggered the
   * ~133-footprint fallback. With the occupancy gate in place the histogram is now Poisson counts
   * over a low background, so the right test is against that background: a window is lit iff it
   * exceeds mean + k*sqrt(mean) of the OTHER windows. MEASURED on the first gated capture --
   * background mean 4.75, peak window 39 at 64 hits = 27 sigma -- where top/8 = 8 admitted nine
   * windows (span 1..40) and this admits one (span 39..39).
   * The background is the MEDIAN-based mean rather than the plain mean so the peak cannot inflate
   * the very threshold meant to exclude it. AUTODISCOVER_MIN_HITS remains the absolute floor. */
  int lit_floor;
  {
    int v[NR_PDCCH_MAX_CANDIDATE_WINDOWS], m = 0;
    for (int w = 0; w < n_rb_carrier / 6 && w < NR_PDCCH_MAX_CANDIDATE_WINDOWS; w++)
      v[m++] = s_hit_count[w];
    for (int i = 1; i < m; i++) { int x = v[i]; int j = i - 1; while (j >= 0 && v[j] > x) { v[j+1] = v[j]; j--; } v[j+1] = x; }
    const double bg = (m > 0) ? (double)v[m / 2] : 0.0;   /* median = background, peak-immune */
    const double k = 5.0;
    const int stat_floor = (int)(bg + k * sqrt(bg > 1.0 ? bg : 1.0) + 0.5);
    lit_floor = stat_floor > AUTODISCOVER_MIN_HITS ? stat_floor : AUTODISCOVER_MIN_HITS;
    /* NO CLAMP TO THE PEAK. It was here so a uniform CORESET could not go unlit, but it also
     * guarantees the strongest window is ALWAYS lit, which defeats the significance test entirely:
     * MEASURED 2026-09-21, a window at 11 hits against a background of 6 (~2 sigma, i.e. noise)
     * was declared a footprint (rb_offset=12 span_rb=6) purely because it was the maximum. When
     * nothing is significant the right answer is to keep observing -- which the caller already
     * does when first_w < 0. */
  }
  int obs_lit = 0;
  for (int w = 0; w < NR_PDCCH_MAX_CANDIDATE_WINDOWS; w++)
    obs_lit += (s_hit_count[w] >= lit_floor);
  const int obs_nw = obs_lit > 0 ? obs_lit : 1;
  const int obs_hits_needed = obs_nw * AUTODISCOVER_HITS_PER_WINDOW;
  {
    static int s_gate_diag = -1;
    if (s_gate_diag < 0) s_gate_diag = (getenv("ISAC_DISCOVER_DIAG") != NULL) ? 1 : 0;
    if (s_gate_diag && (s_obs_calls % 5000) == 0) {
      printf("DISCOVERGATE calls=%d total_hits=%d top=%d floor=%d lit=%d needed=%d\n", s_obs_calls, obs_total_hits, obs_top, lit_floor, obs_lit, obs_hits_needed);
      fflush(stdout);
    }
  }
  if (s_obs_calls >= AUTODISCOVER_MAX_OBS_CALLS && obs_total_hits < obs_hits_needed) {
    /* Waited long enough and the evidence never arrived -- reset rather than decide on noise. */
    memset(s_hit_count, 0, sizeof(s_hit_count));
  memset(s_hit_count1, 0, sizeof(s_hit_count1));
    s_obs_calls = 0;
    return false;
  }
  /* BACKGROUND MUST BE ESTIMABLE BEFORE THE SIGNIFICANCE TEST CAN RUN -- see the note on
   * lit_floor. With the occupancy gate the histogram fills ~20x more slowly than the 1000-call
   * dwell assumes, and a median of 0 turns `median + 5*sqrt(median)` into a flat floor of 5, which
   * declared a different footprint every dwell (rb_offset 12 / 36 / 42; peaks at windows 39/17/6).
   * Hold the decision until the median window count is meaningful. Still bounded by
   * AUTODISCOVER_MAX_OBS_CALLS, so a genuinely empty cell gives up exactly as before. */
  {
    static int s_min_bg = -1;
    if (s_min_bg < 0) {
      const char *e = getenv("ISAC_DISCOVER_MIN_BG");
      s_min_bg = (e != NULL) ? atoi(e) : 3;
      if (s_min_bg < 0) s_min_bg = 0;
    }
    if (s_min_bg > 0) {
      int v[NR_PDCCH_MAX_CANDIDATE_WINDOWS], m = 0;
      for (int w = 0; w < n_rb_carrier / 6 && w < NR_PDCCH_MAX_CANDIDATE_WINDOWS; w++)
        v[m++] = s_hit_count[w];
      for (int i = 1; i < m; i++) { int x = v[i]; int j = i - 1; while (j >= 0 && v[j] > x) { v[j+1] = v[j]; j--; } v[j+1] = x; }
      const int bg = (m > 0) ? v[m / 2] : 0;
      if (bg < s_min_bg && s_obs_calls < AUTODISCOVER_MAX_OBS_CALLS) {
        return false;  /* keep observing: the background is not yet estimable */
      }
    }
  }
  if (s_obs_calls < AUTODISCOVER_OBS_CALLS || obs_total_hits < obs_hits_needed) {
    return false;
  }

  /* DISCOVERHIST (ISAC_DISCOVER_DIAG=1): dump the WHOLE per-window hit histogram at the decision
   * point. Added 2026-09-07 because three consecutive live runs converged to three DIFFERENT
   * footprints (84/168, 240/12, 12/240) against a known truth of rb_offset=0 span_rb=270, all of
   * them ending at RB 252 -- which the first_w/last_w rule alone cannot explain. Whether the fix is
   * "the MIN_HITS threshold is too strict" or "there is a real artifact at the top of the band"
   * depends on the SHAPE of this histogram, so measure it before changing the rule. */
  {
    static int s_hist_diag = -1;
    if (s_hist_diag < 0) {
      s_hist_diag = (getenv("ISAC_DISCOVER_DIAG") != NULL) ? 1 : 0;
    }
    if (s_hist_diag) {
      char h[1400];
      int p = 0, tot = 0;
      const int nw = n_rb_carrier / 6;
      for (int w = 0; w < nw && w < NR_PDCCH_MAX_CANDIDATE_WINDOWS; w++) {
        tot += s_hit_count[w];
        if (p < (int)sizeof(h) - 12) {
          p += snprintf(h + p, sizeof(h) - p, "%u ", (unsigned)s_hit_count[w]);
        }
      }
      printf("DISCOVERHIST calls=%d nw=%d total_hits=%d min_hits=%d hits: %s\n",
             s_obs_calls, nw, tot, AUTODISCOVER_MIN_HITS, h);
      fflush(stdout);
    }
  }

  // Decision point: which windows repeatedly cleared the threshold over the observation window?
  // first_w/last_w of the confirmed set, NOT requiring every window in between to also be
  // confirmed -- the same "generous, scan-everything" philosophy this function already applies
  // below to fields it cannot determine precisely. A short internal gap of unconfirmed windows
  // (a CCE range this dwell just didn't happen to use) is still safely inside a real CORESET's span.
  /* TOP-3 peaks with their significance -- the lit SPAN hides whether a second CORESET exists. */
  {
    int t[3] = {-1, -1, -1};
    for (int w = 0; w < n_rb_carrier / 6 && w < NR_PDCCH_MAX_CANDIDATE_WINDOWS; w++)
      for (int i = 0; i < 3; i++)
        if (t[i] < 0 || s_hit_count[w] > s_hit_count[t[i]]) {
          for (int j = 2; j > i; j--) t[j] = t[j - 1];
          t[i] = w;
          break;
        }
    int v[NR_PDCCH_MAX_CANDIDATE_WINDOWS], m = 0;
    for (int w = 0; w < n_rb_carrier / 6 && w < NR_PDCCH_MAX_CANDIDATE_WINDOWS; w++) v[m++] = s_hit_count[w];
    for (int i = 1; i < m; i++) { int x = v[i]; int j = i - 1; while (j >= 0 && v[j] > x) { v[j+1] = v[j]; j--; } v[j+1] = x; }
    const double bg = (m > 0) ? (double)v[m / 2] : 0.0;
    const double sd = sqrt(bg > 1.0 ? bg : 1.0);
    LOG_A(PHY,
          "SENSING: COREMAPTOP bg=%.1f excl_w=%d..%d top: w%d=%d(%.1fsig) w%d=%d(%.1fsig) w%d=%d(%.1fsig)\n",
          bg, s_css0_excl_first_w, s_css0_excl_last_w,
          t[0], t[0] >= 0 ? s_hit_count[t[0]] : 0, t[0] >= 0 ? (s_hit_count[t[0]] - bg) / sd : 0.0,
          t[1], t[1] >= 0 ? s_hit_count[t[1]] : 0, t[1] >= 0 ? (s_hit_count[t[1]] - bg) / sd : 0.0,
          t[2], t[2] >= 0 ? s_hit_count[t[2]] : 0, t[2] >= 0 ? (s_hit_count[t[2]] - bg) / sd : 0.0);
  }
  /* LONG-TERM ACCUMULATION + RECURRENCE REPORT. The per-dwell histogram is about to be consumed
   * and reset; fold it into evidence that survives, because recurrence across dwells -- not peak
   * height within one -- is what distinguishes a UE's CORESET from a burst. */
  {
    const int nw = n_rb_carrier / 6;
    int t3[3] = {-1, -1, -1};
    for (int w = 0; w < nw && w < NR_PDCCH_MAX_CANDIDATE_WINDOWS; w++) {
      s_lt_hits[w] += (unsigned long)s_hit_count[w];
      if (s_css0_excl_first_w >= 0 && w >= s_css0_excl_first_w && w <= s_css0_excl_last_w) continue;
      for (int i = 0; i < 3; i++)
        if (t3[i] < 0 || s_hit_count[w] > s_hit_count[t3[i]]) {
          for (int j = 2; j > i; j--) t3[j] = t3[j - 1];
          t3[i] = w;
          break;
        }
    }
    for (int i = 0; i < 3; i++)
      if (t3[i] >= 0 && s_hit_count[t3[i]] > 0) s_lt_dwells[t3[i]]++;
    s_lt_ndwell++;
    int ord[NR_PDCCH_MAX_CANDIDATE_WINDOWS], n_ord = 0;
    for (int w = 0; w < nw && w < NR_PDCCH_MAX_CANDIDATE_WINDOWS; w++)
      if (s_lt_dwells[w] > 0) ord[n_ord++] = w;
    for (int i = 1; i < n_ord; i++) {
      const int x = ord[i];
      int j = i - 1;
      while (j >= 0 && s_lt_dwells[ord[j]] < s_lt_dwells[x]) { ord[j + 1] = ord[j]; j--; }
      ord[j + 1] = x;
    }
    char b[700];
    int u = 0;
    for (int i = 0; i < n_ord && i < 8 && u < (int)sizeof(b) - 40; i++) {
      const int w = ord[i];
      u += snprintf(b + u, sizeof(b) - u, "w%d:%u/%u dwells,%lu hits,rnti=0x%04x  ",
                    w, s_lt_dwells[w], s_lt_ndwell, s_lt_hits[w], s_lt_rnti[w]);
    }
    LOG_A(PHY, "SENSING: COREMAPLT dwell=%u (recurrence across dwells; excl w%d..%d) %s\n",
          s_lt_ndwell, s_css0_excl_first_w, s_css0_excl_last_w, b);
  }
  const int nw_total = n_rb_carrier / 6;

  /* Do not commit a permanent catalogue from one transient dwell. Measured on Salt, the old path
   * committed after dwell 3 to w13 although the persistent table had w0 and w13 tied at 2/3. It
   * then generated only the 322 intervals containing w13, making every CORESET elsewhere
   * impossible to discover. Eight independent dwells are cheap compared with the catalogue walk. */
  enum { MIN_ORACLE_DWELLS = 8, MAX_ORACLE_SEEDS = 8 };
  if (s_lt_ndwell < MIN_ORACLE_DWELLS) {
    memset(s_hit_count, 0, sizeof(s_hit_count));
    memset(s_hit_count1, 0, sizeof(s_hit_count1));
    s_obs_calls = 0;
    return false;
  }

  int seeds[MAX_ORACLE_SEEDS];
  int nseed = 0;
  const unsigned recurrence_floor = (s_lt_ndwell + 4) / 5; /* >=20% of independent dwells */
  for (int rank = 0; rank < MAX_ORACLE_SEEDS; ++rank) {
    int best = -1;
    for (int w = 0; w < nw_total && w < NR_PDCCH_MAX_CANDIDATE_WINDOWS; ++w) {
      if (s_css0_excl_first_w >= 0 && w >= s_css0_excl_first_w && w <= s_css0_excl_last_w)
        continue;
      bool used = false;
      for (int i = 0; i < nseed; ++i)
        used |= seeds[i] == w;
      if (!used && s_lt_dwells[w] >= recurrence_floor
          && (best < 0 || s_lt_dwells[w] > s_lt_dwells[best]
              || (s_lt_dwells[w] == s_lt_dwells[best] && s_lt_hits[w] > s_lt_hits[best])))
        best = w;
    }
    if (best < 0)
      break;
    seeds[nseed++] = best;
  }

  /* Geometry verification uses only fresh dedicated DCI evidence collected in this epoch. The
   * catalogue is complete even when nseed==0: oracle evidence changes order, never eligibility. */
  s_ext_n = nr_pdcch_extent_candidates_multi(seeds, nseed, nw_total,
                                             s_ext_cand, NR_PDCCH_EXTENT_MAX_CAND);
  if (s_ext_n <= 0)
    return false;
  s_ext_idx = 0;
  s_ext_phase_idx = 0;
  extent_clear_evidence();

  int first_w = s_ext_cand[0].first_w;
  int last_w  = s_ext_cand[0].last_w;
  LOG_A(PHY, "SENSING: recurrent oracle committed after %u dwells: seeds=%d floor=%u catalog=%d\n",
        s_lt_ndwell, nseed, recurrence_floor, s_ext_n);
  /* The oracle phase orders six physical RB-phase hypotheses; it is not dedicated-BWP truth. */
  const int rb_offset = first_w * 6 + extent_phase(0);
  const int span_rb   = (last_w - first_w + 1) * 6;

  g_cfg.coreset_type            = 0;  // PDCCH-Config (dedicated), NOT MIB/SIB1 -- see coreset_type's
                                        // own comment in autoconf_css0() for why this field matters
  g_cfg.coreset_freq_domain     = span_rb / 6;
  /* The CORESET's own offset, in ITS OWN frame. It used to be written into bwp_start, which is a
   * DIFFERENT frame of reference: BWPStart moves the bandwidth part, and with it RIV interpretation
   * and dci_length, so a nonzero CORESET offset silently corrupted the frequency allocation of
   * every grant. Inert on this cell (the dedicated CORESET starts at RB 0, so both were 0) -- and
   * that is exactly why it survived: it is only reachable once a footprint with a nonzero offset is
   * discovered, which is what the extent sweep would have started producing. */
  g_cfg.coreset_rb_offset       = rb_offset;
  g_cfg.bwp_start               = 0;  // full-carrier BWP; see above
  /* bwp_size is the DL BWP size (drives the RIV/frequency-allocation field-width computation), NOT
   * the discovered CORESET span -- a dedicated CORESET is normally a subset of its BWP. The real
   * RRC-configured dedicated BWP is ciphered and unavailable to a passive receiver (the same
   * reasoning that makes this whole feature necessary), so use the full carrier width, exactly as
   * this project's own manual ground-truth dedicated config does
   * (tests/passive_rx/ota/nrue.passive_rx.conf: "bwp = 0:273:...", the full 273 PRB carrier). The
   * discovered footprint stays only in bwp_start/coreset_freq_domain above. */
  g_cfg.bwp_size                = n_rb_carrier;
  g_cfg.coreset_pdcch_dmrs_scrambling_id = pci;
  // shift_index=0, not pci: matches the manual ground-truth dedicated conf's own field 5
  // (tests/passive_rx/ota/nrue.passive_rx.conf: "45:1:0:0:0:2"), same reasoning as
  // coreset_interleaver_size below -- inert either way given reg_bundle_size=0 (both
  // nr_pdcch_demapping_deinterleaving() and cce_to_reg_interleaving() take the identity path
  // and never read ShiftIndex when non-interleaved), but 0 is the value that's actually
  // correct here, not a default guess of pci.
  g_cfg.coreset_shift_index     = 0;
  /* mapping list for candidate 0 is started below, once the duration is decided */

  /* CSS0 autoconf (a hard prerequisite for this feature's bootstrap RNTI -- see the autodiscover
   * conf knob's own comment) runs first and populates these SAME g_cfg fields for CORESET#0/SIB1.
   * Since this function overwrites g_cfg IN PLACE, anything CSS0 set that isn't touched here stays
   * leaked from the common search space into what is now a dedicated-CORESET search, where it is
   * actively wrong (a SIB1-only RNTI pin, format-1_0-only scanning, AL4/AL8-pinned candidates,
   * CSS0's own energy-gate-off choice). Reset every one of them to the values this project's own
   * known-working manual dedicated config uses
   * (tests/passive_rx/ota/nrue.passive_rx.conf's pdcch_blind_monitor_{ss,rnti_range,dci10,
   * noise_gates} lines), not CSS0's CORESET#0/SIB1-specific choices. */
  /* ALL AGGREGATION LEVELS AUTO. Read the units warning before changing any of these.
   *
   * srsRAN's `dci_aggregation_level=N` in the gNB log is **log2(L), NOT L**. This cell logs
   * `dci_aggregation_level=1`, which is **L=2 (AL2)**, not AL1. Verified against the receiver's own
   * FULLCRC probe, which is the only metric that can see a real decode: every genuine recovery of
   * the live C-RNTI 0x4604 came out at **L=2** (36x `L=2 dci_len=47` for the DL 1_1 grants, 9x
   * `L=2 dci_len=43` for the UL 0_1 grants). An AL1-ONLY conf recovered the live C-RNTI ZERO times
   * in the same conditions, while an AL2/4/8 conf recovered it 26496 times.
   *
   * An earlier revision of this block on 2026-09-06 read that log field as a literal AL, concluded
   * "this cell is 100% AL1", and is RETRACTED. The `blind-pdcch-needs-al1-scanning` memory makes
   * the same units mistake -- do not quote its "99.997% at AL1" without re-checking.
   *
   * Leaving AL1 at `0` (auto) rather than `-1` is harmless and slightly more robust: the allocator
   * fills AL2/AL4/AL8 first and AL1 last, so on this 45-CCE CORESET AL1 receives zero candidates
   * anyway (measured ladder: `AL1=0 AL2=13 AL4=6 AL8=3`). If a future deployment really does use
   * AL1, it needs the OTHER levels capped/disabled to free budget -- "auto" alone will not do it. */
  g_cfg.ss_al_candidates[0] = 0;  // AL1 auto (gets no budget here; real grants are AL2 on this cell)
  g_cfg.ss_al_candidates[1] = 0;  // AL2 auto
  g_cfg.ss_al_candidates[2] = 0;  // AL4 auto
  g_cfg.ss_al_candidates[3] = 0;  // AL8 auto
  g_cfg.rnti_min = NR_PDCCH_BLIND_RNTI_MIN_DEFAULT;  // wide dynamic C-RNTI range, not CSS0's
  g_cfg.rnti_max = NR_PDCCH_BLIND_RNTI_MAX_DEFAULT;  // SI-RNTI-only pin (rnti_min=rnti_max=0xFFFF)
  g_cfg.dci10_scan        = 1;  // scan BOTH 1_0 and 1_1 -- CSS0 pins 2 (1_0-only, correct for a
                                // SIB1-only common search space), but this feature exists to
                                // recover DEDICATED 1_1 decode, so 1_1 scanning must not stay off
  g_cfg.dci10_ss_type     = 0;  // UE-specific (NR_BLIND_SS_UE_SPECIFIC), not CSS0's _COMMON
  g_cfg.dci10_n_rb_riv    = 0;  // auto: bwp_size above, not CORESET#0's own span
  g_cfg.dci10_rb_offset   = -1; // auto: bwp_start above, not CORESET#0's rb_offset=0
  g_cfg.dci10_mux_pattern = 0;  // only consulted for SIB1, meaningless here
  g_cfg.dci10_sib1        = 0;  // this is not SIB1 -- leaving CSS0's sib1=1 would apply the
                                // mux-pattern default TDRA table to a dedicated 1_0 decode
  /* ENERGY GATE: DELIBERATELY NOT SET HERE (2026-09-07). This used to hardcode
   * energy_adapt_factor = 3.0f, which silently OVERRODE whatever pdcch_blind_monitor_noise_gates
   * said in the conf -- so disabling the gate in the conf had no effect on the autodiscover path
   * and was impossible to diagnose from the config surface.
   *
   * MEASURED with the override in place, energy gate nominally disabled in the conf:
   * held[energy=17296703] persist=0 snr=0, accepts=0 -- the gate rejected EVERY candidate for a
   * whole 240 s run, while the same run's raw decodes recovered 158899 genuine C-RNTI (0x462d)
   * payloads. The adaptive floor estimator does not survive this cell's dedicated-CORESET
   * candidate population, so a 3x-floor threshold throws away all the real grants.
   *
   * g_cfg already holds the conf's parsed value at this point, so leaving both fields untouched
   * makes the gate an OPERATOR decision (parse_noise_gates()) rather than something autodiscover
   * asserts. The manual dedicated conf still runs it at 3.0 by saying so explicitly. */

  /* Fields Technique A cannot determine (it detects OCCUPANCY, not CORESET/SS structure) but that
   * nr_pdcch_blind_monitor_process()'s existing occasion gate and run_occasion()'s CORESET builder
   * both require non-zero/valid before they will do anything at all -- without these, "geometry
   * found" would still produce zero scanning, silently. Deliberately conservative, scan-everything
   * defaults rather than guesses tuned to one deployment:
   *  - coreset_duration=1: every dedicated CORESET this project has captured is 1 symbol (see the
   *    repeated "S=1/L=13 for a 1-symbol CORESET" note elsewhere in this file).
   *  - coreset_reg_bundle_size=0: non-interleaved, this project's own live-confirmed dedicated
   *    CORESET mapping (run_occasion()'s own comment: "NON INTERLEAVED reg_bundle_sz=6" -> the
   *    CONFIG field encoding for that is 0, not 6 -- see that comment for the encoding).
   *  - ss_monitoring_slot_periodicity=1/offset=0/duration=1: monitor EVERY slot. We have no way to
   *    derive the real dedicated SS periodicity from occupancy alone, and scanning every slot is
   *    always a superset of any real (sparser) schedule -- costs CPU, not correctness.
   *  - ss_first_symbol = the symbol Technique A actually locked onto.
   *  - dmrs_typeA_position: left exactly as CSS0 autoconf/the conf set it. It is the ASN.1 enum
   *    (pos2 = 0), so the zero default is already the TS 38.331 spec default -- see the block
   *    further down where an earlier "fix it up to 2" made it illegal.
   * ponytail: fixed "scan every slot" ceiling -- ~2000 extra occasions/s of CPU on a cell whose
   * real dedicated SS periodicity is sparser. Upgrade path: derive periodicity from the actual
   * inter-occupancy gap Technique A already measures, once that's shown to matter live. */
  {
    uint32_t h0 = 0, h1 = 0;
    for (int w = first_w; w <= last_w && w < NR_PDCCH_MAX_CANDIDATE_WINDOWS; w++) { h0 += s_hit_count[w]; h1 += s_hit_count1[w]; }
    g_cfg.coreset_duration = (h0 > 0 && h1 * 2 >= h0) ? 2 : 1; /* symbol 1 lit at >= half of symbol 0's rate */
    LOG_I(PHY, "SENSING: Phase 3 autodiscover -- CORESET duration %d (symbol-0 hits %u, symbol-1 hits %u over the footprint)\n",
          g_cfg.coreset_duration, h0, h1);
  }
  /* Start a new occupancy window for any later inconclusive retry. MUST run after the duration
   * readout above, not before: this used to sit right after first_w/last_w were picked, which
   * zeroed s_hit_count[]/s_hit_count1[] before the duration block below could read them -- every
   * declared footprint logged "symbol-0 hits 0, symbol-1 hits 0" and coreset_duration was silently
   * forced to 1 regardless of the real evidence, live-measured 2026-09-17 (rb_offset=0 span_rb=48
   * bootstrap_rnti=0x0, exhausting and rediscovering every ~50s on a real commercial cell). */
  memset(s_hit_count, 0, sizeof(s_hit_count));
  memset(s_hit_count1, 0, sizeof(s_hit_count1));
  s_obs_calls = 0;
  /* CCE-to-REG mapping: hypothesis 0 is non-interleaved (bundle 0 -- the demapper's identity
   * path, this project's every captured dedicated CORESET); the interleaved (L, R, shift)
   * hypotheses follow, each with the same dwell, when the non-interleaved one collects no
   * dedicated-DCI evidence (nr_pdcch_map_candidates, extent_advance). */
  map_restart(span_rb, g_cfg.coreset_duration, pci);
  g_cfg.ss_monitoring_slot_periodicity   = 1;
  g_cfg.ss_monitoring_slot_offset        = 0;
  g_cfg.ss_duration                      = 1;
  g_cfg.ss_first_symbol                  = symbol;
  /* dmrs_typeA_position is the ASN.1 ENUM (NR_MIB__dmrs_TypeA_Position_pos2 = 0, pos3 = 1), NOT a
   * symbol index. 0 IS the spec default (pos2), so it needs no fixing up -- and writing 2 here made
   * blind_fill_dmrs_mask() match neither enum branch and return -1, rejecting EVERY genuine grant
   * with "DM-RS symbol mask undefined for this TDRA entry / additional-position". Measured
   * 2026-09-07 with ISAC_DCI_WATCH_RNTI: 100 % of real C-RNTI decodes rejected there in auto,
   * while the manual conf (which pins 0 via pdcch_blind_monitor_bwp field 3) accepted 29,331.
   * Leave the parsed/CSS0 value alone. */

  uint16_t bootstrap_rnti = 0;
  uint8_t  bootstrap_class = 0xFF;
  uint32_t age = 0;
  nr_pdcch_blind_monitor_confirmed_rnti(abs_slot, &bootstrap_rnti, &bootstrap_class, &age);
  // Only bootstrap_rnti is consumed below (the log line); the function unconditionally writes
  // through all three out-params (see nr_pdcch_blind_rnti_bootstrap.c), so these two can't be NULL.
  (void)bootstrap_class;
  (void)age;

  s_dedicated_found = true;
  /* Initialize only after the discovered duration, BWP and PCI are final. */
  lookahead_lanes_init();

  LOG_A(PHY, "SENSING: Phase 3 autodiscover -- CORESET footprint rb_offset=%d span_rb=%d "
            "bootstrap_rnti=0x%x\n", rb_offset, span_rb, bootstrap_rnti);
  return true;  // g_cfg's CORESET fields are now populated; dci_length sweep is the caller's next step
}

/* Technique C's result lands here rather than at a direct g_cfg write from the RT tap, since g_cfg
 * is a static owned by this translation unit and the RT tap only ever sees the const accessor. */
void nr_pdcch_blind_monitor_autodiscover_set_dci_length(int dci_length)
{
  g_cfg.dci_length_override = dci_length;
}

void nr_pdcch_blind_monitor_autodiscover_next(void)
{
  if (!s_ext_verified)
    return;
  /* The caller has copied the verified config into its operational bank. Re-arm only discovery;
   * CSS0 and the long-term occupancy evidence remain valid. */
  s_dedicated_found = false;
  s_ext_n = s_ext_idx = s_ext_phase_idx = 0;
  s_map_stage = 0;
  s_map_n = s_map_idx = 0;
  s_lane_dispatch_ext = s_lane_dispatch_map = s_lane_dispatch_phase = 0;
  s_lane_dispatch_stage = s_lane_dispatch_map_max = 0;
  memset(s_hit_count, 0, sizeof(s_hit_count));
  memset(s_hit_count1, 0, sizeof(s_hit_count1));
  s_obs_calls = 0;
  g_cfg.dci_length_override = 0;
  memset(s_lane, 0, sizeof(s_lane));
  extent_clear_evidence();
  discovered_verified(); /* a banked discovered CORESET moves the hand-off on (and may pause the walk) */
  LOG_A(PHY, "SENSING: multi-CORESET discovery resumed (verified bank retained by RT monitor)\n");
}

/* Called once per candidate-bearing occasion once the footprint is found. Returns true when it
 * has just CHANGED the applied extent, so the caller can rebuild anything derived from it. */
bool nr_pdcch_blind_monitor_autodiscover_extent_step(uint32_t abs_slot)
{
  (void)abs_slot;
  if (!s_dedicated_found || s_ext_verified || s_ext_n <= 0)
    return false;
  /* Independent of DCI-length convergence. Wrong geometry must not prevent trying the next one. */
  return ++s_ext_occ >= NR_PDCCH_EXTENT_VERIFY_OCC ? extent_advance() : false;
}

static int32_t blind_fill_dmrs_mask(int dmrs_TypeA_Position, int NrOfSymbols, int startSymbol,
                                    mappingType_t mappingtype, int add_pos, int length);

/* int-only wrapper: mappingType_t is not visible in this module's public header, and pulling the
 * PHY type in just for one argument would widen that header's dependencies for every consumer. */
int32_t nr_pdcch_blind_dmrs_mask(int dmrs_TypeA_Position, int NrOfSymbols, int startSymbol,
                                 int mapping_type_is_b, int add_pos, int length)
{
  return blind_fill_dmrs_mask(dmrs_TypeA_Position, NrOfSymbols, startSymbol,
                              mapping_type_is_b ? typeB : typeA, add_pos, length);
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
// "decode:mcs_table:xoverhead:rv0_only:max_per_slot[:thread[:queue_depth[:thread_core]]]".
// The three deferred-decode fields are appended rather than given their own config line so an
// existing conf keeps working unchanged and reads as the in-line default -- see
// nr_pdcch_blind_monitor_rt.h's pdsch_thread comment for why the default must stay 0.
static int parse_pdsch(const char* s)
{
  g_cfg.pdsch_thread      = 0;
  g_cfg.pdsch_queue_depth = 0;
  g_cfg.pdsch_thread_core = -1;
  const int n = sscanf(s, "%d:%d:%d:%d:%d:%d:%d:%d", &g_cfg.pdsch_decode, &g_cfg.pdsch_mcs_table,
                       &g_cfg.pdsch_xoverhead, &g_cfg.pdsch_rv0_only, &g_cfg.pdsch_max_per_slot,
                       &g_cfg.pdsch_thread, &g_cfg.pdsch_queue_depth, &g_cfg.pdsch_thread_core);
  return n >= 1;
}

// "n_consumers[:queue_depth[:core]]" -- defer the blind-PDCCH SCAN off the PHY receive thread.
// Its own config line rather than more fields appended to pdcch_blind_monitor_pdsch, because it
// defers a DIFFERENT stage (the scan, not the decode of what the scan found) and the two are
// independently useful: either, neither, or both. Absent/0 = in-line, the previous behaviour.
/* Its own config line rather than more fields on pdcch_blind_monitor_ul_pusch, for the same reason
 * parse_scan_thread gives: it defers a stage, which is independent of whether that stage runs at
 * all. Absent/0 = in-line, the previous behaviour. */
// "max_trials[:beta_idx[:alpha_idx]]" -- UCI-on-PUSCH reservation search. Absent/0 = off.
static int parse_ul_uci(const char* s)
{
  g_cfg.ul_uci_beta  = 11;  // ~20, a mid-table default; override from the gNB's own betaOffsets
  g_cfg.ul_uci_alpha = 0;   // 0.5, what this deployment logs
  /* 32: a failed grant costs at most a couple of cached attempts, and only one failure in 32 pays
   * for the wide sweep. The footprint is a property of the UE's report config, so once learned it
   * serves every later grant -- that is what makes online recovery affordable at all. */
  g_cfg.ul_uci_explore_every = 32;
  const int n = sscanf(s, "%d:%d:%d:%d", &g_cfg.ul_uci_search, &g_cfg.ul_uci_beta,
                       &g_cfg.ul_uci_alpha, &g_cfg.ul_uci_explore_every);
  if (n < 1 || g_cfg.ul_uci_search < 0 || g_cfg.ul_uci_explore_every < 0) {
    g_cfg.ul_uci_search = 0;
    return 0;
  }
  return 1;
}

static int parse_ul_thread(const char* s)
{
  g_cfg.ul_queue_depth = 0;
  g_cfg.ul_thread_core = -1;
  const int n = sscanf(s, "%d:%d:%d", &g_cfg.ul_thread, &g_cfg.ul_queue_depth, &g_cfg.ul_thread_core);
  if (n < 1 || g_cfg.ul_thread < 0) {
    g_cfg.ul_thread = 0;
    return 0;
  }
  return 1;
}

static int parse_scan_thread(const char* s)
{
  g_cfg.scan_queue_depth = 0;
  g_cfg.scan_thread_core = -1;
  const int n = sscanf(s, "%d:%d:%d", &g_cfg.scan_thread, &g_cfg.scan_queue_depth, &g_cfg.scan_thread_core);
  return n >= 1;
}

// "scan:ss_type:n_rb_riv:rb_offset:length_override:class_mask:mux_pattern:sib1" -- everything after
// `scan` is optional and defaults to the auto/spec value, so "1" alone is a valid line meaning
// "also scan format 1_0 in this (UE-specific) search space, sizing everything from the configured
// BWP". See the field comments in nr_pdcch_blind_monitor_rt.h.
static int parse_dci10(const char* s)
{
  g_cfg.dci10_ss_type        = 0;
  g_cfg.dci10_n_rb_riv       = 0;
  g_cfg.dci10_rb_offset      = -1;
  g_cfg.dci10_length_override = 0;
  g_cfg.dci10_class_mask     = 0;
  g_cfg.dci10_mux_pattern    = 0;
  g_cfg.dci10_sib1           = 0;
  const int n = sscanf(s, "%d:%d:%d:%d:%d:%d:%d:%d", &g_cfg.dci10_scan, &g_cfg.dci10_ss_type,
                       &g_cfg.dci10_n_rb_riv, &g_cfg.dci10_rb_offset, &g_cfg.dci10_length_override,
                       &g_cfg.dci10_class_mask, &g_cfg.dci10_mux_pattern, &g_cfg.dci10_sib1);
  if (n < 1) {
    return 0;
  }
  if (g_cfg.dci10_scan < 0 || g_cfg.dci10_scan > 2 || g_cfg.dci10_ss_type < 0 || g_cfg.dci10_ss_type > 1
      || g_cfg.dci10_mux_pattern < 0 || g_cfg.dci10_mux_pattern > 3) {
    return 0;
  }
  return 1;
}

// ---------------------------------------------------------------------------------------------
// UPLINK config (DCI format 0_1). Split across several keys rather than one very long one, matching
// the DL style: each key is independently parseable and independently wrong, so a malformed one
// disables only its own group.
// ---------------------------------------------------------------------------------------------

// "scan[:length_override]" -- scan 0 = off (default), 1 = on. length_override is the live-verified
// payload width; 0 = derive from the field widths. SET IT (see the header's UL section).
static int parse_dci01(const char* s)
{
  g_cfg.dci01_length_override = 0;
  const int n = sscanf(s, "%d:%d", &g_cfg.dci01_scan, &g_cfg.dci01_length_override);
  if (n < 1 || g_cfg.dci01_scan < 0 || g_cfg.dci01_scan > 1) {
    return 0;
  }
  return 1;
}

// "bwp_start:bwp_size" -- the UL BWP. bwp_size is also the RIV reference for resource allocation
// type 1, which is the only type this deployment uses.
static int parse_ul_bwp(const char* s)
{
  int start = 0, size = 0;
  if (sscanf(s, "%d:%d", &start, &size) != 2 || start < 0 || size < 1 || size > 275) {
    return 0;
  }
  g_cfg.ul.bwp_start = (uint16_t)start;
  g_cfg.ul.bwp_size  = (uint16_t)size;
  return 1;
}

// "S:L:k2[:map],..." -- the pusch-TimeDomainAllocationList. NOTE the extra k2 field compared with
// the DL tda key: k2 is what makes an UL grant actionable at all (the PUSCH is k2 slots after the
// DCI) and it exists nowhere in the payload, so it has to come from here.
static int parse_ul_tda(const char* s)
{
  int n = 0;
  const char* p = s;
  while (*p != '\0' && n < 16) {
    int start = 0, len = 0, k2 = 0, map = 0;
    const int got = sscanf(p, "%d:%d:%d:%d", &start, &len, &k2, &map);
    if (got < 3) {
      return 0;
    }
    if (start < 0 || start > 13 || len < 1 || start + len > 14 || map < 0 || map > 1 || k2 < 0 || k2 > 32) {
      return 0;
    }
    g_cfg.ul.tda_start[n]   = (uint8_t)start;
    g_cfg.ul.tda_length[n]  = (uint8_t)len;
    g_cfg.ul.tda_k2[n]      = (uint8_t)k2;
    g_cfg.ul.tda_mapping[n] = (uint8_t)map;
    n++;
    const char* comma = strchr(p, ',');
    if (comma == NULL) {
      break;
    }
    p = comma + 1;
  }
  g_cfg.ul.tda_count = n;
  return n > 0;
}

// "config_type:add_pos:max_length" -- UL DM-RS. config_type 0 = type1, 1 = type2.
static int parse_ul_dmrs(const char* s)
{
  return sscanf(s, "%d:%d:%d", &g_cfg.ul.dmrs_config_type, &g_cfg.ul.dmrs_add_pos,
                &g_cfg.ul.dmrs_max_length) == 3;
}

// "transform_precoding:mcs_table:data_scrambling_id:ul_dmrs_scrambling_id:phy_cell_id"
// The two scrambling ids take <0 to mean "use the PCI", which is the spec default and what this
// deployment does (the gNB dumps pusch_dmrs_scrambling_id=2 = nid_pusch = PCI).
static int parse_ul_misc(const char* s)
{
  int pci = 0;
  const int n = sscanf(s, "%d:%d:%d:%d:%d", &g_cfg.ul.transform_precoding, &g_cfg.ul.mcs_table,
                       &g_cfg.ul.data_scrambling_id, &g_cfg.ul.ul_dmrs_scrambling_id, &pci);
  if (n < 5 || pci < 0 || pci > 1007) {
    return 0;
  }
  g_cfg.ul.phy_cell_id = (uint16_t)pci;
  return 1;
}

// "decode[:max_per_slot[:ta_offset_samples]]" -- passive PUSCH receive.
// decode: 0 = off, 1 = channel-estimate + CFR + LDPC decode, 2 = CHANNEL-ESTIMATE AND CFR ONLY.
// Mode 2 exists because the two halves have very different value per microsecond. UTIM measured
// the LDPC decode at 403 us and the full nr_rx_pusch_group_tp at 319 us of a 1065 us grant, while
// the DM-RS CFR -- the only part the sensing pipeline consumes -- costs 29 us and does not depend
// on the transport block at all. With the UL CRC at 0 % that is ~700 us per grant spent on an
// output nothing reads.
static int parse_ul_pusch(const char* s)
{
  g_cfg.ul_pusch_max_per_slot = 0;
  g_cfg.ul_ta_offset_samples  = 0;
  const int n = sscanf(s, "%d:%d:%d", &g_cfg.ul_pusch_decode, &g_cfg.ul_pusch_max_per_slot,
                       &g_cfg.ul_ta_offset_samples);
  if (n < 1 || g_cfg.ul_pusch_decode < 0 || g_cfg.ul_pusch_decode > 2) {
    return 0;
  }
  return 1;
}

// 16 ints, in fill_dci_pdu_rel15()'s NR_UL_DCI_FORMAT_0_1 PACKER order; -1 = the documented default.
// time_domain_assignment has NO entry, for the same reason it has none on the DL side: it is
// DERIVED from the TDRA list count rather than being a second independently-wrong knob.
static int parse_ul_dci_bits(const char* s)
{
  return sscanf(s, "%d:%d:%d:%d:%d:%d:%d:%d:%d:%d:%d:%d:%d:%d:%d:%d",
                &g_cfg.ul.carrier_indicator_bits, &g_cfg.ul.ul_sul_bits, &g_cfg.ul.bwp_indicator_bits,
                &g_cfg.ul.freq_hopping_bits, &g_cfg.ul.harq_pid_bits, &g_cfg.ul.dai1_bits,
                &g_cfg.ul.dai2_bits, &g_cfg.ul.sri_bits, &g_cfg.ul.precoding_info_bits,
                &g_cfg.ul.antenna_ports_bits, &g_cfg.ul.srs_request_bits, &g_cfg.ul.csi_request_bits,
                &g_cfg.ul.cbg_bits, &g_cfg.ul.ptrs_dmrs_bits, &g_cfg.ul.beta_offset_bits,
                &g_cfg.ul.dmrs_seq_init_bits) == 16;
}

// Same syntax as pdcch_blind_monitor_tda, for the pdsch-ConfigCommon list format 1_0 uses under
// every RNTI class except C-RNTI-in-a-UE-specific-search-space (TS 38.214 Table 5.1.2.1.1-1).
static int parse_tda_common(const char* s)
{
  int         n = 0;
  const char* p = s;
  while (*p != '\0' && n < 16) {
    int       start = 0, len = 0, map = 0;
    const int got = sscanf(p, "%d:%d:%d", &start, &len, &map);
    if (got < 2) {
      return 0;
    }
    if (start < 0 || start > 13 || len < 1 || start + len > 14 || map < 0 || map > 1) {
      return 0;
    }
    g_cfg.extract.tda_common_start[n]   = (uint8_t)start;
    g_cfg.extract.tda_common_length[n]  = (uint8_t)len;
    g_cfg.extract.tda_common_mapping[n] = (uint8_t)map;
    n++;
    const char* comma = strchr(p, ',');
    if (comma == NULL) {
      break;
    }
    p = comma + 1;
  }
  g_cfg.extract.tda_common_count = n;
  return n > 0;
}

/* Format-1_0 startup reconciliation. Same purpose as the 1_1 field-width check above -- say out
 * loud what the configuration implies, so a wrong value shows up as a line at start-up rather than
 * as a silently empty accept census hours later. Unlike 1_1's, the 1_0 width is a pure spec formula,
 * so the only thing that can be wrong here is n_rb_riv (or an override that disagrees with it). */
static void nr_pdcch_blind_log_dci10_config(void)
{
  if (g_cfg.dci10_scan == 0) {
    return;
  }
  /* In a COMMON search space n_rb_riv defaults to the CORESET's own measured RB count, which is not
   * known until the RT tap reads the frequency-domain bitmap -- so there is nothing honest to print
   * here. Say so rather than printing a 0 that reads as a misconfiguration; the RT path logs the
   * resolved value once ("blind PDCCH formats: ..."). */
  const bool     n_rb_deferred = (g_cfg.dci10_ss_type == NR_BLIND_SS_COMMON) && (g_cfg.dci10_n_rb_riv <= 0);
  const int      n_rb_auto = (g_cfg.dci10_ss_type == NR_BLIND_SS_COMMON) ? 0 : g_cfg.bwp_size;
  const uint16_t n_rb      = (g_cfg.dci10_n_rb_riv > 0) ? (uint16_t)g_cfg.dci10_n_rb_riv : (uint16_t)n_rb_auto;
  const uint16_t formula   = nr_pdcch_blind_dci10_size(n_rb);
  if (n_rb_deferred) {
    LOG_I(PHY,
          "SENSING: blind PDCCH DCI 1_0 scanning %s, ss=common; n_rb_riv/rb_offset/dci_length are taken "
          "from the CORESET at run time (see the \"blind PDCCH formats\" line) -- class_mask=0x%x "
          "mux_pattern=%u sib1=%d tda_common_entries=%d\n",
          (g_cfg.dci10_scan == 2) ? "EXCLUSIVE (format 1_1 not scanned)" : "alongside format 1_1",
          (unsigned)g_cfg.dci10_class_mask, (unsigned)g_cfg.dci10_mux_pattern, g_cfg.dci10_sib1,
          g_cfg.extract.tda_common_count);
    return;
  }
  LOG_I(PHY,
        "SENSING: blind PDCCH DCI 1_0 scanning %s, ss=%s n_rb_riv=%s%u rb_offset=%s%d dci_length=%u "
        "class_mask=0x%x mux_pattern=%u sib1=%d tda_common_entries=%d\n",
        (g_cfg.dci10_scan == 2) ? "EXCLUSIVE (format 1_1 not scanned)" : "alongside format 1_1",
        (g_cfg.dci10_ss_type == NR_BLIND_SS_COMMON) ? "common" : "ue-specific",
        (g_cfg.dci10_n_rb_riv > 0) ? "" : "auto:", (unsigned)n_rb,
        (g_cfg.dci10_rb_offset >= 0) ? "" : "auto:", g_cfg.dci10_rb_offset,
        (unsigned)(g_cfg.dci10_length_override > 0 ? (uint16_t)g_cfg.dci10_length_override : formula),
        (unsigned)g_cfg.dci10_class_mask, (unsigned)g_cfg.dci10_mux_pattern, g_cfg.dci10_sib1,
        g_cfg.extract.tda_common_count);
  if (g_cfg.dci10_length_override > 0 && formula > 0 && (uint16_t)g_cfg.dci10_length_override != formula) {
    /* TS 38.212 7.3.1.0 only ever pads format 1_0 UPWARDS (to DCI 0_0's size, in a UE-specific
     * search space). An override BELOW the formula cannot be a legitimate size and means the two
     * disagree about n_rb_riv -- in which case every field is read from the wrong offset. */
    if ((uint16_t)g_cfg.dci10_length_override < formula) {
      LOG_E(PHY,
            "SENSING: pdcch_blind_monitor_dci10 length override %d is SMALLER than the %u bits "
            "n_rb_riv=%u implies -- format 1_0 is only ever zero-padded upwards, so one of the two is "
            "wrong and every field will be read from the wrong bit offset\n",
            g_cfg.dci10_length_override, (unsigned)formula, (unsigned)n_rb);
    } else {
      LOG_I(PHY,
            "SENSING: DCI 1_0 payload padded %u -> %d bits (TS 38.212 7.3.1.0 size alignment against "
            "DCI 0_0 in a UE-specific search space); the padding is checked for zero on every decode\n",
            (unsigned)formula, g_cfg.dci10_length_override);
    }
  }
  if (g_cfg.dci10_ss_type == NR_BLIND_SS_COMMON && g_cfg.coreset_type != 1) {
    LOG_W(PHY,
          "SENSING: DCI 1_0 configured for a COMMON search space but pdcch_blind_monitor_coreset's "
          "type is %d (PDCCH-Config), not 1 (CORESET0/MIB-SIB1). SI-/RA-/TC-RNTI DCIs live in the "
          "CORESET#0 common search spaces, and the DM-RS reference point differs between the two "
          "CORESET types -- a mismatch here can never decode\n",
          g_cfg.coreset_type);
  }
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
  /* NOT covered by the memset: 0 is a VALID core id here, and core 0 is one of this deployment's
   * --thread-pool cores, so a zeroed default would silently pin the deferred-decode consumer onto
   * the RT candidate-decode pool -- the one placement the feature exists to avoid. -1 = unpinned. */
  g_cfg.pdsch_thread_core = -1;
  // Noise-floor gates default: the RNTI range above is, by necessity, nearly the whole space (see
  // its own comment) and on its own cannot keep the false-accept rate down at this scan's trial
  // volume -- see nr_pdcch_blind_monitor_rt.h's field comments for what each number means.
  // energy_min defaults OFF: it is an ABSOLUTE threshold in receiver-dependent units, so shipping a
  // default for it was always wrong -- 2.0 was calibrated at 106 PRB and does not carry to 273 PRB
  // or to any real OTA gain setting, which is why it ended up disabled in the configs rather than
  // retuned. energy_adapt_factor ALSO now defaults OFF (2026-09-04): measured live on this cell's
  // CORESET#0 to saturate its own floor estimator and reject 100% of candidates within seconds
  // (the estimator is fed by every candidate it gates, so a dense/small CORESET converges the
  // "noise" floor to signal level -- see PHASE1_CSS0_AUTOCONF_HANDOVER.md). A large dedicated
  // CORESET was the case this gate was designed for and where it stays safe, but a default this
  // deployment-shape-dependent is not a safe global default; a deployment that wants it back on
  // sets pdcch_blind_monitor_noise_gates explicitly (the 5th field, energy_adapt_factor).
  g_cfg.energy_min             = 0.0f;
  g_cfg.energy_adapt_factor    = 0.0f;
  g_cfg.rnti_persist_k         = 2;
  g_cfg.rnti_persist_window_ms = 500;
  g_cfg.min_snr_lin            = 4.0f; // ~6 dB
  // Spec defaults for the deployment-fact overrides: tda_count=0 keeps the default TDRA table and
  // dmrs_add_pos<0 keeps fill_dmrs_mask()'s pos2 fallback -- i.e. exactly the pre-2026-07-30
  // behaviour unless the corresponding config lines are present.
  g_cfg.extract.tda_count      = 0;
  g_cfg.extract.tda_common_count = 0;
  // Format 1_0 scanning off => the module behaves exactly as before.
  g_cfg.dci10_scan             = 0;
  g_cfg.dci10_rb_offset        = -1;
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
  g_cfg.scan_thread            = 0;  // in-line on the PHY receive thread, as before
  g_cfg.scan_queue_depth       = 0;
  g_cfg.scan_thread_core       = -1;
  g_cfg.ul_uci_search          = 0;  // off = previous behaviour
  g_cfg.ul_uci_beta            = 11;
  g_cfg.ul_uci_alpha           = 0;
  g_cfg.ul_thread              = 0;  // in-line on the PHY receive thread, as before
  g_cfg.ul_queue_depth         = 0;
  g_cfg.ul_thread_core         = -1;
  /* UL defaults: every width at -1 = the documented assumption, both identities at -1 = fall back
   * to the PCI, scan off. With dci01_scan == 0 none of this is read, so an unconfigured deployment
   * is bit-identical to before. */
  g_cfg.dci01_scan               = 0;
  g_cfg.dci01_length_override    = 0;
  g_cfg.ul.dmrs_add_pos          = -1;
  g_cfg.ul.dmrs_max_length       = 0;
  g_cfg.ul.data_scrambling_id    = -1;
  g_cfg.ul.ul_dmrs_scrambling_id = -1;
  g_cfg.ul.carrier_indicator_bits = -1;
  g_cfg.ul.ul_sul_bits            = -1;
  g_cfg.ul.bwp_indicator_bits     = -1;
  g_cfg.ul.freq_hopping_bits      = -1;
  g_cfg.ul.harq_pid_bits          = -1;
  g_cfg.ul.dai1_bits              = -1;
  g_cfg.ul.dai2_bits              = -1;
  g_cfg.ul.sri_bits               = -1;
  g_cfg.ul.precoding_info_bits    = -1;
  g_cfg.ul.antenna_ports_bits     = -1;
  g_cfg.ul.srs_request_bits       = -1;
  g_cfg.ul.csi_request_bits       = -1;
  g_cfg.ul.cbg_bits               = -1;
  g_cfg.ul.ptrs_dmrs_bits         = -1;
  g_cfg.ul.beta_offset_bits       = -1;
  g_cfg.ul.dmrs_seq_init_bits     = -1;

  char*     p_coreset = NULL;
  char*     p_ss       = NULL;
  char*     p_bwp       = NULL;
  char*     p_rnti_range = NULL;
  char*     p_noise_gates = NULL;
  char*     p_scan_thread = NULL;
  char*     p_ul_thread   = NULL;
  char*     p_ul_uci      = NULL;
  char*     p_tda        = NULL;
  char*     p_dmrs       = NULL;
  char*     p_pdsch      = NULL;
  char*     p_dci_bits   = NULL;
  char*     p_dci10      = NULL;
  char*     p_tda_common = NULL;
  char*     p_dci01      = NULL;
  char*     p_ul_bwp     = NULL;
  char*     p_ul_tda     = NULL;
  char*     p_ul_dmrs    = NULL;
  char*     p_ul_misc    = NULL;
  char*     p_ul_dci_bits = NULL;
  char*     p_ul_pusch   = NULL;
  paramdef_t params[] = {
      {"pdcch_blind_monitor_coreset",
        "Dedicated CORESET geometry for blind PDCCH monitoring; "
        "num_freq_groups:duration:reg_bundle_size:interleaver_size:shift_index:pdcch_dmrs_scrambling_id "
        "(num_freq_groups = contiguous 6-PRB groups from group 0, e.g. 16 for a 96-PRB CORESET)",
        0, .strptr = &p_coreset, .defstrval = "", TYPE_STRING, 0},
      {"pdcch_blind_monitor_ss",
        "Dedicated SearchSpace geometry; "
        "monitoring_slot_periodicity:monitoring_slot_offset:duration:first_symbol:"
        "al1_cand:al2_cand:al4_cand:al8_cand (0 = adaptive, >0 = pin, <0 = disable)",
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
      {"pdcch_blind_monitor_autoconf",
        "1 = derive the COMMON search space (CORESET#0/SearchSpace#0, initial BWP, DCI 1_0 size) "
        "from MIB/SIB1 at runtime, so the receiver works on a cell whose dedicated configuration is "
        "unknown. Overrides pdcch_blind_monitor_coreset/_ss/_bwp once SIB1 decodes. Default 0 keeps "
        "any hand-written (dedicated) config, which is denser but cell-specific",
        0, .iptr = &g_cfg.autoconf, .defintval = 0, TYPE_INT, 0},
      {"pdcch_blind_monitor_autodiscover",
        "Phase 3: recover the DEDICATED CORESET/search space by DM-RS correlation + dci_length "
        "sweep instead of reading pdcch_blind_monitor_coreset/_ss/_bwp. Default 0. Requires "
        "pdcch_blind_monitor_autoconf=1 (Phase 1) to have a working common search space first -- "
        "the bootstrap RNTI comes from THAT path's own grants.",
        0, .iptr = &g_cfg.autodiscover, .defintval = 0, TYPE_INT, 0},
      {"pdcch_blind_monitor_full_auto",
        "1 = also auto-discover PAYLOAD INTERPRETATION (Technique D: TDA/DM-RS-position/MCS-table, "
        "TB-CRC-scored) instead of using pdcch_blind_monitor_tda/_dmrs/_pdsch's hand-written values. "
        "Default 0 -- the DEFAULT behaviour is already 'manual conf, auto-extracted gNB values': "
        "CORESET geometry, dci_length and live RNTIs are self-discovered whenever autodiscover=1 "
        "regardless of this flag, only the payload FIELD LAYOUT stays human-supplied until this is "
        "set. For UL, 0 keeps the manual DCI length/widths/TDA/DM-RS settings; 1 enables the "
        "experimental independent UL length, field-width and interpretation searches. Unresolved "
        "or oversized UL searches refuse grants, never silently fall back to manual settings.",
        0, .iptr = &g_cfg.dl_full_auto, .defintval = 0, TYPE_INT, 0},
      {"pdcch_blind_monitor_dci10",
        "DCI format 1_0 scanning; scan[:ss_type[:n_rb_riv[:rb_offset[:length_override[:class_mask"
        "[:mux_pattern[:sib1]]]]]]] (scan 0=format 1_1 only, 1=both, 2=1_0 only; ss_type 0=UE-specific, "
        "1=common). Needed for SIB1/RAR/Msg4-RRCSetup and C-RNTI fallback grants, none of which are "
        "carried on format 1_1",
        0, .strptr = &p_dci10, .defstrval = "", TYPE_STRING, 0},
      {"pdcch_blind_monitor_tda_common",
        "The gNB pdsch-ConfigCommon pdsch-TimeDomainAllocationList (same S:L[:mapping],... syntax as "
        "pdcch_blind_monitor_tda). TS 38.214 Table 5.1.2.1.1-1 uses this list -- NOT the dedicated one "
        "-- for every DCI 1_0 except C-RNTI in a UE-specific search space. Omit to reuse "
        "pdcch_blind_monitor_tda, which is correct when the gNB derives both from the same "
        "pdsch-ConfigCommon",
        0, .strptr = &p_tda_common, .defstrval = "", TYPE_STRING, 0},
      {"pdcch_blind_monitor_dci01",
        "UPLINK DCI format 0_1 scanning; scan[:length_override] (scan 0=off default, 1=on). "
        "length_override is the live-verified payload width -- SET IT: 0_1's per-field widths are "
        "RRC-derived and this deployment's UL RRC config is not readable off the air",
        0, .strptr = &p_dci01, .defstrval = "", TYPE_STRING, 0},
      {"pdcch_blind_monitor_ul_bwp", "UL BWP for DCI 0_1 sizing; bwp_start:bwp_size", 0,
        .strptr = &p_ul_bwp, .defstrval = "", TYPE_STRING, 0},
      {"pdcch_blind_monitor_ul_tda",
        "pusch-TimeDomainAllocationList; S:L:k2[:mapping_type],... (note the k2 field -- the PUSCH "
        "is k2 slots after its DCI and k2 appears nowhere in the payload)",
        0, .strptr = &p_ul_tda, .defstrval = "", TYPE_STRING, 0},
      {"pdcch_blind_monitor_ul_dmrs", "UL DM-RS; dmrs_config_type:add_pos:max_length", 0,
        .strptr = &p_ul_dmrs, .defstrval = "", TYPE_STRING, 0},
      {"pdcch_blind_monitor_ul_misc",
        "transform_precoding:mcs_table:data_scrambling_id:ul_dmrs_scrambling_id:phy_cell_id "
        "(scrambling ids <0 = use the PCI)",
        0, .strptr = &p_ul_misc, .defstrval = "", TYPE_STRING, 0},
      {"pdcch_blind_monitor_ul_dci_bits",
        "DCI 0_1 per-field widths in packer order, -1 = default; "
        "carrier:ulsul:bwp:hopping:harq:dai1:dai2:sri:precoding:antports:srsreq:csireq:cbg:ptrs:"
        "beta:dmrsseq",
        0, .strptr = &p_ul_dci_bits, .defstrval = "", TYPE_STRING, 0},
      {"pdcch_blind_monitor_ul_pusch",
        "Passive PUSCH receive; decode[:max_per_slot[:ta_offset_samples]] (decode 0=off default, "
        "1=on; ta_offset 0 = derive N_TA_offset from the sample rate -- the per-UE N_TA is in no DCI)",
        0, .strptr = &p_ul_pusch, .defstrval = "", TYPE_STRING, 0},
      {"pdcch_blind_monitor_ul_uci",
        "UCI-on-PUSCH reservation search: max_trials[:beta_idx[:alpha_idx]]. 0/absent = off. O_ACK is "
        "not in the UL DCI (the DAI pins it only mod 4), so candidates are tried and the TB CRC decides",
        0, .strptr = &p_ul_uci, .defstrval = "", TYPE_STRING, 0},
      {"pdcch_blind_monitor_ul_thread",
        "Defer the passive PUSCH decode off the PHY receive thread; n_consumers[:queue_depth[:core]]. "
        "0/absent = in-line. Measured: the UL decode is 1065us per grant against a 500us slot, "
        "over_slot 99.5 %",
        0, .strptr = &p_ul_thread, .defstrval = "", TYPE_STRING, 0},
      {"pdcch_blind_monitor_scan_thread",
        "Defer the blind-PDCCH scan (FEP/LLR/demap/candidate decode) off the PHY receive thread; "
        "n_consumers[:queue_depth[:core]]. 0/absent = in-line. Measured: the scan is 69-102us per "
        "occasion = 14-20 % of a 500us slot, straddling the ~18 % at which PBCH lock is lost",
        0, .strptr = &p_scan_thread, .defstrval = "", TYPE_STRING, 0},
      {"pdcch_blind_monitor_pdsch",
        "Passive data-aided PDSCH; decode:mcs_table:xoverhead:rv0_only:max_per_slot "
        "(decode 0=off, 1=decode+count CRC pass rate only, 2=also submit the reconstructed CFR)",
        0, .strptr = &p_pdsch, .defstrval = "", TYPE_STRING, 0},
  };
  config_get(config_get_if(), params, (int)(sizeof(params) / sizeof(params[0])), "sensing");

  /* The three lines below describe a DEDICATED search space, which a passive receiver can never
   * read off the air (it arrives in RRCReconfiguration over a ciphered SRB). Requiring them made
   * "self-configure from the MIB" unreachable: delete them and init() returned here, leaving
   * g_enabled = 0, which gates the entire RT tap (nr_pdcch_blind_monitor_rt.c). Autoconf would then
   * populate a config nothing ever read -- and it logged its success line while doing so.
   * g_cfg.autoconf is already set: config_get() above filled it via .iptr. */
  const bool have_manual_coreset = (p_coreset != NULL && p_coreset[0] != '\0'
                                    && p_ss != NULL && p_ss[0] != '\0'
                                    && p_bwp != NULL && p_bwp[0] != '\0');
  if (!have_manual_coreset) {
    // Only some of the three lines present -- e.g. a half-finished migration to/from autoconf.
    // Silently falling back (to autoconf if it's on, disabled otherwise) would discard an
    // operator's stray config with no trace. Warn regardless of g_cfg.autoconf: the silent-discard
    // risk exists whichever way this falls.
    const bool any_manual = (p_coreset != NULL && p_coreset[0] != '\0')
                          || (p_ss != NULL && p_ss[0] != '\0')
                          || (p_bwp != NULL && p_bwp[0] != '\0');
    if (any_manual) {
      LOG_W(PHY,
            "SENSING: pdcch_blind_monitor_coreset/_ss/_bwp are only PARTIALLY set "
            "(coreset='%s' ss='%s' bwp='%s') -- all three describe one dedicated search space and "
            "are only ever used together. The incomplete set is being IGNORED. Supply all three, "
            "or none\n",
            p_coreset ? p_coreset : "", p_ss ? p_ss : "", p_bwp ? p_bwp : "");
    }
  }
  if (!have_manual_coreset && !g_cfg.autoconf) {
    return; // monitor disabled -- either the three lines together, or autoconf
  }
  if (have_manual_coreset) {
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
  }
  if (p_rnti_range != NULL && p_rnti_range[0] != '\0' && !parse_rnti_range(p_rnti_range)) {
    LOG_E(PHY, "SENSING: malformed pdcch_blind_monitor_rnti_range '%s'; using default %u-%u\n", p_rnti_range,
          NR_PDCCH_BLIND_RNTI_MIN_DEFAULT, NR_PDCCH_BLIND_RNTI_MAX_DEFAULT);
  }
  if (p_ul_uci != NULL && p_ul_uci[0] != '\0' && !parse_ul_uci(p_ul_uci)) {
    LOG_E(PHY, "SENSING: malformed pdcch_blind_monitor_ul_uci '%s'; UCI reservation search off\n",
          p_ul_uci);
  }
  if (p_ul_thread != NULL && p_ul_thread[0] != '\0' && !parse_ul_thread(p_ul_thread)) {
    LOG_E(PHY, "SENSING: malformed pdcch_blind_monitor_ul_thread '%s'; UL decode stays in-line\n",
          p_ul_thread);
  }
  if (p_scan_thread != NULL && p_scan_thread[0] != '\0' && !parse_scan_thread(p_scan_thread)) {
    LOG_E(PHY, "SENSING: malformed pdcch_blind_monitor_scan_thread '%s'; scan stays in-line\n", p_scan_thread);
    g_cfg.scan_thread = 0;
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
  if (p_dci10 != NULL && p_dci10[0] != '\0' && !parse_dci10(p_dci10)) {
    g_cfg.dci10_scan = 0;
    LOG_E(PHY, "SENSING: malformed pdcch_blind_monitor_dci10 '%s'; format 1_0 scanning disabled\n", p_dci10);
  }
  if (p_dci01 != NULL && p_dci01[0] != '\0' && !parse_dci01(p_dci01)) {
    g_cfg.dci01_scan = 0;
    LOG_E(PHY, "SENSING: malformed pdcch_blind_monitor_dci01 '%s'; UL DCI 0_1 scanning disabled\n", p_dci01);
  }
  if (p_ul_bwp != NULL && p_ul_bwp[0] != '\0' && !parse_ul_bwp(p_ul_bwp)) {
    g_cfg.dci01_scan = 0;
    LOG_E(PHY, "SENSING: malformed pdcch_blind_monitor_ul_bwp '%s'; UL DCI 0_1 scanning disabled\n", p_ul_bwp);
  }
  if (p_ul_tda != NULL && p_ul_tda[0] != '\0' && !parse_ul_tda(p_ul_tda)) {
    g_cfg.dci01_scan = 0;
    LOG_E(PHY, "SENSING: malformed pdcch_blind_monitor_ul_tda '%s'; UL DCI 0_1 scanning disabled\n", p_ul_tda);
  }
  if (p_ul_dmrs != NULL && p_ul_dmrs[0] != '\0' && !parse_ul_dmrs(p_ul_dmrs)) {
    LOG_E(PHY, "SENSING: malformed pdcch_blind_monitor_ul_dmrs '%s'; UL DM-RS left at defaults\n", p_ul_dmrs);
  }
  if (p_ul_misc != NULL && p_ul_misc[0] != '\0' && !parse_ul_misc(p_ul_misc)) {
    LOG_E(PHY, "SENSING: malformed pdcch_blind_monitor_ul_misc '%s'; UL identities left at defaults\n", p_ul_misc);
  }
  if (p_ul_pusch != NULL && p_ul_pusch[0] != '\0' && !parse_ul_pusch(p_ul_pusch)) {
    g_cfg.ul_pusch_decode = 0;
    LOG_E(PHY, "SENSING: malformed pdcch_blind_monitor_ul_pusch '%s'; passive PUSCH decode disabled\n",
          p_ul_pusch);
  }
  if (p_ul_dci_bits != NULL && p_ul_dci_bits[0] != '\0' && !parse_ul_dci_bits(p_ul_dci_bits)) {
    LOG_E(PHY, "SENSING: malformed pdcch_blind_monitor_ul_dci_bits '%s' (need 16 ints); UL widths left "
               "at defaults\n", p_ul_dci_bits);
  }
  /* An UL scan with no UL BWP cannot size a RIV and would sweep a wrong width silently -- the same
   * class of failure the 1_0 path guards with its n_rb_riv check. */
  if (g_cfg.dci01_scan && !g_cfg.dl_full_auto && g_cfg.ul.bwp_size < 1) {
    g_cfg.dci01_scan = 0;
    LOG_E(PHY, "SENSING: pdcch_blind_monitor_dci01 is on but pdcch_blind_monitor_ul_bwp is unset; "
               "UL DCI 0_1 scanning disabled\n");
  }
  if (p_tda_common != NULL && p_tda_common[0] != '\0' && !parse_tda_common(p_tda_common)) {
    g_cfg.extract.tda_common_count = 0;
    LOG_E(PHY,
          "SENSING: malformed pdcch_blind_monitor_tda_common '%s'; DCI 1_0 will reuse the dedicated "
          "pdcch_blind_monitor_tda list\n",
          p_tda_common);
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
  if (have_manual_coreset) {
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
  } else {
    LOG_I(PHY, "SENSING: blind PDCCH config deferred to CSS0 autoconf; DCI-1_1 width reconciliation "
               "not applicable (autoconf scans DCI format 1_0 only)\n");
  }

  if (g_cfg.pdsch_decode > 0 && g_cfg.extract.tda_count == 0) {
    LOG_W(PHY,
          "SENSING: passive PDSCH decode enabled with NO pdcch_blind_monitor_tda -- the 3GPP default "
          "TDRA table will be assumed, which is wrong for any gNB carrying its own "
          "pdsch-TimeDomainAllocationList; expect a ~0%% CRC pass rate that says nothing about the channel\n");
  }

  g_enabled = 1;
  nr_pdcch_blind_log_dci10_config();
  if (have_manual_coreset) {
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
  } else {
    // Under deferred/autoconf config, g_cfg's coreset/ss/bwp fields are not populated yet at this
    // point (autoconf fills them later, once the MIB decodes) -- printing them here would claim
    // configured values that don't exist, the same "looks broken while healthy" trap the DCI-width
    // reconciliation block above already guards against.
    LOG_I(PHY, "SENSING: blind PDCCH monitor: autoconf mode, will configure once MIB is decoded\n");
  }
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
// Table 7.3.1.2.2-2: DM-RS type 1, maxLength 2 -- the 5-bit antenna-ports field a rank-4 cell with
// 4 DL antennas uses (OTA 2026-09-15). Columns: {cdm_groups, port0..port7, DM-RS symbols (1 or 2)}.
// Rows 0-11 are Table -1 with one front-loaded symbol; rows 12-30 use the double symbol.
static const uint8_t g_table_7_3_2_3_3_2[31][10] = {
    {1,1,0,0,0,0,0,0,0,1}, {1,0,1,0,0,0,0,0,0,1}, {1,1,1,0,0,0,0,0,0,1}, {2,1,0,0,0,0,0,0,0,1},
    {2,0,1,0,0,0,0,0,0,1}, {2,0,0,1,0,0,0,0,0,1}, {2,0,0,0,1,0,0,0,0,1}, {2,1,1,0,0,0,0,0,0,1},
    {2,0,0,1,1,0,0,0,0,1}, {2,1,1,1,0,0,0,0,0,1}, {2,1,1,1,1,0,0,0,0,1}, {2,1,0,1,0,0,0,0,0,1},
    {2,1,0,0,0,0,0,0,0,2}, {2,0,1,0,0,0,0,0,0,2}, {2,0,0,1,0,0,0,0,0,2}, {2,0,0,0,1,0,0,0,0,2},
    {2,0,0,0,0,1,0,0,0,2}, {2,0,0,0,0,0,1,0,0,2}, {2,0,0,0,0,0,0,1,0,2}, {2,0,0,0,0,0,0,0,1,2},
    {2,1,1,0,0,0,0,0,0,2}, {2,0,0,1,1,0,0,0,0,2}, {2,0,0,0,0,1,1,0,0,2}, {2,0,0,0,0,0,0,1,1,2},
    {2,1,0,0,0,1,0,0,0,2}, {2,0,0,1,0,0,0,1,0,2}, {2,1,1,0,0,1,0,0,0,2}, {2,0,0,1,1,0,0,1,0,2},
    {2,1,1,0,0,1,1,0,0,2}, {2,0,0,1,1,0,0,1,1,2}, {2,1,0,1,0,1,0,1,0,2},
};

// Table 7.3.1.2.2-3: DM-RS type 2, maxLength 1 (5-bit field, 24 rows). Columns: {cdm_groups, port0..port5}.
static const uint8_t g_table_7_3_2_3_3_3[24][7] = {
    {1,1,0,0,0,0,0}, {1,0,1,0,0,0,0}, {1,1,1,0,0,0,0}, {2,1,0,0,0,0,0}, {2,0,1,0,0,0,0}, {2,0,0,1,0,0,0},
    {2,0,0,0,1,0,0}, {2,1,1,0,0,0,0}, {2,0,0,1,1,0,0}, {2,1,1,1,0,0,0}, {2,1,1,1,1,0,0}, {3,1,0,0,0,0,0},
    {3,0,1,0,0,0,0}, {3,0,0,1,0,0,0}, {3,0,0,0,1,0,0}, {3,0,0,0,0,1,0}, {3,0,0,0,0,0,1}, {3,1,1,0,0,0,0},
    {3,0,0,1,1,0,0}, {3,0,0,0,0,1,1}, {3,1,1,1,0,0,0}, {3,0,0,0,1,1,1}, {3,1,1,1,1,0,0}, {3,1,0,1,0,0,0},
};
// Table 7.3.1.2.2-4: DM-RS type 2, maxLength 2 (6-bit field, 58 rows). Columns: {cdm_groups, port0..port11, symbols}.
static const uint8_t g_table_7_3_2_3_3_4[58][14] = {
    {1,1,0,0,0,0,0,0,0,0,0,0,0,1}, {1,0,1,0,0,0,0,0,0,0,0,0,0,1}, {1,1,1,0,0,0,0,0,0,0,0,0,0,1},
    {2,1,0,0,0,0,0,0,0,0,0,0,0,1}, {2,0,1,0,0,0,0,0,0,0,0,0,0,1}, {2,0,0,1,0,0,0,0,0,0,0,0,0,1},
    {2,0,0,0,1,0,0,0,0,0,0,0,0,1}, {2,1,1,0,0,0,0,0,0,0,0,0,0,1}, {2,0,0,1,1,0,0,0,0,0,0,0,0,1},
    {2,1,1,1,0,0,0,0,0,0,0,0,0,1}, {2,1,1,1,1,0,0,0,0,0,0,0,0,1}, {3,1,0,0,0,0,0,0,0,0,0,0,0,1},
    {3,0,1,0,0,0,0,0,0,0,0,0,0,1}, {3,0,0,1,0,0,0,0,0,0,0,0,0,1}, {3,0,0,0,1,0,0,0,0,0,0,0,0,1},
    {3,0,0,0,0,1,0,0,0,0,0,0,0,1}, {3,0,0,0,0,0,1,0,0,0,0,0,0,1}, {3,1,1,0,0,0,0,0,0,0,0,0,0,1},
    {3,0,0,1,1,0,0,0,0,0,0,0,0,1}, {3,0,0,0,0,1,1,0,0,0,0,0,0,1}, {3,1,1,1,0,0,0,0,0,0,0,0,0,1},
    {3,0,0,0,1,1,1,0,0,0,0,0,0,1}, {3,1,1,1,1,0,0,0,0,0,0,0,0,1}, {2,1,0,1,0,0,0,0,0,0,0,0,0,1},
    {3,1,0,0,0,0,0,0,0,0,0,0,0,2}, {3,0,1,0,0,0,0,0,0,0,0,0,0,2}, {3,0,0,1,0,0,0,0,0,0,0,0,0,2},
    {3,0,0,0,1,0,0,0,0,0,0,0,0,2}, {3,0,0,0,0,1,0,0,0,0,0,0,0,2}, {3,0,0,0,0,0,1,0,0,0,0,0,0,2},
    {3,0,0,0,0,0,0,1,0,0,0,0,0,2}, {3,0,0,0,0,0,0,0,1,0,0,0,0,2}, {3,0,0,0,0,0,0,0,0,1,0,0,0,2},
    {3,0,0,0,0,0,0,0,0,0,1,0,0,2}, {3,0,0,0,0,0,0,0,0,0,0,1,0,2}, {3,0,0,0,0,0,0,0,0,0,0,0,1,2},
    {3,1,1,0,0,0,0,0,0,0,0,0,0,2}, {3,0,0,1,1,0,0,0,0,0,0,0,0,2}, {3,0,0,0,0,1,1,0,0,0,0,0,0,2},
    {3,0,0,0,0,0,0,1,1,0,0,0,0,2}, {3,0,0,0,0,0,0,0,0,1,1,0,0,2}, {3,0,0,0,0,0,0,0,0,0,0,1,1,2},
    {3,1,1,0,0,0,0,1,0,0,0,0,0,2}, {3,0,0,1,1,0,0,0,0,1,0,0,0,2}, {3,0,0,0,0,1,1,0,0,0,0,1,0,2},
    {3,1,1,0,0,0,0,1,1,0,0,0,0,2}, {3,0,0,1,1,0,0,0,0,1,1,0,0,2}, {3,0,0,0,0,1,1,0,0,0,0,1,1,2},
    {1,1,0,0,0,0,0,0,0,0,0,0,0,2}, {1,0,1,0,0,0,0,0,0,0,0,0,0,2}, {1,0,0,0,0,0,0,1,0,0,0,0,0,2},
    {1,0,0,0,0,0,0,0,1,0,0,0,0,2}, {1,1,1,0,0,0,0,0,0,0,0,0,0,2}, {1,0,0,0,0,0,0,1,1,0,0,0,0,2},
    {2,1,1,0,0,0,0,0,0,0,0,0,0,2}, {2,0,0,1,1,0,0,0,0,0,0,0,0,2}, {2,0,0,0,0,0,0,1,1,0,0,0,0,2},
    {2,0,0,0,0,0,0,0,0,1,1,0,0,2},
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

// ---------------------------------------------------------------------------------------------
// Steps shared by the format 1_1 and format 1_0 entry points. Factored out rather than duplicated
// because the mismatched-bits check in particular is subtle (it compares a re-encode against the
// ORIGINAL soft-LLR polarity, and the caller owns the threshold) -- two copies of it would be one
// copy too many. The SPLIT between the two helpers is deliberate and load-bearing: the re-encode is
// only run for a candidate whose RNTI already passed its range check, which at this scan's trial
// volume (~470k occasions per capture) is the difference between paying a polar encode per
// candidate and paying it per plausible one.
// ---------------------------------------------------------------------------------------------

/* Precomputed polar result for the next decode on THIS thread -- see nr_pdcch_blind_monitor.h. */
static __thread nr_pdcch_blind_polar_pre_t tls_polar_pre;
void nr_pdcch_blind_polar_pre_set(const nr_pdcch_blind_polar_pre_t *pre)
{
  if (pre != NULL)
    tls_polar_pre = *pre;
  else
    tls_polar_pre.llr = NULL;
}

/// Step 1: RNTI-independent polar decode. The CRC-recovered value IS the candidate RNTI in its low
/// 16 bits; the full 24 bits are returned because only a genuine decode has the upper 8 zero.
static uint32_t blind_polar_decode(const int16_t* llr,
                                   uint8_t        aggregation_level,
                                   uint16_t       dci_length,
                                   uint16_t       rnti_min,
                                   uint16_t       rnti_max,
                                   uint64_t       dci_estimation[2])
{
  dci_estimation[0] = 0;
  dci_estimation[1] = 0;
  uint32_t crc;
  /* GPU batch hand-off (nr_polar_gpu.h): this candidate was already decoded as part of its
     occasion's batch. One-shot and fully qualified -- anything that does not match exactly falls
     through to the CPU decoder below, which is also what happens when a decode path runs the
     decoder twice for one candidate. */
  if (tls_polar_pre.llr == llr && tls_polar_pre.dci_length == dci_length
      && tls_polar_pre.aggregation_level == aggregation_level) {
    dci_estimation[0] = tls_polar_pre.payload;
    crc = tls_polar_pre.crc;
    tls_polar_pre.llr = NULL;
  } else {
    crc = polar_decoder_int16((int16_t*)llr, dci_estimation, 1, NR_POLAR_DCI_MESSAGE_TYPE,
                              dci_length, aggregation_level);
  }

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
      /* payload= is the RAW decoded bits, printed here rather than in DCIGT because DCIGT sits
       * DOWNSTREAM of the accept gate: when the per-field widths are wrong the payload is rejected
       * as an implausible DCI 1_1 and DCIGT never fires, which is exactly the case that needs the
       * dump. Combined with `crc` equal to the C-RNTI read from the gNB log at capture time, these
       * lines are a ground-truth-scorable record of real grants that the field-width solve can be
       * run against offline (the method CLAUDE.md section 12 used). MSB-first: bit i of the
       * dci_length-bit payload is bit (dci_length-1-i) of the value. */
      printf("FULLCRC L=%u dci_len=%u crc=0x%x upper=0x%x in_range=%d payload=0x%016llx\n",
             (unsigned)aggregation_level, (unsigned)dci_length, crc, crc >> 16,
             (crc >= rnti_min && crc <= rnti_max) ? 1 : 0,
             (unsigned long long)dci_estimation[0]);
  }
  return crc;
}

/// Step 2b: mismatched-bits false-detection measure. Migrated from NRSniffer's dci_nr.c
/// (nr_dci_false_detection): re-encode the decoded payload with the just-recovered RNTI and count
/// bit mismatches against the ORIGINAL soft LLR polarity. A CRC match is a 1/65536 chance false
/// accept even on a candidate that never carried real PDCCH; this is a far stronger discriminator,
/// since a genuine decode's re-encoded codeword should agree with almost every soft-bit sign. The
/// caller owns the accept/reject threshold -- this only measures.
static uint16_t blind_mismatched_bits(const int16_t* llr,
                                      uint64_t       dci_estimation[2],
                                      uint32_t       crc,
                                      uint8_t        aggregation_level,
                                      uint16_t       dci_length)
{
  uint32_t encoder_output[NR_MAX_DCI_SIZE_DWORD];
  polar_encoder_fast(dci_estimation, (void*)encoder_output, (int)crc, 1, NR_POLAR_DCI_MESSAGE_TYPE, dci_length,
                     aggregation_level);
  const uint8_t* enout_p        = (const uint8_t*)encoder_output;
  const int      encoded_length = (int)aggregation_level * 108;
  uint16_t       mismatches     = 0;
  for (int i = 0; i < encoded_length / 8; i++) {
    for (int b = 0; b < 8; b++)
      mismatches += ((enout_p[i] >> b) & 1) ^ ((llr[i * 8 + b] >> 15) & 1);
  }
  return mismatches;
}

// ---------------------------------------------------------------------------------------------
// DCI format 1_0 (TS 38.212 7.3.1.2.1). See nr_pdcch_blind_monitor.h's nr_blind_dci_format_t block
// for why this exists and why one decode serves all five RNTI variants.
// ---------------------------------------------------------------------------------------------

/// Bits every format-1_0 variant carries BESIDES the frequency-domain assignment. All five field
/// lists sum to this same value, which is what TS 38.212 7.3.1.0's size-alignment rules rely on:
///   C-RNTI / TC-RNTI : 1 identifier + 4 TDA + 1 VRB + 5 MCS + 1 NDI + 2 RV + 4 HARQ + 2 DAI
///                      + 2 TPC + 3 PUCCH-RI + 3 PDSCH-to-HARQ            = 28
///   SI-RNTI          : 4 + 1 + 5 + 2 RV + 1 SI-indicator + 15 reserved   = 28
///   RA-RNTI          : 4 + 1 + 5 + 2 TB-scaling + 16 reserved            = 28
///   P-RNTI           : 2 SM-indicator + 8 short-message + 4 + 1 + 5
///                      + 2 TB-scaling + 6 reserved                       = 28
#define NR_BLIND_DCI10_FIXED_BITS 28

/// ceil(log2(N*(N+1)/2)) -- the resource-allocation-type-1 (RIV) field width, shared by 0_0/1_0/1_1.
static int blind_riv_bits(uint16_t n_rb)
{
  const double span = ((double)n_rb * (double)(n_rb + 1)) / 2.0;
  return (int)ceil(log2(span));
}

uint16_t nr_pdcch_blind_dci10_size(uint16_t n_rb_riv)
{
  if (n_rb_riv < 1) {
    return 0;
  }
  return (uint16_t)(NR_BLIND_DCI10_FIXED_BITS + blind_riv_bits(n_rb_riv));
}

uint16_t nr_pdcch_blind_dci00_size(uint16_t n_rb_riv, int supplementary_uplink)
{
  if (n_rb_riv < 1) {
    return 0;
  }
  // TS 38.212 7.3.1.1.1: identifier(1) + freq domain(RIV) + TDA(4) + frequency hopping(1) + MCS(5)
  // + NDI(1) + RV(2) + HARQ process(4) + TPC for scheduled PUSCH(2) [+ UL/SUL indicator(1)].
  return (uint16_t)(20 + blind_riv_bits(n_rb_riv) + (supplementary_uplink ? 1 : 0));
}

/// Which RNTI classes to attempt when the caller does not narrow it. A C-RNTI format-1_0 CAN appear
/// in a common search space, but it is bit-indistinguishable from TC-RNTI there and TC selects the
/// same (common) TDRA list, so TC covers both -- see nr_blind_rnti_class_t's comment.
static uint32_t dci10_default_class_mask(uint8_t ss_type)
{
  if (ss_type == NR_BLIND_SS_COMMON) {
    return (1u << NR_BLIND_RNTI_CLASS_SI) | (1u << NR_BLIND_RNTI_CLASS_P) | (1u << NR_BLIND_RNTI_CLASS_RA)
           | (1u << NR_BLIND_RNTI_CLASS_TC);
  }
  return 1u << NR_BLIND_RNTI_CLASS_C;
}

/// Map a class onto the nr_rnti_type_t get_dl_tda_info() switches on (TS 38.214 Table 5.1.2.1.1-1).
static nr_rnti_type_t dci10_rnti_type(nr_blind_rnti_class_t klass)
{
  switch (klass) {
    case NR_BLIND_RNTI_CLASS_SI: return TYPE_SI_RNTI_;
    case NR_BLIND_RNTI_CLASS_RA: return TYPE_RA_RNTI_;
    case NR_BLIND_RNTI_CLASS_P:  return TYPE_P_RNTI_;
    case NR_BLIND_RNTI_CLASS_TC: return TYPE_TC_RNTI_;
    default:                     return TYPE_C_RNTI_;
  }
}

/// Parse ONE format-1_0 payload under ONE RNTI-class hypothesis and fill everything in `out` except
/// rnti/mismatched_bits (which the caller already wrote). Returns false with out->reject_reason set
/// on the first check that fails, leaving the caller free to try the next hypothesis.
///
/// Field order and widths are taken from TS 38.212 7.3.1.2.1 and cross-checked field-by-field
/// against this codebase's OWN gNB packer (gNB_scheduler_primitives.c's NR_DL_DCI_FORMAT_1_0 case,
/// which writes MSB-first from `dci_size` downward) -- the same reconcile-a-derivation-against-a-
/// known-good-implementation discipline that caught the SLIV ambiguity in the SIB1 work.
static bool dci10_parse(uint64_t                             payload,
                        uint16_t                             dci_length,
                        int                                  riv_bits,
                        int                                  pad_bits,
                        nr_blind_rnti_class_t                klass,
                        const nr_pdcch_blind_dci10_ctx_t*    ctx,
                        const nr_pdcch_blind_extract_opts_t* opts,
                        nr_pdcch_blind_result_t*             out)
{
  int      pos        = (int)dci_length;
  uint32_t fdra       = 0;
  uint32_t tda_idx    = 0;
  uint32_t vrb        = 0;
  uint32_t mcs        = 0;
  uint32_t ndi        = 0;
  uint32_t rv         = 0;
  uint32_t harq_pid   = 0;
  uint32_t tb_scaling = 0;
  uint32_t si_ind     = 0;
  uint32_t sm_ind     = 0;
  uint32_t sm         = 0;
  uint32_t reserved   = 0;

  switch (klass) {
    case NR_BLIND_RNTI_CLASS_C:
    case NR_BLIND_RNTI_CLASS_TC: {
      // Identifier for DCI formats: 1 = DL. A 0 here is DCI format 0_0, the UL grant that shares
      // this search space and payload size by construction (TS 38.212 7.3.1.0) -- correctly
      // rejected, exactly as the format 1_1 path rejects it.
      if (read_field(payload, &pos, 1) != 1) {
        out->reject_reason = "DCI-1_0 identifier=0 (format 0_0 UL grant, not a PDSCH DCI)";
        return false;
      }
      fdra = read_field(payload, &pos, riv_bits);
      // TS 38.212 7.3.1.2.1: an ALL-ONES frequency-domain field on a C-/TC-RNTI format 1_0 marks a
      // PDCCH ORDER -- a command to start random access -- and the remaining bits are
      // ra_preamble_index / UL-SUL / SSB index / PRACH mask, NOT a grant. Parsing it as one yields a
      // confident and completely wrong allocation, so it is rejected rather than mis-read.
      if (riv_bits < 32 && fdra == ((1u << riv_bits) - 1u)) {
        out->reject_reason = "DCI-1_0 PDCCH order (RA initiation), not a PDSCH assignment";
        return false;
      }
      tda_idx  = read_field(payload, &pos, 4);
      vrb      = read_field(payload, &pos, 1);
      mcs      = read_field(payload, &pos, 5);
      ndi      = read_field(payload, &pos, 1);
      rv       = read_field(payload, &pos, 2);
      harq_pid = read_field(payload, &pos, 4);
      (void)read_field(payload, &pos, 2); // Downlink assignment index (2 RESERVED bits for TC-RNTI)
      (void)read_field(payload, &pos, 2); // TPC command for scheduled PUCCH
      (void)read_field(payload, &pos, 3); // PUCCH resource indicator
      (void)read_field(payload, &pos, 3); // PDSCH-to-HARQ_feedback timing indicator
      break;
    }
    case NR_BLIND_RNTI_CLASS_SI:
      fdra     = read_field(payload, &pos, riv_bits);
      tda_idx  = read_field(payload, &pos, 4);
      vrb      = read_field(payload, &pos, 1);
      mcs      = read_field(payload, &pos, 5);
      rv       = read_field(payload, &pos, 2);
      si_ind   = read_field(payload, &pos, 1);
      reserved = read_field(payload, &pos, 15);
      // 15 bits that the spec fixes at zero: a 1-in-32768 signature, and by far the strongest
      // false-accept discriminator available to a blind receiver on this format.
      if (reserved != 0) {
        out->reject_reason = "DCI-1_0/SI-RNTI reserved bits are non-zero";
        return false;
      }
      break;
    case NR_BLIND_RNTI_CLASS_RA:
      fdra       = read_field(payload, &pos, riv_bits);
      tda_idx    = read_field(payload, &pos, 4);
      vrb        = read_field(payload, &pos, 1);
      mcs        = read_field(payload, &pos, 5);
      tb_scaling = read_field(payload, &pos, 2);
      reserved   = read_field(payload, &pos, 16);
      if (reserved != 0) {
        out->reject_reason = "DCI-1_0/RA-RNTI reserved bits are non-zero";
        return false;
      }
      break;
    case NR_BLIND_RNTI_CLASS_P:
      sm_ind     = read_field(payload, &pos, 2);
      sm         = read_field(payload, &pos, 8);
      fdra       = read_field(payload, &pos, riv_bits);
      tda_idx    = read_field(payload, &pos, 4);
      vrb        = read_field(payload, &pos, 1);
      mcs        = read_field(payload, &pos, 5);
      tb_scaling = read_field(payload, &pos, 2);
      reserved   = read_field(payload, &pos, 6);
      if (reserved != 0) {
        out->reject_reason = "DCI-1_0/P-RNTI reserved bits are non-zero";
        return false;
      }
      // TS 38.331 Short Message Indicator: 00 is reserved; 01 = scheduling info only (a real PDSCH
      // grant, no short message); 10 = short message only, which carries NO PDSCH assignment, so the
      // allocation fields below are meaningless; 11 = both.
      if (sm_ind == 0) {
        out->reject_reason = "DCI-1_0/P-RNTI shortMessageIndicator=00 (reserved)";
        return false;
      }
      if (sm_ind == 2) {
        out->reject_reason = "DCI-1_0/P-RNTI carries a short message only (no PDSCH assignment)";
        return false;
      }
      break;
    default:
      out->reject_reason = "unknown DCI-1_0 RNTI class";
      return false;
  }

  // TS 38.212 7.3.1.0: in a UE-specific search space the smaller of format 0_0/1_0 is ZERO-padded
  // up to the other's size. Any non-zero padding means this is not a real format-1_0 payload (or
  // n_rb_riv is wrong, which would invalidate everything above it anyway).
  if (pad_bits > 0) {
    const int chunk = (pad_bits > 32) ? 32 : pad_bits;
    if (read_field(payload, &pos, chunk) != 0) {
      out->reject_reason = "DCI-1_0 size-alignment padding is non-zero";
      return false;
    }
  }

  // TS 38.214 5.1.3.1: a PDSCH scheduled by format 1_0 ALWAYS uses Table 5.1.3.1-1 (qam64),
  // whatever mcs-Table the deployment configures -- qam256 is conditioned on format 1_1. In that
  // table entries 0..28 are valid and 29..31 are reserved for retransmissions whose modulation the
  // UE already knows. (The format 1_1 path above uses >=28 because table 2, which a qam256
  // deployment selects there, reserves 28..31; here the table is fixed by the spec so the exact
  // bound is known.)
  if (mcs >= 29) {
    out->reject_reason = "DCI-1_0 MCS in the reserved range (29-31 of Table 5.1.3.1-1)";
    return false;
  }
  // TS 38.214 Table 5.1.3.2-2: scaling factor S = {1, 0.5, 0.25}; the fourth code point is reserved.
  if ((klass == NR_BLIND_RNTI_CLASS_RA || klass == NR_BLIND_RNTI_CLASS_P) && tb_scaling > 2) {
    out->reject_reason = "DCI-1_0 TB scaling in the reserved code point (3)";
    return false;
  }
  // TS 38.211 7.3.1.6 interleaved VRB-to-PRB mapping permutes the allocation in 2-RB bundles, so the
  // PRBs are NOT the contiguous set the RIV names. This receiver's whole downstream -- DM-RS channel
  // estimation, RE enumeration, the data-aided reconstruction -- indexes a contiguous PRB range, and
  // OAI's UE PHY implements no de-interleaving at all (nr_ue_procedures.c sets
  // dlsch_pdu->vrb_to_prb_mapping and NOTHING in openair1/PHY/NR_UE_TRANSPORT ever reads it), so
  // there is nothing to reuse. Decoded, reported, and REJECTED -- silently extracting the wrong REs
  // would look like a weak channel rather than a wrong one.
  if (vrb != 0) {
    out->reject_reason = "DCI-1_0 interleaved VRB-to-PRB mapping (not supported: no de-interleaver)";
    return false;
  }

  uint16_t start_rb, num_rb;
  if (!riv_to_prb_alloc(fdra, ctx->n_rb_riv, &start_rb, &num_rb)) {
    out->reject_reason = "RIV decodes to a PRB allocation outside the DCI-1_0 frequency reference";
    return false;
  }

  // ---- TDRA. TS 38.214 Table 5.1.2.1.1-1, mirroring get_dl_tda_info()/get_dl_tdalist(): the
  // DEDICATED pdsch-Config list applies only to C-RNTI in a UE-SPECIFIC search space; everything
  // else -- SI/RA/TC-RNTI, and C-RNTI in a common search space -- takes pdsch-ConfigCommon's list.
  // Note the field is ALWAYS 4 bits in format 1_0 (unlike 1_1's ceil(log2(count))), so an index past
  // the end of a short list is a real and useful false-accept discriminator, not a config error. ----
  const bool     dedicated_list = (klass == NR_BLIND_RNTI_CLASS_C) && (ctx->ss_type == NR_BLIND_SS_UE_SPECIFIC);
  int            n_tda          = 0;
  const uint8_t* t_start        = NULL;
  const uint8_t* t_len          = NULL;
  const uint8_t* t_map          = NULL;
  if (opts != NULL) {
    if (!dedicated_list && opts->tda_common_count > 0) {
      n_tda   = opts->tda_common_count;
      t_start = opts->tda_common_start;
      t_len   = opts->tda_common_length;
      t_map   = opts->tda_common_mapping;
    } else if (opts->tda_count > 0) {
      n_tda   = opts->tda_count;
      t_start = opts->tda_start;
      t_len   = opts->tda_length;
      t_map   = opts->tda_mapping;
    }
  }
  // SIB1's own DCIs are the one case where no configured list can apply: pdsch-ConfigCommon travels
  // INSIDE SIB1, so a receiver decoding SIB1 has not read it yet and the default table selected by
  // the SS/PBCH-to-CORESET#0 multiplexing pattern is what the gNB used (TS 38.214 5.1.2.1.1).
  if (klass == NR_BLIND_RNTI_CLASS_SI && ctx->sib1) {
    n_tda = 0;
  }

  NR_tda_info_t tda = {0};
  if (n_tda > 0) {
    if ((int)tda_idx >= n_tda) {
      out->reject_reason = "DCI-1_0 time_domain_assignment index beyond the configured TDRA list";
      return false;
    }
    tda.valid_tda        = true;
    tda.startSymbolIndex = t_start[tda_idx];
    tda.nrOfSymbols      = t_len[tda_idx];
    tda.mapping_type     = t_map[tda_idx] ? typeB : typeA;
    if (tda.nrOfSymbols < 1 || tda.startSymbolIndex + tda.nrOfSymbols > 14
        || (tda.mapping_type == typeA && tda.startSymbolIndex + tda.nrOfSymbols < 2)) {
      out->reject_reason = "configured TDRA entry spans an illegal symbol range";
      return false;
    }
  } else {
    // mux_pattern is validated by the caller (get_default_table_type() AssertFatal()s outside 1..3,
    // and this runs on the RT receive path).
    tda = get_dl_tda_info(NULL /* dl_BWP: forces the default-table branch */, 0 /* ss_type, unused */,
                          (int)tda_idx, ctx->dmrs_typeA_position, ctx->mux_pattern ? ctx->mux_pattern : 1,
                          dci10_rnti_type(klass), (ctx->ss_type == NR_BLIND_SS_COMMON) ? 0 : 1,
                          ctx->sib1 != 0);
    if (!tda.valid_tda) {
      out->reject_reason = "DCI-1_0 time_domain_assignment index invalid for the default TDRA table";
      return false;
    }
  }

  // ---- DM-RS. TS 38.214 5.1.6.2, and this codebase's own fill_dmrs_mask() implements it the same
  // way: for a PDSCH scheduled by format 1_0 with mapping type A, dmrs-AdditionalPosition is pos2
  // REGARDLESS of any dedicated DMRS config (see its `dci_format != NR_DL_DCI_FORMAT_1_0` guard).
  // This is why the deployment's dmrs override -- correct for format 1_1, and set to pos1 on gNBs
  // that configure it -- must NOT be applied here. Type B does read the dedicated config, so the
  // override still applies on that branch. maxLength is 1: format 1_0 is single-symbol front-loaded
  // DM-RS by definition. ----
  const int add_pos = (tda.mapping_type == typeA)
                          ? 2
                          : ((opts != NULL && opts->dmrs_add_pos >= 0) ? opts->dmrs_add_pos : 2);
  const int32_t dmrs_mask = blind_fill_dmrs_mask(ctx->dmrs_typeA_position, tda.nrOfSymbols, tda.startSymbolIndex,
                                                 tda.mapping_type, add_pos, 1 /* maxLength */);
  if (dmrs_mask <= 0) {
    out->reject_reason = "DM-RS symbol mask undefined for this DCI-1_0 TDRA entry";
    return false;
  }

  // ---- All checks passed. ----
  out->dci_format   = NR_BLIND_DCI_FORMAT_1_0;
  out->rnti_class   = (uint8_t)klass;
  out->start_rb     = start_rb;
  out->num_rb       = num_rb;
  out->start_symbol = (uint8_t)tda.startSymbolIndex;
  out->num_symbols  = (uint8_t)tda.nrOfSymbols;
  out->dl_dmrs_symb_pos = (uint16_t)dmrs_mask;
  // TS 38.214 5.1.6.1.3: for a PDSCH scheduled by format 1_0 the UE assumes 1 CDM group without data
  // (group 0) for a 2-symbol allocation and 2 CDM groups ({0,1}, i.e. the whole DM-RS symbol
  // reserved) in every other case. Format 1_0 has no antenna-ports field to read this from -- it is
  // derived, not decoded. Consequence worth knowing: at n_dmrs_cdm_groups == 2 the data-aided RE
  // enumeration takes its whole-symbol branch, which is the case that behaves identically with and
  // without the DM-RS-symbol data-RE fix; a 2-symbol 1_0 grant lands in the other one.
  out->n_dmrs_cdm_groups = (tda.nrOfSymbols == 2) ? 1 : 2;
  out->dmrs_ports        = 1;  // antenna port 1000 only (TS 38.214 5.1.6.2)
  out->nscid             = 0;  // format 1_0 has no DM-RS sequence initialisation field
  out->mcs               = (uint8_t)mcs;
  out->mcs_table         = 0;  // Table 5.1.3.1-1, always -- see the MCS check above
  out->rv                = (uint8_t)rv;
  out->ndi               = (uint8_t)ndi;
  out->harq_pid          = (uint8_t)harq_pid;
  out->tda_index         = (uint8_t)tda_idx;
  out->mapping_type      = (tda.mapping_type == typeB) ? 1 : 0;
  out->vrb_to_prb        = (uint8_t)vrb;
  out->tb_scaling        = (uint8_t)tb_scaling;
  out->si_indicator      = (uint8_t)si_ind;
  out->short_messages_ind = (uint8_t)sm_ind;
  out->short_messages    = (uint8_t)sm;
  out->plausible         = true;
  out->reject_reason     = NULL;
  return true;
}

static bool blind_decode_and_interpret_10(const int16_t*                       llr,
                                          uint8_t                              aggregation_level,
                                          uint16_t                             dci_length,
                                          const nr_pdcch_blind_dci10_ctx_t*    ctx,
                                          uint16_t                             rnti_min,
                                          uint16_t                             rnti_max,
                                          const nr_pdcch_blind_extract_opts_t* opts,
                                          nr_pdcch_blind_result_t*             out,
                                          nr_dci10_interpretation_report_t*    report)
{
  if (report) {
    memset(report, 0, sizeof(*report));
    report->state = NR_DCI_REJECTED;
    report->unique_candidate = -1;
  }
  memset(out, 0, sizeof(*out));

  out->plausible  = false;
  out->dci_format = NR_BLIND_DCI_FORMAT_1_0;

  if (ctx == NULL || dci_length == 0 || dci_length > 63 || ctx->n_rb_riv < 1) {
    out->reject_reason = "invalid dci_length / DCI-1_0 context";
    return false;
  }
  if (ctx->mux_pattern > 3) {
    // get_default_table_type() AssertFatal()s on anything outside 1..3 and this runs on the RT
    // receive path, so it is checked here rather than allowed to abort the softmodem.
    out->reject_reason = "invalid SS/PBCH-to-CORESET0 multiplexing pattern";
    return false;
  }
  const int riv_bits = blind_riv_bits(ctx->n_rb_riv);
  const int need     = NR_BLIND_DCI10_FIXED_BITS + riv_bits;
  if (need > (int)dci_length) {
    out->reject_reason = "DCI-1_0 field widths exceed dci_length";
    return false;
  }
  const int pad_bits = (int)dci_length - need;

  // ---- Step 1: one RNTI-independent polar decode, shared by every class hypothesis below. ----
  uint64_t       dci_estimation[2] = {0};
  const uint32_t crc = blind_polar_decode(llr, aggregation_level, dci_length, rnti_min, rnti_max, dci_estimation);
  out->rnti          = (uint16_t)crc;
  // Kept even when this candidate is about to be rejected: TS 38.212 7.3.1.0 size-aligns 0_0 with
  // 1_0, so this same word IS the format-0_0 payload when the identifier bit is 0.
  out->payload       = dci_estimation[0];

  // ---- Step 2: which classes are admissible for this CRC-recovered value. The broadcast RNTIs are
  // FIXED by TS 38.321 Table 7.1-1 (SI-RNTI = 0xFFFF, P-RNTI = 0xFFFE) and sit outside the dynamic
  // C-RNTI range by construction, so they are admitted independently of [rnti_min, rnti_max] rather
  // than requiring the caller to widen a range that exists to reject exactly those values. ----
  const uint32_t mask = ctx->rnti_class_mask ? ctx->rnti_class_mask : dci10_default_class_mask(ctx->ss_type);
  nr_blind_rnti_class_t attempts[3];
  int                   n_attempts = 0;

  if (crc == 0xFFFF) {
    if (mask & (1u << NR_BLIND_RNTI_CLASS_SI)) {
      attempts[n_attempts++] = NR_BLIND_RNTI_CLASS_SI;
    }
  } else if (crc == 0xFFFE) {
    if (mask & (1u << NR_BLIND_RNTI_CLASS_P)) {
      attempts[n_attempts++] = NR_BLIND_RNTI_CLASS_P;
    }
  } else if (crc >= rnti_min && crc <= rnti_max) {
    // RA first: it is the only one of the three dynamic-range classes with a spec-fixed reserved
    // field (16 bits) AND a bounded value range, so when it passes it passes for a reason. A
    // C-/TC-RNTI payload can also present 16 zero tail bits (rv=0, harq=0, dai=0, tpc=0, PUCCH-RI=0,
    // k1=0 is an entirely ordinary grant), which is why the RNTI bound is applied as well.
    /* EXACT test when SIB1 has told us the PRACH config, range test otherwise.
     * `crc <= NR_PDCCH_BLIND_RA_RNTI_MAX` admits 17920 of 65536 values -- 27 % of random CRCs --
     * which is why the RA accept counter has been indistinguishable from noise (RA=13 in a 900 s
     * capture, never repeating). RA-RNTI is a STRUCTURED value: decomposing it and checking the
     * fields against the cell's own msg1-FDM and SUL presence typically cuts the admissible set
     * by 8-16x. That matters because RA-RNTI and SI-RNTI are the only two classes a passive
     * receiver can verify without already knowing the answer. */
    if (mask & (1u << NR_BLIND_RNTI_CLASS_RA)) {
      /* Tighten ONLY when the PRACH config is actually in hand. The first cut gated the fallback
       * on `prior == NULL`, but a prior exists as soon as SIB1 is decoded -- and this cell's SIB1
       * turns out to carry no rach-ConfigCommon reachable here (measured: "rach(none)"), which
       * would have made ra_rnti_valid() always false and silently DISABLED RA decoding altogether,
       * killing the RAR -> TC-RNTI harvest this path exists to feed. Gate on the RACH fields
       * themselves, so the exact test is used when it can be, and the old range test otherwise. */
      const nr_pdcch_sib1_prior_t *ra_prior = nr_pdcch_sib1_prior_get();
      const bool ra_exact = (ra_prior != NULL) && ra_prior->rach_valid;
      const bool ra_ok = ra_exact ? nr_pdcch_sib1_prior_ra_rnti_valid((uint16_t)crc)
                                  : (crc <= NR_PDCCH_BLIND_RA_RNTI_MAX);
      if (ra_ok && ra_exact) {
        static _Atomic uint64_t s_rav = 0;
        const uint64_t v = atomic_fetch_add_explicit(&s_rav, 1, memory_order_relaxed) + 1;
        if (v == 1 || (v % 100) == 0)
          LOG_A(PHY, "SENSING: RA-RNTI VERIFIED 0x%x -- %llu passed the exact PRACH decomposition\n",
                (unsigned)crc, (unsigned long long)v);
      }
      if (ra_ok) {
        attempts[n_attempts++] = NR_BLIND_RNTI_CLASS_RA;
      }
    }
    // C and TC share one hypothesis: the field lists are bit-identical and the label is chosen by
    // search space (see nr_blind_rnti_class_t). EITHER bit therefore enables it -- gating on the
    // label's own bit would silently return nothing when a caller asks for C in a common search
    // space, which is a configuration foot-gun rather than a meaningful distinction.
    const nr_blind_rnti_class_t dyn =
        (ctx->ss_type == NR_BLIND_SS_COMMON) ? NR_BLIND_RNTI_CLASS_TC : NR_BLIND_RNTI_CLASS_C;
    if (mask & ((1u << NR_BLIND_RNTI_CLASS_C) | (1u << NR_BLIND_RNTI_CLASS_TC))) {
      attempts[n_attempts++] = dyn;
    }
  } else {
    out->reject_reason = "CRC-recovered value outside plausible RNTI range";
    return false;
  }

  if (n_attempts == 0) {
    out->reject_reason = "no DCI-1_0 RNTI class enabled for this CRC-recovered value";
    return false;
  }

  // ---- Step 2b: the re-encode false-detection measure, once -- it depends only on the recovered
  // RNTI and the coded bits, not on which class hypothesis wins. ----
  out->mismatched_bits = blind_mismatched_bits(llr, dci_estimation, crc, aggregation_level, dci_length);

  // Manual returns the first passing class. Auto keeps every class: RA reserved
  // zeros can also be an ordinary C/TC grant. All hypotheses share the same
  // polar word and re-encode evidence, without repeating the polar decode.

  const uint16_t saved_rnti       = out->rnti;
  const uint16_t saved_mismatches = out->mismatched_bits;
  for (int i = 0; i < n_attempts; i++) {
    const char* last_reason = NULL;
    out->rnti_class = (uint8_t)attempts[i];
    const bool parsed = dci10_parse(dci_estimation[0], dci_length, riv_bits, pad_bits, attempts[i], ctx, opts, out);
    if (report) {
      report->candidates[report->attempted++] = *out;
      if (parsed) {
        report->unique_candidate = i;
        report->surviving++;
      }
    } else if (parsed) {

      return true;
    }
    last_reason = out->reject_reason;
    // dci10_parse() writes into `out` as it goes, so reset the fields the caller still needs before
    // the next hypothesis -- without this a failed attempt would clear the RNTI a later one reports.
    memset(out, 0, sizeof(*out));
    out->plausible       = false;
    out->dci_format      = NR_BLIND_DCI_FORMAT_1_0;
    out->rnti            = saved_rnti;
    out->mismatched_bits = saved_mismatches;
    out->reject_reason   = last_reason;
    // The decoded word survives every class hypothesis -- it is what the format-0_0 reader needs.
    out->payload         = dci_estimation[0];
  }
  if (report && report->surviving == 1) {
    *out = report->candidates[report->unique_candidate];
    report->state = NR_DCI_UNRESOLVED; // protocol validity is not physical validation
    return true;
  }
  if (report && report->surviving > 1) {
    report->state = NR_DCI_AMBIGUOUS;
    report->unique_candidate = -1;
    out->reject_reason = "ambiguous DCI-1_0 RNTI-class interpretations";
  }
  return false;
}

bool nr_pdcch_blind_decode_and_extract_10(const int16_t *llr, uint8_t aggregation_level,
                                         uint16_t dci_length, const nr_pdcch_blind_dci10_ctx_t *ctx,
                                         uint16_t rnti_min, uint16_t rnti_max,
                                         const nr_pdcch_blind_extract_opts_t *opts,
                                         nr_pdcch_blind_result_t *out)
{
  return blind_decode_and_interpret_10(llr, aggregation_level, dci_length, ctx,
                                        rnti_min, rnti_max, opts, out, NULL);
}

bool nr_pdcch_blind_decode_10_mode(bool automatic,
                                  const int16_t *llr, uint8_t aggregation_level,
                                  uint16_t dci_length, const nr_pdcch_blind_dci10_ctx_t *ctx,
                                  uint16_t rnti_min, uint16_t rnti_max,
                                  const nr_pdcch_blind_extract_opts_t *opts,
                                  nr_pdcch_blind_result_t *out,
                                  nr_dci10_interpretation_report_t *report)
{
  nr_dci10_interpretation_report_t local;
  if (!automatic && report) {
    memset(report, 0, sizeof(*report));
    report->state = NR_DCI_UNRESOLVED;
    report->unique_candidate = -1;
  }
  return blind_decode_and_interpret_10(llr, aggregation_level, dci_length, ctx,
                                        rnti_min, rnti_max, opts, out,
                                        automatic ? (report ? report : &local) : NULL);
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

bool nr_pdcch_blind_decode_raw(const int16_t *llr, uint8_t aggregation_level,
                                 uint16_t dci_length, uint16_t rnti_min, uint16_t rnti_max,
                                 bool require_dl_indicator, nr_pdcch_blind_raw_result_t *out)
{
  if (!out) return false;
  memset(out,0,sizeof(*out));
  if (!llr || dci_length<1 || dci_length>63 || !rnti_min || rnti_min>rnti_max ||
      (aggregation_level!=1 && aggregation_level!=2 && aggregation_level!=4 &&
       aggregation_level!=8 && aggregation_level!=16)) {
    out->reject_reason="invalid raw DL decode arguments";
    return false;
  }
  uint64_t bits[2]={0};
  const uint32_t crc=blind_polar_decode(llr,aggregation_level,dci_length,rnti_min,rnti_max,bits);
  out->payload=bits[0]; out->rnti=(uint16_t)crc;
  if(crc<rnti_min || crc>rnti_max) {
    out->reject_reason="CRC-recovered value outside plausible RNTI range";
    return false;
  }
  if(require_dl_indicator && ((bits[0]>>(dci_length-1))&1)==0) {
    out->reject_reason="format indicator=0 (UL grant, not DL)";
    return false;
  }
  out->mismatched_bits=blind_mismatched_bits(llr,bits,crc,aggregation_level,dci_length);
  return true;
}

bool nr_pdcch_blind_decode_raw_11(const int16_t *llr, uint8_t aggregation_level,
                                 uint16_t dci_length, uint16_t rnti_min, uint16_t rnti_max,
                                 nr_pdcch_blind_raw_result_t *out)
{
  return nr_pdcch_blind_decode_raw(llr, aggregation_level, dci_length, rnti_min, rnti_max, true, out);
}

bool nr_pdcch_blind_extract_11(const nr_pdcch_blind_raw_result_t *raw,
                               uint16_t dci_length, uint16_t bwp_size,
                               uint8_t dmrs_typeA_position,
                               const nr_pdcch_blind_extract_opts_t *opts,
                               nr_pdcch_blind_result_t *out)
{
  if (!out) return false;
  memset(out, 0, sizeof(*out));
  if (!raw || !dci_length || dci_length > 63 || !bwp_size) {
    out->reject_reason = "invalid raw DCI/extraction arguments";
    return false;
  }
  out->rnti = raw->rnti;
  out->mismatched_bits = raw->mismatched_bits;
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
  const uint64_t payload = raw->payload;

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
  /* 4-bit field: Table 7.3.1.2.2-1 (type 1, maxLength 1). 5-bit field: Table -2 (type 1, maxLength 2),
   * whose code point also fixes the DM-RS symbol count. Type 2 (5/6 bits, Tables -3/-4) is not
   * decoded: rejected as out of range, so a type-2 hypothesis never produces a grant. */
  /* Table by (field width, DM-RS type): 4 bits = type 1 len 1 (-1); 5 bits = type 1 len 2 (-2) or
   * type 2 len 1 (-3), told apart by the DM-RS type hypothesis; 6 bits = type 2 len 2 (-4). */
  const int dmrs_t2 = (opts != NULL && opts->dmrs_config_type == 1);
  int ap_table = 1, ap_rows = 12, ap_nports = 4;
  if (f.ant_ports == 5 && !dmrs_t2) { ap_table = 2; ap_rows = 31; ap_nports = 8; }
  else if (f.ant_ports == 5 && dmrs_t2) { ap_table = 3; ap_rows = 24; ap_nports = 6; }
  else if (f.ant_ports == 6 && dmrs_t2) { ap_table = 4; ap_rows = 58; ap_nports = 12; }
  else if (f.ant_ports != 4) { out->reject_reason = "antenna_ports width inconsistent with the DM-RS type"; return false; }
  if (antenna_ports >= (uint32_t)ap_rows) {
    out->reject_reason = "antenna_ports field outside its table's valid rows";
    return false;
  }
  const uint8_t *ap_row = ap_table == 1 ? g_table_7_3_2_3_3_1[antenna_ports]
                        : ap_table == 2 ? g_table_7_3_2_3_3_2[antenna_ports]
                        : ap_table == 3 ? g_table_7_3_2_3_3_3[antenna_ports] : g_table_7_3_2_3_3_4[antenna_ports];
  const int ap_len2 = (ap_table == 2) || (ap_table == 4);
  // Table selection is still unknown here. MCS 28 is valid in tables 0 and 2;
  // the actual PDSCH decoder checks the selected table's nonzero code rate.
  if (mcs >= 29) {
    out->reject_reason = "MCS reserved in every supported DL table (29-31)";
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
  const int max_len = ap_table == 2 ? g_table_7_3_2_3_3_2[antenna_ports][9]
                    : ap_table == 4 ? g_table_7_3_2_3_3_4[antenna_ports][13]
                    : (opts != NULL && opts->dmrs_max_length > 0) ? opts->dmrs_max_length : 1;
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
  out->n_dmrs_cdm_groups  = ap_row[0];
  out->dmrs_ports         = 0;
  for (int k = 0; k < ap_nports; k++)
    out->dmrs_ports |= (uint16_t)(ap_row[1 + k] << k);
  out->dmrs_config_type   = (uint8_t)dmrs_t2;
  (void)ap_len2;
  out->nscid              = (uint8_t)dmrs_seq_init;
  out->mcs                = (uint8_t)mcs;
  out->rv                 = (uint8_t)rv;
  out->ndi                = (uint8_t)ndi;
  out->harq_pid           = (uint8_t)harq_pid;
  out->tda_index          = (uint8_t)time_domain_assignment;
  out->mapping_type       = (tda.mapping_type == typeB) ? 1 : 0;
  out->dci_format         = NR_BLIND_DCI_FORMAT_1_1;
  out->rnti_class         = NR_BLIND_RNTI_CLASS_C;
  out->plausible          = true;
  out->reject_reason      = NULL;
  return true;
}

static bool blind_decode_and_extract_11_inner(const int16_t* llr,
                                          uint8_t         aggregation_level,
                                          uint16_t        dci_length,
                                          uint16_t        bwp_size,
                                          uint8_t         dmrs_typeA_position,
                                          uint16_t        rnti_min,
                                          uint16_t        rnti_max,
                                          const nr_pdcch_blind_extract_opts_t* opts,
                                          nr_pdcch_blind_result_t* out)
{
  nr_pdcch_blind_raw_result_t raw;
  if (!nr_pdcch_blind_decode_raw_11(llr, aggregation_level, dci_length, rnti_min, rnti_max, &raw)) {
    memset(out, 0, sizeof(*out));
    out->rnti = raw.rnti;
    out->reject_reason = raw.reject_reason;
    return false;
  }
  return nr_pdcch_blind_extract_11(&raw, dci_length, bwp_size, dmrs_typeA_position, opts, out);
}

/* ISAC_DCI_WATCH_RNTI=0x<rnti>: print WHY a GENUINE candidate is rejected. The existing
 * last_reject= summary is useless for this because noise candidates that clear the RNTI range
 * check outnumber real grants ~100:1, so it always reports a noise RNTI. Filtering on the live
 * C-RNTI (re-read from the gNB log at capture time -- it churns on re-attach) makes the reason
 * read off real grants only. Zero cost when unset: one cached getenv plus an integer compare. */
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
  const bool ok = blind_decode_and_extract_11_inner(llr, aggregation_level, dci_length, bwp_size,
                                                    dmrs_typeA_position, rnti_min, rnti_max, opts, out);
  static int s_watch = -1;
  if (s_watch < 0) {
    const char* e = getenv("ISAC_DCI_WATCH_RNTI");
    s_watch = (e != NULL) ? (int)strtol(e, NULL, 0) : 0;
  }
  if (s_watch > 0 && out->rnti == (uint16_t)s_watch) {
    static unsigned long n_hit = 0, n_ok = 0;
    n_hit++;
    if (ok) {
      n_ok++;
    }
    if (n_hit <= 20 || (n_hit % 2000) == 0) {
      printf("DCIWATCH n=%lu ok=%lu L=%u dci_len=%u bwp=%u tda_cnt=%d dmrs_pos=%u "
             "reason=\"%s\" mcs=%u rv=%u ant=%u tda_idx=%u rb=[%u..%u)\n",
             n_hit, n_ok, (unsigned)aggregation_level, (unsigned)dci_length, (unsigned)bwp_size,
             (opts != NULL) ? opts->tda_count : -1, (unsigned)dmrs_typeA_position,
             ok ? "ACCEPT" : (out->reject_reason ? out->reject_reason : "(null)"),
             out->mcs, out->rv, out->n_dmrs_cdm_groups, out->tda_index,
             out->start_rb, out->start_rb + out->num_rb);
      fflush(stdout);
    }
  }
  return ok;
}

// =============================================================================================
// UPLINK: DCI formats 0_1 and 0_0. See the header's UL section for why this exists and, more
// importantly, for why the field WIDTHS here must be reconciled against the deployment rather
// than trusted.
// =============================================================================================

// ---------------------------------------------------------------------------------------------
// TS 38.214 Table 6.1.2.1.1-2 (default PUSCH time-domain resource allocation A, normal CP).
// Columns: {mapping type (0=A,1=B), k2 base, S, L}. The actual k2 is this base PLUS j, where j
// depends on the numerology (TS 38.214 6.1.2.1.1: j = {1,1,2,3,11,21}[mu]) -- so k2 is NOT a
// property of the table alone, which is why blind_ul_tda() takes mu.
//
// Duplicated from nr_mac_common.c's table_6_1_2_1_1_2 for the same reason the DL tables above are
// duplicated: they are 3GPP spec constants, and the file they live in compiles into a heavy target
// this lean library deliberately does not link.
// ---------------------------------------------------------------------------------------------
static const uint8_t g_table_6_1_2_1_1_2[16][4] = {
    {0, 0, 0, 14}, {0, 0, 0, 12}, {0, 0, 0, 10}, {1, 0, 2, 10},
    {1, 0, 4, 10}, {1, 0, 4, 8},  {1, 0, 4, 6},  {0, 1, 0, 14},
    {0, 1, 0, 12}, {0, 1, 0, 10}, {0, 2, 0, 14}, {0, 2, 0, 12},
    {0, 2, 0, 10}, {1, 0, 8, 6},  {0, 3, 0, 14}, {0, 3, 0, 10},
};

/// TS 38.214 6.1.2.1.1's j, indexed by numerology mu.
static const uint8_t g_ul_tda_j[6] = {1, 1, 2, 3, 11, 21};

// ---------------------------------------------------------------------------------------------
// TS 38.211 Tables 6.4.1.1.3-3 / -4 (PUSCH DM-RS positions l' within a slot). These are the UPLINK
// tables and they are NOT the same as the PDSCH ones already duplicated above -- rows 8/9 and
// 10/11 differ, so reusing g_table_7_4_1_1_2_3 would give a wrong DM-RS mask on exactly the
// mid-length allocations. Row index is 0 for ld < 4 and ld-3 otherwise (12 rows, ld up to 14),
// which is also a different indexing convention from the PDSCH table's.
// Columns 0-3 = mapping type A, 4-7 = type B. l' == l0 is encoded as bit 0.
// ---------------------------------------------------------------------------------------------
static const int32_t g_table_6_4_1_1_3_3[12][8] = {
    {-1, -1, -1, -1, 1, 1, 1, 1},            // ld < 4
    {0, 0, 0, 0, 1, 1, 1, 1},                // ld = 4
    {0, 0, 0, 0, 1, 17, 17, 17},             // ld = 5
    {0, 0, 0, 0, 1, 17, 17, 17},             // ld = 6
    {0, 0, 0, 0, 1, 17, 17, 17},             // ld = 7
    {0, 128, 128, 128, 1, 65, 73, 73},       // ld = 8
    {0, 128, 128, 128, 1, 65, 73, 73},       // ld = 9
    {0, 512, 576, 576, 1, 257, 273, 585},    // ld = 10
    {0, 512, 576, 576, 1, 257, 273, 585},    // ld = 11
    {0, 512, 576, 2336, 1, 1025, 1057, 585}, // ld = 12
    {0, 2048, 2176, 2336, 1, 1025, 1057, 585}, // ld = 13
    {0, 2048, 2176, 2336, 1, 1025, 1057, 585}, // ld = 14
};
static const int32_t g_table_6_4_1_1_3_4[12][8] = {
    {-1, -1, -1, -1, -1, -1, -1, -1}, // ld < 4
    {0, 0, -1, -1, -1, -1, -1, -1},   // ld = 4
    {0, 0, -1, -1, 3, 3, -1, -1},     // ld = 5
    {0, 0, -1, -1, 3, 3, -1, -1},     // ld = 6
    {0, 0, -1, -1, 3, 3, -1, -1},     // ld = 7
    {0, 0, -1, -1, 3, 99, -1, -1},    // ld = 8
    {0, 0, -1, -1, 3, 99, -1, -1},    // ld = 9
    {0, 768, -1, -1, 3, 387, -1, -1}, // ld = 10
    {0, 768, -1, -1, 3, 387, -1, -1}, // ld = 11
    {0, 768, -1, -1, 3, 1539, -1, -1},// ld = 12
    {0, 3072, -1, -1, 3, 1539, -1, -1},// ld = 13
    {0, 3072, -1, -1, 3, 1539, -1, -1},// ld = 14
};

/// PUSCH DM-RS symbol bitmap. Mirrors nr_mac_common.c's get_l_prime() exactly, except that where
/// that function AssertFatal()s on an invalid (ld, column) pair -- aborting the softmodem -- this
/// one returns -1, because a blind decoder must be able to REJECT a candidate that a real UE could
/// never have been given.
int32_t nr_pdcch_blind_ul_dmrs_mask(uint8_t num_symbols,
                                  uint8_t start_symbol,
                                  int     mapping_type_is_b,
                                  int     add_pos,
                                  int     max_length,
                                  uint8_t dmrs_typeA_position)
{
  if (!num_symbols || start_symbol + num_symbols > 14 ||
      mapping_type_is_b < 0 || mapping_type_is_b > 1 ||
      max_length < 1 || max_length > 2 || dmrs_typeA_position > 1 ||
      add_pos < 0 || add_pos > 3) {
    return -1;
  }
  const int ld  = mapping_type_is_b ? num_symbols : (num_symbols + start_symbol);
  const int row = (ld < 4) ? 0 : (ld - 3);
  if (row < 0 || row > 11) {
    return -1;
  }
  int col = add_pos + (mapping_type_is_b ? 4 : 0);
  // ASN.1 ENUM, not a symbol index: pos2 = 0, pos3 = 1 (NR_MIB__dmrs_TypeA_Position_pos2/_pos3),
  // which is what mac->dmrs_TypeA_Position carries into autoconf_css0(). The old `== 2 ? 2 : 3`
  // read it as a symbol number and so mapped pos2 (0) to l0 = 3 -- see the DL twin of this bug
  // fixed 2026-09-07 in blind_fill_dmrs_mask()'s caller.
  const int l0 = (dmrs_typeA_position == NR_ServingCellConfigCommon__dmrs_TypeA_Position_pos3) ? 3 : 2;
  int32_t l_prime;
  int32_t l0_shift;
  if (max_length <= 1) {
    l_prime  = g_table_6_4_1_1_3_3[row][col];
    l0_shift = 1 << l0;
  } else {
    l_prime  = g_table_6_4_1_1_3_4[row][col];
    l0_shift = (1 << l0) | (1 << (l0 + 1));
  }
  if (l_prime < 0) {
    return -1;
  }
  const uint32_t mask = mapping_type_is_b ? (l_prime << start_symbol) : (l_prime | l0_shift);
  const uint32_t allocation = ((1u << num_symbols) - 1u) << start_symbol;
  if (!mask || (mask & ~allocation)) return -1;
  return (int32_t)mask;
}

// Per-field widths actually used by the UL extraction, resolving each override against the
// documented default. Kept in ONE place so nr_pdcch_blind_dci01_size() (which validates a config)
// and nr_pdcch_blind_decode_and_extract_01() (which reads the payload) can never disagree about the
// layout -- the two disagreeing is precisely the bug this reconciliation exists to catch.
typedef struct {
  int carrier_ind, ul_sul, bwp_ind, riv, tda, fh;
  int harq_pid, dai1, dai2, sri, precoding, ant_ports;
  int srs_req, csi_req, cbg, ptrs_dmrs, beta_offset, dmrs_seq_init;
} blind_ul_field_bits_t;

static blind_ul_field_bits_t blind_ul_field_bits(const nr_pdcch_blind_ul_opts_t* opts)
{
  blind_ul_field_bits_t f;
  const double riv_span = ((double)opts->bwp_size * (double)(opts->bwp_size + 1)) / 2.0;
  f.riv = (int)ceil(log2(riv_span));
  // time_domain_assignment: nr_dci_size() uses ceil(log2(tdaList->count)) when a
  // pusch-TimeDomainAllocationList is configured, and 4 (the 16-entry default table) otherwise.
  // Derived from tda_count, never a separate knob -- same rule as the DL path.
  if (opts->tda_count > 0) {
    int b = 0;
    while ((1 << b) < opts->tda_count) {
      b++;
    }
    f.tda = b;
  } else {
    f.tda = 4;
  }
  f.carrier_ind    = pick_bits(opts->carrier_indicator_bits, 0);
  f.ul_sul         = pick_bits(opts->ul_sul_bits, 0);
  f.bwp_ind        = pick_bits(opts->bwp_indicator_bits, 0);
  f.fh             = pick_bits(opts->freq_hopping_bits, 0);
  f.harq_pid       = pick_bits(opts->harq_pid_bits, 4);
  f.dai1           = pick_bits(opts->dai1_bits, 2);
  f.dai2           = pick_bits(opts->dai2_bits, 0);
  f.sri            = pick_bits(opts->sri_bits, 0);
  f.precoding      = pick_bits(opts->precoding_info_bits, 0);
  f.ant_ports      = pick_bits(opts->antenna_ports_bits, 2);
  f.srs_req        = pick_bits(opts->srs_request_bits, 2);
  f.csi_req        = pick_bits(opts->csi_request_bits, 0);
  f.cbg            = pick_bits(opts->cbg_bits, 0);
  f.ptrs_dmrs      = pick_bits(opts->ptrs_dmrs_bits, 0);
  f.beta_offset    = pick_bits(opts->beta_offset_bits, 0);
  f.dmrs_seq_init  = pick_bits(opts->dmrs_seq_init_bits, 1);
  return f;
}

uint16_t nr_pdcch_blind_dci01_size(const nr_pdcch_blind_ul_opts_t* opts)
{
  if (opts == NULL || opts->bwp_size < 1) {
    return 0;
  }
  const blind_ul_field_bits_t f = blind_ul_field_bits(opts);
  // Constant-width fields: format identifier (1) + MCS (5) + NDI (1) + RV (2) + TPC (2)
  // + UL-SCH indicator (1) = 12. Everything else is RRC-derived.
  return (uint16_t)(12 + f.carrier_ind + f.ul_sul + f.bwp_ind + f.riv + f.tda + f.fh + f.harq_pid
                    + f.dai1 + f.dai2 + f.sri + f.precoding + f.ant_ports + f.srs_req + f.csi_req
                    + f.cbg + f.ptrs_dmrs + f.beta_offset + f.dmrs_seq_init);
}

/// Resolve the PUSCH time-domain allocation. `mu` is the numerology, needed for k2's j offset when
/// the default table applies. Returns false when the index is past the configured list -- on a
/// 2-entry list that rejects 14 of 16 code points, which is a strong plausibility check in itself.
static bool blind_ul_tda(const nr_pdcch_blind_ul_opts_t* opts,
                         uint32_t idx,
                         uint8_t  mu,
                         uint8_t* S,
                         uint8_t* L,
                         uint8_t* mapping_is_b,
                         uint8_t* k2)
{
  if (opts->tda_count > 0) {
    if ((int)idx >= opts->tda_count) {
      return false;
    }
    *S            = opts->tda_start[idx];
    *L            = opts->tda_length[idx];
    *mapping_is_b = opts->tda_mapping[idx] ? 1 : 0;
    *k2           = opts->tda_k2[idx];
  } else {
    if (idx >= 16) {
      return false;
    }
    *mapping_is_b = g_table_6_1_2_1_1_2[idx][0] ? 1 : 0;
    *S            = g_table_6_1_2_1_1_2[idx][2];
    *L            = g_table_6_1_2_1_1_2[idx][3];
    *k2           = (uint8_t)(g_table_6_1_2_1_1_2[idx][1] + g_ul_tda_j[(mu < 6) ? mu : 1]);
  }
  if (*L < 1 || (int)(*S) + (int)(*L) > 14) {
    return false;
  }
  return true;
}

/// Shared tail: everything both 0_1 and 0_0 do once their own field walk has produced the common
/// quantities. Factored out for the same reason blind_polar_decode()/blind_mismatched_bits() were:
/// so the two formats cannot drift apart on the parts that are genuinely identical.
///
/// `force_add_pos` is -1 for format 0_1 (use the deployment's configured dmrs-AdditionalPosition)
/// and 2 for format 0_0 with mapping type A, where TS 38.214 6.2.2 fixes it at pos2 REGARDLESS of
/// any dedicated DMRS-UplinkConfig. That is the uplink twin of the rule the DL path documents for
/// 1_0, and it is silently wrong rather than loudly wrong if inherited from the 0_1 config: the
/// front-loaded DM-RS symbol lands in the same place either way, so a DM-RS-only tap still works
/// while every additional-DM-RS symbol -- and hence the data-RE set and G -- is wrong.
static bool blind_ul_finish(const nr_pdcch_blind_ul_opts_t* opts,
                            uint32_t riv,
                            uint32_t tda_idx,
                            uint32_t mcs,
                            uint32_t antenna_ports,
                            uint32_t nrOfLayers,
                            int      force_add_pos,
                            nr_pdcch_blind_ul_result_t* out)
{
  if (opts->numerology > 5 || opts->dmrs_typeA_position > 1) {
    out->reject_reason = "invalid measured UL numerology or MIB DMRS position";
    return false;
  }
  uint16_t start_rb, num_rb;
  if (!riv_to_prb_alloc(riv, opts->bwp_size, &start_rb, &num_rb)) {
    out->reject_reason = "RIV decodes to a PRB allocation outside the UL BWP";
    return false;
  }
  const int table = opts->mcs_table < 0 ? 0 : opts->mcs_table;
  if (table > 4 || mcs > 31 || nr_get_code_rate_ul(mcs, table) == 0) {
    out->reject_reason = "UL MCS reserved or invalid in the selected table";

    return false;
  }

  uint8_t S, L, mapping_is_b, k2;
  if (!blind_ul_tda(opts, tda_idx, opts->numerology,
                    &S, &L, &mapping_is_b, &k2)) {
    out->reject_reason = "time-domain assignment index past the pusch-TimeDomainAllocationList";
    return false;
  }

  // Mapping type B keeps the dedicated value even under format 0_0 -- the pos2 rule is scoped to
  // type A (TS 38.214 6.2.2), exactly as the DL 1_0 path scopes its own.
  const int add_pos = (force_add_pos >= 0 && !mapping_is_b)
                          ? force_add_pos
                          : ((opts->dmrs_add_pos >= 0) ? opts->dmrs_add_pos : 2);
  const int max_len = (opts->dmrs_max_length > 0) ? opts->dmrs_max_length : 1;
  const int32_t mask = nr_pdcch_blind_ul_dmrs_mask(L, S, mapping_is_b, add_pos, max_len, opts->dmrs_typeA_position);
  if (mask < 0) {
    out->reject_reason = "no valid PUSCH DM-RS position for this allocation length";
    return false;
  }

  // Antenna ports -> (CDM groups without data, port bitmask). Closed form rather than a table,
  // copied from mac_tables.c's ul_ports_config() for the transform-precoder-disabled / dmrs-type1 /
  // maxLength1 / rank-1 case (TS 38.212 Table 7.3.1.1.2-8), which is the only combination this
  // deployment produces. Verified against the live gNB 2026-08-25: it logs `ant=2` on every UL DCI
  // and dumps `num_dmrs_cdm_grps_no_data=2 dmrs_ports=1`, which is exactly what val=2 gives here.
  /* `antenna_ports` is a RAW payload field, and the width sweep tries 2..5 bits for it, so values
   * up to 31 reach this point. The closed form below is defined ONLY over Table 7.3.1.1.2-8's four
   * rows (transform precoder disabled, DM-RS type 1, maxLength 1, rank 1) -- the sole combination
   * this path supports and the only one its caller emits. Outside that domain it produces nonsense.
   * Measured 2026-09-09: antenna_ports=14 yields 1u<<12, a port bitmap with no port below 12, which
   * AssertFatal()s inside get_dmrs_port() ("No dmrs port corresponding to layer 0 found") and
   * killed the entire softmodem mid-capture. Values above 17 are worse still -- they truncate to 0
   * in a uint16_t and read silently as "DCI 1_0, port 0".
   * A blind decoder must REJECT a code point it cannot interpret, never abort and never guess. */
  if (antenna_ports > 3) {
    out->reject_reason = "antenna-ports code point outside Table 7.3.1.1.2-8's four rows";
    return false;
  }
  uint8_t  cdm_groups;
  uint16_t ports;
  if (opts->transform_precoding == 1) {
    cdm_groups = 2;
    ports      = (uint16_t)(1u << antenna_ports);
  } else if (nrOfLayers <= 1) {
    cdm_groups = (antenna_ports > 1) ? 2 : 1;
    ports      = (uint16_t)(1u << ((antenna_ports > 1) ? (antenna_ports - 2) : antenna_ports));
  } else {
    // Multi-layer UL is out of scope: this deployment schedules num_layers=1 and the passive
    // receiver has no way to separate UE layers it was not precoded for. Reject rather than
    // produce a confident wrong port set.
    out->reject_reason = "multi-layer PUSCH not supported by this monitor";
    return false;
  }

  /* The DM-RS port bitmap must actually name a port for every layer, within 0..11. get_dmrs_port()
   * AssertFatal()s otherwise -- it is gNB code, written for a scheduler that only ever hands it its
   * own valid configuration -- and an AssertFatal on a blind receive path kills the softmodem over
   * a hypothesis that was merely wrong. Measured 2026-09-09: a length-45 UE (0x461e) reached a
   * width hypothesis whose antenna-ports code point produced a bitmap with no bit below 12, and the
   * run aborted with "No dmrs port corresponding to layer 0 found". Those UEs had never emitted a
   * grant before -- they used to overflow the class cap -- so this landmine was reachable all along
   * and simply never stepped on. Same rule as nr_pdcch_blind_ul_dmrs_mask(): a blind decoder
   * REJECTS an implausible candidate, it does not abort. */
  {
    const int layers = (nrOfLayers < 1) ? 1 : nrOfLayers;
    int usable = 0;
    for (int i = 0; i < 12; i++) {
      if ((ports >> i) & 1) {
        usable++;
      }
    }
    if (ports != 0 && usable < layers) {
      out->reject_reason = "DM-RS port bitmap names no usable port in 0..11 for every layer";
      return false;
    }
  }

  out->start_rb          = start_rb;
  out->num_rb            = num_rb;
  out->bwp_start         = opts->bwp_start;
  out->bwp_size          = opts->bwp_size;
  out->tda_index         = (uint8_t)tda_idx;
  out->start_symbol      = S;
  out->num_symbols       = L;
  out->mapping_type      = mapping_is_b;
  out->k2                = k2;
  out->mcs               = (uint8_t)mcs;
  out->mcs_table         = (uint8_t)((opts->mcs_table >= 0) ? opts->mcs_table : 0);
  out->nrOfLayers        = (uint8_t)((nrOfLayers < 1) ? 1 : nrOfLayers);
  out->ul_dmrs_symb_pos  = (uint16_t)mask;
  out->dmrs_config_type  = (uint8_t)((opts->dmrs_config_type > 0) ? 1 : 0);
  out->n_dmrs_cdm_groups = cdm_groups;
  out->dmrs_ports        = ports;
  out->antenna_ports_field = (uint8_t)antenna_ports;
  out->transform_precoding = (uint8_t)((opts->transform_precoding == 1) ? 1 : 0);
  out->data_scrambling_id  = (uint16_t)((opts->data_scrambling_id >= 0) ? opts->data_scrambling_id : opts->phy_cell_id);
  out->ul_dmrs_scrambling_id =
      (uint16_t)((opts->ul_dmrs_scrambling_id >= 0) ? opts->ul_dmrs_scrambling_id : opts->phy_cell_id);
  out->plausible     = true;
  out->reject_reason = NULL;
  return true;
}

bool nr_pdcch_blind_decode_01_mode(bool automatic, const int16_t *llr, uint8_t aggregation_level,
                                   uint16_t dci_length, const nr_pdcch_blind_ul_opts_t *opts,
                                   uint16_t rnti_min, uint16_t rnti_max,
                                   nr_pdcch_blind_ul_result_t *out)
{
  return automatic ? nr_pdcch_blind_decode_raw_01(llr,aggregation_level,dci_length,rnti_min,rnti_max,out)
                   : nr_pdcch_blind_decode_and_extract_01(llr,aggregation_level,dci_length,opts,rnti_min,rnti_max,out);
}

bool nr_pdcch_blind_decode_raw_01(const int16_t* llr,
                                          uint8_t        aggregation_level,
                                          uint16_t       dci_length,
                                          uint16_t       rnti_min,
                                          uint16_t       rnti_max,
                                          nr_pdcch_blind_ul_result_t* out)
{
  if (!out) return false;
  memset(out, 0, sizeof(*out));
  out->width_hyp_class = out->interp_hyp_class = -1;
  out->plausible      = false;
  out->ul_dci_format  = NR_BLIND_UL_DCI_FORMAT_0_1;
  out->dci_length     = dci_length;

  if (!llr || dci_length == 0 || dci_length > 63 || rnti_min == 0 || rnti_min > rnti_max ||
      (aggregation_level != 1 && aggregation_level != 2 && aggregation_level != 4 &&
       aggregation_level != 8 && aggregation_level != 16)) {
    out->reject_reason = "invalid dci_length/opts argument";
    return false;
  }

  // ---- Step 1: RNTI-independent polar decode, shared verbatim with both DL entry points. ----
  uint64_t       dci_estimation[2] = {0};
  const uint32_t crc = blind_polar_decode(llr, aggregation_level, dci_length, rnti_min, rnti_max, dci_estimation);

  // raw_payload/crc_rnti are filled BEFORE any plausibility check: they are the reconciliation
  // instrument (see the header), and a rejected payload is exactly the case worth dumping while a
  // width assignment is still being pinned against the gNB's own log.
  out->raw_payload = dci_estimation[0];
  out->crc_rnti    = (uint16_t)crc;

  if (crc < rnti_min || crc > rnti_max) {
    out->reject_reason = "CRC-recovered value outside plausible RNTI range";
    return false;
  }
  out->rnti = (uint16_t)crc;
  out->mismatched_bits = blind_mismatched_bits(llr, dci_estimation, crc, aggregation_level, dci_length);

  if ((out->raw_payload >> (dci_length - 1)) & 1) {
    out->reject_reason = "format indicator=1 (DL assignment, not an UL grant)";
    return false;
  }
  return true;
}

bool nr_pdcch_blind_decode_and_extract_01(const int16_t *llr, uint8_t aggregation_level,
                                        uint16_t dci_length, const nr_pdcch_blind_ul_opts_t *opts,
                                        uint16_t rnti_min, uint16_t rnti_max,
                                        nr_pdcch_blind_ul_result_t *out)
{
  if (!nr_pdcch_blind_decode_raw_01(llr, aggregation_level, dci_length, rnti_min, rnti_max, out))
    return false;
  const uint16_t mismatch = out->mismatched_bits;
  const bool ok = nr_pdcch_blind_extract_01(out->raw_payload, dci_length, out->rnti, opts, out);
  out->mismatched_bits = mismatch;
  return ok;
}

bool nr_pdcch_blind_extract_01(uint64_t payload, uint16_t dci_length, uint16_t rnti,
                             const nr_pdcch_blind_ul_opts_t *opts, nr_pdcch_blind_ul_result_t *out)
{
  if (!out) return false;
  memset(out, 0, sizeof(*out));
  out->width_hyp_class = out->interp_hyp_class = -1;
  out->raw_payload = payload;
  out->dci_length = dci_length;
  out->rnti = out->crc_rnti = rnti;
  out->ul_dci_format = NR_BLIND_UL_DCI_FORMAT_0_1;
  if (!opts || opts->bwp_size < 1 || opts->bwp_size > 275 || opts->tda_count < 0 ||
      opts->tda_count > 16 || dci_length == 0 || dci_length > 63) {
    out->reject_reason = "invalid UL payload/opts";
    return false;
  }
  const blind_ul_field_bits_t f = blind_ul_field_bits(opts);
  if (nr_pdcch_blind_dci01_size(opts) > dci_length) {
    out->reject_reason = "configured UL DCI field widths exceed dci_length";
    return false;
  }

  // ---- Step 2: field walk, in fill_dci_pdu_rel15()'s NR_UL_DCI_FORMAT_0_1 PACKER order.
  // That order is deliberately NOT nr_dci_size()'s accumulation order -- the two differ (size adds
  // the HARQ process before the carrier indicator, the packer emits the carrier indicator first).
  // Totals agree either way; OFFSETS follow the packer, and offsets are what a decoder needs. ----
  int            pos     = (int)dci_length;

  const uint32_t format_indicator = read_field(payload, &pos, 1);
  out->carrier_indicator = read_field(payload, &pos, f.carrier_ind);
  out->ul_sul_indicator = read_field(payload, &pos, f.ul_sul);
  const uint32_t bwp_indicator = read_field(payload, &pos, f.bwp_ind);
  const uint32_t riv           = read_field(payload, &pos, f.riv);
  const uint32_t tda_idx       = read_field(payload, &pos, f.tda);
  const uint32_t freq_hopping  = read_field(payload, &pos, f.fh);
  const uint32_t mcs           = read_field(payload, &pos, 5);
  const uint32_t ndi           = read_field(payload, &pos, 1);
  const uint32_t rv            = read_field(payload, &pos, 2);
  const uint32_t harq_pid      = read_field(payload, &pos, f.harq_pid);
  const uint32_t dai1          = read_field(payload, &pos, f.dai1);
  (void)read_field(payload, &pos, f.dai2);
  const uint32_t tpc           = read_field(payload, &pos, 2);
  (void)read_field(payload, &pos, f.sri);
  const uint32_t precoding     = read_field(payload, &pos, f.precoding);
  const uint32_t antenna_ports = read_field(payload, &pos, f.ant_ports);
  const uint32_t srs_request   = read_field(payload, &pos, f.srs_req);
  const uint32_t csi_request   = read_field(payload, &pos, f.csi_req);
  (void)read_field(payload, &pos, f.cbg);
  (void)read_field(payload, &pos, f.ptrs_dmrs);
  (void)read_field(payload, &pos, f.beta_offset);
  const uint32_t dmrs_seq_init = read_field(payload, &pos, f.dmrs_seq_init);
  const uint32_t ulsch_ind     = read_field(payload, &pos, 1);

  // ---- Step 3: plausibility. ----
  if (format_indicator != 0) {
    out->reject_reason = "format indicator=1 (DL assignment, not an UL grant)";
    return false;
  }
  // UL-SCH indicator 0 means the grant carries CSI ONLY and no transport block (TS 38.212
  // 7.3.1.1.2). It is a real, correctly-decoded grant -- but there is no PUSCH data to extract or
  // decode, and its k2 is replaced by the CSI report's own reportSlotOffset, which is not in the
  // payload. Rejecting it here keeps a downstream consumer from being handed a slot number that
  // was never derived.
  if (ulsch_ind == 0) {
    out->reject_reason = "UL-SCH indicator=0 (CSI-only grant, carries no PUSCH data)";
    return false;
  }

  out->bwp_indicator          = (uint8_t)bwp_indicator;
  out->freq_domain_assignment = riv;
  out->frequency_hopping      = (uint8_t)freq_hopping;
  out->rv                     = (uint8_t)rv;
  out->ndi                    = (uint8_t)ndi;
  out->harq_pid               = (uint8_t)harq_pid;
  out->nscid                  = (uint8_t)dmrs_seq_init;
  out->tpc                    = (uint8_t)tpc;
  out->dai                    = (uint8_t)dai1;
  out->srs_request            = (uint8_t)srs_request;
  out->csi_request            = (uint8_t)csi_request;
  out->precoding_info         = (uint8_t)precoding;
  out->ulsch_indicator        = (uint8_t)ulsch_ind;

  // nrOfLayers from the precoding-information field. With that field 0 bits wide -- a single UE
  // transmit port, which is what this deployment's rank-1 UL produces -- rank is 1 BY DEFINITION
  // and there is nothing to resolve.
  //
  // When the field IS present, mapping its code point to (nrOfLayers, TPMI) needs TS 38.212
  // Table 7.3.1.1.2-2..5, selected by the UE's SRS port count, maxRank and codebookSubset -- all of
  // which live in a PUSCH-Config this receiver cannot read. Rejecting is the honest outcome:
  // assuming rank 1 anyway would produce a confident wrong DM-RS port set and a wrong TBS on every
  // multi-layer grant, which is the failure mode this module has been bitten by twice already.
  uint32_t layers = 1;
  if (f.precoding != 0 && precoding != 0) {
    /* A NON-ZERO code point needs TS 38.212 Tables 7.3.1.1.2-2..5, selected by the UE's SRS port
     * count, maxRank and codebookSubset -- all in a PUSCH-Config this receiver cannot read.
     * Rejecting is the honest outcome; guessing would give a confident wrong DM-RS port set and a
     * wrong TBS on every multi-layer grant. */
    out->reject_reason = "non-zero precoding code point: layer count needs a PUSCH-Config this receiver cannot read";
    return false;
  }
  /* Code point 0, however, IS resolvable with no config at all: it is "1 layer, TPMI 0" in EVERY
   * one of those four tables, whatever the antenna-port count, maxRank or codebookSubset. So a
   * present-but-zero precoding field costs nothing to accept.
   *
   * This mattered: the first live capture with the pinned layout rejected 18514 of 18614 correctly
   * decoded UL grants here, because the guard keyed on the field EXISTING rather than on its VALUE
   * -- and this cell logs mimo=0 on 100 % of its UL DCIs. The decoded rv/tpc/ulsch/dai/h_id/ndi on
   * those rejected grants already matched the gNB exactly, i.e. everything upstream was right. */
  return blind_ul_finish(opts, riv, tda_idx, mcs, antenna_ports, layers,
                         -1 /* 0_1 uses the configured dmrs-AdditionalPosition */, out);
}

bool nr_pdcch_blind_extract_00(uint64_t       payload,
                               uint16_t       dci_length,
                               uint16_t       crc_rnti,
                               const nr_pdcch_blind_ul_opts_t* opts,
                               nr_pdcch_blind_ul_result_t* out)
{
  memset(out, 0, sizeof(*out));
  out->width_hyp_class = out->interp_hyp_class = -1;
  out->plausible     = false;
  out->ul_dci_format = NR_BLIND_UL_DCI_FORMAT_0_0;
  out->dci_length    = dci_length;
  out->raw_payload   = payload;
  out->crc_rnti      = crc_rnti;
  out->rnti          = crc_rnti;

  if (opts == NULL || dci_length == 0 || dci_length > 63 || opts->bwp_size < 1) {
    out->reject_reason = "invalid dci_length/opts argument";
    return false;
  }

  // Format 0_0's field list is spec-fixed -- unlike 0_1 there are no RRC-derived widths at all
  // (TS 38.212 7.3.1.1.1): identifier 1, FDRA = RIV, TDA always 4, hopping flag 1, MCS 5, NDI 1,
  // RV 2, HARQ 4, TPC 2. That is what makes this branch essentially free, and it is also why
  // nothing here consults blind_ul_field_bits().
  const double   riv_span = ((double)opts->bwp_size * (double)(opts->bwp_size + 1)) / 2.0;
  const int      riv_bits = (int)ceil(log2(riv_span));
  const uint16_t need     = (uint16_t)(20 + riv_bits);
  if (need > dci_length) {
    out->reject_reason = "DCI 0_0 field list exceeds dci_length";
    return false;
  }

  int pos = (int)dci_length;
  const uint32_t format_indicator = read_field(payload, &pos, 1);
  const uint32_t riv              = read_field(payload, &pos, riv_bits);
  const uint32_t tda_idx          = read_field(payload, &pos, 4);
  const uint32_t freq_hopping     = read_field(payload, &pos, 1);
  const uint32_t mcs              = read_field(payload, &pos, 5);
  const uint32_t ndi              = read_field(payload, &pos, 1);
  const uint32_t rv               = read_field(payload, &pos, 2);
  const uint32_t harq_pid         = read_field(payload, &pos, 4);
  const uint32_t tpc              = read_field(payload, &pos, 2);

  if (format_indicator != 0) {
    out->reject_reason = "format indicator=1 (this is DCI 1_0, not 0_0)";
    return false;
  }
  // Size-alignment padding. TS 38.212 7.3.1.0 zero-pads whichever of 0_0/1_0 is smaller in a
  // UE-specific search space, so any excess MUST be zero -- the same check the 1_0 path already
  // applies, and a cheap false-accept discriminator.
  if (pos > 0 && (payload & ((1ULL << pos) - 1ULL)) != 0) {
    out->reject_reason = "DCI 0_0 size-alignment padding is non-zero";
    return false;
  }

  out->freq_domain_assignment = riv;
  out->frequency_hopping      = (uint8_t)freq_hopping;
  out->rv                     = (uint8_t)rv;
  out->ndi                    = (uint8_t)ndi;
  out->harq_pid               = (uint8_t)harq_pid;
  out->tpc                    = (uint8_t)tpc;
  out->ulsch_indicator        = 1; // 0_0 always schedules UL-SCH; there is no indicator field
  // DCI 0_0 carries no antenna-ports field. TS 38.214 6.2.2: port 0, and the CDM-group count is
  // derived from the allocation length exactly as the DL 1_0 path derives its own
  // (numDmrsCdmGrpsNoData = 1 for a 2-symbol allocation, 2 otherwise). blind_ul_finish() gets the
  // code point that reproduces that: val 0 -> 1 group/port 0, val 2 -> 2 groups/port 0.
  // The length is not known until the TDRA is resolved, so pass val=2 (the ordinary case) and
  // correct it below for the 2-symbol one.
  if (!blind_ul_finish(opts, riv, tda_idx, mcs, 2 /* 2 CDM groups, port 0 */, 1,
                       2 /* TS 38.214 6.2.2: pos2 for 0_0 mapping type A */, out)) {
    return false;
  }
  if (out->num_symbols <= 2) {
    out->n_dmrs_cdm_groups   = 1;
    out->antenna_ports_field = 0;
  }
  out->nscid = 0; // TS 38.211 6.4.1.1.1: n_SCID = 0 for a DCI 0_0 scheduled PUSCH
  return true;
}

int nr_pdcch_blind_dl_layout_candidates(const nr_pdcch_blind_raw_result_t *raw,
                                        uint16_t len, uint16_t bwp, uint8_t typeA,
                                        nr_pdcch_blind_result_t out[3], uint8_t ids[3])
{
  if (!raw || !out || !ids || !bwp || bwp>275 || !len || len>63) return 0;
  int count=0;
  /* Initial supported profile: type-1 RA, one codeword, type-1/len1 port table,
   * no cross-carrier/optional rate-matching fields. These are hypotheses, NOT
   * learned RRC facts. Unknown BWP-indicator and TDA-index widths are enumerated.
   * TB CRC remains the authority; unsupported layouts stay unresolved. */
  for (int bw=0; bw<=2; ++bw) {
    for (int td=0; td<=4; ++td) {
      nr_pdcch_blind_extract_opts_t o={0};
      o.bwp_indicator_bits=bw;
      o.harq_pid_bits=4;
      o.dai_bits=2;
      o.pdsch_to_harq_bits=3;
      o.antenna_ports_bits=4;
      o.srs_request_bits=2;
      o.tda_count=1<<td;
      o.dmrs_add_pos=0;
      o.dmrs_max_length=1;
      if (nr_pdcch_blind_dci_size_ex(bwp,&o)!=len) continue;
      /* S/L is deliberately a legal parser scaffold, never an applied hypothesis:
       * runtime MUST replace it through Technique D before decoding a transport block.
       * The inferred index width is not a claim about the dedicated list's entry count. */
      for (int i=0;i<o.tda_count;++i) {
        o.tda_start[i]=1; o.tda_length[i]=13; o.tda_mapping[i]=0;
      }
      nr_pdcch_blind_result_t parsed;
      if (!nr_pdcch_blind_extract_11(raw,len,bwp,typeA,&o,&parsed)) continue;
      if (count==3) return 0; // at most one TDA width per BWP width at an exact length
      out[count]=parsed;
      ids[count++]=(uint8_t)(bw*5+td);
    }
  }
  if (count) return count;

  /* ---- OPTIONAL-FIELD FALLBACK  (agnosticity #6) ---------------------------------------------
   * The profile above fixes every optional DCI-1_1 field at its "not configured" width. When that
   * explains the observed payload length, it is the answer and we never get here -- so this pass
   * cannot change the behaviour of a deployment that already works. It runs ONLY when NO standard
   * layout reproduces the length, which is exactly the case the ledger called "general
   * optional-field/BWP/type-0 support missing": a gNB that configures dynamic PRB bundling,
   * rate-matching groups, ZP-CSI-RS or VRB-to-PRB interleaving carries extra bits the standard
   * profile cannot account for, and today that DCI is simply unexplainable.
   * Widths are the legal alternatives from TS 38.212 7.3.1.2.2, and the exact-length filter plus
   * the TB-CRC authority downstream remain the arbiter -- these are hypotheses, not learned facts.
   * Ambiguity is still refused (>3 surviving layouts return 0) rather than guessed. */
  static const int kPrbBundling[2] = {0, 1};
  static const int kRateMatch[3]   = {0, 1, 2};
  static const int kZpCsi[3]       = {0, 1, 2};
  static const int kVrbToPrb[2]    = {0, 1};
  for (int bw=0; bw<=2; ++bw) {
    for (int td=0; td<=4; ++td) {
      for (unsigned pb=0; pb<2; ++pb) {
        for (unsigned rm=0; rm<3; ++rm) {
          for (unsigned zp=0; zp<3; ++zp) {
            for (unsigned vp=0; vp<2; ++vp) {
              if (!kPrbBundling[pb] && !kRateMatch[rm] && !kZpCsi[zp] && !kVrbToPrb[vp])
                continue; // already covered by the standard pass above
              nr_pdcch_blind_extract_opts_t o={0};
              o.bwp_indicator_bits=bw;
              o.harq_pid_bits=4;
              o.dai_bits=2;
              o.pdsch_to_harq_bits=3;
              o.antenna_ports_bits=4;
              o.srs_request_bits=2;
              o.tda_count=1<<td;
              o.dmrs_add_pos=0;
              o.dmrs_max_length=1;
              o.prb_bundling_bits=kPrbBundling[pb];
              o.rate_matching_bits=kRateMatch[rm];
              o.zp_csirs_bits=kZpCsi[zp];
              o.vrb_to_prb_bits=kVrbToPrb[vp];
              if (nr_pdcch_blind_dci_size_ex(bwp,&o)!=len) continue;
              for (int i=0;i<o.tda_count;++i) { o.tda_start[i]=1; o.tda_length[i]=13; o.tda_mapping[i]=0; }
              nr_pdcch_blind_result_t parsed;
              if (!nr_pdcch_blind_extract_11(raw,len,bwp,typeA,&o,&parsed)) continue;
              if (count==3) return 0; // ambiguous optional-field layouts are not a unique solution
              out[count]=parsed;
              ids[count++]=(uint8_t)(64 + ((bw*5+td)&0x1f));
            }
          }
        }
      }
    }
  }
  return count;
}

static pthread_mutex_t common_facts_lock=PTHREAD_MUTEX_INITIALIZER;
static nr_pdcch_blind_common_config_t common_facts;
static bool common_facts_valid;
static bool common_tda_valid(int count, const uint8_t *start, const uint8_t *length,
                             const uint8_t *mapping)
{
  if (count<0 || count>16) return false;
  for(int i=0;i<count;++i)
    if (!length[i] || start[i]+length[i]>14 || mapping[i]>1) return false;
  return true;
}
static bool sib1_cache_suppressed;
static void sib1_cache_store(const nr_pdcch_blind_common_config_t *f);
bool nr_pdcch_blind_publish_common(const nr_pdcch_blind_common_config_t *f)
{
  if (!f || f->pci>1007 || !f->dl_bwp_size || f->dl_bwp_start+f->dl_bwp_size>275
      || f->ul_bwp_start+f->ul_bwp_size>275 || f->dl_mu>4 || f->ul_mu>4
      || !common_tda_valid(f->dl_count,f->dl_start,f->dl_length,f->dl_mapping)
      || !common_tda_valid(f->ul_count,f->ul_start,f->ul_length,f->ul_mapping))
    return false;
  pthread_mutex_lock(&common_facts_lock);
  const bool changed=!common_facts_valid || memcmp(&common_facts,f,sizeof(*f));
  common_facts=*f;
  common_facts_valid=true;
  sib1_cache_suppressed=false;
  pthread_mutex_unlock(&common_facts_lock);
  if(changed)
    sib1_cache_store(f);
  if(changed)
    LOG_I(PHY,"PASSIVE: SIB1 common facts PCI=%u DL-BWP=%u+%u DL-TDAs=%u "
              "UL-BWP=%u+%u UL-TDAs=%u; dedicated config remains a hypothesis\n",
          f->pci,f->dl_bwp_start,f->dl_bwp_size,f->dl_count,f->ul_bwp_start,f->ul_bwp_size,f->ul_count);
  /* No DL seed from SIB1's common TDRA list: the dedicated list is an RRC switch the receiver
   * cannot read, so the DCI 1_1 TDA width is SEARCHED (nr_dci11_resolver_init, NR_DCI11_TDA_UNKNOWN)
   * and the S/L per index come from Technique D. The UL seed above the RT tap is unchanged. */
  return true;
}
/* SIB1 facts cache, per PCI. OAI's own SI-RNTI path decodes SIB1 on about half of the X410
 * acquisitions (2026-09-15: 0 hits in 31k CORESET#0 candidates with PBCH at 50/50, on the same cell
 * that decoded SIB1 at once one attempt earlier). A real receiver keeps SIB1 per cell too. The
 * cached facts are loaded only when the live ones are absent, are logged as CACHED, and stay what
 * they always were: a hypothesis the TB CRC judges. ISAC_SIB1_CACHE=0 disables; the path is
 * ISAC_SIB1_CACHE_DIR (default /tmp/passive_rx). */
static void sib1_cache_path(uint16_t pci, char *out, size_t n)
{
  const char *dir = getenv("ISAC_SIB1_CACHE_DIR");
  snprintf(out, n, "%s/sib1_common_pci%u.bin", dir && dir[0] ? dir : "/tmp/passive_rx", pci);
}
static bool sib1_cache_enabled(void)
{
  const char *e = getenv("ISAC_SIB1_CACHE");
  return e == NULL || atoi(e) != 0;
}
static void sib1_cache_store(const nr_pdcch_blind_common_config_t *f)
{
  if (!sib1_cache_enabled()) return;
  char path[256];
  sib1_cache_path(f->pci, path, sizeof(path));
  FILE *fp = fopen(path, "wb");
  if (fp == NULL) { mkdir("/tmp/passive_rx", 0777); fp = fopen(path, "wb"); }
  if (fp == NULL) return;
  const uint32_t magic = 0x53494231u; /* "SIB1" */
  fwrite(&magic, sizeof(magic), 1, fp);
  fwrite(f, sizeof(*f), 1, fp);
  fclose(fp);
}
static bool sib1_cache_load(uint16_t pci, nr_pdcch_blind_common_config_t *f)
{
  if (!sib1_cache_enabled()) return false;
  char path[256];
  sib1_cache_path(pci, path, sizeof(path));
  FILE *fp = fopen(path, "rb");
  if (fp == NULL) return false;
  uint32_t magic = 0;
  const bool ok = fread(&magic, sizeof(magic), 1, fp) == 1 && magic == 0x53494231u
                  && fread(f, sizeof(*f), 1, fp) == 1 && f->pci == pci;
  fclose(fp);
  return ok;
}
bool nr_pdcch_blind_get_common(uint16_t pci, nr_pdcch_blind_common_config_t *f)
{
  if (!f) return false;
  pthread_mutex_lock(&common_facts_lock);
  bool ok=common_facts_valid && common_facts.pci==pci;
  if(ok) *f=common_facts; else memset(f,0,sizeof(*f));
  pthread_mutex_unlock(&common_facts_lock);
  if (!ok && !sib1_cache_suppressed) {
    static uint16_t s_tried_pci = 0xFFFF;
    if (s_tried_pci != pci) {
      s_tried_pci = pci;
      nr_pdcch_blind_common_config_t c;
      if (sib1_cache_load(pci, &c)) {
        LOG_A(PHY, "PASSIVE: SIB1 common facts for PCI %u loaded from CACHE (live SIB1 not decoded yet): "
                   "DL-BWP=%u+%u DL-TDAs=%u UL-BWP=%u+%u UL-TDAs=%u -- hypothesis, TB CRC decides\n",
              pci, c.dl_bwp_start, c.dl_bwp_size, c.dl_count, c.ul_bwp_start, c.ul_bwp_size, c.ul_count);
        if (nr_pdcch_blind_publish_common(&c)) { *f = c; ok = true; }
      }
    }
  }
  return ok;
}
/* sib1_cache_suppressed: a reset means FORGET; the on-disk cache is not reloaded until a new publish. */
void nr_pdcch_blind_reset_common(void)
{
  pthread_mutex_lock(&common_facts_lock);
  common_facts_valid=false;
  memset(&common_facts,0,sizeof(common_facts));
  sib1_cache_suppressed = true;
  pthread_mutex_unlock(&common_facts_lock);
}
