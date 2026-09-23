/* SPDX-License-Identifier: OAI-Public-License-1.1 */
/** Deterministic golden checks against the Python reference named in ../README.md. */
#include "adaptive_threshold.h"
#include "aoa.h"
#include "cross_leg_fusion.h"
#include "clean_detector.h"
#include "detector_cuda.h"
#include "enu_tracker.h"
#include "hierarchical_tracker.h"
#include "nr_isac.h"
#include "nr_isac_ssb_axis.h"
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
#include <limits>
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

// Fixed-seed LCG + Box-Muller, deterministic across runs/platforms (no <random> engine-specific
// output). Same shape as detector_cuda_benchmark.cc's dense_window() noise, reused here so
// CLEAN's CFAR background statistic sees a genuine (not degenerate/noiseless) population -- P17.
double seeded_gaussian(uint32_t& state)
{
  auto next_uniform = [&]() {
    state = 1664525u * state + 1013904223u;
    return std::max((state >> 8) / 16777216.0, 1e-12);
  };
  const double u1 = next_uniform(), u2 = next_uniform();
  return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * PI * u2);
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
  const double noise_sigma=0.05;
  uint32_t noise_state=0xC0FFEEu;
  for(uint32_t r=0;r<w.rows;++r){w.row_time_slots[r]=r;w.row_slot_idx[r]=r;
    const double time=r*slot_duration_s(w.scs_hz);
    for(uint32_t k=0;k<w.subcarriers;++k){
      const double phase=-2.0*PI*range_bin*k/w.subcarriers
                         -2.0*PI*rate*w.fc_hz*time/C_MPS;
      for(uint32_t a=0;a<4;++a){
        const std::complex<double> signal=std::polar(1.0,phase+gains[a]);
        const std::complex<double> noise(noise_sigma*seeded_gaussian(noise_state),
                                         noise_sigma*seeded_gaussian(noise_state));
        w.values[w.sample(a,r,k)]=static_cast<std::complex<float>>(signal+noise);
      }
    }}
  return w;
}

// canonical CLEAN (clean_detector.h) accepts one independent receiver; split spatial RF channels
// first, mirroring sensing_engine.cc's independent_receiver_view().
CfrWindow receiver_view(const CfrWindow& input, uint32_t receiver)
{
  CfrWindow output = input;
  output.antennas = 1;
  const size_t cells = static_cast<size_t>(input.rows) * input.subcarriers;
  output.values.assign(input.values.begin() + receiver * cells,
                       input.values.begin() + (receiver + 1) * cells);
  return output;
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
  // P17: 300 m (16 range bins @ 512 sc/30 kHz) leaves too few CFAR training cells after the
  // detector's own guard exclusion (>=33 bins needed here; see clean_detector.cc's P17(d)
  // warning) -- every CPI reads as noise before any component is even scored. 700 m (36 bins)
  // clears that floor. The noiseless fixture also made CLEAN's background statistic degenerate;
  // fractional_component() now injects seeded Gaussian noise for a genuine population.
  PipelineConfig config;
  config.maximum_range_m=700.0;config.maximum_target_speed_mps=50.0;
  config.false_object_intensity_per_s=1.0;
  const double injected_range_bin=12.25,injected_doppler_bin=32.3125;
  const auto result=detect_clean(
      receiver_view(fractional_component(injected_range_bin,injected_doppler_bin),0),config);
  require(result.components.size()==1,"CLEAN failed to return exactly one component");
  const auto& component=result.components[0];
  close(component.range_bin,injected_range_bin,result.psf_range_halfwidth_bins+0.5,
        "CLEAN range must locate the injected target within PSF tolerance");
  close(component.doppler_bin,injected_doppler_bin,result.psf_doppler_halfwidth_bins+0.5,
        "CLEAN Doppler must locate the injected target within PSF tolerance");
  require(std::isfinite(component.local.z)&&component.local.z>component.local_threshold,
          "admitted component must clear its own reported CFAR threshold with a finite z");

  const double zero_range_bin=8.5,zero_doppler_bin=32.0;
  const auto zero_doppler=detect_clean(
      receiver_view(fractional_component(zero_range_bin,zero_doppler_bin),0),config);
  require(zero_doppler.components.size()==1,"zero-Doppler CUT was incorrectly notched");
  const auto& zero_component=zero_doppler.components[0];
  close(zero_component.range_bin,zero_range_bin,zero_doppler.psf_range_halfwidth_bins+0.5,
        "zero-Doppler range must locate the injected target within PSF tolerance");
  close(zero_component.doppler_bin,zero_doppler_bin,zero_doppler.psf_doppler_halfwidth_bins+0.5,
        "zero-Doppler bin must remain searchable");
  require(std::isfinite(zero_component.local.z)&&zero_component.local.z>zero_component.local_threshold,
          "zero-Doppler component must clear its own reported CFAR threshold with a finite z");
}

