/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* mpci audit (branch mpci/c-audit): interleave tests of the modules the audit converted to cell-indexed statics.
 * Each test drives cell X and cell Y alternately from ONE thread that re-binds (nr_cell_bind), then compares what each cell reads back with what only that
 * cell wrote. Cell indices are fresh per test (the slots are not re-initialised when a cell is rebound).
 *   nr_td_order       SIB1 TDRA store and the census per cell
 *   nr_pdsch_xoverhead x-overhead evidence per cell
 *   nr_dci_history    nr_dci_hist_global() per cell
 *   nr_pdsch_dmrs_scan identity votes per cell
 *   nr_pdsch_config_sweep per-RNTI certification (same RNTI, same configuration, two cells) */
#include <cstring>
#include <gtest/gtest.h>
#include "nr_td_test_baseline.h"
extern "C" {
#include "nr_td_order.h"
#include "nr_pdsch_xoverhead.h"
#include "nr_dci_history.h"
#include "nr_pdsch_dmrs_scan.h"
#include "nr_pdsch_config_sweep.h"
#include "nr_passive_cfg_epoch.h"
#include "nr_td_legal.h"
#include "common/utils/nr_cell.h"
#include "common/config/config_userapi.h"
#include "common/utils/LOG/log.h"

configmodule_interface_t *uniqCfg = nullptr;
void exit_function(const char *file, const char *fn, int line, const char *message, int fatal)
{
  if (message)
    fprintf(stderr, "%s:%d %s: %s\n", file, line, fn, message);
  if (fatal)
    abort();
  exit(EXIT_SUCCESS);
}
/* nr_mac_common.c (nr_compute_tbs of the x-overhead module) references it from code this test never runs; same stub as nr_dci_history_test.cc */
void *get_softmodem_params(void)
{
  static char p[4096];
  return p;
}
}

namespace {
struct Bind {
  explicit Bind(int c) { nr_cell_bind(c); }
  ~Bind() { nr_cell_bind(0); }
};
} // namespace

TEST(MpciAudit, TdOrderSib1StoreAndCensusPerCell)
{
  nr_td_tdra_t a[2], b[1];
  memset(a, 0, sizeof(a));
  memset(b, 0, sizeof(b));
  a[0].S = 2;  a[0].L = 12;
  a[1].S = 2;  a[1].L = 7;
  b[0].S = 1;  b[0].L = 13;
  nr_td_tdra_t out[NR_TD_MAX_SIB1_TDRA];
  for (int round = 0; round < 3; round++) { /* interleaved: X sets, Y sets, both read, X counts, Y counts */
    nr_cell_bind(4);
    nr_td_sib1_store_set(a, 2);
    nr_cell_bind(5);
    nr_td_sib1_store_set(b, 1);
    nr_cell_bind(4);
    EXPECT_EQ(nr_td_sib1_store_get(out), 2);
    EXPECT_EQ(out[1].L, 7);
    nr_td_census_count(10, NR_TD_CENSUS_MATCH);
    nr_cell_bind(5);
    EXPECT_EQ(nr_td_sib1_store_get(out), 1);
    EXPECT_EQ(out[0].S, 1);
    nr_td_census_count(10, NR_TD_CENSUS_MISMATCH);
    nr_td_census_count(10, NR_TD_CENSUS_MISMATCH);
  }
  uint64_t cx[3][3], cy[3][3];
  nr_cell_bind(4);
  nr_td_census_get(cx);
  nr_cell_bind(5);
  nr_td_census_get(cy);
  nr_cell_bind(0);
  uint64_t sx = 0, sy = 0, mx = 0, my = 0;
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++) {
      sx += cx[i][j];
      sy += cy[i][j];
      if (j == NR_TD_CENSUS_MATCH) mx += cx[i][j];
      if (j == NR_TD_CENSUS_MISMATCH) my += cy[i][j];
    }
  EXPECT_EQ(sx, 3u);  /* cell 4: three MATCH counts, nothing of cell 5's */
  EXPECT_EQ(mx, 3u);
  EXPECT_EQ(sy, 6u);  /* cell 5: six MISMATCH counts */
  EXPECT_EQ(my, 6u);
  EXPECT_EQ(nr_td_sib1_store_get(out), 0); /* cell 0 never set one */
}

TEST(MpciAudit, XoverheadEvidencePerCell)
{
  {
    Bind b(4);
    nr_pdsch_xoverhead_reset(0);
  }
  {
    Bind b(5);
    nr_pdsch_xoverhead_reset(6);
  }
  for (int i = 0; i < 4; i++) {
    nr_cell_bind(4);
    EXPECT_EQ(nr_pdsch_xoverhead_snapshot().assumed, 0);
    nr_cell_bind(5);
    EXPECT_EQ(nr_pdsch_xoverhead_snapshot().assumed, 6);
  }
  nr_cell_bind(0);
}

