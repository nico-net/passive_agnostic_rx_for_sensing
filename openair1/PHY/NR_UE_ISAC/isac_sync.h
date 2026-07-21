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

/*! \file openair1/PHY/NR_UE_ISAC/isac_sync.h
 * \brief OTA STO/CFO/SFO/CPE synchronisation for the sensing CFR grid (ota_sync_passive_ue.md).
 *
 * Phase 1 (this file, so far): fine (sub-sample) STO tracking and correction, per accumulated
 * CPI row, on the RAW per-subcarrier grid, before Stage-4b interpolation. See
 * docs/NR_UE_ISAC_sync_gap_analysis.md for the full design rationale and hook-point analysis.
 * Later phases (CFO, SFO, closed-loop LOS pinning) extend this same per-row LOS tracking and
 * will be added to this file as they land.
 */

#ifndef NR_ISAC_SYNC_H
#define NR_ISAC_SYNC_H

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "defs_nr_UE_ISAC.h"
#include "isac_fft.h"

namespace nr_isac {

/**
 * @brief Shared utility: builds one row's compact CIR from its occupied subcarriers.
 *
 * Gathers a row's real samples at their native comb stride (bailing if a nominal position isn't
 * actually occupied -- a rare same-slot multi-source merge where two combs overlap
 * inconsistently) and IFFTs them via isac_fft (arbitrary N, since row width varies with
 * source/comb). Factored out of cpi_sto_tracker (Phase 1) so Phase 3's SFO tracker can reuse the
 * exact same CIR-building logic with its own, differently-shaped peak-search strategy, per Phase
 * 0's "reuse the existing computation, don't duplicate it" rule -- what legitimately differs
 * between phases is how each searches the resulting CIR for a peak, not how the CIR is built.
 *
 * RT-safety: engine thread only. FFT plans are cached by compact-CIR length; steady state (once
 * every distinct row width has been seen) is allocation-free. Each tracker owns its own instance
 * (not shared across trackers) -- trivially cheap to warm up twice, and keeps trackers decoupled.
 */
class row_cir_builder
{
public:
  /// Returns false (cir left unspecified) if the row's occupancy isn't a clean single native comb,
  /// or is too short (fewer than MIN_CIR_LEN samples, see .cc) to be useful.
  bool build(const icf_t* row, const uint8_t* mask, uint32_t nof_subc, uint32_t comb, std::vector<icf_t>& cir);

private:
  fft_plan* plan_for(uint32_t m);

