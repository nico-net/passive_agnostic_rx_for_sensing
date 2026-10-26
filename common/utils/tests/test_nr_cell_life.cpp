// SPDX-License-Identifier: LicenseRef-CSSL-1.0
// Cell lifecycle (task A8): admit -> prealloc (bound) -> fence -> count -> start (bound); refuse beyond N_max; lost -> retire after
// the hysteresis; re-activation of a prepared slot without allocation; primary always slot 0. Host caps are SYNTHETIC FIXTURES
// (test inputs). The real nr_cell_alloc_fence() is process-wide and one-way: only the LAST test uses it. [OFFLINE VERIFIED, unit]
#include <gtest/gtest.h>
#include <cstring>
#include <string>
#include <vector>
extern "C" {
#include "nr_cell_life.h"
}

#define MIB ((size_t)1 << 20)
static nr_host_caps_t caps(int max_cells)
{
  nr_host_caps_t c;
  memset(&c, 0, sizeof c);
  c.mem_avail = (size_t)100 * 1024 * MIB; // fixture: 100 GiB
  c.max_cells = max_cells;
  c.chan_budget_msps = -1;
  c.consumers = 4;
  c.depth = 64;
  return c;
}
static nr_cell_desc_t desc(double centre, int pci, int nrb = 106)
{
  nr_cell_desc_t d;
  nr_cell_desc_init(&d);
  d.centre_hz = centre;
  d.pci = pci;
  d.n_rb = nrb;
  d.scs_khz = 30;
  d.n_rx = 4;
  return d;
}
static nr_inv_cell_t icell(const char *id, double centre, int pci, int state = NR_CELLD_NEW)
{
  nr_inv_cell_t c;
  memset(&c, 0, sizeof c);
  snprintf(c.id, sizeof c.id, "%s", id);
  c.desc = desc(centre, pci);
  c.desc.state = state;
  return c;
}
static nr_cell_mgr_cfg_t mcfg()
{
  nr_cell_mgr_cfg_t c;
  memset(&c, 0, sizeof c);
  c.grid_n_rb = 106;
  c.pass1_ssb = 516;
  c.pass1_k_ssb = 0;
  c.retire_hysteresis_ns = 1000;
  return c;
}

struct Rec {
  std::vector<std::string> ev; // "prealloc k bound fenced", ...
  int fenced = 0;
  bool real_fence = false;
  int count_at_start = -1;
  std::vector<std::string> logs;
};
static int h_prealloc(void *ctx, int cell, const nr_mgr_slot_t *s)
{
  Rec *r = (Rec *)ctx;
  r->ev.push_back("prealloc " + std::to_string(cell) + " bound=" + std::to_string(nr_cell_idx()) + " fenced=" + std::to_string(r->fenced) + " id=" +
                  (s ? s->id : "-"));
  return 0;
}
static void h_fence(void *ctx)
{
  Rec *r = (Rec *)ctx;
  r->fenced++;
  r->ev.push_back("fence");
  if (r->real_fence)
    nr_cell_alloc_fence();
}
static int h_start(void *ctx, int cell, const nr_mgr_slot_t *s, int re)
{
  Rec *r = (Rec *)ctx;
  r->count_at_start = nr_cell_count();
  r->ev.push_back("start " + std::to_string(cell) + " bound=" + std::to_string(nr_cell_idx()) + " fenced=" + std::to_string(r->fenced) + " re=" +
                  std::to_string(re) + " id=" + (s ? s->id : "-"));
  return 0;
}
static void h_retire(void *ctx, int cell, const nr_mgr_slot_t *s)
{
  Rec *r = (Rec *)ctx;
  r->ev.push_back("retire " + std::to_string(cell) + " bound=" + std::to_string(nr_cell_idx()) + " id=" + (s ? s->id : "-"));
}
static void h_log(void *ctx, const char *l) { ((Rec *)ctx)->logs.push_back(l); }
static nr_cell_life_ops_t ops(Rec *r)
{
  nr_cell_life_ops_t o;
  memset(&o, 0, sizeof o);
  o.prealloc = h_prealloc;
  o.fence = h_fence;
  o.start = h_start;
  o.retire = h_retire;
  o.log = h_log;
  o.ctx = r;
  return o;
}

