/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_csirs_disc_synth.h"
#include "nr_csirs_disc_fp_internal.h"
#include <gtest/gtest.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <random>
#include <algorithm>
extern "C" {
void *uniqCfg=nullptr;
void exit_function(const char *,const char *,int,const char *s,int) {fprintf(stderr,"%s\n",s?s:"");abort();}
}
struct Ring {
  nr_csirs_disc_ring_t r={};
  ~Ring() {nr_csirs_disc_synth_free(&r);}
};
static nr_csirs_disc_synth_resource_t resource(int row,int density=2)
{
  static const int bm[]={1,8,1,2,1,15,3,3,63,7,15,15,7,7,7,15,15,15};
  static const int cd[]={0,0,1,1,1,1,1,2,1,2,1,2,1,2,3,1,2,3};
  nr_csirs_disc_synth_resource_t c={};
  c.row=row;c.fd_bitmap=bm[row-1];c.l0=4;c.l1=8;c.cdm=cd[row-1];c.density=row==1?3:density;
  c.rb_start=4;c.n_rb=17;c.scramb_id=415;c.period=5;c.offset=1;c.power=1.6;
  return c;
}
static nr_csirs_disc_synth_cfg_t config(const nr_csirs_disc_synth_resource_t *c,int n=1)
{
  nr_csirs_disc_synth_cfg_t cfg={};
  cfg.n_slots=640;cfg.n_rb=32;cfg.n_ant=4;cfg.slots_per_frame=20;cfg.first_slot=1003;
  cfg.seed=0x839412;cfg.snr_db=5;cfg.load=1;cfg.dmrs=true;cfg.ssb=true;cfg.resources=c;cfg.n_resources=n;
  return cfg;
}
static std::vector<uint8_t> exclusions(int nb)
{
  std::vector<uint8_t> e(14*12*nb);
  if(nb>=20) for(int l=0;l<4;++l) for(int k=(12*nb-240)/2;k<(12*nb+240)/2;++k) e[l*12*nb+k]=1;
  return e;
}
static bool same(const nr_csirs_disc_fp_t &a,const nr_csirs_disc_fp_t &b)
{
  return a.sym_mask==b.sym_mask && a.sc_mask==b.sc_mask && a.density==b.density && a.rb_start==b.rb_start &&
    a.n_rb==b.n_rb && a.period==b.period && a.offset==b.offset;
}
static void check(const nr_csirs_disc_ring_t &r,const nr_csirs_disc_fp_t &expected)
{
  auto e=exclusions(r.n_rb);nr_csirs_disc_fp_cfg_t cfg={0,e.data(),1.03f};nr_csirs_disc_fp_t f[32]={};
  int n=nr_csirs_disc_footprints_cpu(&r,&cfg,f,32);
  bool found=false;
  for(int i=0;i<n;++i) {
    found|=same(f[i],expected);
    if(!same(f[i],expected)) printf("candidate sym=%x sc=%x d=%u rb=%u:%u P=%u o=%u strength=%.3f\n",
      f[i].sym_mask,f[i].sc_mask,f[i].density,f[i].rb_start,f[i].n_rb,f[i].period,f[i].offset,f[i].strength);
  }
  EXPECT_TRUE(found)<<"expected sym="<<expected.sym_mask<<" sc="<<expected.sc_mask<<" d="<<(int)expected.density
     <<" rb="<<expected.rb_start<<":"<<expected.n_rb<<" P="<<expected.period<<" o="<<expected.offset;
  EXPECT_EQ(n,1);
  nr_csirs_disc_detect_reset();
  nr_csirs_disc_fp_t stateful[32]={};
  ASSERT_EQ(nr_csirs_disc_detect(&r,&cfg,stateful,32),n);
  for(int i=0;i<n;++i) {EXPECT_TRUE(same(f[i],stateful[i]));EXPECT_FLOAT_EQ(f[i].strength,stateful[i].strength);}
  nr_csirs_disc_detect_reset();
  if(const auto *ops=nr_csirs_disc_gpu_load()) {
    nr_csirs_disc_fp_t gpu[32]={};ASSERT_EQ(ops->footprints(&r,&cfg,gpu,32),n);
    for(int i=0;i<n;++i) {EXPECT_TRUE(same(f[i],gpu[i]));EXPECT_FLOAT_EQ(f[i].strength,gpu[i].strength);}
  }
}
class Footprints : public ::testing::TestWithParam<std::tuple<int,int,int>> {};
TEST_P(Footprints, LoadedGrid)
{
  auto [row,density,snr]=GetParam();auto c=resource(row,density);auto cfg=config(&c);
  std::mt19937 random(row*71+density*23+snr);
  const int periods[]={4,5,8,10,16,20};
  c.period=periods[random()%6];c.offset=random()%c.period;c.rb_start=2+random()%7;c.n_rb=15+random()%7;
  cfg.n_slots=128*c.period;cfg.snr_db=snr;cfg.seed=random();cfg.load=0.95;
  Ring r;ASSERT_EQ(nr_csirs_disc_synth(&r.r,&cfg),0);
  nr_csirs_disc_fp_t expected={};ASSERT_EQ(nr_csirs_disc_synth_footprint(&c,&expected),0);check(r.r,expected);
  printf("detection row=%d density=%d snr=%d period=%u slots=%d occasions=128\n",row,density,snr,c.period,cfg.n_slots);
}
INSTANTIATE_TEST_SUITE_P(Rows,Footprints,::testing::Values(
  std::make_tuple(1,3,5),std::make_tuple(1,3,10),std::make_tuple(1,3,20),
  std::make_tuple(2,2,5),std::make_tuple(2,2,10),std::make_tuple(2,2,20),
  std::make_tuple(2,0,5),std::make_tuple(2,0,10),std::make_tuple(2,0,20),
  std::make_tuple(2,1,5),std::make_tuple(2,1,10),std::make_tuple(2,1,20),
  std::make_tuple(4,2,5),std::make_tuple(4,2,10),std::make_tuple(4,2,20),
  std::make_tuple(6,2,5),std::make_tuple(6,2,10),std::make_tuple(6,2,20),
  std::make_tuple(11,2,5),std::make_tuple(11,2,10),std::make_tuple(11,2,20),
  std::make_tuple(16,2,5),std::make_tuple(16,2,10),std::make_tuple(16,2,20),
  std::make_tuple(18,2,5),std::make_tuple(18,2,10),std::make_tuple(18,2,20)));
