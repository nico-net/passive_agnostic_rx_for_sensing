/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this
 * file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.openairinterface.org/?page_id=698
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

#include <cstring>
#include <vector>
#include <gtest/gtest.h>
extern "C" {
#include "nr_hyp_sweep.h"
#include "common/utils/LOG/log.h"
#include "common/config/config_userapi.h"
}
extern "C" {
configmodule_interface_t *uniqCfg = nullptr;
void exit_function(const char *,const char *,int,const char *,int) { std::abort(); }
}
static nr_hyp_t hyp(int n) { nr_hyp_t h{}; h.len=sizeof(n); memcpy(h.bytes,&n,sizeof(n)); return h; }
static int value(const nr_hyp_t *h) { int n; memcpy(&n,h->bytes,sizeof(n)); return n; }
static bool even(const nr_hyp_t *h,void*) { return value(h)%2==0; }
static bool eq(const nr_hyp_t *a,const nr_hyp_t *b,const void *s,void*) {
  return value(a)%*static_cast<const int*>(s)==value(b)%*static_cast<const int*>(s);
}
static bool possible(const nr_hyp_t *h,const void *s,void*) { return value(h)!=*static_cast<const int*>(s); }
TEST(HypSweep, ConstraintsAndEverySample) {
  nr_hyp_t raw[]={hyp(0),hyp(2),hyp(4),hyp(1)};
  nr_hyp_sweep_state_t st;
  int two=2, three=3; const void *samples[]={&two,&three};
  ASSERT_EQ(nr_hyp_sweep_init(&st,raw,4,even,nullptr,eq,samples,1,nullptr),1);
  EXPECT_EQ(st.classes[0].members,3);
  ASSERT_EQ(nr_hyp_sweep_init(&st,raw,4,even,nullptr,eq,samples,2,nullptr),3);
  EXPECT_EQ(st.classes[0].members,1);
  EXPECT_EQ(nr_hyp_sweep_init(&st,raw,4,nullptr,nullptr,eq,nullptr,0,nullptr),4);
}
TEST(HypSweep, RefusalClearsOldWinnerAndChecksBounds) {
  nr_hyp_sweep_state_t st{};
  std::vector<nr_hyp_t> raw(65,hyp(0));
  EXPECT_EQ(nr_hyp_sweep_init(&st,raw.data(),65,nullptr,nullptr,nullptr,nullptr,0,nullptr),-2);
  EXPECT_EQ(nr_hyp_sweep_winner(&st),-1);
  EXPECT_EQ(st.n_classes,0);
  EXPECT_EQ(nr_hyp_sweep_init(&st,raw.data(),8193,nullptr,nullptr,nullptr,nullptr,0,nullptr),-1);
  raw[0].len=NR_HYP_BYTES+1;
  EXPECT_EQ(nr_hyp_sweep_init(&st,raw.data(),1,nullptr,nullptr,nullptr,nullptr,0,nullptr),-3);
}
TEST(HypSweep, GateSkipsWithoutScoringOrEliminatingAndChecksWinner) {
  nr_hyp_t raw[]={hyp(0),hyp(1)}, out;
  nr_hyp_sweep_state_t st; int rejected=0;
  ASSERT_EQ(nr_hyp_sweep_init(&st,raw,2,nullptr,nullptr,nullptr,nullptr,0,nullptr),2);
  EXPECT_EQ(nr_hyp_sweep_next(&st,&rejected,possible,nullptr,&out),1);
  EXPECT_EQ(st.classes[0].trials,0u);
  rejected=1;
  EXPECT_EQ(nr_hyp_sweep_next(&st,&rejected,possible,nullptr,&out),0);
  st.winner=1;
  EXPECT_EQ(nr_hyp_sweep_next(&st,&rejected,possible,nullptr,&out),-1);
}
TEST(HypSweep, OracleConvergesButTrueTiesAndZeroEvidenceDoNot) {
  for (int mode=0;mode<3;++mode) {
    nr_hyp_t raw[]={hyp(0),hyp(1)},out;
    nr_hyp_sweep_state_t st;
    ASSERT_EQ(nr_hyp_sweep_init(&st,raw,2,nullptr,nullptr,nullptr,nullptr,0,nullptr),2);
    for (int n=0;n<1200 && nr_hyp_sweep_winner(&st)<0;++n) {
      int c=nr_hyp_sweep_next(&st,nullptr,nullptr,nullptr,&out);
      /* Per-class trial count avoids the plan's round-robin/parity aliasing bug. */
      bool pass=st.classes[c].trials%4!=0;
      if (mode==0 && c==0) pass=false;
      if (mode==2) pass=false;
      nr_hyp_sweep_feed(&st,c,pass);
    }
    EXPECT_EQ(nr_hyp_sweep_winner(&st),mode==0?1:-1);
    if(mode==1) { EXPECT_EQ(st.classes[0].passes,st.classes[1].passes); }
  }
}
int main(int argc,char **argv) { logInit(); testing::InitGoogleTest(&argc,argv); return RUN_ALL_TESTS(); }
