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

/*! \file openair1/PHY/NR_UE_ISAC/defs_nr_UE_ISAC.h
 * \brief Internal (C++) definitions for the OAI-UE ISAC sensing pipeline: the parsed
 * argument struct, per-CPI snapshot, range-velocity map and detection records. Ported
 * 1:1 from srsUE (phy_sensing_args_t / sensing_slot_t / sensing_rvm_t / sensing_detection_t).
 *
 * Included only by the engine .cc files, never by the C PHY procedures.
 */

#ifndef NR_ISAC_DEFS_H
#define NR_ISAC_DEFS_H

#include <complex>
#include <cstdint>
#include <string>
#include <vector>

#include "nr_isac.h"

namespace nr_isac {

/// Complex float sample used throughout the off-RT DSP (matches srsUE icf_t).
using icf_t = std::complex<float>;

/// Subcarriers per PRB and the max PRB count used to pre-size the RT-safe snapshot buffers.
constexpr uint32_t ISAC_NRE     = 12;
constexpr uint32_t ISAC_NSYMB   = 14;
constexpr uint32_t ISAC_MAX_PRB = 275;

/**
 * @brief Parsed sensing configuration (mirror of srsUE phy_sensing_args_t).
 *
 * Defaults match the srsUE reference so behaviour is identical for the same settings.
 */
struct nr_isac_args_t {
  bool             enable      = false;              ///< Master enable (zero overhead when false)
  nr_isac_source_t source      = NR_ISAC_SRC_CSI_RS; ///< Primary source (lowest-numbered enabled) — for logs/single-source ref_type
  uint32_t         sources_mask = 1u << NR_ISAC_SRC_CSI_RS; ///< Enabled-source set: bit i set => source i feeds the fused grid
  uint32_t         cpi_slots   = 256;                ///< Coherent processing interval (slow-time length)
  bool             interpolate = true;               ///< Fill comb gaps (freq) + resample non-uniform slow-time

  bool        capture_enable   = false; ///< Also dump the raw RVM raster to file for offline analysis
  bool        selftest         = false; ///< Inject a default synthetic echo (25% range/velocity) to validate DSP
  std::string selftest_targets = "";    ///< Synthetic targets: "DELAY_US:DOPPLER_HZ:GAIN,..." injected into the CFR
  std::string selftest_los     = "";    ///< ota_sync_passive_ue.md Phase 6a: known LOS-path impairment
                                        ///< "STO_US:CFO_HZ:SFO_PPM" for the offline sync self-test (empty = none)

  // ota_sync_passive_ue.md Phases 1-4: master enable for the STO/CFO/SFO/closed-loop LOS-pinning
  // corrections. Default true (this is the new default-corrected behaviour); Phase 6a/6b's
  // "corrections disabled" baseline runs set this false to reproduce pre-Phase-1 behaviour exactly
  // (Phase 4's residual measurement still runs when false -- see sensing_engine.cc -- since observing
  // an uncorrected residual/smear is the whole point of the disabled baseline, not something to blind).
  bool sync_correction_enable = true;

  // Per-phase gates under the master switch above, for isolating which phase costs what. All default
  // true, so sync_correction=1 behaves exactly as before. Phase 1's ESTIMATION always runs when
  // Phase 2 is enabled (Phase 2 reuses its per-row LOS taps); sync_sto gates only whether Phase 1
  // APPLIES its correction. Added 2026-07-23 after measurement disproved both standing theories for
  // sync-ON's detection loss (search window and fade gate are both correctly tuned -- see
  // FADE_MIN_SNR_LINEAR's comment in isac_sync.cc), leaving the corrections themselves as the suspect.
  bool sync_sto = true; ///< Phase 1 fine-STO correction
  bool sync_cfo = true; ///< Phase 2 residual-CFO / per-row CPE de-rotation
  bool sync_sfo = true; ///< Phase 3 SFO delay-drift correction
  bool sync_los = true; ///< Phase 4 closed-loop LOS bias

  // Differential bistatic range (m) at which the LOS/direct path is expected, used to SEED Phase 1/3's
  // per-row peak search and Phase 4's baseline matcher. TESTBED-SPECIFIC: 98.0 is the live B210 cell's
  // diagnosed fixed group-delay artifact (originally recorded as ~49 m, doubled 2026-07-23 when the
  // range axis lost its erroneous factor 2 -- the tap did not move). A geometry whose direct path
  // arrives at its true differential range of 0 (e.g. tests/sensing_sim) must set this to 0.0:
  // leaving it at 98 there made Phase 4 lock its "LOS baseline" onto the MOVING TARGET (measured:
  // baseline established at bin 13 / 100.6 m / +5.18 m/s, unmistakably the target, not a static
  // direct path) and then feed back a bias fighting that target's own motion.
  float nominal_los_range_m = 98.0f;

