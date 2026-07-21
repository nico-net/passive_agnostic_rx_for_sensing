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

#include "detection_report.h"
#include <cmath>
#include <cstdio>

namespace nr_isac {

const char* nr_isac_source_to_ref_type(nr_isac_source_t source)
{
  switch (source) {
    case NR_ISAC_SRC_PDSCH_DMRS:
      return "pdsch_dmrs";
    case NR_ISAC_SRC_PDSCH_DATA:
      return "pdsch_data";
    case NR_ISAC_SRC_CSI_RS:
    default:
      return "csi_rs";
  }
}

std::string nr_isac_sources_to_ref_type(uint32_t sources_mask)
{
  // Count enabled sources; a single source keeps its plain wire name for backward compatibility.
  int          count = 0;
  std::string  joined;
  for (int i = 0; i < NR_ISAC_SRC_COUNT; i++) {
    if (sources_mask & (1u << i)) {
      if (count > 0) {
        joined += "+";
      }
      joined += nr_isac_source_to_ref_type((nr_isac_source_t)i);
      count++;
    }
  }
  if (count == 0) {
    return "csi_rs"; // defensive: empty set falls back to the default
  }
  if (count == 1) {
    return joined;
  }
  return "fused(" + joined + ")";
}

namespace {

// Append a JSON string literal (quotes included), escaping the characters reachable from
// operator-supplied ids (quote, backslash, control chars) so the line stays valid JSON.
void append_json_string(std::string& out, const std::string& s)
{
  out += '"';
  for (char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
          out += buf;
        } else {
          out += c;
        }
    }
  }
  out += '"';
}

// Append a finite JSON number. NaN/Inf are coerced to 0 (serde_json rejects those literals).
void append_json_double(std::string& out, double v)
{
  if (!std::isfinite(v)) {
    v = 0.0;
  }
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.9g", v);
  out += buf;
}

} // namespace

std::string build_detection_report_json(const detection_report_t& rep)
{
  std::string out;
  out.reserve(512);

  out += "{\"rx_id\":";
  append_json_string(out, rep.rx_id);

  out += ",\"illuminator\":{\"id\":";
  append_json_string(out, rep.illuminator_id);
  out += ",\"pci\":";
  out += std::to_string(rep.pci);
  out += ",\"ref_type\":";
  append_json_string(out, rep.ref_type);
  out += '}';

  out += ",\"tx_position\":[";
  append_json_double(out, rep.tx_pos_x);
  out += ',';
  append_json_double(out, rep.tx_pos_y);
  out += ']';

  out += ",\"rx_position\":[";
  append_json_double(out, rep.rx_pos_x);
  out += ',';
  append_json_double(out, rep.rx_pos_y);
  out += ']';

  out += ",\"cpi_start_time_utc_ns\":";
  out += std::to_string(rep.cpi_start_time_utc_ns);
  out += ",\"cpi_duration_ns\":";
  out += std::to_string(rep.cpi_duration_ns);

  out += ",\"fc_hz\":";
  append_json_double(out, rep.fc_hz);

  const sensing_rvm_t  rvm_default;
  const sensing_rvm_t& rvm = (rep.rvm != nullptr) ? *rep.rvm : rvm_default;
  out += ",\"range_res_m\":";
  append_json_double(out, rvm.range_res_m);
  out += ",\"vel_res_mps\":";
  append_json_double(out, rvm.vel_res_mps);
  out += ",\"range_max_m\":";
  append_json_double(out, rvm.range_max_m);
  out += ",\"vel_max_mps\":";
  append_json_double(out, rvm.vel_max_mps);

  out += ",\"detections\":[";
  if (rep.detections != nullptr) {
    bool first = true;
    for (const sensing_detection_t& d : *rep.detections) {
      if (!first) {
        out += ',';
      }
      first = false;
      out += "{\"bistatic_range_m\":";
      append_json_double(out, d.range_m);
      out += ",\"bistatic_velocity_mps\":";
      append_json_double(out, d.vel_mps);
      out += ",\"snr_db\":";
      append_json_double(out, d.snr_db);
      out += '}';
    }
  }
  out += ']';

  // rvm_blob is an optional (serde default) field. Emit the RVM raster only in capture mode.
  if (rep.include_rvm_blob && rep.rvm != nullptr && !rvm.power.empty()) {
    out += ",\"rvm_blob\":[";
    bool first = true;
    for (float p : rvm.power) {
      if (!first) {
        out += ',';
      }
      first = false;
      append_json_double(out, p);
    }
    out += ']';
  }

  // Phase 5 (ota_sync_passive_ue.md): per-CPI STO/CFO/SFO estimates + the Phase 4 LOS residual.
  // Not part of repos/isac's DetectionReport schema -- confirmed safe to add (see detection_report.h's
  // file comment): unrecognised fields are silently ignored by isac-track's plain serde deserialiser.
  out += ",\"sync\":{";

  out += "\"sto\":{\"n_valid\":";
  out += std::to_string(rep.sto.n_valid);
  out += ",\"slope_bins_per_s\":";
  append_json_double(out, rep.sto.slope_bins_per_s);
  out += ",\"mean_frac_bin\":";
  append_json_double(out, rep.sto.mean_frac_bin);
  out += ",\"drift_bins_cpi\":";
  append_json_double(out, rep.sto.drift_bins_cpi);
  out += ",\"is_constant\":";
  out += rep.sto.is_constant ? "true" : "false";
  out += '}';

  out += ",\"cfo\":{\"n_valid\":";
  out += std::to_string(rep.cfo.n_valid);
  out += ",\"cfo_hz\":";
  append_json_double(out, rep.cfo.cfo_hz);
  out += ",\"cfo_hz_filtered\":";
  append_json_double(out, rep.cfo.cfo_hz_filtered);
  out += ",\"cfo_rate_hz_per_cpi\":";
  append_json_double(out, rep.cfo.cfo_rate_hz_per_cpi);
  out += ",\"residual_phase_rms_rad\":";
  append_json_double(out, rep.cfo.residual_phase_rms_rad);
  out += '}';

  out += ",\"sfo\":{\"n_candidate\":";
  out += std::to_string(rep.sfo.n_candidate);
  out += ",\"n_excluded_isi\":";
  out += std::to_string(rep.sfo.n_excluded_isi);
  out += ",\"n_fit\":";
  out += std::to_string(rep.sfo.n_fit);
  out += ",\"sfo_ppm\":";
  append_json_double(out, rep.sfo.sfo_ppm);
  out += ",\"sample_clock_error_hz\":";
  append_json_double(out, rep.sfo.sample_clock_error_hz);
  out += ",\"corrected\":";
  out += rep.sfo.corrected ? "true" : "false";
  out += '}';

  out += ",\"los_residual\":{\"baseline_established\":";
  out += rep.los.baseline_established ? "true" : "false";
  out += ",\"detection_found\":";
  out += rep.los.detection_found ? "true" : "false";
  out += ",\"range_bin\":";
  out += std::to_string(rep.los.range_bin);
  out += ",\"doppler_bin\":";
  out += std::to_string(rep.los.doppler_bin);
  out += ",\"range_residual_m\":";
  append_json_double(out, rep.los.range_residual_m);
  out += ",\"vel_residual_mps\":";
  append_json_double(out, rep.los.vel_residual_mps);
  out += ",\"delay_bias_s\":";
  append_json_double(out, rep.los.delay_bias_s);
  out += ",\"cfo_bias_hz\":";
  append_json_double(out, rep.los.cfo_bias_hz);
  out += '}';

  out += '}'; // end "sync"

  out += '}';
  return out;
}

} // namespace nr_isac
