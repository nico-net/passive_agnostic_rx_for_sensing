/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_csirs_disc_synth.h"
#include "nr_csirs_disc_gate_internal.h"
#include <random>
#include <complex>
#include <cstring>
#include <string>
#include <array>
#include <unistd.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

extern "C" {
#include "nfapi/open-nFAPI/nfapi/public_inc/fapi_nr_ue_interface.h"
void *uniqCfg = nullptr;
uint32_t *nr_gold_pdsch(int, int, int, int, int, int);
void exit_function(const char *, const char *, int, const char *message, int)
{
  std::fprintf(stderr, "%s\n", message ? message : "");
  std::abort();
}
int nr_csirs_blind_rt_test_confirm_ring(const nr_csirs_disc_ring_t *,
                                        fapi_nr_dl_config_csirs_pdu_rel15_t *,
                                        uint16_t *,
                                        uint16_t *,
                                        int16_t *,
                                        int,
                                        uint32_t *);
int nr_csirs_blind_rt_test_candidate_count(void);
}

namespace {
struct Ring {
  nr_csirs_disc_ring_t ring = {};
  ~Ring() { nr_csirs_disc_synth_free(&ring); }
};

nr_csirs_disc_synth_cfg_t config(const nr_csirs_disc_synth_resource_t *resources, int n_resources)
{
  return {.n_slots = 640,
          .n_rb = 217,
          .n_ant = 4,
          .slots_per_frame = 20,
          .first_slot = 1000,
          .seed = 0x5a17c5d,
          .snr_db = 10,
          .load = 1,
          .dmrs = true,
          .ssb = true,
          .full_rank_mimo = true,
          .resources = resources,
          .n_resources = n_resources};
}

bool exact(const fapi_nr_dl_config_csirs_pdu_rel15_t &p,
           uint16_t period,
           uint16_t offset,
           const nr_csirs_disc_synth_resource_t &r)
{
  return p.row == r.row && p.freq_domain == r.fd_bitmap && p.symb_l0 == r.l0 && p.symb_l1 == r.l1
      && p.cdm_type == r.cdm && p.freq_density == r.density && p.start_rb == r.rb_start
      && p.nr_of_rbs == r.n_rb && p.scramb_id == r.scramb_id && period == r.period && offset == r.offset;
}
} // namespace

TEST(CsirsDiscE2E, DisabledPipelineIsByteIdentical)
{
  // The enable bit is process-lifetime state. Use a fresh death-test child.
  ::testing::FLAGS_gtest_death_test_style="threadsafe";
  EXPECT_EXIT(([] {
    unsetenv("ISAC_CSIRS_DISC");
    nr_csirs_disc_ring_t ring={};
    nr_csirs_disc_slot_meta_t meta={};
    std::vector<int16_t> samples(4*14*24*8,12345);
    ring.n_ant=4; ring.n_rb=8; ring.capacity=1; ring.slots_per_frame=20;
    ring.slot_stride=samples.size(); ring.grid_base=samples.data(); ring.meta=&meta;
    std::array<unsigned char,sizeof(ring)> before;
    memcpy(before.data(),&ring,sizeof(ring));
    const auto iq_before=samples;
    nr_csirs_disc_metrics_t a={},b={};
    nr_csirs_disc_get_metrics(&a);
    nr_csirs_disc_pipeline_init(nullptr);
    nr_csirs_disc_process(&ring);
    nr_csirs_disc_get_metrics(&b);
    EXPECT_EQ(memcmp(before.data(),&ring,sizeof(ring)),0);
    EXPECT_EQ(memcmp(&a,&b,sizeof(a)),0);
    EXPECT_EQ(samples,iq_before);
    EXPECT_EQ(meta.valid,0);
    _exit(::testing::Test::HasFailure()?1:0);
  }()), ::testing::ExitedWithCode(0), "");
}

