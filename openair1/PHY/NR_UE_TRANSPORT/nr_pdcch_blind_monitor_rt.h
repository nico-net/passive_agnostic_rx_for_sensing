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

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.h
 * \brief RT receive-path pieces for the blind PDCCH monitor (Phase 3 live wiring): the shared
 * config struct + accessor, and the RT tap entry point.
 *
 * Deliberately SEPARATE from nr_pdcch_blind_monitor.h: that header (and its .c file) is shared
 * with the offline gtest (tests/nr_pdcch_blind_monitor_test.cc) and kept off the heavy
 * PHY_NR_UE/executables dependency graph by design -- it owns config parsing (static g_cfg) and
 * the pure decode/extract functions only. The actual RT DSP wiring
 * (nr_pdcch_blind_monitor_process(), FEP/LLR/demapping/channel-estimation/CFR-submission) lives in
 * a SEPARATE translation unit, nr_pdcch_blind_monitor_rt.c, compiled into PHY_NR_UE_SRC so it can
 * call nr_slot_fep()/nr_pdcch_generate_llr()/nr_pdsch_channel_estimation()/nr_isac_submit_cfr()
 * without those heavy dependencies leaking into the offline-tested lib or its gtest target. This
 * header is the seam between the two: it declares the config struct + read-only accessor
 * (implemented in nr_pdcch_blind_monitor.c, which owns the static g_cfg) and the RT entry point
 * (implemented in nr_pdcch_blind_monitor_rt.c). Pulls in "PHY/defs_nr_UE.h" (needed for
 * PHY_VARS_NR_UE/UE_nr_rxtx_proc_t, the second of which has no forward-declarable tag), so only
 * RT-linkable code includes it -- the offline test does not.
 */

#ifndef NR_PDCCH_BLIND_MONITOR_RT_H
#define NR_PDCCH_BLIND_MONITOR_RT_H

#include "PHY/defs_nr_UE.h"
#include "nr_pdcch_blind_monitor.h" // nr_pdcch_blind_extract_opts_t, carried in the cfg below

