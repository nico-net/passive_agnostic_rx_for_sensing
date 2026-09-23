/* SPDX-License-Identifier: OAI-Public-License-1.1 */
/** Public OAI glue for the native transcription of the validated Python sensing pipeline. */
#include "nr_isac.h"

#include "flow_gate.h"
#include "sensing_engine.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <time.h>
#include <vector>

extern "C" {
#include "common/config/config_userapi.h"
#include "common/utils/LOG/log.h"
}

int AOA_ENABLE = 0;
int AOA_UL_ENABLE = 0;

namespace {
using namespace nr_isac;

std::unique_ptr<SensingEngine> engine;
PipelineConfig pipeline;
std::atomic<bool> enabled{false}, started{false};
uint32_t aoa_antennas = 0;

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

bool parse_spatial_receivers(const char* spec, SpatialReceiverGeometry* out)
{
  if (!out) return false;
  if (!spec || !*spec) { *out = SpatialReceiverGeometry{}; return true; }
  std::stringstream stream(spec); std::string token; std::vector<Vec3> positions;
  while (std::getline(stream, token, ';')) {
    double x=0.0,y=0.0,z=0.0; int consumed=0;
    if (std::sscanf(token.c_str(), " %lf , %lf , %lf %n", &x,&y,&z,&consumed) != 3
        || token.find_first_not_of(" \t", static_cast<size_t>(consumed)) != std::string::npos
        || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
      return false;
    positions.push_back({x,y,z});
  }
  if (positions.size() != 4) return false;
  for (size_t i=0;i<4;++i)
    for (size_t j=i+1;j<4;++j)
      if (!(norm(positions[i]-positions[j]) > 0.0)) return false;
  for (size_t i=0;i<4;++i) out->positions[i]=positions[i];
  out->configured=true;return true;
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

int environment_bool(const char* name, int fallback)
{
  const char* value = std::getenv(name); return value ? (std::atoi(value) != 0) : fallback;
}

} // namespace

namespace {
double monotonic_s(){timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);return ts.tv_sec+1e-9*ts.tv_nsec;}
nr_isac::FlowGate flow_gate(2.0);
std::atomic<uint64_t> gate_admitted{0},gate_rejected{0};
std::atomic<bool> gate_watchdog_run{false};
std::thread gate_watchdog;
}

extern "C" void nr_isac_flow_note(uint16_t rnti,int uplink)
{
  if(!enabled.load(std::memory_order_relaxed))return;
  if(flow_gate.note(rnti,uplink!=0,monotonic_s())>0)LOG_I(PHY,"SENSING_GATE open rnti=0x%04x\n",rnti);
}
extern "C" int nr_isac_flow_admit(uint16_t rnti)
{
  if(!enabled.load(std::memory_order_relaxed))return 0;
  const bool ok=flow_gate.admit(rnti,monotonic_s());
  (ok?gate_admitted:gate_rejected).fetch_add(1,std::memory_order_relaxed);
  return ok?1:0;
}
extern "C" void nr_isac_request_discard(void){if(engine)engine->request_discard_pending();}
extern "C" int nr_isac_drained(void){return engine&&engine->submissions_drained()?1:0;}

extern "C" void nr_isac_init(void)
{
  if (enabled.load(std::memory_order_relaxed)) return;
  int p_enable=0,p_num_ues=1,p_sync=1,p_family_static=1,p_track=1,p_hierarchical=1;
  int p_evidence_mode=0,p_lifecycle_features=0;
  double p_evidence_window=.150,p_existence_scale=1.0;
  int p_aoa=0,p_aoa_ul=0,p_capture=0,p_min_rows=16,p_max_rows=512,p_bootstrap=3,p_pending_mib=2048;
  int p_subslot_symbols=0,p_subslot_min_re=600,p_admission_start=0,p_admission_slots=0;
  double p_min_dwell=.006,p_max_dwell=.032,p_k_sigma=3.0,p_migration=.5,p_phase=PI/4.0;
  // These are application requirements, not detector defaults.  A deployment must declare them
  // explicitly; silently inheriting the historical campaign envelope is hidden hard-tuning.
  double p_max_speed=0.0,p_max_range=0.0,p_false_intensity=0.0;
  double p_rx_x=0,p_rx_y=0,p_rx_z=0,p_tx_x=0,p_tx_y=0,p_tx_z=0,p_rotation=0,p_subslot_snr=0;
  double p_aoa_max_manifold=.25,p_aoa_max_phase=PI/4.0,p_aoa_max_az_sigma=45.0;
  double p_aoa_max_el_sigma=45.0;
  char *p_source=nullptr,*p_sources=nullptr,*p_durations=nullptr,*p_array=nullptr,*p_broadside=nullptr;
  char *p_array_calibration=nullptr,*p_spatial_receivers=nullptr;
  char *p_out=nullptr,*p_rx_id=nullptr,*p_illum=nullptr,*p_report=nullptr,*p_endpoint=nullptr;
  paramdef_t params[] = {
    integer("enable","enable native passive sensing",PARAMFLAG_BOOL,&p_enable,0),
    integer("num_ues","decoded UE count (supported 1..4)",0,&p_num_ues,1),
    integer("evidence_mode","experimental ablation: 0 baseline, 1..6 B..G",0,&p_evidence_mode,0),
    integer("lifecycle_features","experimental bitmask: grouped birth=1, object/path=2, retirement=4",0,&p_lifecycle_features,0),
    real("evidence_window_s","causal evidence window, global 0.1..0.2 seconds",&p_evidence_window,.150),
    real("existence_threshold_scale","development-only existence operating point",&p_existence_scale,1.0),
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
    real("maximum_target_speed_mps","required physical target speed bound",&p_max_speed,0.0),
    real("max_range_m","required maximum detector excess range",&p_max_range,0.0),
    real("false_object_intensity_per_s","required declared false-object intensity",&p_false_intensity,0.0),
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
    text("spatial_rx_positions",
         "four X410 receiver ENU positions x,y,z separated by semicolons; disables AoA/coherent combining",
         &p_spatial_receivers,""),
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
  };
  config_get(config_get_if(),params,sizeof(params)/sizeof(params[0]),"sensing");
  if (!p_enable) return;
  if (!(std::isfinite(p_max_speed) && p_max_speed > 0.0
        && std::isfinite(p_max_range) && p_max_range > 0.0
        && std::isfinite(p_false_intensity) && p_false_intensity > 0.0)) {
    LOG_E(PHY,"SENSING: maximum_target_speed_mps, max_range_m, and false_object_intensity_per_s must be explicitly positive; sensing disabled\n");
    return;
  }
  pipeline = PipelineConfig{};
  pipeline.evidence_mode=static_cast<uint32_t>(p_evidence_mode);
  pipeline.lifecycle_features=static_cast<uint32_t>(p_lifecycle_features);
  pipeline.evidence_window_s=p_evidence_window;
  pipeline.existence_threshold_scale=p_existence_scale;
  pipeline.num_ues=p_num_ues>0?static_cast<uint32_t>(p_num_ues):0;pipeline.sources_mask=sources_mask(p_sources,p_source);
  pipeline.duration_bank_s=duration_bank(p_durations);pipeline.bootstrap_duration_index=std::max(0,p_bootstrap);
  pipeline.minimum_dwell_s=p_min_dwell;pipeline.maximum_dwell_s=p_max_dwell;
  pipeline.minimum_rows=std::max(2,p_min_rows);pipeline.maximum_rows=std::max(pipeline.minimum_rows,(uint32_t)std::max(2,p_max_rows));
  pipeline.pending_row_budget_bytes=(uint64_t)std::max(64,p_pending_mib)*1024ULL*1024ULL;
  pipeline.k_sigma=p_k_sigma;pipeline.migration_eta_bins=p_migration;pipeline.phase_error_max_rad=p_phase;
  pipeline.maximum_target_speed_mps=p_max_speed;pipeline.maximum_range_m=p_max_range;
  pipeline.false_object_intensity_per_s=p_false_intensity;
  pipeline.sync_enable=p_sync!=0;pipeline.family_static=p_family_static!=0;pipeline.tracker_enable=p_track!=0;
  pipeline.hierarchical_tracker_enable=p_hierarchical!=0;pipeline.capture_rvm=p_capture!=0;
  pipeline.rx_position={p_rx_x,p_rx_y,p_rx_z};pipeline.tx_position={p_tx_x,p_tx_y,p_tx_z};
  AOA_ENABLE=environment_bool("AOA_ENABLE",p_aoa);const int requested=environment_bool("AOA_UL_ENABLE",p_aoa_ul);
  AOA_UL_ENABLE=AOA_ENABLE&&requested;pipeline.aoa_enable=AOA_ENABLE;pipeline.aoa_ul_enable_requested=requested;
  pipeline.aoa_ul_enable=AOA_UL_ENABLE;
  if (!parse_spatial_receivers(p_spatial_receivers,&pipeline.spatial_receivers)) {
    LOG_E(PHY,"SENSING: spatial_rx_positions must contain four distinct finite x,y,z tuples; sensing disabled\n");
    AOA_ENABLE=AOA_UL_ENABLE=0;return;
  }
  if (pipeline.spatial_receivers.configured && (AOA_ENABLE || (p_array_calibration && *p_array_calibration))) {
    LOG_E(PHY,"SENSING: spatial receiver mode forbids AoA and receive-chain calibration; sensing disabled\n");
    AOA_ENABLE=AOA_UL_ENABLE=0;return;
  }
  if (pipeline.spatial_receivers.configured) {
    pipeline.hierarchical_tracker_enable=false;
    pipeline.duration_bank_s={SPATIAL_CPI_DURATION_S};pipeline.bootstrap_duration_index=0;
    pipeline.minimum_dwell_s=SPATIAL_CPI_DURATION_S;
    pipeline.maximum_dwell_s=SPATIAL_CPI_DURATION_S;
  }
  pipeline.aoa_quality.maximum_relative_manifold_residual_energy=p_aoa_max_manifold;
  pipeline.aoa_quality.maximum_phase_fit_residual_rms_rad=p_aoa_max_phase;
  pipeline.aoa_quality.maximum_azimuth_stddev_deg=p_aoa_max_az_sigma;
  pipeline.aoa_quality.maximum_elevation_stddev_deg=p_aoa_max_el_sigma;
  if (pipeline.aoa_enable) {
    if (!parse_array(p_array,p_rotation,p_broadside,pipeline.tx_position,pipeline.rx_position,&pipeline.array)) {
      LOG_E(PHY,"SENSING: aoa_enable requires a valid surveyed rank-two four-element array; sensing disabled\n");
      AOA_ENABLE=AOA_UL_ENABLE=0;return;
    }
    if (!parse_array_calibration(p_array_calibration,&pipeline.array_calibration)) {
      LOG_E(PHY,"SENSING: rx_array_calibration must contain four unique observed,gain,phase_rad,delay_ns tuples; sensing disabled\n");
      AOA_ENABLE=AOA_UL_ENABLE=0;return;
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
  const uint32_t sensing_channels=pipeline.spatial_receivers.configured?4:(aoa_antennas?aoa_antennas:1);
  try { engine=std::make_unique<SensingEngine>(pipeline,275,sensing_channels); }
  catch(const std::exception& e){LOG_E(PHY,"SENSING: invalid native configuration: %s\n",e.what());return;}
  enabled.store(true,std::memory_order_release);
  LOG_I(PHY,"SENSING: current-CPI GLRT+CLEAN enabled, num_ues=%u sources=0x%x spatial_receivers=%d AoA=%d UL-AoA=%d array_calibration=%d\n",
        pipeline.num_ues,pipeline.sources_mask,pipeline.spatial_receivers.configured,
        AOA_ENABLE,AOA_UL_ENABLE,pipeline.array_calibration.configured);
}

// Task 10 will replace this in place with the real recorded-close writer.
static void nr_isac_record_gate_close(void) {}

extern "C" void nr_isac_start(void)
{
  bool expected = false;
  if (!enabled.load() || !engine || !started.compare_exchange_strong(expected, true)) return;
  try {
    engine->start();
  } catch (const std::exception& error) {
    started.store(false);
    enabled.store(false);
    LOG_E(PHY, "SENSING: startup failed before CFR admission: %s\n", error.what());
    return;
  }
  gate_watchdog_run.store(true);
  gate_watchdog = std::thread([] {
    double last_stats = monotonic_s();
    while (gate_watchdog_run.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      const double now = monotonic_s();
      if (flow_gate.poll(now) < 0) {
        LOG_I(PHY, "SENSING_GATE close reason=no_dl_ul_flow_for_2s\n");
        nr_isac_request_discard();
        nr_isac_record_gate_close(); // Task 10; until then define it as an empty static function
      }
      if (now - last_stats >= 10.0) {
        last_stats = now;
        LOG_I(PHY, "SENSING_GATE stats open=%d admitted=%lu rejected=%lu gate_discarded_rows=%lu\n",
              flow_gate.open() ? 1 : 0, (unsigned long)gate_admitted.load(), (unsigned long)gate_rejected.load(),
              (unsigned long)(engine ? engine->gate_discarded_rows() : 0));
      }
    }
  });
}
extern "C" void nr_isac_stop(void)
{
  gate_watchdog_run.store(false);
  if (gate_watchdog.joinable()) gate_watchdog.join();
  if (engine && started.exchange(false)) engine->stop();
}
extern "C" int nr_isac_enabled(void){return enabled.load(std::memory_order_relaxed);}
extern "C" int nr_isac_source(void){for(int i=0;i<NR_ISAC_SRC_COUNT;++i)if(pipeline.sources_mask&(1u<<i))return i;return NR_ISAC_SRC_CSI_RS;}
extern "C" int nr_isac_source_enabled(int source){return enabled.load()&&source>=0&&source<NR_ISAC_SRC_COUNT&&(pipeline.sources_mask&(1u<<source));}
extern "C" uint32_t nr_isac_rx_channels(void)
{ return enabled.load() ? (pipeline.spatial_receivers.configured ? 4u : (AOA_ENABLE ? aoa_antennas : 1u)) : 0u; }
extern "C" uint32_t nr_isac_subslot_config(uint32_t* min_re,float* min_snr){if(!enabled.load())return 0;if(min_re)*min_re=pipeline.subslot_min_re;if(min_snr)*min_snr=pipeline.subslot_min_snr_db;return pipeline.subslot_symbols;}

extern "C" void nr_isac_submit_cfr(uint32_t slot,int source,const nr_isac_carrier_t* carrier,const float* h,
                                    const uint32_t* k,const uint32_t* l,uint32_t n,float noise)
{nr_isac_submit_cfr_at(slot,0.0f,source,carrier,h,k,l,n,noise);}
extern "C" void nr_isac_submit_cfr_at(uint32_t slot,float fraction,int source,const nr_isac_carrier_t* carrier,const float* h,
                                       const uint32_t* k,const uint32_t* l,uint32_t n,float noise)
{nr_isac_submit_cfr_multi(slot,fraction,source,carrier,h,1,n,k,l,n,noise);}
extern "C" void nr_isac_submit_cfr_multi(uint32_t slot,float fraction,int source,const nr_isac_carrier_t* carrier,const float* h,
                                          uint32_t antennas,uint32_t stride,const uint32_t* k,const uint32_t* l,uint32_t n,float noise)
{nr_isac_submit_cfr_multi_session(slot,fraction,source,carrier,h,antennas,stride,k,l,n,noise,0);}
extern "C" void nr_isac_submit_cfr_multi_session(uint32_t slot,float fraction,int source,const nr_isac_carrier_t* carrier,const float* h,
                                          uint32_t antennas,uint32_t stride,const uint32_t* k,const uint32_t* l,uint32_t n,float noise,
                                          uint64_t session_id)
{
  if(!enabled.load(std::memory_order_relaxed)||!engine||!carrier||!h||!k||!l||!n)return;
  if(source<0||source>=NR_ISAC_SRC_COUNT)source=nr_isac_source();
  if(!nr_isac_source_enabled(source))return;
  if(nr_isac_source_illum(static_cast<nr_isac_source_t>(source))==NR_ISAC_ILLUM_UL
      && session_id==0){
    static std::atomic<bool> warned{false};
    if(!warned.exchange(true))
      LOG_E(PHY,"SENSING: UL CFR without session_id rejected at the ABI (source=%d); further rejections are silent\n",source);
    return;
  }
  const uint32_t expected_channels=nr_isac_rx_channels();
  const uint64_t maximum_re=(uint64_t)carrier->nof_prb*12u*14u;
  // Fail closed before allocation or pointer arithmetic.  The caller is inside the PHY, but this
  // ABI boundary must not turn a corrupt antenna/stride/RE count into an unbounded allocation or
  // an out-of-bounds read on the real-time thread.
  if(!expected_channels||antennas!=expected_channels||antennas>4||stride<n
      ||carrier->nof_prb<1||carrier->nof_prb>275||n>maximum_re){
    static std::atomic<bool> warned{false};
    if(!warned.exchange(true))
      LOG_E(PHY,"SENSING: CFR rejected at the ABI: antennas=%u expected=%u stride=%u n=%u nof_prb=%u "
            "(spatial mode needs --ue-nb-ant-rx 4); further rejections are silent\n",
            antennas,expected_channels,stride,n,carrier->nof_prb);
    return;
  }
  static thread_local std::vector<std::complex<float>> packed;
  const size_t total=(size_t)antennas*n;if(packed.size()<total)packed.resize(total);
  for(uint32_t a=0;a<antennas;++a){const float* input=h+(size_t)2*a*stride;for(uint32_t i=0;i<n;++i)packed[(size_t)a*n+i]={input[2*i],input[2*i+1]};}
  engine->submit(slot,fraction,static_cast<nr_isac_source_t>(source),*carrier,packed.data(),antennas,k,l,n,noise,session_id);
}
