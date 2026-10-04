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
#include <cstring>
#include <memory>
#include <gtest/gtest.h>
#include "nr_td_test_baseline.h"
extern "C" {
#include "nr_dci_history.h"
#include "nr_passive_cfg_epoch.h"
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
  nr_dci_hist_entry_t g6 = dci(6, 0), x7 = dci(7, 2, 0, 20); /* the mixed-slot row: fewer PRBs */
  g6.confirmed = x7.confirmed = true; /* BC9d: only CONFIRMED DCIs exclude (AdjacencyUsesOnlyConfirmedNeighbours) */
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
  g6b.confirmed = true;
  nr_dci_hist_push(o.get(), &g6b);
  nr_dci_hist_push(o.get(), &x7);
  uint64_t f4[NR_DCI_HIST_ROWS] = {0};
  EXPECT_EQ(nr_dci_hist_adj_exclusions(o.get(), &x7, row_k0_tda2_cert, nullptr, f4), 0);
  /* certified-flag side: the certified tda2 occupant of slot 7 makes a k0=1 slot-6 tda0 trap need an accident */
  EXPECT_TRUE(nr_dci_hist_k0_certified(h.get(), &g6, 1, 0x3, 0x1, &kGeo, row_k0_tda2_cert, nullptr));
  EXPECT_FALSE(nr_dci_hist_k0_certified(h.get(), &g6, 1, 0x3, 0x1, &kGeo, row_k0_none, nullptr)); /* uncertified row: ambiguous */
}
int main(int argc, char **argv)
{
  logInit();
  testing::InitGoogleTest(&argc, argv);
  nr_td_test::register_baseline(); /* every test starts at fb0 / CB0 off (the engine BC9 was written against): nr_td_test_baseline.h */
  return RUN_ALL_TESTS();
}

/* Review I2: the table mask of the certified flag covers every hypothesis that may be the truth -- a FIELD/PRIOR-dormant
 * one included. A sibling dormant in another MCS table makes a cross-table-equivalent occupant compatible: not certified. */
extern "C" {
#include "nr_pdsch_config_sweep.h"
}
TEST(DciHistory, DormantSiblingTableKeepsCrossTableOccupantCompatible)
{
  static nr_pdsch_config_sweep_state_t st;
  nr_pdsch_config_sweep_init(&st, 4);
  int lead = -1;
  for (int i = 0; i < st.n_hyp && lead < 0; i++)
    if (st.hyp[i].mcs_table == 0 && st.hyp[i].k0 == 1)
      lead = i;
  ASSERT_GE(lead, 0);
  auto only_table0 = [](const nr_pdsch_cfg_hypothesis_t *h, const void *) { return h->mcs_table == 0; };
  ASSERT_GT(nr_pdsch_config_sweep_set_dormant(&st, NR_TD_DORMANT_FIELD_BASE, only_table0, nullptr), 0);
  nr_pdsch_cfg_hypothesis_t h{};
  uint64_t sib = 0;
  uint8_t tables = 0;
  ASSERT_TRUE(nr_pdsch_config_sweep_siblings_of(&st, lead, &h, &sib, &tables));
  EXPECT_EQ(sib, UINT64_C(0x1));
  EXPECT_EQ(tables & 0x3, 0x3); /* the FIELD-dormant table-1 hypotheses still count */
  auto r = ring();
  const nr_dci_hist_entry_t g = dci(100, 0, 0, 50, 2);
  push(r.get(), g);
  push(r.get(), dci(101, 0, 0, 50, 1)); /* table 1 mcs 1 == table 0 mcs 2 */
  EXPECT_FALSE(nr_dci_hist_k0_certified(r.get(), &g, 1, sib | 0x2, tables, &kGeo, nullptr, nullptr));
  /* GEOM dormancy (a guarded pin) is the one cause that removes a table from the possible truths */
  static nr_pdsch_config_sweep_state_t gs;
  nr_pdsch_config_sweep_init(&gs, 4);
  ASSERT_GT(nr_pdsch_config_sweep_set_dormant(&gs, NR_TD_DORMANT_GEOM, only_table0, nullptr), 0);
  ASSERT_TRUE(nr_pdsch_config_sweep_siblings_of(&gs, lead, &h, &sib, &tables));
  EXPECT_EQ(tables, 0x1);
}

