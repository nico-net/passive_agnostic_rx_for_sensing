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
#include <vector>

#include "defs_nr_UE_ISAC.h"
#include "isac_fft.h"

namespace nr_isac {

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
   */
  void process(const icf_t*                       h_cpi,
               uint32_t                          nof_slow,
               uint32_t                          nof_subc,
               uint32_t                          comb_spacing,
               const nr_isac_carrier_t&          carrier,
               float                             period_slots,
               const uint32_t*                   row_comb,
               sensing_rvm_t&                    rvm,
               std::vector<sensing_detection_t>& detections);

private:
  bool ensure_plans(uint32_t nof_range, uint32_t nof_slow);
  void inject_selftest(icf_t* work, uint32_t nof_slow, uint32_t nof_subc, double df_comb, double t_slow, double fc);
  void cfar(const sensing_rvm_t& rvm, std::vector<sensing_detection_t>& detections);

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
  std::vector<icf_t>   range_in;  ///< [nof_subc] windowed CFR row
  std::vector<icf_t>   range_out; ///< [nof_range]
  std::vector<float>  hann;      ///< [nof_slow] slow-time (Doppler) window
  std::vector<float>  freq_hann; ///< [nof_subc] frequency (range) window
  std::vector<double> integ;     ///< integral image for CA-CFAR

  // Synthetic targets to inject (parsed once from args.selftest_targets / args.selftest)
  std::vector<sensing_target_t> targets_;
};

} // namespace nr_isac

#endif // NR_ISAC_RANGE_DOPPLER_H
