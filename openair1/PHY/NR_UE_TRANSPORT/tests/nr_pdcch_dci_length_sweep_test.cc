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

/*! \file openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_dci_length_sweep_test.cc
 * \brief Phase 3 Technique C (roadmap artifact): the dci_length sweep's SELECTION logic, tested
 * against a synthetic scorer stub so the test needs no real candidate stream. This deliberately
 * tests the two false-accept traps this project has already paid for once (see
 * nr_pdcch_dci_length_sweep.c's own header comment): a length that only ever produces an
 * upper-8-bits-zero pass is NOT enough (1-in-256 test alone produced 42k chance passes on 10.9M
 * candidates in this project's prior work), and an INVARIANT payload at some length is a
 * degenerate polar fixed point, not a real length -- both must be rejected even though they would
 * pass a naive "any CRC-adjacent pass" check.
 */
#include <cstdint>
#include <gtest/gtest.h>

extern "C" {
#include "nr_pdcch_dci_length_sweep.h"
}

// Synthetic scorer: length 47 produces varying payloads with 3 hitting the bootstrap RNTI (a
// realistic, non-degenerate signal); length 39 produces the SAME payload every time (a degenerate
// polar fixed point -- must be rejected even though its upper-8-bits are chosen to look promising);
// every other length in range produces pure chance noise.
struct SweepFixture {
  int         calls_at_len[128] = {};
  static bool decode(int dci_length, int trial_idx, uint16_t* rnti_out, uint32_t* payload_hash_out, void* ctx) {
    SweepFixture* f = static_cast<SweepFixture*>(ctx);
    f->calls_at_len[dci_length]++;
    if (dci_length == 47) {
      *rnti_out = (trial_idx % 5 == 0) ? 0x4601 : (uint16_t)(0x1000 + trial_idx);
      *payload_hash_out = 1000u + (uint32_t)trial_idx;  // varies every trial
      return (trial_idx % 5 == 0) || ((trial_idx * 2654435761u) % 256 == 0);
    }
    if (dci_length == 39) {
      *rnti_out = 0x4601;             // looks tempting on RNTI alone
      *payload_hash_out = 42u;        // but INVARIANT -- degenerate fixed point
      return (trial_idx % 3 == 0);
    }
    // Everywhere else: pure 1/256 chance, never the bootstrap RNTI.
    *rnti_out = (uint16_t)(0x2000 + trial_idx);
    *payload_hash_out = 5000u + (uint32_t)trial_idx;
    return ((trial_idx * 2654435761u) % 256 == 0);
  }
};

TEST(DciLengthSweep, PicksTheLengthWithVaryingPayloadsAndBootstrapHits) {
  SweepFixture fx;
  const int best = nr_pdcch_dci_length_sweep(&SweepFixture::decode, &fx, /*min_len=*/30, /*max_len=*/70,
                                             /*bootstrap_rnti=*/0x4601);
  EXPECT_EQ(best, 47);
}

TEST(DciLengthSweep, RejectsAnInvariantPayloadDespiteMatchingTheBootstrapRnti) {
  // Regression guard for the exact trap this project has already hit: length 39 matches the
  // bootstrap RNTI on every trial, which a naive "does it hit the known RNTI" scorer would love --
  // but every trial decodes to the IDENTICAL payload_hash, which is the degenerate-fixed-point
  // signature this function must reject.
  SweepFixture fx;
  const int best = nr_pdcch_dci_length_sweep(&SweepFixture::decode, &fx, 30, 70, 0x4601);
  EXPECT_NE(best, 39);
}

TEST(DciLengthSweep, ReturnsMinusOneWhenNoLengthClearsSignificance) {
  // A scorer where NOTHING is ever real -- every length is pure chance noise.
  auto pure_noise = [](int, int trial_idx, uint16_t* rnti_out, uint32_t* payload_hash_out, void*) -> bool {
    *rnti_out = (uint16_t)(0x3000 + trial_idx);
    *payload_hash_out = 9000u + (uint32_t)trial_idx;
    return ((trial_idx * 2654435761u) % 256 == 0);
  };
  const int best = nr_pdcch_dci_length_sweep(pure_noise, nullptr, 30, 70, 0x9999 /* never seen */);
  EXPECT_EQ(best, -1);
}

int main(int argc, char** argv)
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
