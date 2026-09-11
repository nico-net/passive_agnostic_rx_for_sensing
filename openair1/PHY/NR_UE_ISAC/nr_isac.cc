/* SPDX-License-Identifier: OAI-Public-License-1.1 */
/** Public OAI glue for the native transcription of the validated Python sensing pipeline. */
#include "nr_isac.h"

#include "sensing_engine.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

extern "C" {
#include "common/config/config_userapi.h"
#include "common/utils/LOG/log.h"
}


namespace {
using namespace nr_isac;

// adaptive_RX_pipeline.md P13: one independent SensingEngine per ACTIVE receive branch, indexed
// by branch_id. A fixed std::array sized to NR_RX_BRANCH_MAX, NOT a vector packed to the active
// count: nr_rx_branch_set_t::b[] is itself branch_id-indexed, so with this shape the index IS the
// identity, "is this branch active" is a null test on the slot, and there is no second mapping
// table that can fall out of step with the branch set. NR_RX_BRANCH_MAX is 4, so the unused slots
// cost one null pointer each.
//
// `pipeline` stays the BASE parsed configuration. The process-wide queries below (sources_mask,
// sub-slot policy, AoA antenna count) are branch-independent by construction -- they come from one
// [sensing] section -- while every engine owns its own per-branch copy of the configuration, made
// by branch_pipeline_config(). No second PipelineConfig array is kept: SensingEngine takes its
// config BY VALUE, so a retained copy would be a second source of truth serving nobody.
std::array<std::unique_ptr<SensingEngine>, NR_RX_BRANCH_MAX> engines;
uint32_t engines_built = 0;
PipelineConfig pipeline;
std::atomic<bool> enabled{false}, started{false};
uint32_t aoa_antennas = 0;
// P13: misrouting a CFR row into another branch's coherent window is SILENT corruption, so every
// refusal is counted and the first one per branch id is logged loudly.
std::atomic<uint64_t> dropped_unrouted{0};
std::atomic<uint32_t> logged_unrouted{0};
std::atomic<uint64_t> untagged_submissions{0};
std::atomic<bool> logged_untagged_fanin{false};
// P10b: a branch whose physical channel a producer cannot reach contributes NOTHING rather than
// being folded into another branch. Same drop-and-say-so rule as the routing refusal above.
std::atomic<uint64_t> plan_skipped_branches{0};
std::atomic<bool> logged_plan_skip{false};

// adaptive_RX_pipeline.md P03: [sensing] rx_branches / rx_branch_phys_map, foundation only (not
// yet consulted by the RT read loop -- see nr_isac.h's nr_isac_rx_branches() comment).
nr_rx_branch_set_t branches{};
bool branches_valid = false;
int expected_nb_antennas_rx = 0; // 0 = unknown; set via nr_isac_set_nb_antennas_rx() before init

paramdef_t integer(const char* name, const char* help, unsigned flags, int* value, int fallback)
{
  paramdef_t p{}; std::strncpy(p.optname, name, sizeof(p.optname) - 1);
  p.helpstr = help; p.paramflags = flags; p.iptr = value; p.defintval = fallback; p.type = TYPE_INT;
  return p;
}

paramdef_t real(const char* name, const char* help, double* value, double fallback)
{
  paramdef_t p{}; std::strncpy(p.optname, name, sizeof(p.optname) - 1);
  p.helpstr = help; p.dblptr = value; p.defdblval = fallback; p.type = TYPE_DOUBLE;
  return p;
}

paramdef_t text(const char* name, const char* help, char** value, const char* fallback)
{
  paramdef_t p{}; std::strncpy(p.optname, name, sizeof(p.optname) - 1);
  p.helpstr = help; p.strptr = value; p.defstrval = fallback; p.type = TYPE_STRING;
  return p;
}

uint32_t source_bit(const std::string& token)
{
  if (token == "csi_rs") return 1u << NR_ISAC_SRC_CSI_RS;
  if (token == "pdsch_dmrs") return 1u << NR_ISAC_SRC_PDSCH_DMRS;
  if (token == "pdsch_data") return 1u << NR_ISAC_SRC_PDSCH_DATA;
  if (token == "pdsch_dmrs_blind") return 1u << NR_ISAC_SRC_PDSCH_DMRS_BLIND;
  if (token == "pusch_dmrs") return 1u << NR_ISAC_SRC_PUSCH_DMRS;
  if (token == "pusch_data") return 1u << NR_ISAC_SRC_PUSCH_DATA;
  if (token == "ssb") return 1u << NR_ISAC_SRC_SSB;
  return 0;
}

uint32_t sources_mask(const char* list, const char* fallback)
{
  const std::string input = list && *list ? list : (fallback ? fallback : "csi_rs");
  uint32_t result = 0; std::stringstream stream(input); std::string token;
  while (std::getline(stream, token, ',')) {
    const size_t first = token.find_first_not_of(" \t"), last = token.find_last_not_of(" \t");
    if (first == std::string::npos) continue;
    token = token.substr(first, last - first + 1);
    const uint32_t bit = source_bit(token);
    if (!bit) LOG_W(PHY, "SENSING: unknown source '%s' ignored\n", token.c_str());
    result |= bit;
  }
  return result ? result : (1u << NR_ISAC_SRC_CSI_RS);
}

std::vector<double> duration_bank(const char* spec)
{
  std::vector<double> result; std::stringstream stream(spec ? spec : "8,16,24,32"); std::string token;
  while (std::getline(stream, token, ',')) {
    char* end = nullptr; const double milliseconds = std::strtod(token.c_str(), &end);
    if (end != token.c_str() && milliseconds > 0.0) result.push_back(milliseconds * 1e-3);
  }
  return result.empty() ? std::vector<double>{0.008, 0.016, 0.024, 0.032} : result;
}

bool parse_vec3(const char* spec, Vec3* out)
{
  if (!spec || !out) return false;
  double x=0.0,y=0.0,z=0.0;
  const int count = std::sscanf(spec, "%lf,%lf,%lf", &x, &y, &z);
  if (count < 2) return false;
  *out = {x,y,count >= 3 ? z : 0.0}; return true;
}

bool parse_array(const char* spec, double rotation_deg, const char* broadside_spec,
                 Vec3 tx, Vec3 rx, ArrayGeometry* out)
{
  if (!spec || !*spec || !out) return false;
  const double angle = rotation_deg * PI / 180.0, cs = std::cos(angle), sn = std::sin(angle);
  std::stringstream stream(spec); std::string token; std::vector<Vec3> positions;
  while (std::getline(stream, token, ';')) {
    double x=0.0,y=0.0,z=0.0; const int count = std::sscanf(token.c_str(), "%lf,%lf,%lf", &x,&y,&z);
    if (count < 2) return false;
    positions.push_back({x*cs-y*sn,x*sn+y*cs,count>=3?z:0.0});
  }
  if (positions.size() != 4) return false;
  Matrix gram(3,3);
  for(size_t a=1;a<4;++a){const Vec3 b=positions[a]-positions[0];for(size_t i=0;i<3;++i)for(size_t j=0;j<3;++j)gram(i,j)+=b[i]*b[j];}
  const SymmetricEigen eig = symmetric_eigen(gram); const double largest=std::max(0.0,eig.values[2]);
  const double tol=9.0*std::numeric_limits<double>::epsilon()*largest;
  if (!(eig.values[1] > tol) || eig.values[0] > tol) return false;
  Vec3 broadside;
  if (!parse_vec3(broadside_spec, &broadside)) {
    broadside = normalized({eig.vectors(0,0),eig.vectors(1,0),eig.vectors(2,0)});
    if (std::abs(broadside.z) >= std::max(std::abs(broadside.x),std::abs(broadside.y))) {
      if (broadside.z < 0.0) broadside=broadside*-1.0;
    } else if (dot(broadside,tx-rx)<0.0) broadside=broadside*-1.0;
  } else broadside=normalized(broadside);
  for(size_t i=0;i<4;++i)out->positions[i]=positions[i];
  out->broadside=broadside;out->configured=true;return true;
}

bool parse_array_calibration(const char* spec, ArrayCalibration* out)
{
  if (!out) return false;
  if (!spec || !*spec) { *out = ArrayCalibration{}; return true; }
  std::stringstream stream(spec); std::string token; size_t physical = 0;
  ArrayCalibration parsed;
  std::array<uint8_t, 4> seen{};
  while (std::getline(stream, token, ';')) {
    if (physical >= 4) return false;
    unsigned observed = 0; double gain = 0.0, phase = 0.0, delay_ns = 0.0; int consumed = 0;
    if (std::sscanf(token.c_str(), " %u , %lf , %lf , %lf %n",
                    &observed, &gain, &phase, &delay_ns, &consumed) != 4
        || token.find_first_not_of(" \t", static_cast<size_t>(consumed)) != std::string::npos
        || observed >= 4 || seen[observed]++
        || !(std::isfinite(gain) && gain > 0.0)
        || !std::isfinite(phase) || !std::isfinite(delay_ns))
      return false;
    parsed.physical_to_observed[physical] = observed;
    parsed.gain[physical] = gain;
    parsed.phase_rad[physical] = phase;
    parsed.delay_s[physical] = delay_ns * 1e-9;
    ++physical;
  }
  if (physical != 4) return false;
  parsed.configured = true;
  *out = parsed;
  return true;
}


} // namespace

