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
  /* CORESET type. 0 = PDCCH-Config (a dedicated CORESET; the DM-RS reference point is the
     BWP start), 1 = MIB/SIB1 (CORESET0; the reference point is CRB0). This is NOT cosmetic:
     nr_rx_pdcch_symbol() sets dmrs_ref = BWPStart only for the PDCCH-Config case, so a
     CORESET0 configured with a non-zero bwp_start and the wrong type generates its DM-RS
     sequence offset by bwp_start RBs and can NEVER decode. Default 0 keeps every existing
     dedicated-CORESET config bit-identical (they all use bwp_start = 0). */
  int      coreset_type;

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

  // ---- Deferred decode (PASSIVE_RX_ONLY_HANDOVER.md §15, 2026-08-24) --------------------------
  // nr_pdsch_passive_decode() costs 775 us mean / 2689 us max against a 500 us slot budget, so an
  // occasion that runs one ALWAYS overruns its deadline; a three-arm ablation at ~1500 offered
  // grants/s showed that alone taking PBCH lock from 88 s to 3 s (max_pos_acc runaway
  // 375->583->794). Setting this moves the decode to a consumer thread reading the same rxdata,
  // policed against the RF producer's position so a job whose samples were overwritten is dropped
  // rather than decoded. See nr_pdsch_passive_queue.h for the full measurement.
  //
  // DEFAULT 0 = the previous in-line behaviour, bit-identical, so the A/B is one config field.
  int   pdsch_thread;       // 0 = decode in-line on the PHY receive thread (default), 1 = consumer
  int   pdsch_queue_depth;  // ring depth, 0 = auto (8). Bounded by the rxdata lifetime, NOT taste:
                            // depth x 775 us must stay inside one frame (10 ms at mu=1), so 8 gives
                            // 6.2 ms with margin. Raising it does not buy throughput, it buys
                            // staleness.
  int   pdsch_thread_core;  // core to pin the consumer to; <0 = unpinned (default). MUST NOT be one
                            // of --thread-pool's cores -- the point is to stop competing with the
                            // receive path, and sharing a core with the candidate-decode pool would
                            // partly undo that.

  /* ---- Deferred blind-PDCCH SCAN. Distinct from pdsch_thread above: that defers the PDSCH DECODE
   * of an already-accepted grant; this defers the SCAN that finds the grant at all -- the full-slot
   * FEP, PDCCH channel estimation/equalisation, demapping and the candidate polar decodes.
   *
   * MEASURED (BTIM, mean per monitoring occasion, this deployment): fep_llr 54-62us out of a
   * 69-102us occasion, i.e. 60-79 % of it. Against a 500us slot that is 13.9 % receive-thread duty
   * at 165 grants/s and 16-20 % at ~1530 grants/s, straddling the ~18 % at which section 15
   * measured this receiver losing PBCH lock -- which is the mechanism behind the grant-rate-driven
   * 0 %/91 % PDSCH CRC bimodality (BRANCH_IMBALANCE_HARQ_PLAN.md section 12).
   *
   * DEFAULT 0 = in-line on the PHY receive thread, i.e. bit-identical to the previous behaviour.
   * The deferral changes WHICH thread demodulates, so it is opt-in rather than silent. */
  int   scan_thread;       // consumer count; 0 = in-line (default). >1 warns -- see
                           // nr_pdcch_passive_queue.h: the occasion body's energy floor, RNTI
                           // persistence table and counters are not yet thread-safe, and ONE
                           // consumer already sustains a slot-rate occasion stream.
  int   scan_queue_depth;  // ring depth; 0 = auto (8). Same bound as the PDSCH queue: a job holds
                           // only a slot reference, so it must be consumed before the producer laps
                           // rxdata, and a deeper ring buys staleness rather than throughput.
  int   scan_thread_core;  // first core to pin consumers to; <0 = unpinned. Same caution as
                           // pdsch_thread_core -- do not share with --thread-pool's cores.

  // ---- Deferred passive PUSCH decode (2026-08-28). All-zero = in-line on the PHY receive thread,
  // exactly the previous behaviour. UTIM measured that in-line decode at 1065 us mean against a
  // 500 us slot budget -- over_slot 16830/16907 = 99.5 %, i.e. every uplink grant overran its
  // deadline. See nr_pusch_passive_queue.h.
  int   ul_thread;         // consumer count; 0 = in-line (default). Bounded by the decode-context
                           // count, since consumer index IS context index and two consumers must
                           // never share one PHY_VARS_gNB.
  int   ul_queue_depth;    // ring depth; 0 = auto (8). Deeper buys staleness, not throughput.
  int   ul_thread_core;    // first core to pin consumers to; <0 = unpinned. Do not share with
                           // --thread-pool's cores.

  // ---- UCI-on-PUSCH reservation search (2026-08-28). 0 = off, and off is the previous behaviour.
  // O_ACK is not in the uplink DCI -- the DAI pins it only modulo 4 -- so the candidates consistent
  // with the observed DAI are tried and the transport-block CRC decides. See the block comment on
  // passive_ul_unav_res() in nr_pusch_passive_decode.c.
  int   ul_uci_search;     // max candidates to try per failed grant; 0 = disabled
  int   ul_uci_beta;       // betaOffsets HARQ-ACK index (TS 38.213 Table 9.3-1)
  int   ul_uci_alpha;      // alpha-scaling index: 0=0.5 1=0.65 2=0.8 3=1.0

  // ---- DCI format 1_0 scanning (2026-08-21). All-zero = off, i.e. exactly the format-1_1-only
  // behaviour this module had before. See nr_pdcch_blind_monitor.h's nr_blind_dci_format_t block
  // for why a passive receiver needs 1_0 at all (SIB1, Msg2/RAR, Msg4/RRCSetup, C-RNTI fallback).
  //
  // COST: format 1_0 and 1_1 have DIFFERENT payload widths (44 vs 47 bits at 273 PRB here), and the
  // polar decoder is sized by that width, so scanning both means a SECOND decode per candidate --
  // this roughly doubles the tap's CPU. That is why it is opt-in and why dci10_scan == 2 (1_0 only)
  // exists: a receiver watching CORESET#0 for broadcast/RA traffic has no 1_1 to find there.
  int dci10_scan;        // 0 = format 1_1 only (default), 1 = both, 2 = format 1_0 only
  int dci10_ss_type;     // nr_blind_ss_type_t: 0 = UE-specific (default), 1 = common. Selects
                         // 1_0's frequency reference, PRB origin and TDRA list -- see the
                         // nr_pdcch_blind_dci10_ctx_t field comments; NOT a cosmetic label.
  int dci10_n_rb_riv;    // TS 38.212 7.3.1.0's N_RB^DL,BWP for format 1_0. 0 = auto: bwp_size for a
                         // UE-specific search space, and the CONFIGURED CORESET's own RB count for a
                         // common one (which is CORESET#0's size when this monitor is pointed at it,
                         // exactly what the spec asks for).
  int dci10_rb_offset;   // TS 38.214 5.1.2.2.2's PRB origin. <0 = auto: bwp_start for a UE-specific
                         // search space, the CORESET's first RB for a common one.
  int dci10_length_override; // 0 = use nr_pdcch_blind_dci10_size(). Unlike the 1_1 override this
                         // should almost never be needed -- 1_0's width is a pure spec formula with
                         // no RRC-derived terms. Set it only to apply TS 38.212 7.3.1.0's
                         // UE-specific-search-space zero-padding up to DCI 0_0's size, which
                         // nr_pdcch_blind_dci00_size() computes.
  int dci10_class_mask;  // bitmask of (1u << nr_blind_rnti_class_t); 0 = auto from ss_type.
                         // Narrowing it is the cheapest false-accept reduction on this format.
  int dci10_mux_pattern; // SS/PBCH-to-CORESET#0 multiplexing pattern (1/2/3); 0 = 1. Only consulted
                         // for SIB1's own DCIs with no TDRA list configured.
  int dci10_sib1;        // 1 = the SI-RNTI DCIs being scanned schedule SIB1 itself, so no
                         // pdsch-ConfigCommon TDRA list can exist yet (it travels inside SIB1) and
                         // the mux-pattern default table applies instead.

  // ---- UPLINK: DCI format 0_1 (2026-08-26) ------------------------------------------------
  // UL grants ride the SAME CORESET and CCE space as the DL ones and were already being polar-
  // decoded and thrown away at the format-indicator test. Keeping them is what makes the receiver
  // bidirectional: each one names a PUSCH that can then be extracted from the same 4-antenna
  // stream. Format 0_1 has its own RRC-derived width (43 bits on this cell against 1_1's 47), so
  // unlike 0_0 -- which is size-aligned with 1_0 and therefore free -- it needs its own polar
  // decode. Budget roughly one extra 1_0-scan's worth of CPU per candidate.
  int dci01_scan;            // 0 = off (default, bit-identical to before), 1 = scan DCI 0_1
  int dci01_length_override; // 0 = use nr_pdcch_blind_dci01_size(&ul). SET THIS: the formula's
                             // defaults are a starting point, not the pinned layout -- see the UL
                             // section of nr_pdcch_blind_monitor.h. 43 is the live-verified value
                             // for this deployment at 273 PRB, read off the gNB's own FAPI dump.
  int ul_pusch_decode;   // 0 = off (default). 1 = decode the PUSCH each recovered UL grant names.
                         // Runs IN-LINE in the uplink slot. That is affordable only because uplink
                         // slots are otherwise idle for this receiver -- 2 of every 10 slots do no
                         // work at all today -- but the decode is the same order of cost as the DL
                         // one that was measured overrunning a 500 us slot budget, so watch
                         // max_pos_acc and PBCH before trusting a long capture.
  int ul_pusch_max_per_slot; // 0 = 1
  int ul_ta_offset_samples;  // 0 = derive N_TA_offset from the sample rate. The per-UE N_TA is NOT
                         // in any DCI, so this is where a measured or swept residual goes.
  nr_pdcch_blind_ul_opts_t ul; // UL BWP, pusch-TimeDomainAllocationList (k2 lives here), DM-RS,
                               // waveform/identities and the per-field widths.
  /* PHASE 1 self-configuration. 1 = derive the COMMON search space (CORESET#0 / SearchSpace#0)
   * from MIB/SIB1 at runtime instead of reading it from pdcch_blind_monitor_coreset/_ss/_bwp.
   * Default 0: an existing config describes the DEDICATED search space, and replacing it with the
   * common one trades a dense data-aided source for a sparse SIB1 one -- never do that unasked. */
  int autoconf;
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

