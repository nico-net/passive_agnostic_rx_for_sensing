/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include <gtest/gtest.h>
#include <vector>
#include <random>
#include "nr_periodic_track.h"
static bool est(nr_ptrack_type_t ty,std::vector<uint64_t> h,unsigned &p,unsigned &o) {return nr_ptrack_estimate(ty,h.data(),h.size(),&p,&o);}
TEST(Ptrack,CleanPeriodAndOffset) {
  unsigned p,o;
  EXPECT_FALSE(est(NR_PTRACK_SRS,{7,27,47},p,o)); // below the minimum number of detections
  ASSERT_TRUE(est(NR_PTRACK_SRS,{7,27,47,67},p,o)); EXPECT_EQ(p,20u); EXPECT_EQ(o,7u);
  ASSERT_TRUE(est(NR_PTRACK_SRS,{3,4,5,6},p,o)); EXPECT_EQ(p,1u);
  ASSERT_TRUE(est(NR_PTRACK_SRS,{5,2565,5125,7685},p,o)); EXPECT_EQ(p,2560u); EXPECT_EQ(o,5u);
}
TEST(Ptrack,MissedOccasionsTolerated) {
  unsigned p,o;
  ASSERT_TRUE(est(NR_PTRACK_SRS,{7,27,67,87,107},p,o)); EXPECT_EQ(p,20u); EXPECT_EQ(o,7u);   // 1 of 6 occasions missed
  ASSERT_TRUE(est(NR_PTRACK_CSIRS,{3,43,123,163,203,243},p,o)); EXPECT_EQ(p,40u);            // 1 of 7 missed
  EXPECT_FALSE(est(NR_PTRACK_SRS,{7,27,107,187},p,o) && p==20);                              // 3 of 5 missed: not claimed as 20
}
TEST(Ptrack,StrayDetectionDoesNotBreakPeriod) {
  unsigned p,o;
  ASSERT_TRUE(est(NR_PTRACK_SRS,{7,27,47,53,67,87,107,127,147},p,o)); EXPECT_EQ(p,20u); EXPECT_EQ(o,7u);
}
TEST(Ptrack,LargestConsistentPeriodWins) {
  unsigned p,o;
  ASSERT_TRUE(est(NR_PTRACK_SRS,{0,40,80,120},p,o)); EXPECT_EQ(p,40u);  // 20, 10, 8, 5 ... also fit, but at a low hit rate
  ASSERT_TRUE(est(NR_PTRACK_CG,{9,137,265,393},p,o)); EXPECT_EQ(p,128u); // CG-only period
  EXPECT_FALSE(est(NR_PTRACK_CSIRS,{1,2,3,4},p,o)); // 1-slot recurrence is not a CSI-RS period
}
TEST(Ptrack,IllegalOrIrregularGapsNotPeriodic) {
  unsigned p,o;
  EXPECT_FALSE(est(NR_PTRACK_SRS,{7,10,13,16},p,o));
  EXPECT_FALSE(est(NR_PTRACK_SRS,{7,9,17,19},p,o));   // TDD gaps
  EXPECT_FALSE(est(NR_PTRACK_SRS,{7,9,17,19,27,29,37},p,o));
}
TEST(Ptrack,LearningAndRefinement) {
  nr_ptrack_t t;nr_ptrack_init(&t);nr_ptrack_counters_t c{};
  // every second period missed at first -> 20, then the in-between occasions show up -> re-learned as 10
  for(uint64_t s:{7,27,47,67}) nr_ptrack_hit(&t,NR_PTRACK_SRS,s,&c);
  ASSERT_TRUE(t.confirmed); EXPECT_EQ(t.period,20u); EXPECT_EQ(c.confirmed,1u);
  EXPECT_EQ(nr_ptrack_next(&t,67),87u); EXPECT_TRUE(nr_ptrack_due(&t,87)); EXPECT_FALSE(nr_ptrack_due(&t,88));
  for(uint64_t s:{77,87,97,107}) nr_ptrack_hit(&t,NR_PTRACK_SRS,s,&c);
  ASSERT_TRUE(t.confirmed); EXPECT_EQ(t.period,10u); EXPECT_EQ(t.offset,7u);
  EXPECT_GE(c.audit_hit,1u);
}
TEST(Ptrack,CountersAndDemotionOnMisses) {
  nr_ptrack_t t;nr_ptrack_init(&t);nr_ptrack_counters_t c{};
  for(uint64_t s:{7,27,47,67}) nr_ptrack_hit(&t,NR_PTRACK_SRS,s,&c);
  nr_ptrack_hit(&t,NR_PTRACK_SRS,87,&c); // predicted hit
  EXPECT_EQ(c.predicted,1u); EXPECT_EQ(c.hit,1u);
  nr_ptrack_miss(&t,NR_PTRACK_SRS,100,&c); EXPECT_EQ(c.predicted,1u); // 100 was not an occasion: ignored
  for(uint64_t s:{107,127,147}) {nr_ptrack_miss(&t,NR_PTRACK_SRS,s,&c); EXPECT_TRUE(t.confirmed);}
  nr_ptrack_miss(&t,NR_PTRACK_SRS,167,&c);
  EXPECT_FALSE(t.confirmed); EXPECT_EQ(c.miss,4u); EXPECT_EQ(c.demoted,1u); EXPECT_EQ(c.predicted,5u);
  EXPECT_EQ(t.n,0u); EXPECT_EQ(nr_ptrack_next(&t,167),0u);
}
TEST(Ptrack,OccasionalMissesDoNotDemote) {
  nr_ptrack_t t;nr_ptrack_init(&t);nr_ptrack_counters_t c{};
  for(uint64_t s:{0,10,20,30}) nr_ptrack_hit(&t,NR_PTRACK_SRS,s,&c);
  for(int k=4;k<60;k++) {
    uint64_t s=10ull*k;
    if(k%5==0) nr_ptrack_miss(&t,NR_PTRACK_SRS,s,&c); else nr_ptrack_hit(&t,NR_PTRACK_SRS,s,&c);
  }
  EXPECT_TRUE(t.confirmed); EXPECT_EQ(c.demoted,0u); EXPECT_EQ(c.miss,11u); EXPECT_EQ(c.predicted,56u);
}
TEST(Ptrack,SparseWindowDemotes) {
  nr_ptrack_t t;nr_ptrack_init(&t);nr_ptrack_counters_t c{};
  for(uint64_t s:{0,10,20,30}) nr_ptrack_hit(&t,NR_PTRACK_SRS,s,&c);
  // 1 hit per 4 occasions: never 4 misses in a row, yet the occasions are no longer used
  for(int k=4;k<40 && t.confirmed;k++) {
    uint64_t s=10ull*k;
    if(k%4==0) nr_ptrack_hit(&t,NR_PTRACK_SRS,s,&c); else nr_ptrack_miss(&t,NR_PTRACK_SRS,s,&c);
  }
  EXPECT_FALSE(t.confirmed); EXPECT_GE(c.demoted,1u);
}
TEST(Ptrack,StaleHistoryRestarts) {
  nr_ptrack_t t;nr_ptrack_init(&t);nr_ptrack_counters_t c{};
  for(uint64_t s:{7,27,47,67}) nr_ptrack_hit(&t,NR_PTRACK_SRS,s,&c);
  nr_ptrack_hit(&t,NR_PTRACK_SRS,67+NR_PTRACK_EXPIRY+1,&c);
  EXPECT_FALSE(t.confirmed); EXPECT_EQ(t.n,1u); EXPECT_EQ(c.demoted,1u);
  EXPECT_FALSE(nr_ptrack_due(&t,67+NR_PTRACK_EXPIRY+2));
}
TEST(Ptrack,DuplicateSlotIgnored) {
  nr_ptrack_t t;nr_ptrack_init(&t);
  nr_ptrack_hit(&t,NR_PTRACK_SRS,5,nullptr);nr_ptrack_hit(&t,NR_PTRACK_SRS,5,nullptr);
  EXPECT_EQ(t.n,1u);
}
TEST(Ptrack,NoisyDetectionStatistics) {
  std::mt19937 rng(1);
  const unsigned periods[]={5,10,20,40,80,160,320};
  int ok_trials=0,total=0;
  for(unsigned P:periods) for(int trial=0;trial<200;trial++) {
    total++;unsigned off=rng()%P;nr_ptrack_t t;nr_ptrack_init(&t);nr_ptrack_counters_t c{};
    std::bernoulli_distribution miss(0.15),stray(0.03);
    for(int k=0;k<40;k++) {
      uint64_t s=off+(uint64_t)P*k;
      if(!miss(rng)) nr_ptrack_hit(&t,NR_PTRACK_SRS,s,&c);
      else nr_ptrack_miss(&t,NR_PTRACK_SRS,s,&c);
      if(stray(rng)) { uint64_t x=s+1+rng()%(P>1?P-1:1); if(x%P!=off) nr_ptrack_hit(&t,NR_PTRACK_SRS,x,&c); }
    }
    // 4 misses in a row (0.05 % per occasion) legitimately demote; the pattern is relearned from the next detections
    // (a stray detection can fake a finer grid for a few occasions with only 4 detections; the next detections relearn the true one)
    if(t.confirmed && t.period%P==0 && t.offset%P==off) ++ok_trials;
  }
  EXPECT_GE(ok_trials,total*95/100);
}
TEST(Ptrack,RandomDetectionsNeverConfirm) {
  std::mt19937 rng(7);
  unsigned confirmed=0;
  for(int trial=0;trial<2000;trial++) {
    std::vector<uint64_t> h;uint64_t s=0;
    for(int k=0;k<8;k++) {s+=1+rng()%300;h.push_back(s);}
    unsigned p,o;confirmed+=nr_ptrack_estimate(NR_PTRACK_SRS,h.data(),h.size(),&p,&o);
  }
  EXPECT_EQ(confirmed,0u);
}