  // CA-CFAR
  uint32_t cfar_guard = 4;      ///< CA-CFAR guard cells (per side)
  uint32_t cfar_train = 8;      ///< CA-CFAR training cells (per side)
  // Per-Doppler-column CFAR (opt-in). When set, a cell must ALSO pass a 1-D CA-CFAR whose training
  // cells lie only in the SAME Doppler column (range-only, excluding the guard band around the
  // cell), on top of the normal 2-D box test -- i.e. it is judged against the noise level of its OWN
  // velocity lane. WHY: a strong moving target leaves a range-wide pedestal that uniformly elevates
  // its own Doppler column(s) (RVM-confirmed 2026-07-24, see PHASE2_MOT_MULTIUE_HANDOVER.md); the 2-D
  // box straddles that hot column AND its cooler neighbours, so the averaged threshold sits below the
  // pedestal and every pedestal cell fires. Training WITHIN the column makes the pedestal its own
  // noise floor (cell/noise ~ 1 -> rejected), while a real target -- a sharp peak in a column that is
  // otherwise cool -- still clears it. Range-whitening (range_whiten) attacks the harmonic pedestals
  // from hot subcarriers; this attacks the fundamental-velocity pedestal from estimation noise. The
  // two are complementary.
  bool     cfar_per_column = false;
  // Per-RANGE-ROW CFAR (opt-in) -- the mirror of cfar_per_column, and the one aimed at the ghost
  // family that actually dominates after everything else (RVM-confirmed 2026-07-25, see the sub-slot
  // RVM animation in MULTISTATIC_FAST_TARGET_NOTES.md): a strong scatterer smears along SLOW-TIME,
  // producing a bright horizontal RIDGE across its own range row at every velocity. Those detections
  // are not statistical false alarms -- they are the target's own energy mis-attributed to wrong
  // velocities, which is exactly why lowering cfar_target_fa_per_cpi never removed them (measured:
  // raw precision rose but TRACK precision stayed flat). cfar_per_column trains along RANGE and so
  // rejects the vertical same-velocity pedestal; a ridge runs perpendicular to that and passes it
  // untouched. Training along DOPPLER (same range row, guard band excluded) makes the ridge its own
  // noise floor, so only a peak genuinely standing above the ridge survives. Complementary to
  // cfar_per_column -- enabling both brackets a strong target in each axis.
  bool     cfar_per_row    = false;
  // Sub-bin peak interpolation (opt-in): 3-point parabolic fit in dB through each detection's peak
  // and its two neighbours, in BOTH range and Doppler, so a detection is reported at its fractional
  // bin position instead of the bin centre. Without it every measurement carries a uniform
  // +/-half-bin quantisation error (+/-1.5 m in range at 100 MHz), which the tracker and the
  // multilateration then have to absorb as if it were sensor noise. Fitting in dB rather than linear
  // power keeps a windowed mainlobe close to parabolic -- the linear-power version is what forced
  // isac_sync.cc's empirical HANN_ESTIMATOR_SCALE correction. Offsets are clamped to +/-0.5 bin.
  bool     subbin_interp   = false;
  // Measured-gating-offset rejection (opt-in). harmonic_reject assumes the scheduling replicas sit at
  // INTEGER multiples k*v of a target's range-rate, which holds only for a strictly periodic gate. On
  // real traffic the gate is irregular -- T_slot was measured wandering 1.0-3.7 slots -- so the offsets
  // move CPI to CPI and the integer test catches only part of the family. This measures them instead:
  // range_doppler transforms the per-row energy envelope (the physical amplitude modulation that
  // creates the replicas) at the Doppler axis length, so its peak bins ARE the offsets, then rejects a
  // detection sitting at one of them from a clearly stronger same-range detection. gating_snr_margin
  // is the guard that keeps it honest: with several real targets crowded in Doppler, one real target
  // can sit a gating offset from another, so only a distinctly stronger neighbour may veto.
  bool     gating_reject      = false;
  uint32_t gating_max_offsets = 3;     ///< how many of the strongest envelope peaks to treat as offsets
  float    gating_min_rel     = 0.35f; ///< peak must reach this fraction of the strongest envelope peak
  uint32_t gating_tol_bins    = 2;     ///< Doppler-bin tolerance when matching an offset
  float    gating_snr_margin  = 3.0f;  ///< the vetoing neighbour must be this many dB STRONGER
  // cfar_pfa <= 0 (default) => AUTO: derive the per-cell Pfa from cfar_target_fa_per_cpi and the
  // CURRENT CPI's grid size (nof_range_bins * nof_doppler_bins), in range_doppler.cc's cfar().
  // WHY: cfar_pfa is a PER-CELL false-alarm probability, but the number of false alarms that
  // actually show up per CPI scales with the grid size (bandwidth * cpi_slots), which changes with
  // carrier config -- a fixed cfar_pfa tuned for one grid silently produces a different, wrong
  // false-alarm COUNT on a different one, exactly the same "don't hand-pick a constant, derive from
  // what's measurable" problem as track_confirm_m. LIVE-CONFIRMED 2026-07-24
  // (PHASE2_MOT_MULTIUE_HANDOVER.md): 100 MHz/273 PRB, cpi_slots=128 -> 3276*128 ~= 420k cells;
  // cfar_pfa=1e-4 (a fixed value carried over from earlier, smaller-grid tuning) predicts ~42
  // statistical false alarms/CPI purely by design, which a single-track filter silently ignores but
  // multi_target_tracker's M-of-N initiation does not -- this is what fed its track churn, not a
  // clutter/mirror bug. Explicit cfar_pfa > 0 still pins a manual per-cell value.
  float    cfar_pfa   = 0.0f;
  float    cfar_target_fa_per_cpi = 1.0f; ///< AUTO mode only (cfar_pfa<=0): desired MEAN number of
                                          ///< CA-CFAR statistical false alarms per CPI across the
                                          ///< whole range-Doppler grid, grid-size independent.
  // Closed-loop adaptation for cfar_target_fa_per_cpi (opt-in). WHY: this value was originally tuned
  // (2026-07-24) to solve a DIFFERENT problem -- ~16 raw detections/CPI dominated by structural noise
  // on a scene with only 2 real targets -- and was never revisited once the downstream filters
  // (harmonic_reject, matrix completion, doppler_sparse) matured enough to do real-vs-ghost
  // discrimination themselves. Left static at 1.0, it throttles the ENTIRE detection stream to ~1
  // event/CPI (live-measured: 77 detections / 76 CPIs = 1.013/CPI, matching the target almost
  // exactly) -- so a real target and a random statistical false alarm compete for the same
  // vanishingly small budget, and which one wins is close to a coin flip. That is a real source of
  // this project's run-to-run variance, not any single structured ghost mechanism. Rather than
  // hand-pick a new fixed number, this closed-loop adjusts cfar_target_fa_per_cpi from the MEASURED
  // raw (pre-NMS/pre-filter) detection count each CPI -- too few starves real targets (raise the
  // budget); too many floods the downstream filters (lower it back). The seed
  // (cfar_target_fa_per_cpi itself) is unchanged; only the loop is new.
  bool     cfar_fa_adapt_enable    = false;
  float    cfar_fa_min             = 0.5f;   ///< floor: never below the original strict regime
  float    cfar_fa_max             = 20.0f;  ///< ceiling: well below the pre-fix ~42/CPI statistical
                                             ///< baseline that motivated cfar_target_fa_per_cpi at all
  float    cfar_fa_adapt_rate      = 1.15f;  ///< per-CPI multiplicative step (raise: *rate, lower: /rate)
  uint32_t cfar_fa_target_min_det  = 2;      ///< below this many RAW detections/CPI -> raise the budget
  uint32_t cfar_fa_target_max_det  = 8;      ///< above this many RAW detections/CPI -> lower the budget