/**
 * @brief Run ONE monitoring occasion: FEP -> PDCCH LLR -> demap -> per-candidate decode -> accepts.
 *
 * Split out of nr_pdcch_blind_monitor_process() so it can run on a scan consumer instead of the PHY
 * receive thread (see nr_pdcch_passive_queue.h for the BTIM measurement that motivates it). The
 * SAME code runs either way -- in-line when the pool is not configured, deferred when it is -- so
 * the two are directly A/B-able with one config field and no second code path to keep in step.
 *
 * Takes only `proc`: the occasion's slot index is derived from it exactly as before the split, so a
 * deferred occasion lands on the same ISAC slow-time row as an in-line one would. The queue's own
 * monotonic slot reference stays inside the queue, where it belongs -- it answers "do these samples
 * still exist", not "which row is this".
 *
 * @param serial_candidates run the per-candidate decodes in THIS thread instead of fanning them out
 *        to the shared UE pool. True from a scan consumer: the occasion is already off the receive
 *        thread, so the fan-out protects nothing and instead blocks at priority 50 behind whatever
 *        else the pool is serving -- measured at 18.4us in-line versus 271.1us deferred for the
 *        identical 38-candidate workload.
 */
void nr_pdcch_blind_monitor_run_occasion(PHY_VARS_NR_UE *ue, const UE_nr_rxtx_proc_t *proc,
                                         bool serial_candidates);

#ifdef __cplusplus
}
#endif

#endif // NR_PDCCH_BLIND_MONITOR_RT_H
