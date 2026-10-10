/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_pdcch_nid_occupancy.h"
#include <gtest/gtest.h>
#include <array>
#include <chrono>
#include <random>

namespace {
constexpr int fft=4096, rb=273, fco=3900;
using Grid=std::array<nr_pdcch_nid_iq_t,fft>;
bool take(nr_nid_occupancy_t &s,const Grid &v,int sym=0,unsigned floor=10) {
 return nr_nid_occupancy_take(&s,v.data(),fft,fco,12*rb,0,0,48,sym,3.981071706f,floor);
}
void fill(Grid &v,int first,int count,int amplitude) {
 for(int sc=12*first;sc<12*(first+count);sc++)v[(fco+sc)%fft]={(int16_t)amplitude,(int16_t)-amplitude};
}
}
TEST(NidOccupancy, EmptyAndCssOnlyKeepExactlyFloorPerSymbol) {
 for(int css:{0,48}) {
  nr_nid_occupancy_t s={};Grid v={};fill(v,0,css,12000);
  for(unsigned pct:{1u,10u,37u,100u})for(int block=0;block<3;block++) {
   int count[3]={};
   for(int n=0;n<100;n++)for(int sym=0;sym<3;sym++)count[sym]+=take(s,v,sym,pct);
   for(int n:count)EXPECT_EQ(n,(int)pct);
  }
 }
}
TEST(NidOccupancy, DedicatedAndFftWrap) {
 nr_nid_occupancy_t s={};Grid v={};fill(v,0,rb,100);fill(v,66,12,1000);
 for(int n=0;n<100;n++)EXPECT_TRUE(take(s,v));
 EXPECT_FLOAT_EQ(s.noise[0],20000.f);
}
TEST(NidOccupancy, NoiseFloorTracksBothDirections) {
 nr_nid_occupancy_t s={};Grid v={};fill(v,0,rb,100);
 for(int n=0;n<100;n++)take(s,v);
 for(int amplitude:{1000,10}) {
  fill(v,0,rb,amplitude);int count=0;
  for(int n=0;n<300;n++)count+=take(s,v);
  EXPECT_EQ(count,30);
  EXPECT_NEAR(s.noise[0],2.f*amplitude*amplitude,2.f*amplitude*amplitude*.0001f);
 }
}
TEST(NidOccupancy, NeverConditionsOnDmrsNoiseOrGoldSigns) {
 Grid a={},b={};std::mt19937 rng(25);std::normal_distribution<float> noise(0,400);
 nr_nid_occupancy_t sa={},sb={};
 int accepted=0;
 for(int n=0;n<1000;n++) {
  for(auto &x:a)x={(int16_t)noise(rng),(int16_t)noise(rng)};
  b=a;
  for(int m=1;m<12*rb;m+=4)b[(fco+m)%fft]={32767,-32768};
  bool aa=take(sa,a),bb=take(sb,b);EXPECT_EQ(aa,bb);accepted+=aa;
  EXPECT_FLOAT_EQ(sa.noise[0],sb.noise[0]);
 }
 EXPECT_EQ(accepted,100);
}
TEST(NidOccupancy, PartialPointAAndCssBoundary) {
 nr_nid_occupancy_t s={};Grid v={};fill(v,0,rb,100);
 // A CSS interval crossing the 6-RB boundary must not inflate the floor/peak.
 fill(v,3,13,16000);
 int count=0;
 for(int n=0;n<100;n++)count+=nr_nid_occupancy_take(&s,v.data(),fft,fco,12*rb,-7,4,13,2,4.f,10);
 EXPECT_EQ(count,10);
}
TEST(NidOccupancy, FullScalePowerDoesNotOverflow) {
 Grid v;for(auto &y:v)y={-32768,-32768};nr_nid_occupancy_t s={};int count=0;
 for(int n=0;n<100;n++)count+=take(s,v);
 EXPECT_EQ(count,10);EXPECT_FLOAT_EQ(s.noise[0],2147483648.f);
}
TEST(NidOccupancy, CostDiagnostic) {
 Grid v={};fill(v,0,rb,100);nr_nid_occupancy_t s={};
 auto start=std::chrono::steady_clock::now();int count=0;
 for(int n=0;n<10000;n++) {
  v[(fco+12*66)%fft].r=(int16_t)(100+(n&1)); // prevent loop-invariant IQ hoisting
  count+=take(s,v);
 }
 double us=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count()/10000;
 printf("NID_OCCUPANCY_CPU mean_us=%.3f offered=10000 taken=%d (shared sandbox; RT p99 deferred)\n",us,count);
 EXPECT_EQ(count,1000);
}
