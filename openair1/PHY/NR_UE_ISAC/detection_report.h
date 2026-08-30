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

/*! \file openair1/PHY/NR_UE_ISAC/detection_report.h
 * \brief Central-node DetectionReport serialiser (port of srsUE nr::detection_report).
 *
 * Field names and units mirror the fusion node's wire contract
 * (repos/isac crates/isac-core/src/report.rs — DetectionReport / Illuminator / Detection),
 * so isac_core::report::DetectionReport deserialises the emitted JSON unchanged.
 *
 * Phase 5 (ota_sync_passive_ue.md) adds an optional "sync" object with per-CPI STO/CFO/SFO
 * estimates and the Phase 4 LOS residual. Confirmed safe against `repos/isac`'s wire contract:
 * `DetectionReport` derives plain `serde::Deserialize` with no `#[serde(deny_unknown_fields)]`
 * (crates/isac-core/src/report.rs), and `isac-bus::read_reports_jsonl()` (the function
 * `isac-track replay` calls) parses each line with a plain `serde_json::from_str::<DetectionReport>`
 * — unrecognised fields are silently ignored, not an error. Extending the JSON in place was
 * therefore safe; no sibling output file was needed.
 */

#ifndef NR_ISAC_DETECTION_REPORT_H
#define NR_ISAC_DETECTION_REPORT_H

#include <cstdint>
#include <string>
#include <vector>

#include "defs_nr_UE_ISAC.h"
#include "isac_sync.h"
#include "target_tracker.h"

namespace nr_isac {

/// One CPI's worth of sensing outputs plus surveyed geometry/ids for a DetectionReport.
struct detection_report_t {
  std::string rx_id;                        ///< DetectionReport.rx_id
  std::string illuminator_id;               ///< Illuminator.id (logical, for Tx<->Rx pairing)
  uint32_t    pci                   = 0;     ///< Illuminator.pci (observed physical cell id)
  std::string ref_type;                     ///< Illuminator.ref_type: "csi_rs" | "pdsch_dmrs" | "pdsch_data"
  float       tx_pos_x              = 0.0f;  ///< tx_position[0], surveyed ENU metres
  float       tx_pos_y              = 0.0f;  ///< tx_position[1], surveyed ENU metres
  float       rx_pos_x              = 0.0f;  ///< rx_position[0], surveyed ENU metres
  float       rx_pos_y              = 0.0f;  ///< rx_position[1], surveyed ENU metres
  int64_t     cpi_start_time_utc_ns = 0;     ///< cpi_start_time_utc_ns (system-clock stand-in)
  int64_t     cpi_duration_ns       = 0;     ///< cpi_duration_ns
  double      fc_hz                 = 0.0;   ///< fc_hz (illuminator centre frequency)

  /// P_D measured receiver-side by det_quality (see its `detection_rate()`), or negative when the
  /// quality gate is not running / has not warmed up. Emitted so the central node does not have to
  /// estimate it from its own track population, which is self-reinforcing -- see the header note on
  /// `detection_rate()` for the measured pathology this exists to avoid.
  double      p_detect              = -1.0;

  bool        subbin_interp         = false; ///< whether sub-bin peak interpolation was on, which is what
                                             ///< decides whether the declared range/rate noise is a
                                             ///< bin-quantisation floor or an SNR-limited residual

  const sensing_rvm_t*                    rvm              = nullptr; ///< range/velocity resolution + extents
  const std::vector<sensing_detection_t>* detections       = nullptr; ///< per-CPI CA-CFAR detections
  bool                                    include_rvm_blob = false;    ///< emit the RVM raster as rvm_blob

  /// Confirmed tracks for this CPI, with the single-receiver AoA position fix when they carry a
  /// bearing. Additive to the wire schema (repos/isac ignores unknown fields); consumed by the live
  /// monitor GUI, which is a single-receiver tool and so cannot get positions from cross-rx fusion.
  const std::vector<sensing_track_t>*     tracks           = nullptr;