TEST(CsirsDiscE2E, ANoConfirmationOnDmrsOnlyCell)
{
  setenv("ISAC_CSIRS_DISC", "1", 1);
  setenv("ISAC_CSIRS_DISC_BACKEND", "cpu", 1);
  setenv("ISAC_CSIRS_DISC_SWEEP_HYPS", "32", 1);
  nr_csirs_disc_synth_init();
  Ring r;
  auto cfg = config(nullptr, 0);
  cfg.n_slots = 160;
  ASSERT_EQ(nr_csirs_disc_synth(&r.ring, &cfg), 0);
  // Add a strong TDD power cycle without adding any CSI-RS sequence.
  for(int t=0;t<r.ring.capacity;++t) if(r.ring.meta[t].abs_slot%10>=5)
    for(size_t i=0;i<r.ring.slot_stride;++i) r.ring.grid_base[t*r.ring.slot_stride+i]/=8;
  nr_csirs_disc_process(&r.ring);
  EXPECT_EQ(nr_csirs_blind_rt_test_candidate_count(), 0);
}

TEST(CsirsDiscE2E, MixedRowsDiscoverConfirmAndExportExact)
{
  setenv("ISAC_CSIRS_DISC", "1", 1);
  setenv("ISAC_CSIRS_DISC_BACKEND", "cpu", 1);
  setenv("ISAC_CSIRS_DISC_SWEEP_HYPS", "64", 1);
  nr_csirs_disc_synth_init();
  nr_csirs_disc_synth_resource_t resources[] = {
      {.row = 1, .l0 = 4, .l1 = 0, .cdm = 0, .density = 3, .fd_bitmap = 1, .rb_start = 0,
       .n_rb = 52, .scramb_id = 415, .period = 10, .offset = 1, .power = 1.6},
      {.row = 2, .l0 = 2, .l1 = 0, .cdm = 0, .density = 2, .fd_bitmap = 8, .rb_start = 60,
       .n_rb = 24, .scramb_id = 101, .period = 5, .offset = 2, .power = 3.0},
      {.row = 4, .l0 = 6, .l1 = 0, .cdm = 1, .density = 2, .fd_bitmap = 2, .rb_start = 90,
       .n_rb = 28, .scramb_id = 202, .period = 8, .offset = 3, .power = 10.0},
      {.row = 6, .l0 = 8, .l1 = 0, .cdm = 1, .density = 2, .fd_bitmap = 15, .rb_start = 120,
       .n_rb = 28, .scramb_id = 303, .period = 10, .offset = 4, .power = 10.0},
      {.row = 11, .l0 = 10, .l1 = 0, .cdm = 1, .density = 2, .fd_bitmap = 15, .rb_start = 150,
       .n_rb = 28, .scramb_id = 404, .period = 5, .offset = 1, .power = 3.0},
      {.row = 16, .l0 = 4, .l1 = 8, .cdm = 1, .density = 2, .fd_bitmap = 15, .rb_start = 180,
       .n_rb = 28, .scramb_id = 505, .period = 8, .offset = 5, .power = 3.0},
  };
  Ring r;
  auto cfg = config(resources, 6);
  ASSERT_EQ(nr_csirs_disc_synth(&r.ring, &cfg), 0);

  const auto begin = std::chrono::steady_clock::now();
  nr_csirs_disc_process(&r.ring);
  const double batch_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
  nr_csirs_disc_metrics_t metrics = {};
  nr_csirs_disc_get_metrics(&metrics);
  std::printf("pipeline footprints=%llu hyps=%llu winners=%llu ambiguous=%llu\n",
              (unsigned long long)metrics.footprints,
              (unsigned long long)metrics.hyps,
              (unsigned long long)metrics.winners,
              (unsigned long long)metrics.ambiguous);
  ASSERT_EQ(nr_csirs_blind_rt_test_candidate_count(), 6);

  fapi_nr_dl_config_csirs_pdu_rel15_t pdu[8] = {};
  uint16_t period[8] = {}, offset[8] = {};
  int16_t delta[8] = {};
  uint32_t slots = 0;
  const int n = nr_csirs_blind_rt_test_confirm_ring(&r.ring, pdu, period, offset, delta, 8, &slots);
  ASSERT_EQ(n, 6);
  for(int k=0;k<n;++k) printf("CONF row=%u fd=%u l=%u rb=%u:%u id=%u P/O=%u/%u\n",pdu[k].row,pdu[k].freq_domain,pdu[k].symb_l0,pdu[k].start_rb,pdu[k].nr_of_rbs,pdu[k].scramb_id,period[k],offset[k]);
  for (const auto &resource : resources) {
    bool found = false;
    for (int k = 0; k < n; ++k)
      found |= exact(pdu[k], period[k], offset[k], resource) && delta[k] == 0;
    EXPECT_TRUE(found) << "row=" << static_cast<int>(resource.row);
  }
  EXPECT_GE(metrics.winners, 6u);
  EXPECT_GE(metrics.cpu_batches, 1u);
  std::printf("CSI-RS discovery CPU batch %.3f ms; slots-to-confirm=%u\n", batch_ms, slots);
}

