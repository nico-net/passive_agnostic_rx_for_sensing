/* SPDX-License-Identifier: OAI-Public-License-1.1 */
/** Deterministic golden checks against the Python reference named in ../README.md. */
#include "adaptive_threshold.h"
#include "aoa.h"
#include "cross_leg_fusion.h"
#include "detector.h"
#include "detector_cuda.h"
#include "enu_tracker.h"
#include "hierarchical_tracker.h"
#include "nr_isac.h"
#include "fft.h"
#include "report_writer.h"
#include "sensing_engine.h"
#include "sync_correction.h"
#include "variable_cpi.h"

#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace nr_isac;

namespace {
void require(bool condition, const char* message)
{
  if (!condition) throw std::runtime_error(message);
}

void close(double actual, double expected, double tolerance, const char* message)
{
  if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance) {
    std::fprintf(stderr, "%s: actual=%.17g expected=%.17g tolerance=%.3g\n",
                 message, actual, expected, tolerance);
    throw std::runtime_error(message);
  }
}

CfrWindow fractional_component(double range_bin=12.25, double doppler_bin=32.3125)
{
  CfrWindow w; w.antennas=4;w.rows=64;w.subcarriers=512;w.scs_hz=30000.0;
  w.fc_hz=3499440000.0;w.values.resize((size_t)w.antennas*w.rows*w.subcarriers);
  w.observed.assign((size_t)w.rows*w.subcarriers,1);w.row_time_slots.resize(w.rows);
  w.row_slot_idx.resize(w.rows);w.row_slot_frac.assign(w.rows,0.0);w.row_source_mask.assign(w.rows,1);
  const double dwell=(w.rows-1)*slot_duration_s(w.scs_hz);
  const double rate_res=C_MPS*(w.rows-1.0)/(w.fc_hz*w.rows*dwell);
  const double rate=-(doppler_bin-32.0)*rate_res;
  const double gains[4]{0.0,0.25,-0.4,0.7};
  for(uint32_t r=0;r<w.rows;++r){w.row_time_slots[r]=r;w.row_slot_idx[r]=r;
    const double time=r*slot_duration_s(w.scs_hz);
    for(uint32_t k=0;k<w.subcarriers;++k){
      const double phase=-2.0*PI*range_bin*k/w.subcarriers
                         -2.0*PI*rate*w.fc_hz*time/C_MPS;
      for(uint32_t a=0;a<4;++a)w.values[w.sample(a,r,k)]=std::polar(1.0f,static_cast<float>(phase+gains[a]));
    }}
  return w;
}

void test_fft()
{
  std::vector<std::complex<double>> values{{1.0,2.0},{-0.5,0.25},{3.0,-1.0},{0.0,0.5},{2.0,0.0}};
  fft_inplace(values,false);
  const std::complex<double> expected[]={{5.5,1.75},{-1.6074392409273424,3.0960482796416748},
      {2.2870565790782695,5.9658699338354326},{-0.8600055959534274,-2.6793954253978542},
      {-0.31961174219749988,1.8674772119207461}};
  for(size_t i=0;i<values.size();++i)require(std::abs(values[i]-expected[i])<1e-12,"FFT differs from numpy.fft.fft");
  fft_inplace(values,true);
  const std::complex<double> original[]={{1,2},{-.5,.25},{3,-1},{0,.5},{2,0}};
  for(size_t i=0;i<values.size();++i)require(std::abs(values[i]-original[i])<1e-12,"inverse FFT differs from NumPy");
}

void test_adaptive_threshold()
{
  close(adaptive_z_threshold(0.01,8),3.023341439739154,1e-8,"adaptive threshold parity");
  const uint32_t nr=8,nd=9;std::vector<double> surface(nr*nd);
  for(uint32_t r=0;r<nr;++r)for(uint32_t d=0;d<nd;++d)surface[(size_t)r*nd+d]=
      std::exp(0.07*r-0.03*d+0.01*((r*17+d*11)%7));
  surface[3*nd+4]=9.0;
  const auto stat=cut_excluded_local_statistic(surface,nr,nd,3,4,3,3,1,1);
  close(stat.z,10.031723118345276,1e-10,"CUT-excluded log-MAD statistic parity");
  require(stat.training_cells==40,"CUT-excluded training-cell count parity");
}

void test_detector()
{
  PipelineConfig config;config.maximum_components=1;config.maximum_objects=1;
  config.maximum_range_m=300.0;config.maximum_target_speed_mps=50.0;
  const auto result=detect_clean(fractional_component(),config);
  require(result.components.size()==1,"CLEAN failed to return pure component");
  close(result.components[0].range_bin,12.25,2e-4,"continuous CLEAN range parity");
  close(result.components[0].doppler_bin,32.3125,2e-4,"continuous CLEAN Doppler parity");
  close(result.components[0].score,32767.999999999607,2e-4,"normalized pure-component score parity");

  const auto zero_doppler=detect_clean(fractional_component(8.5,32.0),config);
  require(zero_doppler.components.size()==1,"zero-Doppler CUT was incorrectly notched");
  close(zero_doppler.components[0].range_bin,8.5,2e-4,"zero-Doppler range parity");
  close(zero_doppler.components[0].doppler_bin,32.0,2e-4,"zero-Doppler bin must remain searchable");
}

