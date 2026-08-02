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

  // CA-CFAR
  uint32_t cfar_guard = 4;      ///< CA-CFAR guard cells (per side)
  uint32_t cfar_train = 8;      ///< CA-CFAR training cells (per side)
  float    cfar_pfa   = 1e-3f;  ///< CA-CFAR probability of false alarm

  // Clutter suppression + detection cleanup (Stage 5)
  uint32_t zero_doppler_guard = 3;  ///< Doppler bins around zero velocity to notch out (static clutter / ghost)
  uint32_t zero_range_guard   = 2;  ///< Range bins near zero delay to notch out (direct-path / LOS clutter)
  uint32_t nms_range_bins     = 3;  ///< Non-max-suppression radius in range bins (0 disables)
  uint32_t nms_doppler_bins   = 3;  ///< Non-max-suppression radius in Doppler bins (0 disables)
  uint32_t max_detections     = 32; ///< Cap on reported detections per CPI after suppression

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
  float              vel_res_mps      = 0.0f; ///< Doppler bin size in velocity (bistatic)
  float              vel_max_mps      = 0.0f; ///< +/- unambiguous velocity extent
};

/// A single CA-CFAR detection in the RVM.
struct sensing_detection_t {
  uint32_t range_bin   = 0;
  uint32_t doppler_bin = 0;
  float    range_m     = 0.0f;
  float    vel_mps     = 0.0f;
  float    snr_db      = 0.0f;
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
  nr_isac_source_t  source   = NR_ISAC_SRC_CSI_RS; ///< Which reference produced this row (fusion diagnostics)
  nr_isac_carrier_t carrier  = {};                ///< Carrier geometry valid for this snapshot

  std::vector<icf_t>     h;     ///< Per-RE CFR Ĥ at the reference REs
  std::vector<uint32_t> k_abs; ///< Absolute subcarrier index (relative to CRB0) for each RE in @ref h
  std::vector<uint32_t> l_sym; ///< OFDM symbol index for each RE in @ref h
  uint32_t              nof_re       = 0;
  uint32_t              comb_spacing = 0; ///< Subcarrier spacing of the reference comb
  uint32_t              period_slots = 0; ///< Slow-time sampling period, in slots
};

} // namespace nr_isac

#endif // NR_ISAC_DEFS_H