  // Keyed by compact-CIR length (M); only a handful of distinct M recur per CPI (one per native
  // comb in play), so a small linear-scan cache is fine and avoids recomputing Bluestein chirps.
  std::vector<std::pair<uint32_t, std::unique_ptr<fft_plan>>> plan_cache_;
  std::vector<icf_t> compact_; ///< scratch: compacted occupied samples for one row
};

/// Per-row LOS/direct-path delay estimate (Phase 1; peak_val added for Phase 2's CFO/CPE reuse).
struct los_row_estimate_t {
  uint32_t row      = 0;     ///< index into the CPI's accumulated row array
  double   time_s   = 0.0;   ///< row's absolute slow-time position within the CPI, seconds
  uint32_t peak_bin = 0;     ///< integer CIR bin nearest the LOS tap (comb-invariant, see .cc)
  double   frac_bin = 0.0;   ///< sub-bin parabolic offset from peak_bin, in bins, range [-0.5, 0.5]
  icf_t    peak_val = icf_t(0.0f, 0.0f); ///< complex CIR sample at peak_bin (pre-STO-correction);
                                         ///< Phase 2 reads its phase, no second CIR pass needed
  bool     valid    = false; ///< false if the row's occupancy wasn't a clean uniform comb, or too short
};

/// Outcome of fitting sub-bin delay vs. time across a CPI's valid rows (Phase 1 classification).
struct sto_fit_result_t {
  uint32_t n_valid        = 0;     ///< valid rows that went into the fit
  double   slope_bins_per_s = 0.0; ///< fitted sub-bin delay drift rate (bins/second)
  double   mean_frac_bin   = 0.0;  ///< mean sub-bin fractional delay across valid rows, in bins
  double   drift_bins_cpi  = 0.0;  ///< |slope * CPI time span|, in range bins -- the classification statistic
  bool     is_constant     = false; ///< true => classified as fine STO (corrected here); false => SFO leaking
                                     ///< through, hand off to Phase 3 (no correction applied here)
};

/**
 * @brief Phase 1: per-CPI fine (sub-sample) STO tracking and correction.
 *
 * For each committed row with a clean, uniformly-spaced occupancy (a single dominant native
 * comb), IFFTs the row's occupied subcarriers (via isac_fft, since row width varies with
 * source/comb) to a compact CIR, locates the LOS tap near the known fixed group-delay bin, and
 * refines its position with parabolic interpolation across the 3 bins straddling the peak. Fits
 * the per-row sub-bin delay vs. each row's own absolute time across the CPI (irregular spacing):
 * a value that stays constant is pure (fine) STO and is corrected by nulling ONLY that common
 * sub-bin component with a frequency-domain phase ramp applied identically to every row's
 * occupied columns -- the row's *integer* CIR bin (the diagnosed ~49 m / bin-6 group delay) is
 * deliberately left untouched, since removing it is a separate, out-of-scope concern (see
 * docs/NR_UE_ISAC_sync_gap_analysis.md). A value drifting linearly with row time is SFO leaking
 * through the fine-STO estimate; no correction is attempted here, only reported (Phase 3's job).
 *
 * RT-safety: engine thread only, called from sensing_engine's CPI-close path, before Stage-4b
 * interpolation. FFT plans are cached by compact-CIR length across CPIs, so steady state (once
 * every distinct row width has been seen) is allocation-free.
 */
class cpi_sto_tracker
{
public:
  /**
   * @param h_cpi          raw per-subcarrier CPI grid, row-major [cpi_rows][nof_subc]; mutated in
   *                       place when a constant (fine) STO is found and corrected
   * @param occ_all        per-row occupancy mask, row-major [cpi_rows][nof_subc]
   * @param cpi_rows       number of committed rows this CPI
   * @param nof_subc       grid width (carrier.nof_prb * 12)
   * @param row_comb       per-row native comb (row_native_comb), length cpi_rows
   * @param row_time_slots per-row absolute slow-time position, in slots, length cpi_rows
   * @param carrier        carrier geometry for this CPI (SCS drives the bin/delay conversion)
   */
  sto_fit_result_t process(icf_t*                   h_cpi,
                           const uint8_t*           occ_all,
                           uint32_t                 cpi_rows,
                           uint32_t                 nof_subc,
                           const uint32_t*          row_comb,
                           const double*            row_time_slots,
                           const nr_isac_carrier_t& carrier);

  /// Per-row LOS estimates from the most recent process() call, keyed by row index. Phase 2's CFO
  /// tracker reuses these directly rather than re-running the per-row CIR/peak search.
  const std::vector<los_row_estimate_t>& last_row_estimates() const { return rows_; }

private:
  bool estimate_row(const icf_t*   row,
                    const uint8_t* mask,
                    uint32_t       nof_subc,
                    uint32_t       comb,
                    uint32_t       nominal_bin,
                    los_row_estimate_t& out);
  void apply_correction(icf_t* h_cpi, const uint8_t* occ_all, uint32_t cpi_rows, uint32_t nof_subc,
                        double mean_frac_bin);

