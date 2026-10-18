#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <cstdio>
#include <random>
#include <chrono>
#include <cstring>
#include <unistd.h>
#include <vector>
#include <gtest/gtest.h>
extern "C" {
#include "nr_pusch_ul_disc.h"
#include "PHY/nr_phy_common/inc/nr_passive_delay_fast.h"
#include "nr_pusch_ul_bgdisc.h"
#include "nr_gpu_pass_ul.h"
#include "nr_pdcch_blind_monitor.h"
#include "openair2/LAYER2/NR_MAC_COMMON/nr_mac_common.h"
#include "executables/softmodem-common.h"
#include "common/utils/LOG/log.h"
#include "common/config/config_userapi.h"
uint32_t *nr_gold_pdsch(int, int, int, int, int, int);
int nr_pdsch_dmrs_rx(int, const unsigned int *, c16_t *, unsigned short, unsigned char,
                     unsigned short, uint8_t, int16_t);
configmodule_interface_t *uniqCfg = nullptr;
/* this test does not link nr_gpu_pass_ul.c: a recording stand-in for the per-RNTI cache the worker feeds */
static ul_hyp_t g_put_h;
static int g_puts;
void ul_hyp_cache_put(uint16_t, const ul_hyp_t *h) { g_put_h = *h; g_puts++; }
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

TEST(PuschUlDisc, TaTrackKeepsAdvanceAcrossOldSlotsSilenceAndSeedsNewRntis) {
  setenv("ISAC_UL_TA_TRACK","1",1);
  nr_pusch_ul_disc_reset();
  bool tracked=true;
  EXPECT_EQ(nr_pusch_ul_disc_timing_seed(7,1600,100,&tracked),1600); /* nothing learned: N_TA_offset */
  EXPECT_FALSE(tracked);
  nr_pusch_ul_disc_timing_update(7,1600,-16,0.9,100);
  EXPECT_EQ(nr_pusch_ul_disc_timing_seed(7,1600,100,&tracked),-16);  /* same slot (second consumer / CRC trial) keeps it */
  EXPECT_TRUE(tracked);
  EXPECT_EQ(nr_pusch_ul_disc_timing_seed(7,1600,90,&tracked),-16);   /* an older slot too */
  EXPECT_EQ(nr_pusch_ul_disc_timing_seed(7,1600,9000,&tracked),-16); /* 4.5 s of silence: still the seed, searched wide */
  EXPECT_FALSE(tracked);
  EXPECT_EQ(nr_pusch_ul_disc_timing_seed(8,1600,9000,&tracked),-16); /* a never-timed RNTI starts from the cell's advance */
  EXPECT_FALSE(tracked);
  nr_pusch_ul_disc_timing_update(9,1600,1648,0.5,9001);              /* one far UE does not move the cell mode */
  nr_pusch_ul_disc_timing_update(10,1600,0,0.9,9002);
  EXPECT_EQ(nr_pusch_ul_disc_timing_seed(11,1600,9003,&tracked),-16);
  EXPECT_EQ(nr_pusch_ul_disc_timing_seed(9,1600,9003,&tracked),1648); /* but keeps its own */
  EXPECT_TRUE(tracked);
  unsetenv("ISAC_UL_TA_TRACK");
}

namespace {
Fixture *g_bg_fix;
int g_bg_runs;
int bg_runner(const nr_ul_bgdisc_job_t *j) {
  ++g_bg_runs;
  const int priors[]={415,91};
  if (std::memcmp(j->iq[0], g_bg_fix->data[0].data(), j->length*sizeof(c16_t))) return 1; /* the worker sees the submitted samples */
  const auto r=nr_pusch_ul_disc_search(&g_bg_fix->grid,j->rnti,0,priors,2,0,0,0);
  if (!r.accepted) return 1;
  ul_hyp_t h{};
  h.ta=j->ta_offset; h.have_dmrs=1; h.dmrs_id=r.id; h.nscid=r.nscid; h.dmrs_type=r.dmrs_type; h.dmrs_mask=r.mask;
  ul_hyp_cache_put(j->rnti,&h);
  return 0;
}
nr_ul_bgdisc_job_t bg_job(Fixture &f, uint16_t rnti) {
  nr_ul_bgdisc_job_t j{};
  j.rnti=rnti; j.nant=4; j.length=fft*symbols; j.ta_offset=1600;
  for (int a=0;a<4;++a) j.iq[a]=f.data[a].data();
  return j;
}
}

TEST(UlBgDisc, GrantNeverWaitsForSearch) {
  nr_ul_bgdisc_stop();
  ASSERT_EQ(nr_ul_bgdisc_start(4,nullptr),0); /* worker stopped */
  Fixture f(7);
  double worst=0;
  for (int i=0;i<100;++i) {
    auto j=bg_job(f,1000+i);
    const auto t0=std::chrono::steady_clock::now();
    nr_ul_bgdisc_submit(&j);
    worst=std::max(worst,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t0).count());
  }
  nr_ul_bgdisc_stats_t st; nr_ul_bgdisc_stats(&st);
  EXPECT_EQ(st.nohyp,100u);
  EXPECT_EQ(st.submitted,4u); /* depth 4: the rest are dropped, none blocks */
  EXPECT_EQ(st.full,96u);
  EXPECT_LT(worst,1.0);
  nr_ul_bgdisc_stop();
}