TEST(MpciAudit, DciHistoryPerCell)
{
  nr_cell_bind(4);
  nr_dci_hist_t *hx = nr_dci_hist_global();
  nr_cell_bind(5);
  nr_dci_hist_t *hy = nr_dci_hist_global();
  EXPECT_NE(hx, hy);
  nr_cell_bind(4);
  EXPECT_EQ(nr_dci_hist_global(), hx); /* stable per cell */
  nr_dci_hist_entry_t e;
  memset(&e, 0, sizeof(e));
  e.rnti = 0x4601;
  e.abs_slot = 100;
  e.dci11 = true;
  e.num_rb = 20;
  nr_dci_hist_push(hx, &e);
  nr_dci_hist_entry_t out[4];
  EXPECT_EQ(nr_dci_hist_at(hx, 0x4601, 100, out, 4), 1);
  nr_cell_bind(5);
  EXPECT_EQ(nr_dci_hist_at(nr_dci_hist_global(), 0x4601, 100, out, 4), 0); /* the other cell's history never saw it */
  nr_cell_bind(0);
}

TEST(MpciAudit, DmrsScanVotesPerCell)
{
  nr_dmrs_scan_hit_t hit_x = {}, hit_y = {};
  hit_x.nid = 100; hit_x.nscid = 0; hit_x.rb0 = 0; hit_x.nrb = 4; hit_x.score = 0.9f; hit_x.second = 0.1f;
  hit_y = hit_x;
  hit_y.nid = 200;
  const uint16_t rnti = 0x4601; /* the same RNTI in both cells */
  for (int s = 0; s < 6; s++) {
    nr_cell_bind(4);
    nr_dmrs_scan_vote(rnti, 1000 + s, &hit_x, 1);
    if (s < 2) {
      nr_cell_bind(5);
      nr_dmrs_scan_vote(rnti, 1000 + s, &hit_y, 1);
    }
  }
  int ids[2];
  nr_cell_bind(4);
  EXPECT_EQ(nr_dmrs_scan_rnti_slots(rnti), 6);
  EXPECT_GE(nr_dmrs_scan_rnti_ids(rnti, ids, 2), 1);
  EXPECT_EQ(ids[0], 100);
  nr_cell_bind(5);
  EXPECT_EQ(nr_dmrs_scan_rnti_slots(rnti), 2); /* cell 5 voted in two slots only */
  EXPECT_GE(nr_dmrs_scan_rnti_ids(rnti, ids, 2), 1);
  EXPECT_EQ(ids[0], 200);
  nr_cell_bind(0);
}

static int32_t audit_legal(int, int length, int start, int mapping_b, int add, int maxlen)
{
  return mapping_b ? 0 : 1 + start * 1000 + length * 40 + add * 3 + maxlen;
}

TEST(MpciAudit, SweepCertificationPerCell)
{
  const uint16_t rnti = 0x4601;
  nr_pdsch_sweep_ticket_t tx{}, ty{};
  nr_pdsch_cfg_hypothesis_t h{};
  nr_cell_bind(4);
  nr_pdsch_config_sweep_reset_all();
  ASSERT_TRUE(nr_pdsch_config_sweep_select(1, rnti, 0, 2, 0, audit_legal, &tx, &h));
  nr_cell_bind(5);
  nr_pdsch_config_sweep_reset_all();
  ASSERT_TRUE(nr_pdsch_config_sweep_select(1, rnti, 0, 2, 0, audit_legal, &ty, &h));
  EXPECT_NE(nr_pdsch_config_sweep_row_k0_allowed(1, rnti, 0), 0u);
  /* cell 4 certifies k0 = 0 only for its row, cell 5 k0 = 1 only: neither sees the other's certification */
  nr_cell_bind(4);
  EXPECT_GT(nr_pdsch_config_sweep_certify_k0(&tx, 0x1), 0);
  nr_cell_bind(5);
  EXPECT_GT(nr_pdsch_config_sweep_certify_k0(&ty, 0x2), 0);
  nr_cell_bind(4);
  EXPECT_EQ(nr_pdsch_config_sweep_row_k0_allowed(1, rnti, 0) & 0x3, 0x1u);
  nr_cell_bind(5);
  EXPECT_EQ(nr_pdsch_config_sweep_row_k0_allowed(1, rnti, 0) & 0x3, 0x2u);
  /* a ticket of cell 4 names a context slot of cell 4: cell 5 re-selecting the same key gets its own generation */
  EXPECT_TRUE(nr_pdsch_config_sweep_is_settled(1, rnti, 0, 0) == false);
  nr_cell_bind(0);
  EXPECT_EQ(nr_pdsch_config_sweep_row_k0_allowed(1, rnti, 0), 0x3u); /* cell 0 has no context for the RNTI: the unconstrained default */
  nr_cell_bind(4);
  nr_pdsch_config_sweep_reset_all();
  EXPECT_EQ(nr_pdsch_config_sweep_row_k0_allowed(1, rnti, 0), 0x3u); /* cell 4 reset */
  nr_cell_bind(5);
  EXPECT_EQ(nr_pdsch_config_sweep_row_k0_allowed(1, rnti, 0) & 0x3, 0x2u); /* cell 5 untouched by cell 4's reset */
  nr_cell_bind(0);
}

int main(int argc, char **argv)
{
  logInit();
  set_glog(OAILOG_ERR);
  ::testing::InitGoogleTest(&argc, argv);
  nr_td_test::register_baseline(); /* every test starts at fb0 / CB0 off, as the other sweep tests */
  return RUN_ALL_TESTS();
}
