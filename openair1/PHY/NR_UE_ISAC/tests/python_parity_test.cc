/* SPDX-License-Identifier: OAI-Public-License-1.1 */
/** Deterministic golden checks against the Python reference named in ../README.md. */
#include "adaptive_threshold.h"
#include "aoa.h"
#include "detector.h"
#include "enu_tracker.h"
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
}

int main()
{
  try {test_fft();test_adaptive_threshold();test_detector();test_aoa();test_enu_geometry();test_variable_cpi();test_validation_report_compatibility();test_causal_cpi_pipeline();}
  catch(const std::exception& e){std::fprintf(stderr,"python parity test failed: %s\n",e.what());return EXIT_FAILURE;}
  std::puts("native sensing golden parity checks passed");return EXIT_SUCCESS;
}
