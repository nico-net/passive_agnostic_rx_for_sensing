/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#include "report_writer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#ifdef ENABLE_ZEROMQ
#include <zmq.h>
#endif

namespace nr_isac {
namespace {

void string_value(std::string& out, const std::string& value)
{
  out.push_back('"');
  for (unsigned char c : value) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) { char buffer[8]; std::snprintf(buffer, sizeof(buffer), "\\u%04x", c); out += buffer; }
        else out.push_back(static_cast<char>(c));
    }
  }
  out.push_back('"');
}

void number(std::string& out, double value)
{
  if (!std::isfinite(value)) { out += "null"; return; }
  char buffer[48]; std::snprintf(buffer, sizeof(buffer), "%.17g", value); out += buffer;
}

void matrix(std::string& out, const Matrix& value)
{
  out.push_back('[');
  for (size_t r = 0; r < value.rows(); ++r) {
    if (r) out.push_back(',');
    out.push_back('[');
    for (size_t c = 0; c < value.cols(); ++c) { if (c) out.push_back(','); number(out, value(r,c)); }
    out.push_back(']');
  }
  out.push_back(']');
}

void optional(std::string& out, const std::optional<double>& value)
{ if (value) number(out, *value); else out += "null"; }

} // namespace

std::string source_name(nr_isac_source_t source)
{
  switch (source) {
    case NR_ISAC_SRC_CSI_RS: return "csi_rs";
    case NR_ISAC_SRC_PDSCH_DMRS: return "pdsch_dmrs";
    case NR_ISAC_SRC_PDSCH_DATA: return "pdsch_data";
    case NR_ISAC_SRC_PDSCH_DMRS_BLIND: return "pdsch_dmrs_blind";
    case NR_ISAC_SRC_PUSCH_DMRS: return "pusch_dmrs";
    case NR_ISAC_SRC_PUSCH_DATA: return "pusch_data";
    case NR_ISAC_SRC_SSB: return "ssb";
    default: return "invalid";
  }
}

std::string sources_name(uint32_t mask)
{
  std::string joined; uint32_t count = 0;
  for (uint32_t i = 0; i < NR_ISAC_SRC_COUNT; ++i) if (mask & (1u << i)) {
    if (count++) joined += '+';
    joined += source_name(static_cast<nr_isac_source_t>(i));
  }
  if (!count) return "none";
  return count == 1 ? joined : "fused(" + joined + ')';
}