TEST(UlBgDisc, WorkerFillsCache) {
  nr_ul_bgdisc_stop(); nr_pusch_ul_disc_reset();
  Fixture f(33);
  const int mask=nr_pdcch_blind_ul_dmrs_mask(14,0,0,2,1,0);
  f.add(415,1,0,mask,7.5,310);
  g_bg_fix=&f; g_bg_runs=0; g_puts=0;
  ASSERT_EQ(nr_ul_bgdisc_start(4,bg_runner),0);
  auto j=bg_job(f,0x4601);
  ASSERT_EQ(nr_ul_bgdisc_submit(&j),1);
  EXPECT_EQ(nr_ul_bgdisc_submit(&j),0); /* same RNTI in flight */
  for (int i=0;i<200 && g_puts==0;++i) usleep(10000);
  nr_ul_bgdisc_stop();
  ASSERT_EQ(g_puts,1);
  EXPECT_EQ(g_put_h.dmrs_id,415); EXPECT_EQ(g_put_h.nscid,1); EXPECT_EQ(g_put_h.ta,1600);
}

TEST(UlBgDisc, ExpiredSnapshotDropped) {
  nr_ul_bgdisc_stop();
  setenv("ISAC_UL_BGDISC_MAX_AGE_MS","30",1);
  Fixture f(8);
  g_bg_fix=&f; g_bg_runs=0;
  ASSERT_EQ(nr_ul_bgdisc_start(4,nullptr),0);
  auto j=bg_job(f,5);
  ASSERT_EQ(nr_ul_bgdisc_submit(&j),1);
  usleep(80000);
  ASSERT_EQ(nr_ul_bgdisc_start(4,bg_runner),0);
  nr_ul_bgdisc_stats_t st{};
  for (int i=0;i<200 && st.expired==0;++i) { usleep(10000); nr_ul_bgdisc_stats(&st); }
  nr_ul_bgdisc_stop();
  unsetenv("ISAC_UL_BGDISC_MAX_AGE_MS");
  EXPECT_EQ(st.expired,1u);
  EXPECT_EQ(g_bg_runs,0); /* never ran: the samples were not read */
}

TEST(UlBgDisc, SweepFindsUnpriorIdAndNoiseDoesNot) {
  nr_pusch_ul_disc_reset();
  Fixture f(61);
  const int mask=nr_pdcch_blind_ul_dmrs_mask(14,0,0,2,1,0);
  f.add(700,0,0,mask,3,100);
  const int priors[]={1,2};
  const auto r=nr_pusch_ul_disc_search_sweep(&f.grid,0x77,0,priors,2,0,0,0,0,1024);
  ASSERT_TRUE(r.accepted); EXPECT_EQ(r.id,700);
  nr_pusch_ul_disc_reset();
  Fixture n(62);
  EXPECT_FALSE(nr_pusch_ul_disc_search_sweep(&n.grid,0x78,0,priors,2,0,0,0,0,1024).accepted);
}

TEST(UlBgDisc, SingleFalseCrcDoesNotLatch) {
  EXPECT_EQ(nr_ul_bgdisc_latch(1,0xA,99,0),0);       /* nothing cached: the first pass pends too */
  EXPECT_EQ(nr_ul_bgdisc_latch(1,0xA,100,0),1);      /* its confirmation in another slot */
  EXPECT_EQ(nr_ul_bgdisc_latch(1,0xA,101,0xA),1);    /* same as cached */
  EXPECT_EQ(nr_ul_bgdisc_latch(1,0xB,102,0xA),0);    /* one CRC pass of a different hypothesis: not latched */
  EXPECT_EQ(nr_ul_bgdisc_latch(1,0xB,102,0xA),0);    /* the same slot again is not a second pass */
  EXPECT_EQ(nr_ul_bgdisc_latch(1,0xB,103,0xA),1);    /* second pass in another slot */
  EXPECT_EQ(nr_ul_bgdisc_latch(2,0xC,200,0xA),0);
  EXPECT_EQ(nr_ul_bgdisc_latch(3,0xC,201,0xA),1);    /* two RNTIs agree */
}

