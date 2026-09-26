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

// Standalone unit test for nr_harq_init_tx.h -- the per-(RNTI, HARQ pid) reserved-MCS retransmission
// record shared by the passive DL and UL decoders. No PHY/UE dependencies.

#include <gtest/gtest.h>

extern "C" {
#include "nr_harq_init_tx.h"
}

TEST(HarqInitTx, MissedInitialTxIsRefused)
{
  // Nothing was ever recorded for this (rnti, pid): a reserved-MCS grant on it must be refused, not
  // guessed at.
  nr_harq_init_tx_table_t t = {};
  nr_harq_init_tx_t out = {};
  EXPECT_FALSE(nr_harq_init_tx_lookup(&t, 0x4601, 3, 0, &out));
}

TEST(HarqInitTx, RecordThenLookupSameNdiSucceeds)
{
  nr_harq_init_tx_table_t t = {};
  nr_harq_init_tx_record(&t, 0x4601, 3, /*ndi=*/0, /*qm=*/6, /*nl=*/2, /*bg=*/1, /*tbs=*/12345, /*R=*/658);
  nr_harq_init_tx_t out = {};
  ASSERT_TRUE(nr_harq_init_tx_lookup(&t, 0x4601, 3, /*ndi=*/0, &out));
  EXPECT_EQ(out.tbs, 12345u);
  EXPECT_EQ(out.bg, 1);
  EXPECT_EQ(out.qm, 6);
  EXPECT_EQ(out.nl, 2);
  EXPECT_EQ(out.code_rate, 658u);
}

TEST(HarqInitTx, NdiToggleInvalidatesTheRecord)
{
  // A fresh initial transmission (NDI toggled) starts a new TB. The stale record from the PREVIOUS
  // TB on this same HARQ process must not be handed out for it.
  nr_harq_init_tx_table_t t = {};
  nr_harq_init_tx_record(&t, 0x4601, 3, /*ndi=*/0, 6, 2, 1, 12345, 658);
  nr_harq_init_tx_t out = {};
  EXPECT_FALSE(nr_harq_init_tx_lookup(&t, 0x4601, 3, /*ndi=*/1, &out));
}

TEST(HarqInitTx, ReservedMcsLookupAfterNdiToggleRerecordedSucceeds)
{
  // The realistic sequence: initial TX (ndi=0) recorded, a NEW initial TX starts (ndi=1) and is
  // itself resolvable so it re-records, and only THEN does a reserved-MCS retransmission (still
  // ndi=1) arrive -- it must see the NEW record, not the stale ndi=0 one.
  nr_harq_init_tx_table_t t = {};
  nr_harq_init_tx_record(&t, 0x4601, 3, 0, 6, 2, 1, 12345, 658);
  nr_harq_init_tx_record(&t, 0x4601, 3, 1, 4, 2, 2, 9000, 308);
  nr_harq_init_tx_t out = {};
  ASSERT_TRUE(nr_harq_init_tx_lookup(&t, 0x4601, 3, 1, &out));
  EXPECT_EQ(out.tbs, 9000u);
  EXPECT_EQ(out.bg, 2);
}

TEST(HarqInitTx, DifferentPidsOnSameRntiDoNotCollide)
{
  nr_harq_init_tx_table_t t = {};
  nr_harq_init_tx_record(&t, 0x4601, 0, 0, 6, 2, 1, 1000, 100);
  nr_harq_init_tx_record(&t, 0x4601, 1, 0, 4, 1, 2, 2000, 200);
  nr_harq_init_tx_t out0 = {}, out1 = {};
  ASSERT_TRUE(nr_harq_init_tx_lookup(&t, 0x4601, 0, 0, &out0));
  ASSERT_TRUE(nr_harq_init_tx_lookup(&t, 0x4601, 1, 0, &out1));
  EXPECT_EQ(out0.tbs, 1000u);
  EXPECT_EQ(out1.tbs, 2000u);
}

TEST(HarqInitTx, DifferentRntisSamePidDoNotCollide)
{
  nr_harq_init_tx_table_t t = {};
  nr_harq_init_tx_record(&t, 0x4601, 5, 0, 6, 2, 1, 1000, 100);
  nr_harq_init_tx_record(&t, 0x4602, 5, 0, 4, 1, 2, 2000, 200);
  nr_harq_init_tx_t out = {};
  ASSERT_TRUE(nr_harq_init_tx_lookup(&t, 0x4602, 5, 0, &out));
  EXPECT_EQ(out.tbs, 2000u);
  EXPECT_EQ(out.qm, 4);
}

TEST(HarqInitTx, WrapEvictsLeastRecentlyTouchedWhenFull)
{
  // Fill every slot with a distinct (rnti, pid), then record one MORE distinct (rnti, pid): the
  // table must not grow -- it evicts the least-recently-touched entry (rnti 0, pid 0, never
  // re-touched) and the new one must be findable afterward.
  nr_harq_init_tx_table_t t = {};
  for (int i = 0; i < NR_HARQ_INIT_TX_N; i++)
    nr_harq_init_tx_record(&t, (uint16_t)(0x1000 + i), 0, 0, 6, 2, 1, (uint32_t)(1000 + i), 100);
  // Touch every entry except the first so it is unambiguously the LRU victim.
  for (int i = 1; i < NR_HARQ_INIT_TX_N; i++)
    nr_harq_init_tx_record(&t, (uint16_t)(0x1000 + i), 0, 0, 6, 2, 1, (uint32_t)(1000 + i), 100);
  nr_harq_init_tx_record(&t, 0x9999, 0, 0, 6, 2, 1, 42, 100);

  nr_harq_init_tx_t out = {};
  EXPECT_TRUE(nr_harq_init_tx_lookup(&t, 0x9999, 0, 0, &out));
  EXPECT_FALSE(nr_harq_init_tx_lookup(&t, 0x1000, 0, 0, &out)); // evicted
  EXPECT_TRUE(nr_harq_init_tx_lookup(&t, 0x1001, 0, 0, &out));  // survives, was re-touched
}

TEST(HarqInitTx, RecordOnSameKeyUpdatesInPlaceRatherThanEvicting)
{
  nr_harq_init_tx_table_t t = {};
  for (int i = 0; i < NR_HARQ_INIT_TX_N; i++)
    nr_harq_init_tx_record(&t, (uint16_t)(0x1000 + i), 0, 0, 6, 2, 1, (uint32_t)(1000 + i), 100);
  // Re-record the FIRST key (a legitimate new initial TX on it): must update, not evict itself and
  // land somewhere else / duplicate.
  nr_harq_init_tx_record(&t, 0x1000, 0, 1, 4, 1, 2, 7777, 200);
  int used = 0;
  for (int i = 0; i < NR_HARQ_INIT_TX_N; i++)
    if (t.e[i].used)
      used++;
  EXPECT_EQ(used, NR_HARQ_INIT_TX_N); // still exactly N distinct entries, no growth
  nr_harq_init_tx_t out = {};
  ASSERT_TRUE(nr_harq_init_tx_lookup(&t, 0x1000, 0, 1, &out));
  EXPECT_EQ(out.tbs, 7777u);
}

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