  // Clutter suppression + detection cleanup (Stage 5)
  uint32_t zero_doppler_guard = 3;  ///< Doppler bins around zero velocity to notch out (static clutter / ghost)
  uint32_t zero_range_guard   = 2;  ///< Range bins near zero delay to notch out (direct-path / LOS clutter)
  // Per-row comb de-aliasing (range_doppler.cc): a row whose real samples are comb-N spaced only
  // unambiguously supports the first nof_range/N bins; beyond that is the row's own periodic image.
  // Bins are hard-zeroed from that point, with a raised-cosine ramp of this width immediately before
  // it. Tuned against the passive_rx test harness (single comb-12 CSI-RS source, tests/passive_rx/):
  // a strong near-ideal direct path aliases to the comb-12 boundary and its Hann-window sidelobe skirt
  // bleeds BACKWARD into the nominally-valid region -- e.g. at the default width of 4, bin (valid-4) is
  // still at full gain (0 dB), so a sidelobe skirt spanning ~10+ bins back from the boundary is entirely
  // untouched. Widening this trades usable range (dealias_taper_bins * range_res_m, ~ (N-1)/N of it)
  // for suppression of that skirt; it does not affect rows with comb<=1 (row_comb absent or 1 -> no
  // taper at all, per range_doppler.cc's `valid = nof_range` default).
  uint32_t dealias_taper_bins = 4;  ///< Raised-cosine de-alias taper width (bins) before the comb cutoff
  uint32_t nms_range_bins     = 3;  ///< Non-max-suppression radius in range bins (0 disables)
  uint32_t nms_doppler_bins   = 3;  ///< Non-max-suppression radius in Doppler bins (0 disables)
  uint32_t max_detections     = 32; ///< Cap on reported detections per CPI after suppression
  /// Hard upper bound on DETECTABLE differential range, in metres; 0 = disabled (whole axis).
  ///
  /// The range axis spans nof_range * range_res, which at 273 PRB is ~10 km -- far beyond any range
  /// the receiver can physically observe. In the simulator the channel is capped at SENS_MAX_TAPS
  /// (255 taps @ 122.88 Msps = 622 m), so EVERY cell past that is by construction an artifact:
  /// window skirt, comb alias or noise. Measured 2026-07-30 on the 273 PRB passive scene: of 820
  /// reported detections, the median range was 3356 m and only 17 sat in the band the targets
  /// actually occupied -- the artifacts were consuming the whole max_detections budget and crowding
  /// out every real target.
  ///
  /// Zeroing those bins BEFORE CFAR (alongside zero_range_guard, which does the same job at the near
  /// end) is strictly better than filtering detections afterwards: it also keeps the artifacts out of
  /// the CFAR noise estimate, so the threshold over the real band stops being inflated by them.
  /// Set it from physics -- the CIR span in simulation, the instrumented range OTA -- not by tuning.
  float    max_range_m        = 0.0f;

  // Detector front-end. "fft" (default) = the legacy range-IFFT + Doppler-FFT/NUDFT chain, which
  // ASSUMES uniform, complete, real-symmetric sampling and therefore scatters each target's energy
  // into predictable ghost locations (harmonics from irregular slow-time sampling, pedestals from
  // gap interpolation, conjugate mirrors) -- every artifact this module has fought is a violation of
  // one of those FFT assumptions. "matched_filter" replaces the core with a hypothesis-test bank
  // matched to the ACTUAL sampling: per row, a range transform over ONLY the occupied subcarriers (no
  // interpolation -> no gap pedestal), each row energy-normalised by its own occupancy so a
  // low-occupancy slot contributes as much SIGNAL as a full one (removes the amplitude modulation
  // that irregular scheduling imposes -> removes the gating harmonics AT THE SOURCE), then coherent
  // NUDFT integration across slow-time at the true sample times. This is what proper gated-OFDM
  // passive radar does; the FFT is its fast approximation, valid only under full uniform sampling.
  // Needs the occupancy mask + raw (un-resampled) grid + row times (sensing_engine passes them).
  std::string detector          = "fft"; ///< "fft" | "matched_filter"
  bool        mf_per_row_norm   = true;  ///< matched_filter: equalise each row by its occupancy count

  // ---- Sub-slot CFR sampling (data-aided PDSCH tap) ------------------------------------------
  // The slow-time sample rate IS the radar PRF, and it caps the unambiguous bistatic range-rate at
  // c/(2*fc*t_slow). One row per slot (t_slow = 0.5 ms at 30 kHz SCS) gives only +/-48 m/s here, and a
  // target of speed v generates a rate up to 2v -- so anything past ~24 m/s aliases (measured
  // 2026-07-25: a 50 m/s target has no valid trajectory at all). But data-aided reconstruction already
  // gives H = Y/X at EVERY data RE, i.e. at every OFDM symbol, and the tap was collapsing all 14
  // symbols into a single row. Emitting one row per symbol GROUP raises the PRF up to 14x
  // (+/-670 m/s), which covers cars, drones and aircraft.
  //
  // The catch this must handle: a shorter row integrates fewer REs, so it is both SPARSER in frequency
  // (worse range profile / more sidelobes) and LOWER SNR (~10*log10(N_re) of coherent gain). Rows that
  // fail either test are worse than useless -- they inject noise into the slow-time sequence and feed
  // the CFAR false alarms. So grouping is ADAPTIVE: a group keeps absorbing the next symbol until it
  // satisfies BOTH gates below, and a tail group that can never satisfy them is merged backwards into
  // its predecessor rather than emitted. Set subslot_symbols = 0 (default) for the legacy
  // one-row-per-slot behaviour.
  uint32_t    subslot_symbols    = 0;      ///< target OFDM symbols per row; 0 = off (one row/slot)
  uint32_t    subslot_min_re     = 600;    ///< SPARSITY gate: min distinct REs for a row to stand alone
  float       subslot_min_snr_db = 10.0f;  ///< SNR gate: min estimated post-integration row SNR (dB)

  // CLEAN deconvolution (opt-in, clean_deconv.{h,cc}). Runs on the COMPLEX range-Doppler map before
  // CFAR. A strong scatterer does not sit in one cell: the range/Doppler windows spread it into a
  // deterministic sidelobe/pedestal skirt (its point-spread response), and CFAR happily reports the
  // brighter skirt bins as separate "targets" -- the dominant MOT ghost source measured 2026-07-25
  // (~2 ghosts per real detection, and the range-wide pedestal at the target's own velocity that
  // cfar_per_column/range_whiten only heuristically dent). CLEAN is the principled fix: iteratively
  // (1) find the brightest cell, (2) COHERENTLY subtract a loop-gain fraction of its full modelled
  // response (kr ⊗ kd, the separable window PSF) from the map, (3) bank the extracted component; then
  // restore each banked component as a narrow clean beam. What survives to CFAR is the real scatterers
  // plus noise, with each strong target's own coherent skirt removed -- so its sidelobes can no longer
  // masquerade as detections. Removes the LINEAR (window-sidelobe/pedestal) ghost family; it does NOT
  // by itself remove amplitude-gating harmonics (those are a data-domain effect, not the clean window
  // PSF -- for those the PSF would have to be modelled through the actual per-row occupancy, which
  // breaks the separable shift-invariant form; see clean_deconv.cc). Requires the shift-invariant
  // operator: gated to detector=="fft" with a uniform full-band comb (all row_comb<=1) -- CLEAN is
  // skipped (with a one-time warning) under matched_filter or a fused multi-comb grid.
  bool        clean_deconv           = false;
  // Occupancy-aware forward-model CLEAN (opt-in, requires clean_deconv=1). The separable window PSF
  // above only reaches clean-window sidelobes (measured ≤ −45 dB, so net-negative live 2026-07-25).
  // This variant models each component's PSF through the ACTUAL per-row 5G subcarrier occupancy mask:
  // a strong scatterer's slow-time signal is amplitude-modulated by the varying per-row occupancy, and
  // that modulation's spectrum is exactly the amplitude-gating Doppler-harmonic replica family (the
  // measured dominant ghost source). Forward-modelling a unit scatterer THROUGH the same occ mask
  // reproduces those replicas, so subtracting the modelled response removes them coherently -- which
  // the shift-invariant window PSF cannot. Cost: the operator is non-separable (occ differs per row),
  // so each component needs a full range+Doppler forward transform (~one CPI's worth each) -- keep the
  // component budget small (stop_db tighter). Consumes the RAW occupancy grid + true row times (like
  // matched_filter: sensing_engine bypasses gap-fill/resampling and passes occ_all + cpi_row_time),
  // and does NOT per-row-normalise (that would erase the very gating whose harmonics we model).
  bool        clean_occ_aware        = false;
  // 0 (default) => AUTO: derive the component budget each CPI from the previous CPI's raw detection
  // count (+ a small margin), same "measure, don't hand-pick a constant" pattern as mc_rank /
  // cfar_pfa / track_confirm_m. A positive value pins the budget manually.
  uint32_t    clean_max_components   = 0;
  uint32_t    clean_max_components_cap = 16;   ///< auto mode: hard ceiling on the derived budget
  float       clean_loop_gain        = 0.8f;   ///< gamma: fraction of the peak subtracted per iteration
                                               ///< (<1 for stable deconvolution; classic CLEAN uses 0.1-1)
  float       clean_stop_db          = 25.0f;  ///< stop once the residual peak is this many dB below the
                                               ///< CPI's initial peak (the deconvolution noise floor)
  uint32_t    clean_restore_bins     = 1;      ///< clean-beam half-width (bins) for restored components
                                               ///< (0 = pure single-bin delta)