std::string build_report_json(const PipelineReport& r, const PipelineConfig& c, bool emit_maps)
{
  const size_t expected_map_cells = static_cast<size_t>(r.detector.axes.range_bins)
                                    * r.detector.axes.rate_bins;
  if (c.capture_rvm
      && (r.detector.initial_likelihood.size() != expected_map_cells
          || r.detector.initial_dl_likelihood.size() != expected_map_cells
          || r.detector.dl_observed_re_count == 0))
    throw std::invalid_argument("captured DL range-Doppler map is absent or malformed");
  const size_t expected_ul_map_cells = static_cast<size_t>(r.uplink_detector.axes.range_bins)
                                       * r.uplink_detector.axes.rate_bins;
  if (c.capture_rvm && r.uplink_valid
      && r.uplink_detector.initial_likelihood.size() != expected_ul_map_cells)
    throw std::invalid_argument("captured UL range-Doppler map is absent or malformed");
  std::string out;
  out.reserve(emit_maps
                  ? (r.detector.initial_likelihood.size()
                     + r.detector.initial_dl_likelihood.size()) * 12
                  : 8192);
  out += "{\"schema\":\"oai.native_python_parity.v2\",\"rx_id\":"; string_value(out,c.rx_id);
  out += ",\"illuminator_id\":"; string_value(out,c.illuminator_id);
  out += ",\"cpi_sequence\":" + std::to_string(r.cpi_sequence);
  out += ",\"cpi_start_time_utc_ns\":" + std::to_string(r.start_utc_ns);
  out += ",\"cpi_duration_ns\":" + std::to_string(r.cpi_duration_ns);
  out += ",\"first_row_time_ns\":" + std::to_string(r.first_row_time_ns);
  out += ",\"last_row_time_ns\":" + std::to_string(r.last_row_time_ns);
  out += ",\"midpoint_air_time_s\":"; number(out,r.midpoint_air_time_s);
  out += ",\"actual_row_count\":" + std::to_string(r.detector.axes.rate_bins);
  out += ",\"observed_re_count\":" + std::to_string(r.detector.axes.observed_re_count);
  out += ",\"ref_type\":"; string_value(out,sources_name(r.sources_mask));
  out += ",\"source_mask\":" + std::to_string(r.sources_mask) + ",\"source_occurrences\":{";
  bool first=true; for(uint32_t i=0;i<NR_ISAC_SRC_COUNT;++i)if(r.source_occurrences[i]){
    if(!first)out.push_back(',');
    first=false;string_value(out,source_name(static_cast<nr_isac_source_t>(i)));
    out.push_back(':');out+=std::to_string(r.source_occurrences[i]);} out+='}';
  out += ",\"src_occ\":[";
  for(uint32_t i=0;i<NR_ISAC_SRC_COUNT;++i){if(i)out.push_back(',');out+=std::to_string(r.source_occurrences[i]);}
  out += ']';
  out += ",\"illumination_processing\":{\"coherent_ul_dl_fusion\":false,"
      "\"dl_detector\":true,\"ul_detector\":"
      + std::string(r.uplink_valid ? "true" : "false") + '}';
  out += ",\"pusch_session_ids\":[";
  for (size_t index = 0; index < r.uplink_session_ids.size(); ++index) {
    if (index) out.push_back(',');
    out += std::to_string(r.uplink_session_ids[index]);
  }
  out += ']';
  out += ",\"spatial_frontend_processing\":{\"jobs\":"
      + std::to_string(r.spatial_frontend_jobs)
      + ",\"worker_count\":" + std::to_string(r.spatial_frontend_worker_count)
      + ",\"peak_concurrency\":" + std::to_string(r.spatial_frontend_peak_concurrency)
      + ",\"wall_s\":";
  number(out, r.spatial_frontend_wall_s);
  out += '}';
  out += ",\"range_res_m\":"; number(out,r.detector.axes.range_res_m);
  out += ",\"range_max_m\":"; number(out,r.detector.axes.range_bins*r.detector.axes.range_res_m);
  out += ",\"vel_res_mps\":"; number(out,r.detector.axes.rate_res_mps);
  double vmax=0.0;for(double v:r.detector.axes.rate_axis_mps)vmax=std::max(vmax,std::abs(v));
  out += ",\"vel_max_mps\":";number(out,vmax);out += ",\"dwell_s\":";number(out,r.detector.axes.dwell_s);
  out += ",\"aoa_policy\":{\"master\":" + std::string(c.aoa_enable?"true":"false")
      + ",\"uplink_requested\":" + std::string(c.aoa_ul_enable_requested?"true":"false")
      + ",\"uplink\":" + std::string(c.aoa_ul_enable?"true":"false") + '}';
  out += ",\"aoa_quality_policy\":{\"maximum_relative_manifold_residual_energy\":";
  number(out,c.aoa_quality.maximum_relative_manifold_residual_energy);
  out += ",\"maximum_phase_fit_residual_rms_rad\":";
  number(out,c.aoa_quality.maximum_phase_fit_residual_rms_rad);
  out += ",\"maximum_azimuth_stddev_deg\":";
  number(out,c.aoa_quality.maximum_azimuth_stddev_deg);
  out += ",\"maximum_elevation_stddev_deg\":";
  number(out,c.aoa_quality.maximum_elevation_stddev_deg);out += '}';
  out += ",\"array_calibration\":{\"configured\":"
      + std::string(c.array_calibration.configured?"true":"false")
      + ",\"correction_convention\":\"gain*exp(j*(phase_rad+2*pi*f_offset_hz*delay_s))\""
        ",\"physical_to_observed\":[";
  for(size_t i=0;i<4;++i){if(i)out.push_back(',');out+=std::to_string(c.array_calibration.physical_to_observed[i]);}
  out += "],\"gain\":[";
  for(size_t i=0;i<4;++i){if(i)out.push_back(',');number(out,c.array_calibration.gain[i]);}
  out += "],\"phase_rad\":[";
  for(size_t i=0;i<4;++i){if(i)out.push_back(',');number(out,c.array_calibration.phase_rad[i]);}
  out += "],\"delay_s\":[";
  for(size_t i=0;i<4;++i){if(i)out.push_back(',');number(out,c.array_calibration.delay_s[i]);}
  out += "]}";
  out += ",\"sensing_admission\":{\"enabled\":"
      + std::string(c.admission_window_enabled?"true":"false")
      + ",\"start_radio_slot\":" + std::to_string(c.admission_start_slot)
      + ",\"end_radio_slot_exclusive\":" + std::to_string(c.admission_end_slot)
      + ",\"subslot_symbols\":" + std::to_string(c.subslot_symbols) + '}';
  out += ",\"sync\":{\"rows\":"+std::to_string(r.sync.rows)+",\"admitted_rows\":"
      +std::to_string(r.sync.admitted_rows)+",\"los_bins\":";number(out,r.sync.los_bins);
  out += ",\"sto\":{\"mean_frac_bin\":";number(out,r.sync.sto_bins);out += ",\"applied\":";
  out += r.sync.sto_applied?"true":"false";out += "},\"sfo\":{\"sfo_ppm\":";number(out,r.sync.sfo_ppm);
  out += ",\"applied\":";out += r.sync.sfo_applied?"true":"false";out += "},\"cfo\":{\"cfo_hz\":";
  number(out,r.sync.cfo_hz);out += ",\"applied\":";out += r.sync.cfo_applied?"true":"false";
  out += "},\"reject_reason\":";string_value(out,r.sync.reject_reason);out+='}';
  out += ",\"allocation_alignment\":{\"families\":"+std::to_string(r.detector_alignment.families)
      +",\"repeated_families\":"+std::to_string(r.detector_alignment.repeated_families)
      +",\"aligned_rows\":"+std::to_string(r.detector_alignment.aligned_rows)
      +",\"singleton_rows\":"+std::to_string(r.detector_alignment.singleton_rows)+'}';
  out += ",\"covariance\":{\"policy\":\"current_cpi_global_within_family_first_difference\",\"variance\":";
  number(out,r.current_cpi_variance);out += ",\"families\":"+std::to_string(r.covariance_family_count)
      +",\"difference_samples\":"+std::to_string(r.covariance_difference_count)+'}';
  out += ",\"cpi_plan\":{\"plan_id\":"+std::to_string(r.plan.plan_id)+",\"reason\":";
  string_value(out,r.plan.reason);out += ",\"target_dwell_s\":";number(out,r.plan.target_dwell_s);
  out += ",\"duration_bank_index\":"+std::to_string(r.plan.duration_bank_index)
      +",\"minimum_rows\":"+std::to_string(r.plan.minimum_rows)
      +",\"maximum_rows\":"+std::to_string(r.plan.maximum_rows)+",\"rate_search_center_mps\":";
  optional(out,r.plan.rate_search_center_mps);out += ",\"rate_search_half_width_mps\":";
  optional(out,r.plan.rate_search_half_width_mps);out += ",\"full_search\":";
  out += r.plan.full_search()?"true":"false";out+='}';
  out += ",\"adaptive_threshold\":";number(out,r.detector.adaptive_threshold);
  out += ",\"detector_stop\":{\"method\":\"current_cpi_exponential_null_alpha_spending\",\"reason\":";
  string_value(out,r.detector.stop_reason);
  out += ",\"psf_range_halfwidth_bins\":"+std::to_string(r.detector.psf_range_halfwidth_bins)
      +",\"psf_doppler_halfwidth_bins\":"+std::to_string(r.detector.psf_doppler_halfwidth_bins)
      +",\"searched_cells\":"+std::to_string(r.detector.searched_cells)
      +",\"resolution_cells\":"+std::to_string(r.detector.resolution_cells)
      +",\"effective_hypotheses\":"+std::to_string(r.detector.effective_hypotheses)
      +",\"identifiability_guard\":"+std::to_string(r.detector.identifiability_guard)
      +",\"null_scale\":";number(out,r.detector.null_scale);
  out += ",\"null_scale_source\":";string_value(out,r.detector.null_scale_source);
  out += ",\"calibration_capture\":null,\"runtime_truth_inputs\":[]}";
  out += ",\"detections\":[";
  for(size_t i=0;i<r.detections.size();++i){const auto&d=r.detections[i];if(i)out.push_back(',');
    out += "{\"bistatic_range_m\":";number(out,d.range_m);out += ",\"bistatic_velocity_mps\":";number(out,d.range_rate_mps);
    out += ",\"score\":";number(out,d.score);out += ",\"decision_statistic\":";number(out,d.decision_statistic);
    out += ",\"decision_threshold\":";number(out,d.decision_threshold);out += ",\"effective_decision_threshold\":";
    number(out,d.effective_decision_threshold);out += ",\"source_component_iteration\":"+std::to_string(d.source_component_iteration);out += ",\"bulk_component_iteration\":"+std::to_string(d.bulk_component_iteration);
    out += ",\"complex_coefficient\":[";number(out,d.complex_coefficient.real());out.push_back(',');
    number(out,d.complex_coefficient.imag());out += "],\"component_lineage\":[";
    for(size_t lineage=0;lineage<d.component_lineage.size();++lineage){if(lineage)out.push_back(',');out+=std::to_string(d.component_lineage[lineage]);}
    out += ']';
    if(d.aoa.valid){out += ",\"azimuth_deg\":";number(out,d.aoa.azimuth_deg);
      out += ",\"elevation_deg\":";number(out,d.aoa.elevation_deg);}
    out += ",\"range_rate_covariance\":";if(d.covariance_valid)matrix(out,d.range_rate_covariance);else out+="null";
    out += ",\"aoa\":{\"valid\":";out += d.aoa.valid?"true":"false";out += ",\"reason\":";string_value(out,d.aoa.reason);
    out += ",\"azimuth_deg\":";number(out,d.aoa.azimuth_deg);out += ",\"elevation_deg\":";number(out,d.aoa.elevation_deg);
    out += ",\"direction_enu\":[";number(out,d.aoa.direction.x);out.push_back(',');number(out,d.aoa.direction.y);out.push_back(',');number(out,d.aoa.direction.z);out+="]";
    out += ",\"covariance_rad2\":";if(d.aoa.covariance_valid)matrix(out,d.aoa.covariance_rad2);else out+="null";
    out += ",\"phase_fit_residual_rms_rad\":";number(out,d.aoa.phase_fit_residual_rms_rad);
    out += ",\"relative_manifold_residual_energy\":";number(out,d.aoa.relative_manifold_residual_energy);
    out += ",\"visible_region_clipped\":";out += d.aoa.visible_region_clipped?"true":"false";
    out += ",\"phase_wraps_tested\":"+std::to_string(d.aoa.phase_wraps_tested);
    out += ",\"component_aoa_count\":"+std::to_string(d.aoa.component_aoa_count);
    out += ",\"component_direction_coherence\":";number(out,d.aoa.component_direction_coherence);
    out += ",\"component_direction_rms_deg\":";number(out,d.aoa.component_direction_rms_deg);
    out += ",\"component_direction_max_deg\":";number(out,d.aoa.component_direction_max_deg);
    out += ",\"covariance_status\":";string_value(out,d.aoa.covariance_status);out+='}';
    out += ",\"ul_confirmation\":{\"supported\":";
    out += d.ul_confirmation_supported?"true":"false";
    out += ",\"candidate_specific\":";
    out += d.ul_confirmation_candidate_specific?"true":"false";
    out += ",\"status\":";string_value(out,d.ul_confirmation_status);out+="}";out+='}';}out+=']';
  out += ",\"spatial_receivers\":[";
  for(size_t receiver=0;receiver<r.spatial_receivers.size();++receiver){
    const auto& spatial=r.spatial_receivers[receiver];if(receiver)out.push_back(',');
    out += "{\"receiver_id\":";string_value(out,spatial.receiver_id);
    out += ",\"receiver_position_enu_m\":[";number(out,spatial.receiver_position.x);out.push_back(',');
    number(out,spatial.receiver_position.y);out.push_back(',');number(out,spatial.receiver_position.z);out+=']';
    out += ",\"sync\":{\"rows\":"+std::to_string(spatial.sync.rows)+",\"admitted_rows\":"
        +std::to_string(spatial.sync.admitted_rows)+",\"los_bins\":";number(out,spatial.sync.los_bins);
    out += ",\"sto_applied\":";out += spatial.sync.sto_applied?"true":"false";
    out += ",\"sfo_applied\":";out += spatial.sync.sfo_applied?"true":"false";
    out += ",\"cfo_applied\":";out += spatial.sync.cfo_applied?"true":"false";out+='}';
    out += ",\"allocation_alignment\":{\"families\":"+std::to_string(spatial.detector_alignment.families)
        +",\"repeated_families\":"+std::to_string(spatial.detector_alignment.repeated_families)
        +",\"aligned_rows\":"+std::to_string(spatial.detector_alignment.aligned_rows)
        +",\"singleton_rows\":"+std::to_string(spatial.detector_alignment.singleton_rows)+'}';
    out += ",\"causal_clutter\":{\"families\":"+std::to_string(spatial.causal_clutter.families)
        +",\"bootstrap_families\":"+std::to_string(spatial.causal_clutter.bootstrap_families)
        +",\"registered_families\":"+std::to_string(spatial.causal_clutter.registered_families)
        +",\"predicted_cells\":"+std::to_string(spatial.causal_clutter.predicted_cells)
        +",\"updated_cells\":"+std::to_string(spatial.causal_clutter.updated_cells)
        +",\"innovation_rejections\":"+std::to_string(spatial.causal_clutter.innovation_rejections)
        +",\"mean_update_gain\":";
    number(out,spatial.causal_clutter.mean_update_gain);out+='}';
    out += ",\"covariance\":{\"policy\":\"current_cpi_global_within_family_first_difference\",\"variance\":";
    number(out,spatial.current_cpi_variance);out += ",\"families\":"
        +std::to_string(spatial.covariance_family_count)+",\"difference_samples\":"
        +std::to_string(spatial.covariance_difference_count)+'}';
    out += ",\"range_res_m\":";number(out,spatial.detector.axes.range_res_m);
    out += ",\"vel_res_mps\":";number(out,spatial.detector.axes.rate_res_mps);
    out += ",\"dwell_s\":";number(out,spatial.detector.axes.dwell_s);
    out += ",\"detector_stop\":{\"reason\":";string_value(out,spatial.detector.stop_reason);
    out += ",\"threshold_first_iteration\":";number(out,spatial.detector.adaptive_threshold);
    out += ",\"psf_range_halfwidth_bins\":"+std::to_string(spatial.detector.psf_range_halfwidth_bins)
        +",\"psf_doppler_halfwidth_bins\":"+std::to_string(spatial.detector.psf_doppler_halfwidth_bins)
        +",\"effective_hypotheses\":"+std::to_string(spatial.detector.effective_hypotheses)
        +",\"split_validation_rejections\":"+std::to_string(spatial.detector.split_validation_rejections)
        +",\"skirt_components\":"+std::to_string(spatial.detector.skirt_components)
        +",\"null_scale\":";number(out,spatial.detector.null_scale);
    out += ",\"null_scale_source\":";string_value(out,spatial.detector.null_scale_source);
    out += ",\"calibration_capture\":null,\"tracker_feedback\":false,\"aoa\":false,\"runtime_truth_inputs\":[]}";
    // (2026-09-22) OFFLINE DIAGNOSTIC (capture_rvm only): every CLEAN component with its two CFAR
    // statistics and the skirt verdict, so the greatest-of rule can be audited against ground truth.
    if (c.capture_rvm) {
      out += ",\"clean_components\":[";
      for (size_t i = 0; i < spatial.detector.components.size(); ++i) {
        const auto& cc = spatial.detector.components[i]; if (i) out.push_back(',');
        out += "{\"range_bin\":";number(out,cc.range_bin);
        out += ",\"doppler_bin\":";number(out,cc.doppler_bin);
        out += ",\"row_z\":";number(out,cc.local.z);
        out += ",\"row_threshold\":";number(out,cc.local_threshold);
        out += ",\"column_z\":";number(out,cc.column_z);
        out += ",\"column_threshold\":";number(out,cc.column_threshold);
        out += ",\"column_training_cells\":"+std::to_string(cc.column_training_cells);
        out += ",\"skirt\":";out += cc.skirt?"true":"false";
        out += ",\"iteration\":"+std::to_string(cc.iteration);
        out += ",\"score\":";number(out,cc.score);out += '}';
      }
      out += ']';
    }
    out += ",\"detections\":[";
    for(size_t i=0;i<spatial.detections.size();++i){const auto&d=spatial.detections[i];if(i)out.push_back(',');
      out += "{\"bistatic_range_m\":";number(out,d.range_m);
      out += ",\"bistatic_velocity_mps\":";number(out,d.range_rate_mps);
      out += ",\"score\":";number(out,d.score);out += ",\"decision_statistic\":";number(out,d.decision_statistic);
      out += ",\"decision_threshold\":";number(out,d.decision_threshold);
      out += ",\"split_validated\":";out += d.split_validated?"true":"false";
      out += ",\"split_minimum_z\":";number(out,d.split_minimum_z);
      out += ",\"split_threshold\":";number(out,d.split_threshold);
      out += ",\"range_rate_covariance\":";if(d.covariance_valid)matrix(out,d.range_rate_covariance);else out+="null";
      out += ",\"complex_coefficient\":[";number(out,d.complex_coefficient.real());out.push_back(',');
      number(out,d.complex_coefficient.imag());out += "],\"component_lineage\":[";
      for(size_t lineage=0;lineage<d.component_lineage.size();++lineage){if(lineage)out.push_back(',');out+=std::to_string(d.component_lineage[lineage]);}
      out += ']';
      out += ",\"object_component_count\":"+std::to_string(d.object_component_count);
      out += ",\"source_component_iteration\":"+std::to_string(d.source_component_iteration);
      out += ",\"bulk_component_iteration\":"+std::to_string(d.bulk_component_iteration)+'}';}
    out+=']';
    out += ",\"long_dwells\":[";
    for(size_t li=0;li<spatial.long_dwells.size();++li){const auto&L=spatial.long_dwells[li];if(li)out.push_back(',');
      out += "{\"dwell_s\":";number(out,L.dwell_s);out += ",\"mid_time_slots\":";number(out,L.mid_time_slots);
      out += ",\"rows\":"+std::to_string(L.rows)+",\"detections\":[";
      for(size_t i=0;i<L.detections.size();++i){const auto&d=L.detections[i];if(i)out.push_back(',');
        out += "{\"bistatic_range_m\":";number(out,d.range_m);
        out += ",\"bistatic_velocity_mps\":";number(out,d.range_rate_mps);
        out += ",\"score\":";number(out,d.score);out += ",\"decision_statistic\":";number(out,d.decision_statistic);
        out += ",\"decision_threshold\":";number(out,d.decision_threshold);
        out += ",\"range_rate_covariance\":";if(d.covariance_valid)matrix(out,d.range_rate_covariance);else out+="null";
        out += ",\"source_component_iteration\":"+std::to_string(d.source_component_iteration);
        out += ",\"bulk_component_iteration\":"+std::to_string(d.bulk_component_iteration);
        out += ",\"dwell_s\":";number(out,d.dwell_s);out += '}';}
      out += "]}";}
    out += ']';
    out += ",\"uplink_dtd_dfs\":{\"present\":";
    out += spatial.uplink_present?"true":"false";
    out += ",\"pusch_session_id\":"+std::to_string(spatial.uplink_session_id);
    out += ",\"valid\":";out += spatial.uplink_valid?"true":"false";
    out += ",\"search_complete\":";out += spatial.uplink_search_complete?"true":"false";
    out += ",\"same_pusch_direct_reference\":";
    out += spatial.uplink_valid?"true":"false";
    out += ",\"error\":";string_value(out,spatial.uplink_error);
    out += ",\"reference_policy\":\"earliest_persistent_current_pusch\"";
    out += ",\"runtime_truth_inputs\":[],\"external_calibration_inputs\":[]";
    if(spatial.uplink_valid){
      out += ",\"range_res_m\":";number(out,spatial.uplink_detector.axes.range_res_m);
      out += ",\"vel_res_mps\":";number(out,spatial.uplink_detector.axes.rate_res_mps);
      out += ",\"detector_stop\":{\"reason\":";
      string_value(out,spatial.uplink_detector.stop_reason);
      out += ",\"threshold_first_iteration\":";
      number(out,spatial.uplink_detector.adaptive_threshold);
      out += ",\"psf_range_halfwidth_bins\":"
          +std::to_string(spatial.uplink_detector.psf_range_halfwidth_bins)
          +",\"psf_doppler_halfwidth_bins\":"
          +std::to_string(spatial.uplink_detector.psf_doppler_halfwidth_bins)
          +",\"component_count\":"
          +std::to_string(spatial.uplink_detector.components.size())
          +",\"object_count\":"
          +std::to_string(spatial.uplink_detector.objects.size())+'}';
      out += ",\"direct_reference\":{\"rows\":"+std::to_string(spatial.uplink_sync.rows)
          +",\"admitted_rows\":"+std::to_string(spatial.uplink_sync.admitted_rows)
          +",\"delay_bins\":";number(out,spatial.uplink_sync.los_bins);
      out += ",\"delay_standard_error_bins\":";
      number(out,spatial.uplink_sync.sto_standard_error_bins);
      out += ",\"cfo_resolution_hz\":";number(out,spatial.uplink_sync.cfo_resolution_hz);
      out += ",\"range_rate_valid\":";
      out += spatial.uplink_direct_path_rate_valid?"true":"false";
      out += ",\"range_rate_mps\":";
      number(out,spatial.uplink_direct_path_range_rate_mps);
      out += ",\"range_rate_variance_mps2\":";
      number(out,spatial.uplink_direct_path_rate_variance_mps2);
      out += "}";
      out += ",\"detections\":[";
      for(size_t i=0;i<spatial.uplink_detections.size();++i){
        const auto&d=spatial.uplink_detections[i];if(i)out.push_back(',');
        out += "{\"delta_path_range_m\":";number(out,d.range_m);
        out += ",\"delta_path_range_rate_mps\":";number(out,d.range_rate_mps);
        out += ",\"score\":";number(out,d.score);
        out += ",\"range_rate_covariance\":";
        if(d.covariance_valid)matrix(out,d.range_rate_covariance);else out+="null";
        out += ",\"source_component_iteration\":"+std::to_string(d.source_component_iteration);
        out += ",\"bulk_component_iteration\":"+std::to_string(d.bulk_component_iteration);
        out += '}';
      }
      out += ']';
    }
    out += '}';
    if(emit_maps){
      const auto& axes=spatial.detector.axes;
      const uint32_t nb=(c.rvm_max_range_m>0.0 && axes.range_res_m>0.0)
          ? std::min<uint32_t>(axes.range_bins,(uint32_t)std::ceil(c.rvm_max_range_m/axes.range_res_m))
          : axes.range_bins;
      out += ",\"rvm_layout\":\"doppler_major_range_minor\",\"rvm_range_bins\":"+std::to_string(nb)
          +",\"rvm_rate_bins\":"+std::to_string(axes.rate_bins)+",\"rvm_range_res_m\":";
      number(out,axes.range_res_m);out += ",\"rvm_rate_res_mps\":";number(out,axes.rate_res_mps);
      out += ",\"rvm_blob\":[";
      bool first_spatial=true;for(uint32_t d=0;d<axes.rate_bins;++d)
        for(uint32_t q=0;q<nb;++q){if(!first_spatial)out.push_back(',');first_spatial=false;
          const double value=spatial.detector.initial_likelihood[(size_t)q*axes.rate_bins+d];
          number(out,std::isfinite(value)&&value>0.0?value:0.0);}out+=']';
      if(spatial.detector.final_likelihood.size()==spatial.detector.initial_likelihood.size()){out += ",\"rvm_final_blob\":[";
        bool first_f=true;for(uint32_t d=0;d<axes.rate_bins;++d)
          for(uint32_t q=0;q<nb;++q){if(!first_f)out.push_back(',');first_f=false;
            const double value=spatial.detector.final_likelihood[(size_t)q*axes.rate_bins+d];
            number(out,std::isfinite(value)&&value>0.0?value:0.0);}out+=']';}}
    out+='}';
  }
  out+=']';
  out += ",\"uplink_sessions\":[";
  for (size_t session_index = 0;
       session_index < r.uplink_session_receivers.size(); ++session_index) {
    const auto& session = r.uplink_session_receivers[session_index];
    if (session_index) out.push_back(',');
    out += "{\"pusch_session_id\":";
    out += std::to_string(session.empty() ? 0 : session.front().uplink_session_id);
    out += ",\"receivers\":[";
    for (size_t receiver_index = 0; receiver_index < session.size(); ++receiver_index) {
      const auto& spatial = session[receiver_index];
      if (receiver_index) out.push_back(',');
      out += "{\"receiver_id\":";string_value(out,spatial.receiver_id);
      out += ",\"present\":";out += spatial.uplink_present?"true":"false";
      out += ",\"valid\":";out += spatial.uplink_valid?"true":"false";
      out += ",\"search_complete\":";
      out += spatial.uplink_search_complete?"true":"false";
      out += ",\"error\":";string_value(out,spatial.uplink_error);
      out += ",\"runtime_truth_inputs\":[],\"external_calibration_inputs\":[]";
      if (spatial.uplink_valid) {
        out += ",\"range_res_m\":";number(out,spatial.uplink_detector.axes.range_res_m);
        out += ",\"vel_res_mps\":";number(out,spatial.uplink_detector.axes.rate_res_mps);
        out += ",\"detector_stop_reason\":";
        string_value(out,spatial.uplink_detector.stop_reason);
        out += ",\"direct_reference\":{\"delay_bins\":";
        number(out,spatial.uplink_sync.los_bins);
        out += ",\"delay_standard_error_bins\":";
        number(out,spatial.uplink_sync.sto_standard_error_bins);
        out += ",\"admitted_rows\":"
            +std::to_string(spatial.uplink_sync.admitted_rows);
        // per-session direct-path Doppler (same fields as the primary-session block) so every
        // UE session can feed the UE localiser without a mobility prior
        out += ",\"range_rate_valid\":";
        out += spatial.uplink_direct_path_rate_valid?"true":"false";
        out += ",\"range_rate_mps\":";
        number(out,spatial.uplink_direct_path_range_rate_mps);
        out += ",\"range_rate_variance_mps2\":";
        number(out,spatial.uplink_direct_path_rate_variance_mps2);
        out += '}';
        // OFFLINE DIAGNOSTIC: per-session UL map (pre-CLEAN), same layout as the DL rvm_blob.
        // Map and clean_diag are gated independently: the map is a raster (emit_maps-decimated,
        // like every other rvm_blob site), clean_diag is O(components) and stays on the DL
        // clean_components analog's (:280) undecimated c.capture_rvm gate -- decimating it buys
        // nothing and would contradict "component lists keep their current capture_rvm gating".
        if (emit_maps && !spatial.uplink_detector.initial_likelihood.empty()) {
          const auto& ax = spatial.uplink_detector.axes;
          const uint32_t nb=(c.rvm_max_range_m>0.0 && ax.range_res_m>0.0)
              ? std::min<uint32_t>(ax.range_bins,(uint32_t)std::ceil(c.rvm_max_range_m/ax.range_res_m))
              : ax.range_bins;
          out += ",\"rvm_layout\":\"doppler_major_range_minor\",\"rvm_range_bins\":"+std::to_string(nb)
              +",\"rvm_rate_bins\":"+std::to_string(ax.rate_bins)+",\"rvm_range_res_m\":";
          number(out,ax.range_res_m);out += ",\"rvm_rate_res_mps\":";number(out,ax.rate_res_mps);
          out += ",\"rvm_blob\":[";
          bool first=true;
          for(uint32_t d=0;d<ax.rate_bins;++d) for(uint32_t q=0;q<nb;++q){
            if(!first)out.push_back(',');first=false;
            const double v=spatial.uplink_detector.initial_likelihood[(size_t)q*ax.rate_bins+d];
            number(out,std::isfinite(v)&&v>0.0?v:0.0);}
          out+=']';
        }
        if (c.capture_rvm && !spatial.uplink_detector.initial_likelihood.empty()) {
          const auto& ud = spatial.uplink_detector;
          out += ",\"clean_diag\":{\"components\":"+std::to_string(ud.components.size())
              +",\"skirt_components\":"+std::to_string(ud.skirt_components)
              +",\"objects\":"+std::to_string(ud.objects.size())
              +",\"split_validation_rejections\":"+std::to_string(ud.split_validation_rejections)
              +",\"identifiability_guard\":"+std::to_string(ud.identifiability_guard)
              +",\"psf_range_halfwidth_bins\":"+std::to_string(ud.psf_range_halfwidth_bins)
              +",\"psf_doppler_halfwidth_bins\":"+std::to_string(ud.psf_doppler_halfwidth_bins)
              +",\"component_list\":[";
          for(size_t i=0;i<ud.components.size();++i){const auto& cc=ud.components[i]; if(i)out.push_back(',');
            out += "{\"r\":";number(out,cc.range_bin);out += ",\"d\":";number(out,cc.doppler_bin);
            out += ",\"z\":";number(out,cc.local.z);out += ",\"thr\":";number(out,cc.local_threshold);
            out += ",\"skirt\":";out += cc.skirt?"true":"false";out += ",\"score\":";number(out,cc.score);out+='}';}
          out += "]}";
        }
        out += ",\"long_dwells\":[";
        for(size_t li=0;li<spatial.uplink_long_dwells.size();++li){
          const auto& L=spatial.uplink_long_dwells[li]; if(li)out.push_back(',');
          out += "{\"dwell_s\":";number(out,L.dwell_s);out += ",\"mid_time_slots\":";number(out,L.mid_time_slots);
          out += ",\"rows\":"+std::to_string(L.rows)+",\"detections\":[";
          for(size_t i=0;i<L.detections.size();++i){const auto&d=L.detections[i];if(i)out.push_back(',');
            out += "{\"delta_path_range_m\":";number(out,d.range_m);
            out += ",\"delta_path_range_rate_mps\":";number(out,d.range_rate_mps);
            out += ",\"score\":";number(out,d.score);
            out += ",\"decision_statistic\":";number(out,d.decision_statistic);
            out += ",\"decision_threshold\":";number(out,d.decision_threshold);
            out += ",\"dwell_s\":";number(out,d.dwell_s);out += '}';}
          out += "]}";}
        out += ']';
        out += ",\"detections\":[";
        for (size_t detection_index = 0;
             detection_index < spatial.uplink_detections.size(); ++detection_index) {
          const auto& detection = spatial.uplink_detections[detection_index];
          if (detection_index) out.push_back(',');
          out += "{\"delta_path_range_m\":";number(out,detection.range_m);
          out += ",\"delta_path_range_rate_mps\":";
          number(out,detection.range_rate_mps);
          out += ",\"score\":";number(out,detection.score);
          out += ",\"range_rate_covariance\":";
          if(detection.covariance_valid)matrix(out,detection.range_rate_covariance);
          else out += "null";
          out += '}';
        }
        out += ']';
      }
      out += '}';
    }
    out += "]}";
  }
  out += ']';
  if(!r.spatial_receivers.empty()){
    const auto& processing=r.multistatic_tracker_processing;
    out += ",\"multistatic_tracker_processing\":{\"budget_s\":";
    number(out,processing.processing_budget_s);out += ",\"elapsed_s\":";
    number(out,processing.processing_elapsed_s);
    out += ",\"birth_elapsed_s\":"; number(out,processing.birth_elapsed_s);
    out += ",\"ul_elapsed_s\":"; number(out,processing.ul_elapsed_s);
    out += ",\"local_map_cells\":"+std::to_string(processing.local_map_cells)
        +",\"asynchronous_proposals\":"+std::to_string(processing.asynchronous_proposals)
        +",\"soft_supported_epochs\":"+std::to_string(processing.soft_supported_epochs)
        +",\"visibility_negative_epochs\":"+std::to_string(processing.visibility_negative_epochs)
        +",\"evidence_retirements\":"+std::to_string(processing.evidence_retirements);
    out += ",\"association_hypotheses\":"+std::to_string(processing.association_hypotheses)
        +",\"tracklet_hypotheses\":"+std::to_string(processing.tracklet_hypotheses)
        +",\"birth_tuple_hypotheses\":"+std::to_string(processing.birth_tuple_hypotheses)
        +",\"birth_queue_pops\":"+std::to_string(processing.birth_queue_pops)
        +",\"birth_fit_attempts\":"+std::to_string(processing.birth_fit_attempts)
        +",\"birth_cross_check_rejections\":"+std::to_string(processing.birth_cross_check_rejections)
        +",\"birth_fit_valid\":"+std::to_string(processing.birth_fit_valid)
        +",\"birth_ground_floor_rejections\":"
        +std::to_string(processing.birth_ground_floor_rejections)
        +",\"birth_multimodal_covariance_widenings\":"
        +std::to_string(processing.birth_multimodal_covariance_widenings)
        +",\"birth_four_receiver_refits\":"
        +std::to_string(processing.birth_four_receiver_refits)
        +",\"birth_ue_geometry_disambiguations\":"
        +std::to_string(processing.birth_ue_geometry_disambiguations)
        +",\"ul_track_corrections\":"
        +std::to_string(processing.ul_track_corrections)
        +",\"ul_ue_geometry_validations\":"
        +std::to_string(processing.ul_ue_geometry_validations)
        +",\"birth_seeds_by_receiver\":["+std::to_string(processing.birth_seeds_by_receiver[0])
        +","+std::to_string(processing.birth_seeds_by_receiver[1])
        +","+std::to_string(processing.birth_seeds_by_receiver[2])
        +","+std::to_string(processing.birth_seeds_by_receiver[3])+"]"
        +",\"admitted_birth_solutions\":"+std::to_string(processing.admitted_birth_solutions)
        +",\"soft_map_queries\":"+std::to_string(processing.soft_map_queries)
        +",\"conditioned_soft_queries\":"+std::to_string(processing.conditioned_soft_queries)
        +",\"soft_explanation_reductions\":"+std::to_string(processing.soft_explanation_reductions)
        +",\"duplicate_consolidations\":"+std::to_string(processing.duplicate_consolidations)
        +",\"owned_seed_exclusions\":"+std::to_string(processing.owned_seed_exclusions)
        +",\"object_discrepancy_updates\":"+std::to_string(processing.object_discrepancy_updates)
        +",\"predictive_folds\":"+std::to_string(processing.predictive_folds)
        +",\"soft_birth_rejections\":"+std::to_string(processing.soft_birth_rejections)
        +",\"reflected_birth_rejections\":"+std::to_string(processing.reflected_birth_rejections)
        +",\"reflected_path_assignments\":"+std::to_string(processing.reflected_path_assignments)
        +",\"reflector_pair_updates\":"+std::to_string(processing.reflector_pair_updates)
        +",\"admitted_reflector_planes\":"+std::to_string(processing.admitted_reflector_planes)
        +",\"best_provisional_plane_valid\":"+(processing.best_provisional_plane_valid?"true":"false")
        +",\"best_provisional_plane_bic\":";
    number(out, processing.best_provisional_plane_bic);
    out += ",\"best_provisional_plane_offset_m\":";
    number(out, processing.best_provisional_plane_offset_m);
    out += ",\"best_provisional_plane_normal\":[";
    number(out, processing.best_provisional_plane_normal[0]); out += ",";
    number(out, processing.best_provisional_plane_normal[1]); out += ",";
    number(out, processing.best_provisional_plane_normal[2]);
    out += "]"
        +std::string(",\"best_provisional_plane_supporting_pairs\":")
        +std::to_string(processing.best_provisional_plane_supporting_pairs)
        +",\"best_provisional_plane_supporting_parent_tracks\":"
        +std::to_string(processing.best_provisional_plane_supporting_parent_tracks)
        +",\"micro_doppler_sideband_candidates\":"
        +std::to_string(processing.micro_doppler_sideband_candidates)
        +",\"micro_doppler_sideband_suppressed\":"
        +std::to_string(processing.micro_doppler_sideband_suppressed)
        +",\"ul_validation_attempts\":"+std::to_string(processing.ul_validation_attempts)
        +",\"ul_supported_birth_solutions\":"+std::to_string(processing.ul_supported_birth_solutions)
        +",\"ul_contradicted_birth_solutions\":"+std::to_string(processing.ul_contradicted_birth_solutions)
        +",\"ul_unavailable_birth_solutions\":"+std::to_string(processing.ul_unavailable_birth_solutions)
        +",\"ul_track_validation_attempts\":"+std::to_string(processing.ul_track_validation_attempts)
        +",\"ul_supported_track_updates\":"+std::to_string(processing.ul_supported_track_updates)
        +",\"ul_contradicted_track_updates\":"+std::to_string(processing.ul_contradicted_track_updates)
        +",\"ul_unavailable_track_updates\":"+std::to_string(processing.ul_unavailable_track_updates)
        +",\"ul_ue_bootstrap_attempts\":"+std::to_string(processing.ul_ue_bootstrap_attempts)
        +",\"ul_ue_bootstrap_promotions\":"+std::to_string(processing.ul_ue_bootstrap_promotions)
        +",\"ul_ue_crossvalidated_proposals\":"+std::to_string(processing.ul_ue_crossvalidated_proposals)
        +",\"ul_ue_causal_confirmations\":"+std::to_string(processing.ul_ue_causal_confirmations)
        +",\"ul_ue_inconsistent_proposals\":"+std::to_string(processing.ul_ue_inconsistent_proposals)
        +",\"ul_ue_bootstrap_unavailable\":"+std::to_string(processing.ul_ue_bootstrap_unavailable)
        +",\"ul_ue_multiplicity_rejections\":"+std::to_string(processing.ul_ue_multiplicity_rejections)
        +",\"ul_unknown_session_batches\":"+std::to_string(processing.ul_unknown_session_batches)
        +",\"ul_session_count\":"+std::to_string(processing.ul_session_count)
        +",\"ul_established_session_count\":"+std::to_string(processing.ul_established_session_count)
        +",\"ul_training_hypotheses\":"+std::to_string(processing.ul_training_hypotheses)
        +",\"ul_fitted_ue_states\":"+std::to_string(processing.ul_fitted_ue_states)
        +",\"ul_processing_deferred\":"+std::to_string(processing.ul_processing_deferred)
        +",\"birth_search_deadline_exhausted\":";
    out += processing.birth_search_deadline_exhausted?"true":"false";
    out += ",\"processing_deadline_exhausted\":";
    out += processing.processing_deadline_exhausted?"true":"false";out+='}';
  }
  out += ",\"uplink\":{\"present\":";
  out += r.uplink_present ? "true" : "false";
  out += ",\"pusch_session_id\":" + std::to_string(r.uplink_session_id);
  out += ",\"valid\":";
  out += r.uplink_valid ? "true" : "false";
  out += ",\"error\":";
  string_value(out, r.uplink_error);
  if (r.uplink_valid) {
    out += ",\"actual_row_count\":" + std::to_string(r.uplink_detector.axes.rate_bins);
    out += ",\"observed_re_count\":" + std::to_string(r.uplink_detector.axes.observed_re_count);
    out += ",\"range_res_m\":";number(out,r.uplink_detector.axes.range_res_m);
    out += ",\"vel_res_mps\":";number(out,r.uplink_detector.axes.rate_res_mps);
    out += ",\"dwell_s\":";number(out,r.uplink_detector.axes.dwell_s);
    out += ",\"adaptive_threshold\":";number(out,r.uplink_detector.adaptive_threshold);
    out += ",\"sync\":{\"rows\":"+std::to_string(r.uplink_sync.rows)
        +",\"admitted_rows\":"+std::to_string(r.uplink_sync.admitted_rows)+",\"los_bins\":";
    number(out,r.uplink_sync.los_bins);out += ",\"sto\":{\"mean_frac_bin\":";
    number(out,r.uplink_sync.sto_bins);out += ",\"applied\":";
    out += r.uplink_sync.sto_applied?"true":"false";out += "},\"sfo\":{\"sfo_ppm\":";
    number(out,r.uplink_sync.sfo_ppm);out += ",\"applied\":";
    out += r.uplink_sync.sfo_applied?"true":"false";out += "},\"cfo\":{\"cfo_hz\":";
    number(out,r.uplink_sync.cfo_hz);out += ",\"applied\":";
    out += r.uplink_sync.cfo_applied?"true":"false";out += "},\"reject_reason\":";
    string_value(out,r.uplink_sync.reject_reason);out += '}';
    out += ",\"allocation_alignment\":{\"families\":"
        +std::to_string(r.uplink_detector_alignment.families)
        +",\"repeated_families\":"+std::to_string(r.uplink_detector_alignment.repeated_families)
        +",\"aligned_rows\":"+std::to_string(r.uplink_detector_alignment.aligned_rows)
        +",\"singleton_rows\":"+std::to_string(r.uplink_detector_alignment.singleton_rows)+'}';
    out += ",\"covariance\":{\"policy\":\"current_cpi_global_within_family_first_difference\",\"variance\":";
    number(out,r.uplink_current_cpi_variance);out += ",\"families\":"
        +std::to_string(r.uplink_covariance_family_count)+",\"difference_samples\":"
        +std::to_string(r.uplink_covariance_difference_count)+'}';
    out += ",\"detections\":[";
    for(size_t i=0;i<r.uplink_detections.size();++i){const auto&d=r.uplink_detections[i];if(i)out.push_back(',');
      out += "{\"bistatic_range_m\":";number(out,d.range_m);out += ",\"bistatic_velocity_mps\":";number(out,d.range_rate_mps);
      out += ",\"score\":";number(out,d.score);out += ",\"decision_statistic\":";number(out,d.decision_statistic);
      out += ",\"decision_threshold\":";number(out,d.decision_threshold);out += ",\"effective_decision_threshold\":";
      number(out,d.effective_decision_threshold);out += ",\"source_component_iteration\":"+std::to_string(d.source_component_iteration);out += ",\"bulk_component_iteration\":"+std::to_string(d.bulk_component_iteration);
      out += ",\"aoa\":{\"valid\":";out += d.aoa.valid?"true":"false";out += ",\"reason\":";
      string_value(out,d.aoa.reason);out += "}}";}out+=']';
    out += ",\"tracks\":[";
    for(size_t i=0;i<r.uplink_tracks.size();++i){const auto&t=r.uplink_tracks[i];if(i)out.push_back(',');
      out += "{\"track_id\":"+std::to_string(t.track_id)+",\"status\":";string_value(out,t.status);
      out += ",\"updated_this_cpi\":";out += t.updated?"true":"false";
      out += ",\"bistatic_range_m\":";number(out,t.range_m);out += ",\"bistatic_velocity_mps\":";
      number(out,t.range_rate_mps);out += ",\"coast_count\":"+std::to_string(t.coast_count)+'}';}out+=']';
    if(emit_maps){
      const auto& ax=r.uplink_detector.axes;
      const uint32_t nb=(c.rvm_max_range_m>0.0 && ax.range_res_m>0.0)
          ? std::min<uint32_t>(ax.range_bins,(uint32_t)std::ceil(c.rvm_max_range_m/ax.range_res_m))
          : ax.range_bins;
      out += ",\"ul_rvm_layout\":\"doppler_major_range_minor\",\"ul_rvm_range_bins\":"+std::to_string(nb)
          +",\"ul_rvm_rate_bins\":"+std::to_string(ax.rate_bins)+",\"ul_rvm_range_res_m\":";
      number(out,ax.range_res_m);out += ",\"ul_rvm_rate_res_mps\":";number(out,ax.rate_res_mps);
      out += ",\"ul_rvm_blob\":[";
      bool first_ul=true;for(uint32_t d=0;d<ax.rate_bins;++d)
        for(uint32_t q=0;q<nb;++q){if(!first_ul)out.push_back(',');first_ul=false;
          const double value=r.uplink_detector.initial_likelihood[(size_t)q*ax.rate_bins+d];
          number(out,std::isfinite(value)&&value>0.0?value:0.0);}out+=']';}
  }
  out += '}';
  out += ",\"tracks\":[";
  for(size_t i=0;i<r.tracks.size();++i){const auto&t=r.tracks[i];if(i)out.push_back(',');out += "{\"track_id\":"+std::to_string(t.track_id)+",\"status\":";
    string_value(out,t.status);out += ",\"updated_this_cpi\":";out += t.updated?"true":"false";
    out += ",\"bistatic_range_m\":";number(out,t.range_m);out += ",\"bistatic_velocity_mps\":";number(out,t.range_rate_mps);
    out += ",\"position\":";if(t.position_valid){out.push_back('[');number(out,t.position_enu_m.x);out.push_back(',');number(out,t.position_enu_m.y);out.push_back(',');number(out,t.position_enu_m.z);out.push_back(']');}else out+="null";
    out += ",\"velocity\":";if(t.position_valid){out.push_back('[');number(out,t.velocity_enu_mps.x);out.push_back(',');number(out,t.velocity_enu_mps.y);out.push_back(',');number(out,t.velocity_enu_mps.z);out.push_back(']');}else out+="null";
    out += ",\"position_covariance\":";if(t.position_valid)matrix(out,t.position_covariance);else out+="null";
    out += ",\"velocity_covariance\":";if(t.position_valid)matrix(out,t.velocity_covariance);else out+="null";
    out += ",\"azimuth_deg\":";number(out,t.azimuth_deg);out += ",\"elevation_deg\":";number(out,t.elevation_deg);
    out += ",\"coast_count\":"+std::to_string(t.coast_count);
    out += ",\"last_update_used_angles\":";out += t.last_update_used_angles?"true":"false";
    out += ",\"temporal_aoa_rejections\":"+std::to_string(t.temporal_aoa_rejections);
    out += ",\"auxiliary_aoa_updates\":"+std::to_string(t.auxiliary_aoa_updates);
    out += ",\"receiver_update_count\":"+std::to_string(t.receiver_update_count);
    out += ",\"geometry_condition\":";
    if(t.has_geometry_condition)number(out,t.geometry_condition);else out+="null";
    out += ",\"ul_validation\":{\"status\":";
    string_value(out,t.ul_validation_status);out += ",\"log_bayes_factor\":";
    number(out,t.ul_validation_log_bayes_factor);out += '}';
    out += ",\"path_model\":{\"direct_predictive_bic\":";
    number(out,t.direct_predictive_bic);out += ",\"reflected_predictive_bic\":";
    number(out,t.reflected_predictive_bic);
    out += ",\"direct_validation_epochs\":"+std::to_string(t.direct_validation_epochs)
        +",\"direct_support_epochs\":"+std::to_string(t.direct_support_epochs)
        +",\"direct_supported_receivers\":"+std::to_string(t.direct_supported_receivers)
        +",\"direct_validation_mean_log_bayes_factor\":";
    number(out,t.direct_validation_mean_log_bayes_factor);
    out += ",\"direct_emission_validated\":";
    out += t.direct_emission_validated?"true":"false";
    out += ",\"existence_score\":"; number(out,t.existence_score);
    out += ",\"existence_threshold\":"; number(out,t.existence_threshold);
    out += ",\"existence_receiver_mask\":"+std::to_string(t.existence_receiver_mask);
    out += ",\"shared_reflector_id\":"
        +std::to_string(t.shared_reflector_id)+",\"physical_parent_track_id\":"
        +std::to_string(t.physical_parent_track_id)+'}';
    out += ",\"origin_hypothesis\":{\"decision\":";
    string_value(out,t.origin_hypothesis);out += ",\"ready\":";
    out += t.origin_evidence_ready?"true":"false";
    out += ",\"log_evidence\":[";
    for(size_t h=0;h<t.origin_log_evidence.size();++h){if(h)out.push_back(',');number(out,t.origin_log_evidence[h]);}
    out += "],\"temporal_structure_log_bayes_factor\":";
    number(out,t.origin_temporal_structure_log_bayes_factor);
    out += ",\"cross_rx_structure_log_bayes_factor\":";
    number(out,t.origin_cross_rx_structure_log_bayes_factor);
    out += ",\"stationary_log_bayes_factor\":";
    number(out,t.origin_stationary_log_bayes_factor);
    out += ",\"ul_direct_log_bayes_factor\":";
    number(out,t.origin_ul_direct_log_bayes_factor);
    out += ",\"exclusivity_log_bayes_factor\":";
    number(out,t.origin_exclusivity_log_bayes_factor);
    out += ",\"exclusivity_competitor_track_id\":"
        +std::to_string(t.origin_exclusivity_competitor_track_id);
    out += ",\"observed_epochs\":"+std::to_string(t.origin_observed_epochs)
        +",\"cross_rx_epochs\":"+std::to_string(t.origin_cross_rx_epochs)+'}';
    out += ",\"imm_model_probabilities\":";
    if(t.imm_valid){out.push_back('[');for(size_t m=0;m<t.imm_model_probabilities.size();++m){if(m)out.push_back(',');number(out,t.imm_model_probabilities[m]);}out.push_back(']');}else out+="null";
    out += '}';}out+=']';
  out += ",\"dropped_submissions\":"+std::to_string(r.dropped_submissions)
      +",\"dropped_cpis\":"+std::to_string(r.dropped_cpis)
      +",\"discarded_pending_rows\":"+std::to_string(r.discarded_pending_rows)
      +",\"discarded_pending_intervals\":"+std::to_string(r.discarded_pending_intervals)
      +",\"stale_submissions\":"+std::to_string(r.stale_submissions);
  if(emit_maps){
    const auto& axes=r.detector.axes;
    const uint32_t nb=(c.rvm_max_range_m>0.0 && axes.range_res_m>0.0)
        ? std::min<uint32_t>(axes.range_bins,(uint32_t)std::ceil(c.rvm_max_range_m/axes.range_res_m))
        : axes.range_bins;
    out += ",\"rvm_layout\":\"doppler_major_range_minor\",\"rvm_range_bins\":"+std::to_string(nb)
        +",\"rvm_rate_bins\":"+std::to_string(axes.rate_bins)+",\"rvm_range_res_m\":";
    number(out,axes.range_res_m);out += ",\"rvm_rate_res_mps\":";number(out,axes.rate_res_mps);
    out += ",\"rvm_blob\":[";
    bool first_value=true;for(uint32_t d=0;d<axes.rate_bins;++d)for(uint32_t q=0;q<nb;++q){
      if(!first_value)out.push_back(',');
      first_value=false;const double value=r.detector.initial_likelihood[(size_t)q*axes.rate_bins+d];
      number(out,std::isfinite(value)&&value>0.0?value:0.0);}out+=']';
    out += ",\"dl_rvm_layout\":\"doppler_major_range_minor\""
        ",\"dl_rvm_source_mask\":" + std::to_string(DL_SOURCE_BITS)
        + ",\"dl_rvm_observed_re_count\":" + std::to_string(r.detector.dl_observed_re_count)
        + ",\"dl_rvm_blob\":[";
    first_value=true;for(uint32_t d=0;d<axes.rate_bins;++d)for(uint32_t q=0;q<nb;++q){
      if(!first_value)out.push_back(',');
      first_value=false;const double value=r.detector.initial_dl_likelihood[(size_t)q*axes.rate_bins+d];
      number(out,std::isfinite(value)&&value>0.0?value:0.0);}out+=']';}
  out += "}\n";return out;
}