extern "C" void nr_isac_init(void)
{
  if (enabled.load(std::memory_order_relaxed)) return;
  int p_enable=0,p_num_ues=1,p_sync=1,p_family_static=1,p_track=1,p_hierarchical=1;
  int p_aoa=0,p_aoa_ul=0,p_capture=0,p_min_rows=16,p_max_rows=512,p_bootstrap=3,p_pending_mib=2048;
  int p_max_components=8,p_max_objects=8,p_train_r=12,p_train_d=12,p_guard_r=2,p_guard_d=2;
  int p_subslot_symbols=0,p_subslot_min_re=600,p_admission_start=0,p_admission_slots=0;
  double p_min_dwell=.006,p_max_dwell=.032,p_k_sigma=3.0,p_migration=.5,p_phase=PI/4.0;
  double p_max_speed=50.0,p_max_range=312.283810417,p_path_delay=312.283810417;
  double p_path_doppler=0.0,p_significance=-10.0,p_false_intensity=0.01/0.0305;
  double p_rx_x=0,p_rx_y=0,p_rx_z=0,p_tx_x=0,p_tx_y=0,p_tx_z=0,p_rotation=0,p_subslot_snr=0;
  double p_aoa_max_manifold=.25,p_aoa_max_phase=PI/4.0,p_aoa_max_az_sigma=45.0;
  double p_aoa_max_el_sigma=45.0;
  char *p_source=nullptr,*p_sources=nullptr,*p_durations=nullptr,*p_array=nullptr,*p_broadside=nullptr;
  char *p_array_calibration=nullptr;
  char *p_rx_branches=nullptr,*p_rx_branch_phys_map=nullptr;
  char *p_out=nullptr,*p_rx_id=nullptr,*p_illum=nullptr,*p_report=nullptr,*p_endpoint=nullptr;
  paramdef_t params[] = {
    integer("enable","enable native passive sensing",PARAMFLAG_BOOL,&p_enable,0),
    integer("num_ues","decoded UE count (supported 1..4)",0,&p_num_ues,1),
    text("source","legacy single CFR source",&p_source,"csi_rs"),
    text("sources","comma-separated DL and UL CFR sources processed independently",&p_sources,""),
    text("duration_bank_ms","ascending tracker CPI bank in milliseconds",&p_durations,"8,16,24,32"),
    integer("bootstrap_duration_index","known-good fallback bank index",0,&p_bootstrap,3),
    real("minimum_dwell_s","minimum legal coherent dwell",&p_min_dwell,.006),
    real("maximum_dwell_s","maximum legal coherent dwell",&p_max_dwell,.032),
    integer("minimum_rows","minimum measured rows",0,&p_min_rows,16),
    integer("maximum_rows","measured row buffer ceiling",0,&p_max_rows,512),
    integer("pending_row_budget_mib","unplanned-row backlog memory ceiling",0,&p_pending_mib,2048),
    real("k_sigma","planner uncertainty multiplier",&p_k_sigma,3.0),
    real("migration_eta_bins","permitted range migration",&p_migration,.5),
    real("phase_error_max_rad","permitted residual acceleration phase",&p_phase,PI/4.0),
    real("maximum_target_speed_mps","physical target speed bound",&p_max_speed,50.0),
    real("max_range_m","maximum detector excess range",&p_max_range,312.283810417),
    integer("clean_max_components","sequential CLEAN component count",0,&p_max_components,8),
    integer("max_detections","collapsed object count",0,&p_max_objects,8),
    real("maximum_path_delay_m","multipath grouping delay",&p_path_delay,312.283810417),
    real("maximum_path_doppler_hz","multipath grouping Doppler",&p_path_doppler,0.0),
    real("leading_significance_db","multipath leading-path significance",&p_significance,-10.0),
    integer("adaptive_training_range_bins","CFAR range training radius",0,&p_train_r,12),
    integer("adaptive_training_doppler_bins","CFAR Doppler training radius",0,&p_train_d,12),
    integer("adaptive_guard_range_bins","CFAR CUT range guard",0,&p_guard_r,2),
    integer("adaptive_guard_doppler_bins","CFAR CUT Doppler guard (never a notch)",0,&p_guard_d,2),
    real("false_object_intensity_per_s","declared false-object intensity",&p_false_intensity,0.01/0.0305),
    integer("sync_correction","adaptive STO/CPE/SFO",PARAMFLAG_BOOL,&p_sync,1),
    integer("family_static","scheduler-family static subtraction",PARAMFLAG_BOOL,&p_family_static,1),
    integer("track_enable","enable Stage-1 tracking",PARAMFLAG_BOOL,&p_track,1),
    integer("hierarchical_tracker_enable","enable global ENU Stage 2",PARAMFLAG_BOOL,&p_hierarchical,1),
    integer("aoa_enable","master surveyed-array AoA switch",PARAMFLAG_BOOL,&p_aoa,0),
    integer("aoa_ul_enable","permit PUSCH rows in AoA only",PARAMFLAG_BOOL,&p_aoa_ul,0),
    text("rx_array","exactly four x,y[,z] element offsets separated by semicolons",&p_array,""),
    real("rx_array_boresight_deg","rotate legacy x,y array coordinates in ENU",&p_rotation,0.0),
    text("rx_array_broadside_enu","explicit broadside x,y,z; empty derives from geometry",&p_broadside,""),
    text("rx_array_calibration",
         "four physical-to-observed correction tuples observed,gain,phase_rad,delay_ns",
         &p_array_calibration,""),
    real("aoa_max_manifold_residual","maximum unexplained single-manifold energy fraction",
         &p_aoa_max_manifold,.25),
    real("aoa_max_phase_fit_rms_rad","maximum array phase-fit residual RMS",
         &p_aoa_max_phase,PI/4.0),
    real("aoa_max_azimuth_stddev_deg","maximum admitted azimuth standard uncertainty",
         &p_aoa_max_az_sigma,45.0),
    real("aoa_max_elevation_stddev_deg","maximum admitted elevation standard uncertainty",
         &p_aoa_max_el_sigma,45.0),
    real("rx_pos_x","receiver ENU east",&p_rx_x,0),real("rx_pos_y","receiver ENU north",&p_rx_y,0),real("rx_pos_z","receiver ENU up",&p_rx_z,0),
    real("tx_pos_x","transmitter ENU east",&p_tx_x,0),real("tx_pos_y","transmitter ENU north",&p_tx_y,0),real("tx_pos_z","transmitter ENU up",&p_tx_z,0),
    integer("capture","include native range-Doppler raster in reports",PARAMFLAG_BOOL,&p_capture,0),
    text("out_path","output prefix",&p_out,"/tmp/oaiue_sensing"),text("rx_id","receiver id",&p_rx_id,"rx1"),
    text("illuminator_id","illuminator id",&p_illum,"gnb1"),text("report_path","JSONL report path",&p_report,""),
    text("report_endpoint","ZeroMQ PUB bind endpoint",&p_endpoint,""),
    integer("subslot_symbols","sub-slot CFR sampling groups",0,&p_subslot_symbols,0),
    integer("subslot_min_re","minimum REs per sub-slot row",0,&p_subslot_min_re,600),
    real("subslot_min_snr_db","minimum sub-slot SNR",&p_subslot_snr,0.0),
    integer("admission_start_slot","absolute RF slot at which sensing FIFO admission opens",0,&p_admission_start,0),
    integer("admission_num_slots","finite RF-slot admission span; zero disables the gate",0,&p_admission_slots,0),
    text("rx_branches","active receiver branch ids, e.g. \"0,1,2,3\"",&p_rx_branches,"0"),
    text("rx_branch_phys_map","branch:physical channel map, e.g. \"0:0,1:1,2:2,3:3\"",
         &p_rx_branch_phys_map,"0:0"),
  };
  config_get(config_get_if(),params,sizeof(params)/sizeof(params[0]),"sensing");
  // P14 Stage A: the AoA environment override is gone; a stale launcher must be told, not ignored.
  if (nr_isac_obsolete_env_keys() > 0)
    LOG_E(PHY,"SENSING: AOA_ENABLE/AOA_UL_ENABLE are obsolete environment keys and are IGNORED; "
              "use the [sensing] aoa_enable / aoa_ul_enable configuration keys instead\n");
  if (!p_enable) return;
  branches_valid=false;
  if (nr_rx_branch_set_parse(&branches,p_rx_branches,p_rx_branch_phys_map,p_rx_id?p_rx_id:"rx")!=0) {
    LOG_E(PHY,"SENSING: rx_branches/rx_branch_phys_map invalid; sensing disabled\n");
    return;
  }
  // P06a: FATAL, not "sensing disabled". Every branch names a receive antenna it will decode
  // from; naming more branches than antennas cannot be honoured, and continuing with sensing
  // silently off produces a capture that looks merely empty instead of misconfigured.
  AssertFatal(!(expected_nb_antennas_rx>0
                && nr_rx_branch_set_check_antennas(&branches,expected_nb_antennas_rx)!=0),
              "SENSING: rx_branches names %d branches but only %d receive antennas are configured "
              "(--ue-nb-ant-rx)\n",(int)branches.n_active,expected_nb_antennas_rx);
  branches_valid=true;
  pipeline = PipelineConfig{};
  pipeline.num_ues=p_num_ues>0?static_cast<uint32_t>(p_num_ues):0;pipeline.sources_mask=sources_mask(p_sources,p_source);
  pipeline.duration_bank_s=duration_bank(p_durations);pipeline.bootstrap_duration_index=std::max(0,p_bootstrap);
  pipeline.minimum_dwell_s=p_min_dwell;pipeline.maximum_dwell_s=p_max_dwell;
  pipeline.minimum_rows=std::max(2,p_min_rows);pipeline.maximum_rows=std::max(pipeline.minimum_rows,(uint32_t)std::max(2,p_max_rows));
  pipeline.pending_row_budget_bytes=(uint64_t)std::max(64,p_pending_mib)*1024ULL*1024ULL;
  pipeline.k_sigma=p_k_sigma;pipeline.migration_eta_bins=p_migration;pipeline.phase_error_max_rad=p_phase;
  pipeline.maximum_target_speed_mps=p_max_speed;pipeline.maximum_range_m=p_max_range;
  pipeline.maximum_components=p_max_components>0?static_cast<uint32_t>(p_max_components):0;
  pipeline.maximum_objects=p_max_objects>0?static_cast<uint32_t>(p_max_objects):0;
  pipeline.maximum_path_delay_m=p_path_delay;pipeline.maximum_path_doppler_hz=p_path_doppler;
  pipeline.leading_significance_db=p_significance;pipeline.adaptive_training_range_bins=std::max(1,p_train_r);
  pipeline.adaptive_training_doppler_bins=std::max(1,p_train_d);pipeline.adaptive_guard_range_bins=std::max(0,p_guard_r);
  pipeline.adaptive_guard_doppler_bins=std::max(0,p_guard_d);pipeline.false_object_intensity_per_s=p_false_intensity;
  pipeline.sync_enable=p_sync!=0;pipeline.family_static=p_family_static!=0;pipeline.tracker_enable=p_track!=0;
  pipeline.hierarchical_tracker_enable=p_hierarchical!=0;pipeline.capture_rvm=p_capture!=0;
  pipeline.rx_position={p_rx_x,p_rx_y,p_rx_z};pipeline.tx_position={p_tx_x,p_tx_y,p_tx_z};
  pipeline.aoa_enable=p_aoa!=0;pipeline.aoa_ul_enable_requested=p_aoa_ul!=0;
  pipeline.aoa_ul_enable=pipeline.aoa_enable&&pipeline.aoa_ul_enable_requested;
  pipeline.aoa_quality.maximum_relative_manifold_residual_energy=p_aoa_max_manifold;
  pipeline.aoa_quality.maximum_phase_fit_residual_rms_rad=p_aoa_max_phase;
  pipeline.aoa_quality.maximum_azimuth_stddev_deg=p_aoa_max_az_sigma;
  pipeline.aoa_quality.maximum_elevation_stddev_deg=p_aoa_max_el_sigma;
  if (pipeline.aoa_enable) {
    if (!parse_array(p_array,p_rotation,p_broadside,pipeline.tx_position,pipeline.rx_position,&pipeline.array)) {
      LOG_E(PHY,"SENSING: aoa_enable requires a valid surveyed rank-two four-element array; sensing disabled\n");
      pipeline.aoa_enable=pipeline.aoa_ul_enable=false;return;
    }
    if (!parse_array_calibration(p_array_calibration,&pipeline.array_calibration)) {
      LOG_E(PHY,"SENSING: rx_array_calibration must contain four unique observed,gain,phase_rad,delay_ns tuples; sensing disabled\n");
      pipeline.aoa_enable=pipeline.aoa_ul_enable=false;return;
    }
    aoa_antennas=4;
  }
  pipeline.out_path=p_out?p_out:"/tmp/oaiue_sensing";pipeline.rx_id=p_rx_id?p_rx_id:"rx1";
  pipeline.illuminator_id=p_illum?p_illum:"gnb1";pipeline.report_path=p_report?p_report:"";
  pipeline.report_endpoint=p_endpoint?p_endpoint:"";pipeline.subslot_symbols=std::max(0,p_subslot_symbols);
  pipeline.subslot_min_re=std::max(0,p_subslot_min_re);pipeline.subslot_min_snr_db=p_subslot_snr;
  if (p_admission_start < 0 || p_admission_slots < 0) {
    LOG_E(PHY,"SENSING: admission_start_slot and admission_num_slots must be non-negative\\n"); return;
  }
  if (p_admission_slots > 0) {
    const uint64_t start = static_cast<uint64_t>(p_admission_start);
    const uint64_t slots = static_cast<uint64_t>(p_admission_slots);
    if (start > std::numeric_limits<uint64_t>::max() - slots) {
      LOG_E(PHY,"SENSING: admission slot range overflows\\n"); return;
    }
    pipeline.admission_window_enabled=true;pipeline.admission_start_slot=start;
    pipeline.admission_end_slot=start+slots;
  }
  // P13: one engine per active branch, each with its OWN PipelineConfig copy so N engines cannot
  // interleave into one report file or fight over one ZeroMQ bind. With a single active branch
  // branch_pipeline_config() returns the base configuration untouched -- that is what keeps a
  // legacy receiver's rx_id/out_path/report_path exactly what they are today.
  for (auto& slot : engines) slot.reset();
  engines_built = 0;
  dropped_unrouted.store(0, std::memory_order_relaxed);
  plan_skipped_branches.store(0, std::memory_order_relaxed);
  logged_plan_skip.store(false, std::memory_order_relaxed);
  logged_unrouted.store(0, std::memory_order_relaxed);
  untagged_submissions.store(0, std::memory_order_relaxed);
  logged_untagged_fanin.store(false, std::memory_order_relaxed);
  // P10b: the configuration surface does NOT reject aoa_enable together with several receive
  // branches -- aoa_enable/rx_array and rx_branches are parsed independently above -- yet the two
  // are mutually exclusive deployment models: AoA needs one CO-LOCATED four-element array on ONE
  // branch, multi-branch needs physically separated single-antenna receivers. The combination is
  // now REFUSED, but by SensingEngine's own pre-existing "AoA requires four channels" guard a few
  // lines below, which on its own gives the operator no idea why they suddenly have one channel.
  // This is that explanation, and it must be printed BEFORE the loop that fails -- a message after
  // the loop would be unreachable in exactly the configuration it describes. Making it a config
  // parse-time rejection instead is an operator call (it changes which configurations start) and
  // is deliberately left open; see docs/aoa_removal_audit.md Stage B item 2.
  if (pipeline.aoa_enable && branch_active_count(branches) > 1)
    LOG_E(PHY,"SENSING: aoa_enable is set but %d receive branches are active. AoA needs a co-located "
              "four-element array on ONE branch; a multi-branch receiver measures one antenna per "
              "branch, so each engine is built with ONE channel and the AoA guard below will refuse "
              "this configuration. Use one branch for AoA, or drop aoa_enable\n",
          branch_active_count(branches));
  for (uint8_t b = 0; b < NR_RX_BRANCH_MAX; ++b) {
    // branch_is_active(), the SAME predicate branch_pipeline_config() counts and
    // branch_engine_index() routes by. Using a different one here is precisely how a set could
    // build N engines while deriving one unsuffixed output identity for all of them.
    if (!branch_is_active(branches, b)) continue;
    try {
      // P10b (AoA-removal audit Stage B item 1, the audit's stale `nr_isac.cc:317` citation): the
      // channel count comes from the BRANCH SET once there is more than one branch. A physically
      // separated branch is one antenna by construction (P03's branch:physical map is 1:1), so
      // asking for four here would reserve four planes per row in every engine -- 4x the CFR
      // working set, in a pipeline with a documented allocation-failure history -- to hold three
      // planes that no producer can fill. With ONE active branch this is exactly the previous
      // expression, which is what keeps the co-located-array AoA path untouched.
      const uint32_t branch_antennas =
          branch_active_count(branches) > 1 ? 1u : (aoa_antennas ? aoa_antennas : 1u);
      engines[b] = std::make_unique<SensingEngine>(branch_pipeline_config(pipeline, branches, b),
                                                   275, branch_antennas);
    } catch (const std::exception& e) {
      // A partially built array is not a usable receiver: one branch silently missing would look
      // like a coverage result rather than a configuration error.
      LOG_E(PHY, "SENSING: invalid native configuration for branch %u: %s\n", (unsigned)b, e.what());
      for (auto& slot : engines) slot.reset();
      engines_built = 0;
      return;
    }
    ++engines_built;
  }
  if (!engines_built) {
    LOG_E(PHY, "SENSING: rx_branches names no active receive branch; sensing disabled\n");
    return;
  }
  enabled.store(true,std::memory_order_release);
  LOG_I(PHY,"SENSING: native Python-parity pipeline enabled, num_ues=%u sources=0x%x separate_DL_UL=1 AoA=%d UL-AoA=%d array_calibration=%d engines=%u\n",
        pipeline.num_ues,pipeline.sources_mask,(int)pipeline.aoa_enable,(int)pipeline.aoa_ul_enable,
        pipeline.array_calibration.configured,engines_built);
}

