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
#include <cstdio>
#include <vector>
#include <gtest/gtest.h>
extern "C" {
#include "nr_pdsch_xoverhead.h"
#include "openair2/LAYER2/NR_MAC_COMMON/nr_mac_common.h"
#include "common/utils/LOG/log.h"
#include "common/config/config_userapi.h"
configmodule_interface_t *uniqCfg = nullptr;
void exit_function(const char *,const char *,int,const char *,int) { std::abort(); }
}
class XOverhead : public ::testing::Test { void SetUp() override { nr_pdsch_xoverhead_reset(0); } };

struct Alloc { uint16_t Qm, R, rb, sym; };
static bool distinguishing(const Alloc &a, uint32_t t[4]) {
  const uint16_t oh[4] = {0, 6, 12, 18};
  for (int i = 0; i < 4; ++i) t[i] = nr_compute_tbs(a.Qm, a.R, a.rb, a.sym, 6, oh[i], 0, 1);
  return t[0] && t[1] != t[0] && t[2] != t[0] && t[3] != t[0];
}
/* Realistic single-layer allocations: MCS table 0/1 code rates, 1..40 PRB, common TDA lengths,
 * one front-loaded DM-RS symbol (6 REs/PRB). Reports how often xOverhead is TBS-distinguishable at
 * all -- the number that decides how fast the live elimination can confirm. */
