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

/*! \file openair1/PHY/NR_UE_ISAC/isac_aoa.h
 * \brief Receive-array angle-of-arrival estimation for the ISAC sensing pipeline
 *        (PHASE3_AOA_MULTISTATIC_HANDOVER §5.4).
 *
 * Turns a per-antenna CPI CFR grid into one bearing per CA-CFAR detection, in the SAME ENU
 * convention isac-core's `TxRxPair::bearing_meas()` uses (degrees CCW from east), so the emitted
 * `Detection.azimuth_deg` drops straight into the central node's measurement model.
 *
 * ## Why this is a separate, deliberately plain transform
 *
 * The main `range_doppler` chain carries a lot of opt-in, DATA-DEPENDENT machinery (spectral
 * whitening, ECA, matrix completion, CLEAN, sparse Doppler). Running it per antenna would apply a
 * *different* real weighting to each antenna's grid, and a per-antenna weighting perturbs exactly the
 * quantity AoA measures — the inter-element phase. This module therefore evaluates its own minimal,
 * identical-for-every-antenna chain:
 *
 *     per-subcarrier slow-time mean subtraction  ->  occupied-only range projection
 *                                                ->  non-uniform slow-time (true row times) projection
 *
 * The correctness argument is short and worth stating once: at a target's cell the antennas see
 * `H_a = s_a · H_0` for a scalar steering coefficient `s_a`, and every stage above is LINEAR and
 * identical across antennas, so `s_a` factors straight through. That also means the *choice* of
 * window is irrelevant to the estimated bearing (a common real weighting cancels in `z_a·conj(z_0)`),
 * which is why no effort is spent matching the main path's window exactly.
 *
 * By the same argument the STO/CFO/SFO sync corrections are skipped here: they are common-mode
 * receiver impairments, so they multiply every antenna's row by the same factor and cancel in the
 * phase difference. What they *can* do is move a target's peak by a fraction of a bin relative to the
 * corrected grid the detections came from — which is why the cell is re-peaked over a small
 * neighbourhood (`aoa_cell_search_bins`) rather than trusted blindly.
 *
 * ## Ambiguities, stated up front
 *
 * - **Element spacing > λ/2** folds distinct bearings onto the same phase. Flagged at parse time.
 * - **A LINEAR array cannot tell θ from its mirror about the array axis.** This is physics, not a
 *   bug: the phase depends only on `d·û`, and both bearings give the same projection. The scan is
 *   therefore restricted to the half-plane the array faces (`aoa_broadside_deg`), and a target in the
 *   back half-plane is reported mirrored. Only a genuinely 2-D array removes this.
 */

#ifndef NR_ISAC_AOA_H
#define NR_ISAC_AOA_H

#include <cstdint>
#include <string>
#include <vector>

#include "defs_nr_UE_ISAC.h"

namespace nr_isac {

/// Max receive elements the estimator will model (the planned hardware is a 4-channel X410).
constexpr uint32_t ISAC_MAX_RX_ANT = 8;

/// Parsed receive-array geometry, in ENU-aligned metres (the boresight rotation is folded in here so
/// nothing downstream has to think about frames).
struct aoa_array_t {
  std::vector<float> ex;             ///< element x offsets, ENU-aligned, metres
  std::vector<float> ey;             ///< element y offsets, ENU-aligned, metres
  double             lambda_m = 0.0; ///< carrier wavelength
  bool               collinear = true;  ///< true => mirror ambiguity about the array axis (see header)
  double             axis_rad  = 0.0;   ///< array axis direction (ENU rad); meaningful when collinear
  double             max_spacing_m = 0.0;
  bool               ambiguous = false; ///< min element spacing exceeds lambda/2

