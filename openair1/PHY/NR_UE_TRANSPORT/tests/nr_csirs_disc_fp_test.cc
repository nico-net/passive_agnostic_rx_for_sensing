/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_csirs_disc_synth.h"
#include <gtest/gtest.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <random>
#include <algorithm>
#include <dlfcn.h>
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
  if(!ops) GTEST_SKIP()<<"CUDA module/device unavailable; run build_gpu/test_nr_csirs_disc_fp --gtest_filter=CsirsDisc.GpuMatchesCpu";
  Dl_info info={};ASSERT_NE(dladdr(reinterpret_cast<void *>(ops->footprints),&info),0);
  void *module=dlopen(info.dli_fname,RTLD_NOW|RTLD_LOCAL);ASSERT_NE(module,nullptr);
  auto alloc=reinterpret_cast<void *(*)(size_t)>(dlsym(module,"nr_csirs_disc_gpu_alloc_unified"));
  auto release=reinterpret_cast<void (*)(void *)>(dlsym(module,"nr_csirs_disc_gpu_free_unified"));
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
  dlclose(module);
}
int main(int argc,char **argv)
{
  setvbuf(stdout,nullptr,_IONBF,0);setenv("ISAC_CSIRS_DISC","1",1);
  nr_csirs_disc_synth_init();::testing::InitGoogleTest(&argc,argv);return RUN_ALL_TESTS();
}