extern "C" void nr_isac_start(void)
{
  bool expected = false;
  if (!enabled.load() || !engines_built || !started.compare_exchange_strong(expected, true)) return;
  try {
    // P13: every branch's engine gets its own accumulation and processing threads here. They share
    // nothing; one branch with no UL data, or no data at all, simply never wakes its own workers
    // and cannot reach another branch's.
    for (auto& slot : engines) if (slot) slot->start();
  } catch (const std::exception& error) {
    // A partial start is not a usable state. stop() on an engine that never started is a no-op by
    // contract (running_.exchange(false) is already false), so this is safe on the whole array.
    for (auto& slot : engines) if (slot) slot->stop();
    started.store(false);
    enabled.store(false);
    LOG_E(PHY, "SENSING: startup failed before CFR admission: %s\n", error.what());
  }
}
extern "C" void nr_isac_stop(void)
{
  if (!started.exchange(false)) return;
  for (auto& slot : engines) if (slot) slot->stop();
  const unsigned long long unrouted = dropped_unrouted.load(std::memory_order_relaxed);
  const unsigned long long untagged = untagged_submissions.load(std::memory_order_relaxed);
  const unsigned long long plan_skips = plan_skipped_branches.load(std::memory_order_relaxed);
  if (unrouted || plan_skips || (untagged && engines_built > 1))
    LOG_I(PHY, "SENSING: branch routing census: engines=%u dropped_unrouted=%llu untagged=%llu "
               "plan_skipped_branches=%llu\n",
          engines_built, unrouted, untagged, plan_skips);
}
extern "C" int nr_isac_enabled(void){return enabled.load(std::memory_order_relaxed);}
extern "C" int nr_isac_source(void){for(int i=0;i<NR_ISAC_SRC_COUNT;++i)if(pipeline.sources_mask&(1u<<i))return i;return NR_ISAC_SRC_CSI_RS;}
extern "C" int nr_isac_source_enabled(int source){return enabled.load()&&source>=0&&source<NR_ISAC_SRC_COUNT&&(pipeline.sources_mask&(1u<<source));}
extern "C" uint32_t nr_isac_aoa_antennas(void){return enabled.load()&&pipeline.aoa_enable?aoa_antennas:0;}
extern "C" int nr_isac_submit_plan(nr_isac_submit_plan_t* out,int max,uint32_t legacy_nof_ant,
                                    uint32_t available_antennas,uint32_t* pack_antennas)
{
  if(!enabled.load(std::memory_order_relaxed)||!engines_built){
    if(pack_antennas)*pack_antennas=0;
    return 0;
  }
  const nr_rx_branch_set_t* set=branches_valid?&branches:nullptr;
  const int written=build_submit_plan(set,out,max,legacy_nof_ant,available_antennas,pack_antennas);
  // A branch that produced no entry measured nothing this slot. Silence here would look like
  // coverage; it is a configuration error (a physical channel the producer cannot reach).
  if(written>0&&set){
    const int active=branch_active_count(*set);
    if(active>1&&written<active){
      plan_skipped_branches.fetch_add((unsigned)(active-written),std::memory_order_relaxed);
      if(!logged_plan_skip.exchange(true,std::memory_order_relaxed))
        LOG_E(PHY,"SENSING: %d of %d active branches name a physical channel this CFR producer "
                  "cannot reach (available=%u); their rows are DROPPED, never folded into another "
                  "branch\n",active-written,active,available_antennas);
    }
  }
  return written;
}

