#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <cstdio>
#include <random>
#include <vector>
#include <gtest/gtest.h>
extern "C" {
#include "nr_pusch_ul_disc.h"
#include "nr_pdcch_blind_monitor.h"
#include "openair2/LAYER2/NR_MAC_COMMON/nr_mac_common.h"
#include "executables/softmodem-common.h"
#include "common/utils/LOG/log.h"
#include "common/config/config_userapi.h"
uint32_t *nr_gold_pdsch(int, int, int, int, int, int);
int nr_pdsch_dmrs_rx(int, const unsigned int *, c16_t *, unsigned short, unsigned char,
                     unsigned short, uint8_t, int16_t);
configmodule_interface_t *uniqCfg = nullptr;
void exit_function(const char *, const char *, int, const char *, int) { std::abort(); }
softmodem_params_t *get_softmodem_params(void) { static softmodem_params_t p; return &p; }
}

namespace {
constexpr int fft=2048, nrb=106, symbols=14, first=fft-12*nrb/2;
constexpr int rb=12, width=40, slot=7;
struct Fixture {
  std::vector<c16_t> data[4];
  nr_pusch_ul_disc_grid_t grid{};
  explicit Fixture(unsigned seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> noise(0, 16);
    grid.fft=fft; grid.n_rb_carrier=nrb; grid.first_carrier=first;
    grid.rb_start=rb; grid.rb_count=width; grid.symbols_per_slot=symbols;
    grid.slot=slot; grid.start_symbol=0; grid.num_symbols=14;
    grid.normal_cp=1; grid.n_rx=4;
    for (int a=0;a<4;++a) {
      data[a].resize(symbols*fft);
      for (auto &v:data[a]) v={(int16_t)std::lround(noise(rng)),(int16_t)std::lround(noise(rng))};
      grid.rx[a]=data[a].data();
    }
  }
  void add(int id, int nscid, int type, uint16_t mask, double delay, double cfo_hz) {
    const int nprb=type ? 4:6;
    std::vector<c16_t> pilot(nprb*(rb+width));
    for (int sym=0;sym<symbols;++sym) if (mask&(1u<<sym)) {
      const uint32_t *gold=nr_gold_pdsch(nrb,symbols,id,nscid,slot,sym);
      nr_pdsch_dmrs_rx(0,gold,pilot.data(),1000,0,rb+width,type,16384);
      for (int m=0;m<nprb*width;++m) {
        const int off=type ? 6*(m/2)+(m%2) : 4*(m/2)+2*(m%2);
        const int bin=(first+12*rb+off)%fft;
        const c16_t p=pilot[nprb*rb+m];
        for (int a=0;a<4;++a) {
          const double phase=-2*M_PI*delay*bin/fft + 2*M_PI*cfo_hz*(slot*0.0005+sym*0.0005/14)
                             + a*0.37;
          const double amp=240.0+20*a;
          const double mag=std::hypot((double)p.r,(double)p.i);
          data[a][sym*fft+bin].r += (int16_t)std::lround(amp*(p.r*std::cos(phase)+p.i*std::sin(phase))/mag);
          data[a][sym*fft+bin].i += (int16_t)std::lround(amp*(p.r*std::sin(phase)-p.i*std::cos(phase))/mag);
        }
      }
    }
  }
};
}

TEST(PuschUlDisc, FourRxHiddenTypeAndLayout) {
  double search_cpu_s=0;
  for (int type : {0,1}) for (int mapping : {0,1}) {
    nr_pusch_ul_disc_reset();
    Fixture f(33+type*10+mapping);
    const int mask=nr_pdcch_blind_ul_dmrs_mask(14,0,mapping,2,1,0);
    ASSERT_GT(mask,0);
    f.add(415,1,type,mask,7.5,310);
    const int priors[]={415,91};
    const std::clock_t start=std::clock();
    const auto r=nr_pusch_ul_disc_search(&f.grid,0x1234,0,priors,2,1-type,0,0);
    search_cpu_s+=double(std::clock()-start)/CLOCKS_PER_SEC;
    ASSERT_TRUE(r.accepted);
    EXPECT_EQ(r.id,415); EXPECT_EQ(r.nscid,1); EXPECT_EQ(r.dmrs_type,type);
    EXPECT_EQ(r.mask,mask);
    EXPECT_LE(r.false_alarm_bound,1e-6);
  }
  std::printf("ULC_BENCH prior_cpu_us_per_grant=%.0f\n",search_cpu_s*1e6/4);
}

