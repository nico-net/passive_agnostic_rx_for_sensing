/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_gpu_budget.h"
#include "../../NR_UE_TRANSPORT/nr_pdcch_nid_chunks.h"
#include <gtest/gtest.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

TEST(GpuBudget, HeavyDiscoveryAndEverySlotRT) {
  nr_gpu_budget_test_reset(30);
  uint64_t discovery=0, done=0, rt_done=0, rt_jobs=0, cpu_jobs=0;
  bool active=false,rt=false;
  std::vector<uint64_t> completions;size_t oldest=0;
  // Ten seconds of mu=1 slots, a permanently backlogged discovery queue.
  // In-flight discovery does not refuse priority RT; it blocks the next slice.
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
      if(nr_gpu_budget_rt_begin()) {rt=true;rt_done=now+50;}
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
  EXPECT_EQ(cpu_jobs,0u);
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
  EXPECT_EQ(nr_gpu_budget_discovery_begin(2000000),1);
  nr_gpu_budget_discovery_end(2000000,0);
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
TEST(GpuBudget, NoCatchupBurstAndOverrunChargesDebt) {
  nr_gpu_budget_test_reset(30);
  ASSERT_EQ(nr_gpu_budget_discovery_begin(100000000),1);
  nr_gpu_budget_discovery_end(100001000,1000);
  EXPECT_EQ(nr_gpu_budget_discovery_begin(100001001),0);
  ASSERT_EQ(nr_gpu_budget_discovery_begin(100010000),1);
  nr_gpu_budget_discovery_end(100016000,6000);
  EXPECT_EQ(nr_gpu_budget_discovery_begin(100017000),0);
  ASSERT_EQ(nr_gpu_budget_discovery_begin(200000000),1);
  nr_gpu_budget_discovery_end(200000000,0);
  nr_gpu_budget_metrics_t m;nr_gpu_budget_metrics(&m);
  EXPECT_EQ(m.discovery_overruns,1u);
  nr_gpu_budget_fault();EXPECT_EQ(nr_gpu_budget_discovery_begin(200000001),-1);
  ASSERT_TRUE(nr_gpu_budget_rt_begin());nr_gpu_budget_rt_end();
}
TEST(GpuBudget, RTEntersInflightSliceAndBlocksTheNextOne) {
  nr_gpu_budget_test_reset(100);
  ASSERT_EQ(nr_gpu_budget_discovery_begin(0),1);
  ASSERT_TRUE(nr_gpu_budget_rt_begin());
  ASSERT_TRUE(nr_gpu_budget_rt_begin());
  nr_gpu_budget_discovery_end(10,10);
  EXPECT_EQ(nr_gpu_budget_discovery_begin(20),0);
  nr_gpu_budget_rt_end();
  EXPECT_EQ(nr_gpu_budget_discovery_begin(20),0);
  nr_gpu_budget_rt_end();
  ASSERT_EQ(nr_gpu_budget_discovery_begin(20),1);
  nr_gpu_budget_discovery_end(30,10);
  nr_gpu_budget_metrics_t m;nr_gpu_budget_metrics(&m);
  EXPECT_EQ(m.rt_gpu_fallbacks,0u);
}
TEST(GpuBudget, ConcurrentRTDoesNotLoseDiscoveryOwnership) {
  nr_gpu_budget_test_reset(100);
  ASSERT_EQ(nr_gpu_budget_discovery_begin(0),1);
  std::atomic<int> bad{0};std::vector<std::thread> threads;
  for(int t=0;t<4;++t)threads.emplace_back([&] {
    for(int i=0;i<10000;++i) {
      if(!nr_gpu_budget_rt_begin())++bad;
      else nr_gpu_budget_rt_end();
    }
  });
  for(auto &t:threads)t.join();
  EXPECT_EQ(bad.load(),0);
  EXPECT_EQ(nr_gpu_budget_discovery_begin(0),0);
  nr_gpu_budget_discovery_end(0,0);
  ASSERT_EQ(nr_gpu_budget_discovery_begin(0),1);
  nr_gpu_budget_discovery_end(0,0);
}
TEST(GpuBudget, HostCompletionDelayAlreadyPaysPacingDebt) {
  nr_gpu_budget_test_reset(30);
  ASSERT_EQ(nr_gpu_budget_discovery_begin(1000),1);
  nr_gpu_budget_discovery_end(1100,30);
  ASSERT_EQ(nr_gpu_budget_discovery_begin(1100),1);
  nr_gpu_budget_discovery_end(1130,30);
  EXPECT_EQ(nr_gpu_budget_discovery_begin(1199),0);
  ASSERT_EQ(nr_gpu_budget_discovery_begin(1200),1);
  nr_gpu_budget_discovery_end(1230,30);
}
TEST(GpuBudget, ChunkPlansCoverTailsAndAdversarialSurvivors) {
  for(int total: {1,31,32,33,64,336,3360,65536}) {
    for(int limit: {NR_NID_SWEEP_SNAPSHOTS,NR_NID_SECONDARY_IDS,
                   NR_NID_SCORE_HYPOTHESES,NR_NID_SCORE_SNAPSHOTS}) {
      int seen=0;
      for(int first=0;first<total;first+=limit) {
        int count=nr_nid_chunk_count(total,first,limit);
        EXPECT_GT(count,0);EXPECT_LE(count,limit);seen+=count;
      }
      EXPECT_EQ(seen,total);EXPECT_EQ(nr_nid_chunk_count(total,total,limit),0);
    }
  }
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

TEST(GpuBudget, RepeatedOutsideOccupancyCannotResetMaintain) {
  nr_gpu_discovery_duty_t s={};
  auto due=[&](uint64_t now,int verified,int outside) {
    return nr_gpu_discovery_due(&s,now,1,1,verified,outside,5000000,500000,10000000);
  };
  ASSERT_TRUE(due(1,0,0));
  EXPECT_EQ(s.mode,NR_GPU_DISC_MAINTAIN);
  for(int t=100; t<500000;t+=100)EXPECT_FALSE(due(t,0,(t/100)%2));
  EXPECT_TRUE(due(500001,0,1));
  EXPECT_FALSE(due(500002,1,0));EXPECT_EQ(s.mode,NR_GPU_DISC_PAUSED);
  EXPECT_TRUE(due(500003,0,1));EXPECT_EQ(s.mode,NR_GPU_DISC_MAINTAIN);
  EXPECT_FALSE(due(500004,0,1));
}
TEST(GpuBudget, LargeGrantCounter) {
  EXPECT_EQ(nr_gpu_budget_rt_large_active(),0);
  nr_gpu_budget_rt_large_begin();nr_gpu_budget_rt_large_begin();
  EXPECT_EQ(nr_gpu_budget_rt_large_active(),2);
  nr_gpu_budget_rt_large_end();nr_gpu_budget_rt_large_end();nr_gpu_budget_rt_large_end(); /* extra end clamps at 0 */
  EXPECT_EQ(nr_gpu_budget_rt_large_active(),0);
}

/* ---- G4: the process-wide discovery ticket ---- */
#include <chrono>
static void hold_us(int us) {std::this_thread::sleep_for(std::chrono::microseconds(us));}
TEST(DiscTicket, RecursiveAndReleaseWithoutAcquireIsNoop) {
  nr_gpu_disc_ticket_test_reset(30,nullptr);
  nr_gpu_disc_ticket_release();
  nr_gpu_disc_ticket_acquire(0);
  nr_gpu_disc_ticket_acquire_bulk(0);
  nr_gpu_disc_ticket_acquire(0);
  nr_gpu_disc_ticket_release();nr_gpu_disc_ticket_release();nr_gpu_disc_ticket_release();
  nr_gpu_disc_ticket_stats_t st;nr_gpu_disc_ticket_stats(0,&st);
  EXPECT_EQ(st.grants,1u);  /* the inner acquires only counted */
  nr_gpu_disc_ticket_acquire(0);nr_gpu_disc_ticket_release();
  nr_gpu_disc_ticket_stats(0,&st);EXPECT_EQ(st.grants,2u);
}
TEST(DiscTicket, AtMostOneHolderAmongCells) {
  nr_gpu_disc_ticket_test_reset(100,nullptr);
  std::atomic<int> in{0},max_in{0},grants{0};std::vector<std::thread> th;
  for(int cell=0;cell<4;++cell)th.emplace_back([&,cell] {
    for(int i=0;i<150;++i) {
      if(i&1)nr_gpu_disc_ticket_acquire_bulk(cell);else nr_gpu_disc_ticket_acquire(cell);
      int v=++in;int m=max_in.load();while(v>m && !max_in.compare_exchange_weak(m,v)) {}
      hold_us(50);--in;++grants;
      nr_gpu_disc_ticket_release();
    }
  });
  for(auto &t:th)t.join();
  EXPECT_EQ(max_in.load(),1);EXPECT_EQ(grants.load(),600);
  uint64_t g=0;for(int c=0;c<4;++c) {nr_gpu_disc_ticket_stats_t st;nr_gpu_disc_ticket_stats(c,&st);g+=st.grants;EXPECT_EQ(st.grants,150u);}
  EXPECT_EQ(g,600u);
}
TEST(DiscTicket, SingleCellIsNeverPacedOrBlocked) {
  nr_gpu_disc_ticket_test_reset(30,nullptr);
  auto t0=std::chrono::steady_clock::now();
  for(int i=0;i<200;++i) {nr_gpu_disc_ticket_acquire_bulk(0);hold_us(100);nr_gpu_disc_ticket_release();}
  const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t0).count();
  nr_gpu_disc_ticket_stats_t st;nr_gpu_disc_ticket_stats(0,&st);
  EXPECT_EQ(st.pace_us,0u);EXPECT_EQ(st.contended,0u);EXPECT_LT(ms,200*0.1*8+50); /* 200 x 100 us holds, generous scheduling slack */
}
TEST(DiscTicket, TwoCellsBulkIsPacedToTheDutyBudgetShortIsNot) {
  nr_gpu_disc_ticket_test_reset(30,nullptr); /* 30 % per cell, two cells: capped at 60 %, a bulk hold of h is followed by h x 40/60 of idle */
  nr_gpu_disc_ticket_test_rt_frac(0.9);      /* the RT work keeps the device 90 % busy: the cap is max(60, 100-90) */
  /* both cells have used the ticket: the pacing is on */
  nr_gpu_disc_ticket_acquire(0);nr_gpu_disc_ticket_release();
  nr_gpu_disc_ticket_acquire(1);nr_gpu_disc_ticket_release();
  auto t0=std::chrono::steady_clock::now();
  nr_gpu_disc_ticket_acquire_bulk(0);hold_us(30000);nr_gpu_disc_ticket_release(); /* pace 20 ms */
  nr_gpu_disc_ticket_acquire(1);nr_gpu_disc_ticket_release(); /* short: not paced */
  const double short_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t0).count();
  EXPECT_LT(short_ms,45.0);
  nr_gpu_disc_ticket_acquire_bulk(1);nr_gpu_disc_ticket_release(); /* bulk: waits the pace of the 20 ms hold */
  const double bulk_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t0).count();
  EXPECT_GE(bulk_ms,48.0);
  nr_gpu_disc_ticket_stats_t st;nr_gpu_disc_ticket_stats(0,&st);EXPECT_GE(st.pace_us,19000u);EXPECT_LE(st.pace_us,21000u);
}
TEST(DiscTicket, PreferringCellFirstThenLeastUsedThenArrival) {
  nr_gpu_disc_ticket_test_reset(100,nullptr);
  /* cell 2 used the device a lot, cell 1 a little */
  nr_gpu_disc_ticket_acquire(2);hold_us(30000);nr_gpu_disc_ticket_release();
  nr_gpu_disc_ticket_acquire(1);hold_us(1000);nr_gpu_disc_ticket_release();
  std::vector<int> order;std::mutex mu;
  nr_gpu_disc_ticket_acquire(3); /* main holds; the waiters queue behind it */
  auto waiter=[&](int cell) {return std::thread([&,cell] {
    nr_gpu_disc_ticket_acquire(cell);{std::lock_guard<std::mutex> g(mu);order.push_back(cell);}nr_gpu_disc_ticket_release();});};
  std::vector<std::thread> th;
  th.push_back(waiter(2));hold_us(5000);th.push_back(waiter(1));hold_us(5000);th.push_back(waiter(0));hold_us(5000);
  nr_gpu_budget_discovery_prefer_cell(2,1); /* cell 2 in ACQUIRE jumps the queue */
  nr_gpu_disc_ticket_release();
  for(auto &t:th)t.join();
  ASSERT_EQ(order.size(),3u);
  EXPECT_EQ(order[0],2);  /* prefer */
  EXPECT_EQ(order[1],0);  /* never used: least usage */
  EXPECT_EQ(order[2],1);
  EXPECT_EQ(nr_gpu_budget_discovery_preferred(),1);
  nr_gpu_budget_discovery_prefer_cell(2,0);EXPECT_EQ(nr_gpu_budget_discovery_preferred(),0);
}
/* the cap leaves the idle device free: paced only while the RT work is keeping it busy */
static uint64_t pace_after_two_cells(bool rt_busy) {
  nr_gpu_budget_test_reset(30);
  nr_gpu_disc_ticket_test_reset(30,nullptr);
  if(rt_busy) {EXPECT_EQ(nr_gpu_budget_rt_begin(),1);}
  const auto t0=std::chrono::steady_clock::now();
  while(std::chrono::steady_clock::now()-t0<std::chrono::milliseconds(900)) {
    for(int cell=0;cell<2;++cell) {nr_gpu_disc_ticket_acquire_bulk(cell);hold_us(1000);nr_gpu_disc_ticket_release();}
  }
  if(rt_busy)nr_gpu_budget_rt_end();
  nr_gpu_disc_ticket_stats_t a,b;nr_gpu_disc_ticket_stats(0,&a);nr_gpu_disc_ticket_stats(1,&b);
  return a.pace_us+b.pace_us;
}
TEST(DiscTicket, PacedOnlyWhileRtKeepsTheDeviceBusy) {
  EXPECT_EQ(pace_after_two_cells(false),0u);
  EXPECT_GT(pace_after_two_cells(true),0u);
}