extern "C" uint32_t nr_isac_subslot_config(uint32_t* min_re,float* min_snr){if(!enabled.load())return 0;if(min_re)*min_re=pipeline.subslot_min_re;if(min_snr)*min_snr=pipeline.subslot_min_snr_db;return pipeline.subslot_symbols;}
extern "C" const nr_rx_branch_set_t* nr_isac_rx_branches(void){return (enabled.load()&&branches_valid)?&branches:nullptr;}
extern "C" nr_rx_branch_set_t* nr_isac_rx_branches_mutable(void){return (enabled.load()&&branches_valid)?&branches:nullptr;}
extern "C" void nr_isac_set_nb_antennas_rx(int nb_antennas_rx){expected_nb_antennas_rx=nb_antennas_rx;}

extern "C" void nr_isac_submit_cfr(uint32_t slot,int source,const nr_isac_carrier_t* carrier,const float* h,
                                    const uint32_t* k,const uint32_t* l,uint32_t n,float noise)
{nr_isac_submit_cfr_at(slot,0.0f,source,carrier,h,k,l,n,noise);}
extern "C" void nr_isac_submit_cfr_at(uint32_t slot,float fraction,int source,const nr_isac_carrier_t* carrier,const float* h,
                                       const uint32_t* k,const uint32_t* l,uint32_t n,float noise)
{nr_isac_submit_cfr_multi(slot,fraction,source,carrier,h,1,n,k,l,n,noise);}
/* P10a: every pre-P10 producer routes through here, and this one line is the whole reason none of
 * them changes behaviour -- same body, branch identity explicitly absent. */
