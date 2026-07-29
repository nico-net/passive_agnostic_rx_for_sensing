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

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c
 * \brief RT receive-path tap for the blind PDCCH monitor (Phase 3 live wiring, Stage 1 of
 * /home/sens/.claude/plans/zesty-baking-thompson.md). See nr_pdcch_blind_monitor_rt.h for why this
 * is a separate translation unit from nr_pdcch_blind_monitor.c (the offline-tested pure decode
 * core + config parser): this file calls into PHY_NR_UE (nr_slot_fep, nr_pdcch_generate_llr,
 * nr_pdsch_channel_estimation) and NR_UE_ISAC (nr_isac_submit_cfr), so it is compiled into
 * PHY_NR_UE_SRC rather than the lean, offline-gtest-linked nr_pdcch_blind_monitor library.
 *
 * Builds a fully local, single-search-space nr_phy_data_t/fapi_nr_dl_config_dci_dl_pdu_rel15_t
 * every call, straight from [sensing] pdcch_blind_monitor_* config (via
 * nr_pdcch_blind_monitor_get_cfg()) -- NEVER the real MAC-driven phy_pdcch_config the caller's own
 * phy_data carries (that instance is untouched; see the call site in executables/nr-ue.c). This is
 * what makes the tap safe to run even in --passive-rx's UE_RECEIVING_SIB state, where the real
 * phy_pdcch_config.nb_search_space is always 0.
 */

#include "PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.h"
#include "PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.h"

#include <string.h>
#include <time.h> // clock_gettime for the rnti_seen correlation line below

#include "common/utils/LOG/log.h"
#include "common/utils/nr/nr_common.h"

#include "PHY/NR_UE_TRANSPORT/nr_transport_proto_ue.h" // nr_pdcch_demapping_deinterleaving/_unscrambling/_generate_llr
#include "PHY/MODULATION/modulation_UE.h"               // nr_slot_fep
#include "PHY/NR_UE_ESTIMATION/nr_estimation.h"          // nr_pdsch_channel_estimation
#include "PHY/TOOLS/tools_defs.h"                        // allocCast2D/fourDimArray_t
#include "PHY/NR_UE_ISAC/nr_isac.h"                      // nr_isac_submit_cfr/_enabled/_source_enabled
#include "nfapi/open-nFAPI/nfapi/public_inc/fapi_nr_ue_constants.h" // FAPI_NR_CCE_REG_MAPPING_TYPE_*

#define NR_PDCCH_BLIND_RE_PER_RB_OUT_DMRS 9 // == dci_nr.c's file-local RE_PER_RB_OUT_DMRS #define
#define NR_PDCCH_BLIND_AL2_MAX_CANDIDATES 8 // scan every non-overlapping AL2 CCE position -- a blind
                                            // receiver does not know which of these the real UE's own
                                            // RNTI hash landed on (unlike this SS's own al2_cand
                                            // config value, which describes ONE known UE's candidate
                                            // count -- see the plan's Stage-1 implementation note)
#define NR_PDCCH_BLIND_MAX_ANT 8 // matches csi_rx.c's NR_ISAC_CSIRS_MAX_ANT -- same reasoning, a
                                 // generous cap on the AoA receive array size this tap will extract

static void build_coreset_bitmap(int num_groups, uint8_t bitmap[6])
{
  memset(bitmap, 0, 6);
  for (int g = 0; g < num_groups && g < 45; g++) {
    bitmap[g / 8] |= (uint8_t)(0x80 >> (g % 8));
  }
}