/// [sensing] pdcch_blind_monitor_* parsed config. Owned (static instance) by
/// nr_pdcch_blind_monitor.c; read-only access for RT code via nr_pdcch_blind_monitor_get_cfg().
typedef struct {
  int      coreset_freq_domain; // num_groups: contiguous 6-PRB frequency-domain groups from group 0
  int      coreset_duration;
  int      coreset_reg_bundle_size;
  int      coreset_interleaver_size;
  int      coreset_shift_index;
  uint16_t coreset_pdcch_dmrs_scrambling_id;

  int ss_monitoring_slot_periodicity;
  int ss_monitoring_slot_offset;
  int ss_duration;
  int ss_first_symbol;
  int ss_al_candidates[4];

  int bwp_start;
  int bwp_size;
  int dmrs_typeA_position;
  int dci_length_override; // 0 = fall back to nr_pdcch_blind_dci_size(bwp_size)'s formula. A blind
                           // receiver has no RRC visibility into several fields nr_dci_size()'s
                           // real DCI-1_1 formula depends on (found live 2026-07-28: this
                           // deployment's dl_DataToUL_ACK/pdsch_CGB_Transmission/etc. give a REAL
                           // payload of 45 bits vs the formula's hand-derived 48 for BWP 0:106 --
                           // 3-bit mismatch, confirmed via a one-shot diagnostic dump of the real
                           // MAC-driven rel15->dci_length_options[0] against the active UE's own
                           // PDCCH decode -- see nr_pdcch_blind_monitor.c's file-level comment).
                           // Same "verify against a live run, don't just guess" pattern as
                           // csirs_monitor's scramb_id/crbs -- set this from a live capture rather
                           // than trusting the formula for a new deployment.

  uint16_t rnti_min;
  uint16_t rnti_max;

  // ---- Noise-floor / false-accept reduction (2026-07-28, see PHASE3_BLIND_PDCCH_LIVE_WIRING_HANDOVER.md
  // "Eliminate false-positive PDSCH DMRS"). The RNTI range above alone cannot distinguish a real
  // grant from noise at the trial volume this scan runs at (8 CCE candidates x ~2000 slots/sec) --
  // these three gates attack it from different angles, all independently disable-able. ----
  float energy_min;       // raw pre-decode LLR magnitude gate (mean |I|+|Q| over a candidate's REs,
                          // pdcch_e_rx units): skip polar decode entirely below this -- unscheduled
                          // CCEs measured exactly (0,0) live 2026-07-28, real grants measured
                          // mean_abs in the tens, so this is a coarse but cheap and safe gate.
                          // 0 disables it. NOTE this is an ABSOLUTE threshold in receiver-dependent
                          // units, so a value tuned on one deployment/gain setting does not transfer
                          // to another -- prefer energy_adapt_factor below.
  float energy_adapt_factor; // >0 selects the ADAPTIVE energy gate and OVERRIDES energy_min.
                          // The threshold becomes factor * (running estimate of the NOISE-FLOOR
                          // candidate energy), so it self-calibrates to whatever the receiver's
                          // gain, bandwidth and noise environment actually are instead of encoding
                          // one deployment's absolute level. Dimensionless, so one value is portable
                          // -- which is the whole point: the absolute energy_min could not be
                          // carried from the 106 PRB cell to 273 PRB, let alone to real OTA gain
                          // settings. See nr_pdcch_blind_monitor_rt.c's energy_floor_update() for
                          // the estimator and why the median (not the mean) is tracked.
                          // ~2.0 is a sane starting point; 0 keeps the legacy absolute behaviour.
                          // <=0 disables (always decode).
  int   rnti_persist_k;   // accept (and CFR-submit) only once this CRC-recovered RNTI has been seen
                          // at least this many times within rnti_persist_window_ms -- a real UE's
                          // RNTI recurs across many grants; a noise accept is a one-off. <=1
                          // disables (accept on first sighting, i.e. today's behaviour).
  int   rnti_persist_window_ms;
  float min_snr_lin;      // post-channel-estimation gate: mean |H|^2 / nvar over the extracted CFR
                          // REs must exceed this (linear, not dB) or the candidate is held (decoded
                          // fine, plausible fields, but the actual DMRS channel estimate itself
                          // looks like noise) -- catches candidates that pass every payload-level
                          // check by chance but were never really scheduled. <=0 disables.

  // ---- Deployment facts a blind receiver cannot read off the air (2026-07-30). See
  // nr_pdcch_blind_extract_opts_t in nr_pdcch_blind_monitor.h for WHY the spec defaults are wrong
  // for this project's gNB and why that only starts mattering once the PDSCH is actually decoded.
  // Zeroed/absent = the spec-default behaviour this module has always had. ----
  nr_pdcch_blind_extract_opts_t extract;

  // ---- Passive data-aided PDSCH (PASSIVE_PDSCH_DATA_AIDED_HANDOVER.md Part B). ----
  int   pdsch_decode;     // 0 = off (default). 1 = decode overheard PDSCH and COUNT the CRC pass
                          // rate only -- the handover doc's go/no-go gate, deliberately its own
                          // level so the expensive-but-harmless measurement can be run before
                          // anything downstream trusts a reconstructed X. 2 = also re-encode a
                          // CRC-verified TB and submit Ĥ = Y/X as NR_ISAC_SRC_PDSCH_DATA (which
                          // additionally requires `pdsch_data` in [sensing] sources).
  int   pdsch_mcs_table;  // 0 = qam64 (this deployment: no mcs-Table configured), 1 = qam256,
                          // 2 = qam64LowSE. Must match the gNB's PDSCH-Config or every TBS is wrong.
  int   pdsch_xoverhead;  // xOverhead_PDSCH in REs/PRB (0/6/12/18); 0 = not configured, the default.
  int   pdsch_rv0_only;   // 1 = only attempt grants with rv==0 (default). A retransmission is not
                          // self-decodable without the earlier round's soft bits, which a passive
                          // receiver that missed the first grant does not have -- attempting them
                          // burns LDPC iterations for a guaranteed CRC failure. 0 = attempt all,
                          // which is what you want when MEASURING the pass rate by RV.
  int   pdsch_max_per_slot; // cap on decode attempts per monitoring occasion (<=0 = 1). LDPC decode
                            // is by far the most expensive thing in this tap and a noisy slot can
                            // otherwise present several accepted candidates at once.
} nr_pdcch_blind_monitor_cfg_t;

#ifdef __cplusplus
extern "C" {
#endif

/// Non-NULL always; contents only meaningful once nr_pdcch_blind_monitor_enabled() is non-zero
/// (i.e. after nr_pdcch_blind_monitor_init() has parsed a complete config). Do not modify through
/// this pointer -- owned by nr_pdcch_blind_monitor.c.
const nr_pdcch_blind_monitor_cfg_t* nr_pdcch_blind_monitor_get_cfg(void);

/**
 * @brief Blind-PDCCH RT tap: if a monitoring occasion is due and [sensing] pdcch_blind_monitor_*
 * is configured, FEPs the CORESET's own symbol(s), generates LLR (reusing the exported
 * nr_pdcch_generate_llr()), runs the AL2 CCE candidates through demapping/unscrambling (exported
 * from dci_nr.c) and nr_pdcch_blind_decode_and_extract(), and on a plausible result extracts a
 * DMRS channel estimate (nr_pdsch_channel_estimation()) and submits it to the sensing engine as
 * NR_ISAC_SRC_PDSCH_DMRS_BLIND. No-op when the monitor/ISAC source is not enabled. Uses a fully
 * local nr_phy_data_t -- never touches the real MAC-driven phy_pdcch_config, so this cannot
 * collide with the UE's own PDCCH processing even if called outside UE_RECEIVING_SIB.
 *
 * @param ue    UE PHY instance
 * @param proc  Current slot's RX/TX processing context
 */
void nr_pdcch_blind_monitor_process(PHY_VARS_NR_UE *ue, const UE_nr_rxtx_proc_t *proc);

#ifdef __cplusplus
}
#endif

#endif // NR_PDCCH_BLIND_MONITOR_RT_H