extern "C" void nr_isac_submit_cfr_multi(uint32_t slot,float fraction,int source,const nr_isac_carrier_t* carrier,const float* h,
                                          uint32_t antennas,uint32_t stride,const uint32_t* k,const uint32_t* l,uint32_t n,float noise)
{nr_isac_submit_cfr_multi_branch(slot,fraction,source,carrier,h,antennas,stride,k,l,n,noise,NR_ISAC_BRANCH_NONE);}
extern "C" void nr_isac_submit_cfr_multi_branch(uint32_t slot,float fraction,int source,const nr_isac_carrier_t* carrier,const float* h,
                                                 uint32_t antennas,uint32_t stride,const uint32_t* k,const uint32_t* l,uint32_t n,float noise,
                                                 uint8_t branch)
{
  if(!enabled.load(std::memory_order_relaxed)||!engines_built||!carrier||!h||!k||!l||!n)return;
  if(source<0||source>=NR_ISAC_SRC_COUNT)source=nr_isac_source();
  if(!nr_isac_source_enabled(source))return;
  // P13 routing. Decided BEFORE the CFR is packed, so a refused row costs nothing, and gated on the
  // ENGINE SLOT as well as on the branch set, so the two can never disagree about what exists.
  const int index=branch_engine_index(branches,branch);
  if(index<0||!engines[index]){
    // Never fall back to branch 0: folding a branch's rows into another branch's coherent window is
    // exactly the cross-contamination this plan exists to prevent, and it would be invisible
    // downstream. Drop, count, and say so once per offending branch id.
    dropped_unrouted.fetch_add(1,std::memory_order_relaxed);
    const uint32_t bit=branch<32u?(1u<<branch):0x80000000u;
    if(!(logged_unrouted.fetch_or(bit,std::memory_order_relaxed)&bit))
      LOG_E(PHY,"SENSING: CFR tagged receive branch %u has no engine (not named in rx_branches); "
                "those rows are DROPPED, never folded into another branch\n",(unsigned)branch);
    return;
  }
  if(branch==NR_ISAC_BRANCH_NONE){
    untagged_submissions.fetch_add(1,std::memory_order_relaxed);
    if(engines_built>1&&!logged_untagged_fanin.exchange(true,std::memory_order_relaxed))
      LOG_W(PHY,"SENSING: %u branches active but a CFR producer still submits untagged rows; they "
                "are ATTRIBUTED to branch %d, not measured there. Resolved when P10's remaining "
                "producers carry branch identity\n",engines_built,index);
  }
  antennas=std::max(1u,antennas);stride=std::max(stride,n);static thread_local std::vector<std::complex<float>> packed;
  const size_t total=(size_t)antennas*n;if(packed.size()<total)packed.resize(total);
  for(uint32_t a=0;a<antennas;++a){const float* input=h+(size_t)2*a*stride;for(uint32_t i=0;i<n;++i)packed[(size_t)a*n+i]={input[2*i],input[2*i+1]};}
  engines[index]->submit(slot,fraction,static_cast<nr_isac_source_t>(source),*carrier,packed.data(),antennas,k,l,n,noise,branch);
}
