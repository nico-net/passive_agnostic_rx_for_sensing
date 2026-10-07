/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_gpu_budget.h"
#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include <vector>

TEST(GpuBudget, HeavyDiscoveryAndEverySlotRT) {
  nr_gpu_budget_test_reset(30);
  uint64_t discovery=0, done=0, rt_done=0, rt_jobs=0, cpu_jobs=0;
  bool active=false,rt=false;
  std::vector<uint64_t> completions;size_t oldest=0;
  // Ten seconds of mu=1 slots, a permanently backlogged discovery queue.
  // Fake 1 ms kernels overlap several slots: RT must immediately fall back.
  for(uint64_t now=0;now<10000000;now+=10) {
    if(active && now>=done) {
      nr_gpu_budget_discovery_end(now,1000);discovery+=1000;active=false;
      completions.push_back(now);
      while(oldest<completions.size() && completions[oldest]+1000000<now)++oldest;
      EXPECT_LE((completions.size()-oldest)*1000,300000u);
    }
    if(rt && now>=rt_done) {nr_gpu_budget_rt_end();rt=false;}
    if(now%500==0) {
      ++rt_jobs;
      if(nr_gpu_budget_rt_begin()) {ASSERT_FALSE(active);rt=true;rt_done=now+50;}
      else {++cpu_jobs;ASSERT_TRUE(active);}
    }
    if(!active && nr_gpu_budget_discovery_begin(now)==1) {
      ASSERT_FALSE(rt);active=true;done=now+1000;
    }
    if(now && now%1000000==0) {EXPECT_LE(discovery,now*30/100+1000);}
  }
  if(active)nr_gpu_budget_discovery_end(done,1000);
  if(rt)nr_gpu_budget_rt_end();
  EXPECT_EQ(rt_jobs,20000u);
  EXPECT_GT(cpu_jobs,0u);
  EXPECT_GT(discovery,2000000u);
  EXPECT_LE(discovery,3001000u);
  nr_gpu_budget_metrics_t m;nr_gpu_budget_metrics(&m);
  EXPECT_GT(m.discovery_yields,0u);
  EXPECT_EQ(m.rt_gpu_fallbacks,cpu_jobs);
}
struct Job {uint64_t now=0, ready=0;};
static int query(void *p) {auto *j=(Job *)p;j->now+=5;return j->now>=j->ready?0:600;}
static uint64_t clock_us(void *p) {return ((Job *)p)->now;}
TEST(GpuBudget, StalledRTUsesProductionDeadlineLoop) {
  nr_gpu_budget_test_reset(30);
  Job j{0,1000000};ASSERT_TRUE(nr_gpu_budget_rt_begin());
  EXPECT_EQ(nr_gpu_budget_rt_wait(query,&j,0,clock_us,&j),600);
  EXPECT_EQ(j.now,nr_gpu_rt_deadline_us());
  nr_gpu_budget_rt_end();
  EXPECT_EQ(nr_gpu_budget_discovery_begin(2000000),-1);
  nr_gpu_budget_metrics_t m;nr_gpu_budget_metrics(&m);
  EXPECT_EQ(m.rt_gpu_fallbacks,1u);
  EXPECT_EQ(m.rt_gpu_wait_max_us,250u);
}
TEST(GpuBudget, AbsoluteDeadlineIncludesSubmissionTime) {
  nr_gpu_budget_test_reset(30);
  Job j{245,249};
  EXPECT_EQ(nr_gpu_budget_rt_wait(query,&j,0,clock_us,&j),600);
  nr_gpu_budget_test_reset(30);j={0,50};
  EXPECT_EQ(nr_gpu_budget_rt_wait(query,&j,0,clock_us,&j),0);
  EXPECT_EQ(j.now,50u);
}
TEST(GpuBudget, NoCatchupBurstAndOverrunFailsClosed) {
  nr_gpu_budget_test_reset(30);
  ASSERT_EQ(nr_gpu_budget_discovery_begin(100000000),1);
  nr_gpu_budget_discovery_end(100001000,1000);
  EXPECT_EQ(nr_gpu_budget_discovery_begin(100001001),0);
  ASSERT_EQ(nr_gpu_budget_discovery_begin(100010000),1);
  nr_gpu_budget_discovery_end(100016000,6000);
  EXPECT_EQ(nr_gpu_budget_discovery_begin(200000000),-1);
  ASSERT_TRUE(nr_gpu_budget_rt_begin());nr_gpu_budget_rt_end();
}
TEST(GpuBudget, ConcurrentAdmissionNeverOverlapsRTAndDiscovery) {
  nr_gpu_budget_test_reset(100);
  std::atomic<int> disc{0},rt{0},bad{0};
  std::vector<std::thread> workers;
  for(int t=0;t<4;++t)workers.emplace_back([&,t] {
    for(int i=0;i<10000;++i) {
      if(t<2) {
        if(nr_gpu_budget_rt_begin()) {
          rt.fetch_add(1);if(disc.load())bad.fetch_add(1);
          rt.fetch_sub(1);nr_gpu_budget_rt_end();
        }
      } else if(nr_gpu_budget_discovery_begin(0)==1) {
        disc.fetch_add(1);if(rt.load() || disc.load()!=1)bad.fetch_add(1);
        disc.fetch_sub(1);nr_gpu_budget_discovery_end(0,0);
      }
    }
  });
  for(auto &t:workers)t.join();
  EXPECT_EQ(bad.load(),0);
}
TEST(GpuBudget, AcquireMaintainPauseEpochOccupancyAndRecheck) {
  nr_gpu_discovery_duty_t s={};
  auto due=[&](uint64_t now,int epoch,int published,int verified,int unexplained) {
    return nr_gpu_discovery_due(&s,now,epoch,published,verified,unexplained,5000000,500000,10000000);
  };
  EXPECT_TRUE(due(1,1,0,0,0));EXPECT_TRUE(due(100,1,0,0,0));
  EXPECT_TRUE(due(200,1,1,0,0));EXPECT_FALSE(due(300,1,1,0,0));
  EXPECT_TRUE(due(500200,1,1,0,0));
  EXPECT_FALSE(due(500300,1,1,1,0));
  EXPECT_FALSE(due(1000000,1,1,1,0));
  EXPECT_TRUE(due(10500300,1,1,1,0));
  EXPECT_TRUE(due(11000000,1,1,1,1));
  EXPECT_TRUE(due(11000001,2,0,0,0));
  EXPECT_TRUE(due(16000002,2,0,0,0));
  EXPECT_FALSE(due(16000100,2,0,0,0));
}