  uint32_t size() const { return (uint32_t)ex.size(); }
  bool     usable() const { return ex.size() >= 2 && lambda_m > 0.0; }
};

/**
 * @brief Parse `"x,y;x,y;..."` element offsets (array frame, metres) and rotate them into ENU.
 *
 * Identical format and semantics to the simulator's `[sensing_channel] rx_array` — the two MUST
 * agree, since a mismatch between the geometry the air carries and the geometry the estimator
 * assumes is a silent-garbage failure, not a degraded one.
 *
 * @param quiet  suppress the summary/ambiguity log line. Set for the start-up probes, which parse
 *               against a PLACEHOLDER frequency just to count elements: their ambiguity verdict is
 *               computed from the wrong wavelength and printing it is actively misleading (it did
 *               mislead, on the first live run). Only the real per-carrier parse should report.
 * @return false (leaving @p out unusable) for an empty/malformed spec or fewer than 2 elements.
 */
bool parse_rx_array(const std::string& spec, double boresight_deg, double fc_hz, aoa_array_t& out,
                    bool quiet = false);

/**
 * @brief Ray ∩ bistatic-ellipse: turn ONE receiver's (differential range, bearing) into a position.
 *
 * The exact closed form isac-core's `TxRxPair::localize_with_bearing()` uses -- deliberately the same
 * algebra, so a fix computed UE-side and the same fix computed at the central node cannot disagree:
 *
 *     P = R + t·û,  R_b = |P − T| + t   =>   t = (R_b² − |a|²) / (2·(R_b + a·û)),  a = R − T
 *
 * The t² terms cancel, so this is exact, not an iteration.
 *
 * @param reported_range_m DIFFERENTIAL range ΔR = R_b − L, the convention the DetectionReport uses
 *        (`report_is_differential = true` at the central node). Note that `sensing_detection_t`'s
 *        `range_m` subtracts no LOS reference of its own -- it equals ΔR only because the direct path
 *        lands at range bin 0 on this harness. If that ever stops holding (the OTA cell puts the
 *        direct path at ~98 m; see `nominal_los_range_m`), the fixes computed here shift with it and
 *        the range convention must be reconciled BEFORE trusting a position from this function.
 * @return false when the solution is behind the receiver or the denominator degenerates -- which is
 *         what happens when the range and the bearing describe different objects.
 */
bool aoa_localize(double tx_x, double tx_y, double rx_x, double rx_y, double reported_range_m,
                  double bearing_deg, double& out_x, double& out_y);

/**
 * @brief Position-anchored Doppler-harmonic rejection (GHOST_KINEMATIC_CONSISTENCY_HANDOVER §3, A).
 *
 * `range_doppler`'s existing `harmonic_reject` anchors a harmonic to its fundamental by RANGE BIN
 * alone. Two detections that are harmonics of each other share range AND bearing (they are one
 * physical reflection mis-binned in Doppler), so they localise to the same point; two genuinely
 * distinct targets sharing a range bin -- which is common -- almost never also share a bearing.
 * Anchoring on the AoA-derived POSITION is therefore a strictly TIGHTER test in principle.
 *
 * **MEASURED (2026-07-30, GHOST_KINEMATIC_CONSISTENCY_HANDOVER.md 7.3): a flat metre tolerance on
 * that position anchor LOST to the existing range anchor**, because the two fixes of a genuine
 * harmonic pair are dominated by INDEPENDENT bearing estimation noise, not by geometry -- a flat gate
 * tight enough to reject unrelated targets was also too tight to hold real pairs together. The gate
 * therefore normalises the tangential (bearing-driven) residual by each detection's OWN reported
 * `azimuth_std_deg` via a chi2 test (`harmonic_pos_chi2`), and leaves only the radial residual as a
 * flat tolerance (`harmonic_pos_range_tol_m`) -- there is no per-detection range uncertainty to build
 * a real chi2 test from, so none is invented. See `same_reflection()` in the .cc.
 *
 * Scope, stated so it is not oversold: like every same-CPI test this still needs the FUNDAMENTAL to
 * have been detected in this CPI. It does nothing for an orphaned harmonic -- that is Phase B, and
 * it lives in `repos/isac` where a track carries position and velocity.
 *
 * Automatically inert when no detection carries a bearing (a receiver with no array), because a
 * detection without a position can neither be rejected nor act as a fundamental.
 *
 * @param args   supplies rx/tx survey positions, harmonic_pos_chi2/range_tol_m, and harmonic_* tolerances.
 * @param dets   modified in place; rejected detections are erased.
 * @return number of detections rejected.
 */
uint32_t harmonic_pos_reject(const nr_isac_args_t& args, std::vector<sensing_detection_t>& dets);

/// One detection's bearing estimate.
struct aoa_estimate_t {
  float azimuth_deg     = 0.0f;
  float azimuth_std_deg = 0.0f;
  float snr_db          = 0.0f;   ///< per-element SNR used for the σ estimate
  bool  valid           = false;  ///< false => do not report an azimuth for this detection
  bool  mirror_ambiguous = false; ///< linear array: the mirror bearing is equally consistent
};

/**
 * @brief Per-antenna CPI CFR grids -> one bearing per detection.
 *
 * Owns only scratch buffers; construct once per engine and reuse.
 */
class aoa_estimator
{
public:
  aoa_estimator(const nr_isac_args_t& args_, const aoa_array_t& array_);

  /**
   * @brief Estimate a bearing for every detection in @p dets.
   *
   * @param h_ant   per-antenna CPI grid, antenna-major: `h_ant[(a*row_stride + n)*nof_subc + c]`
   * @param row_stride rows allocated per antenna plane. Passed explicitly rather than assumed equal
   *                to @p nof_slow because the engine sizes the grid for a FULL CPI while a flush can
   *                hand over fewer rows; conflating the two would read the wrong antenna's data with
   *                no error of any kind -- exactly the silent-failure class this project keeps hitting.
   * @param occ     shared per-row occupancy mask `[nof_slow][nof_subc]` (identical for all antennas —
   *                every antenna observes the same REs)
   * @param row_time_slots per-row slow-time position in slots (the TRUE, non-uniform times)
   * @param period_slots   the uniform row spacing the detections' Doppler bins were computed against
   * @param out     one entry per detection, same order
   */
  void process(const icf_t*                            h_ant,
               uint32_t                                nof_ant,
               uint32_t                                nof_slow,
               uint32_t                                row_stride,
               uint32_t                                nof_subc,
               const uint8_t*                          occ,
               const double*                           row_time_slots,
               double                                  period_slots,
               const nr_isac_carrier_t&                carrier,
               const std::vector<sensing_detection_t>& dets,
               std::vector<aoa_estimate_t>&            out);