// Periodic INFO-level summary. The interesting per-candidate detail (LOG_D "blind PDCCH accept")
// is invisible at this project's usual phy_log_level=info -- confirmed live 2026-07-28 during
// Stage 1 validation (empty passive UE log despite the tap running). Every-slot occasions would
// flood at LOG_D's own level anyway, so this is a deliberate low-rate INFO counter, not a
// downgrade of the per-candidate line.
#define NR_PDCCH_BLIND_SUMMARY_PERIOD_OCC 1000
static uint64_t    g_occasions_run  = 0;
static uint64_t    g_candidates_run = 0;
static uint64_t    g_accepts        = 0; // raw plausibility accepts (Step 1-4 of decode_and_extract),
                                         // BEFORE the noise-floor gates below -- unchanged meaning
                                         // from before 2026-07-28's gates, so old logs stay comparable
static const char *g_last_reject_reason = NULL; // TEMPORARY diagnostic, 2026-07-28 root-cause pass
static uint16_t     g_last_reject_rnti  = 0;
static uint64_t g_cfr_submits    = 0; // final count that actually reached the ISAC engine, i.e. after
                                      // ALL gates (raw accept + energy + persistence + SNR)

// ---- Noise-floor gate counters (2026-07-28) -- how many raw accepts each gate held back, so the
// periodic summary shows where candidates are actually being lost, not just the final count. ----
static uint64_t g_held_energy   = 0; // skipped decode entirely, raw LLR energy below energy_min
static uint64_t g_held_persist  = 0; // decoded+accepted but RNTI not yet seen rnti_persist_k times
static uint64_t g_held_snr      = 0; // decoded+accepted+persisted but post-estimation SNR too low

// ---- RNTI persistence tracking (2026-07-28): a real UE's RNTI recurs across many grants; a noise
// accept is a one-off. Small ring buffer of recent (rnti, abs_slot) sightings -- linear scan is fine
// given the raw accept rate is on the order of ~1/s (measured), so the buffer holds at most a few
// seconds of history regardless of window size. See nr_pdcch_blind_monitor_rt.h's rnti_persist_k/
// rnti_persist_window_ms field comments. ----
#define NR_PDCCH_BLIND_PERSIST_MAX 64
static struct {
  uint16_t rnti;
  uint32_t abs_slot;
} g_recent[NR_PDCCH_BLIND_PERSIST_MAX];
static int g_recent_head  = 0;
static int g_recent_count = 0;

// Returns true once `rnti` has been sighted at least `min_k` times (including this one) within the
// last `window_slots` slots. Always records the current sighting regardless of the outcome, so a
// candidate that fails today can contribute toward tomorrow's threshold.
static bool rnti_persistence_check(uint16_t rnti, uint32_t abs_slot, uint32_t window_slots, int min_k)
{
  if (min_k <= 1) {
    return true; // gate disabled -- accept-on-first-sighting, matches pre-2026-07-28 behaviour
  }
  int seen = 0;
  for (int i = 0; i < g_recent_count; i++) {
    if (g_recent[i].rnti == rnti && (abs_slot - g_recent[i].abs_slot) <= window_slots) {
      seen++;
    }
  }
  g_recent[g_recent_head].rnti     = rnti;
  g_recent[g_recent_head].abs_slot = abs_slot;
  g_recent_head                    = (g_recent_head + 1) % NR_PDCCH_BLIND_PERSIST_MAX;
  if (g_recent_count < NR_PDCCH_BLIND_PERSIST_MAX) {
    g_recent_count++;
  }
  return (seen + 1) >= min_k; // +1 counts the sighting just recorded
}

