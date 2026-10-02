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
/* BC9: DL DCI history ring, per-world occupant check (certified flag) and deterministic DCI-adjacency k0 exclusions. */
#include <cstdlib>
#include <memory>
#include <gtest/gtest.h>
extern "C" {
#include "nr_dci_history.h"
#include "common/utils/LOG/log.h"
#include "common/config/config_userapi.h"
configmodule_interface_t *uniqCfg = nullptr;
void exit_function(const char *, const char *, int, const char *, int) { std::abort(); }
/* nr_mac_common.c (Qm / code-rate tables) references it from code this test never runs; same stub as nr_pdcch_blind_monitor_test.cc */
void *get_softmodem_params(void)
{
  static char p[4096];
  return p;
}
}

static const uint64_t kCfg = 0xC0FFEE;
static nr_dci_hist_entry_t dci(uint32_t slot, uint8_t tda, uint16_t start_rb = 0, uint16_t num_rb = 50, uint8_t mcs = 9,
                               uint8_t rv = 0, uint16_t rnti = 0x1234)
{
  nr_dci_hist_entry_t e{};
  e.abs_slot = slot;
  e.cfg = kCfg;
  e.rnti = rnti;
  e.dci11 = true;
  e.tda = tda;
  e.mcs = mcs;
  e.rv = rv;
  e.start_rb = start_rb;
  e.num_rb = num_rb;
  e.dmrs_ports = 1;
  e.n_cdm = 1;
  return e;
}
static std::unique_ptr<nr_dci_hist_t> ring(uint32_t period = 20480)
{
  auto h = std::make_unique<nr_dci_hist_t>();
  nr_dci_hist_init(h.get(), period);
  return h;
}
static void push(nr_dci_hist_t *h, nr_dci_hist_entry_t e) { nr_dci_hist_push(h, &e); }
static const nr_dci_geom_t kGeo = {.nb_symb = 13, .dmrs_mask = 0x804, .dmrs_type = 0, .nl = 1, .xoh = 0};

TEST(DciHistory, DciHistoryRingOrderAndEviction)
{
  auto h = ring();
  for (uint32_t s = 0; s < 70; s++)
    push(h.get(), dci(s, 0, (uint16_t)s));
  nr_dci_hist_entry_t out[8];
  EXPECT_EQ(nr_dci_hist_at(h.get(), 0x1234, 69, out, 8), 1);
  EXPECT_EQ(out[0].start_rb, 69);
  EXPECT_EQ(nr_dci_hist_at(h.get(), 0x1234, 5, out, 8), 0); /* evicted: depth 64 */
  EXPECT_EQ(nr_dci_hist_at(h.get(), 0x1234, 6, out, 8), 1);  /* the oldest kept */
  /* newest first */
  nr_dci_hist_entry_t near[16];
  ASSERT_EQ(nr_dci_hist_near(h.get(), 0x1234, 60, 2, near, 16), 5);
  EXPECT_EQ(near[0].abs_slot, 62u);
  EXPECT_EQ(near[4].abs_slot, 58u);
  /* window: one entry far in the future hides everything older than 64 slots before it */
  push(h.get(), dci(200, 0));
  EXPECT_EQ(nr_dci_hist_at(h.get(), 0x1234, 69, out, 8), 0);
  EXPECT_EQ(nr_dci_hist_at(h.get(), 0x1234, 200, out, 8), 1);
  /* wrap of the slot numbering: 20479 and 0 are adjacent */
  auto w = ring(20480);
  push(w.get(), dci(20479, 0));
  push(w.get(), dci(0, 0));
  EXPECT_EQ(nr_dci_hist_diff(w.get(), 0, 20479), 1);
  EXPECT_EQ(nr_dci_hist_at(w.get(), 0x1234, 20479, out, 8), 1);
  /* LRU over RNTIs: the 17th RNTI evicts the least recently written one */
  auto r = ring();
  for (uint16_t k = 0; k < NR_DCI_HIST_RNTIS; k++)
    push(r.get(), dci(10, 0, 0, 50, 9, 0, (uint16_t)(0x100 + k)));
  push(r.get(), dci(11, 0, 0, 50, 9, 0, 0x100)); /* refresh the first */
  push(r.get(), dci(12, 0, 0, 50, 9, 0, 0x999));
  EXPECT_EQ(nr_dci_hist_at(r.get(), 0x100, 11, out, 8), 1);
  EXPECT_EQ(nr_dci_hist_at(r.get(), 0x101, 10, out, 8), 0); /* the LRU victim */
  EXPECT_EQ(nr_dci_hist_at(r.get(), 0x999, 12, out, 8), 1);
}