  /**
   * @brief Direct-path array self-calibration (§5.4 item 3).
   *
   * The illuminator's position is surveyed, so the LOS tap sits at a KNOWN bearing at (near) zero
   * differential range and zero Doppler. Estimating the per-channel phase/gain offsets against that
   * reference gives continuous self-calibration with no injected tone and no anechoic chamber.
   *
   * Call once per CPI *before* process(); the offsets are smoothed across CPIs and applied inside
   * process(). A no-op unless `args.aoa_selfcal` is set.
   *
   * @param known_bearing_rad ENU bearing of the illuminator as seen from this receiver.
   * @return true if this CPI contributed a usable calibration update.
   */
  bool calibrate_from_los(const icf_t*             h_ant,
                          uint32_t                 nof_ant,
                          uint32_t                 nof_slow,
                          uint32_t                 row_stride,
                          uint32_t                 nof_subc,
                          const uint8_t*           occ,
                          const double*            row_time_slots,
                          double                   period_slots,
                          const nr_isac_carrier_t& carrier,
                          double                   known_bearing_rad);

  /// Current per-channel calibration phasors (unit modulus; element 0 is always 1). Diagnostics/tests.
  const std::vector<icf_t>& calibration() const { return cal_; }

  /// Number of CPIs that have contributed to the calibration average.
  uint32_t calibration_updates() const { return cal_updates_; }

private:
  /// Project every antenna's every slow-time row onto ONE range bin, into `rowsum_`.
  ///
  /// This is the whole cost of the estimator (O(nof_ant * nof_slow * nof_subc)), and it is
  /// independent of the Doppler bin -- so it is hoisted out of the Doppler search rather than
  /// recomputed per cell. With the default +/-2-bin re-peak that is a 5x saving (25 cells -> 5 range
  /// projections); measured necessary, not speculative: at 273 PRB / 128 rows / 4 antennas the naive
  /// form is ~0.67 G complex ops per CPI, which does not fit in a CPI.
  void range_project(uint32_t nof_ant, uint32_t nof_slow, uint32_t nof_subc, uint32_t range_bin);

  /// Finish the transform for one Doppler bin from the hoisted `rowsum_`. O(nof_ant * nof_slow).
  void eval_cell(uint32_t nof_ant, uint32_t nof_slow, double f_d_norm, std::vector<icf_t>& z) const;

  /// Slow-time mean removal, shared across antennas (see the header's linearity argument).
  void remove_clutter(const icf_t* h_ant, uint32_t nof_ant, uint32_t nof_slow, uint32_t row_stride,
                      uint32_t nof_subc, const uint8_t* occ);

  /// Bearing from calibrated phasors, by whichever estimator the config selected.
  bool estimate_bearing(const std::vector<icf_t>& z, double& theta_rad, bool& mirror) const;
  bool bearing_interferometry(const std::vector<icf_t>& z, double& theta_rad) const;
  bool bearing_beamscan(const std::vector<icf_t>& z, double& theta_rad) const;
  bool bearing_music(const std::vector<std::vector<icf_t>>& snapshots, double& theta_rad) const;

  /// 1-σ bearing uncertainty (radians) from the array geometry at `theta` and a linear SNR.
  double bearing_sigma(double theta_rad, double snr_lin) const;

  /// Steering coefficient of element `a` for ENU bearing `theta`.
  icf_t steering(uint32_t a, double theta_rad) const;

  nr_isac_args_t args;
  aoa_array_t    array;

  std::vector<icf_t>  work_;      ///< clutter-removed per-antenna grid, antenna-major
  std::vector<double> row_t_;     ///< per-row TRUE slow-time position (slots), for the non-uniform DFT
  std::vector<icf_t>  rowsum_;    ///< [nof_ant][nof_slow] range projection at the current range bin
  std::vector<float>  slow_win_;  ///< [nof_slow] slow-time (Doppler) window
  std::vector<icf_t>  cal_;       ///< per-channel calibration phasors
  std::vector<icf_t>  cal_accum_; ///< running (unnormalised) calibration sum across CPIs
  uint32_t            cal_updates_ = 0;
  double              scan_lo_ = 0.0; ///< scan window (radians), set from the array + config
  double              scan_hi_ = 0.0;
};

} // namespace nr_isac

#endif // NR_ISAC_AOA_H