TEST(NrCellLife, OrderPutsTheStartedCellFirst)
{
  nr_cell_inventory_t in;
  memset(&in, 0, sizeof in);
  in.cell[in.n++] = icell("i403", 3630.0e6, 403);
  in.cell[in.n++] = icell("v415", 3680.01e6, 415, NR_CELLD_MAINTAIN);
  nr_cell_desc_t p = desc(3680.01e6 + 100, -1); // started tuning, PCI unknown
  nr_cell_inventory_t o;
  int m = -2;
  ASSERT_EQ(2, nr_cell_life_order(&p, &in, 1000, &o, &m));
  EXPECT_EQ(1, m);
  EXPECT_STREQ("v415", o.cell[0].id);
  EXPECT_EQ(NR_CELLD_MAINTAIN, o.cell[0].desc.state);
  EXPECT_DOUBLE_EQ(3680.01e6 + 100, o.cell[0].desc.centre_hz); // the started tuning wins
  EXPECT_EQ(415, o.cell[0].desc.pci);                          // unknowns come from the inventory
  EXPECT_STREQ("i403", o.cell[1].id);
  // not listed: a synthetic primary first, the inventory after it; a MAINTAIN entry of another cell is demoted
  nr_cell_desc_t q = desc(3500e6, 7);
  ASSERT_EQ(3, nr_cell_life_order(&q, &in, 1000, &o, &m));
  EXPECT_EQ(-1, m);
  EXPECT_STREQ(NR_CELL_LIFE_PRIMARY_ID, o.cell[0].id);
  EXPECT_EQ(NR_CELLD_ACQUIRE, o.cell[2].desc.state);
  // no inventory: only the started cell
  ASSERT_EQ(1, nr_cell_life_order(&q, NULL, 1000, &o, &m));
}

TEST(NrCellLife, NoInventoryIsOneCell)
{
  Rec r;
  nr_cell_life_t L;
  nr_host_caps_t c = caps(4);
  nr_cell_mgr_cfg_t mc = mcfg();
  nr_cell_life_ops_t o = ops(&r);
  nr_cell_desc_t p = desc(3680.01e6, -1);
  EXPECT_EQ(1, nr_cell_life_plan(&L, &mc, &c, &o, &p, NULL, 1000));
}

TEST(NrCellLife, AdmitPreallocFenceCountStartAllBound)
{
  Rec r;
  nr_cell_life_t L;
  nr_host_caps_t c = caps(NR_ADMIT_UNLIMITED_CELLS);
  nr_cell_mgr_cfg_t mc = mcfg();
  nr_cell_life_ops_t o = ops(&r);
  nr_cell_desc_t p = desc(3680.01e6, -1);
  nr_cell_inventory_t in;
  memset(&in, 0, sizeof in);
  in.cell[in.n++] = icell("i403", 3630.0e6, 403);
  in.cell[in.n++] = icell("v415", 3680.01e6, 415);
  in.cell[in.n++] = icell("w508", 3600.0e6, 508);
  nr_cell_bind(0);
  ASSERT_EQ(3, nr_cell_life_plan(&L, &mc, &c, &o, &p, &in, 1000));
  ASSERT_EQ(3, nr_cell_life_activate(&L, &in, 0));
  std::vector<std::string> want = {"prealloc 0 bound=0 fenced=0 id=v415", "prealloc 1 bound=1 fenced=0 id=i403", "prealloc 2 bound=2 fenced=0 id=w508",
                                   "fence",
                                   "start 0 bound=0 fenced=1 re=0 id=v415", "start 1 bound=1 fenced=1 re=0 id=i403", "start 2 bound=2 fenced=1 re=0 id=w508"};
  EXPECT_EQ(want, r.ev);
  EXPECT_EQ(3, r.count_at_start);
  EXPECT_EQ(0, nr_cell_idx()); // binding restored
  EXPECT_EQ(-1, nr_cell_life_activate(&L, &in, 0)); // once
  nr_cell_set_count(1);
}

