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

/*! \file openair1/PHY/NR_UE_ISAC/range_doppler.h
 * \brief OFDM-based range-Doppler processor (port of srsUE nr::range_doppler).
 */

#ifndef NR_ISAC_RANGE_DOPPLER_H
#define NR_ISAC_RANGE_DOPPLER_H

#include <memory>
#include <string>
#include <vector>

#include "clean_deconv.h"
#include "defs_nr_UE_ISAC.h"
#include "eca_clutter.h"
#include "isac_fft.h"
#include "sparse_doppler.h"

namespace nr_isac {

class hierarchical_tracker;

/// Phase 6a (ota_sync_passive_ue.md): a known impairment to apply to the synthetic LOS/reference
/// path itself, parsed from `args.selftest_los = "STO_US:CFO_HZ:SFO_PPM"`. Distinct from
/// `sensing_target_t` (which describes an echo/target, not the reference path).
struct selftest_los_impairment_t {
  double sto_s    = 0.0;   ///< constant delay offset (seconds) -- Phase 1's fine-STO correction target
  double cfo_hz   = 0.0;   ///< constant residual carrier offset (Hz) -- Phase 2's target
  double sfo_ppm  = 0.0;   ///< fractional sample-clock error (ppm) -- Phase 3's target; delay drifts as
                           ///< `sfo_ppm * 1e-6 * elapsed_time_s`
  bool   present  = false; ///< true if @p spec parsed to a non-degenerate impairment
};

/// Parses "STO_US:CFO_HZ:SFO_PPM" (same colon-separated style as `selftest_targets`) into @p out.
/// Returns false (and leaves @p out default) for an empty or malformed spec. Shared between
/// range_doppler's self-test config surface and the offline sync test harness (tests/isac_sync_test.cc)
/// so the parsing logic isn't duplicated.
bool parse_selftest_los(const std::string& spec, selftest_los_impairment_t& out);

/// The complex-tone model shared by every self-test signal this module synthesizes (existing target
/// injection, and Phase 6a's LOS-path impairment injection): a pure delay `tau` + Doppler `fd` tone
/// at frequency-axis position `m_times_df` (= subcarrier_index * df_comb) and slow-time position
/// `n_times_t_slow` (= row_index * t_slow), scaled to amplitude `amp`. Matches the physical model
/// Phases 1-4 assume (`exp(-j*2*pi*m*df*tau + j*2*pi*fd*n*t_slow)`), so a correct estimator should
/// recover exactly the injected tau/fd/drift.
icf_t selftest_tone(double amp, double m_times_df, double tau, double fd, double n_times_t_slow);

/**
 * @brief OFDM-based range-Doppler processor.
 *
 * Turns a coherent-processing-interval CFR matrix (slow-time x subcarrier) into a
 * range-velocity map: clutter removal (per-subcarrier slow-time DC) -> range IFFT over
 * frequency -> Doppler FFT over slow-time -> |.|^2 -> zero-Doppler/zero-range notch ->
 * 2D CA-CFAR + non-max suppression. DFT plans are created once and reused across CPIs.
 * Runs on the (non real-time) sensing engine thread only.
 */
class range_doppler
{
public:
  explicit range_doppler(const nr_isac_args_t& args_);
  ~range_doppler() = default;

  /**
   * @brief Computes the RVM and detections for one CPI.
   *
   * @param h_cpi            CFR matrix, row-major [nof_slow][nof_subc] (row = reference occurrence)
   * @param nof_slow         Number of slow-time samples (occurrences in the CPI)
   * @param nof_subc         Number of reference subcarriers (uniform comb)
   * @param comb_spacing     Subcarrier spacing of the reference-grid columns (1 for the fused grid)
   * @param carrier          Carrier geometry (SCS, centre frequency) for axis scaling
   * @param period_slots     Slow-time sampling period of the reference resource, in slots
   * @param row_comb         Optional per-slow-time-row native comb (in subcarriers). A comb-N row
   *                         supports unambiguous range only to nof_range/N bins; beyond that its
   *                         range profile is aliased replicas (grating lobes), which are tapered to
   *                         zero. Pass nullptr to disable per-row de-aliasing (keeps full range).
   * @param[out] rvm         Resulting range-velocity map
   * @param[out] detections  CA-CFAR detections
   * @param row_time_slots   Optional per-row actual sample time (in slots, row 0 = 0). Required for
   *                         the non-uniform Doppler DFT (args.doppler_nudft); pass nullptr for the
   *                         uniform-FFT path (rows assumed evenly spaced at period_slots).
   */
  void process(const icf_t*                       h_cpi,
               uint32_t                          nof_slow,
               uint32_t                          nof_subc,
               uint32_t                          comb_spacing,
               const nr_isac_carrier_t&          carrier,
               float                             period_slots,
               const uint32_t*                   row_comb,
               sensing_rvm_t&                    rvm,
               std::vector<sensing_detection_t>& detections,
               const double*                     row_time_slots = nullptr,
               const uint8_t*                    occ_mask       = nullptr,
               const hierarchical_tracker*       feedback_tracker = nullptr);

private:
  bool ensure_plans(uint32_t nof_range, uint32_t nof_slow);
  void inject_selftest(icf_t* work, uint32_t nof_slow, uint32_t nof_subc, double df_comb, double t_slow, double fc,
                       uint32_t comb_spacing);
  void cfar(const sensing_rvm_t& rvm, std::vector<sensing_detection_t>& detections, uint32_t nof_slow,
            const double* row_time_slots, float period_slots, const hierarchical_tracker* feedback_tracker = nullptr);
  // CLEAN deconvolution helpers (clean_deconv). build_clean_kernels fills clean_kr/clean_kd for the
  // current CPI geometry; clean_components_budget picks the per-CPI component budget (manual or auto).
  void     build_clean_kernels(uint32_t nof_range, uint32_t nof_dopp, bool use_nudft);
  uint32_t clean_components_budget() const;
  // Occupancy-aware forward-model CLEAN (clean_occ_aware). occ_forward_transform pushes an input grid
  // through the exact range+Doppler operator WITH per-row occupancy masking (no per-row norm), writing
  // a complex fftshifted map -- used both for the data map and for each component's PSF. clean_occ_aware
  // runs the CLEAN loop over that operator and writes the cleaned power into rvm.power.
  void occ_forward_transform(const icf_t* grid, uint32_t nof_slow, uint32_t nof_subc, uint32_t nof_range,
                             uint32_t nof_dopp, const uint8_t* occ_mask, const uint32_t* row_comb,
                             const double* row_time_slots, float period_slots, bool use_nudft,
                             icf_t* cmap_out);
  void clean_occ_aware(const icf_t* work_grid, uint32_t nof_slow, uint32_t nof_subc, uint32_t nof_range,
                       uint32_t nof_dopp, const uint8_t* occ_mask, const uint32_t* row_comb,
                       const double* row_time_slots, float period_slots, bool use_nudft,
                       sensing_rvm_t& rvm);