  // Conjugate-image rejection. The range IFFT of a CFR with any real-valued (conjugate-symmetric)
  // component — e.g. the near-zero-Doppler residual a static/slow scatterer leaves after clutter
  // removal — produces a mirror "ghost" at range bin (nof_range-1-r) for a true scatterer at r. A
  // physical scatterer and its numerical image cannot both be real targets, so after NMS we drop the
  // weaker detection of any pair sitting at each other's range-mirror. Strongly suppressed already for
  // fast targets (their Doppler breaks the symmetry); this cleans up the static/slow residual.
  bool     conj_image_reject = true; ///< Enable range-conjugate-image ghost rejection (post-NMS)
  uint32_t conj_image_guard  = 4;    ///< Range-bin tolerance when matching a detection to its mirror
  // Doppler-harmonic rejection (opt-in): drop a detection at the same range as a STRONGER one whose
  // velocity magnitude is a near-integer multiple (2..harmonic_max_k) of the stronger one's -- the
  // scheduling-driven slow-time harmonics of a moving target. See range_doppler.cc's harmonic block.
  bool     harmonic_reject = false;
  uint32_t harmonic_guard  = 4;    ///< range-bin tolerance when pairing a harmonic to its fundamental
  uint32_t harmonic_max_k  = 4;    ///< highest harmonic order k to reject (2..k)
  float    harmonic_tol    = 0.15f;///< fractional tolerance on the integer velocity ratio
  float    harmonic_snr_margin = 6.0f; ///< the fundamental (lower |velocity|) may be at most this many
                                       ///< dB weaker than the harmonic and still trigger rejection --
                                       ///< guards a strong real detection against a weak low-velocity blip
  // Far-range harmonic (range-smeared pedestal) rejection (opt-in). The amplitude-gating harmonics of
  // a strong NEAR target smear across the WHOLE range axis at k*v_target (RVM-confirmed 2026-07-24,
  // PHASE2_MOT_MULTIUE_HANDOVER.md: far>500m ghosts logged at -15.6/-23.8 m/s = 2x/3x the +7.8 m/s
  // near target). These have NO co-located near-range parent, so same-range harmonic_reject and
  // track_harmonic_reject both miss them. This finds the scene's dominant NEAR-range Doppler
  // component(s) directly from the range-integrated power profile (no dependence on the parent being
  // a separate detection) and drops any FAR-range detection whose |velocity| is a near-integer
  // multiple (2..harmonic_max_k) of one. k=1 (same-velocity pedestal) is intentionally NOT rejected
  // here (that's cfar_per_column's job, and a genuine far target could share a near target's speed).
  bool     far_harmonic_reject = false;
  float    far_harmonic_far_m  = 500.0f;  ///< a detection past this range is a far-ghost candidate
  float    far_harmonic_near_m = 250.0f;  ///< dominant Doppler components are sought within this range

  // CPI-quality gate (opt-in). When a CPI's row spacing (T_slot) is much larger than the recent
  // typical value, that CPI physically lacks enough well-spaced samples to resolve a real target --
  // RVM-diagnostics-confirmed 2026-07-24 (PHASE2_MOT_MULTIUE_HANDOVER.md) that these "starved" CPIs
  // are exactly where amplitude-gating harmonic ghosts survive with NO co-existing real-target
  // reference for any detection- or track-level rejection to compare against (far_harmonic_reject and
  // the orphaned-harmonic probe both need such a reference and have none in a starved CPI). Rather
  // than filter individual detections post-hoc, suppress the WHOLE CPI's detections and let the
  // tracker coast through it -- physically honest ("this CPI can't be trusted"), and the threshold is
  // MEASURED (ratio to a running EMA of T_slot) rather than a fixed constant, since "normal" T_slot
  // depends on the cell's own traffic pattern.
  bool     cpi_quality_gate      = false;
  float    cpi_quality_max_ratio = 1.5f;  ///< gate a CPI whose T_slot exceeds this x the running EMA
  float    cpi_quality_ema_alpha = 0.2f;  ///< EMA smoothing factor for the running "typical" T_slot

  // Clutter-removal method (ECA_CLUTTER_HANDOVER.md). "mean" = legacy per-subcarrier slow-time mean
  // subtraction (default, behaviour-preserving). "eca+" = CFR-domain ECA/ECA+ oblique projection
  // (eca_clutter.{h,cc}): removes the whole near-zero-Doppler clutter band over a bounded delay
  // window *in the complex CFR domain* before the range IFFT, so the static/slow residual that
  // produces the conjugate mirror ghost never reaches the transform. Mean subtraction is the exact
  // special case eca_delay_max_m=full & eca_doppler_max_mps=0.
  std::string clutter_removal    = "mean"; ///< "mean" | "eca+"
  float       eca_delay_max_m    = 0.0f;   ///< ECA delay removal window [0, delay_max] m; <=0 => full range
  float       eca_doppler_max_mps = 0.5f;  ///< ECA Doppler removal half-band [-v,+v] around zero, in the
                                            ///< same BISTATIC RANGE-RATE units as sensing_rvm_t::vel_res_mps
                                            ///< (2026-07-23: that axis lost its erroneous monostatic factor
                                            ///< 2, so a given value here now covers half as many bins as it
                                            ///< did before — re-tune per-geometry, and note the effect in
                                            ///< bins is value/vel_res_mps, which changes with CPI length)