static std::vector<Alloc> survey(int &n_total, int &n_all3, int &n_any) {
  static const uint16_t Rs[] = {120,193,308,379,449,526,602,679,753,340,438,466,517,567,616,666,719,772,822,873,910,948};
  std::vector<Alloc> good;
  n_total = n_all3 = n_any = 0;
  for (uint16_t Qm : {2, 4, 6}) for (uint16_t R : Rs) for (uint16_t rb = 1; rb <= 40; ++rb) for (uint16_t sym : {13, 12, 7, 4, 2}) {
    Alloc a{Qm, R, rb, sym}; uint32_t t[4];
    const bool all3 = distinguishing(a, t);
    if (!t[0]) continue;
    ++n_total; n_all3 += all3; n_any += (t[1] != t[0] || t[2] != t[0] || t[3] != t[0]);
    if (all3) good.push_back(a);
  }
  return good;
}
TEST_F(XOverhead, DistinguishableAllocationsExistAndAreCommon) {
  int n, a3, any; auto good = survey(n, a3, any);
  printf("xOverhead TBS-distinguishability over %d realistic allocations: all three alternatives refuted by %d (%.1f%%), "
         "at least one by %d (%.1f%%)\n", n, a3, 100.0 * a3 / n, any, 100.0 * any / n);
  uint32_t t[4]; Alloc small{2, 379, 7, 13}; distinguishing(small, t);
  printf("the 7-PRB QPSK R=379 13-symbol grant typical of this cell: TBS(0/6/12/18) = %u %u %u %u\n", t[0], t[1], t[2], t[3]);
  ASSERT_FALSE(good.empty());
  EXPECT_GT(100.0 * a3 / n, 25.0) << "if most allocations cannot refute anything, live confirmation would starve";
}
TEST_F(XOverhead, OneDistinguishingDecodeRefutesEveryAlternative) {
  int n, a3, any; auto good = survey(n, a3, any); ASSERT_FALSE(good.empty());
  const Alloc a = good.front();
  EXPECT_FALSE(nr_pdsch_xoverhead_observe(a.Qm, a.R, a.rb, a.sym, 6, 0, 0, 1, /*crc_ok=*/false)) << "a failed CRC is no evidence";
  EXPECT_EQ(nr_pdsch_xoverhead_snapshot().crc_ok_seen, 0u);
  EXPECT_TRUE(nr_pdsch_xoverhead_observe(a.Qm, a.R, a.rb, a.sym, 6, 0, 0, 1, true));
  const auto s = nr_pdsch_xoverhead_snapshot();
  EXPECT_TRUE(s.confirmed);
  EXPECT_EQ(s.refuted_by[1], 1u); EXPECT_EQ(s.refuted_by[2], 1u); EXPECT_EQ(s.refuted_by[3], 1u);
  EXPECT_EQ(s.indistinct[1], 0u);
  EXPECT_FALSE(nr_pdsch_xoverhead_observe(a.Qm, a.R, a.rb, a.sym, 6, 0, 0, 1, true)) << "confirmed only once";
}
TEST_F(XOverhead, PartialRefutationsAccumulateUntilAllThreeAreCovered) {
  /* The 7-PRB QPSK 13-symbol grant typical of this cell: TBS(0/6/12/18) = 72/72/64/64, so one such
   * decode refutes 12 and 18 but NOT 6 -- confirmation must wait for a grant that separates 0 from 6. */
  uint32_t t[4]; const Alloc partial{2, 379, 7, 13}; distinguishing(partial, t);
  ASSERT_EQ(t[1], t[0]); ASSERT_NE(t[2], t[0]); ASSERT_NE(t[3], t[0]);
  EXPECT_FALSE(nr_pdsch_xoverhead_observe(partial.Qm, partial.R, partial.rb, partial.sym, 6, 0, 0, 1, true));
  auto s = nr_pdsch_xoverhead_snapshot();
  EXPECT_FALSE(s.confirmed);
  EXPECT_EQ(s.refuted_by[1], 0u); EXPECT_EQ(s.indistinct[1], 1u);
  EXPECT_EQ(s.refuted_by[2], 1u); EXPECT_EQ(s.refuted_by[3], 1u);
  int n, a3, any; auto good = survey(n, a3, any); const Alloc a = good.front();
  EXPECT_TRUE(nr_pdsch_xoverhead_observe(a.Qm, a.R, a.rb, a.sym, 6, 0, 0, 1, true));
  EXPECT_TRUE(nr_pdsch_xoverhead_snapshot().confirmed);
}
TEST_F(XOverhead, IndistinguishableAllocationsRefuteNothingAndNeverConfirm) {
  /* Search for a real allocation where TBS(0) == TBS(x) for every alternative -- tiny grants at
   * the bottom of the TBS table quantise onto the same entry. */
  bool found = false;
  for (uint16_t rb = 1; rb <= 2 && !found; ++rb)
    for (uint16_t R : {120, 157, 193, 251}) {
      const uint32_t t0 = nr_compute_tbs(2, R, rb, 2, 6, 0, 0, 1);
      if (t0 && t0 == nr_compute_tbs(2, R, rb, 2, 6, 6, 0, 1) && t0 == nr_compute_tbs(2, R, rb, 2, 6, 12, 0, 1)
          && t0 == nr_compute_tbs(2, R, rb, 2, 6, 18, 0, 1)) {
        found = true;
        EXPECT_FALSE(nr_pdsch_xoverhead_observe(2, R, rb, 2, 6, 0, 0, 1, true));
        const auto s = nr_pdsch_xoverhead_snapshot();
        EXPECT_FALSE(s.confirmed) << "TBS-indistinct decode must not confirm";
        EXPECT_EQ(s.refuted_by[1] + s.refuted_by[2] + s.refuted_by[3], 0u);
        EXPECT_EQ(s.indistinct[1], 1u);
        break;
      }
    }
  ASSERT_TRUE(found) << "test needs at least one TBS-indistinct allocation in the search range";
}
TEST_F(XOverhead, ChangingTheAssumptionDiscardsOldEvidence) {
  int n, a3, any; auto good = survey(n, a3, any); const Alloc a = good.front();
  nr_pdsch_xoverhead_observe(a.Qm, a.R, a.rb, a.sym, 6, 0, 0, 1, true);
  ASSERT_TRUE(nr_pdsch_xoverhead_snapshot().confirmed);
  nr_pdsch_xoverhead_observe(a.Qm, a.R, a.rb, a.sym, 6, /*used=*/6, 0, 1, true); // decoder now runs with 6
  const auto s = nr_pdsch_xoverhead_snapshot();
  EXPECT_EQ(s.assumed, 6);
  EXPECT_EQ(s.crc_ok_seen, 1u) << "evidence for 0 is not evidence for 6";
}
int main(int argc, char **argv) { logInit(); testing::InitGoogleTest(&argc, argv); return RUN_ALL_TESTS(); }