  row_cir_builder     cir_builder_; ///< shared CIR-building logic (see class comment above)
  std::vector<icf_t>  cir_;         ///< scratch: CIR for one row
  std::vector<los_row_estimate_t> rows_; ///< per-row results, current CPI
};

/// Outcome of the per-CPI residual-CFO fit + CPE correction (Phase 2).
struct cfo_fit_result_t {
  uint32_t n_valid                 = 0;   ///< valid LOS-tap rows used in the line fit
  double   cfo_hz                  = 0.0; ///< this CPI's raw fitted residual CFO (line-fit slope / 2*pi), Hz
  double   cfo_hz_filtered         = 0.0; ///< alpha-beta-filtered CFO estimate, tracked slowly across CPIs
  double   cfo_rate_hz_per_cpi     = 0.0; ///< alpha-beta filter's tracked drift rate, Hz per CPI (diagnostic)
  double   residual_phase_rms_rad  = 0.0; ///< RMS of (observed - fitted-line) phase across valid rows
};

/**
 * @brief Phase 2: residual-CFO estimation (from Phase 1's per-row LOS-tap phase) + per-row CPE
 * de-rotation.
 *
 * Reuses Phase 1's already-computed per-row LOS CIR peaks (cpi_sto_tracker::last_row_estimates())
 * rather than re-running the per-row CIR/peak search. Phase 0 confirmed neither existing
 * frequency-offset facility (`--ue-fo-compensation`'s one-shot acquisition-time LO retune, or
 * `--cont-fo-comp`'s 20 ms PBCH-driven PI loop) tracks at sensing-grade rate/resolution, so this is
 * an independent, ISAC-path-only loop that does not touch either existing mechanism (see
 * docs/NR_UE_ISAC_sync_gap_analysis.md section 0).
 *
 * For each row with a valid LOS tap: phase(row) = angle(peak_val), sequentially unwrapped across
 * rows in time order, then least-squares fit vs. each row's own absolute time (irregular spacing,
 * same as Phase 1). The fitted slope is the residual CFO (rad/s -> Hz), kept for reporting and the
 * slow cross-CPI alpha-beta tracker below. The actual correction fully de-rotates each valid row by
 * its OWN raw observed LOS-tap phase (not just the line-fit residual): CFO/CPE manifests as a phase
 * rotation common to every subcarrier of a row (to first order, no ICI modelled), so this single
 * reference tap IS that row's common-phase-error, and subtracting it whole removes both the smooth
 * CFO trend and whatever doesn't fit the line in one step -- there is no separate "de-trend, then
 * remove residual" pass.
 *
 * Because this is a UNIFORM (not per-subcarrier-linear) phase rotation, it commutes with Phase 1's
 * frequency-ramp STO correction and leaves range-domain structure (magnitude/delay, per-row) exactly
 * as-is -- only row-to-row (Doppler-axis) phase coherence changes, which is the smear this phase
 * targets.
 *
 * Known limitation (see gap-analysis doc): the sequential unwrap assumes the true residual CFO
 * doesn't rotate phase by >= pi between consecutive valid rows. With fused sources (this repo's
 * live config), frequent PDSCH rows keep row-to-row gaps small; a csirs_monitor-only source set
 * (Phase 8, 20 ms period) is far more exposed to unwrap ambiguity at larger residual CFO -- flagged
 * for that phase's mandatory re-validation, not solved here.
 */
class cpi_cfo_tracker
{
public:
  /**
   * @param h_cpi    raw per-subcarrier CPI grid, row-major [][nof_subc]; mutated in place
   * @param occ_all  per-row occupancy mask, row-major [][nof_subc]
   * @param nof_subc grid width
   * @param rows     Phase 1's per-row LOS estimates for this CPI (cpi_sto_tracker::last_row_estimates())
   */
  cfo_fit_result_t process(icf_t* h_cpi, const uint8_t* occ_all, uint32_t nof_subc,
                           const std::vector<los_row_estimate_t>& rows);

private:
  bool   state_init_          = false;
  double cfo_state_hz_        = 0.0; ///< alpha-beta filter position state (Hz)
  double cfo_rate_hz_per_cpi_ = 0.0; ///< alpha-beta filter velocity state (Hz/CPI tick)
};

/// Outcome of the per-CPI SFO delay-drift fit + correction (Phase 3).
struct sfo_fit_result_t {
  uint32_t n_candidate           = 0;     ///< rows with a usable tentative peak from the sequential walk
  uint32_t n_excluded_isi        = 0;     ///< candidate rows excluded for anomalous out-of-window CIR energy
  uint32_t n_fit                 = 0;     ///< rows actually used in the line fit (candidates minus ISI exclusions)
  double   sfo_ppm               = 0.0;   ///< fitted fractional sample-clock error, ppm (dimensionless slope * 1e6)
  double   sample_clock_error_hz = 0.0;   ///< sfo_ppm expressed in absolute Hz at this build's actual sample rate
  bool     corrected             = false; ///< true if n_fit met the minimum and a correction was applied
};

/**
 * @brief Phase 3: SFO (sample-clock error) delay-drift estimation and correction.
 *
 * The dominant contributor to the Doppler-axis smear the guard-band widening previously masked.
 * Unlike Phase 2, this phase does NOT reuse Phase 1's per-row LOS estimates: Phase 1 searches a
 * small, FIXED window around a constant nominal bin (right for fine/near-zero drift), but SFO can
 * walk the LOS peak by hundreds of range bins over a multi-second CPI (e.g. ~3000 m / ~367 bins at
 * 2 ppm over 5 s -- see docs/NR_UE_ISAC_sync_gap_analysis.md section 9) -- far outside Phase 1's
 * window. Reusing Phase 1's estimates here would silently feed the fit garbage once drift exceeds
 * that window. Instead this phase does its own sequential TRACKING search per row (reusing only
 * the CIR-building step via row_cir_builder, not the peak-finding strategy): the first row seeds
 * from the same nominal LOS bin Phase 1 uses, and each subsequent row searches a fixed-width window
 * *re-centered on the previous accepted row's own peak* -- the same predict-from-previous structure
 * as Phase 2's sequential phase unwrap, which lets cumulative drift across a CPI be arbitrarily
 * large as long as the PER-STEP drift between consecutive valid rows stays within the window.
 *
 * ISI-contamination gating: per candidate row, the average CIR power just OUTSIDE its own local
 * search window is compared against a running (Welford) mean/stddev of that same statistic,
 * computed separately PER NATIVE COMB (comb-12 CSI-RS and comb-1/2 PDSCH rows have different noise
 * floors, so comparing across combs would be meaningless). Anomalous rows are excluded from the fit
 * AND from updating the running stats (so one bad row doesn't poison the baseline used to judge the
 * next), and the walk's anchor does not advance on an excluded row either -- hard exclusion, not
 * soft down-weighting, chosen for implementation simplicity (a WLS with continuous ISI weights is
 * a natural refinement, not implemented here; stated explicitly per the task's tradeoff-disclosure
 * requirement).
 *
 * The surviving {time, delay} pairs are least-squares fit; the slope IS the fractional sample-clock
 * error (dimensionless, seconds of delay drift per second elapsed == ppm/1e6) -- no unit conversion
 * needed beyond x1e6. Correction is a single per-row frequency-domain phase ramp, chosen over a
 * cubic Farrow resampler: exp(j*2*pi*k*SCS*tau) is the EXACT frequency-domain representation of a
 * (circular) time-domain shift by any tau, not just sub-sample tau, since we already hold the CFR
 * in the frequency domain there is no second FFT/IFFT round trip, and this is the SAME technique
 * Phase 1 already uses for its (much smaller) correction. The corrected quantity per row is
 * `slope * time_s[row]` ONLY (the fit's intercept, i.e. its value at CPI-start, is deliberately
 * excluded) -- this removes exactly the DRIFT accumulated since CPI start and leaves the row's
 * absolute/coarse delay level (the diagnosed ~49 m / bin-6 group delay, and whatever Phase 1 left
 * of it) untouched, mirroring Phase 1's fractional-only correction boundary. Because the fit gives
 * a continuous function of time, every row is corrected at its own exact time -- there is no
 * coarser "update interval" and therefore no separate residual/sub-interval correction pass.
 *
 * Invariant check: like Phase 1/2, this is a phase-only multiply of existing occupied columns --
 * nof_subc, column identity, and occupied-column count are untouched, so `nof_range x df = total
 * bandwidth` (range_res = 8.16 m) is preserved exactly.
 *
 * RT-safety: engine thread only, called from sensing_engine's CPI-close path after Phase 1/2 (order
 * among the three doesn't matter -- see docs/NR_UE_ISAC_sync_gap_analysis.md section 9), before
 * Stage-4b interpolation.
 */
class cpi_sfo_tracker
{
public:
  /**
   * @param h_cpi          raw per-subcarrier CPI grid, row-major [cpi_rows][nof_subc]; mutated in
   *                       place when the fit succeeds
   * @param occ_all        per-row occupancy mask, row-major [cpi_rows][nof_subc]
   * @param cpi_rows       number of committed rows this CPI
   * @param nof_subc       grid width (carrier.nof_prb * 12)
   * @param row_comb       per-row native comb (row_native_comb), length cpi_rows
   * @param row_time_slots per-row absolute slow-time position, in slots, length cpi_rows
   * @param carrier        carrier geometry for this CPI (SCS drives the bin/delay conversion)
   */
  sfo_fit_result_t process(icf_t*                   h_cpi,
                           const uint8_t*           occ_all,
                           uint32_t                 cpi_rows,
                           uint32_t                 nof_subc,
                           const uint32_t*          row_comb,
                           const double*            row_time_slots,
                           const nr_isac_carrier_t& carrier);

private:
  struct sfo_row_t {
    uint32_t row                    = 0;
    double   time_s                 = 0.0;
    double   tau_s                  = 0.0;
    uint32_t comb                   = 0;
    double   out_win_energy_per_bin = 0.0;
    bool     valid                  = false;
    bool     excluded_isi           = false;
  };

