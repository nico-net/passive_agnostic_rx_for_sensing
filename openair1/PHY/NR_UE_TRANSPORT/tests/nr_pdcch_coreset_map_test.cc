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

/*! \file openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_coreset_map_test.cc
 * \brief Phase 3 Technique A (roadmap artifact): full-carrier DM-RS correlation mapping, tested
 * with a SYNTHETIC symbol containing one genuinely occupied 6-RB window (built by regenerating
 * the exact same DM-RS this function itself uses to detect it -- a closed-loop test, deliberately
 * so: it proves the correlation MATH is self-consistent before any live capture is spent on it,
 * the same discipline nr_pdcch_blind_monitor_test.cc's own RawPayloadRoundTrip group uses).
 */
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>
#include <gtest/gtest.h>

extern "C" {
#include "PHY/impl_defs_top.h"
#include "common/config/config_userapi.h"
#include "common/utils/LOG/log.h"
#include "nr_pdcch_coreset_map.h"
// Real current signatures (openair1/PHY/NR_REFSIG/nr_refsig.h) -- nr_pdcch_dmrs_ref()'s third
// argument is an RB COUNT (it writes nb_rb_coreset*3 pilots), not a raw pilot-element count.
uint32_t* nr_gold_pdcch(int N_RB_DL, int symbols_per_slot, unsigned short scrambling_id, int slot, int symbol);
void nr_pdcch_dmrs_ref(const uint32_t* gold, c16_t* pilot, unsigned short nb_rb_coreset);
}

// LOG/CONFIG_LIB need these outside a full softmodem executable -- same convention as
// nr_pdcch_blind_monitor_test.cc / isac_sync_test.cc.
extern "C" configmodule_interface_t* uniqCfg = nullptr;
extern "C" void exit_function(const char* file, const char* function, const int line, const char* s, const int assert)
{
  if (s != nullptr) {
    fprintf(stderr, "%s:%d %s() Exiting: %s\n", file, line, function, s);
  }
  if (assert) {
    abort();
  } else {
    exit(EXIT_SUCCESS);
  }
}

TEST(CoresetMap, FindsOneSyntheticallyOccupiedWindowAboveNoise) {
  const int n_rb_carrier = 48;       // small carrier for a fast, deterministic test
  const int ofdm_symbol_size = 512;  // must exceed n_rb_carrier*12 with margin
  const int first_carrier_offset = 10;
  const uint16_t scrambling_id = 2;  // this project's PCI on the reference cell
  const int slot = 3, symbol = 0;

  std::vector<c16_t> rxdataF(ofdm_symbol_size, {0, 0});
  std::mt19937 rng(42);
  std::normal_distribution<double> noise(0.0, 8.0);
  for (auto& s : rxdataF) {
    s.r = (int16_t)std::lround(noise(rng));
    s.i = (int16_t)std::lround(noise(rng));
  }

  // Plant a real, correctly-generated PDCCH DM-RS at RB offset 18 (a 6-RB window not aligned to
  // either carrier edge, so the test cannot pass by an off-by-one accident at the boundary).
  const int occupied_rb_offset = 18;
  const int pilot_rb_count = occupied_rb_offset + 6;
  uint32_t* gold = nr_gold_pdcch(n_rb_carrier, 14, scrambling_id, slot, symbol);
  std::vector<c16_t> pilot(pilot_rb_count * 3);
  nr_pdcch_dmrs_ref(gold, pilot.data(), (unsigned short)pilot_rb_count);
  for (int rb = occupied_rb_offset; rb < occupied_rb_offset + 6; rb++) {
    for (int p = 0; p < 3; p++) {
      const int k = (first_carrier_offset + rb * 12 + 1 + 4 * p) % ofdm_symbol_size;
      // pilot[] is already conj(X); the received symbol at a real DM-RS RE is Y = X (unit
      // channel, no noise added on top of the ambient floor above) -> conj(pilot) recovers X.
      rxdataF[k].r = (int16_t)pilot[rb * 3 + p].r;
      rxdataF[k].i = (int16_t)(-pilot[rb * 3 + p].i);
    }
  }

  nr_pdcch_coreset_candidate_t candidates[16];
  const int n = nr_pdcch_coreset_map_scan(rxdataF.data(), ofdm_symbol_size, n_rb_carrier, first_carrier_offset,
                                          scrambling_id, slot, symbol, candidates, 16);
  ASSERT_GT(n, 0);
  EXPECT_EQ(candidates[0].rb_offset, occupied_rb_offset);
  EXPECT_GT(candidates[0].corr, 0.7);  // real DM-RS gives ~0.8-0.95 per this project's own measured range
}

TEST(CoresetMap, ReportsNothingOnPureNoise) {
  const int n_rb_carrier = 48, ofdm_symbol_size = 512, first_carrier_offset = 10;
  std::vector<c16_t> rxdataF(ofdm_symbol_size, {0, 0});
  std::mt19937 rng(7);
  std::normal_distribution<double> noise(0.0, 8.0);
  for (auto& s : rxdataF) {
    s.r = (int16_t)std::lround(noise(rng));
    s.i = (int16_t)std::lround(noise(rng));
  }
  nr_pdcch_coreset_candidate_t candidates[16];
  const int n = nr_pdcch_coreset_map_scan(rxdataF.data(), ofdm_symbol_size, n_rb_carrier, first_carrier_offset,
                                          2, 3, 0, candidates, 16);
  // Pure noise gives sqrt(pi)/(2*sqrt(18)) ~= 0.209 per trial (this project's own already-derived
  // figure for an 18-pilot CCE); the significance bar this function uses must sit well above that.
  EXPECT_EQ(n, 0);
}

int main(int argc, char** argv)
{
  logInit();
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