  // Fast-time (range/frequency-axis) window applied to the active subcarriers before the range
  // IFFT. "hann" (default, behaviour-preserving) has ~-31 dB first sidelobes; "chebyshev" swaps in
  // an equiripple Dolph-Chebyshev window (range_doppler.cc's build_chebyshev_window) so the strong
  // LOS/direct-path peak's own sidelobe skirt sits at a controlled, much deeper level instead of
  // bleeding into nearby range bins (SYNC_NOISE_HANDOVER.md's 55-100m LOS-skirt finding) -- at the
  // cost of a wider mainlobe (coarser range resolution) than Hann for the same sidelobe target.
  std::string range_window            = "hann"; ///< "hann" | "chebyshev"
  float       range_window_sidelobe_db = 60.0f;  ///< Chebyshev equiripple sidelobe level, dB (positive)
  // Spectral whitening (opt-in) before the range IFFT. Attenuates ONLY subcarriers whose slow-time
  // RMS exceeds the across-subcarrier median (scale = median/rms, so <=1 for hot subcarriers, exactly
  // 1 for at-or-below-median ones -- weak/empty subcarriers are never amplified). WHY: with the
  // irregular, per-slot-varying pdsch_data occupancy, a handful of subcarriers carry disproportionate
  // energy (sparse-aperture / Y-over-X estimation), and a near-impulse in the subcarrier domain
  // becomes a FLAT pedestal across the whole range axis after the IFFT -- RVM-confirmed 2026-07-24 as
  // a strong moving target's energy smeared across ~900 range bins at its own Doppler (and harmonics),
  // the dominant source of multi_target_tracker false tracks on a 2-target scene
  // (PHASE2_MOT_MULTIUE_HANDOVER.md). Soft-capping the hot subcarriers to the median flattens that
  // aperture and collapses the pedestal, without a scene-specific tuned threshold.
  bool        range_whiten            = false;
  // Non-uniform Doppler DFT (opt-in). The reference occurrences (PDSCH/CSI-RS slots) arrive at
  // IRREGULAR slot times, but the Doppler transform is a uniform FFT -- so the slow-time tone of a
  // moving target is evaluated as if evenly sampled, which turns it into a HARMONIC-rich signal
  // (strong ghosts at 2x/3x its velocity, RVM-confirmed 2026-07-24 -- see
  // PHASE2_MOT_MULTIUE_HANDOVER.md). Whether the rows are resampled onto a uniform grid
  // (interpolate=1, a chord approximation of a rotating phasor) or treated as uniform as-is
  // (interpolate=0), the harmonics appear. This mode instead evaluates a direct non-uniform DFT at
  // each row's ACTUAL time cpi_row_time[n]: X[k] = sum_n x[n]*w[n]*exp(-j2*pi*k*(t_n/T_mean)/N),
  // which reduces EXACTLY to the uniform FFT when the samples are evenly spaced, and removes the
  // harmonic generation at the source (leaving only a mild non-uniform-sampling sidelobe pedestal,
  // which range_whiten / cfar_per_column already handle). When enabled the slow-time resampling
  // (resample_slow_time) is bypassed -- the raw irregular rows are used directly. O(N^2) per range
  // bin (engine thread only, N = cpi_slots); the NUDFT matrix is built once per CPI.
  bool        doppler_nudft           = false;
  // Low-rank matrix completion of the slow-time x subcarrier CFR grid (opt-in), before range/Doppler.
  // Replaces the per-row linear frequency gap-fill (interp_freq_row) with a joint 2-D completion that
  // uses the physical prior that the CFR is LOW-RANK (rank ~ number of scatterers). This fills every
  // unobserved (unscheduled) entry with a coherent value, giving a CONSISTENT full aperture every
  // slot -- which removes the amplitude-gating that convolves each target's Doppler line with the
  // schedule mask and creates the 2x/3x harmonic ghosts AT THE SOURCE, rather than filtering their
  // symptoms downstream. Singular Value Projection (matrix_complete.{h,cc}), engine thread only.
  bool        slow_time_complete      = false;
  // mc_rank == 0 (default) => AUTO: derive the completion rank each CPI from the MEASURED number of
  // confirmed detections in the PREVIOUS CPI (+1 for the LOS/clutter residual), clamped to
  // [1, mc_rank_max], instead of a hand-picked constant. WHY: the correct rank IS the number of
  // scatterers in the scene, which a generic/independent receiver cannot know in advance and which
  // changes with the scene (1 target vs 5) -- a fixed rank tuned to one scene's target count is
  // exactly the kind of hand-picked constant this module avoids elsewhere (cfar_pfa, track_confirm_m).
  // The detection count is a noisy but already-measured, zero-extra-cost proxy: it under-counts by
  // design when targets are missed that CPI, which is safe (a too-low rank still recovers the
  // dominant scatterers; a too-high rank risks fitting noise into extra "phantom" components).
  // Explicit mc_rank > 0 still pins a manual value.
  uint32_t    mc_rank                 = 0;
  uint32_t    mc_rank_max             = 8;  ///< AUTO mode only: ceiling on the derived rank
  uint32_t    mc_iters                = 15; ///< SVP iterations (projection + data-consistency)
  uint32_t    mc_power_iters          = 1;  ///< extra subspace power iterations per projection