/* ---- BC9d: hard TDD / DCI-adjacency exclusions only from CONFIRMED DCIs (TB CRC pass of the grant they scheduled) ---- */
static int32_t bc9d_legal(int, int length, int start, int mapping_b, int add, int maxlen)
{
  return mapping_b ? 0 : 1 + start * 1000 + length * 40 + add * 3 + maxlen; /* same catalogue as nr_pdsch_config_sweep_test.cc */
}
/* TDD: period 10, slots 8 and 9 UL, the rest DL (a DCI in slot 7 cannot schedule k0 = 1) */
static bool bc9d_tdd(void *, uint32_t s, int8_t *last)
{
  for (int k = 0; k <= NR_DCI_HIST_K0_MAX; k++)
    last[k] = (int8_t)((s + (uint32_t)k) % 10 >= 8 ? -1 : 13);
  return true;
}
static int g_bc9d_calls;
static int bc9d_exclude(void *, uint64_t cfg, uint16_t rnti, uint8_t tda, const int8_t *last, bool)
{
  g_bc9d_calls++;
  nr_td_excl_t e;
  memcpy(e.last, last, sizeof(e.last));
  return nr_pdsch_config_sweep_exclude_key(cfg, rnti, tda, &e);
}
static bool bc9d_constrained(void *, uint64_t cfg, uint16_t rnti) { return nr_pdsch_config_sweep_rnti_constrained(rnti, cfg); }
static uint64_t bc9d_row_k0(void *, uint64_t cfg, uint16_t rnti, uint8_t tda) { return nr_pdsch_config_sweep_row_k0_allowed(cfg, rnti, tda); }
static const nr_dci_excl_ops_t kOps = {bc9d_tdd, bc9d_exclude, bc9d_constrained, bc9d_row_k0, nullptr};
static int bc9d_count_k0(uint8_t tda, int k0)
{ /* hypotheses with this k0 in the live (kCfg, 0x1234, tda) context (select opens it if needed) */
  nr_pdsch_sweep_ticket_t t{};
  nr_pdsch_cfg_hypothesis_t hy{};
  EXPECT_TRUE(nr_pdsch_config_sweep_select(kCfg, 0x1234, tda, 2, 0, bc9d_legal, &t, &hy));
  static nr_pdsch_config_sweep_state_t st;
  EXPECT_TRUE(nr_pdsch_config_sweep_snapshot(&t, &st));
  int n = 0;
  for (int i = 0; i < st.n_hyp; i++)
    n += st.hyp[i].k0 == k0;
  return n;
}
struct DciBc9d : testing::Test {
  void SetUp() override
  {
    nr_pdsch_config_sweep_reset_all();
    nr_pdsch_config_sweep_prior_reset();
    nr_pdsch_config_sweep_k0_legacy_set(0);
    nr_dci_hist_enabled_set(1);
    g_bc9d_calls = 0;
  }
  void TearDown() override
  {
    nr_pdsch_config_sweep_k0_legacy_set(-1);
    nr_dci_hist_enabled_set(-1);
  }
};

TEST_F(DciBc9d, ConfirmedBitSetFromFeedback)
{
  auto h = ring();
  push(h.get(), dci(10, 0));
  push(h.get(), dci(10, 1, 0, 20));
  nr_dci_hist_entry_t o = dci(11, 0);
  o.cfg = 0xBAD;
  push(h.get(), o);
  bool found = false;
  EXPECT_EQ(nr_dci_hist_confirm(h.get(), 0x1234, 10, kCfg, 0, &found), 1);
  EXPECT_TRUE(found);
  nr_dci_hist_entry_t out[4];
  ASSERT_EQ(nr_dci_hist_at(h.get(), 0x1234, 10, out, 4), 2);
  for (int i = 0; i < 2; i++)
    EXPECT_EQ(out[i].confirmed, out[i].tda == 0) << i; /* only the (slot, cfg, row) of the passing grant */
  /* a second pass of the same DCI: found, nothing newly confirmed */
  EXPECT_EQ(nr_dci_hist_confirm(h.get(), 0x1234, 10, kCfg, 0, &found), 0);
  EXPECT_TRUE(found);
  /* missed lookups: another slot, configuration, RNTI; an evicted entry */
  EXPECT_EQ(nr_dci_hist_confirm(h.get(), 0x1234, 12, kCfg, 0, &found), 0);
  EXPECT_FALSE(found);
  EXPECT_EQ(nr_dci_hist_confirm(h.get(), 0x1234, 11, kCfg, 0, &found), 0); /* slot 11 holds only cfg 0xBAD */
  EXPECT_FALSE(found);
  EXPECT_EQ(nr_dci_hist_confirm(h.get(), 0x9999, 10, kCfg, 0, &found), 0);
  EXPECT_FALSE(found);
  ASSERT_EQ(nr_dci_hist_at(h.get(), 0x1234, 11, out, 4), 1);
  EXPECT_FALSE(out[0].confirmed);
  for (uint32_t s = 20; s < 90; s++)
    push(h.get(), dci(s, 0));
  EXPECT_EQ(nr_dci_hist_confirm(h.get(), 0x1234, 20, kCfg, 0, &found), 0); /* depth 64: overwritten */
  EXPECT_FALSE(found);
  /* the driver: a missed lookup still applies the TDD rule (the CRC pass proves the DCI, the key is known) */
  ASSERT_GT(bc9d_count_k0(0, 1), 0);
  nr_dci_confirm_out_t co;
  nr_dci_hist_on_confirm(h.get(), 0x1234, 7, kCfg, 0, &kOps, &co);
  EXPECT_FALSE(co.found);
  EXPECT_TRUE(co.tdd);
  EXPECT_GT(co.tdd_removed, 0);
  EXPECT_EQ(bc9d_count_k0(0, 1), 0);
}