// Explicit opt-in: Monte Carlo calibration is deliberately outside ordinary ctest.
static void calibrate_gate(bool loaded)
{
  setenv("ISAC_CSIRS_DISC", "1", 1);
  nr_csirs_disc_synth_init();
  const int trials=getenv("CSD_T12_TRIALS")?atoi(getenv("CSD_T12_TRIALS")):10000;
  ASSERT_GT(trials,0);
  const unsigned seed=getenv("CSD_T12_SEED")?strtoul(getenv("CSD_T12_SEED"),nullptr,0):0x12c5d415;
  std::mt19937 random(seed);
  const auto gate=nr_csirs_disc_gate_config();
  unsigned false_winners=0;
  double max_standard=0, max_margin=0, max_z=0;
  for(int i=0;i<trials;++i) {
    const int rows[]={1,2,4,6,11,16};
    int row=rows[i%6], slots=1+random()%8, nrb=8+random()%89, start=random()%24;
    auto cfg=config(nullptr,0);
    cfg.n_slots=slots; cfg.n_rb=start+nrb; cfg.seed=random();
    cfg.load=loaded?1:0; cfg.dmrs=loaded; cfg.ssb=false;
    cfg.snr_db=loaded?10:0;
    cfg.first_slot=random()%10000;
    Ring r; ASSERT_EQ(nr_csirs_disc_synth(&r.ring,&cfg),0);
    // Reindex independent noise/traffic occasions onto a consistent period-5
    // lattice. No CSI-RS is transmitted; traffic has no CSI-RS slot initializer.
    for(int s=0;s<slots;++s) {
      unsigned abs=20*(cfg.first_slot/20)+2+5*s;
      r.ring.meta[s]={abs,uint16_t((abs/20)%1024),uint8_t(abs%20),1};
    }
    if(loaded) {
      // Replace the generator's structural DM-RS comb with OAI's actual type-1
      // Gold/QPSK sequence (port 1000, nSCID=0), after lattice reindexing. Full
      // QPSK data remains on all other REs; the four branches share the signal
      // through distinct flat channels and have independent 10 dB AWGN.
      std::normal_distribution<double> noise(0,2048*std::pow(10.,-cfg.snr_db/20)/std::sqrt(2.));
      for(int slot=0;slot<slots;++slot) for(int l:{2,11}) {
        const uint32_t *gold=nr_gold_pdsch(cfg.n_rb,14,cfg.seed%1024,0,r.ring.meta[slot].slot,l);
        for(int ant=0;ant<4;++ant) {
          auto channel=std::polar(0.85+0.1*ant,0.73*ant);
          auto *dst=nr_csirs_disc_ring_sym(&r.ring,slot,ant,l);
          for(int k=0;k<12*cfg.n_rb;k+=2) {
            unsigned bits=(gold[(k/2)/16]>>(2*((k/2)%16)))&3;
            auto value=channel*std::complex<double>(bits&1?-1:1,bits&2?-1:1)*(2048*std::sqrt(1.9/2));
            dst[2*k]=std::clamp(std::lround(value.real()+noise(random)),-32768L,32767L);
            dst[2*k+1]=std::clamp(std::lround(value.imag()+noise(random)),-32768L,32767L);
          }
        }
      }
    }
    nr_csirs_disc_hyp_t h={};
    h.row=row; h.freq_domain=row>=6?15:1;
    h.cdm_type=row>=4?1:0; h.density=row==1?3:2;
    h.symb_l0=loaded && i%2?2:4; h.symb_l1=9;
    h.rb_start=start; h.n_rb=nrb; h.period=5; h.offset=2;
    nr_csirs_disc_sweep_cfg_t sweep={0,1024,1,32};
    nr_csirs_disc_win_t w={};
    ASSERT_EQ(nr_csirs_disc_sweep_cpu(&r.ring,&h,1,&sweep,&w),1);
    ASSERT_GT(w.noise_sigma,0);
    double standard=(w.z-w.noise_mu)/w.noise_sigma;
    double margin=(w.z-w.z_second)/w.noise_sigma;
    max_standard=std::max(max_standard,standard); max_margin=std::max(max_margin,margin);
    max_z=std::max(max_z,double(w.z));
    false_winners+=nr_csirs_disc_gate_accept(&w,&gate);
    if((i+1)%1000==0) {printf("T12 progress load=%d trials=%d false=%u max_z=%.6f max_standard=%.6f max_margin=%.6f\n",
      loaded,i+1,false_winners,max_z,max_standard,max_margin);fflush(stdout);}
  }
  printf("T12 CALIBRATION load=%d seed=%u trials=%d false=%u rate=%.9g K=%.3f M=%.3f floor=%.3f max_z=%.6f max_standard=%.6f max_margin=%.6f\n",
    loaded,seed,trials,false_winners,double(false_winners)/trials,gate.k,gate.m,gate.floor,max_z,max_standard,max_margin);
  EXPECT_LE(double(false_winners)/trials,1e-4);
}
TEST(CsirsDiscE2E, DISABLED_T12NoiseCalibration) {calibrate_gate(false);}
TEST(CsirsDiscE2E, DISABLED_T12LoadedCalibration) {calibrate_gate(true);}