  nr_isac_args_t args;

  // DFT plans (lazily created / resized)
  std::unique_ptr<fft_plan> range_plan; ///< length nof_range, inverse (range IFFT)
  std::unique_ptr<fft_plan> dopp_plan;  ///< length nof_slow, forward (Doppler FFT)
  uint32_t                  range_N = 0;
  uint32_t                  dopp_N  = 0;

  // Scratch buffers (engine thread only)
  std::vector<icf_t>   work;      ///< [nof_slow][nof_subc] working copy
  std::vector<icf_t>   cir_rm;    ///< [nof_range][nof_slow] range-major CIR
  std::vector<icf_t>   dopp_in;   ///< [nof_slow]
  std::vector<icf_t>   dopp_out;  ///< [nof_slow]
  std::vector<icf_t>   nudft_mat; ///< [nof_dopp][nof_slow] non-uniform Doppler DFT matrix (doppler_nudft)
  std::vector<icf_t>   range_in;  ///< [nof_subc] windowed CFR row
  std::vector<icf_t>   range_out; ///< [nof_range]
  std::vector<icf_t>   cmap;      ///< [nof_range][nof_dopp] complex RVM (clean_deconv only)
  std::vector<icf_t>   clean_kr;  ///< [nof_range] separable range-axis PSF kernel (clean_deconv)
  std::vector<icf_t>   clean_kd;  ///< [nof_dopp]  separable Doppler-axis PSF kernel (clean_deconv)
  bool                 clean_warned_ = false; ///< one-time "CLEAN skipped (non-separable operator)" log
  uint32_t             clean_prev_raw_det_    = 0; ///< previous CPI's raw detection count (auto budget)
  uint32_t             clean_last_components_ = 0; ///< components extracted last CPI (diagnostics)
  std::vector<icf_t>   clean_res;   ///< [nof_range*nof_dopp] CLEAN residual map (occ-aware)
  std::vector<icf_t>   clean_psf;   ///< [nof_range*nof_dopp] per-component PSF map (occ-aware)
  std::vector<icf_t>   clean_synth; ///< [nof_slow*nof_subc] per-component synthetic grid (occ-aware)
  std::vector<uint32_t> gating_offsets_; ///< this CPI's MEASURED gating offsets, in Doppler bins
                                         ///< (gating_reject; see the estimator in process())
  std::vector<float>  hann;      ///< [nof_slow] slow-time (Doppler) window
  std::vector<float>  freq_hann; ///< [nof_subc] frequency (range) window
  std::vector<float>  whiten_rms;    ///< [nof_subc] per-subcarrier slow-time RMS (range_whiten)
  std::vector<float>  whiten_sorted; ///< scratch for the median-RMS nth_element (range_whiten)
  std::vector<double> integ;     ///< integral image for CA-CFAR

  // ECA/ECA+ clutter canceller, constructed only when args.clutter_removal == "eca+" (else the
  // legacy per-subcarrier slow-time mean subtraction runs). See eca_clutter.h.
  std::unique_ptr<eca_clutter> eca_;

  // Synthetic targets to inject (parsed once from args.selftest_targets / args.selftest)
  std::vector<sensing_target_t> targets_;

  // Closed-loop state for doppler_sparse's lambda_scale (see defs_nr_UE_ISAC.h's comment): persists
  // across CPIs, seeded from args.doppler_sparse_lambda_scale on first use (negative = not yet seeded).
  float sparse_lambda_scale_state_ = -1.0f;

  // Closed-loop state for cfar_target_fa_per_cpi (see defs_nr_UE_ISAC.h's cfar_fa_adapt_enable
  // comment): persists across CPIs, seeded from args.cfar_target_fa_per_cpi on first use.
  float cfar_fa_state_ = -1.0f;
  /// CPI counter, used only to rate-limit the adaptive-clutter-guard diagnostic.
  uint32_t notch_log_ = 0;

  // Phase 6a: parsed args.selftest_los, kept for visibility/logging only -- see the constructor's
  // comment for why process() itself doesn't consume this (it would be downstream of Phases 1-4).
  selftest_los_impairment_t los_impairment_;
};

} // namespace nr_isac

#endif // NR_ISAC_RANGE_DOPPLER_H