TEST_F(DciBc9d, SpuriousDciNeverExcludesTruth)
{
  /* Truth: row 0 has k0 = 1. A spurious row-0 DCI accepted in slot 7 (k0 = 1 would land on UL slot 8) used to remove every
   * k0 = 1 entry of row 0 at accept time. Accepted but never confirmed (no CRC pass): nothing may be excluded. */
  auto h = ring();
  const int k1 = bc9d_count_k0(0, 1);
  ASSERT_GT(k1, 0);
  nr_dci_hist_entry_t sp = dci(7, 0);
  nr_dci_hist_on_accept(h.get(), &sp);
  EXPECT_EQ(g_bc9d_calls, 0);
  EXPECT_EQ(bc9d_count_k0(0, 1), k1);
  EXPECT_EQ(nr_pdsch_config_sweep_row_k0_allowed(kCfg, 0x1234, 0), UINT64_C(0x3));
  /* Adjacency variant: a REAL row-1 DCI in slot 7 is confirmed (TDD certifies row 1 to k0 = 0: it occupies slot 7). A spurious
   * row-0 DCI in slot 6 would make "row 0, k0 = 1" collide with it: with the old rule it removed the truth of row 0. */
  bc9d_count_k0(1, 0); /* open row 1 */
  auto h2 = ring();
  nr_dci_hist_entry_t sp6 = dci(6, 0), real7 = dci(7, 1, 0, 20);
  nr_dci_hist_on_accept(h2.get(), &sp6);
  nr_dci_hist_on_accept(h2.get(), &real7);
  nr_dci_confirm_out_t co;
  nr_dci_hist_on_confirm(h2.get(), 0x1234, 7, kCfg, 1, &kOps, &co);
  EXPECT_TRUE(co.found);
  EXPECT_EQ(nr_pdsch_config_sweep_row_k0_allowed(kCfg, 0x1234, 1), UINT64_C(0x1)); /* row 1 certified k0 = 0 by its confirmed DCI */
  EXPECT_EQ(co.adj_rows, 0);
  EXPECT_EQ(bc9d_count_k0(0, 1), k1); /* the truth of row 0 survives */
  EXPECT_EQ(nr_pdsch_config_sweep_row_k0_allowed(kCfg, 0x1234, 0), UINT64_C(0x3));
}

TEST_F(DciBc9d, ConfirmedDciAppliesTddExclusion)
{
  auto h = ring();
  const int k1 = bc9d_count_k0(0, 1);
  ASSERT_GT(k1, 0);
  ASSERT_GT(bc9d_count_k0(0, 0), 0);
  nr_dci_hist_entry_t d = dci(7, 0);
  nr_dci_hist_on_accept(h.get(), &d);
  EXPECT_EQ(bc9d_count_k0(0, 1), k1); /* accept alone: untouched */
  nr_dci_confirm_out_t co;
  EXPECT_EQ(nr_dci_hist_on_confirm(h.get(), 0x1234, 7, kCfg, 0, &kOps, &co), 1);
  EXPECT_TRUE(co.found);
  EXPECT_EQ(co.newly, 1);
  EXPECT_TRUE(co.tdd);
  EXPECT_GT(co.tdd_removed, 0);
  EXPECT_EQ(bc9d_count_k0(0, 1), 0);
  EXPECT_GT(bc9d_count_k0(0, 0), 0);
  EXPECT_EQ(nr_pdsch_config_sweep_row_k0_allowed(kCfg, 0x1234, 0), UINT64_C(0x1));
  /* a second pass of the same DCI re-applies nothing */
  g_bc9d_calls = 0;
  EXPECT_EQ(nr_dci_hist_on_confirm(h.get(), 0x1234, 7, kCfg, 0, &kOps, &co), 0);
  EXPECT_EQ(g_bc9d_calls, 0);
  /* no TDD pattern (NSA / no SIB1): a confirmed DCI excludes nothing by TDD */
  const nr_dci_excl_ops_t no_tdd = {nullptr, bc9d_exclude, bc9d_constrained, bc9d_row_k0, nullptr};
  nr_dci_hist_entry_t d2 = dci(17, 1);
  nr_dci_hist_on_accept(h.get(), &d2);
  const int r1k1 = bc9d_count_k0(1, 1);
  nr_dci_hist_on_confirm(h.get(), 0x1234, 17, kCfg, 1, &no_tdd, &co);
  EXPECT_FALSE(co.tdd);
  EXPECT_EQ(bc9d_count_k0(1, 1), r1k1);
}

