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
 */

#include <cstdint>
#include <set>
#include <vector>
#include <gtest/gtest.h>
extern "C" {
#include "nr_dci11_pin.h"
}

// ---- (a): successive reseeds visit every candidate, for several n ----------------------------

TEST(Dci11Pin, RoundRobinAloneVisitsEveryCandidate) {
  for (int n : {3, 8, 10, 13}) {
    uint32_t cursor = 0;
    std::set<int> seen;
    for (int i = 0; i < n; i++) {
      const int idx = nr_dci11_pin_round_robin(&cursor, n);
      ASSERT_GE(idx, 0);
      ASSERT_LT(idx, n);
      seen.insert(idx);
    }
    EXPECT_EQ((int)seen.size(), n) << "n=" << n;
  }
}

// R32's actual bug: the ORIGINAL caller advanced an equivalent cursor every OCCASION (not just at
// reseed) and only consulted it once per DCI11_PIN_BLOCK_OCCASIONS occasions, so successive seeds
// landed gcd(block_occasions, n) apart -- e.g. n=8, block=50 -> only 2 of 8 candidates ever pinned.
// This drives the REAL select()/seed() pair through a full simulated capture (block_occasions=1,
// mirroring "the cursor must only move at reseed time") and checks every candidate gets pinned at
// least once, for the same n values the ruling named.
TEST(Dci11Pin, SuccessiveReseedsThroughSelectAndSeedVisitEveryCandidate) {
  for (int n : {3, 8, 10, 13}) {
    std::vector<uint16_t> layout_ids(n);
    for (int i = 0; i < n; i++) layout_ids[i] = (uint16_t)(100 + i); // arbitrary distinct ids
    nr_dci11_pin_t pin{};
    uint32_t cursor = 0;
    std::set<uint16_t> seen;
    // Mirrors the real caller's loop: the occasion that reseeds a pin uses the freshly-chosen
    // candidate DIRECTLY (never a second select() call on the same occasion); block_occasions=1
    // means the pin then survives exactly one further occasion before the NEXT select() call
    // reports -1 again, driving the next reseed -- so each iteration here is one reseed event.
    for (int reseed = 0; reseed < n; reseed++) {
      const int idx = nr_dci11_pin_select(&pin, /*cfg=*/1, layout_ids.data(), n, -1, -1,
                                          /*has_stats=*/false, 0, 0,
                                          /*block_occasions=*/1, /*giveup_trials=*/1000000);
      ASSERT_EQ(idx, -1) << "reseed " << reseed << " (n=" << n << ")";
      ASSERT_FALSE(pin.valid) << "a pin dropped by rotation must read invalid before reseeding";
      const int chosen = nr_dci11_pin_round_robin(&cursor, n);
      nr_dci11_pin_seed(&pin, /*cfg=*/1, layout_ids[chosen]);
      seen.insert(layout_ids[chosen]);
    }
    EXPECT_EQ((int)seen.size(), n) << "n=" << n << ": not every candidate was ever pinned";
  }
}

// ---- (b): settled/preferred bypass the pin -----------------------------------------------------

TEST(Dci11Pin, SettledBypassesThePinUnchanged) {
  const uint16_t layout_ids[3] = {10, 20, 30};
  nr_dci11_pin_t pin{};
  nr_dci11_pin_seed(&pin, /*cfg=*/1, /*layout=*/20);
  const int idx = nr_dci11_pin_select(&pin, /*cfg=*/1, layout_ids, 3, /*settled=*/1, /*preferred=*/-1,
                                      false, 0, 0, 50, 1000);
  EXPECT_EQ(idx, 1);
  EXPECT_TRUE(pin.valid);
  EXPECT_EQ(pin.layout, 20);
  EXPECT_EQ(pin.occ, 0u); // untouched, not even counted as an occasion on the pin
}

TEST(Dci11Pin, PreferredBypassesThePinUnchanged) {
  const uint16_t layout_ids[3] = {10, 20, 30};
  nr_dci11_pin_t pin{};
  nr_dci11_pin_seed(&pin, /*cfg=*/1, /*layout=*/10);
  const int idx = nr_dci11_pin_select(&pin, /*cfg=*/1, layout_ids, 3, /*settled=*/-1, /*preferred=*/2,
                                      false, 0, 0, 50, 1000);
  EXPECT_EQ(idx, 2);
  EXPECT_TRUE(pin.valid);
  EXPECT_EQ(pin.layout, 10);
  EXPECT_EQ(pin.occ, 0u);
}

// ---- (c): a cell-geometry change invalidates the pin -------------------------------------------

TEST(Dci11Pin, CfgChangeInvalidatesThePin) {
  const uint16_t layout_ids[2] = {5, 6};
  nr_dci11_pin_t pin{};
  nr_dci11_pin_seed(&pin, /*cfg=*/100, /*layout=*/5);
  ASSERT_TRUE(pin.valid);
  // Same cfg: the pin is used normally.
  EXPECT_EQ(nr_dci11_pin_select(&pin, 100, layout_ids, 2, -1, -1, false, 0, 0, 50, 1000), 0);
  // A real cell-geometry change (cfg differs): the pin must be dropped, even though `layout_ids`
  // still happens to contain the same layout id -- a different cfg means a different Technique-D
  // key for that id, so re-using it would silently mix evidence across geometries.
  const int idx = nr_dci11_pin_select(&pin, /*cfg=*/200, layout_ids, 2, -1, -1, false, 0, 0, 50, 1000);
  EXPECT_EQ(idx, -1);
  EXPECT_FALSE(pin.valid);
}