/* ISAC_UL_DELAY_FAST: the pruned estimator must give the full-grid decision (argmax of P over all n positions, peak/mean
 * > 15 by Parseval) on narrow LS rows: clean delays, weak signals near the threshold, pure noise. */
TEST(UlDelayFast, MatchesFullGridDecision) {
  for (const int n : {4096, 1536}) {
  std::mt19937 rng(7);
  std::normal_distribution<double> g(0, 1);
  std::vector<c16_t> ls(n);
  int ran = 0, nonzero = 0, bad = 0;
  for (int c = 0; c < 400; c++) {
    const int m = 12 * (1 + (int)(rng() % (n == 4096 ? 21 : 5))); /* 1..21 PRB (1..5 at n=1536, 40 MHz) */
    const int dly = (int)(rng() % 601) - 300;
    const double snr = (c % 5 == 0) ? -100 : -15.0 + (rng() % 45); /* every 5th case pure noise */
    const double a = 1000 * pow(10, snr / 20), sn = 1000;
    std::fill(ls.begin(), ls.end(), c16_t{0, 0});
    for (int k = 0; k < m; k++) {
      const double ph = -2 * M_PI * k * dly / n;
      ls[k].r = (int16_t)lrint(a * cos(ph) + sn * g(rng));
      ls[k].i = (int16_t)lrint(a * sin(ph) + sn * g(rng));
    }
    double e = 0, pb = -1;
    int tb = 0;
    for (int k = 0; k < m; k++) e += (double)ls[k].r * ls[k].r + (double)ls[k].i * ls[k].i;
    for (int t = 0; t < n; t++) {
      const double p = nr_pdf_power(ls.data(), m, n, t);
      if (p > pb) { pb = p; tb = t; }
    }
    const int ref = pb > NR_PDF_THRESHOLD * e ? (tb > n / 2 ? tb - n : tb) : 0;
    delay_t d = {0};
    if (!nr_passive_est_delay_fast(n, ls.data(), &d)) continue;
    ran++;
    nonzero += ref != 0;
    if (d.est_delay != ref) {
      bad++;
      printf("m=%d dly=%d snr=%.0f ref=%d fast=%d\n", m, dly, snr, ref, d.est_delay);
    }
    if (ref != 0) EXPECT_NEAR(d.delay_max_val, pb / (2.0 * n * n), 1.0);
  }
  EXPECT_EQ(bad, 0);
  EXPECT_GT(ran, 300);
  EXPECT_GT(nonzero, 100);
  }
  const int n = 4096;
  std::vector<c16_t> ls(n);
  delay_t d = {0};
  std::fill(ls.begin(), ls.end(), c16_t{1, 1}); /* full-width row: not applicable, the caller runs the full estimator */
  EXPECT_FALSE(nr_passive_est_delay_fast(n, ls.data(), &d));
  /* the receiver's type-1 LS row repeats each pilot-pair estimate 4x (smooth, noise peaks wider): still the full-grid decision */
  std::mt19937 rng(11);
  std::normal_distribution<double> g(0, 1000);
  for (int c = 0; c < 100; c++) {
    const int m = 12 * (1 + c % 21);
    std::fill(ls.begin(), ls.end(), c16_t{0, 0});
    for (int k = 0; k < m; k += 4) {
      const c16_t v = {(int16_t)lrint(g(rng)), (int16_t)lrint(g(rng))};
      for (int j = 0; j < 4; j++) ls[k + j] = v;
    }
    double e = 0, pb = -1;
    int tb = 0;
    for (int k = 0; k < m; k++) e += (double)ls[k].r * ls[k].r + (double)ls[k].i * ls[k].i;
    for (int t = 0; t < n; t++) {
      const double p = nr_pdf_power(ls.data(), m, n, t);
      if (p > pb) { pb = p; tb = t; }
    }
    const int ref = pb > NR_PDF_THRESHOLD * e ? (tb > n / 2 ? tb - n : tb) : 0;
    delay_t dd = {0};
    ASSERT_TRUE(nr_passive_est_delay_fast(n, ls.data(), &dd)) << m;
    EXPECT_EQ(dd.est_delay, ref) << m;
  }
}

int main(int argc, char **argv) {
  logInit();
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