/* The same confirmed-DCI TDD exclusion under the receiver defaults since 2026-10-04 (fb2 + CB0 elimination on), on purpose:
 * the hard exclusion is a catalogue prune, independent of the field book's dormant causes and of the CB0 channel. */
TEST_F(DciBc9d, ConfirmedDciAppliesTddExclusionUnderDefaults)
{
  nr_td_test::pin_defaults();
  nr_pdsch_config_sweep_reset_all();
  ASSERT_EQ(nr_pdsch_config_sweep_fieldbook_mode(), 2);
  auto h = ring();
  const int k1 = bc9d_count_k0(0, 1);
  ASSERT_GT(k1, 0);
  ASSERT_GT(bc9d_count_k0(0, 0), 0);
  nr_dci_hist_entry_t d = dci(7, 0);
  nr_dci_hist_on_accept(h.get(), &d);
  EXPECT_EQ(bc9d_count_k0(0, 1), k1);
  nr_dci_confirm_out_t co;
  EXPECT_EQ(nr_dci_hist_on_confirm(h.get(), 0x1234, 7, kCfg, 0, &kOps, &co), 1);
  EXPECT_TRUE(co.tdd);
  EXPECT_GT(co.tdd_removed, 0);
  EXPECT_EQ(bc9d_count_k0(0, 1), 0);
  EXPECT_GT(bc9d_count_k0(0, 0), 0);
  EXPECT_EQ(nr_pdsch_config_sweep_row_k0_allowed(kCfg, 0x1234, 0), UINT64_C(0x1));
}

TEST_F(DciBc9d, AdjacencyUsesOnlyConfirmedNeighbours)
{
  /* pure rule: unconfirmed x or y never excludes */
  auto h = ring();
  nr_dci_hist_entry_t g6 = dci(6, 0), x7 = dci(7, 2, 0, 20);
  nr_dci_hist_push(h.get(), &g6);
  nr_dci_hist_push(h.get(), &x7);
  x7.confirmed = true;
  uint64_t f[NR_DCI_HIST_ROWS] = {0};
  EXPECT_EQ(nr_dci_hist_adj_exclusions(h.get(), &x7, row_k0_tda2_cert, nullptr, f), 0); /* neighbour g6 unconfirmed */
  x7.confirmed = false;
  g6.confirmed = true;
  EXPECT_EQ(nr_dci_hist_adj_exclusions(h.get(), &g6, row_k0_tda2_cert, nullptr, f), 0); /* neighbour x7 unconfirmed */
  /* driver, live sweep: row 0 truth k0 = 0 (real DCI slot 6), row 1 k0 = 0 (real DCI slot 7, certified by TDD) */
  auto r = ring();
  bc9d_count_k0(0, 0);
  bc9d_count_k0(1, 0);
  const int k1 = bc9d_count_k0(0, 1);
  nr_dci_hist_entry_t a6 = dci(6, 0), b7 = dci(7, 1, 0, 20);
  nr_dci_hist_on_accept(r.get(), &a6);
  nr_dci_hist_on_accept(r.get(), &b7);
  nr_dci_confirm_out_t co;
  nr_dci_hist_on_confirm(r.get(), 0x1234, 6, kCfg, 0, &kOps, &co); /* row 1 not yet certified: nothing */
  EXPECT_EQ(co.adj_rows, 0);
  EXPECT_EQ(bc9d_count_k0(0, 1), k1);
  nr_dci_hist_on_confirm(r.get(), 0x1234, 7, kCfg, 1, &kOps, &co); /* row 1 -> {0}: slot 7 occupied, so row 0 k0 = 1 impossible */
  EXPECT_EQ(co.adj_rows, 1);
  EXPECT_GT(co.adj_removed, 0);
  EXPECT_EQ(bc9d_count_k0(0, 1), 0);
  EXPECT_EQ(nr_pdsch_config_sweep_row_k0_allowed(kCfg, 0x1234, 0), UINT64_C(0x1));
}