  // L1/FISTA sparse Doppler verification (opt-in). Re-solves ONLY the range bins the dense CFAR pass
  // already flagged (not the whole grid -- see sparse_doppler.h for why) via L1-regularized recovery
  // over the SAME irregular sample times as doppler_nudft, and drops a detection whose Doppler bin
  // does not survive as a significant peak in the sparse solution -- i.e. it needed to borrow energy
  // from a neighbouring bin in the dense transform, revealed as unnecessary once solved sparsely.
  // Offline-validated (Python cross-check before the C++ port, see PHASE2_MOT_MULTIUE_HANDOVER.md):
  // ~100+ dB peak-to-harmonic ratio on a gappy single tone that gave the dense NUDFT only ~5.5 dB,
  // while correctly preserving a second, weaker, genuinely-present target at its right relative power.
  bool        doppler_sparse            = false;
  uint32_t    doppler_sparse_iters      = 40;   ///< FISTA iterations per verified range bin
  // doppler_sparse_lambda_scale is now only the STARTING point for a CLOSED-LOOP adaptation (in
  // range_doppler.cc), not a fixed multiplier. WHY: the fixed "universal threshold" (scale=1.0) is a
  // conservative, worst-case asymptotic bound -- live-confirmed 2026-07-24
  // (PHASE2_MOT_MULTIUE_HANDOVER.md) to over-suppress real targets on this scene (detections/CPI
  // collapsed to 0-2, long zero-detection stretches), the same "don't hand-pick a constant" problem
  // this module fixed elsewhere (cfar_pfa, track_confirm_m, mc_rank). Each CPI, after doppler_sparse
  // filtering, the scale is adjusted based on the MEASURED number of surviving detections: too few
  // (below doppler_sparse_target_min_det) relaxes it (divide by doppler_sparse_adapt_rate, i.e.
  // smaller threshold, more survives); too many (above doppler_sparse_target_max_det, suggesting
  // ghosts are getting back through) tightens it (multiply by doppler_sparse_adapt_rate). Clamped to
  // [doppler_sparse_lambda_min, doppler_sparse_lambda_max].
  float       doppler_sparse_lambda_scale = 1.0f; ///< initial/seed value for the adaptive loop
  float       doppler_sparse_lambda_min   = 0.05f;
  float       doppler_sparse_lambda_max   = 1.5f;
  float       doppler_sparse_adapt_rate   = 0.85f; ///< per-CPI multiplicative step (< 1)
  uint32_t    doppler_sparse_target_min_det = 1;  ///< below this -> relax (assume real targets missed)
  uint32_t    doppler_sparse_target_max_det = 6;  ///< above this -> tighten (assume ghosts leaking back)
  float       doppler_sparse_peak_ratio  = 0.05f; ///< a detection's sparse-domain power must be at
                                                  ///< least this fraction of that row's max sparse
                                                  ///< power to be kept (FISTA already drives rejected
                                                  ///< bins to near-exactly zero, so this is a lenient
                                                  ///< safety margin, not the main discriminator)
  // SEPARATE, STRICTER threshold for the orphaned-harmonic sub-bin probe (range_doppler.cc). The
  // probe rejects a detection at bin n when a sub-harmonic bin (n/2, n/3) carries at least this
  // fraction of the row's max sparse power. It MUST be distinct from -- and much higher than --
  // doppler_sparse_peak_ratio: reusing the lenient peak-survival ratio (0.05 = -13 dB) for the
  // reject-me-as-a-harmonic decision caused "sidelobe self-destruction" (2026-07-24,
  // PHASE2_MOT_MULTIUE_HANDOVER.md) -- a real strong target's own window sidelobe / SFO-jitter
  // leakage at n/2 exceeded -13 dB, so the target flagged ITSELF as a harmonic of a ghost that
  // wasn't there and dropped, collapsing detections to zero. A genuine sub-harmonic parent sits
  // within a few dB of the harmonic; a mere sidelobe is 15-20 dB down, so ~0.25 (-6 dB) cleanly
  // separates them.
  float       doppler_sparse_harmonic_ratio = 0.25f;

  // Per-CPI target track (target_tracker.{h,cc}): 2-state constant-velocity Kalman filter over the
  // detection stream. Off by default (purely additive output; the raw detections are unchanged).
  // Defaults measured on tests/sensing_sim at 100 MHz/273 PRB, 2026-07-23 -- re-tune per geometry:
  //   track_r_var_m2      observed detrended residual variance of the reported range (2.6 m^2). Pure
  //                       bin quantization would be only Delta^2/12 = 0.78 m^2 at range_res 3.05 m;
  //                       the excess is genuine detection noise, so the larger measured value is used.
  //   track_q_accel       white-noise acceleration PSD. The sim target's bistatic range-rate drifts
  //                       ~0.05 m/s^2, so q ~ 0.05^2; raise it for manoeuvring targets.
  //   track_gate_sigma    normalized-innovation gate in sigmas (3 => accept y^2/S <= 9).
  // Tune r/q by CONSISTENCY, not by eye: sensing_track_t::nis (logged per CPI) should average ~1 for
  // this 1-D measurement when the pair matches reality -- persistently >>1 means R/q are too small.
  std::string track_model        = "cv";   ///< "cv" (2-state const-velocity) | "ca" (3-state
                                           ///< const-acceleration). CA carries acceleration in the
                                           ///< state and predicts it forward, so it tracks continuous
                                           ///< curvature (a turning target) tightly where CV can only
                                           ///< lag; track_q_accel is then a JERK PSD, not accel PSD.
  float    track_init_acc_var    = 100.0f; ///< CA only: initial acceleration variance (m/s^2)^2
  bool     track_enable          = false;
  float    track_r_var_m2        = 2.6f;   ///< R: RANGE measurement variance, m^2
  float    track_rv_var_m2s2     = 1.0f;   ///< R: RANGE-RATE (Doppler) measurement variance, (m/s)^2
                                           ///< -- the Doppler bin (~0.6 m/s here) is coarser/noisier
                                           ///< than range, so this is deliberately loose; the range
                                           ///< measurement carries most of the position weight
  float    track_q_accel         = 0.0025f;///< q: accel PSD, (m/s^2)^2 per Hz
  float    track_gate_sigma      = 3.0f;   ///< association gate, sigmas
  float    track_init_vel_var_m2s2 = 25.0f;///< initial rate variance (Doppler seed is coarse)
  uint32_t track_max_coast       = 5;      ///< consecutive coasted CPIs before the track is dropped

  // Adaptive process noise (innovation-based, Mehra 1970-style): track_q_accel above is a BASELINE,
  // not a fixed value used verbatim. Added 2026-07-23 after a fixed-q filter coasted 5 CPIs through a
  // real acceleration leg and got dropped+re-initialised (see var_report.html) -- the user's point
  // was that q must not require knowing the route in advance. An EWMA of the per-CPI NIS multiplies
  // q: sustained NIS>>1 (constant-velocity model failing to explain the innovations, i.e. a
  // maneuver) inflates q, which widens BOTH the prediction uncertainty and the association gate
  // (S = P00_pred + R depends on the now-larger P00_pred) -- one mechanism fixes both the lag and
  // the over-tight gating that caused the coast/drop cascade. When the target returns to constant
  // velocity, NIS_ewma decays back toward 1 and q returns to baseline on its own; nothing here is
  // tuned per trajectory.
  bool     track_q_adapt_enable  = true;
  float    track_nis_ewma_alpha  = 0.3f;   ///< EWMA smoothing for NIS (same order as SFO_EMA_ALPHA)
  float    track_q_mult_max      = 30.0f;  ///< cap on the inflation factor (bounds worst-case q)
  float    track_q_adapt_window_sigma = 8.0f; ///< a near-miss within this many sigmas feeds the NIS
                                              ///< EWMA even though it fails the (narrower) accept gate
                                              ///< -- this is what lets q inflate DURING a manoeuvre
                                              ///< that has pushed the target just outside the gate,
                                              ///< while still ignoring far mirrors/false alarms