extern "C" {
void nr_csirs_blind_rt_test_discovery_reset(void);
int nr_csirs_blind_rt_add_discovered(const nr_csirs_disc_hyp_t *, const nr_csirs_disc_win_t *);
int nr_csirs_blind_rt_test_discovery_feed(int, uint32_t, double);
int nr_csirs_blind_rt_test_discovered(int, fapi_nr_dl_config_csirs_pdu_rel15_t *, uint16_t *, uint16_t *);
extern uint64_t csirs_disc_readmit_skipped;
}
TEST(CsirsDiscE2E, ReadmissionMergesPeriodExtentAndLatePhase)
{
  setenv("ISAC_CSIRS_DISC", "1", 1);
  nr_csirs_disc_synth_init();
  nr_csirs_blind_rt_test_discovery_reset();
  nr_csirs_disc_hyp_t h = {};
  h.row=1; h.freq_domain=2; h.symb_l0=5; h.density=3;
  h.rb_start=144; h.n_rb=32; h.period=160; h.offset=16;
  nr_csirs_disc_win_t w = {}; w.scramb_id=380;
  ASSERT_EQ(nr_csirs_blind_rt_add_discovered(&h,&w),0);
  const uint64_t before=csirs_disc_readmit_skipped;
  for (unsigned slot : {16u,176u,336u}) nr_csirs_blind_rt_test_discovery_feed(0,slot,5);
  for (int p : {80,160,320,640}) {
    h.period=p; h.offset=16;
    for (int repeat=0;repeat<12;++repeat) EXPECT_EQ(nr_csirs_blind_rt_add_discovered(&h,&w),0);
  }
  h.period=160; h.offset=15; h.rb_start=132; h.n_rb=48;
  EXPECT_EQ(nr_csirs_blind_rt_add_discovered(&h,&w),0);
  h.rb_start=128; h.n_rb=52;
  EXPECT_EQ(nr_csirs_blind_rt_add_discovered(&h,&w),0);
  ASSERT_EQ(nr_csirs_blind_rt_test_candidate_count(),1);
  EXPECT_EQ(csirs_disc_readmit_skipped-before,50u);
  fapi_nr_dl_config_csirs_pdu_rel15_t p = {}; uint16_t period=0, phases[2]={};
  EXPECT_EQ(nr_csirs_blind_rt_test_discovered(0,&p,&period,phases),2);
  EXPECT_EQ(p.start_rb,128); EXPECT_EQ(p.nr_of_rbs,52); EXPECT_EQ(period,80);
  // A non-nested two-RB boundary shift and a weaker incompatible burst also
  // reuse this physical index; neither expands an unverified extent.
  h.rb_start=130;
  EXPECT_EQ(nr_csirs_blind_rt_add_discovered(&h,&w),0);
  h.rb_start=128; h.offset=20;
  EXPECT_EQ(nr_csirs_blind_rt_add_discovered(&h,&w),0);
  EXPECT_EQ(nr_csirs_blind_rt_test_discovered(0,&p,&period,phases),2);
  EXPECT_EQ(p.start_rb,128); EXPECT_EQ(p.nr_of_rbs,52); EXPECT_EQ(period,80);
  EXPECT_EQ(csirs_disc_readmit_skipped-before,52u);
  h.offset=15;
  // Different ID, disjoint extent, slot correction and non-dividing lattice
  // must remain independent candidates.
  ++w.scramb_id; EXPECT_EQ(nr_csirs_blind_rt_add_discovered(&h,&w),1);
  --w.scramb_id; h.rb_start=0; EXPECT_EQ(nr_csirs_blind_rt_add_discovered(&h,&w),2);
  h.rb_start=128; w.slot_delta=1; EXPECT_EQ(nr_csirs_blind_rt_add_discovered(&h,&w),3);
  w.slot_delta=0; h.period=64; EXPECT_EQ(nr_csirs_blind_rt_add_discovered(&h,&w),4);
  nr_csirs_blind_rt_test_discovery_reset();
}