TEST_F(DciBc9d, KillSwitchStillDisablesAll)
{
  auto h = ring();
  const int k1 = bc9d_count_k0(0, 1);
  nr_dci_hist_enabled_set(0);
  nr_dci_hist_entry_t d = dci(7, 0);
  nr_dci_hist_on_accept(h.get(), &d);
  nr_dci_hist_entry_t out[2];
  EXPECT_EQ(nr_dci_hist_at(h.get(), 0x1234, 7, out, 2), 0); /* no history */
  push(h.get(), d);
  nr_dci_confirm_out_t co;
  EXPECT_EQ(nr_dci_hist_on_confirm(h.get(), 0x1234, 7, kCfg, 0, &kOps, &co), 0);
  EXPECT_FALSE(co.found);
  EXPECT_EQ(g_bc9d_calls, 0);
  EXPECT_EQ(bc9d_count_k0(0, 1), k1);
  ASSERT_EQ(nr_dci_hist_at(h.get(), 0x1234, 7, out, 2), 1);
  EXPECT_FALSE(out[0].confirmed); /* no confirmation either */
  /* the environment switch, as the receiver reads it */
  setenv("ISAC_TD_DCI_ADJ", "0", 1);
  nr_dci_hist_enabled_set(-1);
  EXPECT_FALSE(nr_dci_hist_enabled());
  unsetenv("ISAC_TD_DCI_ADJ");
  nr_dci_hist_enabled_set(-1);
  EXPECT_TRUE(nr_dci_hist_enabled());
}

TEST(DciHistory, CertConfirmedOptionCountsOnlyConfirmedOccupants)
{
  auto h = ring();
  const nr_dci_hist_entry_t g = dci(100, 0, 0, 50);
  nr_dci_hist_push(h.get(), &g);
  push(h.get(), dci(101, 0, 0, 20)); /* incompatible, unconfirmed (may be spurious) */
  EXPECT_TRUE(nr_dci_hist_k0_certified(h.get(), &g, 1, 0x3, 0x1, &kGeo, nullptr, nullptr)); /* default: observed suffices */
  h->cert_confirmed = true;
  EXPECT_FALSE(nr_dci_hist_k0_certified(h.get(), &g, 1, 0x3, 0x1, &kGeo, nullptr, nullptr));
  nr_dci_hist_confirm(h.get(), 0x1234, 101, kCfg, 0, nullptr);
  EXPECT_TRUE(nr_dci_hist_k0_certified(h.get(), &g, 1, 0x3, 0x1, &kGeo, nullptr, nullptr));
  /* an unconfirmed COMPATIBLE occupant still spoils it (never less conservative than the default) */
  push(h.get(), dci(101, 0, 0, 50));
  EXPECT_FALSE(nr_dci_hist_k0_certified(h.get(), &g, 1, 0x3, 0x1, &kGeo, nullptr, nullptr));
}

TEST(DciHistoryEpoch, StaleScanAndPdschCannotInsertOrConfirm)
{
  if (!nr_cfg_reconf_enabled()) GTEST_SKIP() << "ISAC_RECONF=1 fixture";
  nr_cfg_epoch_reset();
  auto h = ring();
  const auto e = dci(100, 0);
  nr_dci_hist_on_accept(h.get(), &e);
  uint64_t dropped = 0;
  {
    NR_CFG_EPOCH_WORK(nr_cfg_epoch_current(), &dropped);
    nr_cfg_epoch_note_bwp_change();
    const auto stale = dci(101, 0);
    nr_dci_hist_on_accept(h.get(), &stale);
    bool found = true;
    EXPECT_EQ(nr_dci_hist_confirm(h.get(), e.rnti, e.abs_slot, e.cfg, e.tda, &found), 0);
    EXPECT_FALSE(found);
    EXPECT_EQ(dropped, 1u);
  }
  nr_dci_hist_entry_t out[2]{};
  EXPECT_EQ(nr_dci_hist_at(h.get(), e.rnti, 101, out, 2), 0);
  ASSERT_EQ(nr_dci_hist_at(h.get(), e.rnti, 100, out, 2), 1);
  EXPECT_FALSE(out[0].confirmed);
  bool found = false;
  EXPECT_EQ(nr_dci_hist_confirm(h.get(), e.rnti, e.abs_slot, e.cfg, e.tda, &found), 1);
  EXPECT_TRUE(found); // Existing history reset policy remains R10, outside RI.
}