void test_required_cuda_contract()
{
  if (detector_cuda_available()) return;
  PipelineConfig config;
  config.false_object_intensity_per_s=1.0;
  setenv("NR_ISAC_REQUIRE_CUDA", "1", 1);
  bool rejected = false;
  try {
    (void)detect_clean(receiver_view(fractional_component(), 0), config);
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
  const std::string json=build_report_json(r,c,c.capture_rvm);
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
  const std::string capture_json=build_report_json(r,captured,captured.capture_rvm);
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
  c.maximum_range_m=200.0;c.maximum_target_speed_mps=50.0;c.false_object_intensity_per_s=50.0;
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
  c.maximum_range_m=200.0;c.maximum_target_speed_mps=50.0;c.false_object_intensity_per_s=50.0;
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
  c.capture_rvm = true; c.false_object_intensity_per_s = 50.0;
  c.maximum_range_m = 200.0; c.maximum_target_speed_mps = 50.0;
  c.report_path = path; c.out_path.clear();
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
                    k.data(), symbol.data(), ul.size(), 1.0f, 42);
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
  c.capture_rvm = true; c.false_object_intensity_per_s = 50.0;
  c.maximum_range_m = 200.0; c.maximum_target_speed_mps = 50.0;
  c.report_path = path; c.out_path.clear();
  nr_isac_carrier_t carrier{};
  carrier.nof_prb = 2; carrier.scs_hz = 30000; carrier.dl_center_hz = 3499440000;
  carrier.slots_per_frame = 20; carrier.pci = 1;
  std::vector<std::complex<float>> dl(24, {1.0f, 0.0f});
  // P17(b): an all-zero UL channel is a well-formed (if degenerate) CFR -- the UL pipeline runs
  // to completion without throwing, so SensingEngine reports uplink_valid=true for it (see the
  // prior round's finding). Non-finite samples are the engine's actual invalid path: they make
  // every downstream covariance/statistic computation non-finite, which the UL try/catch in
  // sensing_engine.cc genuinely throws on ("CPI has no positive covariance samples"), leaving
  // uplink_valid at its default false.
  const std::complex<float> nan_sample{std::numeric_limits<float>::quiet_NaN(),
                                       std::numeric_limits<float>::quiet_NaN()};
  std::vector<std::complex<float>> invalid_ul(24, nan_sample);
  std::vector<uint32_t> k(24), symbol(24, 2);
  for (uint32_t i = 0; i < k.size(); ++i) k[i] = i;
  {
    SensingEngine engine(c, 2, 1); engine.start();
    for (uint32_t slot = 0; slot < 10; ++slot) {
      engine.submit(slot, 0.0f, NR_ISAC_SRC_CSI_RS, carrier, dl.data(), 1,
                    k.data(), symbol.data(), dl.size(), 1.0f);
      engine.submit(slot, 0.0f, NR_ISAC_SRC_PUSCH_DMRS, carrier, invalid_ul.data(), 1,
                    k.data(), symbol.data(), invalid_ul.size(), 1.0f, 42);
    }
    engine.stop();
  }
  std::ifstream input(path); require(input.good(), "invalid UL suppressed the DL report file");
  std::string line; require(static_cast<bool>(std::getline(input, line)),
                            "invalid UL suppressed the valid DL report");
  // Two separate substring checks, not one contiguous literal: pusch_session_id (added by the
  // prior round's UL session_id fix) now sits between "present" and "valid" in the JSON field
  // order, which a single fixed literal would miss without meaning anything about validity.
  require(line.find("\"uplink\":{\"present\":true") != std::string::npos,
          "invalid UL suppressed the uplink object entirely");
  require(line.find("\"valid\":false") != std::string::npos,
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
  c.capture_rvm = true; c.false_object_intensity_per_s = 50.0;
  c.maximum_range_m = 200.0; c.maximum_target_speed_mps = 50.0;
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

/** P11fix: the SSB CFR producer's k_abs axis. Every case here is chosen so the OLD code
 *  (wrap modulo ofdm_symbol_size, base = ((start - k_ssb)/12)*12) and the new code DISAGREE --
 *  a non-regression check on values where the two conventions coincide would prove nothing. */
void test_ssb_k_abs()
{
  std::vector<uint32_t> k(NR_ISAC_SSB_NOF_RE);
  const int prb = 273, grid = prb * 12; /* 3276, vs an FFT size of 4096 at this bandwidth */

  /* P11-A1, the wrap boundary. start=3200 puts REs 76..239 past the end of the carrier grid but
   * still well inside the FFT size, so the OLD modulus returned 3276..3439 -- outside the range
   * nr_isac.h:85-86 declares, where sensing_engine.cc:543 drops them one RE at a time in silence. */
  nr_isac_ssb_k_abs(3200, grid, k.data());
  require(k[0] == 3200, "SSB k_abs must start at ssb_start_subcarrier");
  require(k[75] == 3275, "SSB k_abs must reach the last subcarrier of the grid before wrapping");
  require(k[76] == 0, "SSB k_abs must wrap modulo nof_prb*12, not modulo the FFT size");
  require(k[239] == 163, "SSB k_abs must stay wrapped after the grid boundary");
  for (uint32_t i = 0; i < k.size(); ++i)
    require(k[i] < (uint32_t)grid, "every SSB k_abs must lie in [0, nof_prb*12)");

  /* P11-A4, the kSSB term. This deployment's own fixture runs --ssb 150 == 12*12 + 6, so the old
   * CRB flooring labelled element 0 as 144 while the estimate at element 0 is the one at 150. */
  nr_isac_ssb_k_abs(150, grid, k.data());
  require(k[0] == 150, "SSB k_abs must not floor to the containing CRB");
  require(k[239] == 389, "SSB k_abs must be 240 contiguous subcarriers from ssb_start_subcarrier");

  /* Boundary case for the REJECTED `k_ssb = ssb_start_subcarrier % 12` recovery: FR1 with
   * scs_common = 15 kHz leaves k_SSB unshifted (nr_phy_common.c:482-483, `ssb_sco >> scs` with
   * scs = 0), so its post-shift value reaches 23 -- the max of the 0..23 FR1 range
   * (TS 38.211 7.4.3.1; the `< 24` guard at nr_ue_dci_configuration.c:502). `% 12` would have
   * recovered 11, not 23. The axis must be independent of that distinction entirely. */
  const int start_k23 = 12 * 20 + 23;
  nr_isac_ssb_k_abs(start_k23, grid, k.data());
  require(k[0] == (uint32_t)start_k23, "SSB k_abs must be independent of kSSB even at kSSB=23");

  /* Defensive contract: a bad bandwidth must leave the caller's buffer untouched, never divide. */
  k.assign(k.size(), 0xDEADBEEFu);
  nr_isac_ssb_k_abs(150, 0, k.data());
  require(k[0] == 0xDEADBEEFu, "a non-positive carrier bandwidth must write nothing");
}

}

int main()
{
  try {test_fft();test_adaptive_threshold();test_detector();test_required_cuda_contract();test_aoa();test_aoa_component_mixture_and_cross_leg_fusion();test_enu_geometry();test_repeated_ul_confirmation_gates_global_birth();test_variable_cpi();test_causal_cpi_pipeline();test_finite_admission_window();test_mixed_row_dl_rdm_isolation();test_invalid_ul_does_not_suppress_dl();test_dl_capture_fails_closed_without_dl();test_validation_report_compatibility();test_ssb_k_abs();}
  catch(const std::exception& e){std::fprintf(stderr,"python parity test failed: %s\n",e.what());return EXIT_FAILURE;}
  std::puts("native sensing golden parity checks passed");return EXIT_SUCCESS;
}