void nr_pdcch_blind_monitor_process(PHY_VARS_NR_UE *ue, const UE_nr_rxtx_proc_t *proc)
{
  if (!nr_pdcch_blind_monitor_enabled() || !nr_isac_enabled()
      || !nr_isac_source_enabled(NR_ISAC_SRC_PDSCH_DMRS_BLIND)) {
    return;
  }
  const nr_pdcch_blind_monitor_cfg_t *cfg = nr_pdcch_blind_monitor_get_cfg();

  NR_DL_FRAME_PARMS *fp = &ue->frame_parms;
  const uint32_t abs_slot = (uint32_t)proc->frame_rx * fp->slots_per_frame + (uint32_t)proc->nr_slot_rx;
  if (cfg->ss_monitoring_slot_periodicity <= 0
      || (abs_slot % (uint32_t)cfg->ss_monitoring_slot_periodicity) != (uint32_t)cfg->ss_monitoring_slot_offset) {
    return; // not a monitoring occasion this slot
  }
  g_occasions_run++;

  // ---- Build the local, single-search-space PDCCH config. ----
  nr_phy_data_t local_phy_data;
  memset(&local_phy_data, 0, sizeof(local_phy_data));
  local_phy_data.phy_pdcch_config.nb_search_space = 1;
  fapi_nr_dl_config_dci_dl_pdu_rel15_t *rel15 = &local_phy_data.phy_pdcch_config.pdcch_config[0];

  rel15->BWPStart = (uint16_t)cfg->bwp_start;
  rel15->BWPSize  = (uint16_t)cfg->bwp_size;
  rel15->coreset.CoreSetType = NFAPI_NR_CSET_CONFIG_PDCCH_CONFIG;
  rel15->coreset.rb_offset   = 0;
  rel15->coreset.duration    = (uint8_t)cfg->coreset_duration;
  build_coreset_bitmap(cfg->coreset_freq_domain, rel15->coreset.frequency_domain_resource);
  rel15->coreset.CceRegMappingType = FAPI_NR_CCE_REG_MAPPING_TYPE_NON_INTERLEAVED;
  rel15->coreset.RegBundleSize     = (uint8_t)cfg->coreset_reg_bundle_size;
  rel15->coreset.InterleaverSize   = (uint8_t)cfg->coreset_interleaver_size;
  rel15->coreset.ShiftIndex        = (uint8_t)cfg->coreset_shift_index;
  rel15->coreset.pdcch_dmrs_scrambling_id = cfg->coreset_pdcch_dmrs_scrambling_id;
  rel15->coreset.scrambling_rnti   = 0; // PCI-only descrambling -- this gNB never sets
                                       // pdcch_DMRS_ScramblingID (see plan finding 1)
  if (cfg->ss_first_symbol < 0 || cfg->ss_first_symbol >= fp->symbols_per_slot) {
    return;
  }
  rel15->coreset.StartSymbolBitmap = (uint16_t)(1u << (fp->symbols_per_slot - 1 - cfg->ss_first_symbol));

  int n_rb = 0, cset_start = 0;
  get_coreset_rballoc(rel15->coreset.frequency_domain_resource, &n_rb, &cset_start);
  if (n_rb < 12 || rel15->coreset.duration < 1) { // need >=2 AL2 candidates' worth of CCEs to bother
    return;
  }
  const int num_cces = (n_rb * rel15->coreset.duration) / 6;
  const int num_candidates =
      (num_cces / 2 < NR_PDCCH_BLIND_AL2_MAX_CANDIDATES) ? (num_cces / 2) : NR_PDCCH_BLIND_AL2_MAX_CANDIDATES;
  if (num_candidates < 1) {
    return;
  }
  rel15->number_of_candidates = (uint8_t)num_candidates;
  for (int c = 0; c < num_candidates; c++) {
    rel15->CCE[c] = (uint16_t)(c * 2);
    rel15->L[c]   = 2;
  }
  const uint16_t dci_length =
      cfg->dci_length_override > 0 ? (uint16_t)cfg->dci_length_override : nr_pdcch_blind_dci_size((uint16_t)cfg->bwp_size);
  if (dci_length == 0) {
    return;
  }
  rel15->num_dci_options       = 1;
  rel15->dci_length_options[0] = dci_length;

  // ---- FEP the CORESET's own symbol(s) + generate LLR (reuses the real RT PDCCH pipeline). ----
  const int llr_size_symbol    = n_rb * NR_PDCCH_BLIND_RE_PER_RB_OUT_DMRS;
  const int num_monitoring_occ = 1; // exactly one bit set in StartSymbolBitmap by construction above
  c16_t pdcch_llr[1][1][255 * NR_PDCCH_BLIND_RE_PER_RB_OUT_DMRS]; // generously sized; see bound check below
  if ((size_t)(rel15->coreset.duration * llr_size_symbol) > sizeof(pdcch_llr[0][0]) / sizeof(c16_t)) {
    LOG_E(PHY, "SENSING: blind PDCCH monitor CORESET too large for local LLR buffer (n_rb=%d)\n", n_rb);
    return;
  }

  const uint32_t rxdataF_sz = fp->samples_per_slot_wCP;
  __attribute__((aligned(32))) c16_t rxdataF[fp->nb_antennas_rx][rxdataF_sz];

  for (int symbol = cfg->ss_first_symbol; symbol < cfg->ss_first_symbol + rel15->coreset.duration; symbol++) {
    nr_slot_fep(ue, fp, proc->nr_slot_rx, symbol, rxdataF, link_type_dl, 0, ue->common_vars.rxdata);
    __attribute__((aligned(32))) c16_t rxdataF_symb[fp->nb_antennas_rx][((fp->ofdm_symbol_size + 7) / 8) * 8];
    for (int ant = 0; ant < fp->nb_antennas_rx; ant++) {
      memcpy(rxdataF_symb[ant], &rxdataF[ant][symbol * fp->ofdm_symbol_size], sizeof(c16_t) * fp->ofdm_symbol_size);
    }
    nr_pdcch_generate_llr(ue, proc, symbol, &local_phy_data, llr_size_symbol, num_monitoring_occ,
                         rel15->coreset.duration, rxdataF_symb, pdcch_llr);
  }

  // ---- Demapping/deinterleaving + per-candidate unscrambling/decode. Mirrors dci_nr.c's own
  // nr_pdcch_dci_indication()/nr_dci_decoding_procedure(), minus the own-RNTI equality gate --
  // that's the entire "blind" widening (nr_pdcch_blind_decode_and_extract() does its own range
  // check instead). ----
  const int llr_stride = llr_size_symbol; // duration==1 here -> llr_size == llr_size_symbol
  c16_t pdcch_e_rx[NR_MAX_PDCCH_SIZE];
  nr_pdcch_demapping_deinterleaving((uint32_t)n_rb, pdcch_llr[0][0], pdcch_e_rx, rel15->coreset.duration,
                                    rel15->coreset.RegBundleSize, rel15->coreset.InterleaverSize,
                                    rel15->coreset.ShiftIndex, rel15->number_of_candidates, rel15->CCE, rel15->L,
                                    llr_stride);

  // Persistence window in slots -- computed once per occasion (cheap, only used when the gate is
  // enabled). Standard NR: 10ms/frame regardless of numerology, so slots_per_frame slots = 10ms.
  const uint32_t persist_window_slots =
      (cfg->rnti_persist_k > 1)
          ? (uint32_t)(((int64_t)cfg->rnti_persist_window_ms * fp->slots_per_frame) / 10)
          : 0;

  int e_rx_cand_idx = 0;
  for (int c = 0; c < rel15->number_of_candidates; c++) {
    const int L         = rel15->L[c];
    const int n_re_cand = NR_PDCCH_BLIND_RE_PER_RB_OUT_DMRS * L * 6;

    // ---- Gate 1 (cheapest, runs first): raw pre-decode LLR energy. Unscheduled CCEs measured
    // exactly (0,0) live 2026-07-28; skips the polar decode entirely for those, not just the CFR
    // submission -- a real CPU saving alongside the false-accept reduction. ----
    if (cfg->energy_min > 0.0f) {
      const c16_t *e_raw   = &pdcch_e_rx[e_rx_cand_idx];
      double        sum_abs = 0;
      for (int i = 0; i < n_re_cand; i++) {
        sum_abs += (e_raw[i].r < 0 ? -e_raw[i].r : e_raw[i].r) + (e_raw[i].i < 0 ? -e_raw[i].i : e_raw[i].i);
      }
      if ((float)(sum_abs / n_re_cand) < cfg->energy_min) {
        e_rx_cand_idx += n_re_cand;
        g_held_energy++;
        continue;
      }
    }

    int16_t tmp_e[16 * 108];
    nr_pdcch_unscrambling(&pdcch_e_rx[e_rx_cand_idx], rel15->coreset.scrambling_rnti, (uint32_t)(L * 108),
                          rel15->coreset.pdcch_dmrs_scrambling_id, tmp_e);
    e_rx_cand_idx += n_re_cand;
    g_candidates_run++;

    nr_pdcch_blind_result_t out;
    const bool ok = nr_pdcch_blind_decode_and_extract(tmp_e, (uint8_t)L, dci_length, (uint16_t)cfg->bwp_size,
                                                       (uint8_t)cfg->dmrs_typeA_position, cfg->rnti_min,
                                                       cfg->rnti_max, &out);
    if (!ok) {
      g_last_reject_reason = out.reject_reason; // TEMPORARY diagnostic, see periodic summary below
      g_last_reject_rnti   = out.rnti;
      continue;
    }
    g_accepts++;

    // ---- Gate 2: RNTI persistence. A real UE's RNTI recurs across many grants; a noise accept is
    // (almost always) a one-off. See rnti_persistence_check()'s own comment. ----
    if (!rnti_persistence_check(out.rnti, abs_slot, persist_window_slots, cfg->rnti_persist_k)) {
      g_held_persist++;
      continue;
    }

    LOG_D(PHY,
         "SENSING: blind PDCCH accept (%d.%d) rnti=0x%x prb=[%u..%u) sym=[%u..%u) dmrs_mask=0x%x\n",
         proc->frame_rx, proc->nr_slot_rx, out.rnti, out.start_rb, out.start_rb + out.num_rb, out.start_symbol,
         out.start_symbol + out.num_symbols, out.dl_dmrs_symb_pos);

    // ---- Cross-receiver RNTI consistency (offline, post-hoc -- see tests/passive_rx/rnti_gate.py):
    // this line's sole purpose is a wall-clock anchor to correlate accepts across INDEPENDENT
    // receiver PROCESSES that share no RT state. A real active UE's RNTI is legitimately accepted by
    // EVERY receiver that can hear it (same air, same grants); a noise/false accept is
    // receiver-local (wrong CRC-recovered RNTI from that receiver's own decode error) and will not
    // recur on a SECOND receiver at the same wall-clock time. LOG_I (not LOG_D) and deliberately
    // separate from the line above -- that one is for interactive debugging (PRB/symbol detail, gets
    // noisy fast), this one is a fixed, parseable schema meant to be grepped by tooling. ----
    struct timespec rnti_ts;
    clock_gettime(CLOCK_REALTIME, &rnti_ts);
    const long long rnti_utc_ns = (long long)rnti_ts.tv_sec * 1000000000LL + (long long)rnti_ts.tv_nsec;
    LOG_I(PHY, "SENSING: blind PDCCH rnti_seen utc_ns=%lld rnti=0x%x\n", rnti_utc_ns, out.rnti);

    // ---- CFR extraction: nr_pdsch_channel_estimation() on the blind-decoded allocation/DMRS config
    // -- mirrors phy_procedures_nr_ue.c's existing pdsch_dmrs ISAC tap exactly (same function, same
    // pdsch_est_size formula, same comb-2 packing), just fed from a blind decode instead of the UE's
    // own real DLSCH config. ----
    int dmrs_sym = -1;
    for (int m = out.start_symbol; m < out.start_symbol + out.num_symbols; m++) {
      if (out.dl_dmrs_symb_pos & (1u << m)) {
        dmrs_sym = m;
        break;
      }
    }
    if (dmrs_sym < 0) {
      continue;
    }

    fapi_nr_dl_config_dlsch_pdu_rel15_t dlsch_pdu;
    memset(&dlsch_pdu, 0, sizeof(dlsch_pdu));
    dlsch_pdu.BWPStart           = (uint16_t)cfg->bwp_start;
    dlsch_pdu.BWPSize            = (uint16_t)cfg->bwp_size;
    dlsch_pdu.resource_alloc     = 1; // Type-1/RIV -- the only branch this module ever produces
    dlsch_pdu.refPoint           = 0;
    dlsch_pdu.dmrsConfigType     = NFAPI_NR_DMRS_TYPE1;
    dlsch_pdu.n_dmrs_cdm_groups  = out.n_dmrs_cdm_groups;
    dlsch_pdu.dlDmrsScramblingId = fp->Nid_cell;
    dlsch_pdu.nscid              = out.nscid;
    dlsch_pdu.start_symbol       = out.start_symbol;
    dlsch_pdu.number_symbols     = out.num_symbols;
    dlsch_pdu.dlDmrsSymbPos      = out.dl_dmrs_symb_pos;
    dlsch_pdu.dmrs_ports         = out.dmrs_ports;

    const freq_alloc_bitmap_t freq_alloc = set_bitmap_from_start_size(out.start_rb, out.num_rb);

    const uint32_t pdsch_est_size = ((fp->symbols_per_slot * fp->ofdm_symbol_size + 15) / 16) * 16;
    fourDimArray_t *toFree        = NULL;
    allocCast2D(pdsch_dl_ch_estimates, int32_t, toFree, fp->nb_antennas_rx, pdsch_est_size, false);

    // Full-slot FEP for the PDSCH's own DMRS symbol -- separate from the CORESET FEP above (a
    // different symbol in general; the PDSCH allocation starts after the PDCCH region).
    __attribute__((aligned(32))) c16_t rxdataF_pdsch[fp->nb_antennas_rx][rxdataF_sz];
    nr_slot_fep(ue, fp, proc->nr_slot_rx, dmrs_sym, rxdataF_pdsch, link_type_dl, 0, ue->common_vars.rxdata);

    uint32_t nvar = 0;
    nr_pdsch_channel_estimation(ue, proc, &dlsch_pdu, &freq_alloc, 0, get_dmrs_port(0, out.dmrs_ports),
                               (unsigned char)dmrs_sym, pdsch_est_size, pdsch_dl_ch_estimates,
                               fp->samples_per_slot_wCP, rxdataF_pdsch, &nvar);

    const int num_sc = out.num_rb * NR_NB_SC_PER_RB;
    if (num_sc >= 2) {
      const uint32_t base_sc = (uint32_t)(cfg->bwp_start + out.start_rb) * NR_NB_SC_PER_RB;
      // ---- AoA (2026-07-28): nr_pdsch_channel_estimation() above already computed the estimate for
      // EVERY rx antenna (it loops aarx in [0,nb_antennas_rx) internally and writes
      // pdsch_dl_ch_estimates[a][...] for each) -- this was already true before today, nothing new
      // needed there. The only gap was HERE: extraction/submission only ever read antenna 0 and
      // called the single-antenna nr_isac_submit_cfr(). Mirrors csi_rx.c's nr_isac_submit_csirs_ls()
      // exactly (same nr_isac_aoa_antennas()/clamp/pack-then-submit-multi pattern, already
      // live-validated there for the attached-UE AoA path). Antenna 0 stays primary (feeds
      // range-Doppler + the SNR gate below); antennas 1..N-1 exist solely for isac_aoa.cc's bearing
      // estimate.
      uint32_t nof_ant = nr_isac_aoa_antennas();
      if (nof_ant > (uint32_t)fp->nb_antennas_rx) {
        nof_ant = (uint32_t)fp->nb_antennas_rx;
      }
      if (nof_ant == 0) {
        nof_ant = 1;
      }
      if (nof_ant > NR_PDCCH_BLIND_MAX_ANT) {
        nof_ant = NR_PDCCH_BLIND_MAX_ANT;
      }
      static __thread float    isac_h[NR_PDCCH_BLIND_MAX_ANT * 2 * 273 * NR_NB_SC_PER_RB];
      static __thread uint32_t isac_k[273 * NR_NB_SC_PER_RB];
      static __thread uint32_t isac_l[273 * NR_NB_SC_PER_RB];
      uint32_t nof_re  = 0;
      double   h_pow_sum = 0; // antenna 0 only -- feeds the post-estimation SNR gate below
      for (int j = 0; j < num_sc && nof_re < 273 * NR_NB_SC_PER_RB; j += 2) { // comb-2, matches pdsch_dmrs
        for (uint32_t a = 0; a < nof_ant; a++) {
          const c16_t *dl_ch_a = (const c16_t *)&pdsch_dl_ch_estimates[a][fp->ofdm_symbol_size * dmrs_sym];
          const size_t o       = 2 * ((size_t)a * (273 * NR_NB_SC_PER_RB) + nof_re);
          isac_h[o]            = (float)dl_ch_a[j].r;
          isac_h[o + 1]        = (float)dl_ch_a[j].i;
          if (a == 0) {
            h_pow_sum += (double)dl_ch_a[j].r * dl_ch_a[j].r + (double)dl_ch_a[j].i * dl_ch_a[j].i;
          }
        }
        isac_k[nof_re] = base_sc + (uint32_t)j;
        isac_l[nof_re] = (uint32_t)dmrs_sym;
        nof_re++;
      }
      // ---- Gate 3 (last, most expensive to reach): the actual DMRS channel estimate itself looks
      // like noise even though every payload-level check passed by chance. mean|H|^2/nvar is
      // scale-invariant (both come from the SAME nr_pdsch_channel_estimation() call), so no absolute
      // unit conversion is needed. ----
      const bool snr_ok =
          !(cfg->min_snr_lin > 0.0f && nof_re > 0 && nvar > 0
            && (float)(h_pow_sum / nof_re) < cfg->min_snr_lin * (float)nvar);
      if (nof_re > 0 && snr_ok) {
        nr_isac_carrier_t carrier = {.nof_prb         = (uint32_t)fp->N_RB_DL,
                                     .scs_hz          = fp->subcarrier_spacing,
                                     .dl_center_hz    = fp->dl_CarrierFreq,
                                     .pci             = fp->Nid_cell,
                                     .slots_per_frame = fp->slots_per_frame};
        nr_isac_submit_cfr_multi(abs_slot, 0.0f, NR_ISAC_SRC_PDSCH_DMRS_BLIND, &carrier, isac_h, nof_ant,
                                 273 * NR_NB_SC_PER_RB, isac_k, isac_l, nof_re, (float)nvar);
        g_cfr_submits++;
      } else if (nof_re > 0) {
        g_held_snr++;
      }
    }
    free(toFree);
  }

  if (g_occasions_run % NR_PDCCH_BLIND_SUMMARY_PERIOD_OCC == 0) {
    LOG_I(PHY,
         "SENSING: blind PDCCH monitor summary: occasions=%lu candidates=%lu accepts=%lu "
         "held[energy=%lu persist=%lu snr=%lu] cfr_submits=%lu last_reject=\"%s\" last_reject_rnti=0x%x\n",
         (unsigned long)g_occasions_run, (unsigned long)g_candidates_run, (unsigned long)g_accepts,
         (unsigned long)g_held_energy, (unsigned long)g_held_persist, (unsigned long)g_held_snr,
         (unsigned long)g_cfr_submits, g_last_reject_reason ? g_last_reject_reason : "(none yet)",
         g_last_reject_rnti);
  }
}