ReportWriter::ReportWriter(const PipelineConfig& config) : config_(config)
{
  std::string path = config_.report_path;
  if (path.empty() && !config_.out_path.empty()) path = config_.out_path + "_reports.jsonl";
  if (!path.empty()) {
    const std::filesystem::path p(path);
    if (!p.parent_path().empty()) std::filesystem::create_directories(p.parent_path());
    file_.open(path, std::ios::out | std::ios::app);
  }
#ifdef ENABLE_ZEROMQ
  if (!config_.report_endpoint.empty()) {
    zmq_context_ = zmq_ctx_new(); zmq_socket_ = zmq_socket(zmq_context_, ZMQ_PUB);
    if (!zmq_socket_ || zmq_bind(zmq_socket_, config_.report_endpoint.c_str()) != 0) {
      if (zmq_socket_) zmq_close(zmq_socket_);
      if (zmq_context_) zmq_ctx_term(zmq_context_);
      zmq_socket_ = nullptr; zmq_context_ = nullptr;
    }
  }
#endif
}

ReportWriter::~ReportWriter()
{
#ifdef ENABLE_ZEROMQ
  if (zmq_socket_) zmq_close(zmq_socket_);
  if (zmq_context_) zmq_ctx_term(zmq_context_);
#endif
}

void ReportWriter::emit(const PipelineReport& report)
{
  const double now_ns = std::chrono::duration<double, std::nano>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  const bool emit_maps = config_.capture_rvm
      && (now_ns - last_rvm_emit_ns_) >= config_.rvm_period_s * 1e9;
  const std::string line = build_report_json(report, config_, emit_maps);
  if (emit_maps) last_rvm_emit_ns_ = now_ns;
  if (file_) { file_ << line; file_.flush(); }
#ifdef ENABLE_ZEROMQ
  if (zmq_socket_) zmq_send(zmq_socket_, line.data(), line.size() - 1, ZMQ_DONTWAIT);
#endif
}

} // namespace nr_isac