TEST(DciHistory, IncompatibleObservedNeighbourCertifies)
{
  auto h = ring();
  const nr_dci_hist_entry_t g = dci(100, 0, 0, 50);
  nr_dci_hist_push(h.get(), &g);
  /* leader k0 = 1, sibling 0: the world "truth is 0" puts the row-0 DCI of slot 101 on slot 101 */
  push(h.get(), dci(101, 0, 0, 20)); /* different PRBs */
  EXPECT_TRUE(nr_dci_hist_k0_certified(h.get(), &g, 1, 0x3, 0x1, &kGeo, nullptr, nullptr));
  /* leader 0, sibling 1: occupant is the row-0 DCI of slot 99 */
  push(h.get(), dci(99, 0, 0, 50, 9, 1)); /* different rv */
  EXPECT_TRUE(nr_dci_hist_k0_certified(h.get(), &g, 0, 0x2, 0x1, &kGeo, nullptr, nullptr));
}

TEST(DciHistory, MissedNeighbourDciIsAmbiguous)
{
  auto h = ring();
  const nr_dci_hist_entry_t g = dci(100, 0);
  nr_dci_hist_push(h.get(), &g);
  EXPECT_FALSE(nr_dci_hist_k0_certified(h.get(), &g, 1, 0x3, 0x1, &kGeo, nullptr, nullptr));
  /* a DCI of ANOTHER (uncertified) row in the slot proves nothing */
  push(h.get(), dci(101, 2, 0, 20));
  EXPECT_FALSE(nr_dci_hist_k0_certified(h.get(), &g, 1, 0x3, 0x1, &kGeo, nullptr, nullptr));
  /* two siblings: one observed incompatible occupant is not enough */
  push(h.get(), dci(101, 0, 0, 20));
  EXPECT_TRUE(nr_dci_hist_k0_certified(h.get(), &g, 1, 0x1 | 0x2, 0x1, &kGeo, nullptr, nullptr));
  EXPECT_FALSE(nr_dci_hist_k0_certified(h.get(), &g, 1, 0x1 | 0x2 | 0x4, 0x1, &kGeo, nullptr, nullptr)); /* k=2: slot 99 unseen */
}

TEST(DciHistory, CompatibleNeighbourNotCertified)
{
  auto h = ring();
  const nr_dci_hist_entry_t g = dci(100, 0);
  nr_dci_hist_push(h.get(), &g);
  push(h.get(), dci(101, 0)); /* identical allocation: the trap is possible */
  EXPECT_FALSE(nr_dci_hist_k0_certified(h.get(), &g, 1, 0x3, 0x7, &kGeo, nullptr, nullptr));
  /* HARQ pid / NDI differences are not computation differences */
  auto h2 = ring();
  nr_dci_hist_push(h2.get(), &g);
  nr_dci_hist_entry_t x = dci(101, 0);
  x.harq_pid = 5;
  x.ndi = 1;
  nr_dci_hist_push(h2.get(), &x);
  EXPECT_FALSE(nr_dci_hist_k0_certified(h2.get(), &g, 1, 0x3, 0x1, &kGeo, nullptr, nullptr));
  /* one compatible occupant among several in the slot spoils it */
  auto h3 = ring();
  nr_dci_hist_push(h3.get(), &g);
  push(h3.get(), dci(101, 0, 0, 20));
  push(h3.get(), dci(101, 0));
  EXPECT_FALSE(nr_dci_hist_k0_certified(h3.get(), &g, 1, 0x3, 0x1, &kGeo, nullptr, nullptr));
}

TEST(DciHistory, CrossTableEquivalentIsCompatible)
{
  /* TS 38.214 Table 5.1.3.1-1 MCS 2 = Table 5.1.3.1-2 MCS 1 (Qm 2, R 193/1024): same computation on the same PRBs */
  const nr_dci_hist_entry_t g = dci(100, 0, 0, 50, 2), x = dci(101, 0, 0, 50, 1);
  EXPECT_FALSE(nr_dci_hist_incompatible(&g, &x, 0x3, &kGeo)); /* tables {qam64, qam256} alive */
  EXPECT_TRUE(nr_dci_hist_incompatible(&g, &x, 0x1, &kGeo));  /* only qam64 alive: MCS 2 vs 1 differ */
  auto h = ring();
  nr_dci_hist_push(h.get(), &g);
  nr_dci_hist_push(h.get(), &x);
  EXPECT_FALSE(nr_dci_hist_k0_certified(h.get(), &g, 1, 0x3, 0x3, &kGeo, nullptr, nullptr));
  EXPECT_TRUE(nr_dci_hist_k0_certified(h.get(), &g, 1, 0x3, 0x1, &kGeo, nullptr, nullptr));
  /* without the shared geometry (other row) MCS is never a difference */
  EXPECT_FALSE(nr_dci_hist_incompatible(&g, &x, 0x1, nullptr));
}

