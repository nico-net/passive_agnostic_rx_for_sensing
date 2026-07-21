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
  fft_plan* plan_for(uint32_t m);

  // FFT plan cache keyed by compact-CIR length (M). Only a handful of distinct M values recur per
  // CPI (one per distinct native comb in play), so a small linear-scan cache is fine and avoids
  // recomputing Bluestein chirps every CPI.
  std::vector<std::pair<uint32_t, std::unique_ptr<fft_plan>>> plan_cache_;

  std::vector<icf_t>              compact_; ///< scratch: compacted occupied samples for one row
  std::vector<icf_t>              cir_;     ///< scratch: CIR for one row
  std::vector<los_row_estimate_t> rows_;    ///< per-row results, current CPI
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

} // namespace nr_isac

#endif // NR_ISAC_SYNC_H