  // Welford online mean/variance of out_win_energy_per_bin, tracked separately per native comb.
  struct comb_stats_t {
    uint32_t comb = 0;
    uint32_t n    = 0;
    double   mean = 0.0;
    double   m2   = 0.0;
  };
  comb_stats_t& stats_for(uint32_t comb);

  bool track_row(const icf_t* row, const uint8_t* mask, uint32_t nof_subc, uint32_t comb, uint32_t anchor_bin,
                 uint32_t& out_peak, double& out_frac, double& out_win_energy_per_bin);

  row_cir_builder           cir_builder_;
  std::vector<icf_t>        cir_;         ///< scratch: CIR for one row
  std::vector<sfo_row_t>    rows_;        ///< per-row results, current CPI
  std::vector<comb_stats_t> comb_stats_;  ///< per-comb running ISI baseline, current CPI
};

/// Phase 4: one CPI's LOS-detection offset from the established baseline. This is the primary
/// acceptance-criterion signal for every Phase 7 test (per the task text) -- not any individual
/// STO/CFO/SFO number in isolation.
struct los_residual_t {
  bool     baseline_established = false; ///< false while still accumulating the init average
  bool     detection_found      = false; ///< false if no CA-CFAR/NMS detection landed near the
                                          ///< (candidate) baseline this CPI -- no residual measured
  uint32_t range_bin            = 0;     ///< this CPI's matched LOS detection, if found
  uint32_t doppler_bin          = 0;
  double   range_residual_m     = 0.0;   ///< matched detection's range - current baseline/running-avg range
  double   vel_residual_mps     = 0.0;   ///< matched detection's velocity - current baseline/running-avg velocity
  double   delay_bias_s         = 0.0;   ///< integrator's bias state after this update (applied next CPI)
  double   cfo_bias_hz          = 0.0;   ///< integrator's bias state after this update (applied next CPI)
};

/**
 * @brief Phase 4: closed-loop LOS pinning, wrapping the EXISTING range_doppler -> detection_report
 * chain rather than adding a second RD/CFAR/detection path.
 *
 * Two entry points, called from two different places in sensing_engine.cc's per-CPI flow:
 *
 * - `apply_bias_correction()` runs EARLY, in the CPI-close block alongside Phases 1-3 (before
 *   Stage-4b interpolation), and applies whatever bias the closed loop has accumulated from PAST
 *   CPIs' residuals -- using the SAME two correction primitives Phases 1-3 already established
 *   (a linear-in-*k* frequency ramp for the delay bias, a uniform per-row rotation scaled by each
 *   row's own elapsed time for the CFO-like bias), so it commutes with all of Phase 1-3's
 *   corrections and can run in any order relative to them.
 * - `update_residual()` runs LATE, in `process_cpi()` immediately after `range_doppler::process()`
 *   produces this CPI's `detections`/`rvm` -- it identifies the detection nearest the established
 *   (or being-established) LOS baseline, measures this CPI's residual, and folds it into the bias
 *   state that `apply_bias_correction()` will apply on the NEXT CPI. This one-CPI latency is the
 *   expected shape of a closed loop, not an oversight.
 *
 * Baseline establishment: rather than assuming a universal constant (bin 6 is specific to THIS
 * cell's group delay, not a general truth), the first `LOS_BASELINE_INIT_CPIS` CPIs with a
 * detection near an initial guess (the same NOMINAL_LOS_RANGE_M-derived bin Phase 1/3 use for
 * range, and the RVM's own zero-Doppler bin for velocity -- physically justified since the direct
 * path is static once fully corrected, not an arbitrary guess) are averaged to become the baseline.
 *
 * Bias update is a leaky integrator (`bias = (1-LEAK)*bias + KI*residual`), not a pure integrator:
 * a plain integrator risks unbounded windup if the baseline match ever degrades for several CPIs in
 * a row (e.g. a real target passing near the LOS's own range/Doppler cell); the leak keeps the bias
 * state bounded while still accumulating a persistent correction over many CPIs. Gains are starting
 * defaults (see .cc), to be tuned in Phase 6a against the self-test harness's known-injected
 * residual, exactly as the Constraints section asks for PLL/integrator bandwidth to be stated
 * explicitly rather than picked silently.
 *
 * Confirms constraint 5: no second RD/CFAR/detection path exists here -- both methods only read
 * `range_doppler.cc`'s existing `sensing_detection_t`/`sensing_rvm_t` outputs and write bias state
 * consumed as an INPUT correction to the CFR grid, upstream of range_doppler entirely.
 */
class los_baseline_tracker
{
public:
  /// Called after range_doppler::process() has populated this CPI's detections/rvm.
  los_residual_t update_residual(const std::vector<sensing_detection_t>& detections, const sensing_rvm_t& rvm,
                                 double fc_hz);

  /// Called in the CPI-close block, before Stage-4b interpolation (see class comment for placement
  /// rationale). No-op until a baseline has been established and at least one residual measured.
  void apply_bias_correction(icf_t* h_cpi, const uint8_t* occ_all, uint32_t cpi_rows, uint32_t nof_subc,
                             const double* row_time_slots, const nr_isac_carrier_t& carrier) const;

private:
  bool     baseline_set_         = false;
  uint32_t baseline_range_bin_   = 0;
  uint32_t baseline_doppler_bin_ = 0;
  double   baseline_range_m_     = 0.0;
  double   baseline_vel_mps_     = 0.0;

  uint32_t init_count_    = 0; ///< successful (detection found) init CPIs accumulated so far
  double   init_range_sum_ = 0.0;
  double   init_vel_sum_   = 0.0;
  uint64_t init_range_bin_sum_   = 0;
  uint64_t init_doppler_bin_sum_ = 0;

  double delay_bias_s_ = 0.0; ///< leaky-integrator state, applied next CPI
  double cfo_bias_hz_  = 0.0; ///< leaky-integrator state, applied next CPI
};

} // namespace nr_isac

#endif // NR_ISAC_SYNC_H