  /// Rows contributed to this CPI by each source, indexed by nr_isac_source_t. Already counted by the
  /// engine for its own occupancy log line; emitted so the monitor can show DL/UL source mix without
  /// re-deriving it. PUSCH_DMRS is the UPLINK entry, and its geometry differs (see nr_isac.h).
  const uint64_t*                         src_occ          = nullptr;
  uint32_t                                src_occ_len      = 0;

  // Phase 5: per-CPI sync estimates, always populated (Phases 1-4 run unconditionally) and always
  // emitted as a "sync" object -- see the file-level comment above for the wire-compatibility check.
  sto_fit_result_t sto;
  cfo_fit_result_t cfo;
  sfo_fit_result_t sfo;
  los_residual_t   los;
};

/**
 * @brief PER-DETECTION 1-sigma uncertainty on a range or range-rate estimate, from THIS detection's
 *        own SNR and the CPI's resolution.
 *
 * The report-level `range_std_m` / `rate_std_mps` are a property of the CPI, so every detection in a
 * batch declares the same uncertainty however strong or marginal it is. That is the wrong shape: a
 * 25 dB detection and a 7 dB one in the same CPI do not deserve equal weight in a fused fix, and the
 * central node already whitens by whatever sigma it is given (CLAUDE.md 9's whitened Gauss-Newton).
 * The per-detection azimuth already works this way (`azimuth_std_deg`); this closes the same gap for
 * range and rate.
 *
 * Uses the standard estimator bound for a peak located in a transform of resolution `res` at linear
 * post-integration SNR `g`:  sigma ~= res / sqrt(2*g)  -- parameter-free, no fitted constant. Worth
 * recording that this is CONSISTENT with the hand-tuned value it replaces: the existing report-level
 * `0.3 * res` for the sub-bin-interpolated case is what this formula returns at ~7 dB, a typical
 * detection SNR here, so the two agree where they overlap and this only adds the SNR dependence.
 *
 * Clamped at both ends, because an unclamped CRB is a way to claim nonsense:
 *  - never better than `res/20` -- sub-bin interpolation has its own bias floor (CLAUDE.md 6 records
 *    a real parabolic-interpolation bias found on this pipeline), so an arbitrarily strong detection
 *    must not be allowed to declare arbitrarily small error;
 *  - never worse than `res/sqrt(12)`, the uniform bin-quantisation bound, which is what the estimate
 *    degenerates to with no interpolation at all.
 *
 * @param res         range_res_m or vel_res_mps for this CPI
 * @param snr_db      this detection's own SNR
 * @param subbin      whether sub-bin peak interpolation was enabled; if not, quantisation dominates
 *                    and the bound is returned directly (SNR cannot buy sub-bin accuracy you did not
 *                    compute)
 * @return 1-sigma in the same units as @p res, or 0 when @p res is not positive (caller omits it).
 */
double nr_isac_detection_sigma(double res, double snr_db, bool subbin);

/// Maps a sensing reference source to the DetectionReport Illuminator.ref_type string.
const char* nr_isac_source_to_ref_type(nr_isac_source_t source);

/**
 * @brief Maps an enabled-source set (bit i = source i) to the DetectionReport Illuminator.ref_type.
 *
 * A single source maps to its plain name (unchanged wire value). A fused set maps to a composite
 * "fused(csi_rs+pdsch_dmrs)" string. Illuminator.ref_type is a free-form string on the wire
 * (repos/isac crates/isac-core/src/report.rs), so this stays schema-compatible.
 */
std::string nr_isac_sources_to_ref_type(uint32_t sources_mask);

/**
 * @brief Serialises @p rep as a single-line JSON DetectionReport (no trailing newline), matching the
 * fusion node's schema. Non-finite floats (NaN/Inf) are coerced to 0 so the output is always valid
 * JSON. `rvm_blob` is emitted only when @c include_rvm_blob and an RVM is present.
 */
std::string build_detection_report_json(const detection_report_t& rep);

} // namespace nr_isac

#endif // NR_ISAC_DETECTION_REPORT_H