TEST(CsirsDisc, LocalContrastRejectsWholeSymbolGain)
{
  nr_csirs_disc_ring_t r={};ASSERT_EQ(nr_csirs_disc_ring_init(&r,4,8,20,2,false),0);
  for(int t=0;t<2;++t) {
    r.meta[t]={uint32_t(t),0,uint8_t(t),1};
    for(int a=0;a<4;++a) for(int l=0;l<14;++l) {
      int16_t *v=nr_csirs_disc_ring_sym(&r,t,a,l);
      std::fill(v,v+24*8,(t==1 && l==4)?1000:100);
    }
  }
  std::vector<uint32_t> x(2*(14*12*8+168));
  nr_disc_fp_normalize(&r,nullptr,x.data());
  for(auto v:x) EXPECT_EQ(v,4096u);
  for(int a=0;a<4;++a) for(int rb=0;rb<8;++rb) {
    auto v=nr_csirs_disc_ring_sym(&r,1,a,4)+24*rb;
    v[0]=v[1]=3000;
  }
  nr_disc_fp_normalize(&r,nullptr,x.data());
  for(int rb=0;rb<8;++rb) for(int sc=0;sc<12;++sc)
    EXPECT_EQ(x[(14*12*8+168)+4*12*8+12*rb+sc],sc==0?8192u:4096u);
  nr_csirs_disc_ring_free(&r);
}