  // Multi-object tracking (multi_target_tracker.{h,cc}): wraps N per-track filters (each an
  // independent target_tracker above), a global greedy nearest-neighbour association across all
  // tracks vs. this CPI's detections, and an M-of-N track-initiation policy so CFAR false alarms /
  // mirror ghosts don't spawn confirmed tracks on a single hit. Single-target behaviour is the
  // special case max_tracks=1, confirm_m=1. See PHASE2_MOT_MULTIUE_HANDOVER.md Part 1.
  uint32_t track_max_tracks       = 16;    ///< cap on simultaneously maintained tracks (tentative +
                                           ///< confirmed); matches max_detections' CFAR cap
  // track_confirm_m: 0 (default) => AUTO-DERIVE M each CPI from the MEASURED detection density
  // (mean detections/CPI) and track_confirm_target_pfa, rather than a hand-picked constant --
  // solve_confirm_m() in multi_target_tracker.cc finds the smallest M such that
  // P(Binomial(track_confirm_n, p_hit) >= M) <= track_confirm_target_pfa, where p_hit is the
  // association gate's area as a fraction of the full range-Doppler grid area, times the measured
  // mean detections/CPI. A fixed M/N tuned to one scene's false-alarm density silently under- or
  // over-confirms once clutter/SNR conditions change -- LIVE-CONFIRMED 2026-07-24
  // (PHASE2_MOT_MULTIUE_HANDOVER.md open question #2): a hand-picked 3-of-5 let a range=5146 m
  // ghost and several extra ids confirm as tracks on a 2-real-target scene once CFAR was
  // running ~14-16 raw detections/CPI (mostly clutter/mirror residue, not 2 real targets).
  // NOTE (important limitation): this bounds RANDOM/INCOHERENT false alarms landing inside a gate
  // by chance. A COHERENT persistent artifact (mirror ghost, unremoved clutter tap) recurs at
  // essentially the SAME predicted location every CPI and will confirm regardless of M/N -- that
  // failure mode belongs to conj_image_reject/ECA+ upstream, not this gate. Explicit >0 pins a
  // manual value (bypasses auto-derivation entirely).
  uint32_t track_confirm_m        = 0;
  uint32_t track_confirm_n        = 5;     ///< N: sliding window (CPIs) over which the (auto or
                                           ///< manual) M hits count
  float    track_confirm_target_pfa = 1e-3f; ///< AUTO mode only (track_confirm_m==0): target
                                             ///< probability that a purely random/incoherent false
                                             ///< detection stream confirms a spurious track within
                                             ///< the N-CPI window. See track_confirm_m's comment.
  float    track_assoc_gate_sigma = 5.0f;  ///< association gate (sigmas) for matching a detection to
                                           ///< an existing track's PREDICTION. Deliberately >=
                                           ///< track_gate_sigma: this is a coarse pre-filter to route
                                           ///< detections to the right track / prevent two tracks
                                           ///< stealing one detection; each per-track filter still
                                           ///< applies its own tight track_gate_sigma update gate.
  // Association-gate CEILING, independent of the per-track filter's own (adaptive-q-inflated) sigma.
  // WHY: multi_target_tracker's association step uses max(lt.sigma_range_m, baseline) as the gate's
  // radius -- but sigma_range_m grows UNBOUNDED-ish while a track coasts (adaptive q, capped only at
  // track_q_mult_max=30x, compounds every predict step). LIVE-CONFIRMED 2026-07-24
  // (PHASE2_MOT_MULTIUE_HANDOVER.md): this is what let scattered CFAR noise get scooped up as
  // "associated" to a coasting tentative track, satisfying M-of-N even at a low auto-derived M --
  // the gate had simply grown too wide to mean anything. These cap the ASSOCIATION radius only (a
  // routing decision); the per-track Kalman filter's own P/gate for its update-gate is untouched.
  float    track_assoc_gate_max_sr_m   = 15.0f;  ///< hard ceiling on the association gate's range
                                                 ///< radius component, metres (baseline ~1.6 m)
  float    track_assoc_gate_max_sv_mps = 10.0f;  ///< hard ceiling on the association gate's
                                                 ///< range-rate radius component, m/s (baseline ~1 m/s)
  // Track-level Doppler-harmonic rejection (opt-in). The detection-domain harmonic_reject
  // (range_doppler.cc) drops per-CPI harmonic detections, but a harmonic that leaks through on a few
  // CPIs can still CONFIRM a track that then COASTS -- a Kalman track is a smooth, coherent
  // trajectory whether it follows a real target or a harmonic ghost, so the filter itself cannot
  // reject it (PHASE2_MOT_MULTIUE_HANDOVER.md). This suppresses a CONFIRMED track whose |range-rate|
  // is a near-integer multiple (2..harmonic_max_k, tol harmonic_tol -- shared with the detection-
  // domain knob) of another confirmed track's at essentially the same range: the lower-|velocity|
  // member is the fundamental (the real target) and is kept.
  bool     track_harmonic_reject   = false;
  float    track_harmonic_range_m  = 10.0f; ///< max range separation (m) to treat two tracks as co-located

  // Track-level flicker (power-continuity) rejection (opt-in) -- a Track-Before-Detect-style energy
  // consistency test. MOTIVATION (measured 2026-07-25, PHASE2_MOT_MULTIUE_HANDOVER.md): the residual
  // confirmed-track ghosts are amplitude-gating Doppler harmonics -- their strength is tied to the
  // INSTANTANEOUS 5G slot occupancy, so their per-CPI SNR is bursty (large frame-to-frame dB swings,
  // on/off with the scheduler), whereas a real reflector's SNR trajectory is smooth. A tighter CFAR
  // pfa can't separate them (they're structured, not statistical -- the cfar_target_fa 4->1 test left
  // track precision flat), but their DEFINING signature is exactly this flicker. Each track carries an
  // EWMA of |Δsnr_dB| between consecutive accepted detections; a CONFIRMED track whose jitter EWMA
  // exceeds track_flicker_max_db (after >= track_flicker_min_updates samples, so a young track isn't
  // judged on noise) is withheld from the reported set -- same post-hoc, report-level filtering layer
  // as track_harmonic_reject, the per-track Kalman/gating logic is untouched. Threshold is a physical
  // dB level (a real target rarely swings > a few dB/CPI; a gated harmonic routinely swings 10+);
  // A/B-calibrate per scene like the other measured constants.
  bool     track_flicker_reject      = false;
  float    track_flicker_max_db      = 6.0f;  ///< max EWMA |Δsnr_dB|/update for a track to be reported
  uint32_t track_flicker_min_updates = 3;     ///< min accepted detections before the flicker gate applies
  float    track_flicker_ewma_alpha  = 0.4f;  ///< EWMA smoothing for the per-track SNR-jitter estimate

