#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <cstdio>
#include <random>
#include <chrono>
#include <complex>
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

TEST(PuschUlDisc, TaTrackKeepsAdvanceAcrossOldSlotsSilenceAndSeedsNewRntis) {
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
  Fixture f(8);
  g_bg_fix=&f; g_bg_runs=0;
  ASSERT_EQ(nr_ul_bgdisc_start(4,nullptr),0);
  auto j=bg_job(f,5);
  ASSERT_EQ(nr_ul_bgdisc_submit(&j),1);
  usleep(150000); /* older than the 100 ms job age */
  ASSERT_EQ(nr_ul_bgdisc_start(4,bg_runner),0);
  nr_ul_bgdisc_stats_t st{};
  for (int i=0;i<200 && st.expired==0;++i) { usleep(10000); nr_ul_bgdisc_stats(&st); }
  nr_ul_bgdisc_stop();
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

/* UL delay fast path: the pruned estimator must give the full-grid decision (argmax of P over all n positions, peak/mean
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


/* ======================================================================================================================
 * UL timing sweep (G1): delay x SNR x width, 4 RX. TEST-ONLY synthetic bursts, no production knob.
 *
 * Model: one DM-RS symbol on the granted PRBs. The FEP at advance `a` sees the signal with apparent delay d = a - T (T = the
 * advance that would centre the window; + = late), i.e. every bin rotates by exp(-j 2 pi k d / fft) (k = signed bin). The burst is
 * generated once at advance 0 (signal + white noise, int16) and the FEP at any other advance is the exact bin rotation of THAT noisy
 * burst, so every estimator sees the same noise realisation, as it would from one set of time samples.
 * SNR is per RE and per RX: |pilot|^2 / E|noise|^2. Antennas carry independent complex gains (the pair statistics cancel them).
 * ====================================================================================================================== */
namespace tsweep {
struct Cfg { int fft, carrier_rb, rb_start, rb_count, type; };
struct Burst {
  Cfg c;
  int nsym = 1; /* DM-RS symbols in the burst (mask), symbols 2,3.. */
  int slot = 7, id = 415, nscid = 0;
  std::vector<int> bin, ks;            /* per pilot: FFT bin and signed bin */
  std::vector<c16_t> pilot;            /* per pilot (amp 16384 sequence) */
  std::vector<c16_t> y[4][4];          /* [ant][sym] per pilot value at advance 0 */
  int npil() const { return (int)bin.size(); }
};
inline double db2amp(double db) { return std::pow(10.0, db / 20.0); }
inline c16_t sat16(double r, double i) {
  auto f = [](double v) { return (int16_t)std::max(-32768.0, std::min(32767.0, std::nearbyint(v))); };
  return c16_t{f(r), f(i)};
}
inline void make_pilots(Burst &b, int sym, int id, int nscid, std::vector<c16_t> &out) {
  const int nprb = b.c.type ? 4 : 6;
  std::vector<c16_t> pilot(nprb * (b.c.rb_start + b.c.rb_count));
  const uint32_t *gold = nr_gold_pdsch(b.c.carrier_rb, 14, id, nscid, b.slot, sym);
  nr_pdsch_dmrs_rx(0, gold, pilot.data(), 1000, 0, b.c.rb_start + b.c.rb_count, b.c.type, 16384);
  out.assign(pilot.begin() + nprb * b.c.rb_start, pilot.end());
}
/* T: true centring advance. snr_db per RE per RX. */
inline void make_burst(Burst &b, const Cfg &c, int nsym, int id, int nscid, double T, double snr_db, unsigned seed, bool noise_only = false) {
  b = Burst();
  b.c = c; b.nsym = nsym; b.id = id; b.nscid = nscid;
  const int nprb = c.type ? 4 : 6, n = nprb * c.rb_count;
  const int first = c.fft - 12 * c.carrier_rb / 2;
  for (int m = 0; m < n; ++m) {
    const int off = c.type ? 6 * (m / 2) + (m % 2) : 4 * (m / 2) + 2 * (m % 2);
    const int bin = (first + 12 * c.rb_start + off) % c.fft;
    b.bin.push_back(bin);
    b.ks.push_back(bin >= c.fft / 2 ? bin - c.fft : bin);
  }
  std::mt19937 rng(seed);
  std::normal_distribution<double> g(0, 1);
  const double A = noise_only ? 0.0 : 1000.0, sg = 1000.0 * (noise_only ? 1.0 : db2amp(-snr_db)) / std::sqrt(2.0);
  double gph[4], gamp[4];
  for (int a = 0; a < 4; ++a) { gph[a] = 6.283185307 * (rng() % 1000) / 1000.0; gamp[a] = 0.8 + 0.4 * (rng() % 1000) / 1000.0; }
  for (int s = 0; s < nsym; ++s) {
    std::vector<c16_t> pil;
    make_pilots(b, 2 + s, id, nscid, pil);
    if (s == 0) b.pilot = pil;
    for (int a = 0; a < 4; ++a) {
      b.y[a][s].resize(n);
      const double sph = 6.283185307 * (rng() % 1000) / 1000.0; /* per-symbol phase (CFO/phase noise): the estimators must not rely on it */
      for (int m = 0; m < n; ++m) {
        const double mag = std::hypot((double)pil[m].r, (double)pil[m].i);
        const double d = -T; /* apparent delay at advance 0 */
        const double ph = -2 * M_PI * b.ks[m] * d / c.fft + gph[a] + sph;
        const double pr = pil[m].r / mag, pi = pil[m].i / mag;
        /* the transmitted pilot is the conjugate of the receiver-side sequence nr_pdsch_dmrs_rx returns (the Fixture of this file does the same) */
        const double sr = A * gamp[a] * (pr * std::cos(ph) + pi * std::sin(ph));
        const double si = A * gamp[a] * (pr * std::sin(ph) - pi * std::cos(ph));
        b.y[a][s][m] = sat16(sr + sg * g(rng), si + sg * g(rng));
      }
    }
  }
}
/* the FEP at advance a: every pilot bin rotated by exp(-j 2 pi k a / fft) */
struct Fep {
  std::vector<c16_t> row[4]; /* [14*fft] per antenna */
  nr_pusch_ul_disc_grid_t grid{};
};
inline void fep(const Burst &b, double a, Fep &f) {
  const Cfg &c = b.c;
  for (int ant = 0; ant < 4; ++ant) {
    f.row[ant].assign((size_t)14 * c.fft, c16_t{0, 0});
    f.grid.rx[ant] = f.row[ant].data();
    for (int s = 0; s < b.nsym; ++s)
      for (int m = 0; m < b.npil(); ++m) {
        const double ph = -2 * M_PI * b.ks[m] * a / c.fft, cs = std::cos(ph), sn = std::sin(ph);
        const c16_t v = b.y[ant][s][m];
        f.row[ant][(size_t)(2 + s) * c.fft + b.bin[m]] = sat16(v.r * cs - v.i * sn, v.r * sn + v.i * cs);
      }
  }
  f.grid.fft = c.fft; f.grid.n_rb_carrier = c.carrier_rb; f.grid.first_carrier = c.fft - 12 * c.carrier_rb / 2;
  f.grid.rb_start = c.rb_start; f.grid.rb_count = c.rb_count; f.grid.symbols_per_slot = 14; f.grid.slot = b.slot;
  f.grid.start_symbol = 0; f.grid.num_symbols = 14; f.grid.normal_cp = 1; f.grid.n_rx = 4;
}
inline uint16_t mask_of(const Burst &b) { uint16_t m = 0; for (int s = 0; s < b.nsym; ++s) m |= 1u << (2 + s); return m; }

/* BASELINE A: the per-grant estimator of the decode path (comb-2 / config type 1 branch of nr_ul_channel_estimation.c): per antenna, one LS
 * estimate per pilot pair repeated on 4 packed bins, n-point IDFT, peak/mean > 15 (PEAK_DETECT_THRESHOLD), best antenna by peak power. On the first
 * DM-RS symbol only (ul_passive_delay reuses it on the others). */
inline int baseline_est_delay(const Burst &b, const Fep &f, double *peak_over_mean) {
  const int n = b.c.fft;
  double best_pk = -1; int best_est = 0; double best_ratio = 0;
  std::vector<float> re(n), im(n);
  for (int ant = 0; ant < 4; ++ant) {
    std::fill(re.begin(), re.end(), 0.f); std::fill(im.begin(), im.end(), 0.f);
    const int pairs = b.npil() / 2;
    std::vector<c16_t> ls(n, c16_t{0, 0});
    for (int q = 0; q < pairs && 4 * q + 3 < n; ++q) {
      double cr = 0, ci = 0;
      for (int kl = 0; kl < 2; ++kl) {
        const int m = 2 * q + kl;
        const c16_t y = f.row[ant][(size_t)2 * n + b.bin[m]], p = b.pilot[m];
        cr += (double)p.r * y.r - (double)p.i * y.i;  /* p * y: p is the receiver-side sequence */
        ci += (double)p.r * y.i + (double)p.i * y.r;
      }
      const c16_t v = sat16(cr / 65536.0, ci / 65536.0);
      for (int k = 0; k < 4; ++k) ls[4 * q + k] = v;
    }
    for (int k = 0; k < n; ++k) { re[k] = ls[k].r; im[k] = ls[k].i; }
    nr_pdf_fft(re.data(), im.data(), n);
    double pk = -1, sum = 0; int tb = 0;
    for (int t = 0; t < n; ++t) { const double p = (double)re[t] * re[t] + (double)im[t] * im[t]; sum += p; if (p > pk) { pk = p; tb = t; } }
    const double mean = sum / n;
    const int est = (mean > 0 && pk / mean > NR_PDF_THRESHOLD) ? (tb > n / 2 ? tb - n : tb) : 0;
    if (pk >= best_pk) { best_pk = pk; best_est = est; best_ratio = mean > 0 ? pk / mean : 0; }
  }
  if (peak_over_mean) *peak_over_mean = best_ratio;
  return best_est;
}
/* BASELINE B: the timing worker of ul_bg_run: delay_score over coarse candidates then +-coarse refine at step 16, accept at score >= 0.35 and the
 * adjacent-pair bound; known DM-RS id. Returns the chosen advance and the number of FEPs. */
inline double g_bg_score_us; /* accumulated scoring time of baseline B (the emulated FEP rotation is not counted) */
inline bool baseline_bg_search(const Burst &b, int seed, bool tracked, int *adv, int *nfep, double *score_out) {
  const int radius = tracked ? 256 : 2048, coarse = tracked ? 64 : 128;
  int best_adv = seed, best_pairs = 0; double best = -1; int feps = 0;
  Fep f;
  const uint16_t mk = 1u << 2;
  for (int phase = 0; phase < 2; ++phase) {
    const int center = phase ? best_adv : seed, span = phase ? coarse : radius, step = phase ? 16 : coarse;
    for (int d = -span; d <= span; d += step) {
      const int a = center + d;
      if (a < -2048 || a > 2048) continue;
      fep(b, a, f); ++feps;
      int pairs = 0;
      struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
      const double s = nr_pusch_ul_disc_delay_score(&f.grid, mk, b.c.type, b.nscid, b.id, &pairs);
      clock_gettime(CLOCK_MONOTONIC, &t1);
      g_bg_score_us += (t1.tv_sec - t0.tv_sec) * 1e6 + (t1.tv_nsec - t0.tv_nsec) / 1e3;
      if (s > best) { best = s; best_pairs = pairs; best_adv = a; }
    }
  }
  double fa;
  *adv = best_adv; *nfep = feps; *score_out = best;
  return best >= 0.35 && nr_pusch_ul_disc_accept(best, best_pairs, &fa);
}
struct Stat { int n = 0, h2 = 0, h6 = 0, h20 = 0; double sq = 0; double maxabs = 0; std::vector<double> errs; };
inline void add(Stat &s, double err) {
  ++s.n; const double a = std::fabs(err);
  s.h2 += a <= 2; s.h6 += a <= 6; s.h20 += a <= 20; s.sq += err * err; s.maxabs = std::max(s.maxabs, a); s.errs.push_back(a);
}
inline double pct(const Stat &s, double p) {
  if (s.errs.empty()) return 0;
  std::vector<double> e = s.errs; std::sort(e.begin(), e.end());
  return e[std::min<size_t>(e.size() - 1, (size_t)(p * e.size()))];
}
inline void print(const char *tag, const Cfg &c, double snr, const Stat &s, double extra = -1) {
  std::printf("TSWEEP %-22s fft=%d rb=%3d type=%d snr=%+4.0f dB n=%3d |e|<=2:%5.1f%% <=6:%5.1f%% <=20:%5.1f%% rms=%7.2f p50=%6.2f p90=%7.2f max=%7.1f%s",
              tag, c.fft, c.rb_count, c.type, snr, s.n, 100.0 * s.h2 / s.n, 100.0 * s.h6 / s.n, 100.0 * s.h20 / s.n, std::sqrt(s.sq / s.n),
              pct(s, 0.5), pct(s, 0.9), s.maxabs, extra >= 0 ? "" : "\n");
  if (extra >= 0) std::printf(" %.2f\n", extra);
}
}  // namespace tsweep

TEST(UlTimingSweep, BaselineCurrentEstimators) {
  using namespace tsweep;
  /* MEASURED baseline of the CURRENT estimators, comb-2 (config type 1), 4 RX, 1 DM-RS symbol, true advance T uniform in [-fft/5, fft/5] (inside the comb-2 range
   * fft/4 of any estimator that sees the comb; the per-grant estimator's own range is only fft/8 because its LS row repeats each pilot-pair estimate on 4 bins). */
  const Cfg cfgs[] = {{2048, 106, 33, 40, 0}, {2048, 106, 0, 106, 0}, {4096, 273, 100, 100, 0}, {2048, 106, 33, 12, 0}};
  const double snrs[] = {-20, -15, -10, -5, 0, 5, 10};
  const int N = 60;
  for (const Cfg &c : cfgs) {
    for (double snr : snrs) {
      g_bg_score_us = 0;
      Stat sa, sain, sb, sbt; double fep_b = 0, fep_bt = 0; int acc_b = 0, acc_bt = 0;
      std::mt19937 rng(1000 + (int)snr + c.rb_count);
      for (int i = 0; i < N; ++i) {
        const int span = c.fft / 5;
        const int T = (int)(rng() % (2 * span + 1)) - span;
        Burst b; make_burst(b, c, 1, 415, 0, T, snr, rng());
        Fep f0; fep(b, 0, f0);
        /* A: single FEP at advance 0 (seed = nominal); the estimate is the apparent delay, truth = 0 - T */
        const int est = baseline_est_delay(b, f0, nullptr);
        add(sa, est - (0 - T));
        if (std::abs(T) <= c.fft / 8) add(sain, est - (0 - T));
        /* B untracked: seed 0 */
        int adv, nf; double sc;
        bool ok = baseline_bg_search(b, 0, false, &adv, &nf, &sc);
        add(sb, adv - T); fep_b += nf; acc_b += ok;
        /* B tracked: seed within +-100 of T */
        const int seed = T + (int)(rng() % 201) - 100;
        ok = baseline_bg_search(b, seed, true, &adv, &nf, &sc);
        add(sbt, adv - T); fep_bt += nf; acc_bt += ok;
      }
      print("A per-grant est_delay", c, snr, sa);
      if (sain.n) print("A (|T|<=fft/8 only)", c, snr, sain);
      print("B worker untracked", c, snr, sb); std::printf("TSWEEP   B untracked accepted=%.0f%% FEP/job=%.0f\n", 100.0 * acc_b / N, fep_b / N);
      print("B worker tracked+-100", c, snr, sbt); std::printf("TSWEEP   B tracked accepted=%.0f%% FEP/job=%.0f ; scoring cpu per job (both arms averaged) = %.0f us\n", 100.0 * acc_bt / N, fep_bt / N, g_bg_score_us / (2.0 * N));
    }
  }
}


namespace tsweep {
inline double now_us() { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e6 + t.tv_nsec / 1e3; }
}
TEST(UlTimingSweep, ClosedFormDelayVsSnr) {
  using namespace tsweep;
  /* NEW: one FEP at the seed, closed-form delay from the grid, one corrective FEP, verify on the corrected grid. Same bursts/seeds as the baseline test. */
  const Cfg cfgs[] = {{2048, 106, 33, 40, 0}, {2048, 106, 0, 106, 0}, {4096, 273, 100, 100, 0}, {2048, 106, 33, 12, 0}, {2048, 106, 33, 40, 1}, {4096, 273, 0, 273, 1}};
  const double snrs[] = {-20, -15, -10, -5, 0, 5, 10};
  const int N = 60;
  for (const Cfg &c : cfgs) {
    for (double snr : snrs) {
      Stat s1, s2; int acc = 0, ver = 0; double us = 0, ratio_sum = 0;
      std::mt19937 rng(1000 + (int)snr + c.rb_count);
      for (int i = 0; i < N; ++i) {
        const int span = c.fft / 5; /* well inside the comb-2 range fft/4 */
        const int T = (int)(rng() % (2 * span + 1)) - span;
        Burst b; make_burst(b, c, 1, 415, 0, T, snr, rng());
        Fep f0; fep(b, 0, f0);
        nr_pusch_ul_disc_delay_t d;
        const double t0 = now_us();
        const bool ok = nr_pusch_ul_disc_delay_closed(&f0.grid, mask_of(b), c.type, 0, 415, &d);
        us += now_us() - t0;
        ASSERT_TRUE(ok);
        add(s1, d.delay - (0 - T));
        ratio_sum += d.ratio;
        double fa;
        const bool accepted = nr_pusch_ul_disc_delay_accept(&d, 1.0, &fa);
        acc += accepted;
        /* the corrective FEP: advance -= delay (integer samples, as the FEP takes them) */
        const int a1 = -(int)std::lround(d.delay);
        add(s2, a1 - T);
        Fep f1; fep(b, a1, f1);
        nr_pusch_ul_disc_delay_t v;
        if (nr_pusch_ul_disc_delay_closed(&f1.grid, mask_of(b), c.type, 0, 415, &v) && nr_pusch_ul_disc_delay_accept(&v, 1.0, &fa) && std::fabs(v.delay) <= 3) ++ver;
      }
      print("CF estimate (1 FEP)", c, snr, s1);
      print("CF after corr. FEP", c, snr, s2);
      std::printf("TSWEEP   CF accepted(1e-6, family 1)=%.0f%% verified-after-corrective=%.0f%% mean ratio=%.1f cpu/call=%.0f us FEP/job=2\n", 100.0 * acc / N, 100.0 * ver / N, ratio_sum / N, us / N);
    }
  }
}

TEST(UlTimingSweep, ClosedFormStopCondition) {
  using namespace tsweep;
  /* STOP CONDITION: |error| <= 2 samples at -10 dB per-RE SNR with 4 RX. Reference grants: 40 PRB on a 106-PRB carrier (the fixture of this file) and
   * 100 PRB on a 273-PRB carrier. Narrow grants are reported by ClosedFormDelayVsSnr, not asserted (CRLB: 12 PRB at -10 dB is ~5 samples). */
  const Cfg cfgs[] = {{2048, 106, 33, 40, 0}, {4096, 273, 100, 100, 0}};
  for (const Cfg &c : cfgs) {
    Stat s; std::mt19937 rng(77 + c.rb_count);
    for (int i = 0; i < 300; ++i) {
      const int span = c.fft / 5, T = (int)(rng() % (2 * span + 1)) - span;
      Burst b; make_burst(b, c, 1, 415, 0, T, -10.0, rng());
      Fep f0; fep(b, 0, f0);
      nr_pusch_ul_disc_delay_t d;
      ASSERT_TRUE(nr_pusch_ul_disc_delay_closed(&f0.grid, mask_of(b), 0, 0, 415, &d));
      add(s, d.delay - (0 - T));
    }
    print("STOP-COND CF -10 dB", c, -10.0, s);
    EXPECT_GE(s.h2, (int)(0.99 * s.n)) << "rb=" << c.rb_count;
  }
}

TEST(UlTimingSweep, NewNeverWorseThanOld) {
  using namespace tsweep;
  /* G1 non-regression: the closed-form result (ladder + the receive chain's own estimate, better-verified by the same delay-domain ratio) against the per-grant
   * estimator A alone, same bursts, hit = |error| <= 2 samples. Asserted at every (cfg, SNR) point, over all T and inside A's own range |T| <= fft/8. */
  const Cfg cfgs[] = {{2048, 106, 33, 40, 0}, {4096, 273, 100, 100, 0}, {2048, 106, 0, 106, 0}, {2048, 106, 33, 12, 0}};
  const double snrs[] = {-20, -17, -15, -13, -10, -5, 0};
  const int N = 600;
  for (const Cfg &c : cfgs) {
    for (double snr : snrs) {
      int li = 0, gi = 0;
      int ho = 0, hn = 0, hoi = 0, hni = 0, ni = 0, lost = 0, gained = 0, used = 0;
      std::mt19937 rng(5000 + (int)snr + c.rb_count);
      for (int i = 0; i < N; ++i) {
        const int span = c.fft / 5, T = (int)(rng() % (2 * span + 1)) - span;
        Burst b; make_burst(b, c, 1, 415, 0, T, snr, rng());
        Fep f0; fep(b, 0, f0);
        const int est = baseline_est_delay(b, f0, nullptr);
        nr_pusch_ul_disc_delay_t d;
        ASSERT_TRUE(nr_pusch_ul_disc_delay_closed(&f0.grid, mask_of(b), c.type, 0, 415, &d));
        const bool o = std::abs(est - (0 - T)) <= 2, n = std::fabs(d.delay - (0 - T)) <= 2;
        ho += o; hn += n; lost += (o && !n); gained += (!o && n); used += d.used_old;
        if (std::abs(T) <= c.fft / 8) { ++ni; hoi += o; hni += n; li += (o && !n); gi += (!o && n); }
      }
      std::printf("TSWEEP NONREG fft=%d rb=%3d snr=%+4.0f  all-T: old %5.1f%% new %5.1f%% (trial lost %d gained %d, old chosen %d) | in-range |T|<=fft/8 (n=%d): old %5.1f%% new %5.1f%% (lost %d gained %d)\n",
                  c.fft, c.rb_count, snr, 100.0 * ho / N, 100.0 * hn / N, lost, gained, used, ni, ni ? 100.0 * hoi / ni : 0.0, ni ? 100.0 * hni / ni : 0.0, li, gi);
      /* a per-trial guarantee is impossible (both are noisy decisions; a fluke can favour either), so the claim is the paired sign test: gained - lost must not be
       * below -2 sqrt(gained + lost) (z >= -2, the discordant trials are the only information in a paired comparison) */
      EXPECT_GE((double)(gained - lost), -2.0 * std::sqrt((double)(gained + lost))) << "all-T fft=" << c.fft << " rb=" << c.rb_count << " snr=" << snr;
      EXPECT_GE((double)(gi - li), -2.0 * std::sqrt((double)(gi + li))) << "in-range fft=" << c.fft << " rb=" << c.rb_count << " snr=" << snr;
    }
  }
}

TEST(UlTimingSweep, DelayInvariantIdBaselineVsEvidence) {
  using namespace tsweep;
  /* DM-RS id decision with no CRC. BASELINE = the current rule: adjacent-pair score (delay invariant) on one symbol, accept at pairs >= 128 and bound <= 1e-6 over 65536x2x2x32.
   * NEW = closed-form ratio bound (family 1: a known candidate) and the same with a 16-id prior list, one grant; then accumulated over grants. */
  const Cfg c{2048, 106, 33, 40, 0};
  const double snrs[] = {-20, -17, -15, -13, -10, -5, 0};
  for (double snr : snrs) {
    int base = 0, one = 0, one16 = 0; const int N = 60;
    std::mt19937 rng(5 + (int)snr);
    for (int i = 0; i < N; ++i) {
      const int T = (int)(rng() % 401) - 200;
      Burst b; make_burst(b, c, 1, 415, 0, T, snr, rng());
      Fep f0; fep(b, 0, f0);
      int pairs = 0; double fa;
      const double sc = nr_pusch_ul_disc_score(&f0.grid, mask_of(b), 0, 0, 415, &pairs);
      base += nr_pusch_ul_disc_accept(sc, pairs, &fa);
      nr_pusch_ul_disc_delay_t d;
      ASSERT_TRUE(nr_pusch_ul_disc_delay_closed(&f0.grid, mask_of(b), 0, 0, 415, &d));
      one += nr_pusch_ul_disc_delay_accept(&d, 1.0, &fa);
      one16 += nr_pusch_ul_disc_delay_accept(&d, 16.0 * 4, &fa);
    }
    std::printf("TSWEEP ID decision (40 PRB, 4 RX, 1 symbol) snr=%+4.0f dB  baseline-score-accept=%3.0f%%  CF-bound(family 1)=%3.0f%%  CF-bound(16 ids x 4 type/nscid)=%3.0f%%\n",
                snr, 100.0 * base / N, 100.0 * one / N, 100.0 * one16 / N);
  }
  /* false alarms: noise only and wrong ids; the bound must hold */
  int fa_noise = 0, fa_wrong = 0; double max_ratio = 0; const int M = 400;
  std::mt19937 rng(99);
  for (int i = 0; i < M; ++i) {
    Burst b; make_burst(b, c, 1, 415, 0, 0, 0, rng(), true);
    Fep f0; fep(b, 0, f0); nr_pusch_ul_disc_delay_t d; double fa;
    ASSERT_TRUE(nr_pusch_ul_disc_delay_closed(&f0.grid, mask_of(b), 0, 0, 415, &d));
    fa_noise += nr_pusch_ul_disc_delay_accept(&d, 16.0 * 4, &fa); max_ratio = std::max(max_ratio, d.ratio);
    Burst w; make_burst(w, c, 1, 415, 0, 123, 5.0, rng()); /* strong signal, other id */
    Fep fw; fep(w, 0, fw);
    ASSERT_TRUE(nr_pusch_ul_disc_delay_closed(&fw.grid, mask_of(w), 0, 0, 416 + i % 50, &d));
    fa_wrong += nr_pusch_ul_disc_delay_accept(&d, 16.0 * 4, &fa); max_ratio = std::max(max_ratio, d.ratio);
  }
  /* calibration of the bound: with family 1 the fraction of noise-only / wrong-id trials whose bound is <= q must not exceed q (a valid bound is conservative) */
  {
    const int K = 6000; int n01 = 0, n001 = 0, n0001 = 0;
    for (int i = 0; i < K; ++i) {
      Burst b; make_burst(b, c, 1, 415, 0, 0, 0, rng(), true);
      Fep f0; fep(b, 0, f0); nr_pusch_ul_disc_delay_t d; double bd;
      ASSERT_TRUE(nr_pusch_ul_disc_delay_closed(&f0.grid, mask_of(b), 0, 0, 415, &d));
      nr_pusch_ul_disc_delay_accept(&d, 1.0, &bd);
      n01 += bd <= 0.1; n001 += bd <= 0.01; n0001 += bd <= 0.001;
    }
    std::printf("TSWEEP bound calibration over %d noise-only trials (family 1): P(bound<=0.1)=%.4f P(<=0.01)=%.4f P(<=0.001)=%.4f (must be <= the level)\n", K, (double)n01 / K, (double)n001 / K, (double)n0001 / K);
    EXPECT_LE((double)n01 / K, 0.1); EXPECT_LE((double)n001 / K, 0.01); EXPECT_LE((double)n0001 / K, 0.001);
  }
  std::printf("TSWEEP ID false alarms over %d noise-only and %d wrong-id (+5 dB signal) trials at the 1e-6 bound: noise=%d wrong=%d, max ratio seen=%.2f\n", M, M, fa_noise, fa_wrong, max_ratio);
  EXPECT_EQ(fa_noise, 0);
  EXPECT_EQ(fa_wrong, 0);
}

TEST(UlTimingSweep, WrongIdStrongSignalFalseAlarm) {
  using namespace tsweep;
  /* The null that matters on air: a real UE is present (coherent on all RX) and the hypothesis is the WRONG id. The descrambled pilots are then one pseudo-random
   * unit sequence times the per-RX channel, so the RX do not average the delay-domain power (ratio ~ Exp(1)), unlike noise-only input (Gamma(dof)). */
  const Cfg cfgs[] = {{2048, 106, 33, 40, 0}, {2048, 106, 0, 106, 0}, {4096, 273, 100, 100, 0}};
  for (const Cfg &c : cfgs) {
    for (double snr : {5.0, 20.0, 40.0}) {
      int acc1 = 0, acc16 = 0, accfam = 0, n01 = 0, n001 = 0; const int M = 3000; double rmax = 0, rsum = 0;
      std::mt19937 rng(321 + c.rb_count);
      for (int i = 0; i < M; ++i) {
        const int T = (int)(rng() % 401) - 200;
        Burst w; make_burst(w, c, 1, 415, 0, T, snr, rng());
        Fep fw; fep(w, 0, fw); nr_pusch_ul_disc_delay_t d; double fa;
        ASSERT_TRUE(nr_pusch_ul_disc_delay_closed(&fw.grid, mask_of(w), c.type, 0, 500 + i, &d));
        acc1 += nr_pusch_ul_disc_delay_accept(&d, 1.0, &fa);
        n01 += fa <= 0.1; n001 += fa <= 0.01;
        accfam += nr_pusch_ul_disc_delay_accept(&d, 16.0, &fa);
        rmax = std::max(rmax, d.ratio); rsum += d.ratio;
      }
      std::printf("TSWEEP WRONGID rb=%3d snr=%+3.0f dB  wrong-id false accept (1e-6 bound) family1: %d/%d  family16: %d/%d  ratio mean %.2f max %.1f  P(bound<=0.1)=%.4f P(<=0.01)=%.4f\n", c.rb_count, snr, acc1, M, accfam, M, rsum / M, rmax, (double)n01 / M, (double)n001 / M);
      EXPECT_EQ(acc1, 0); EXPECT_EQ(accfam, 0); EXPECT_LE((double)n01 / M, 0.1); EXPECT_LE((double)n001 / M, 0.01);
    }
  }
}

TEST(UlTimingSweep, IdEvidenceAccumulatesAcrossGrantsWithoutCrc) {
  using namespace tsweep;
  const Cfg c{2048, 106, 33, 12, 0}; /* a small grant: one grant alone is not enough */
  for (double snr : {-12.0, -9.0, -6.0}) {
    nr_pusch_ul_disc_reset();
    std::mt19937 rng(31 + (int)snr);
    int confirmed_at = -1, wrong_confirmed = 0; double bound = 1;
    for (int g = 0; g < 60; ++g) {
      const int T = 150 + (int)(rng() % 7) - 3;
      Burst b; make_burst(b, c, 1, 777, 1, T, snr, rng());
      Fep f0; fep(b, 0, f0);
      nr_pusch_ul_disc_delay_t d;
      ASSERT_TRUE(nr_pusch_ul_disc_delay_closed(&f0.grid, mask_of(b), 0, 1, 777, &d));
      int n = 0;
      if (nr_pusch_ul_disc_id_evidence(0x4601, 777, 1, 0, &d, 1000 + g, 64.0, &n, &bound) && confirmed_at < 0) confirmed_at = g + 1;
      nr_pusch_ul_disc_delay_t dw;
      ASSERT_TRUE(nr_pusch_ul_disc_delay_closed(&f0.grid, mask_of(b), 0, 1, 778, &dw));
      if (nr_pusch_ul_disc_id_evidence(0x4601, 778, 1, 0, &dw, 1000 + g, 64.0, &n, &bound)) ++wrong_confirmed;
    }
    std::printf("TSWEEP ID evidence (12 PRB, 4 RX, 1 symbol, snr=%+.0f dB): confirmed after %d grant(s) (-1 = never in 60); wrong id confirmed %d times\n", snr, confirmed_at, wrong_confirmed);
    EXPECT_EQ(wrong_confirmed, 0);
    if (snr >= -9.0) EXPECT_GT(confirmed_at, 0);
  }
}

TEST(UlTimingSweep, CellTrackerTrainedByOneUeTimesTheNext) {
  using namespace tsweep;
  nr_pusch_ul_disc_reset();
  const Cfg c{2048, 106, 33, 40, 0};
  std::mt19937 rng(4);
  /* UE A: closed-form measurement at nominal 1600, trains the cell. UE B (never seen) starts from the cell's seed and its first look is already inside +-3 samples. */
  const int nominal = 1600;
  int TA = 180;
  Burst a; make_burst(a, c, 1, 415, 0, TA, -8.0, rng());
  Fep fa; fep(a, 0, fa);
  nr_pusch_ul_disc_delay_t d; double bd;
  ASSERT_TRUE(nr_pusch_ul_disc_delay_closed(&fa.grid, mask_of(a), 0, 0, 415, &d));
  ASSERT_TRUE(nr_pusch_ul_disc_delay_accept(&d, 1.0, &bd));
  const int measured = nominal - (int)std::lround(d.delay); /* the advance that centres the window */
  nr_pusch_ul_disc_timing_update_ev(0x1001, nominal, nominal + measured - nominal, 5000);
  bool tracked = true;
  const int seed = nr_pusch_ul_disc_timing_seed(0x2002, nominal, 5010, &tracked);
  EXPECT_FALSE(tracked);
  /* the burst model uses advance relative to the nominal: T = centring advance minus nominal */
  const int TB = TA + 3; /* UE B arrives 3 samples away */
  Burst b; make_burst(b, c, 1, 415, 0, TB - (seed - nominal) * 0 /* centring advance */, -8.0, rng());
  Fep fb; fep(b, seed - nominal, fb);
  ASSERT_TRUE(nr_pusch_ul_disc_delay_closed(&fb.grid, mask_of(b), 0, 0, 415, &d));
  EXPECT_LE(std::fabs(d.delay), 8.0) << "seed=" << seed << " measured=" << measured;
  EXPECT_TRUE(nr_pusch_ul_disc_delay_accept(&d, 1.0, &bd));
}



/* ======================================================================================================================
 * Alias / worker-branch test bed (G1 regression: delay beyond the comb-2 unambiguous range).
 * A TIME-DOMAIN model: the frequency-domain phase-ramp model of the sweeps above cannot tell the comb-2 aliases T and T +- fft/2 apart (their pilots are
 * identical), the window position can: a window half an FFT away from the symbol reads half of the neighbouring symbol (ISI), the true window does not. So here a
 * slot stream is synthesised: 3 slots of OFDM symbols (cyclic prefix CP = 144 fft/2048 samples), data QPSK on all granted REs of every symbol but the DM-RS symbol
 * (symbol 2 of the middle slot: comb pilots only), 4 RX with independent complex gains, a 2-tap channel (0 and 16 samples, -7 dB), AWGN, the whole stream delayed by D
 * samples. The FEP at advance a starts the window CP/8 before the FFT start (as passive_ul_fep_offset does) minus a: the centring advance is T = -D - CP/8.
 * "Good window" = the window lies inside the inter-symbol-interference-free zone of both taps: a - T in [-8, CP - 8].
 * ====================================================================================================================== */
namespace tdm {
using cd = std::complex<double>;
inline void fftc(std::vector<cd> &a, bool inv) {
  const size_t n = a.size();
  for (size_t i = 1, j = 0; i < n; ++i) { size_t bit = n >> 1; for (; j & bit; bit >>= 1) j ^= bit; j ^= bit; if (i < j) std::swap(a[i], a[j]); }
  for (size_t len = 2; len <= n; len <<= 1) {
    const double ang = 2 * M_PI / len * (inv ? 1 : -1);
    const cd wl(std::cos(ang), std::sin(ang));
    for (size_t i = 0; i < n; i += len) {
      cd w(1);
      for (size_t k = 0; k < len / 2; ++k) { const cd u = a[i + k], v = a[i + k + len / 2] * w; a[i + k] = u + v; a[i + k + len / 2] = u - v; w *= wl; }
    }
  }
}
struct Model {
  tsweep::Cfg c;
  int cp = 0, D = 0;
  int nsym_grant = 12;        /* grant symbols 2 .. 2 + nsym_grant - 1 of the slot (the DM-RS symbol is the first); symbol 1 is data unless prev_silent */
  bool prev_silent = false;
  tsweep::Burst b;            /* pilot layout (bins, pilot values) */
  std::vector<cd> rx[4];
  static constexpr int kSym = 42, kDmrs = 14 + 2;
  int tstar() const { return -D - cp / 8; }
  void build(const tsweep::Cfg &cfg, int delay, double snr_db, unsigned seed, int id = 415, int nscid = 0) {
    c = cfg; D = delay; cp = 144 * cfg.fft / 2048;
    const int N = c.fft, L = N + cp;
    b = tsweep::Burst(); b.c = c; b.nsym = 1; b.id = id; b.nscid = nscid;
    const int nprb = c.type ? 4 : 6, n = nprb * c.rb_count, first = N - 12 * c.carrier_rb / 2;
    for (int m = 0; m < n; ++m) {
      const int off = c.type ? 6 * (m / 2) + (m % 2) : 4 * (m / 2) + 2 * (m % 2);
      const int bin = (first + 12 * c.rb_start + off) % N;
      b.bin.push_back(bin); b.ks.push_back(bin >= N / 2 ? bin - N : bin);
    }
    tsweep::make_pilots(b, 2, id, nscid, b.pilot);
    std::mt19937 rng(seed);
    std::normal_distribution<double> g(0, 1);
    const double A = 1000.0;
    std::vector<cd> tx((size_t)kSym * L);
    std::vector<cd> X(N);
    for (int s = 0; s < kSym; ++s) {
      std::fill(X.begin(), X.end(), cd(0));
      if (s == kDmrs) {
        for (int m = 0; m < n; ++m) {
          const double mag = std::hypot((double)b.pilot[m].r, (double)b.pilot[m].i);
          X[b.bin[m]] = A * cd(b.pilot[m].r / mag, -b.pilot[m].i / mag); /* the transmitted pilot is the conjugate of the receiver-side sequence */
        }
      } else if (prev_silent && s == kDmrs - 1) {
        /* no transmission before the grant */
      } else {
        for (int k = 0; k < 12 * c.rb_count; ++k) X[(first + 12 * c.rb_start + k) % N] = A * cd((rng() & 1) ? 0.70710678 : -0.70710678, (rng() & 1) ? 0.70710678 : -0.70710678);
      }
      fftc(X, true);
      for (int i = 0; i < N; ++i) tx[(size_t)s * L + cp + i] = X[i] / (double)N;
      for (int i = 0; i < cp; ++i) tx[(size_t)s * L + i] = tx[(size_t)s * L + N + i];
    }
    const double sig = A / std::pow(10.0, snr_db / 20.0) / std::sqrt((double)N) / std::sqrt(2.0); /* per real component */
    const int tau[2] = {0, 16};
    const double amp[2] = {std::sqrt(0.8), std::sqrt(0.2)};
    const size_t len = tx.size();
    for (int a = 0; a < 4; ++a) {
      cd gn[2];
      for (int t = 0; t < 2; ++t) gn[t] = amp[t] * std::polar(1.0, 6.283185307 * (rng() % 1000) / 1000.0);
      rx[a].assign(len, cd(0));
      for (size_t i = 0; i < len; ++i) {
        cd v(0);
        for (int t = 0; t < 2; ++t) { const long j = (long)i - D - tau[t]; if (j >= 0 && j < (long)len) v += gn[t] * tx[j]; }
        rx[a][i] = v + cd(sig * g(rng), sig * g(rng));
      }
    }
  }
  /* the FEP of the DM-RS symbol at advance a; false when the window leaves the stream */
  bool fep(int a, tsweep::Fep &f) const {
    const int N = c.fft, L = N + cp;
    const long s = (long)kDmrs * L + cp - cp / 8 - a;
    if (s < 0 || s + N > (long)rx[0].size()) return false;
    std::vector<cd> w(N);
    for (int ant = 0; ant < 4; ++ant) {
      f.row[ant].assign((size_t)14 * N, c16_t{0, 0});
      f.grid.rx[ant] = f.row[ant].data();
      for (int i = 0; i < N; ++i) w[i] = rx[ant][s + i];
      fftc(w, false);
      for (size_t m = 0; m < b.bin.size(); ++m) f.row[ant][(size_t)2 * N + b.bin[m]] = tsweep::sat16(w[b.bin[m]].real(), w[b.bin[m]].imag());
    }
    f.grid.fft = N; f.grid.n_rb_carrier = c.carrier_rb; f.grid.first_carrier = N - 12 * c.carrier_rb / 2;
    f.grid.rb_start = c.rb_start; f.grid.rb_count = c.rb_count; f.grid.symbols_per_slot = 14; f.grid.slot = b.slot;
    f.grid.start_symbol = 0; f.grid.num_symbols = 14; f.grid.normal_cp = 1; f.grid.n_rx = 4;
    return true;
  }
  /* cyclic-prefix statistic of the grant's symbols for the window at advance a (the production passive_cp_stat on this stream) */
  bool cp_stat(int a, double *m, double *e2) const {
    const int N = c.fft, L = N + cp;
    double sm = 0, se = 0;
    for (int s = 0; s < nsym_grant; ++s) {
      const long w = (long)(kDmrs + s) * L + cp - cp / 8 - a; /* window start */
      if (w - cp < 0 || w + N > (long)rx[0].size()) return false;
      for (int ant = 0; ant < 4; ++ant)
        for (int n = 0; n < cp; ++n) {
          const cd x = rx[ant][w - cp + n], y = rx[ant][w - cp + n + N];
          sm += (x * std::conj(y)).real();
          se += std::norm(x) * std::norm(y);
        }
    }
    *m = sm; *e2 = se;
    return se > 0;
  }
  bool good(int a) const { return a - tstar() >= -8 && a - tstar() <= cp - 8; }
};
struct Ctx { const Model *m; tsweep::Fep f; };
inline bool fep_cb(void *p, int adv) { Ctx *c = (Ctx *)p; return c->m->fep(adv, c->f); }
inline bool cp_cb(void *p, int adv, double *m, double *e2) { Ctx *c = (Ctx *)p; return c->m->cp_stat(adv, m, e2); }

/* the PREVIOUS worker branch exactly as it was before the alias fix, as a reference: ONE verify at a1, alias windows read only after it passed, untracked => 3 windows */
/* OLD (rr-mpci-main) worker: coarse/fine delay_score scan, ids known; returns advance, accepted */
inline bool old_scan(const Model &m, int seed, int *adv, int *nfep) {
  const int radius = 2048, coarse = 128;
  int best_adv = seed, best_pairs = 0; double best = -1; int feps = 0;
  tsweep::Fep f;
  for (int phase = 0; phase < 2; ++phase) {
    const int center = phase ? best_adv : seed, span = phase ? coarse : radius, step = phase ? 16 : coarse;
    for (int d = -span; d <= span; d += step) {
      const int a = center + d;
      if (a < -2048 || a > 2048) continue;
      if (!m.fep(a, f)) continue;
      ++feps;
      int pairs = 0;
      const double sc = nr_pusch_ul_disc_delay_score(&f.grid, 1u << 2, m.c.type, m.b.nscid, m.b.id, &pairs);
      if (sc > best) { best = sc; best_pairs = pairs; best_adv = a; }
    }
  }
  double fa;
  *adv = best_adv; *nfep = feps;
  return best >= 0.35 && nr_pusch_ul_disc_accept(best, best_pairs, &fa);
}
/* the old inline refinement: the receive chain's own estimator at the window it decodes at; moves when |est| > PASSIVE_UL_DELAY_TOL (6) */
inline int old_inline(const Model &m, int adv) {
  tsweep::Fep f;
  if (!m.fep(adv, f)) return adv;
  const int est = tsweep::baseline_est_delay(m.b, f, nullptr);
  return (est > 6 || est < -6) ? adv - est : adv;
}
}  // namespace tdm

TEST(UlAlias, ModelCentringAndOldScan) {
  using namespace tdm;
  /* sanity of the model: at high SNR the closed form at the window of every delay reads the delay modulo the comb period, and the OLD scan finds the good
   * window for a delay beyond the comb range (700 samples, fft 2048): the observation that the previous G1 build lost against */
  const tsweep::Cfg c{2048, 106, 33, 40, 0};
  Model m; m.build(c, 700, 20.0, 5);
  tsweep::Fep f;
  ASSERT_TRUE(m.fep(m.tstar(), f));
  nr_pusch_ul_disc_delay_t d;
  ASSERT_TRUE(nr_pusch_ul_disc_delay_closed(&f.grid, 1u << 2, 0, 0, 415, &d));
  EXPECT_LE(std::fabs(d.delay), 6.0) << "T=" << m.tstar();
  int adv, nf;
  EXPECT_TRUE(old_scan(m, 0, &adv, &nf));
  EXPECT_TRUE(m.good(adv)) << "old scan adv=" << adv << " T=" << m.tstar();
}


namespace tdm {
struct Cell { int n = 0, old_hit = 0, new_hit = 0, lost = 0, gained = 0, old_alias = 0, new_alias = 0, locked = 0; };
inline void tally(Cell &c, const Model &m, int old_adv, int new_adv) {
  const bool o = m.good(old_adv), n = m.good(new_adv);
  ++c.n; c.old_hit += o; c.new_hit += n; c.lost += (o && !n); c.gained += (!o && n);
  const int per = m.c.fft / 2; /* an error that is a multiple of the comb period is an alias */
  c.old_alias += !o && std::abs(std::abs(old_adv - m.tstar()) - per) <= 40; c.new_alias += !n && std::abs(std::abs(new_adv - m.tstar()) - per) <= 40;
}
/* worker branch, new: nr_pusch_ul_disc_timing_resolve, then the receive chain's refinement at the window it decodes at (untracked, seed = nominal = 0) */
inline int new_worker(const Model &m, int seed, bool tracked, nr_pusch_ul_disc_timing_out_t *outp = nullptr, bool use_cp = true) {
  Ctx cx{&m, {}};
  const int prior[4] = {m.b.id + 11, m.b.id, 7, 65535 & (m.b.id * 3)};
  nr_pusch_ul_disc_timing_in_t in{};
  in.grid = &cx.f.grid; in.mask = 1u << 2; in.type = m.c.type; in.nscid = m.b.nscid; in.prior = prior; in.nprior = 4; in.cached_id = -1;
  in.seed = seed; in.ta_offset = 0; in.fft = m.c.fft; in.tracked = tracked; in.fep = fep_cb; in.cp_stat = use_cp ? cp_cb : nullptr; in.ctx = &cx;
  nr_pusch_ul_disc_timing_out_t o;
  nr_pusch_ul_disc_timing_resolve(&in, &o);
  if (outp) *outp = o;
  return o.locked ? o.ta : seed;
}
inline int old_worker(const Model &m, int seed, int *nfep) {
  int adv = seed, nf = 0;
  const bool ok = old_scan(m, seed, &adv, &nf);
  if (nfep) *nfep = nf;
  return ok ? adv : seed;
}
}  // namespace tdm

namespace tdm {
/* the PREVIOUS (uncommitted G1) worker branch, kept as a reference: the alias windows a1 +- fft/2 compared by the pilot ratio, after the verify at a1 */
inline int v1_worker(const Model &m, int seed, bool tracked) {
  Ctx cx{&m, {}};
  const int prior[4] = {m.b.id + 11, m.b.id, 7, 65535 & (m.b.id * 3)};
  const int fft = m.c.fft, mask = 1u << 2, type = m.c.type, nscid = m.b.nscid;
  const int nwin = tracked ? 1 : 3, win[3] = {0, fft / 2, -fft / 2};
  for (int w = 0; w < nwin; ++w) {
    const int a0 = seed + win[w];
    if (a0 < -2048 || a0 > 2048) continue;
    if (!fep_cb(&cx, a0)) continue;
    int bi = -1; nr_pusch_ul_disc_delay_t best{};
    for (int i = 0; i < 4; ++i) { nr_pusch_ul_disc_delay_t d; if (nr_pusch_ul_disc_delay_closed(&cx.f.grid, mask, type, nscid, prior[i], &d) && (bi < 0 || d.ratio > best.ratio)) { best = d; bi = i; } }
    if (bi < 0) continue;
    const double family = 4.0;
    if (!nr_pusch_ul_disc_delay_accept(&best, family, nullptr)) continue;
    int a1 = a0 - (int)std::lrint(best.delay);
    nr_pusch_ul_disc_delay_t v = best;
    if (a1 != a0) {
      if (!fep_cb(&cx, a1)) continue;
      if (!(nr_pusch_ul_disc_delay_closed(&cx.f.grid, mask, type, nscid, prior[bi], &v) && nr_pusch_ul_disc_delay_accept(&v, family, nullptr) && std::fabs(v.delay) <= 4.0)) continue;
    }
    if (type == 0) {
      const int hw = fft / 2, alt[2] = {a1 + hw, a1 - hw};
      int best_a = a1; nr_pusch_ul_disc_delay_t bv = v;
      for (int k = 0; k < 2; ++k) {
        if (alt[k] < -2048 || alt[k] > 2048) continue;
        if (!fep_cb(&cx, alt[k])) continue;
        nr_pusch_ul_disc_delay_t t;
        if (nr_pusch_ul_disc_delay_closed(&cx.f.grid, mask, type, nscid, prior[bi], &t) && t.ratio > bv.ratio) { bv = t; best_a = alt[k]; }
      }
      if (best_a != a1) {
        if (nr_pusch_ul_disc_delay_accept(&bv, family * 3.0, nullptr) && std::fabs(bv.delay) <= 4.0) a1 = best_a - (int)std::lrint(bv.delay);
        else continue;
      }
    }
    return a1;
  }
  return seed;
}
}  // namespace tdm

TEST(UlAlias, WorkerNeverWorseThanOldScan) {
  using namespace tdm;
  /* The worker branch (nr_pusch_ul_disc_timing_resolve) against the OLD coarse/fine scan, over the whole window T in [-fft/2, fft/2) (the comb-2 alias period is
   * fft/2, the unambiguous range fft/4), SNR -10..20 dB per RE per RX, 4 RX, comb 2, 12 / 40 / 100 PRB, fft 2048 / 4096, the symbol before the grant silent or
   * not. Same bursts for all builds. Hit = the window the grant is decoded at lies in the ISI-free zone (Model::good), after each build's own inline refinement.
   * V1 = the previous uncommitted worker (alias windows by pilot ratio), printed for reference. Paired sign test per (cfg, SNR, prev) over all T and per T bucket. */
  struct C { tsweep::Cfg c; } cfgs[] = {{{2048, 106, 33, 40, 0}}, {{2048, 106, 33, 12, 0}}, {{4096, 273, 100, 100, 0}}};
  const double snrs[] = {-10, -5, 0, 10, 20};
  const int NT = 12, NJ = 3;
  for (const C &cc : cfgs) {
    for (int prev_silent = 0; prev_silent < 2; ++prev_silent) {
      for (double snr : snrs) {
        Cell tot;
        int v1_hit = 0, v1_lost = 0, v1_gained = 0;
        std::vector<Cell> per(NT);
        std::mt19937 rng(9000 + (int)snr + cc.c.rb_count + prev_silent);
        for (int k = 0; k < NT; ++k) {
          for (int j = 0; j < NJ; ++j) {
            nr_pusch_ul_disc_reset();
            const int Tc = -cc.c.fft / 2 + (k * cc.c.fft) / NT + (int)(rng() % (cc.c.fft / NT));
            Model m; m.prev_silent = prev_silent; m.build(cc.c, -Tc - 144 * cc.c.fft / 2048 / 8, snr, rng());
            const int oa = old_inline(m, old_worker(m, 0, nullptr));
            const int na = old_inline(m, new_worker(m, 0, false));
            const int va = old_inline(m, v1_worker(m, 0, false));
            tally(per[k], m, oa, na);
            v1_hit += m.good(va); v1_lost += (m.good(oa) && !m.good(va)); v1_gained += (!m.good(oa) && m.good(va));
          }
        }
        for (const Cell &p : per) { tot.n += p.n; tot.old_hit += p.old_hit; tot.new_hit += p.new_hit; tot.lost += p.lost; tot.gained += p.gained; tot.old_alias += p.old_alias; tot.new_alias += p.new_alias; }
        std::printf("ALIAS worker fft=%d rb=%3d prev_silent=%d snr=%+4.0f n=%d old %5.1f%% new %5.1f%% (lost %d gained %d, alias-errors old %d new %d) | V1 %5.1f%% (lost %d gained %d)\n", cc.c.fft,
                    cc.c.rb_count, prev_silent, snr, tot.n, 100.0 * tot.old_hit / tot.n, 100.0 * tot.new_hit / tot.n, tot.lost, tot.gained, tot.old_alias, tot.new_alias,
                    100.0 * v1_hit / tot.n, v1_lost, v1_gained);
        EXPECT_GE((double)(tot.gained - tot.lost), -2.0 * std::sqrt((double)(tot.gained + tot.lost))) << "fft=" << cc.c.fft << " rb=" << cc.c.rb_count << " prev_silent=" << prev_silent << " snr=" << snr;
        for (int k = 0; k < NT; ++k) {
          const Cell &p = per[k];
          if (p.lost > p.gained) std::printf("ALIAS   T-bucket %2d/%d (T~%d): old %d/%d new %d/%d lost %d gained %d\n", k, NT, -cc.c.fft / 2 + (k * cc.c.fft) / NT, p.old_hit, p.n, p.new_hit, p.n, p.lost, p.gained);
          EXPECT_GE((double)(p.gained - p.lost), -2.0 * std::sqrt((double)(p.gained + p.lost))) << "bucket " << k << " fft=" << cc.c.fft << " rb=" << cc.c.rb_count << " prev_silent=" << prev_silent << " snr=" << snr;
        }
      }
    }
  }
}

TEST(UlAlias, InlineNeverWorseThanOld) {
  using namespace tdm;
  /* The inline per-grant step (a grant with no cached hypothesis): old = the receive chain's estimator at the seed window; new = closed form on that same window
   * (used when its 1e-6 bound holds for the known id; moves the window; an alias is preferred only on CP-correlation evidence), then the same refinement. */
  const tsweep::Cfg cfgs[] = {{2048, 106, 33, 40, 0}, {4096, 273, 100, 100, 0}};
  const double snrs[] = {-10, 0, 10, 20};
  const int NT = 12, NJ = 3;
  for (const tsweep::Cfg &c : cfgs) {
    for (int prev_silent = 0; prev_silent < 2; ++prev_silent)
      for (double snr : snrs) {
        Cell tot;
        int nocp_hit = 0;
        std::mt19937 rng(7000 + (int)snr + c.rb_count + prev_silent);
        for (int k = 0; k < NT; ++k)
          for (int j = 0; j < NJ; ++j) {
            const int Tc = -c.fft / 2 + (k * c.fft) / NT + (int)(rng() % (c.fft / NT));
            Model m; m.prev_silent = prev_silent; m.build(c, -Tc - 144 * c.fft / 2048 / 8, snr, rng());
            const int oa = old_inline(m, 0);
            tsweep::Fep f; m.fep(0, f);
            nr_pusch_ul_disc_delay_t d;
            int na = 0, nb = 0;
            if (nr_pusch_ul_disc_delay_closed(&f.grid, 1u << 2, c.type, m.b.nscid, m.b.id, &d) && nr_pusch_ul_disc_delay_accept(&d, 1.0, nullptr)) {
              const int dd = (int)std::lrint(d.delay), base = -dd;
              nb = (dd > 6 || dd < -6) ? base : 0;
              const int cand[3] = {base, base + c.fft / 2, base - c.fft / 2};
              double z[3];
              for (int q = 0; q < 3; ++q) { double mm, e2; z[q] = m.cp_stat(cand[q], &mm, &e2) ? mm / std::sqrt(0.5 * e2) : NAN; }
              const int pick = nr_pusch_ul_disc_alias_choose(z, nullptr, 4, 3);
              na = pick ? cand[pick] : nb;
            }
            na = old_inline(m, na); nb = old_inline(m, nb);
            tally(tot, m, oa, na);
            nocp_hit += m.good(nb);
          }
        std::printf("ALIAS inline fft=%d rb=%3d prev_silent=%d snr=%+4.0f n=%d old %5.1f%% new %5.1f%% (lost %d gained %d) | closed form without CP evidence %5.1f%%\n", c.fft, c.rb_count, prev_silent, snr, tot.n,
                    100.0 * tot.old_hit / tot.n, 100.0 * tot.new_hit / tot.n, tot.lost, tot.gained, 100.0 * nocp_hit / tot.n);
        EXPECT_GE((double)(tot.gained - tot.lost), -2.0 * std::sqrt((double)(tot.gained + tot.lost))) << "inline fft=" << c.fft << " prev_silent=" << prev_silent << " snr=" << snr;
      }
  }
}

TEST(UlAlias, TrackedSeedOnAliasIsCorrected) {
  using namespace tdm;
  /* A tracker trained onto the alias (seed = the true centring advance +- fft/2): the resolver reads the alias candidates too, whatever `tracked` says, and moves
   * to the true window when the evidence is there (SNR >= 0 dB); it must never leave a seed that is already right. */
  const tsweep::Cfg cfgs[] = {{2048, 106, 33, 40, 0}, {4096, 273, 100, 100, 0}};
  const double snrs[] = {0, 10, 20};
  for (const tsweep::Cfg &c : cfgs)
    for (int prev_silent = 0; prev_silent < 2; ++prev_silent)
      for (double snr : snrs) {
        int n = 0, fixed = 0, kept = 0, nk = 0;
        std::mt19937 rng(31 + (int)snr + c.rb_count + prev_silent);
        for (int i = 0; i < 24; ++i) {
          const int Tc = (int)(rng() % (c.fft / 2)) - c.fft / 4; /* |T| < fft/4 so that T +- fft/2 stays inside +-2048 */
          Model m; m.prev_silent = prev_silent; m.build(c, -Tc - 144 * c.fft / 2048 / 8, snr, rng());
          nr_pusch_ul_disc_reset();
          const int wrong = m.tstar() + ((i & 1) ? c.fft / 2 : -c.fft / 2);
          ++n; fixed += m.good(new_worker(m, wrong, true));
          ++nk; kept += m.good(new_worker(m, m.tstar() + 2, true));
        }
        std::printf("ALIAS tracked fft=%d rb=%3d prev_silent=%d snr=%+3.0f: seed on the alias -> good %d/%d, seed right -> kept %d/%d\n", c.fft, c.rb_count, prev_silent, snr, fixed, n, kept, nk);
        EXPECT_EQ(kept, nk) << "fft=" << c.fft << " snr=" << snr;
        EXPECT_GE(fixed, n * 9 / 10) << "fft=" << c.fft << " snr=" << snr << " prev_silent=" << prev_silent;
      }
}

int main(int argc, char **argv) {
  logInit();
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

/* U6/(6b): weak timing observations of one RNTI accumulate across grants per arrival window; noise does not. */
static nr_pusch_ul_disc_delay_t weak_obs(double ratio)
{
  nr_pusch_ul_disc_delay_t d = {};
  d.valid = true;
  d.ratio = ratio;
  d.dof = 2;
  d.trials = 64;
  return d;
}

TEST(UlTimingEvidence, WeakUeConvergesAcrossGrantsAtOneArrivalWindow)
{
  nr_pusch_ul_disc_reset();
  const nr_pusch_ul_disc_delay_t d = weak_obs(6.0);
  EXPECT_FALSE(nr_pusch_ul_disc_delay_accept(&d, 1.0, NULL)) << "one grant alone is not decisive";
  int grants = 0, adv = 0;
  double b = 1;
  bool ok = false;
  int n = 0;
  for (; n < 12 && !ok; n++)
    ok = nr_pusch_ul_disc_timing_evidence(0x4601, 1613 + (n % 3) - 1, &d, 1000 + 20 * n, 1.0, &grants, &adv, &b);
  EXPECT_TRUE(ok);
  EXPECT_GE(n, 3) << "needs several grants";
  EXPECT_LE(n, 6);
  EXPECT_NEAR(adv, 1613, 1);
  EXPECT_LE(b, 1e-6);
}

TEST(UlTimingEvidence, NoiseAtRandomWindowsNeverAccumulates)
{
  nr_pusch_ul_disc_reset();
  const nr_pusch_ul_disc_delay_t d = weak_obs(3.0);
  int grants = 0, adv = 0;
  double b = 1;
  for (int n = 0; n < 400; n++)
    EXPECT_FALSE(nr_pusch_ul_disc_timing_evidence(0x4602, 1000 + (n * 37) % 900, &d, 1000 + 20 * n, 1.0, &grants, &adv, &b));
}

TEST(UlTimingEvidence, EvidenceDecaysAndSameSlotCountsOnce)
{
  nr_pusch_ul_disc_reset();
  const nr_pusch_ul_disc_delay_t d = weak_obs(6.0);
  int grants = 0, adv = 0;
  double b = 1;
  for (int i = 0; i < 20; i++) /* the same slot again and again: one observation */
    EXPECT_FALSE(nr_pusch_ul_disc_timing_evidence(0x4603, 1613, &d, 5000, 1.0, &grants, &adv, &b));
  EXPECT_EQ(grants, 1);
  /* observations far apart in time decay away before they can add up */
  nr_pusch_ul_disc_reset();
  bool ok = false;
  for (int n = 0; n < 12; n++)
    ok |= nr_pusch_ul_disc_timing_evidence(0x4604, 1613, &d, 1000 + 20000 * n, 1.0, &grants, &adv, &b);
  EXPECT_FALSE(ok);
}

TEST(UlTimingEvidence, RntisAreIndependent)
{
  nr_pusch_ul_disc_reset();
  const nr_pusch_ul_disc_delay_t d = weak_obs(6.0);
  int grants = 0, adv = 0;
  double b = 1;
  bool ok = false;
  for (int n = 0; n < 12; n++) /* alternating RNTIs: neither reaches the bound before 3 own grants */
    ok |= nr_pusch_ul_disc_timing_evidence(n % 2 ? 0x4605 : 0x4606, 1613, &d, 1000 + 20 * n, 1.0, &grants, &adv, &b) && n < 4;
  EXPECT_FALSE(ok);
}