TEST(CsirsDisc, AllPeriods)
{
  for(int p:{4,5,8,10,16,20,32,40,64,80,160,320,640}) {
    auto c=resource(1);c.period=p;c.offset=p-1;c.rb_start=3;c.n_rb=9;c.power=1.6;
    auto cfg=config(&c);cfg.n_rb=16;cfg.n_slots=64*p;cfg.load=1;
    Ring r;ASSERT_EQ(nr_csirs_disc_synth(&r.r,&cfg),0);
    nr_csirs_disc_fp_t expected={};ASSERT_EQ(nr_csirs_disc_synth_footprint(&c,&expected),0);
    int first_pass=0;
    auto e=exclusions(r.r.n_rb);nr_csirs_disc_fp_cfg_t fc={0,e.data(),1.03f};
    for(int occasions:{4,8,16,32,64}) {
      r.r.capacity=occasions*p;nr_csirs_disc_fp_t found[32]={};
      int n=nr_csirs_disc_footprints_cpu(&r.r,&fc,found,32);
      if(n==1 && same(found[0],expected)) {first_pass=r.r.capacity;break;}
    }
    EXPECT_GT(first_pass,0)<<"period="<<p;
    printf("first tested passing prefix P=%d slots=%d (4,8,16,32,64 occasions checked)\n",p,first_pass);
    r.r.capacity=cfg.n_slots;check(r.r,expected);
  }
}
TEST(CsirsDisc, GeneratorPowerCalibration)
{
  auto c=resource(1);c.power=1.2;auto cfg=config(&c);cfg.snr_db=100;cfg.n_slots=512;cfg.ssb=false;
  Ring r;ASSERT_EQ(nr_csirs_disc_synth(&r.r,&cfg),0);double pilot=0,data=0;
  for(int t=0;t<r.r.capacity;++t) if(r.r.meta[t].abs_slot%c.period==c.offset)
    for(int a=0;a<4;++a) for(int rb=c.rb_start;rb<c.rb_start+c.n_rb;++rb) {
      const int16_t *x=nr_csirs_disc_ring_sym(&r.r,t,a,c.l0)+24*rb;
      pilot+=double(x[0])*x[0]+double(x[1])*x[1];data+=double(x[2])*x[2]+double(x[3])*x[3];
    }
  EXPECT_NEAR(pilot/data,1.2,0.1);
}
TEST(CsirsDisc, GeneratorDot5LastRb)
{
  for(int row:{2,11,16}) {
    auto c=resource(row,0);c.rb_start=8;c.n_rb=15;
    auto cfg=config(&c);cfg.n_slots=16;cfg.snr_db=100;cfg.ssb=false;cfg.dmrs=false;
    // Odd-width grid: the extra silent RB is outside the physical grid.
    cfg.n_rb=23;Ring r;ASSERT_EQ(nr_csirs_disc_synth(&r.r,&cfg),0);
    for(int t=0;t<r.r.capacity;++t) if(r.r.meta[t].abs_slot%c.period==c.offset) {
      for(int a=0;a<4;++a) {
        const int16_t *x=nr_csirs_disc_ring_sym(&r.r,t,a,c.l0);
        int sc=row==2?3:0;
        auto power=[&](int rb) {int k=2*(12*rb+sc);return double(x[k])*x[k]+double(x[k+1])*x[k+1];};
        EXPECT_NEAR(power(22)/power(8),1.0,0.1)<<"row="<<row<<" t="<<t;
      }
    }
  }
}
TEST(CsirsDisc, GeneratorAndAllRows)
{
  for(int row=1;row<=18;++row) {
    auto c=resource(row);auto cfg=config(&c);cfg.n_slots=16;cfg.n_rb=24;
    Ring a,b;ASSERT_EQ(nr_csirs_disc_synth(&a.r,&cfg),0);ASSERT_EQ(nr_csirs_disc_synth(&b.r,&cfg),0);
    EXPECT_EQ(memcmp(a.r.grid_base,b.r.grid_base,a.r.capacity*a.r.slot_stride*sizeof(int16_t)),0);
    nr_csirs_disc_fp_t expected={};EXPECT_EQ(nr_csirs_disc_synth_footprint(&c,&expected),0);
    EXPECT_NE(expected.sym_mask,0);EXPECT_NE(expected.sc_mask,0);
  }
  auto c=resource(6);c.fd_bitmap=1;auto cfg=config(&c);Ring r;
  EXPECT_EQ(nr_csirs_disc_synth(&r.r,&cfg),-1);
}
TEST(CsirsDisc, OtaGeometryWeakPilot)
{
  for(int p:{5,10}) {
    auto c=resource(1);c.rb_start=0;c.n_rb=52;c.period=p;c.offset=2;c.power=1.2;
    nr_csirs_disc_synth_resource_t pair[]={c,c};pair[1].l0=8;
    auto cfg=config(pair,2);cfg.n_rb=217;cfg.n_slots=256*p;
    Ring r;ASSERT_EQ(nr_csirs_disc_synth(&r.r,&cfg),0);
    nr_csirs_disc_fp_t expected={};ASSERT_EQ(nr_csirs_disc_synth_footprint(&c,&expected),0);expected.sym_mask|=1<<8;
    check(r.r,expected);printf("OTA geometry period=%d slots=%d occasions=256\n",p,cfg.n_slots);
  }
}
TEST(CsirsDisc, WrappedAndMissingSlots)
{
  auto c=resource(1);auto cfg=config(&c);Ring r;ASSERT_EQ(nr_csirs_disc_synth(&r.r,&cfg),0);
  for(int t=0;t<r.r.capacity;++t) if(t%7==0) r.r.meta[t].valid=0;
  for(int t=0;t<r.r.capacity/2;++t) {
    int other=r.r.capacity-1-t;std::swap(r.r.meta[t],r.r.meta[other]);
    std::swap_ranges(r.r.grid_base+t*r.r.slot_stride,r.r.grid_base+(t+1)*r.r.slot_stride,
                     r.r.grid_base+other*r.r.slot_stride);
  }
  nr_csirs_disc_fp_t expected={};ASSERT_EQ(nr_csirs_disc_synth_footprint(&c,&expected),0);check(r.r,expected);
}
TEST(CsirsDisc, MultipleResources)
{
  nr_csirs_disc_synth_resource_t cs[]={resource(1),resource(2)};
  cs[0].rb_start=0;cs[0].n_rb=9;cs[1].l0=8;cs[1].rb_start=12;cs[1].n_rb=13;cs[1].period=8;
  auto cfg=config(cs,2);cfg.n_slots=1280;Ring r;ASSERT_EQ(nr_csirs_disc_synth(&r.r,&cfg),0);
  auto e=exclusions(r.r.n_rb);nr_csirs_disc_fp_cfg_t fc={0,e.data(),1.03f};nr_csirs_disc_fp_t out[32]={};
  int n=nr_csirs_disc_footprints_cpu(&r.r,&fc,out,32);ASSERT_EQ(n,2);
  for(const auto &c:cs) {nr_csirs_disc_fp_t expected={};ASSERT_EQ(nr_csirs_disc_synth_footprint(&c,&expected),0);
    EXPECT_TRUE(same(out[0],expected) || same(out[1],expected));}
}
TEST(CsirsDisc, NoPilots)
{
  for(float snr:{5.0f,10.0f,20.0f}) for(float load:{0.0f,0.85f,1.0f}) {
    auto cfg=config(nullptr,0);cfg.load=load;cfg.snr_db=snr;Ring r;ASSERT_EQ(nr_csirs_disc_synth(&r.r,&cfg),0);
    auto e=exclusions(r.r.n_rb);nr_csirs_disc_fp_cfg_t fc={0,e.data(),1.03f};nr_csirs_disc_fp_t out[32];
    EXPECT_EQ(nr_csirs_disc_footprints_cpu(&r.r,&fc,out,32),0)<<"load="<<load<<" snr="<<snr;
  }
}
TEST(CsirsDisc, ExclusionsAndDefaultThreshold)
{
  auto c=resource(1);auto cfg=config(&c);Ring r;ASSERT_EQ(nr_csirs_disc_synth(&r.r,&cfg),0);
  auto e=exclusions(r.r.n_rb);nr_csirs_disc_fp_cfg_t fc={uint16_t(1<<4),e.data(),1.03f};nr_csirs_disc_fp_t out[32];
  EXPECT_EQ(nr_csirs_disc_footprints_cpu(&r.r,&fc,out,32),0);
  fc.excl_sym_mask_any=0;fc.min_strength=0;
  EXPECT_EQ(nr_csirs_disc_footprints_cpu(&r.r,&fc,out,32),0); // contract: default ratio 2
  fc.min_strength=1.03f;
  for(int k=0;k<12*r.r.n_rb;++k) e[4*12*r.r.n_rb+k]=1;
  EXPECT_EQ(nr_csirs_disc_footprints_cpu(&r.r,&fc,out,32),0);
}
TEST(CsirsDisc, BackendDisabled)
{
  unsetenv("ISAC_CSIRS_DISC");EXPECT_EQ(nr_csirs_disc_gpu_load(),nullptr);
  setenv("ISAC_CSIRS_DISC","1",1);setenv("ISAC_CSIRS_DISC_BACKEND","cpu",1);
  EXPECT_EQ(nr_csirs_disc_gpu_load(),nullptr);unsetenv("ISAC_CSIRS_DISC_BACKEND");
}
TEST(CsirsDisc, GpuMatchesCpu)
{
  setenv("ISAC_CSIRS_DISC","1",1);setenv("ISAC_CSIRS_DISC_BACKEND","gpu",1);
  const auto *ops=nr_csirs_disc_gpu_load();
  if(!ops || !ops->footprints) GTEST_SKIP()<<"CUDA module/device unavailable; run build_gpu/test_nr_csirs_disc_fp --gtest_filter=CsirsDisc.GpuMatchesCpu";
  // The public ops are circuit-breaker wrappers in the executable; dladdr on
  // them cannot locate the CUDA module. Use the loader's allocation hooks.
  auto alloc=nr_csirs_disc_alloc_unified;
  auto release=nr_csirs_disc_free_unified;
  ASSERT_NE(alloc,nullptr);ASSERT_NE(release,nullptr);
  for(int row:{1,2,4,6,11,16,18}) {
    auto c=resource(row);auto cfg=config(&c);Ring r;ASSERT_EQ(nr_csirs_disc_synth(&r.r,&cfg),0);
    auto e=exclusions(r.r.n_rb);nr_csirs_disc_fp_cfg_t fc={0,e.data(),1.03f};nr_csirs_disc_fp_t cpu[32]={},gpu[32]={};
    int n=nr_csirs_disc_footprints_cpu(&r.r,&fc,cpu,32);ASSERT_GE(n,0);
    ASSERT_EQ(ops->footprints(&r.r,&fc,gpu,32),n);
    ASSERT_GT(n,0);
    for(int i=0;i<n;++i) {EXPECT_TRUE(same(cpu[i],gpu[i]));EXPECT_FLOAT_EQ(cpu[i].strength,gpu[i].strength);}
    if(row==1) {
      size_t bytes=r.r.capacity*r.r.slot_stride*sizeof(int16_t);
      auto *managed=static_cast<int16_t *>(alloc(bytes));ASSERT_NE(managed,nullptr);
      memcpy(managed,r.r.grid_base,bytes);auto copy=r.r;copy.grid_base=managed;
      EXPECT_EQ(ops->footprints(&copy,&fc,gpu,32),n);
      for(int i=0;i<n;++i) {EXPECT_TRUE(same(cpu[i],gpu[i]));EXPECT_FLOAT_EQ(cpu[i].strength,gpu[i].strength);}
      release(managed);
    }
  }
}
// Uses the production U1 push path: only 640 raw slots remain after each wrap.
struct Stream {
  nr_csirs_disc_ring_t r={};
  explicit Stream(int nb,bool managed=false) {EXPECT_EQ(nr_csirs_disc_ring_init(&r,4,nb,20,640,managed),0);nr_csirs_disc_detect_reset();}
  ~Stream() {nr_csirs_disc_detect_reset();nr_csirs_disc_ring_free(&r);}
  void push(const nr_csirs_disc_ring_t &src) {
    for(int t=0;t<src.capacity;++t) {
      const int16_t *ants[4];
      for(int a=0;a<4;++a) ants[a]=nr_csirs_disc_ring_sym(&src,t,a,0);
      const auto &m=src.meta[t];
      EXPECT_GE(nr_csirs_disc_ring_push(&r,ants,12*r.n_rb,0,m.abs_slot,m.frame,m.slot),0);
    }
  }
};
static std::vector<nr_csirs_disc_fp_t> detect(Stream &r,nr_csirs_disc_fp_cfg_t &fc)
{
  nr_csirs_disc_fp_t out[32]={};int n=nr_csirs_disc_detect(&r.r,&fc,out,32);
  EXPECT_GE(n,0);return n<0?std::vector<nr_csirs_disc_fp_t>{}:std::vector<nr_csirs_disc_fp_t>(out,out+n);
}
static int stream_until(nr_csirs_disc_synth_cfg_t cfg,const nr_csirs_disc_fp_t *expected,int limit)
{
  Stream stream(cfg.n_rb);auto e=exclusions(cfg.n_rb);nr_csirs_disc_fp_cfg_t fc={0,e.data(),1.03f};
  uint32_t first=cfg.first_slot,seed=cfg.seed;
  for(int fed=0;fed<limit;fed+=100) {
    cfg.n_slots=100;cfg.first_slot=first+fed;cfg.seed=seed+fed*7919u;
    Ring batch;EXPECT_EQ(nr_csirs_disc_synth(&batch.r,&cfg),0);stream.push(batch.r);
    auto out=detect(stream,fc);
    if(!expected) {EXPECT_TRUE(out.empty())<<"slots="<<fed+100;}
    if(expected && out.size()==1 && same(out[0],*expected)) return fed+100;
  }
  return 0;
}
TEST(CsirsDisc, StatefulWeakTrs)
{
  for(int p:{5,10}) {
    auto c=resource(1);c.rb_start=0;c.n_rb=52;c.period=p;c.offset=2;c.power=1.2;
    nr_csirs_disc_synth_resource_t pair[]={c,c};pair[1].l0=8;
    auto cfg=config(pair,2);cfg.n_rb=217;
    nr_csirs_disc_fp_t expected={};ASSERT_EQ(nr_csirs_disc_synth_footprint(&c,&expected),0);expected.sym_mask|=1<<8;
    int slots=stream_until(cfg,&expected,4000);
    EXPECT_GT(slots,0);EXPECT_LE(slots,4000);
    printf("stateful weak TRS P=%d first exact batch slots=%d ms=%.1f\n",p,slots,slots*0.5);
  }
}
TEST(CsirsDisc, StatefulAllPeriods)
{
  for(int p:{4,5,8,10,16,20,32,40,64,80,160,320,640}) {
    auto c=resource(2);c.period=p;c.offset=p-1;c.rb_start=3;c.n_rb=9;
    auto cfg=config(&c);cfg.n_rb=16;
    nr_csirs_disc_fp_t expected={};ASSERT_EQ(nr_csirs_disc_synth_footprint(&c,&expected),0);
    int slots=stream_until(cfg,&expected,128*p);
    EXPECT_GT(slots,0)<<"P="<<p;
    if(p>=160) {EXPECT_GT(slots,640);}
    printf("stateful row2 P=%d first exact batch slots=%d ms=%.1f\n",p,slots,slots*0.5);
  }
}
TEST(CsirsDisc, StatefulNoPilots20000)
{
  // Pure DM-RS, PDSCH alone, and changing scheduled bandwidth with DM-RS.
  for(int kind=0;kind<3;++kind) {
    auto cfg=config(nullptr,0);cfg.n_rb=32;cfg.dmrs=kind!=1;cfg.dmrs_only=kind==0;
    cfg.ssb=false;cfg.load=kind==2?0.85f:1.0f;
    EXPECT_EQ(stream_until(cfg,nullptr,20000),0);
  }
}
TEST(CsirsDisc, StatefulResetAndCursor)
{
  auto c=resource(1);auto cfg=config(&c);cfg.n_slots=600;
  Ring batch;ASSERT_EQ(nr_csirs_disc_synth(&batch.r,&cfg),0);Stream stream(cfg.n_rb);stream.push(batch.r);
  auto e=exclusions(cfg.n_rb);nr_csirs_disc_fp_cfg_t fc={0,e.data(),1.03f};
  auto before=detect(stream,fc);ASSERT_FALSE(before.empty());
  auto again=detect(stream,fc);ASSERT_EQ(before.size(),again.size());
  for(size_t i=0;i<before.size();++i) {EXPECT_TRUE(same(before[i],again[i]));EXPECT_FLOAT_EQ(before[i].strength,again[i].strength);}
  // Invalid writes advance the cursor but contribute no evidence.
  stream.r.meta[0].valid=0;
  again=detect(stream,fc);ASSERT_EQ(before.size(),again.size());
  EXPECT_FLOAT_EQ(before[0].strength,again[0].strength);
  nr_csirs_disc_detect_reset();
  // Epoch owner resets the ring as well. No old evidence can survive.
  memset(stream.r.meta,0,640*sizeof(*stream.r.meta));stream.r.written=0;
  EXPECT_TRUE(detect(stream,fc).empty());
  cfg.resources=nullptr;cfg.n_resources=0;cfg.n_slots=100;cfg.first_slot+=600;
  Ring clean;ASSERT_EQ(nr_csirs_disc_synth(&clean.r,&cfg),0);stream.push(clean.r);
  EXPECT_TRUE(detect(stream,fc).empty());
  stream.r.written+=641;nr_csirs_disc_fp_t out[32];
  EXPECT_EQ(nr_csirs_disc_detect(&stream.r,&fc,out,32),-1);
}
TEST(CsirsDisc, StatefulMatchesFullHistoryAndResetSnapshot)
{
  auto c=resource(1);auto cfg=config(&c);cfg.n_slots=1300;cfg.load=0.85;
  Ring full;ASSERT_EQ(nr_csirs_disc_synth(&full.r,&cfg),0);
  // Absolute-slot gaps and invalid entries must not be mistaken for write counts.
  for(int t=0;t<cfg.n_slots;++t) if(t%7==0) full.r.meta[t].valid=0;
  Stream stream(cfg.n_rb);auto e=exclusions(cfg.n_rb);nr_csirs_disc_fp_cfg_t fc={0,e.data(),1.03f};
  for(int start=0;start<cfg.n_slots;start+=100) {
    auto batch=full.r;batch.capacity=100;batch.meta+=start;batch.grid_base+=start*batch.slot_stride;
    for(int t=0;t<100;++t) {
      auto one=batch;one.capacity=1;one.meta+=t;one.grid_base+=t*one.slot_stride;
      stream.push(one);
      stream.r.meta[(stream.r.written-1)%640].valid=one.meta[0].valid;
    }
    auto out=detect(stream,fc);
    auto prefix=full.r;prefix.capacity=start+100;prefix.written=prefix.capacity;
    nr_csirs_disc_fp_t ref[32]={};int n=nr_csirs_disc_footprints_cpu(&prefix,&fc,ref,32);
    ASSERT_EQ(out.size(),(size_t)n)<<"prefix="<<prefix.capacity;
    for(int i=0;i<n;++i) {EXPECT_TRUE(same(out[i],ref[i]));EXPECT_FLOAT_EQ(out[i].strength,ref[i].strength);}
  }
  nr_csirs_disc_detect_reset();auto out=detect(stream,fc);
  nr_csirs_disc_fp_t ref[32]={};int n=nr_csirs_disc_footprints_cpu(&stream.r,&fc,ref,32);
  ASSERT_EQ(out.size(),(size_t)n);
  for(int i=0;i<n;++i) {EXPECT_TRUE(same(out[i],ref[i]));EXPECT_FLOAT_EQ(out[i].strength,ref[i].strength);}
  // Changing normalization masks discards incompatible history.
  fc.excl_sym_mask_any=1<<4;EXPECT_TRUE(detect(stream,fc).empty());
}
TEST(CsirsDisc, StatefulGpuMatchesCpu)
{
  setenv("ISAC_CSIRS_DISC","1",1);setenv("ISAC_CSIRS_DISC_BACKEND","gpu",1);
  const auto *ops=nr_csirs_disc_gpu_load();
  if(!ops || !ops->footprints) GTEST_SKIP()<<"CUDA unavailable: run build_gpu/test_nr_csirs_disc_fp --gtest_filter=CsirsDisc.StatefulGpuMatchesCpu";
  ASSERT_NE(ops->normalize,nullptr);
  auto c=resource(2);c.period=160;c.offset=159;c.rb_start=3;c.n_rb=9;auto cfg=config(&c);cfg.n_rb=16;cfg.n_slots=100;
  auto e=exclusions(cfg.n_rb);nr_csirs_disc_fp_cfg_t fc={0,e.data(),1.03f};
  std::vector<std::vector<nr_csirs_disc_fp_t>> cpu;
  for(const char *backend:{"cpu","gpu","managed"}) {
    setenv("ISAC_CSIRS_DISC_BACKEND",!strcmp(backend,"cpu")?"cpu":"gpu",1);
    Stream stream(cfg.n_rb,!strcmp(backend,"managed"));
    for(int fed=0;fed<12000;fed+=100) {
      cfg.first_slot=1003+fed;cfg.seed=0x839412+fed*7919u;
      Ring batch;ASSERT_EQ(nr_csirs_disc_synth(&batch.r,&cfg),0);
      // Exercise the GPU primitive directly so a silent CPU fallback cannot pass parity.
      if(strcmp(backend,"cpu")) {
        std::vector<uint32_t> x((size_t)100*(14*12*cfg.n_rb+168));
        ASSERT_EQ(ops->normalize(&batch.r,&fc,x.data()),0);
        std::vector<uint32_t> reference(x.size());nr_disc_fp_normalize(&batch.r,&fc,reference.data());
        ASSERT_EQ(x,reference);
      }
      stream.push(batch.r);auto out=detect(stream,fc);
      if(!strcmp(backend,"cpu")) cpu.push_back(out);
      else {
        const auto &ref=cpu[fed/100];ASSERT_EQ(out.size(),ref.size())<<"fed="<<fed;
        for(size_t i=0;i<out.size();++i) {EXPECT_TRUE(same(out[i],ref[i]));EXPECT_FLOAT_EQ(out[i].strength,ref[i].strength);}
      }
    }
  }
  ASSERT_FALSE(cpu.back().empty());unsetenv("ISAC_CSIRS_DISC_BACKEND");
}
int main(int argc,char **argv)
{
  setvbuf(stdout,nullptr,_IONBF,0);setenv("ISAC_CSIRS_DISC","1",1);
  nr_csirs_disc_synth_init();::testing::InitGoogleTest(&argc,argv);return RUN_ALL_TESTS();
}