extern "C" {
#include "nr_csirs_monitor.h"
}
TEST(CsirsDiscE2E, RefinementReplacesDynamicMonitorLatticeAndExtent)
{
  nr_csirs_blind_rt_test_discovery_reset();
  nr_csirs_disc_hyp_t h = {};
  h.row=1; h.freq_domain=4; h.symb_l0=8; h.density=3;
  h.rb_start=132; h.n_rb=48; h.period=320; h.offset=16;
  nr_csirs_disc_win_t w = {}; w.scramb_id=713;
  ASSERT_EQ(nr_csirs_blind_rt_add_discovered(&h,&w),0);
  for (unsigned slot : {16u,336u,656u}) nr_csirs_blind_rt_test_discovery_feed(0,slot,5);
  nr_csirs_disc_ring_t ring={}; nr_csirs_disc_slot_meta_t meta={};
  std::vector<int16_t> iq(14*24*217,0);
  ring.n_ant=1; ring.n_rb=217; ring.slots_per_frame=20; ring.capacity=1;
  ring.grid_base=iq.data(); ring.slot_stride=iq.size(); ring.meta=&meta;
  fapi_nr_dl_config_csirs_pdu_rel15_t out[32]={}; uint16_t periods[32]={};
  meta.valid=1; meta.abs_slot=1000; meta.slot=0;
  ASSERT_EQ(nr_csirs_blind_rt_test_confirm_ring(&ring,out,periods,nullptr,nullptr,32,nullptr),1);
  EXPECT_EQ(periods[0],320); EXPECT_EQ(out[0].nr_of_rbs,48);
  h.period=80; h.offset=16; h.rb_start=128; h.n_rb=52;
  ASSERT_EQ(nr_csirs_blind_rt_add_discovered(&h,&w),0);
  h.offset=15; ASSERT_EQ(nr_csirs_blind_rt_add_discovered(&h,&w),0);
  for (unsigned k=8;k<12;++k) {
    nr_csirs_blind_rt_test_discovery_feed(0,k*160+15,5);
    nr_csirs_blind_rt_test_discovery_feed(0,k*160+16,5);
  }
  meta.abs_slot=2000;
  ASSERT_EQ(nr_csirs_blind_rt_test_confirm_ring(&ring,out,periods,nullptr,nullptr,32,nullptr),1);
  EXPECT_EQ(periods[0],160); EXPECT_EQ(out[0].nr_of_rbs,52);
  for (unsigned slot : {2095u,2096u,2255u,2256u}) {
    const fapi_nr_dl_config_csirs_pdu_rel15_t *due[64]={};
    int n=nr_csirs_monitor_due_ex(slot/20,slot%20,20,due,nullptr,64), found=0;
    for (int i=0;i<n;++i) if (due[i]->scramb_id==713 && due[i]->freq_domain==4 && due[i]->symb_l0==8) {
      ++found; EXPECT_EQ(due[i]->start_rb,128); EXPECT_EQ(due[i]->nr_of_rbs,52);
    }
    EXPECT_EQ(found,1);
  }
  nr_csirs_blind_rt_test_discovery_reset();
}
