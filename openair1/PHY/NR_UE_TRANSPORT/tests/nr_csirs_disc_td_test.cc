/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include <gtest/gtest.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cmath>
#include "nr_csirs_disc.h"
extern "C" {
int csirs_td_parity(int,int,int,int);
int csirs_td_overflow_reset(void);
int csirs_td_concurrent_reset(void);
double csirs_td_benchmark(void);
void csirs_td_test_init(void);
int csirs_td_gpu_parity(void);
void nr_csirs_disc_fft_cpu(int,int,const int16_t *,int16_t *);
}
TEST(CsirsDiscTd, CpuBitExactWithSlotFep) {
  for(int mu: {0,1,2}) for(int rb: {216,217}) for(int sync: {0,1}) for(int cfo: {0,1,2})
    ASSERT_EQ(csirs_td_parity(mu,rb,sync,cfo),0) << mu << " " << rb << " " << sync << " " << cfo;
}
TEST(CsirsDiscTd, BoundedQueueDropsAndEpochReset) { EXPECT_EQ(csirs_td_overflow_reset(),0); }
TEST(CsirsDiscTd, ProducerAndResetProgressWhileWorkerFftIsPaused) { EXPECT_EQ(csirs_td_concurrent_reset(),0); }
TEST(CsirsDiscTd, DISABLED_ActorCopyBudget) {
  double us=csirs_td_benchmark();
  printf("T8 actor TD copy 4 RX / 217 RB / 4096 FFT: %.3f us/slot (10240 slots)\n",us);
  EXPECT_GT(us,0);EXPECT_LE(us,60);
}
TEST(CsirsDiscTd, DISABLED_GpuParityAndBudget) {
  setenv("ISAC_CSIRS_DISC","1",1);setenv("ISAC_CSIRS_DISC_BACKEND","gpu",1);
  csirs_td_test_init();
  const auto *ops=nr_csirs_disc_gpu_load();
  ASSERT_TRUE(ops && ops->fft && nr_csirs_disc_alloc_unified) << "GPU required";
  constexpr int n=4096, transforms=100*4*14;
  size_t count=size_t(n)*transforms*2;
  auto *in=(int16_t *)nr_csirs_disc_alloc_unified(count*2);
  auto *gpu=(int16_t *)nr_csirs_disc_alloc_unified(count*2);
  auto *cpu=(int16_t *)aligned_alloc(64,count*2);
  ASSERT_TRUE(in && gpu && cpu);
  uint32_t rng=123;
  for(size_t i=0;i<count;++i) {rng=rng*1664525+1013904223;in[i]=(rng>>22)-512;}
  nr_csirs_disc_fft_cpu(n,transforms,in,cpu);
  ASSERT_EQ(ops->fft(n,transforms,in,gpu),0); // warm plan
  int maxerr=0;double mse=0;
  for(size_t i=0;i<count;++i) {int e=abs(int(cpu[i])-gpu[i]);maxerr=std::max(maxerr,e);mse+=double(e)*e;}
  auto begin=std::chrono::steady_clock::now();
  for(int i=0;i<10;++i) ASSERT_EQ(ops->fft(n,transforms,in,gpu),0);
  double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count()/10;
  printf("T8 cuFFT 100 slots: %.3f ms/batch, %.0f slots/s, max error %d LSB, RMS %.3f LSB\n",ms,100000/ms,maxerr,sqrt(mse/count));
  // Stated fixed-point butterfly tolerance for bounded non-saturating +/-512 IQ.
  EXPECT_LE(maxerr,32);EXPECT_LE(sqrt(mse/count),8);EXPECT_LT(ms,50);
  nr_csirs_disc_free_unified(in);nr_csirs_disc_free_unified(gpu);free(cpu);
  EXPECT_EQ(csirs_td_gpu_parity(),0) << "Full CP/CFO/rotation/CRB pipeline, <=64 LSB";
}