// ---- (d): trial-giveup and occasion-rotation both fire at their own thresholds -----------------

TEST(Dci11Pin, TrialGiveupFiresAtItsThreshold) {
  const uint16_t layout_ids[1] = {7};
  nr_dci11_pin_t pin{};
  nr_dci11_pin_seed(&pin, 1, 7);
  // Occasion-rotation bound set unreachably high so only the trial-count giveup can fire.
  const uint32_t block = 1000000, giveup = 1000;
  EXPECT_EQ(nr_dci11_pin_select(&pin, 1, layout_ids, 1, -1, -1, true, /*ok=*/0, /*tr=*/999, block, giveup), 0);
  EXPECT_TRUE(pin.valid) << "one short of the giveup threshold must not drop the pin";
  const int idx = nr_dci11_pin_select(&pin, 1, layout_ids, 1, -1, -1, true, /*ok=*/0, /*tr=*/1000, block, giveup);
  EXPECT_EQ(idx, -1);
  EXPECT_FALSE(pin.valid);
}

TEST(Dci11Pin, TrialGiveupNeverFiresWithAnySuccess) {
  const uint16_t layout_ids[1] = {7};
  nr_dci11_pin_t pin{};
  nr_dci11_pin_seed(&pin, 1, 7);
  // Way past the trial threshold, but with a nonzero pass count -- must not be judged wrong.
  const int idx = nr_dci11_pin_select(&pin, 1, layout_ids, 1, -1, -1, true, /*ok=*/1, /*tr=*/5000,
                                      1000000, 1000);
  EXPECT_EQ(idx, 0);
  EXPECT_TRUE(pin.valid);
}

TEST(Dci11Pin, OccasionRotationFiresAtItsThreshold) {
  const uint16_t layout_ids[1] = {7};
  nr_dci11_pin_t pin{};
  nr_dci11_pin_seed(&pin, 1, 7);
  // Trial-count giveup bound set unreachably high (has_stats=false) so only rotation can fire.
  const uint32_t block = 3, giveup = 1000000;
  EXPECT_EQ(nr_dci11_pin_select(&pin, 1, layout_ids, 1, -1, -1, false, 0, 0, block, giveup), 0); // occ=1
  EXPECT_TRUE(pin.valid);
  EXPECT_EQ(nr_dci11_pin_select(&pin, 1, layout_ids, 1, -1, -1, false, 0, 0, block, giveup), 0); // occ=2
  EXPECT_TRUE(pin.valid);
  const int idx = nr_dci11_pin_select(&pin, 1, layout_ids, 1, -1, -1, false, 0, 0, block, giveup); // occ=3
  EXPECT_EQ(idx, -1);
  EXPECT_FALSE(pin.valid);
}

// ---- Transient absence: a still-valid pin simply not offered this occasion must NOT be dropped --

TEST(Dci11Pin, TransientAbsenceLeavesTheValidPinAlone) {
  const uint16_t other_ids[2] = {1, 2}; // does not contain the pinned layout (99)
  nr_dci11_pin_t pin{};
  nr_dci11_pin_seed(&pin, 1, 99);
  const uint32_t occ_before = pin.occ;
  const int idx = nr_dci11_pin_select(&pin, 1, other_ids, 2, -1, -1, false, 0, 0, 50, 1000);
  EXPECT_EQ(idx, -1);
  EXPECT_TRUE(pin.valid) << "absence this occasion must not be treated as a verdict";
  EXPECT_EQ(pin.layout, 99);
  EXPECT_EQ(pin.occ, occ_before) << "an occasion the pin wasn't even offered on must not count against it";
}

// ---- nr_dci11_pin_is_valid(): the acquire-ordered accessor callers outside this file must use
// instead of reading pin->valid directly (nr_pdcch_blind_monitor_rt.c's RT/deferred-queue threads --
// see the header's threading note). Tracks the plain field through every state transition the other
// tests above already exercise via direct field reads.

TEST(Dci11Pin, IsValidAccessorTracksFieldThroughSeedAndSelectTransitions) {
  nr_dci11_pin_t pin{};
  EXPECT_FALSE(nr_dci11_pin_is_valid(&pin)) << "a never-seeded pin must read invalid";

  nr_dci11_pin_seed(&pin, /*cfg=*/1, /*layout=*/7);
  EXPECT_TRUE(nr_dci11_pin_is_valid(&pin));
  EXPECT_EQ(pin.valid, nr_dci11_pin_is_valid(&pin)) << "accessor must agree with the raw field";

  // Rotation (block_occasions=1) drops the pin on the very next select().
  const uint16_t layout_ids[1] = {7};
  const int idx = nr_dci11_pin_select(&pin, 1, layout_ids, 1, -1, -1, false, 0, 0,
                                      /*block_occasions=*/1, /*giveup_trials=*/1000000);
  EXPECT_EQ(idx, -1);
  EXPECT_FALSE(nr_dci11_pin_is_valid(&pin)) << "rotation must be visible through the accessor too";
  EXPECT_EQ(pin.valid, nr_dci11_pin_is_valid(&pin));

  // Reseeding after the drop must be visible again.
  nr_dci11_pin_seed(&pin, /*cfg=*/1, /*layout=*/8);
  EXPECT_TRUE(nr_dci11_pin_is_valid(&pin));
}