TEST(PuschUlDisc, HighIdAndBackgroundWindow) {
  nr_pusch_ul_disc_reset();
  Fixture high(731);
  const int mask=nr_pdcch_blind_ul_dmrs_mask(14,0,0,1,1,0);
  ASSERT_GT(mask,0);
  high.add(40000,1,1,mask,-9,270);
  const int priors[]={415,40000};
  const auto seeded=nr_pusch_ul_disc_search(&high.grid,0x1010,0,priors,2,0,0,0);
  ASSERT_TRUE(seeded.accepted);
  EXPECT_EQ(seeded.id,40000);
  EXPECT_EQ(seeded.nscid,1);
  const auto cached=nr_pusch_ul_disc_cached(0x1010);
  EXPECT_TRUE(cached.accepted);
  EXPECT_FALSE(cached.fresh);
  EXPECT_EQ(cached.id,40000);
  Fixture next(735);
  next.grid.start_symbol=1;
  next.grid.num_symbols=10;
  const int next_mask=nr_pdcch_blind_ul_dmrs_mask(10,1,1,1,1,0);
  ASSERT_GT(next_mask,0);
  next.add(40000,1,1,next_mask,5,270);
  const int wrong_next[]={415};
  const auto same_ue=nr_pusch_ul_disc_search(&next.grid,0x1010,0,wrong_next,1,0,0,0);
  ASSERT_TRUE(same_ue.accepted);
  EXPECT_EQ(same_ue.id,40000);
  EXPECT_EQ(same_ue.mask,next_mask);
  EXPECT_FALSE(same_ue.fresh);
  nr_pusch_ul_disc_reset();
  Fixture background(740);
  background.add(37,0,0,mask,4,0);
  const int wrong[]={415};
  const auto swept=nr_pusch_ul_disc_search(&background.grid,0x2020,0,wrong,1,0,0,0);
  ASSERT_TRUE(swept.accepted);
  EXPECT_EQ(swept.id,37);
}

TEST(PuschUlDisc, TwoSymbolFrontLoadProvesMaxLengthTwo) {
  nr_pusch_ul_disc_reset();
  int field=-1;
  for (int v=0;v<32;++v) {
    uint8_t cdm=0; uint16_t ports=0; int front=0;
    if (decode_dci_antenna_ports_val(1,nullptr,NR_PUSCH_Config__transformPrecoder_disabled,
                                     v,&cdm,&ports,&front)==0 && front==2) { field=v; break; }
  }
  ASSERT_GE(field,0);
  Fixture f(810);
  const int mask=nr_pdcch_blind_ul_dmrs_mask(14,0,0,1,2,0);
  ASSERT_GT(mask,0);
  f.add(415,0,0,mask,3,190);
  const int prior[]={415};
  const auto r=nr_pusch_ul_disc_search(&f.grid,0x3030,0,prior,1,0,0,field);
  ASSERT_TRUE(r.accepted);
  EXPECT_EQ(r.mask,mask);
  EXPECT_EQ(r.front_load,2);
  EXPECT_EQ(r.max_length,2);
}

TEST(PuschUlDisc, TimingScoreLocatesSignedDelayUnderCfo) {
  Fixture f(144);
  const int mask=nr_pdcch_blind_ul_dmrs_mask(14,0,0,2,1,0);
  f.add(415,0,0,mask,143,450);
  int best=0; double best_score=-1;
  for (int advance=-256;advance<=256;advance+=16) {
    /* FEP advance subtracts arrival delay from the apparent channel delay. */
    Fixture probe(144);
    probe.add(415,0,0,mask,143-advance,450);
    int pairs=0;
    double s=nr_pusch_ul_disc_delay_score(&probe.grid,1u<<2,0,0,415,&pairs);
    if (s>best_score) { best_score=s; best=advance; }
  }
  EXPECT_NEAR(best,143,16);
  EXPECT_GT(best_score,0.8);
}

TEST(PuschUlDisc, NoiseOnlyDoesNotPublish) {
  const int priors[]={415,40000};
  double search_cpu_s=0;
  for (int i=0;i<20;++i) {
    nr_pusch_ul_disc_reset();
    Fixture f(900+i);
    const std::clock_t start=std::clock();
    const auto r=nr_pusch_ul_disc_search(&f.grid,0x4321,0,priors,2,0,0,0);
    search_cpu_s+=double(std::clock()-start)/CLOCKS_PER_SEC;
    EXPECT_FALSE(r.accepted) << i;
  }
  std::printf("ULC_BENCH unresolved_cpu_us_per_grant=%.0f\n",search_cpu_s*1e6/20);
}

TEST(PuschUlDisc, PerUeTimingEmaRejectsOutlier) {
  nr_pusch_ul_disc_reset();
  bool tracked=true;
  EXPECT_EQ(nr_pusch_ul_disc_timing_seed(1,1600,10,&tracked),1600);
  EXPECT_FALSE(tracked);
  nr_pusch_ul_disc_timing_update(1,1600,1712,0.9,10);
  EXPECT_EQ(nr_pusch_ul_disc_timing_seed(1,1600,11,&tracked),1712);
  EXPECT_TRUE(tracked);
  nr_pusch_ul_disc_timing_update(1,1600,1600-800,0.9,11);
  EXPECT_EQ(nr_pusch_ul_disc_timing_seed(1,1600,12,&tracked),1712);
  EXPECT_EQ(nr_pusch_ul_disc_timing_seed(2,1600,12,&tracked),1600);
  EXPECT_FALSE(tracked);
  nr_pusch_ul_disc_epoch(2);
  EXPECT_EQ(nr_pusch_ul_disc_timing_seed(1,1600,13,&tracked),1600);
  EXPECT_FALSE(tracked);
}

int main(int argc, char **argv) {
  logInit();
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