TEST(DciHistory, EmptySiblingSetNeverCertifies)
{
  auto h = ring();
  const nr_dci_hist_entry_t g = dci(100, 0);
  nr_dci_hist_push(h.get(), &g);
  push(h.get(), dci(99, 0, 0, 20));
  push(h.get(), dci(101, 0, 0, 20));
  EXPECT_FALSE(nr_dci_hist_k0_certified(h.get(), &g, 1, 0x0, 0x1, &kGeo, nullptr, nullptr));
  EXPECT_FALSE(nr_dci_hist_k0_certified(h.get(), &g, 1, 0x2, 0x1, &kGeo, nullptr, nullptr)); /* only the leader itself */
}

/* row k0 oracle for the tests: tda 2 certified k0 = 0 (e.g. by the TDD rule), every other row {0,1} */
static uint64_t row_k0_tda2_cert(void *, uint64_t, uint16_t, uint8_t tda) { return tda == 2 ? 0x1 : 0x3; }
static uint64_t row_k0_none(void *, uint64_t, uint16_t, uint8_t) { return 0x3; }

TEST(DciHistory, CertifiedNeighbourRowRulesOutK0)
{
  /* BC7-review example: slot-7 tda2 DCI with k0 certified 0 occupies slot 7, so the slot-6 tda0 grant cannot have k0 = 1 */
  auto h = ring();
  const nr_dci_hist_entry_t g6 = dci(6, 0), x7 = dci(7, 2, 0, 20); /* the mixed-slot row: fewer PRBs */
  nr_dci_hist_push(h.get(), &g6);
  nr_dci_hist_push(h.get(), &x7);
  uint64_t forbid[NR_DCI_HIST_ROWS] = {0};
  EXPECT_EQ(nr_dci_hist_adj_exclusions(h.get(), &x7, row_k0_tda2_cert, nullptr, forbid), 1);
  EXPECT_EQ(forbid[0], UINT64_C(1) << 1);
  EXPECT_EQ(forbid[2], 0u);
  /* the same conclusion when the tda0 DCI is the newer one */
  uint64_t f2[NR_DCI_HIST_ROWS] = {0};
  EXPECT_EQ(nr_dci_hist_adj_exclusions(h.get(), &g6, row_k0_tda2_cert, nullptr, f2), 1);
  EXPECT_EQ(f2[0], UINT64_C(1) << 1);
  /* nothing certified: nothing excluded */
  uint64_t f3[NR_DCI_HIST_ROWS] = {0};
  EXPECT_EQ(nr_dci_hist_adj_exclusions(h.get(), &x7, row_k0_none, nullptr, f3), 0);
  /* another RNTI or configuration never excludes */
  auto o = ring();
  nr_dci_hist_entry_t g6b = dci(6, 0);
  g6b.cfg = 0xBAD;
  nr_dci_hist_push(o.get(), &g6b);
  nr_dci_hist_push(o.get(), &x7);
  uint64_t f4[NR_DCI_HIST_ROWS] = {0};
  EXPECT_EQ(nr_dci_hist_adj_exclusions(o.get(), &x7, row_k0_tda2_cert, nullptr, f4), 0);
  /* certified-flag side: the certified tda2 occupant of slot 7 makes a k0=1 slot-6 tda0 trap need an accident */
  EXPECT_TRUE(nr_dci_hist_k0_certified(h.get(), &g6, 1, 0x3, 0x1, &kGeo, row_k0_tda2_cert, nullptr));
  EXPECT_FALSE(nr_dci_hist_k0_certified(h.get(), &g6, 1, 0x3, 0x1, &kGeo, row_k0_none, nullptr)); /* uncertified row: ambiguous */
}
int main(int argc, char **argv) { logInit(); testing::InitGoogleTest(&argc, argv); return RUN_ALL_TESTS(); }