  // ---- Receive-array angle of arrival (PHASE3_AOA_MULTISTATIC_HANDOVER §5.4) -------------------
  // Opt-in and inert by default: with aoa_enable off the taps submit a single antenna and the engine
  // allocates no per-antenna grid, so a single-channel receiver (B210) costs exactly nothing.
  //
  // SET EXPECTATIONS CORRECTLY (§5, stated here so it isn't mis-sold downstream): AoA does NOT fix
  // ghosts. A ridge/scheduling-harmonic ghost shares its parent target's wavefront, so it shares its
  // bearing and passes every bearing consistency test there is. What AoA buys is ACCURACY and
  // single-receiver observability (one Tx-Rx pair localises, via ray-ellipse intersection); ghost
  // rejection comes from having a third independent RANGE ellipse, not from bearing.
  bool        aoa_enable = false;
  // Element offsets "x,y;x,y;..." in metres, in the ARRAY frame. MUST match the geometry the air
  // actually carries -- in simulation that is `[sensing_channel] rx_array`, and the two disagreeing is
  // a silent-garbage failure of exactly the same class as a wrong csirs_monitor scramb_id.
  std::string rx_array = "";
  float       rx_array_boresight_deg = 0.0f; ///< rotation of the array frame into ENU (deg CCW from east)
  // "beamscan" (default; any element count, conventional/Bartlett beamforming over the manifold with a
  // parabolic-in-dB peak refinement), "interferometry" (exact two-element closed form; automatically
  // used whenever the array has exactly 2 elements), or "music" (>=3 elements; builds a covariance
  // from the detection cell's immediate neighbours as snapshots, since one cell alone gives a rank-1
  // covariance that MUSIC cannot use, and falls back to beamscan if that is not satisfiable).
  std::string aoa_estimator = "beamscan";
  float       aoa_scan_step_deg = 1.0f; ///< manifold scan step for beamscan/MUSIC
  // Minimum detection SNR to attempt a bearing. Below it the azimuth is left ABSENT rather than
  // reported as noise -- isac-core treats a missing azimuth as a plain 2-D measurement and keeps the
  // detection, so withholding costs a little accuracy while reporting garbage costs correctness.
  float       aoa_min_snr_db = 6.0f;
  // Half-width (bins) of the local re-peak search around each detection's (range, Doppler) cell. The
  // AoA path deliberately does not reproduce the main pipeline's sync corrections or sub-bin
  // interpolation (see isac_aoa.h), so the true peak can sit a bin or two away; searching absorbs that.
  uint32_t    aoa_cell_search_bins = 2;
  // ENU direction (deg) the array FACES, used to pick which half-plane a collinear array scans. A
  // linear array physically cannot separate a bearing from its mirror about the array axis, so one
  // half-plane must be chosen; -1000 (default) means "the array's own normal", i.e. boresight + 90.
  float       aoa_broadside_deg = -1000.0f;
  // Direct-path self-calibration (§5.4 item 3): estimate the per-channel phase offsets against the
  // LOS tap, whose bearing is KNOWN from the surveyed illuminator position and which sits at zero
  // Doppler and the configured nominal_los_range_m. Continuous, no injected tone, no anechoic chamber.
  bool        aoa_selfcal = false;

  std::string out_path = "/tmp/oaiue_sensing"; ///< Output path prefix for RVM raster / detections CSV

  // DetectionReport metadata (central-node detection-bus contract).
  std::string rx_id          = "rx1";  ///< Logical receiver id (DetectionReport.rx_id)
  float       rx_pos_x       = 0.0f;   ///< Surveyed receiver ENU x, metres
  float       rx_pos_y       = 0.0f;   ///< Surveyed receiver ENU y, metres
  std::string illuminator_id = "gnb1"; ///< Logical illuminator id for Tx<->Rx pairing (Illuminator.id)
  float       tx_pos_x       = 0.0f;   ///< Surveyed transmitter ENU x, metres
  float       tx_pos_y       = 0.0f;   ///< Surveyed transmitter ENU y, metres
  std::string report_path    = "";     ///< DetectionReport JSON-lines path. Empty -> "<out_path>_reports.jsonl"
  std::string report_endpoint = "";    ///< Optional ZeroMQ PUB bind endpoint (e.g. "tcp://127.0.0.1:5556")
};

/// Range-velocity map (RVM). Power is row-major, range-major: power[range_bin*nof_doppler_bins + doppler_bin].
struct sensing_rvm_t {
  std::vector<float> power;
  uint32_t           nof_range_bins   = 0;
  uint32_t           nof_doppler_bins = 0;
  float              range_res_m      = 0.0f;
  float              range_max_m      = 0.0f;
  float              vel_res_mps      = 0.0f; ///< Doppler bin size as BISTATIC RANGE-RATE (dR/dt), m/s.
                                              ///< NOT a monostatic radial velocity: f_d = Ṙ/λ, no
                                              ///< round-trip factor 2 (it is already inside the
                                              ///< bistatic path R). Matches the range axis, which
                                              ///< reports differential bistatic range. See the
                                              ///< 2026-07-23 calibration-fix note in range_doppler.cc.
  float              vel_max_mps      = 0.0f; ///< +/- unambiguous bistatic range-rate extent, m/s
};

/// A single CA-CFAR detection in the RVM.
struct sensing_detection_t {
  uint32_t range_bin   = 0;
  uint32_t doppler_bin = 0;
  float    range_m     = 0.0f;
  float    vel_mps     = 0.0f;
  float    snr_db      = 0.0f;
  // Receive-array AoA (isac_aoa.h). azimuth_valid == false => this receiver has no array, or the
  // estimate failed its quality gate; the DetectionReport then simply omits the azimuth fields and
  // the central node treats the detection as a plain 2-D range/rate measurement.
  bool     azimuth_valid   = false;
  float    azimuth_deg     = 0.0f; ///< ENU bearing from this receiver, deg CCW from east
  float    azimuth_std_deg = 0.0f; ///< 1-sigma (CRB) uncertainty of the above
};

/// A synthetic target to inject into the CFR for testing (delay/Doppler/gain).
struct sensing_target_t {
  double delay_s    = 0.0;
  double doppler_hz = 0.0;
  double gain       = 1.0;
};

/**
 * @brief Per-slot snapshot handed from the RT tap to the engine thread.
 *
 * Pre-allocated once and recycled through a free/ready queue pair so the RT tap never
 * allocates. Mirrors srsUE sensing_slot_t.
 */
struct sensing_slot_t {
  uint32_t          slot_idx = 0;                 ///< Absolute slot index
  float             slot_frac = 0.0f;              ///< Sub-slot offset within that slot, in slots [0,1).
                                                   ///< 0 for a whole-slot estimate; non-zero only from the
                                                   ///< sub-slot tap (see the sub-slot section above).
  nr_isac_source_t  source   = NR_ISAC_SRC_CSI_RS; ///< Which reference produced this row (fusion diagnostics)
  nr_isac_carrier_t carrier  = {};                ///< Carrier geometry valid for this snapshot

  std::vector<icf_t>     h;     ///< Per-RE CFR Ĥ at the reference REs, ANTENNA-MAJOR: h[a*nof_re + i].
                                ///< nof_ant == 1 for every non-AoA caller, so this is the legacy layout.
  std::vector<uint32_t> k_abs; ///< Absolute subcarrier index (relative to CRB0) for each RE in @ref h
  std::vector<uint32_t> l_sym; ///< OFDM symbol index for each RE in @ref h
  uint32_t              nof_ant      = 1; ///< receive antennas carried in @ref h (1 unless AoA is on)
  uint32_t              nof_re       = 0;
  uint32_t              comb_spacing = 0; ///< Subcarrier spacing of the reference comb
  uint32_t              period_slots = 0; ///< Slow-time sampling period, in slots
  float                 noise_var    = 0.0f; ///< Per-RE noise power of this estimate (0 => unknown, equal weight)
};

} // namespace nr_isac

#endif // NR_ISAC_DEFS_H
