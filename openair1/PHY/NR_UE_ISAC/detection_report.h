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

  bool        subbin_interp         = false; ///< whether sub-bin peak interpolation was on, which is what
                                             ///< decides whether the declared range/rate noise is a
                                             ///< bin-quantisation floor or an SNR-limited residual

  const sensing_rvm_t*                    rvm              = nullptr; ///< range/velocity resolution + extents
  const std::vector<sensing_detection_t>* detections       = nullptr; ///< per-CPI CA-CFAR detections
  bool                                    include_rvm_blob = false;    ///< emit the RVM raster as rvm_blob

  // Phase 5: per-CPI sync estimates, always populated (Phases 1-4 run unconditionally) and always
  // emitted as a "sync" object -- see the file-level comment above for the wire-compatibility check.
  sto_fit_result_t sto;
  cfo_fit_result_t cfo;
  sfo_fit_result_t sfo;
  los_residual_t   los;
};

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
