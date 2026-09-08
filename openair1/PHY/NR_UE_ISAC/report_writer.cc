/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#include "report_writer.h"

#include <algorithm>
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

std::string build_report_json(const PipelineReport& r, const PipelineConfig& c)
{
  std::string out; out.reserve(c.capture_rvm ? r.detector.initial_likelihood.size() * 12 : 8192);
  out += "{\"schema\":\"oai.native_python_parity.v1\",\"rx_id\":"; string_value(out,c.rx_id);
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
  out += ",\"range_res_m\":"; number(out,r.detector.axes.range_res_m);
  out += ",\"range_max_m\":"; number(out,r.detector.axes.range_bins*r.detector.axes.range_res_m);
  out += ",\"vel_res_mps\":"; number(out,r.detector.axes.rate_res_mps);
  double vmax=0.0;for(double v:r.detector.axes.rate_axis_mps)vmax=std::max(vmax,std::abs(v));
  out += ",\"vel_max_mps\":";number(out,vmax);out += ",\"dwell_s\":";number(out,r.detector.axes.dwell_s);
  out += ",\"aoa_policy\":{\"master\":" + std::string(c.aoa_enable?"true":"false")
      + ",\"uplink_requested\":" + std::string(c.aoa_ul_enable_requested?"true":"false")
      + ",\"uplink\":" + std::string(c.aoa_ul_enable?"true":"false") + '}';
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
  out += ",\"detections\":[";
  for(size_t i=0;i<r.detections.size();++i){const auto&d=r.detections[i];if(i)out.push_back(',');
    out += "{\"bistatic_range_m\":";number(out,d.range_m);out += ",\"bistatic_velocity_mps\":";number(out,d.range_rate_mps);
    out += ",\"score\":";number(out,d.score);out += ",\"decision_statistic\":";number(out,d.decision_statistic);
    out += ",\"decision_threshold\":";number(out,d.decision_threshold);out += ",\"effective_decision_threshold\":";
    number(out,d.effective_decision_threshold);out += ",\"source_component_iteration\":"+std::to_string(d.source_component_iteration);
    if(d.aoa.valid){out += ",\"azimuth_deg\":";number(out,d.aoa.azimuth_deg);
      out += ",\"elevation_deg\":";number(out,d.aoa.elevation_deg);}
    out += ",\"range_rate_covariance\":";if(d.covariance_valid)matrix(out,d.range_rate_covariance);else out+="null";
    out += ",\"aoa\":{\"valid\":";out += d.aoa.valid?"true":"false";out += ",\"reason\":";string_value(out,d.aoa.reason);
    out += ",\"azimuth_deg\":";number(out,d.aoa.azimuth_deg);out += ",\"elevation_deg\":";number(out,d.aoa.elevation_deg);
    out += ",\"direction_enu\":[";number(out,d.aoa.direction.x);out.push_back(',');number(out,d.aoa.direction.y);out.push_back(',');number(out,d.aoa.direction.z);out+="]";
    out += ",\"covariance_rad2\":";if(d.aoa.covariance_valid)matrix(out,d.aoa.covariance_rad2);else out+="null";
    out += ",\"phase_fit_residual_rms_rad\":";number(out,d.aoa.phase_fit_residual_rms_rad);
    out += ",\"relative_manifold_residual_energy\":";number(out,d.aoa.relative_manifold_residual_energy);
    out += ",\"phase_wraps_tested\":"+std::to_string(d.aoa.phase_wraps_tested)+'}';out+='}';}out+=']';
  out += ",\"tracks\":[";
  for(size_t i=0;i<r.tracks.size();++i){const auto&t=r.tracks[i];if(i)out.push_back(',');out += "{\"track_id\":"+std::to_string(t.track_id)+",\"status\":";
    string_value(out,t.status);out += ",\"updated_this_cpi\":";out += t.updated?"true":"false";
    out += ",\"bistatic_range_m\":";number(out,t.range_m);out += ",\"bistatic_velocity_mps\":";number(out,t.range_rate_mps);
    out += ",\"position\":";if(t.position_valid){out.push_back('[');number(out,t.position_enu_m.x);out.push_back(',');number(out,t.position_enu_m.y);out.push_back(',');number(out,t.position_enu_m.z);out.push_back(']');}else out+="null";
    out += ",\"velocity\":";if(t.position_valid){out.push_back('[');number(out,t.velocity_enu_mps.x);out.push_back(',');number(out,t.velocity_enu_mps.y);out.push_back(',');number(out,t.velocity_enu_mps.z);out.push_back(']');}else out+="null";
    out += ",\"azimuth_deg\":";number(out,t.azimuth_deg);out += ",\"elevation_deg\":";number(out,t.elevation_deg);
    out += ",\"coast_count\":"+std::to_string(t.coast_count)+'}';}out+=']';
  out += ",\"dropped_submissions\":"+std::to_string(r.dropped_submissions)
      +",\"stale_submissions\":"+std::to_string(r.stale_submissions);
  if(c.capture_rvm){out += ",\"rvm_layout\":\"doppler_major_range_minor\",\"rvm_blob\":[";
    bool first_value=true;for(uint32_t d=0;d<r.detector.axes.rate_bins;++d)for(uint32_t q=0;q<r.detector.axes.range_bins;++q){
      if(!first_value)out.push_back(',');
      first_value=false;const double value=r.detector.initial_likelihood[(size_t)q*r.detector.axes.rate_bins+d];
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
  const std::string line = build_report_json(report, config_);
  if (file_) { file_ << line; file_.flush(); }
#ifdef ENABLE_ZEROMQ
  if (zmq_socket_) zmq_send(zmq_socket_, line.data(), line.size() - 1, ZMQ_DONTWAIT);
#endif
}

} // namespace nr_isac