TEST(NrCellLife, BeyondNmaxRefusedWithoutHooks)
{
  Rec r;
  nr_cell_life_t L;
  nr_host_caps_t c = caps(2); // fixture: the core layout hosts 2 cells
  nr_cell_mgr_cfg_t mc = mcfg();
  nr_cell_life_ops_t o = ops(&r);
  nr_cell_desc_t p = desc(3680.01e6, 415);
  nr_cell_inventory_t in;
  memset(&in, 0, sizeof in);
  in.cell[in.n++] = icell("i403", 3630.0e6, 403);
  in.cell[in.n++] = icell("w508", 3600.0e6, 508);
  ASSERT_EQ(2, nr_cell_life_plan(&L, &mc, &c, &o, &p, &in, 1000));
  ASSERT_EQ(2, nr_cell_life_activate(&L, &in, 0));
  int refused = 0;
  for (auto &l : r.logs)
    refused += l.find("id=w508") != std::string::npos && l.find("event=refuse") != std::string::npos;
  EXPECT_EQ(1, refused);
  for (auto &e : r.ev)
    EXPECT_EQ(std::string::npos, e.find("w508")) << e;
  EXPECT_EQ(2, r.count_at_start);
  nr_cell_set_count(1);
}

TEST(NrCellLife, LostRetiresAfterHysteresisThenReactivates)
{
  Rec r;
  nr_cell_life_t L;
  nr_host_caps_t c = caps(4);
  nr_cell_mgr_cfg_t mc = mcfg();
  nr_cell_life_ops_t o = ops(&r);
  nr_cell_desc_t p = desc(3680.01e6, 415);
  nr_cell_inventory_t in;
  memset(&in, 0, sizeof in);
  in.cell[in.n++] = icell("i403", 3630.0e6, 403);
  ASSERT_EQ(2, nr_cell_life_plan(&L, &mc, &c, &o, &p, &in, 1000));
  ASSERT_EQ(2, nr_cell_life_activate(&L, &in, 0));
  r.ev.clear();
  nr_cell_inventory_t lost = in;
  lost.cell[0].desc.state = NR_CELLD_LOST;
  nr_cell_life_poll(&L, &lost, 100);
  nr_cell_life_poll(&L, NULL, 500); // inside the hysteresis
  EXPECT_TRUE(r.ev.empty());
  EXPECT_TRUE(nr_cell_life_started(&L, 1));
  nr_cell_life_poll(&L, NULL, 1200);
  ASSERT_EQ(1u, r.ev.size());
  EXPECT_EQ("retire 1 bound=1 id=i403", r.ev[0]);
  EXPECT_FALSE(nr_cell_life_started(&L, 1));
  EXPECT_TRUE(nr_cell_life_started(&L, 0)); // the primary is never retired (not in the inventory at all)
  nr_cell_life_poll(&L, &in, 2000);         // back
  ASSERT_EQ(2u, r.ev.size());
  EXPECT_EQ("start 1 bound=1 fenced=1 re=1 id=i403", r.ev[1]);
  EXPECT_EQ(0, nr_cell_idx());
  nr_cell_set_count(1);
}

TEST(NrCellLife, ZZRealFenceAddIntoUnpreparedSlotIsNotStarted)
{
  Rec r;
  r.real_fence = true;
  nr_cell_life_t L;
  nr_host_caps_t c = caps(4);
  nr_cell_mgr_cfg_t mc = mcfg();
  nr_cell_life_ops_t o = ops(&r);
  nr_cell_desc_t p = desc(3680.01e6, 415);
  nr_cell_inventory_t in;
  memset(&in, 0, sizeof in);
  in.cell[in.n++] = icell("i403", 3630.0e6, 403);
  in.cell[in.n++] = icell("w508", 3600.0e6, 508, NR_CELLD_LOST); // wanted list excludes it: N_max = 2
  // plan with a third (future) cell in the wanted set so N_max = 3, but only two are in the first inventory
  nr_cell_inventory_t plan_inv = in;
  plan_inv.cell[1].desc.state = NR_CELLD_NEW;
  ASSERT_EQ(3, nr_cell_life_plan(&L, &mc, &c, &o, &p, &plan_inv, 1000));
  ASSERT_EQ(2, nr_cell_life_activate(&L, &in, 0));
  EXPECT_TRUE(nr_cell_alloc_fenced());
  r.ev.clear();
  nr_cell_inventory_t later = in;
  later.cell[1].desc.state = NR_CELLD_NEW; // w508 appears after the fence
  nr_cell_life_poll(&L, &later, 10);
  EXPECT_TRUE(r.ev.empty());
  int deferred = 0;
  for (auto &l : r.logs)
    deferred += l.find("not_prepared_before_fence") != std::string::npos;
  EXPECT_EQ(1, deferred);
  nr_cell_set_count(1);
}