void test_required_cuda_contract()
{
  if (detector_cuda_available()) return;
  PipelineConfig config;
  config.maximum_components = 1;
  config.maximum_objects = 1;
  setenv("NR_ISAC_REQUIRE_CUDA", "1", 1);
  bool rejected = false;
  try {
    (void)detect_clean(fractional_component(), config);
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  unsetenv("NR_ISAC_REQUIRE_CUDA");
  require(rejected, "NR_ISAC_REQUIRE_CUDA did not reject the CPU fallback");
}

void test_aoa()
{
  const double fc=3499440000.0,wavelength=C_MPS/fc;
  ArrayGeometry geometry;geometry.configured=true;geometry.positions={Vec3{0,0,0},Vec3{.5*wavelength,0,0},
      Vec3{0,.5*wavelength,0},Vec3{.5*wavelength,.5*wavelength,0}};geometry.broadside={0,0,1};
  const Vec3 truth=direction_from_angles(35.0*PI/180.0,20.0*PI/180.0);
  std::array<std::complex<double>,4> response;
  for(size_t a=0;a<4;++a){const double p=2*PI*dot(geometry.positions[a]-geometry.positions[0],truth)/wavelength;
    response[a]=std::polar(2.0,p+0.3);}
  const auto estimate=grid_free_upa_aoa(response,geometry,fc);
  require(estimate.valid,"grid-free AoA rejected a physical UPA response");
  close(estimate.azimuth_deg,35.0,1e-8,"grid-free AoA azimuth parity");
  close(estimate.elevation_deg,20.0,1e-8,"grid-free AoA elevation parity");
  require(estimate.covariance_valid,"grid-free AoA covariance missing");
  require(admit_aoa_for_tracking(estimate, {}).valid,
          "AoA quality admission rejected an exact physical response");
  AoaEstimate poor_fit = estimate;
  poor_fit.relative_manifold_residual_energy = 0.6;
  require(!admit_aoa_for_tracking(poor_fit, {}).valid,
          "AoA quality admission accepted excessive manifold residual");
  AoaEstimate uncertain = estimate;
  uncertain.covariance_rad2(0,0) = std::pow(104.0 * PI / 180.0, 2);
  uncertain.covariance_rad2(1,1) = std::pow(52.0 * PI / 180.0, 2);
  require(!admit_aoa_for_tracking(uncertain, {}).valid,
          "AoA quality admission accepted unusable angular uncertainty");

  CfrWindow calibrated; calibrated.antennas=4;calibrated.rows=2;calibrated.subcarriers=5;
  calibrated.scs_hz=30000;calibrated.fc_hz=fc;calibrated.values.resize(40);
  calibrated.observed.assign(10,1);calibrated.row_time_slots={0,1};
  calibrated.row_slot_idx={0,1};calibrated.row_slot_frac={0,0};
  calibrated.row_source_mask={1,1};
  ArrayCalibration coefficients;coefficients.configured=true;
  coefficients.physical_to_observed={2,0,3,1};
  coefficients.gain={1.0,1.2,.8,1.4};
  coefficients.phase_rad={0.0,-.4,.7,-1.1};
  coefficients.delay_s={0.0,3.2e-9,-2.4e-9,5.1e-9};
  std::vector<std::complex<float>> ideal(calibrated.values.size());
  for(uint32_t physical=0;physical<4;++physical)
    for(uint32_t row=0;row<calibrated.rows;++row)
      for(uint32_t subcarrier=0;subcarrier<calibrated.subcarriers;++subcarrier){
        const auto value=std::polar(1.0f+0.1f*physical,
                                   static_cast<float>(.2*row-.1*subcarrier+.3*physical));
        ideal[calibrated.sample(physical,row,subcarrier)]=value;
        const double offset_hz=(subcarrier-.5*(calibrated.subcarriers-1.0))*calibrated.scs_hz;
        const auto correction=std::polar(coefficients.gain[physical],
            coefficients.phase_rad[physical]+2*PI*offset_hz*coefficients.delay_s[physical]);
        calibrated.values[calibrated.sample(coefficients.physical_to_observed[physical],row,subcarrier)]=
            static_cast<std::complex<float>>(static_cast<std::complex<double>>(value)/correction);
      }
  apply_array_calibration(calibrated,coefficients);
  for(size_t i=0;i<ideal.size();++i)
    require(std::abs(calibrated.values[i]-ideal[i])<5e-7,
            "gain/phase/delay/channel-order calibration differs from reference");
  ArrayCalibration malformed=coefficients;malformed.physical_to_observed={0,0,2,3};
  bool malformed_rejected=false;
  try {apply_array_calibration(calibrated,malformed);}
  catch(const std::invalid_argument&){malformed_rejected=true;}
  require(malformed_rejected,"duplicate array-calibration channel was not rejected");

  CfrWindow policy;policy.antennas=4;policy.rows=2;policy.subcarriers=2;policy.scs_hz=30000;
  policy.fc_hz=fc;policy.values.assign(16,{1,0});policy.observed.assign(4,1);
  policy.row_time_slots={0,1};policy.row_slot_idx={0,1};policy.row_slot_frac={0,0};
  policy.row_source_mask={1u<<NR_ISAC_SRC_CSI_RS,1u<<NR_ISAC_SRC_PUSCH_DMRS};
  const auto disabled=aoa_observed_mask(policy,false,true);
  require(disabled==std::vector<uint8_t>({0,0,0,0}),"AOA_ENABLE=0 must force all AoA input off");
  const auto dl_only=aoa_observed_mask(policy,true,false);
  require(dl_only==std::vector<uint8_t>({1,1,0,0}),"AOA_UL_ENABLE=0 must retain only DL rows");
  const auto dl_ul=aoa_observed_mask(policy,true,true);
  require(dl_ul==policy.observed,"AOA_UL_ENABLE=1 must admit UL rows when master AoA is enabled");

  CfrWindow joint;joint.antennas=4;joint.rows=64;joint.subcarriers=128;joint.scs_hz=30000;
  joint.fc_hz=fc;joint.values.resize((size_t)joint.antennas*joint.rows*joint.subcarriers);
  joint.observed.assign((size_t)joint.rows*joint.subcarriers,1);joint.row_time_slots.resize(joint.rows);
  joint.row_slot_idx.resize(joint.rows);joint.row_slot_frac.assign(joint.rows,0);
  joint.row_source_mask.assign(joint.rows,1u<<NR_ISAC_SRC_CSI_RS);
  const double spacing=.45*wavelength;
  ArrayGeometry yz;yz.configured=true;yz.positions={Vec3{0,-spacing/2,-spacing/2},Vec3{0,-spacing/2,spacing/2},
      Vec3{0,spacing/2,-spacing/2},Vec3{0,spacing/2,spacing/2}};yz.broadside={1,0,0};
  const Vec3 target_direction=normalized({.72,-.48,.50});
  std::array<std::complex<double>,4> los_spatial{},target_spatial{};
  for(size_t a=0;a<4;++a){
    los_spatial[a]=std::polar(1.0,2*PI*dot(yz.positions[a],yz.broadside)/wavelength);
    target_spatial[a]=std::polar(1.0,2*PI*dot(yz.positions[a],target_direction)/wavelength);
  }
  const double target_range=2.0,target_rate=2.0;
  for(uint32_t r=0;r<joint.rows;++r){joint.row_time_slots[r]=r;joint.row_slot_idx[r]=r;
    const double time=r*slot_duration_s(joint.scs_hz);
    for(uint32_t k=0;k<joint.subcarriers;++k){
      const double target_phase=-2*PI*k*joint.scs_hz*target_range/C_MPS
                                -2*PI*target_rate*joint.fc_hz*time/C_MPS;
      for(uint32_t a=0;a<4;++a){
        const std::complex<double> value=40.0*los_spatial[a]
            +target_spatial[a]*std::polar(1.0,target_phase);
        joint.values[joint.sample(a,r,k)]={static_cast<float>(value.real()),static_cast<float>(value.imag())};
      }
    }}
  const auto isolated=isolate_target_response(joint,joint.observed,target_range,target_rate,{},los_spatial,0.0);
  require(isolated.valid,"surveyed-LOS joint fit rejected a separable near-LOS target");
  require(isolated.residual_energy_fraction<1e-5,
          "surveyed-LOS joint fit left excessive residual energy");
  for(size_t a=0;a<4;++a)
    close(std::abs(isolated.response[a]/isolated.response[0]-target_spatial[a]/target_spatial[0]),
          0.0,2e-3,"surveyed-LOS target response differs from Python joint GLS");
  const auto isolated_angle=grid_free_upa_aoa(isolated.response,yz,fc,
      isolated.phase_covariance_valid?&isolated.baseline_phase_covariance:nullptr);
  require(isolated_angle.valid,"isolated surveyed-LOS response has no valid angle");
  require(norm(isolated_angle.direction-target_direction)<2e-3,
          "surveyed-LOS joint-fit direction differs from Python");

  const auto duplicate_los=isolate_target_response(joint,joint.observed,0.0,0.0,{},los_spatial,0.0);
  require(!duplicate_los.valid&&duplicate_los.unique_energy_fraction<1e-6,
          "target identical to surveyed LOS must be non-identifiable");
  const auto duplicate_component=isolate_target_response(
      joint,joint.observed,target_range,target_rate,{{target_range,target_rate}},los_spatial,0.0);
  require(!duplicate_component.valid&&duplicate_component.unique_energy_fraction<1e-6,
          "duplicate target/nuisance columns must be non-identifiable");
}

void test_aoa_component_mixture_and_cross_leg_fusion()
{
  auto estimate = [](double azimuth_deg, double elevation_deg = 5.0) {
    AoaEstimate value;
    value.valid = value.covariance_valid = true;
    value.azimuth_deg = azimuth_deg;
    value.elevation_deg = elevation_deg;
    value.direction = direction_from_angles(azimuth_deg * PI / 180.0,
                                             elevation_deg * PI / 180.0);
    value.covariance_rad2(0, 0) = std::pow(0.5 * PI / 180.0, 2);
    value.covariance_rad2(1, 1) = std::pow(0.5 * PI / 180.0, 2);
    value.phase_fit_residual_rms_rad = 0.01;
    value.relative_manifold_residual_energy = 0.01;
    return value;
  };
  const AoaEstimate mixture = combine_aoa_estimates(
      {estimate(10.0), estimate(12.0), estimate(40.0)}, {100.0, 100.0, 10.0}, {});
  require(mixture.valid && mixture.azimuth_deg > 10.0 && mixture.azimuth_deg < 14.0,
          "component AoA mixture direction differs from Python");
  require(mixture.component_aoa_count == 3 && mixture.component_direction_rms_deg > 5.0,
          "component AoA mixture lost between-scatterer uncertainty");
  require(std::sqrt(mixture.covariance_rad2(0, 0)) * 180.0 / PI > 5.0,
          "component AoA mixture incorrectly divided systematic spread by sample count");
  require(!combine_aoa_estimates(
      {estimate(-70.0), estimate(70.0)}, {1.0, 1.0}, {}).valid,
      "spatially uninformative component disagreement was admitted");

  Detection dl_match, dl_alternative, ul;
  dl_match.range_m = 20.0;
  dl_match.aoa = estimate(10.0);
  dl_alternative.range_m = 50.0;
  dl_alternative.aoa = estimate(80.0);
  ul.aoa = estimate(11.0);
  const auto fused = confirm_and_fuse_dl_with_ul(
      {dl_match, dl_alternative}, {ul}, true);
  require(fused.dl_measurements.size() == 2
              && fused.diagnostics.suppressed_dl_candidates == 0,
          "one-CPI UL evidence deleted a DL alternative");
  require(fused.dl_measurements[0].ul_confirmation_candidate_specific
              && !fused.dl_measurements[1].ul_confirmation_candidate_specific,
          "UL bearing was not paired to the agreeing DL candidate");
  require(fused.auxiliary_ul_aoa.size() == 1
              && fused.auxiliary_ul_aoa[0].covariance_rad2(0, 0)
                     > ul.aoa.covariance_rad2(0, 0),
          "UL auxiliary bearing lacks the cross-leg systematic covariance floor");

  ul.aoa.valid = false;
  const auto scene = confirm_and_fuse_dl_with_ul({dl_match, dl_alternative}, {ul}, false);
  require(scene.dl_measurements.size() == 2 && scene.auxiliary_ul_aoa.empty()
              && scene.dl_measurements[0].ul_confirmation_supported
              && !scene.dl_measurements[0].ul_confirmation_candidate_specific,
          "UL without AoA pretended to provide per-candidate observability");
}

void test_enu_geometry()
{
  BistaticGeometry geometry{{0,0,0},{100,0,0}};geometry.validate();
  const Vec3 position{45,30,8},velocity{2,-1,.5};
  const std::vector<double> state{position.x,position.y,position.z,velocity.x,velocity.y,velocity.z};
  const auto z=enu_measurement_model(state,geometry,true);
  const Vec3 recovered=position_from_measurement(geometry,z[0],z[2],z[3]);
  close(norm(recovered-position),0.0,1e-10,"ray-ellipsoid birth geometry parity");
  const Matrix h=enu_measurement_jacobian(state,geometry,true);
  const double step=1e-5;for(size_t c=0;c<6;++c){auto plus=state,minus=state;plus[c]+=step;minus[c]-=step;
    const auto zp=enu_measurement_model(plus,geometry,true),zm=enu_measurement_model(minus,geometry,true);
    for(size_t r=0;r<4;++r)close(h(r,c),(zp[r]-zm[r])/(2*step),2e-7,"ENU analytic Jacobian parity");}

  Detection birth; birth.range_m=z[0];birth.range_rate_mps=z[1];birth.dwell_s=.032;
  birth.aoa.valid=true;birth.aoa.covariance_valid=true;birth.aoa.azimuth_deg=z[2]*180.0/PI;
  birth.aoa.elevation_deg=z[3]*180.0/PI;birth.aoa.covariance_rad2=Matrix(2,2);
  birth.aoa.covariance_rad2(0,0)=.01;birth.aoa.covariance_rad2(1,1)=.01;
  EnuTrackerConfig config;
  EnuTrack track(1,0.0,birth,geometry,3.0,1.0,.032,config,0);
  Detection candidate=birth;bool found=false;
  for(int i=1;i<=20000&&!found;++i){
    candidate.range_m=birth.range_m+i*.01;
    const auto fit=track.innovation(candidate,3.0,1.0,false);
    found=fit.valid&&fit.with_angles&&fit.nis>config.gate_chi2_2d&&fit.nis<config.gate_chi2_4d;
  }
  require(found,"could not construct a 4-D-only admissible ENU innovation");
  require(track.update(candidate,3.0,1.0,0,std::nullopt,std::nullopt),
          "4-D innovation inside the 4-D gate was incorrectly rejected by the 2-D gate");

  EnuTrack temporal(2,0.0,birth,geometry,3.0,1.0,.032,config,0);
  temporal.predict(0.04);
  Detection impossible=birth;
  impossible.aoa.azimuth_deg += 70.0;
  require(temporal.update(impossible,3.0,1.0,0,std::nullopt,std::nullopt),
          "temporal AoA rejection incorrectly discarded valid range/rate");
  require(!temporal.snapshot().last_update_used_angles
              && temporal.snapshot().temporal_aoa_rejections == 1,
          "coherent but physically impossible AoA jump entered the ENU state");

  temporal.predict(0.08);
  Detection angular_fade=birth;
  angular_fade.aoa.valid=false;
  require(temporal.update(angular_fade,3.0,1.0,0,std::nullopt,std::nullopt),
          "DL angular fade incorrectly discarded range/rate");
  require(temporal.update_angles(birth.aoa)
              && temporal.snapshot().auxiliary_aoa_updates == 1,
          "UL bearing-only fallback did not update a DL angular fade");
}

void test_repeated_ul_confirmation_gates_global_birth()
{
  BistaticGeometry geometry{{0, 0, 0}, {100, 0, 0}};
  const std::vector<double> state{45.0, 30.0, 8.0, 0.0, 0.0, 0.0};
  const auto measurement = enu_measurement_model(state, geometry, true);
  HierarchicalEnuTracker tracker(geometry);
  for (uint64_t step = 0; step < 4; ++step) {
    Detection detection;
    detection.range_m = measurement[0];
    detection.range_rate_mps = measurement[1];
    detection.score = 1000.0;
    detection.dwell_s = 0.032;
    detection.aoa.valid = detection.aoa.covariance_valid = true;
    detection.aoa.azimuth_deg = measurement[2] * 180.0 / PI;
    detection.aoa.elevation_deg = measurement[3] * 180.0 / PI;
    detection.aoa.direction = direction_from_angles(measurement[2], measurement[3]);
    detection.aoa.covariance_rad2(0, 0) = std::pow(1.0 * PI / 180.0, 2);
    detection.aoa.covariance_rad2(1, 1) = std::pow(1.0 * PI / 180.0, 2);
    detection.ul_confirmation_candidate_specific = step >= 2;
    detection.ul_confirmation_supported = step >= 2;
    detection.ul_confirmation_status = step >= 2
        ? "candidate_specific_bearing_match" : "no_cross_leg_bearing_match_preserved";
    tracker.update(step * 0.04, {detection}, 3.0, 1.0, step, 0.032, true, {}, true, true);
    if (step < 3)
      require(tracker.snapshots().empty(),
              "global track birthed before repeated candidate-specific UL support");
  }
  require(tracker.snapshots().size() == 1,
          "repeated candidate-specific UL support did not admit global-track birth");
}

void test_variable_cpi()
{
  PipelineConfig c;CpiPlanner planner(c);TrackSnapshot s;s.status="confirmed";s.has_time=true;
  s.range_m=40;s.range_rate_mps=20;s.range_accel_mps2=50;s.sigma_rate_mps=.2;s.sigma_accel_mps2=.2;
  s.confirmed_update_count=4;s.source_cpi_sequence=7;
  const auto p=planner.plan(s,1.0,3499440000.0,512,30000.0,false,100.0);
  require(p.duration_bank_index==1,"variable-CPI Python bank selection parity");
  close(p.target_dwell_s,.016,1e-15,"variable-CPI dwell parity");
  close(*p.v_q_mps,20.6,1e-12,"variable-CPI rate envelope parity");
  close(*p.a_q_mps2,50.6,1e-12,"variable-CPI acceleration envelope parity");
  require(p.full_search(),"active Python baseline must use full Doppler search");
}

void test_validation_report_compatibility()
{
  PipelineConfig c;
  PipelineReport r;
  r.start_utc_ns=123;r.cpi_duration_ns=7500000;
  r.dropped_cpis=9;r.discarded_pending_rows=11;r.discarded_pending_intervals=2;
  r.first_row_time_ns=50000000;r.last_row_time_ns=57500000;
  r.source_occurrences={1,2,3,4,5,6,7};
  Detection d;d.aoa.valid=true;d.aoa.azimuth_deg=12.5;d.aoa.elevation_deg=-3.0;
  r.detections.push_back(d);
  const std::string json=build_report_json(r,c);
  require(json.find("\"cpi_duration_ns\":7500000")!=std::string::npos,
          "validation report lost CPI duration");
  require(json.find("\"first_row_time_ns\":50000000")!=std::string::npos,
          "validation report lost first radio-row time");
  require(json.find("\"last_row_time_ns\":57500000")!=std::string::npos,
          "validation report lost last radio-row time");
  require(json.find("\"dropped_cpis\":9")!=std::string::npos,
          "validation report lost complete-CPI backpressure count");
  require(json.find("\"discarded_pending_rows\":11")!=std::string::npos,
          "validation report lost bounded-backlog row count");
  require(json.find("\"discarded_pending_intervals\":2")!=std::string::npos,
          "validation report lost bounded-backlog interval count");
  require(json.find("\"src_occ\":[1,2,3,4,5,6,7]")!=std::string::npos,
          "validation report lost source-count compatibility vector");
  require(json.find("\"azimuth_deg\":12.5")!=std::string::npos,
          "validation report lost top-level detection AoA compatibility");
  require(json.find("\"aoa_quality_policy\"")!=std::string::npos,
          "validation report lost AoA quality policy provenance");
  require(json.find("\"array_calibration\"")!=std::string::npos,
          "validation report lost receive-chain calibration provenance");
  require(json.find("\"visible_region_clipped\":false")!=std::string::npos,
          "validation report lost AoA visible-region diagnostic");

  PipelineConfig captured=c;captured.capture_rvm=true;
  r.detector.axes.range_bins=2;r.detector.axes.rate_bins=3;
  r.detector.initial_likelihood={1,2,3,4,5,6};
  r.detector.initial_dl_likelihood={6,5,4,3,2,1};
  r.detector.dl_observed_re_count=42;
  const std::string capture_json=build_report_json(r,captured);
  require(capture_json.find("\"dl_rvm_layout\":\"doppler_major_range_minor\"")!=std::string::npos,
          "captured report lost DL-only RDM layout");
  require(capture_json.find("\"dl_rvm_source_mask\":15")!=std::string::npos,
          "captured report lost exact Python DL source policy");
  require(capture_json.find("\"dl_rvm_observed_re_count\":42")!=std::string::npos,
          "captured report lost DL-only observation count");
  require(capture_json.find("\"dl_rvm_blob\":[6,3,5,2,4,1]")!=std::string::npos,
          "captured report lost Doppler-major DL-only RDM payload");
}

void test_causal_cpi_pipeline()
{
  const std::string path="/tmp/nr_isac_causal_cpi_test.jsonl";
  std::remove(path.c_str());
  PipelineConfig c;
  c.sources_mask=1u<<NR_ISAC_SRC_CSI_RS;
  c.duration_bank_s={0.001};c.bootstrap_duration_index=0;
  c.minimum_dwell_s=0.001;c.maximum_dwell_s=0.001;
  c.minimum_rows=2;c.maximum_rows=4;
  c.sync_enable=false;c.family_static=false;c.tracker_enable=false;
  c.maximum_components=1;c.maximum_objects=1;
  c.report_path=path;c.out_path.clear();
  nr_isac_carrier_t carrier{};
  carrier.nof_prb=2;carrier.scs_hz=30000;carrier.dl_center_hz=3499440000;
  carrier.slots_per_frame=20;carrier.pci=1;
  std::vector<std::complex<float>> h(24);
  std::vector<uint32_t> k(24),symbol(24,2);
  for(uint32_t i=0;i<24;++i)k[i]=i;
  {
    SensingEngine engine(c,2,1);engine.start();
    for(uint32_t slot=0;slot<20;++slot){
      for(uint32_t i=0;i<24;++i)
        h[i]=std::polar(1.0f,static_cast<float>(-.2*i+.1*slot));
      engine.submit(slot,0.0f,NR_ISAC_SRC_CSI_RS,carrier,h.data(),1,
                    k.data(),symbol.data(),h.size(),1.0f);
    }
    engine.stop();
    engine.start();
    for(uint32_t slot=20;slot<23;++slot){
      for(uint32_t i=0;i<24;++i)
        h[i]=std::polar(1.0f,static_cast<float>(-.2*i+.1*slot));
      engine.submit(slot,0.0f,NR_ISAC_SRC_CSI_RS,carrier,h.data(),1,
                    k.data(),symbol.data(),h.size(),1.0f);
    }
    engine.stop();
  }
  std::ifstream input(path);require(input.good(),"causal CPI pipeline emitted no report file");
  std::string line;uint32_t reports=0;
  while(std::getline(input,line)){
    ++reports;
    const std::string sequence="\"cpi_sequence\":"+std::to_string(reports);
    require(line.find(sequence)!=std::string::npos,"causal CPI sequence is not ordered");
    require(line.find("\"dropped_submissions\":0")!=std::string::npos,
            "causal accumulator lost an input row");
    require(line.find("\"dropped_cpis\":0")!=std::string::npos,
            "causal processor lost a complete CPI");
    require(line.find("\"discarded_pending_rows\":0")!=std::string::npos,
            "causal accumulator discarded pending rows");
  }
  require(reports==8,"causal close/drain/restart did not preserve all eight expected CPIs");
  std::remove(path.c_str());
}

void test_finite_admission_window()
{
  const std::string path="/tmp/nr_isac_admission_window_test.jsonl";
  std::remove(path.c_str());
  PipelineConfig c;
  c.sources_mask=1u<<NR_ISAC_SRC_CSI_RS;
  c.duration_bank_s={0.001};c.bootstrap_duration_index=0;
  c.minimum_dwell_s=0.001;c.maximum_dwell_s=0.001;
  c.minimum_rows=2;c.maximum_rows=4;
  c.sync_enable=false;c.family_static=false;c.tracker_enable=false;
  c.maximum_components=1;c.maximum_objects=1;
  c.report_path=path;c.out_path.clear();
  c.admission_window_enabled=true;c.admission_start_slot=20;c.admission_end_slot=24;
  nr_isac_carrier_t carrier{};
  carrier.nof_prb=2;carrier.scs_hz=30000;carrier.dl_center_hz=3499440000;
  carrier.slots_per_frame=20;carrier.pci=1;
  std::vector<std::complex<float>> h(24,{1.0f,0.0f});
  std::vector<uint32_t> k(24),symbol(24,2);
  for(uint32_t i=0;i<24;++i)k[i]=i;
  {
    SensingEngine engine(c,2,1);engine.start();
    for(uint32_t slot=0;slot<30;++slot)
      engine.submit(slot,0.0f,NR_ISAC_SRC_CSI_RS,carrier,h.data(),1,
                    k.data(),symbol.data(),h.size(),1.0f);
    engine.stop();
  }
  std::ifstream input(path);require(input.good(),"admission-gated pipeline emitted no report file");
  std::string line;uint32_t reports=0;
  while(std::getline(input,line)) {
    ++reports;
    require(line.find("\"first_row_time_ns\":10000000")!=std::string::npos,
            "admission gate admitted a row before its configured start slot");
    require(line.find("\"sensing_admission\":{\"enabled\":true,\"start_radio_slot\":20,\"end_radio_slot_exclusive\":24")!=std::string::npos,
            "admission configuration is not attested in the report");
  }
  require(reports==1,"finite admission window emitted an unexpected CPI count");
  std::remove(path.c_str());
}

std::vector<double> json_number_array(const std::string& line, const std::string& key)
{
  const std::string prefix = "\"" + key + "\":[";
  const size_t begin = line.find(prefix);
  require(begin != std::string::npos, "mixed-row capture report is missing an RDM field");
  const char* cursor = line.c_str() + begin + prefix.size();
  std::vector<double> values;
  while (*cursor != ']') {
    char* end = nullptr;
    const double value = std::strtod(cursor, &end);
    require(end != cursor && std::isfinite(value), "mixed-row capture RDM contains an invalid value");
    values.push_back(value);
    cursor = end;
    if (*cursor == ',') ++cursor;
    else require(*cursor == ']', "mixed-row capture RDM has invalid JSON separators");
  }
  require(!values.empty(), "mixed-row capture RDM is empty");
  return values;
}

double map_energy(const std::vector<double>& values)
{
  double total = 0.0;
  for (double value : values) total += value;
  return total;
}

std::array<std::vector<double>, 3> mixed_row_capture(float ul_amplitude,
                                                     const std::string& path)
{
  std::remove(path.c_str());
  PipelineConfig c;
  c.sources_mask = (1u << NR_ISAC_SRC_CSI_RS) | (1u << NR_ISAC_SRC_PUSCH_DMRS);
  c.duration_bank_s = {0.001}; c.bootstrap_duration_index = 0;
  c.minimum_dwell_s = 0.001; c.maximum_dwell_s = 0.001;
  c.minimum_rows = 2; c.maximum_rows = 4;
  c.sync_enable = false; c.family_static = false; c.tracker_enable = false;
  c.maximum_components = 1; c.maximum_objects = 1; c.capture_rvm = true;
  c.maximum_range_m = 200.0; c.report_path = path; c.out_path.clear();
  nr_isac_carrier_t carrier{};
  carrier.nof_prb = 2; carrier.scs_hz = 30000; carrier.dl_center_hz = 3499440000;
  carrier.slots_per_frame = 20; carrier.pci = 1;
  std::vector<std::complex<float>> dl(24, {1.0f, 0.0f});
  std::vector<std::complex<float>> ul(24, {ul_amplitude, 0.0f});
  std::vector<uint32_t> k(24), symbol(24, 2);
  for (uint32_t i = 0; i < k.size(); ++i) k[i] = i;
  {
    SensingEngine engine(c, 2, 1); engine.start();
    for (uint32_t slot = 0; slot < 10; ++slot) {
      // Same slot/fraction/re means PendingRow would formerly average these into one value and
      // retain only an ORed source mask.  The report DL map must now remain invariant to UL.
      engine.submit(slot, 0.0f, NR_ISAC_SRC_CSI_RS, carrier, dl.data(), 1,
                    k.data(), symbol.data(), dl.size(), 1.0f);
      engine.submit(slot, 0.0f, NR_ISAC_SRC_PUSCH_DMRS, carrier, ul.data(), 1,
                    k.data(), symbol.data(), ul.size(), 1.0f);
    }
    engine.stop();
  }
  std::ifstream input(path); require(input.good(), "mixed-row capture emitted no report");
  std::string line; require(static_cast<bool>(std::getline(input, line)), "mixed-row capture report is empty");
  require(line.find("\"dl_rvm_source_mask\":15") != std::string::npos,
          "mixed-row capture does not attest the DL source mask");
  auto primary = json_number_array(line, "rvm_blob");
  auto dl_only = json_number_array(line, "dl_rvm_blob");
  auto ul_only = json_number_array(line, "ul_rvm_blob");
  require(primary.size() == dl_only.size(), "mixed-row primary/DL RDM shapes differ");
  require(primary.size() == ul_only.size(), "mixed-row DL/UL RDM shapes differ");
  std::remove(path.c_str());
  return {std::move(primary), std::move(dl_only), std::move(ul_only)};
}

// --- P10a: branch identity on the CFR ABI ---------------------------------------------------
// Pins the three things this slice claims: an untagged CPI's report is unchanged (no branch field
// at all), a single-branch CPI names its branch, and a CPI fused from several carries the mask
// without naming one of them. The strongest of the three is the FIRST: the tagged line, with only
// the two branch fields removed, must be character-for-character the untagged line -- i.e. carrying
// the identity changes no numeric result anywhere in the pipeline.
std::string drop_json_field(std::string line, const std::string& key)
{
  const std::string needle = ",\"" + key + "\":";
  const size_t at = line.find(needle);
  if (at == std::string::npos) return line;
  size_t end = at + needle.size();
  while (end < line.size() && line[end] != ',' && line[end] != '}') ++end;
  return line.erase(at, end - at);
}

// branch_b < 0 submits one branch only.
std::string branch_capture(int branch_a, int branch_b, const std::string& path)
{
  std::remove(path.c_str());
  PipelineConfig c;
  c.sources_mask = (1u << NR_ISAC_SRC_CSI_RS);
  c.duration_bank_s = {0.001}; c.bootstrap_duration_index = 0;
  c.minimum_dwell_s = 0.001; c.maximum_dwell_s = 0.001;
  c.minimum_rows = 2; c.maximum_rows = 4;
  c.sync_enable = false; c.family_static = false; c.tracker_enable = false;
  c.maximum_components = 1; c.maximum_objects = 1;
  c.maximum_range_m = 200.0; c.report_path = path; c.out_path.clear();
  nr_isac_carrier_t carrier{};
  carrier.nof_prb = 2; carrier.scs_hz = 30000; carrier.dl_center_hz = 3499440000;
  carrier.slots_per_frame = 20; carrier.pci = 1;
  std::vector<std::complex<float>> h(24, {1.0f, 0.0f});
  std::vector<uint32_t> k(24), symbol(24, 2);
  for (uint32_t i = 0; i < k.size(); ++i) k[i] = i;
  {
    SensingEngine engine(c, 2, 1); engine.start();
    for (uint32_t slot = 0; slot < 10; ++slot) {
      engine.submit(slot, 0.0f, NR_ISAC_SRC_CSI_RS, carrier, h.data(), 1,
                    k.data(), symbol.data(), h.size(), 1.0f,
                    branch_a < 0 ? NR_ISAC_BRANCH_NONE : static_cast<uint8_t>(branch_a));
      if (branch_b >= 0)
        engine.submit(slot, 0.5f, NR_ISAC_SRC_CSI_RS, carrier, h.data(), 1,
                      k.data(), symbol.data(), h.size(), 1.0f, static_cast<uint8_t>(branch_b));
    }
    engine.stop();
  }
  std::ifstream input(path); require(input.good(), "branch capture emitted no report");
  std::string line;
  require(static_cast<bool>(std::getline(input, line)), "branch capture report is empty");
  std::remove(path.c_str());
  return line;
}

void test_branch_identity_report()
{
  const std::string legacy = branch_capture(-1, -1, "/tmp/nr_isac_branch_legacy.jsonl");
  require(legacy.find("\"branch_mask\"") == std::string::npos
              && legacy.find("\"branch_id\"") == std::string::npos,
          "an untagged CPI must carry no branch field at all, not branch 0");

  const std::string tagged = branch_capture(3, -1, "/tmp/nr_isac_branch_single.jsonl");
  require(tagged.find("\"branch_mask\":8") != std::string::npos,
          "a single-branch CPI must report its own branch bit");
  require(tagged.find("\"branch_id\":3") != std::string::npos,
          "a single-branch CPI must name its branch");
  // The whole point of the slice: identity is additive. Only the wall clock may differ.
  const std::string key = "cpi_start_time_utc_ns";
  require(drop_json_field(drop_json_field(drop_json_field(tagged, "branch_mask"), "branch_id"), key)
              == drop_json_field(legacy, key),
          "carrying a branch identity changed something other than the branch fields");

  const std::string fused = branch_capture(1, 2, "/tmp/nr_isac_branch_fused.jsonl");
  require(fused.find("\"branch_mask\":6") != std::string::npos,
          "a CPI fused from two branches must report both bits");
  require(fused.find("\"branch_id\"") == std::string::npos,
          "a CPI fused from two branches must not name one of them");
}

// ---------------------------------------------------------------------------------------------
// adaptive_RX_pipeline.md P13: the per-branch SensingEngine array. Three properties are pinned
// here, in the order of how much damage getting them wrong would do:
//   (c) a CFR row tagged with an inactive branch is DROPPED, never routed to another branch;
//   (a) a single-branch (legacy) receiver's output identity is EXACTLY what it is today;
//   (b) two engines fed different data produce independent reports -- one's CPI is bit-identical
//       to what it would have been had the other never existed.
nr_rx_branch_set_t branch_set(std::initializer_list<int> active)
{
  nr_rx_branch_set_t set{};
  for (int i = 0; i < NR_RX_BRANCH_MAX; ++i) {
    set.b[i].branch_id = static_cast<uint8_t>(i);
    set.b[i].physical_channel = -1;
    set.b[i].state = NR_RXB_DISABLED;
  }
  int physical = 0;
  for (int id : active) {
    set.b[id].physical_channel = static_cast<int8_t>(physical++);
    set.b[id].state = NR_RXB_ACQUIRING;
    ++set.n_active;
  }
  return set;
}

void test_branch_engine_routing()
{
  // (c) An inactive, out-of-range or unmapped branch must resolve to "drop" (-1), never to 0.
  const nr_rx_branch_set_t two = branch_set({0, 2});
  require(branch_engine_index(two, 0) == 0, "an active branch must route to its own engine");
  require(branch_engine_index(two, 2) == 2, "the engine array is indexed by branch id, not ordinal");
  require(branch_engine_index(two, 1) == -1,
          "a CFR tagged with an INACTIVE branch must be dropped, never misrouted to another engine");
  require(branch_engine_index(two, 3) == -1, "an unmapped branch must be dropped");
  require(branch_engine_index(two, NR_RX_BRANCH_MAX) == -1, "an out-of-range branch must be dropped");
  require(branch_engine_index(two, 200) == -1, "a garbage branch id must be dropped, not wrapped");
  require(branch_engine_index(two, NR_ISAC_BRANCH_NONE) == 0,
          "an untagged row belongs to the lowest active branch (the legacy engine)");

  // A single-branch deployment may name any branch id; the sentinel must follow it, not assume 0.
  const nr_rx_branch_set_t only_two = branch_set({2});
  require(branch_engine_index(only_two, NR_ISAC_BRANCH_NONE) == 2,
          "with one active branch the legacy engine is that branch, whatever its id");
  require(branch_engine_index(only_two, 0) == -1,
          "branch 0 is not special: unnamed means dropped");

  // With no active branch at all there is nothing to route to; nr_isac_init() refuses this case,
  // and the router must not invent an engine for it either.
  const nr_rx_branch_set_t none = branch_set({});
  require(branch_engine_index(none, NR_ISAC_BRANCH_NONE) == -1 && branch_engine_index(none, 0) == -1,
          "an empty branch set must route nothing");
}

// ---------------------------------------------------------------------------------------------
// adaptive_RX_pipeline.md P10b (AoA-removal audit Stage B item 2): the CFR producers' submission
// plan. Two properties, in order of how much damage getting them wrong would do:
//   (a) THE REGRESSION PIN -- with <= 1 active branch the plan is EXACTLY the call the producer
//       made before P10b: one submission, antenna plane 0, the producer's own antenna count
//       verbatim, branch identity explicitly ABSENT. Every producer routes through this one
//       function, so this single assertion is the structural proof for all of them.
//   (b) with several branches, one SINGLE-ANTENNA submission per branch reading THAT branch's own
//       physical channel and tagged with its branch id -- never another branch's plane, never a
//       fabricated branch 0.
// The drop-not-misroute guarantee itself is NOT re-tested here: every entry this plan produces is
// submitted through nr_isac_submit_cfr_multi_branch(), which test_branch_engine_routing() above
// already pins.
void test_branch_submit_plan()
{
  nr_isac_submit_plan_t plan[NR_RX_BRANCH_MAX];
  uint32_t pack = 0;

  // (a) No branch set at all (sensing's stub/legacy shape) and a single branch, AoA's four antennas.
  require(build_submit_plan(nullptr, plan, NR_RX_BRANCH_MAX, 4, 4, &pack) == 1
              && plan[0].first_ant == 0 && plan[0].nof_ant == 4
              && plan[0].branch_id == NR_ISAC_BRANCH_NONE && pack == 4,
          "with no branch set the plan must be the legacy untagged submission, antennas unchanged");
  for (int id : {0, 3}) {
    const nr_rx_branch_set_t one = branch_set({id});
    require(build_submit_plan(&one, plan, NR_RX_BRANCH_MAX, 4, 4, &pack) == 1
                && plan[0].first_ant == 0 && plan[0].nof_ant == 4
                && plan[0].branch_id == NR_ISAC_BRANCH_NONE && pack == 4,
            "one active branch must keep the legacy multi-antenna AoA submission, untagged");
    // The producer's own clamp is authoritative and must pass through untouched, even when it
    // exceeds what this layer would consider available -- re-clamping here would silently change
    // the live AoA path.
    require(build_submit_plan(&one, plan, NR_RX_BRANCH_MAX, 1, 4, &pack) == 1
                && plan[0].nof_ant == 1 && pack == 1,
            "the single-branch plan must carry the producer's antenna count verbatim");
  }

  // (b) Two branches on physical channels 0 and 1: one single-antenna submission each, tagged.
  const nr_rx_branch_set_t two = branch_set({0, 2});  // branch 0 -> phys 0, branch 2 -> phys 1
  require(build_submit_plan(&two, plan, NR_RX_BRANCH_MAX, 4, 4, &pack) == 2,
          "two active branches must produce one submission each");
  require(plan[0].branch_id == 0 && plan[0].first_ant == 0 && plan[0].nof_ant == 1,
          "a branch submission must be single-antenna and read its own physical channel");
  require(plan[1].branch_id == 2 && plan[1].first_ant == 1 && plan[1].nof_ant == 1,
          "the second branch must read ITS physical channel, and be tagged by branch id not ordinal");
  require(pack == 2, "the producer must pack exactly the planes the plan reads");
  require(plan[0].branch_id != NR_ISAC_BRANCH_NONE && plan[1].branch_id != NR_ISAC_BRANCH_NONE,
          "a multi-branch submission must never be untagged: untagged rows all land on one engine");

  // A branch mapped to a physical channel the producer cannot reach is SKIPPED, not folded into
  // another branch's plane and not silently read out of bounds.
  require(build_submit_plan(&two, plan, NR_RX_BRANCH_MAX, 4, 1, &pack) == 1
              && plan[0].branch_id == 0 && plan[0].first_ant == 0 && pack == 1,
          "a branch whose physical channel exceeds the producer's buffer must be dropped");
  require(build_submit_plan(&two, plan, NR_RX_BRANCH_MAX, 4, 0, &pack) == 0 && pack == 0,
          "with no reachable antenna the producer must submit nothing at all");

  // Never fan out to a silent subset -- the same rule nr_rx_branch_set_dispatch() states.
  require(build_submit_plan(&two, plan, 1, 4, 4, &pack) == -1,
          "a plan that does not fit the caller's array must fail, not drop a branch quietly");

  // A mapped-but-DISABLED slot is not a branch, and must not acquire a submission.
  nr_rx_branch_set_t half = branch_set({0, 1});
  half.b[1].state = NR_RXB_DISABLED;
  require(build_submit_plan(&half, plan, NR_RX_BRANCH_MAX, 4, 4, &pack) == 1
              && plan[0].branch_id == NR_ISAC_BRANCH_NONE,
          "with the second branch disabled the set is single-branch again: back to the legacy plan");

  // Four branches, identity map: the plan must be exhaustive and collision-free.
  const nr_rx_branch_set_t all = branch_set({0, 1, 2, 3});
  require(build_submit_plan(&all, plan, NR_RX_BRANCH_MAX, 4, 4, &pack) == 4 && pack == 4,
          "every active branch must get its own submission");
  uint32_t seen = 0;
  for (int i = 0; i < 4; ++i) {
    require(plan[i].branch_id == (uint8_t)i && plan[i].nof_ant == 1, "branch order must be by id");
    require(!(seen & (1u << plan[i].first_ant)), "no two branches may read the same antenna plane");
    seen |= 1u << plan[i].first_ant;
  }
}

// P10c: the two UL producers (nr_pusch_data_aided.c, nr_pusch_passive_decode.c) pass the passive
// gNB context's OWN allocated plane count as BOTH legacy_nof_ant and available_antennas -- that
// count is min(nb_antennas_rx, PASSIVE_UL_MAX_ANT), fixed at passive_gnb_prepare() time, not
// nb_antennas_rx itself. The hazard this pins is UL-specific: a branch mapped past it would slice
// rxdataF / ul_ch_estimates planes the context never allocated.
void test_ul_submit_plan()
{
  nr_isac_submit_plan_t plan[NR_RX_BRANCH_MAX];
  uint32_t pack = 0;

  // Legacy identity: whatever the context allocated, one untagged submission of exactly that.
  for (uint32_t nant = 1; nant <= 4; ++nant) {
    require(build_submit_plan(nullptr, plan, NR_RX_BRANCH_MAX, nant, nant, &pack) == 1
                && plan[0].first_ant == 0 && plan[0].nof_ant == nant
                && plan[0].branch_id == NR_ISAC_BRANCH_NONE && pack == nant,
            "a UL CFR submission must be untagged and keep the context antenna count at 1 branch");
  }

  // Multi-branch: per-antenna H = Y_a/X is a real per-branch measurement, so each branch reads its
  // own plane -- it is NOT attributed to the lowest branch, and NOT dropped.
  const nr_rx_branch_set_t two = branch_set({0, 2});  // branch 0 -> phys 0, branch 2 -> phys 1
  require(build_submit_plan(&two, plan, NR_RX_BRANCH_MAX, 4, 4, &pack) == 2
              && plan[0].branch_id == 0 && plan[0].first_ant == 0
              && plan[1].branch_id == 2 && plan[1].first_ant == 1,
          "each UL branch must carry its own identity and read its own antenna plane");

  // A context that allocated only 2 planes must not serve branches mapped to planes 2 and 3.
  const nr_rx_branch_set_t high = branch_set({0, 1, 2, 3});
  require(build_submit_plan(&high, plan, NR_RX_BRANCH_MAX, 2, 2, &pack) == 2 && pack == 2,
          "branches beyond the passive UL context allocated planes must be skipped, not sliced");
}

// P13b item 2: aoa_enable + several active branches is refused at CONFIG PARSE time (nr_isac.cc's
// aoa_enable block), not only by SensingEngine's constructor deep in engine construction. The
// predicate is the testable half; the LOG_E and the "return without applying anything" around it
// are the same shape as the two rx_array parse failures next to it. The single-branch pin is the
// important assertion here: the co-located-array AoA deployment must be completely unaffected.
void test_aoa_branch_conflict()
{
  const nr_rx_branch_set_t none = branch_set({});
  const nr_rx_branch_set_t one = branch_set({0});
  const nr_rx_branch_set_t one_high = branch_set({3});
  const nr_rx_branch_set_t two = branch_set({0, 2});
  const nr_rx_branch_set_t all = branch_set({0, 1, 2, 3});

  // THE REGRESSION PIN: the legacy AoA receiver (aoa_enable, one or no branch) is still accepted.
  require(!aoa_conflicts_with_branches(true, none) && !aoa_conflicts_with_branches(true, one)
              && !aoa_conflicts_with_branches(true, one_high),
          "aoa_enable at a single active branch is the co-located-array deployment: stays accepted");

  // The refused combination, at two and at four branches.
  require(aoa_conflicts_with_branches(true, two) && aoa_conflicts_with_branches(true, all),
          "aoa_enable together with several active branches must be refused at parse time");

  // Without aoa_enable a multi-branch set is the normal deployment and must never be touched.
  require(!aoa_conflicts_with_branches(false, two) && !aoa_conflicts_with_branches(false, all),
          "a multi-branch receiver without aoa_enable must not be refused");

  // A mapped-but-DISABLED slot is not a branch: the same predicate build_submit_plan() uses, so a
  // set that reads as single-branch there must read as single-branch here too.
  nr_rx_branch_set_t half = branch_set({0, 1});
  half.b[1].state = NR_RXB_DISABLED;
  require(!aoa_conflicts_with_branches(true, half),
          "with the second branch disabled the set is single-branch again: AoA stays accepted");
}

// P13b item 3: the submission-plan skip census must fire when the plan places NOTHING. The old
// condition was "written > 0 && set", so written == 0 -- every active branch naming a physical
// channel this producer cannot reach, i.e. TOTAL loss for this producer -- incremented no counter
// and logged nothing. FALSIFICATION: the first two require()s below assert 2 and 4 where the
// pre-fix expression (written > 0 ? active - written : 0) yields 0, so they fail against the old
// code; the remaining ones pin the shapes that must keep answering zero.
void test_submit_plan_skip_census()
{
  const nr_rx_branch_set_t two = branch_set({0, 2});
  const nr_rx_branch_set_t all = branch_set({0, 1, 2, 3});
  const nr_rx_branch_set_t one = branch_set({0});

  require(submit_plan_skipped(&two, 0) == 2,
          "with no reachable antenna at all BOTH active branches must be counted as skipped");
  require(submit_plan_skipped(&all, 0) == 4, "the same at four branches");

  // Partial reachability: the pre-existing behaviour, unchanged.
  require(submit_plan_skipped(&two, 1) == 1, "one placed of two active branches is one skipped");
  require(submit_plan_skipped(&two, 2) == 0, "a fully placed plan skips nothing");

  // The legacy shapes answer zero: one untagged submission IS the whole plan there.
  require(submit_plan_skipped(nullptr, 0) == 0 && submit_plan_skipped(&one, 1) == 0
              && submit_plan_skipped(&one, 0) == 0,
          "the legacy single-branch / no-set plan must never report a skipped branch");

  // A FAILED plan (does not fit the caller's array) is not a skip count: active - (-1) would be
  // arithmetic nonsense and would inflate the census by one on every such call.
  require(submit_plan_skipped(&two, -1) == 0 && submit_plan_skipped(&all, -1) == 0,
          "a plan that failed outright must not be counted as skipped branches");

  // End to end against the real plan builder: two branches, zero reachable antennas.
  nr_isac_submit_plan_t plan[NR_RX_BRANCH_MAX];
  uint32_t pack = 0;
  const int written = build_submit_plan(&two, plan, NR_RX_BRANCH_MAX, 4, 0, &pack);
  require(written == 0 && pack == 0 && submit_plan_skipped(&two, written) == 2,
          "the plan builder's own total-loss case must be censused as two skipped branches");
}

void test_branch_output_identity()
{
  PipelineConfig base;
  base.rx_id = "rx1";
  base.out_path = "/tmp/oaiue_sensing";
  base.report_path = "/tmp/sensing/reports.jsonl";
  base.report_endpoint = "tcp://*:5555";

  // (a) THE REGRESSION PIN. One active branch -> every output-identity field byte-identical.
  for (int id : {0, 3}) {
    const PipelineConfig single = branch_pipeline_config(base, branch_set({id}), (uint8_t)id);
    require(single.rx_id == base.rx_id && single.out_path == base.out_path
                && single.report_path == base.report_path
                && single.report_endpoint == base.report_endpoint,
            "a single-branch receiver must keep exactly the configured output identity, unsuffixed");
  }

  const nr_rx_branch_set_t two = branch_set({0, 1});
  const PipelineConfig a = branch_pipeline_config(base, two, 0);
  const PipelineConfig b = branch_pipeline_config(base, two, 1);
  require(a.rx_id == "rx1_b0" && b.rx_id == "rx1_b1", "each branch must report its own rx_id");
  require(a.report_path == "/tmp/sensing/reports_b0.jsonl"
              && b.report_path == "/tmp/sensing/reports_b1.jsonl",
          "branch report paths must differ, with the suffix before the extension");
  require(a.out_path == "/tmp/oaiue_sensing_b0" && b.out_path == "/tmp/oaiue_sensing_b1",
          "an extensionless prefix takes the suffix at the end");
  require(a.report_endpoint == "tcp://*:5555" && b.report_endpoint == "tcp://*:5556",
          "ZeroMQ endpoints must be offset by branch id so two engines cannot fight over one port");
  require(a.report_path != b.report_path && a.out_path != b.out_path
              && a.report_endpoint != b.report_endpoint && a.rx_id != b.rx_id,
          "no two branches may share any output identity");

  // Fix round 1 / defence in depth: a set whose n_active DISAGREES with what is actually mapped
  // must still suffix. nr_rx_branch_set_parse() now rejects this at parse (nr_rx_branch_test.cc's
  // RejectsAPhysicalMappingForAnUnnamedBranch), so it is unreachable through the config surface --
  // it is asserted here so that if that parser invariant is ever weakened again, the failure is a
  // suffixed-but-surprising path rather than two engines silently sharing one report file.
  nr_rx_branch_set_t lying = branch_set({0, 1});
  lying.n_active = 1;  // what a stale/looser parser could have produced
  require(branch_active_count(lying) == 2, "the active count must be measured, not trusted");
  require(branch_pipeline_config(base, lying, 1).report_path == "/tmp/sensing/reports_b1.jsonl"
              && branch_pipeline_config(base, lying, 0).report_path
                     != branch_pipeline_config(base, lying, 1).report_path,
          "two mapped branches must get distinct paths even when n_active claims otherwise");
  // And the routing predicate must agree with it: a mapped-but-DISABLED slot is not an engine.
  nr_rx_branch_set_t half = branch_set({0});
  half.b[1].physical_channel = 1;  // mapped, still NR_RXB_DISABLED
  require(branch_active_count(half) == 1 && branch_engine_index(half, 1) == -1,
          "a mapped but disabled branch has no engine and must not be routed to");

  // A directory containing a dot must not be mistaken for a file extension.
  PipelineConfig dotted = base;
  dotted.report_path = "/tmp/run.1/reports";
  require(branch_pipeline_config(dotted, two, 1).report_path == "/tmp/run.1/reports_b1",
          "a dot in a directory name is not an extension");
  PipelineConfig ipc = base;
  ipc.report_endpoint = "ipc:///tmp/sensing.sock";
  require(branch_pipeline_config(ipc, two, 1).report_endpoint == "ipc:///tmp/sensing.sock_b1",
          "an endpoint with no numeric port still has to become distinct");
}

// (b) Two live engines, started together, fed DIFFERENT CFR. `solo` runs the first engine alone;
// `paired` runs both. The first engine's report must be identical either way -- i.e. nothing about
// another engine's existence, submissions or CPI closure reaches it.
std::string two_engine_capture(bool with_second, const std::string& suffix)
{
  const std::string path_a = "/tmp/nr_isac_p13_a_" + suffix + ".jsonl";
  const std::string path_b = "/tmp/nr_isac_p13_b_" + suffix + ".jsonl";
  std::remove(path_a.c_str()); std::remove(path_b.c_str());
  PipelineConfig base;
  base.sources_mask = (1u << NR_ISAC_SRC_CSI_RS);
  base.duration_bank_s = {0.001}; base.bootstrap_duration_index = 0;
  base.minimum_dwell_s = 0.001; base.maximum_dwell_s = 0.001;
  base.minimum_rows = 2; base.maximum_rows = 4;
  base.sync_enable = false; base.family_static = false; base.tracker_enable = false;
  base.maximum_components = 1; base.maximum_objects = 1;
  base.maximum_range_m = 200.0; base.out_path.clear();
  PipelineConfig ca = base; ca.report_path = path_a; ca.rx_id = "rxA";
  PipelineConfig cb = base; cb.report_path = path_b; cb.rx_id = "rxB";
  nr_isac_carrier_t carrier{};
  carrier.nof_prb = 2; carrier.scs_hz = 30000; carrier.dl_center_hz = 3499440000;
  carrier.slots_per_frame = 20; carrier.pci = 1;
  std::vector<std::complex<float>> ha(24, {1.0f, 0.0f});
  std::vector<std::complex<float>> hb(24, {0.0f, 9.0f});
  std::vector<uint32_t> k(24), symbol(24, 2);
  for (uint32_t i = 0; i < k.size(); ++i) k[i] = i;
  {
    SensingEngine engine_a(ca, 2, 1);
    std::unique_ptr<SensingEngine> engine_b;
    if (with_second) engine_b = std::make_unique<SensingEngine>(cb, 2, 1);
    engine_a.start();
    if (engine_b) engine_b->start();
    for (uint32_t slot = 0; slot < 10; ++slot) {
      engine_a.submit(slot, 0.0f, NR_ISAC_SRC_CSI_RS, carrier, ha.data(), 1,
                      k.data(), symbol.data(), ha.size(), 1.0f, 0);
      // Deliberately a different slot cadence, different amplitude and a different branch tag:
      // if any of the accumulator, planner, clutter map, clock tracker or sequence counter were
      // shared, engine A's single closed CPI could not survive this unchanged.
      if (engine_b)
        for (int repeat = 0; repeat < 3; ++repeat)
          engine_b->submit(slot, 0.25f * repeat, NR_ISAC_SRC_CSI_RS, carrier, hb.data(), 1,
                           k.data(), symbol.data(), hb.size(), 4.0f, 1);
    }
    if (engine_b) engine_b->stop();
    engine_a.stop();
  }
  // NOTE: compares the FIRST report line only. These fixtures close exactly one CPI (10 slots at
  // a 1 ms bank, minimum_rows 2), so the first line IS the whole output; if a future fixture closes
  // two, this must compare the whole file or it will silently stop covering the later CPIs.
  std::ifstream input(path_a); require(input.good(), "engine A emitted no report");
  std::string line;
  require(static_cast<bool>(std::getline(input, line)), "engine A's report is empty");
  if (with_second) {
    std::ifstream other(path_b);
    std::string other_line;
    require(other.good() && std::getline(other, other_line),
            "engine B emitted nothing, so the isolation comparison would be vacuous");
    require(other_line.find("\"rx_id\":\"rxB\"") != std::string::npos
                && line.find("\"rx_id\":\"rxA\"") != std::string::npos,
            "each engine must write its own receiver identity to its own file");
    require(other_line != line, "two engines fed different CFR produced the same report");
  }
  std::remove(path_a.c_str()); std::remove(path_b.c_str());
  return line;
}

void test_branch_engines_are_independent()
{
  const std::string key = "cpi_start_time_utc_ns";
  const std::string solo = drop_json_field(two_engine_capture(false, "solo"), key);
  const std::string paired = drop_json_field(two_engine_capture(true, "paired"), key);
  require(solo == paired,
          "a second engine's submissions changed the first engine's CPI: the instances share state");

  // And the derived per-branch configuration really is what a live engine writes: the file lands at
  // the suffixed path and the line carries the suffixed rx_id. This is what ties the pure helper
  // above to nr_isac.cc's construction loop.
  PipelineConfig base;
  base.sources_mask = (1u << NR_ISAC_SRC_CSI_RS);
  base.duration_bank_s = {0.001}; base.bootstrap_duration_index = 0;
  base.minimum_dwell_s = 0.001; base.maximum_dwell_s = 0.001;
  base.minimum_rows = 2; base.maximum_rows = 4;
  base.sync_enable = false; base.family_static = false; base.tracker_enable = false;
  base.maximum_components = 1; base.maximum_objects = 1;
  base.maximum_range_m = 200.0; base.out_path.clear();
  base.rx_id = "rx1"; base.report_path = "/tmp/nr_isac_p13_derived.jsonl";
  const PipelineConfig derived = branch_pipeline_config(base, branch_set({0, 1}), 1);
  require(derived.report_path == "/tmp/nr_isac_p13_derived_b1.jsonl", "derived path");
  std::remove(derived.report_path.c_str());
  nr_isac_carrier_t carrier{};
  carrier.nof_prb = 2; carrier.scs_hz = 30000; carrier.dl_center_hz = 3499440000;
  carrier.slots_per_frame = 20; carrier.pci = 1;
  std::vector<std::complex<float>> h(24, {1.0f, 0.0f});
  std::vector<uint32_t> k(24), symbol(24, 2);
  for (uint32_t i = 0; i < k.size(); ++i) k[i] = i;
  {
    SensingEngine engine(derived, 2, 1); engine.start();
    for (uint32_t slot = 0; slot < 10; ++slot)
      engine.submit(slot, 0.0f, NR_ISAC_SRC_CSI_RS, carrier, h.data(), 1,
                    k.data(), symbol.data(), h.size(), 1.0f, 1);
    engine.stop();
  }
  std::ifstream input(derived.report_path);
  std::string line;
  require(input.good() && std::getline(input, line),
          "a branch engine wrote nothing at its derived report path");
  require(line.find("\"rx_id\":\"rx1_b1\"") != std::string::npos,
          "a branch engine must stamp its own branch-qualified receiver id");
  std::remove(derived.report_path.c_str());
}

void test_mixed_row_dl_rdm_isolation()
{
  const auto quiet = mixed_row_capture(0.25f, "/tmp/nr_isac_mixed_row_quiet.jsonl");
  const auto loud = mixed_row_capture(8.0f, "/tmp/nr_isac_mixed_row_loud.jsonl");
  require(std::abs(map_energy(quiet[2]) - map_energy(loud[2])) > 1e-4,
          "UL energy did not affect the independent UL RDM");
  require(quiet[0].size() == loud[0].size(), "primary DL RDM shape changed with UL");
  for (size_t i = 0; i < quiet[0].size(); ++i) {
    close(quiet[0][i], quiet[1][i], 1e-10,
          "primary detector input differs from provenance-clean DL input");
    close(quiet[0][i], loud[0][i], 1e-10,
          "UL energy leaked into the primary DL detector RDM");
  }
}

void test_invalid_ul_does_not_suppress_dl()
{
  const std::string path = "/tmp/nr_isac_invalid_ul.jsonl";
  std::remove(path.c_str());
  PipelineConfig c;
  c.sources_mask = (1u << NR_ISAC_SRC_CSI_RS) | (1u << NR_ISAC_SRC_PUSCH_DMRS);
  c.duration_bank_s = {0.001}; c.bootstrap_duration_index = 0;
  c.minimum_dwell_s = 0.001; c.maximum_dwell_s = 0.001;
  c.minimum_rows = 2; c.maximum_rows = 4;
  c.sync_enable = false; c.family_static = false; c.tracker_enable = false;
  c.maximum_components = 1; c.maximum_objects = 1; c.capture_rvm = true;
  c.maximum_range_m = 200.0; c.report_path = path; c.out_path.clear();
  nr_isac_carrier_t carrier{};
  carrier.nof_prb = 2; carrier.scs_hz = 30000; carrier.dl_center_hz = 3499440000;
  carrier.slots_per_frame = 20; carrier.pci = 1;
  std::vector<std::complex<float>> dl(24, {1.0f, 0.0f});
  std::vector<std::complex<float>> invalid_ul(24, {0.0f, 0.0f});
  std::vector<uint32_t> k(24), symbol(24, 2);
  for (uint32_t i = 0; i < k.size(); ++i) k[i] = i;
  {
    SensingEngine engine(c, 2, 1); engine.start();
    for (uint32_t slot = 0; slot < 10; ++slot) {
      engine.submit(slot, 0.0f, NR_ISAC_SRC_CSI_RS, carrier, dl.data(), 1,
                    k.data(), symbol.data(), dl.size(), 1.0f);
      engine.submit(slot, 0.0f, NR_ISAC_SRC_PUSCH_DMRS, carrier, invalid_ul.data(), 1,
                    k.data(), symbol.data(), invalid_ul.size(), 1.0f);
    }
    engine.stop();
  }
  std::ifstream input(path); require(input.good(), "invalid UL suppressed the DL report file");
  std::string line; require(static_cast<bool>(std::getline(input, line)),
                            "invalid UL suppressed the valid DL report");
  require(line.find("\"uplink\":{\"present\":true,\"valid\":false") != std::string::npos,
          "invalid UL is not explicitly marked invalid");
  require(line.find("\"dl_rvm_blob\":[") != std::string::npos,
          "invalid UL removed the valid DL map");
  std::remove(path.c_str());
}

void test_dl_capture_fails_closed_without_dl()
{
  const std::string path = "/tmp/nr_isac_ul_only_capture.jsonl";
  std::remove(path.c_str());
  PipelineConfig c;
  c.sources_mask = 1u << NR_ISAC_SRC_PUSCH_DMRS;
  c.duration_bank_s = {0.001}; c.bootstrap_duration_index = 0;
  c.minimum_dwell_s = 0.001; c.maximum_dwell_s = 0.001;
  c.minimum_rows = 2; c.maximum_rows = 4;
  c.sync_enable = false; c.family_static = false; c.tracker_enable = false;
  c.maximum_components = 1; c.maximum_objects = 1; c.capture_rvm = true;
  c.report_path = path; c.out_path.clear();
  nr_isac_carrier_t carrier{};
  carrier.nof_prb = 2; carrier.scs_hz = 30000; carrier.dl_center_hz = 3499440000;
  carrier.slots_per_frame = 20; carrier.pci = 1;
  std::vector<std::complex<float>> h(24, {1.0f, 0.0f});
  std::vector<uint32_t> k(24), symbol(24, 2);
  for (uint32_t i = 0; i < k.size(); ++i) k[i] = i;
  {
    SensingEngine engine(c, 2, 1); engine.start();
    for (uint32_t slot = 0; slot < 6; ++slot)
      engine.submit(slot, 0.0f, NR_ISAC_SRC_PUSCH_DMRS, carrier, h.data(), 1,
                    k.data(), symbol.data(), h.size(), 1.0f);
    engine.stop();
  }
  std::ifstream input(path);
  std::string line;
  require(!input.good() || !std::getline(input, line),
          "DL RDM capture emitted a report despite having no provenance-proven DL samples");
  std::remove(path.c_str());
}

/** P14 Stage A: the AoA environment override is removed, so the keys must be REPORTED, never
 *  honoured. Counting them (rather than only logging) is what makes the removal testable. */
void test_obsolete_aoa_env_rejected()
{
  ::unsetenv("AOA_ENABLE"); ::unsetenv("AOA_UL_ENABLE");
  require(nr_isac_obsolete_env_keys()==0,"a clean environment must report no obsolete AoA keys");
  ::setenv("AOA_ENABLE","1",1);
  require(nr_isac_obsolete_env_keys()==1,"AOA_ENABLE must be reported obsolete, not honoured");
  ::setenv("AOA_UL_ENABLE","0",1);
  require(nr_isac_obsolete_env_keys()==2,"AOA_UL_ENABLE must be reported obsolete even when zero");
  ::unsetenv("AOA_ENABLE"); ::unsetenv("AOA_UL_ENABLE");
  require(nr_isac_obsolete_env_keys()==0,"obsolete AoA key reporting must not be sticky");
}
}

int main()
{
  try {test_fft();test_adaptive_threshold();test_detector();test_required_cuda_contract();test_aoa();test_aoa_component_mixture_and_cross_leg_fusion();test_enu_geometry();test_repeated_ul_confirmation_gates_global_birth();test_variable_cpi();test_causal_cpi_pipeline();test_finite_admission_window();test_mixed_row_dl_rdm_isolation();test_invalid_ul_does_not_suppress_dl();test_dl_capture_fails_closed_without_dl();test_validation_report_compatibility();test_branch_identity_report();test_branch_engine_routing();test_branch_submit_plan();test_ul_submit_plan();test_aoa_branch_conflict();test_submit_plan_skip_census();test_branch_output_identity();test_branch_engines_are_independent();test_obsolete_aoa_env_rejected();}
  catch(const std::exception& e){std::fprintf(stderr,"python parity test failed: %s\n",e.what());return EXIT_FAILURE;}
  std::puts("native sensing golden parity checks passed");return EXIT_SUCCESS;
}
