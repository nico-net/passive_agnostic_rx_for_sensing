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

double nr_isac_detection_sigma(double res, double snr_db, bool subbin)
{
  if (!(res > 0.0)) {
    return 0.0;
  }
  const double quant = res / std::sqrt(12.0); // uniform bin-quantisation bound
  if (!subbin) {
    return quant; // no sub-bin estimate was computed; SNR cannot improve what was never measured
  }
  const double snr_lin = std::pow(10.0, snr_db / 10.0);
  if (!(snr_lin > 0.0) || !std::isfinite(snr_lin)) {
    return quant;
  }
  const double sigma = res / std::sqrt(2.0 * snr_lin);
  const double floor_ = res / 20.0; // sub-bin interpolation bias floor (see the header comment)
  return std::min(quant, std::max(floor_, sigma));
}

const char* nr_isac_source_to_ref_type(nr_isac_source_t source)
{
  switch (source) {
    case NR_ISAC_SRC_PDSCH_DMRS:
      return "pdsch_dmrs";
    case NR_ISAC_SRC_PDSCH_DATA:
      return "pdsch_data";
    case NR_ISAC_SRC_PDSCH_DMRS_BLIND:
      return "pdsch_dmrs_blind";
    case NR_ISAC_SRC_PUSCH_DMRS:
      return "pusch_dmrs";
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

  // Per-receiver measurement noise (isac-core TrackerParams fallbacks otherwise). Declared by the
  // receiver because a heterogeneous fleet -- a 100 MHz X410 next to a 20-56 MHz B210 -- has very
  // different range variance, and one global value lets the coarser receiver drag every fused fix.
  // Derived from THIS run's own resolution rather than hand-picked: a peak quantised to a bin has a
  // uniform error over that bin, i.e. std = bin/sqrt(12); sub-bin interpolation removes the
  // quantisation and leaves an SNR-limited residual, empirically a few tenths of a bin.
  if (rvm.range_res_m > 0.0f) {
    out += ",\"range_std_m\":";
    append_json_double(out, rep.subbin_interp ? rvm.range_res_m * 0.3 : rvm.range_res_m / std::sqrt(12.0));
  }
  if (rvm.vel_res_mps > 0.0f) {
    out += ",\"rate_std_mps\":";
    append_json_double(out, rep.subbin_interp ? rvm.vel_res_mps * 0.3 : rvm.vel_res_mps / std::sqrt(12.0));
  }

  // Receiver-measured P_D. OMITTED when unavailable, so a consumer falls back to its own estimate
  // exactly as before rather than reading a sentinel as a real detection rate.
  if (rep.p_detect >= 0.0) {
    out += ",\"p_detect\":";
    append_json_double(out, rep.p_detect);
  }

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
      // Bearing fields are OMITTED (not null, not zero) when this receiver has no array or the
      // estimate failed its quality gate: isac-core's Detection has them as serde-default Options, so
      // an absent azimuth degrades cleanly to a 2-D range/rate measurement instead of injecting a
      // bogus 0-degree bearing.
      if (d.azimuth_valid) {
        out += ",\"azimuth_deg\":";
        append_json_double(out, d.azimuth_deg);
        out += ",\"azimuth_std_deg\":";
        append_json_double(out, d.azimuth_std_deg);
      }
      // PER-DETECTION range/rate uncertainty from this detection's own SNR (see
      // nr_isac_detection_sigma). The report-level values remain for consumers that ignore these, and
      // for detections in a CPI with no usable resolution; isac-core prefers the per-detection value
      // when present, so an older tracker simply keeps using the report-level one.
      const double r_sig = nr_isac_detection_sigma(rvm.range_res_m, d.snr_db, rep.subbin_interp);
      const double v_sig = nr_isac_detection_sigma(rvm.vel_res_mps, d.snr_db, rep.subbin_interp);
      if (r_sig > 0.0) {
        out += ",\"range_std_m\":";
        append_json_double(out, r_sig);
      }
      if (v_sig > 0.0) {
        out += ",\"rate_std_mps\":";
        append_json_double(out, v_sig);
      }
      // Adaptive quality posterior (det_quality.h). OMITTED, not zeroed, when the gate did not run --
      // 0.0 would read as "certainly a false alarm" and would suppress every track.
      if (d.p_real >= 0.0f) {
        out += ",\"p_real\":";
        append_json_double(out, d.p_real);
      }
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

  // Confirmed tracks + their single-receiver AoA position fix (sensing_engine calls aoa_localize).
  // Additive field, same wire-compat argument as "sync" below.
  if (rep.tracks != nullptr && !rep.tracks->empty()) {
    out += ",\"tracks\":[";
    bool first_t = true;
    for (const sensing_track_t& t : *rep.tracks) {
      if (!first_t) {
        out += ',';
      }
      first_t = false;
      out += "{\"track_id\":";
      out += std::to_string(t.track_id);
      out += ",\"bistatic_range_m\":";
      append_json_double(out, t.range_m);
      out += ",\"bistatic_velocity_mps\":";
      append_json_double(out, t.range_rate_mps);
      out += ",\"sigma_range_m\":";
      append_json_double(out, t.sigma_range_m);
      out += ",\"coast_count\":";
      out += std::to_string(t.coast_count);
      out += ",\"updated\":";
      out += t.updated ? "true" : "false";
      if (t.azimuth_valid) {
        out += ",\"azimuth_deg\":";
        append_json_double(out, t.azimuth_deg);
        out += ",\"azimuth_std_deg\":";
        append_json_double(out, t.azimuth_std_deg);
      }
      // Omitted, never zeroed, when the track has no bearing: (0,0) is the receiver's own position
      // and would plot as a real fix at the origin.
      if (t.pos_valid) {
        out += ",\"position\":[";
        append_json_double(out, t.pos_x);
        out += ',';
        append_json_double(out, t.pos_y);
        out += ']';
      }
      out += '}';
    }
    out += ']';
  }

  // Per-source row counts for this CPI (index = nr_isac_source_t).
  if (rep.src_occ != nullptr && rep.src_occ_len > 0) {
    out += ",\"src_occ\":[";
    for (uint32_t i = 0; i < rep.src_occ_len; i++) {
      if (i != 0) {
        out += ',';
      }
      out += std::to_string(rep.src_occ[i]);
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
  out += ",\"n_flywheel\":";
  out += std::to_string(rep.sto.n_flywheel);
  out += ",\"absolute_drift_bins\":";
  append_json_double(out, rep.sto.total_drift_bins);
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
