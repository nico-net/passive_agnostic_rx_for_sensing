// SPDX-License-Identifier: LicenseRef-CSSL-1.0
// Cell manager (task D1). Hosts are SYNTHETIC FIXTURES (host caps are test inputs, never constants of the manager):
//   big   DGX-like: 20 cpus -> max_cells 4 [fixture], 100 GiB unified            small  cloud-like: max_cells 2 [fixture], 16 GiB, no GPU
// Inventories: tests/passive_rx/mpci/inventory/*.json. [OFFLINE VERIFIED, unit]
// NOTE: this binary interposes malloc/calloc/realloc (glibc) to count allocations while `g_armed` is set.
#include <gtest/gtest.h>
#include <dlfcn.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cstdlib>
#include <unistd.h>
extern "C" {
#include "nr_cell_mgr.h"
}

static std::atomic<long> g_allocs{0};
static std::atomic<int> g_armed{0};
extern "C" void *__libc_malloc(size_t);
extern "C" void *__libc_calloc(size_t, size_t);
extern "C" void *__libc_realloc(void *, size_t);
extern "C" void *malloc(size_t n)
{
  if (g_armed) g_allocs++;
  return __libc_malloc(n);
}
extern "C" void *calloc(size_t a, size_t b)
{
  if (g_armed) g_allocs++;
  return __libc_calloc(a, b);
}
extern "C" void *realloc(void *p, size_t n)
{
  if (g_armed) g_allocs++;
  return __libc_realloc(p, n);
}

#define MIB ((size_t)1 << 20)
static nr_host_caps_t caps(int max_cells, size_t mem_mib)
{
  nr_host_caps_t c;
  memset(&c, 0, sizeof c);
  c.mem_avail = mem_mib * MIB;
  c.max_cells = max_cells;
  c.chan_budget_msps = -1;
  c.consumers = 4;
  c.depth = 64;
  c.csirs_enabled = c.recovery_enabled = c.gw_enabled = 1;
  return c;
}
static std::string slurp(const std::string &p)
{
  std::ifstream f(p);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}
static std::string fx(const char *n) { return std::string(MPCI_INV_DIR) + "/" + n; }
static nr_cell_inventory_t load(const char *n)
{
  static nr_cell_inventory_t inv;
  std::string t = slurp(fx(n));
  char err[200];
  EXPECT_EQ(0, nr_cell_inventory_parse(t.data(), t.size(), &inv, err, sizeof err)) << err;
  return inv;
}
static std::string cellj(const std::string &id, int pci, double centre, double cfo, const char *state, int nrb = 0)
{
  char b[600];
  snprintf(b, sizeof b,
           "{\"id\":\"%s\",\"centre_hz\":%.0f,\"gscn\":7971,\"ssb_hz\":3679680000,\"ssb_index\":1,\"pci\":%d,\"cfo_hz\":%.0f,\"n_rb_hint\":%s,"
           "\"scs_khz\":30,\"sss_gap\":4,\"dmrs_ratio\":4,\"first_seen_ns\":1,\"state\":\"%s\"}",
           id.c_str(), centre, pci, cfo, nrb ? std::to_string(nrb).c_str() : "null", state);
  return b;
}
// the fake front end: writes inventories atomically into a run dir, as the real one will
struct FakeFrontEnd {
  std::string dir, path;
  FakeFrontEnd()
  {
    char t[] = "/tmp/nr_mgr_testXXXXXX";
    dir = mkdtemp(t);
    path = dir + "/cells.json";
  }
  ~FakeFrontEnd()
  {
    std::string c = "rm -rf " + dir;
    if (system(c.c_str())) {}
  }
  void publish(const std::vector<std::string> &cells)
  {
    std::string t = "{\"schema\":1,\"window\":{\"centre_hz\":3680010000,\"rate_msps\":122.88,\"n_rx\":4},\"cells\":[";
    for (size_t i = 0; i < cells.size(); i++) t += (i ? "," : "") + cells[i];
    t += "]}";
    std::string tmp = path + ".tmp";
    { std::ofstream f(tmp, std::ios::binary); f << t; }
    ASSERT_EQ(0, rename(tmp.c_str(), path.c_str()));
  }
};

struct Rec {
  std::vector<std::string> log;
  std::vector<nr_cell_mgr_event_t> ev;
  std::vector<std::string> evid;
  std::vector<std::string> cause;
  static void cb(void *c, const nr_cell_mgr_event_t *e)
  {
    Rec *r = (Rec *)c;
    r->ev.push_back(*e);
    r->evid.push_back(e->id);
    r->cause.push_back(e->cause);
    r->ev.back().id = nullptr;
    r->ev.back().cause = nullptr;
  }
  static void lg(void *c, const char *l) { ((Rec *)c)->log.push_back(l); }
  int count(int kind) const
  {
    int n = 0;
    for (auto &e : ev) n += e.kind == kind;
    return n;
  }
  int lines(const char *needle) const
  {
    int n = 0;
    for (auto &l : log) n += l.find(needle) != std::string::npos;
    return n;
  }
};
static nr_cell_mgr_cfg_t cfg(Rec *r, int64_t hyst = 0, size_t st = 4096)
{
  nr_cell_mgr_cfg_t c;
  memset(&c, 0, sizeof c);
  c.cb = Rec::cb;
  c.ctx = r;
  c.log = Rec::lg;
  c.log_ctx = r;
  c.retire_hysteresis_ns = hyst;
  c.slot_state_bytes = st;
  c.grid_n_rb = 217;   // receiver start config of the fixtures (-r 217 --ssb 1171, pass-1 k_SSB 14)
  c.pass1_ssb = 1171;
  c.pass1_k_ssb = 14;
  return c;
}
static std::vector<nr_cell_desc_t> descs(const nr_cell_inventory_t &inv)
{
  std::vector<nr_cell_desc_t> v;
  for (int i = 0; i < inv.n; i++) v.push_back(inv.cell[i].desc);
  return v;
}
static const int64_t S = 1000000000LL;

class Mgr : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {}
  nr_cell_mgr_t m;
};

// Process-wide fence: it can only be armed once per process, so every test that plans runs BEFORE the one fence test (see AllocFence, last).
TEST_F(Mgr, PlanLogsAdmitAndReturnsNmax)
{
  Rec r;
  nr_cell_inventory_t inv = load("pass1_20261012.json");
  nr_host_caps_t c = caps(4, 102400);
  nr_cell_mgr_cfg_t g = cfg(&r);
  EXPECT_EQ(4, nr_cell_mgr_plan(&m, &g, &c, descs(inv).data(), inv.n));
  EXPECT_EQ(4, nr_cell_mgr_nmax(&m));
  ASSERT_EQ(1, r.lines("MPCI_ADMIT cells=4 of 6 reason=cores"));
  EXPECT_EQ(0, nr_cell_mgr_active(&m));
}
TEST_F(Mgr, PlanFailsOnBadArgs)
{
  Rec r;
  nr_cell_mgr_cfg_t g = cfg(&r);
  nr_host_caps_t c = caps(4, 102400);
  EXPECT_EQ(-1, nr_cell_mgr_plan(&m, &g, &c, nullptr, 0));
  nr_cell_desc_t d;
  nr_cell_desc_init(&d);
  d.centre_hz = 3.6e9;
  d.state = NR_CELLD_LOST;  // nothing servable
  EXPECT_EQ(-1, nr_cell_mgr_plan(&m, &g, &c, &d, 1));
}
TEST_F(Mgr, AddUpdateRemoveLost)
{
  Rec r;
  nr_cell_inventory_t inv = load("live_tuned_20261023.json");
  nr_host_caps_t c = caps(4, 102400);
  nr_cell_mgr_cfg_t g = cfg(&r, 5 * S);
  ASSERT_EQ(4, nr_cell_mgr_plan(&m, &g, &c, descs(inv).data(), inv.n));
  // first inventory has 7 cells (4 maintain, 1 acquire, 2 new): MAINTAIN first, so 415 923 508 403 take the slots
  EXPECT_EQ(7, nr_cell_mgr_apply(&m, &inv, 0));
  EXPECT_EQ(4, r.count(NR_MGR_EV_ADD));
  EXPECT_EQ(3, r.count(NR_MGR_EV_REFUSE));
  EXPECT_EQ(4, nr_cell_mgr_active(&m));
  EXPECT_EQ(0, nr_cell_mgr_find(&m, "pci415"));
  EXPECT_EQ(3, nr_cell_mgr_find(&m, "pci403"));
  EXPECT_EQ(-1, nr_cell_mgr_find(&m, "pci675"));
  EXPECT_EQ(4, r.lines("event=add"));
  EXPECT_EQ(3, r.lines("event=refuse"));
  EXPECT_NE(std::string::npos, r.log.back().find("reason=cores"));
  // unchanged inventory: no events, no repeated refuse lines
  EXPECT_EQ(0, nr_cell_mgr_apply(&m, &inv, 1 * S));
  // update: same id, new CFO -> RETUNE of that slot only
  r.ev.clear(); r.evid.clear(); r.cause.clear();
  nr_cell_inventory_t u = inv;
  u.cell[1].desc.cfo_hz += 25;
  EXPECT_EQ(1, nr_cell_mgr_apply(&m, &u, 2 * S));
  ASSERT_EQ(1u, r.ev.size());
  EXPECT_EQ(NR_MGR_EV_RETUNE, r.ev[0].kind);
  EXPECT_EQ(1, r.ev[0].slot);
  EXPECT_EQ("pci923", r.evid[0]);
  EXPECT_DOUBLE_EQ(-4832, r.ev[0].prev_cfo_hz);
  EXPECT_DOUBLE_EQ(-4832 + 25, r.ev[0].desc.cfo_hz);
  EXPECT_EQ(1, r.lines("event=update"));
  // update: new centre
  nr_cell_inventory_t u2 = u;
  u2.cell[2].desc.centre_hz += 30000;
  EXPECT_EQ(1, nr_cell_mgr_apply(&m, &u2, 3 * S));
  EXPECT_DOUBLE_EQ(3590010000.0, r.ev.back().prev_centre_hz);
  // lost: retire only after the hysteresis
  nr_cell_inventory_t l = u2;
  l.cell[3].desc.state = NR_CELLD_LOST;
  EXPECT_EQ(0, nr_cell_mgr_apply(&m, &l, 10 * S));
  EXPECT_EQ(0, nr_cell_mgr_tick(&m, 14 * S));
  EXPECT_EQ(4, nr_cell_mgr_active(&m));
  size_t before = r.ev.size();
  EXPECT_EQ(1, nr_cell_mgr_tick(&m, 15 * S));
  ASSERT_EQ(before + 1, r.ev.size());
  EXPECT_EQ(NR_MGR_EV_RETIRE, r.ev.back().kind);
  EXPECT_EQ("lost", r.cause.back());
  EXPECT_EQ(3, r.ev.back().slot);
  EXPECT_EQ(3, nr_cell_mgr_active(&m));
  EXPECT_EQ(-1, nr_cell_mgr_find(&m, "pci403"));
  // the freed slot is given to a refused cell on the next apply (priority: acquire 675 before new 4 / 380)
  EXPECT_EQ(1, nr_cell_mgr_apply(&m, &l, 16 * S));
  EXPECT_EQ(NR_MGR_EV_ADD, r.ev.back().kind);
  EXPECT_EQ("pci675", r.evid.back());
  EXPECT_EQ(3, r.ev.back().slot);
  // removed (id absent) -> retire after the hysteresis, cause "removed"
  nr_cell_inventory_t rm = l;
  rm.n = 0;
  EXPECT_EQ(0, nr_cell_mgr_apply(&m, &rm, 20 * S));
  EXPECT_EQ(4, nr_cell_mgr_tick(&m, 25 * S));
  EXPECT_EQ("removed", r.cause.back());
  EXPECT_EQ(0, nr_cell_mgr_active(&m));
  EXPECT_EQ(5, r.lines("event=retire"));  // 403 (lost) + 4 removed
}
TEST_F(Mgr, LostThatReturnsCancelsRetire)
{
  Rec r;
  nr_cell_inventory_t inv = load("pass1_20261012.json");
  nr_host_caps_t c = caps(6, 102400);
  nr_cell_mgr_cfg_t g = cfg(&r, 5 * S);
  ASSERT_EQ(6, nr_cell_mgr_plan(&m, &g, &c, descs(inv).data(), inv.n));
  nr_cell_mgr_apply(&m, &inv, 0);
  nr_cell_inventory_t l = inv;
  l.cell[0].desc.state = NR_CELLD_LOST;
  nr_cell_mgr_apply(&m, &l, 1 * S);
  nr_cell_mgr_apply(&m, &inv, 3 * S);  // back before the hysteresis
  EXPECT_EQ(0, nr_cell_mgr_tick(&m, 100 * S));
  EXPECT_EQ(6, nr_cell_mgr_active(&m));
}
TEST_F(Mgr, CoChannelPairTwoSlots)
{
  Rec r;
  nr_cell_inventory_t inv = load("live_tuned_20261023.json");
  nr_cell_inventory_t two = inv;
  two.n = 2;
  two.cell[0] = inv.cell[2];  // 508
  two.cell[1] = inv.cell[5];  // PCI 4, same centre and SSB frequency
  nr_host_caps_t c = caps(4, 102400);
  nr_cell_mgr_cfg_t g = cfg(&r);
  ASSERT_EQ(2, nr_cell_mgr_plan(&m, &g, &c, descs(two).data(), two.n));
  EXPECT_EQ(2, nr_cell_mgr_apply(&m, &two, 0));
  const nr_mgr_slot_t *a = nr_cell_mgr_slot(&m, nr_cell_mgr_find(&m, "pci508")), *b = nr_cell_mgr_slot(&m, nr_cell_mgr_find(&m, "pci4"));
  ASSERT_TRUE(a && b);
  EXPECT_NE(a, b);
  EXPECT_DOUBLE_EQ(a->desc.centre_hz, b->desc.centre_hz);
  EXPECT_DOUBLE_EQ(a->desc.ssb_hz, b->desc.ssb_hz);
  EXPECT_NE(a->desc.pci, b->desc.pci);
  // retuning one does not touch the other
  nr_cell_inventory_t u = two;
  u.cell[1].desc.cfo_hz += 10;
  EXPECT_EQ(1, nr_cell_mgr_apply(&m, &u, S));
  EXPECT_EQ(r.evid.size() ? r.evid.back() : "", "pci4");
}
TEST_F(Mgr, RefusedBeyondNmaxWithReason)
{
  Rec r;
  nr_cell_inventory_t inv = load("pass1_20261012.json");
  nr_host_caps_t c = caps(2, 102400);  // small host fixture: 2 cells
  nr_cell_mgr_cfg_t g = cfg(&r);
  ASSERT_EQ(2, nr_cell_mgr_plan(&m, &g, &c, descs(inv).data(), inv.n));
  EXPECT_EQ(6, nr_cell_mgr_apply(&m, &inv, 0));
  EXPECT_EQ(2, r.count(NR_MGR_EV_ADD));
  ASSERT_EQ(4, r.count(NR_MGR_EV_REFUSE));
  for (size_t i = 0; i < r.ev.size(); i++)
    if (r.ev[i].kind == NR_MGR_EV_REFUSE) {
      EXPECT_EQ(NR_ADMIT_CORES, r.ev[i].reason);
      EXPECT_EQ("cores", r.cause[i]);
      EXPECT_EQ(-1, r.ev[i].slot);
    }
  EXPECT_EQ(4, r.lines("event=refuse reason=cores"));
  EXPECT_EQ(0, nr_cell_mgr_apply(&m, &inv, S));  // not repeated
}
TEST_F(Mgr, RefusedByRamNamesRam)
{
  Rec r;
  nr_cell_inventory_t inv = load("pass1_20261012.json");
  nr_host_caps_t c = caps(-1, 24 * 1024);
  c.host_shared_bytes = 0;
  nr_cell_mgr_cfg_t g = cfg(&r);
  int n = nr_cell_mgr_plan(&m, &g, &c, descs(inv).data(), inv.n);
  ASSERT_GE(n, 1);
  if (n < inv.n) {
    nr_cell_mgr_apply(&m, &inv, 0);
    EXPECT_EQ(inv.n - n, r.count(NR_MGR_EV_REFUSE));
    EXPECT_EQ(inv.n - n, r.lines("event=refuse reason=ram"));
  }
}
TEST_F(Mgr, FullAtPlanTimeInventoryGrowthRefusedAsMax)
{
  Rec r;
  nr_cell_inventory_t inv = load("pass1_20261012.json");
  nr_cell_inventory_t three = inv;
  three.n = 3;
  nr_host_caps_t c = caps(-1, 102400);
  nr_cell_mgr_cfg_t g = cfg(&r);
  ASSERT_EQ(3, nr_cell_mgr_plan(&m, &g, &c, descs(three).data(), 3));
  EXPECT_EQ(1, r.lines("reason=none"));
  nr_cell_mgr_apply(&m, &inv, 0);  // 6 cells now, 3 slots
  EXPECT_EQ(3, r.count(NR_MGR_EV_REFUSE));
  EXPECT_EQ(3, r.lines("event=refuse reason=max"));
}
TEST_F(Mgr, Pass2FromLogExcerpt)
{
  Rec r;
  nr_cell_inventory_t inv = load("pass1_20261012.json");
  nr_host_caps_t c = caps(8, 102400);
  nr_cell_mgr_cfg_t g = cfg(&r);
  ASSERT_EQ(6, nr_cell_mgr_plan(&m, &g, &c, descs(inv).data(), inv.n));
  nr_cell_mgr_apply(&m, &inv, 0);
  r.ev.clear(); r.evid.clear(); r.cause.clear(); r.log.clear();
  // 415: k_ssb=14 -> --ssb 1164+7 = 1171 = pass 1: pass 2 equals pass 1 (AUTOFRONT table), no retune
  std::string l415 = slurp(fx("pass2_pci415_excerpt.txt"));
  int ssb = 0;
  double cen = 0;
  EXPECT_EQ(0, nr_cell_mgr_pass2_from_log(&m, "pci415", l415.data(), l415.size(), S, &ssb, &cen));
  EXPECT_EQ(1171, ssb);
  EXPECT_DOUBLE_EQ(3680010000.0, cen);
  EXPECT_TRUE(r.ev.empty());
  // 923: k_ssb=10 -> --ssb 1169, -C 3731910000 (live_chain.sh pass2 result, FRONTLIVE)
  std::string l923 = slurp(fx("pass2_pci923_excerpt.txt"));
  EXPECT_EQ(1, nr_cell_mgr_pass2_from_log(&m, "pci923", l923.data(), l923.size(), S, &ssb, &cen));
  EXPECT_EQ(1169, ssb);
  EXPECT_DOUBLE_EQ(3731910000.0, cen);
  ASSERT_EQ(1u, r.ev.size());
  EXPECT_EQ(NR_MGR_EV_RETUNE, r.ev[0].kind);
  EXPECT_EQ("pass2", r.cause[0]);
  EXPECT_EQ(1169, r.ev[0].ssb);
  EXPECT_EQ(1171, r.ev[0].prev_ssb);
  EXPECT_DOUBLE_EQ(3731850000.0, r.ev[0].prev_centre_hz);
  EXPECT_DOUBLE_EQ(3731910000.0, r.ev[0].desc.centre_hz);
  EXPECT_DOUBLE_EQ(-4850, r.ev[0].desc.cfo_hz);  // CFO kept (autofront: "FO is kept")
  EXPECT_EQ(1, r.lines("event=update reason=pass2"));
  // idempotent; an unchanged inventory does not undo the retune
  EXPECT_EQ(0, nr_cell_mgr_pass2_from_log(&m, "pci923", l923.data(), l923.size(), 2 * S, nullptr, nullptr));
  EXPECT_EQ(0, nr_cell_mgr_apply(&m, &inv, 3 * S));
  EXPECT_DOUBLE_EQ(3731910000.0, nr_cell_mgr_slot(&m, nr_cell_mgr_find(&m, "pci923"))->desc.centre_hz);
  // wrong cell's log, no MIB line, unknown id
  EXPECT_EQ(-1, nr_cell_mgr_pass2_from_log(&m, "pci508", l923.data(), l923.size(), S, nullptr, nullptr));
  const char *nomib = "[PHY] Initial sync successful, PCI: 508\n";
  EXPECT_EQ(-1, nr_cell_mgr_pass2_from_log(&m, "pci508", nomib, strlen(nomib), S, nullptr, nullptr));
  EXPECT_EQ(-1, nr_cell_mgr_pass2_from_log(&m, "nope", l923.data(), l923.size(), S, nullptr, nullptr));
  // ANSI colour in the excerpt does not matter (colour codes sit before the tag, not inside the key=value text)
  std::string col = "\x1b[0m\x1b[1;32m" + slurp(fx("pass2_pci923_excerpt.txt"));
  EXPECT_EQ(0, nr_cell_mgr_pass2_from_log(&m, "pci923", col.data(), col.size(), 4 * S, nullptr, nullptr));
}
// Numerology maths reproduces the live tunings (PROJECT_MEMORY 10-23/24 item 4) from (GSCN, SCS, N_RB, offsetToPointA, k_SSB).
TEST(MgrMaths, LiveTuningsReproduced)
{
  struct { int gscn, nrb, offa, k, ssb; double C; } t[] = {
    {7971, 217, 194, 14, 1171, 3680010000.0},   // 415 (offsetToPointA 194, k 14: "Vodafone rule")
    {8007, 217, 38, 10, 233, 3759990000.0},     // 923 SIB1 offsetToPointA 38, k 10
    {7895, 162, 32, 2, 193, 3590010000.0},      // 508 SIB1 offsetToPointA 32, k 2
    {7795, 51, 36, 22, 227, 3425010000.0},      // 675 SIB1 offsetToPointA 36, k 22
  };
  for (auto &x : t) {
    double ssbhz = 3000000000.0 + (x.gscn - 7499) * 1440000.0;
    int ssb = nr_mgr_ssb_start_sc(30, x.offa, x.k);
    EXPECT_EQ(x.ssb, ssb) << x.gscn;
    EXPECT_DOUBLE_EQ(x.C, nr_mgr_grid_centre_hz(ssbhz, 30, x.nrb, ssb)) << x.gscn;
  }
  // 403: no SIB1 carrier info, grid from the carrier edges; --ssb 18 -r 51 -> -C 3630000000
  EXPECT_DOUBLE_EQ(3630000000.0, nr_mgr_grid_centre_hz(3000000000.0 + (7933 - 7499) * 1440000.0, 30, 51, 18));
  // synthetic SCS 15 kHz, N_RB 106 (hand check, TS 38.211 7.4.3.1): offsetToPointA 20, k_SSB 6 -> 12*20 + 6 = 246;
  // centre = ssb_hz + (6*106 - 120 - 246) * 15 kHz = ssb_hz + 270 * 15 kHz = ssb_hz + 4.05 MHz
  EXPECT_EQ(246, nr_mgr_ssb_start_sc(15, 20, 6));
  EXPECT_DOUBLE_EQ(3504050000.0, nr_mgr_grid_centre_hz(3500000000.0, 15, 106, 246));
  // synthetic SCS 60 kHz: offsetToPointA 40 -> 40>>2 = 10 PRB, k 8 -> 8>>2 = 2: 122; 273 PRB grid: (1638-120-122)*60k
  EXPECT_EQ(122, nr_mgr_ssb_start_sc(60, 40, 8));
  EXPECT_DOUBLE_EQ(3500000000.0 + 1396 * 60000.0, nr_mgr_grid_centre_hz(3500000000.0, 60, 273, 122));
}
TEST_F(Mgr, Pass2UsesSib1OffsetToPointA)
{
  Rec r;
  nr_cell_inventory_t inv = load("live_tuned_20261023.json");
  nr_host_caps_t c = caps(8, 102400);
  nr_cell_mgr_cfg_t g = cfg(&r);
  ASSERT_GE(nr_cell_mgr_plan(&m, &g, &c, descs(inv).data(), inv.n), 1);
  nr_cell_mgr_apply(&m, &inv, 0);
  // 923 live: n_rb_hint 217, log gives offsetToPointA=38 k_ssb=10 -> --ssb 233, -C 3759990000 = the inventory's -C; --ssb changes from the pass-1 value, so the --ssb retune is emitted
  std::string l = slurp(fx("pass2_pci923_excerpt.txt")) + "SENSING: SIB1 offsetToPointA=38 x\n";
  int ssb = 0; double cen = 0;
  EXPECT_EQ(1, nr_cell_mgr_pass2_from_log(&m, "pci923", l.data(), l.size(), S, &ssb, &cen));
  EXPECT_EQ(233, ssb);
  EXPECT_DOUBLE_EQ(3759990000.0, cen);
  // 508: from the pass-1 (217 grid) to the live tuning with SIB1 offsetToPointA 32, k 2, n_rb 162
  std::string l5 = "Initial sync successful, PCI: 508\nMIB fields sfn=1 k_ssb=2 x\noffsetToPointA=32\n";
  EXPECT_EQ(1, nr_cell_mgr_pass2_from_log(&m, "pci508", l5.data(), l5.size(), S, &ssb, &cen));
  EXPECT_EQ(193, ssb);
  EXPECT_DOUBLE_EQ(3590010000.0, cen);
}
TEST_F(Mgr, Pass2LeavesFrontEndRefinedCellAlone)
{
  Rec r;
  nr_cell_inventory_t inv = load("live_tuned_20261023.json");  // n_rb_hint set: front end already carries the grid
  nr_host_caps_t c = caps(8, 102400);
  nr_cell_mgr_cfg_t g = cfg(&r);
  ASSERT_GE(nr_cell_mgr_plan(&m, &g, &c, descs(inv).data(), inv.n), 1);
  nr_cell_mgr_apply(&m, &inv, 0);
  std::string l923 = slurp(fx("pass2_pci923_excerpt.txt"));
  r.ev.clear();
  EXPECT_EQ(0, nr_cell_mgr_pass2_from_log(&m, "pci923", l923.data(), l923.size(), S, nullptr, nullptr));
  EXPECT_TRUE(r.ev.empty());
}
TEST_F(Mgr, FakeFrontEndDrivesThreeCellInventory)
{
  Rec r;
  FakeFrontEnd fe;
  nr_cell_inventory_watch_t w;
  ASSERT_EQ(0, nr_cell_inventory_watch_init(&w, fe.path.c_str()));
  nr_host_caps_t c = caps(4, 102400);
  nr_cell_mgr_cfg_t g = cfg(&r, 2 * S);
  char err[200];
  EXPECT_EQ(0, nr_cell_inventory_poll(&w, err, sizeof err));  // front end not started
  fe.publish({cellj("A", 415, 3680010000, -4849, "acquire"), cellj("B", 923, 3731850000, -4850, "acquire"), cellj("C", 403, 3625290000, 2225, "acquire")});
  ASSERT_EQ(1, nr_cell_inventory_poll(&w, err, sizeof err));
  ASSERT_EQ(3, w.inv.n);
  ASSERT_EQ(4, nr_cell_mgr_plan(&m, &g, &c, descs(w.inv).data(), w.inv.n) + 1);  // 3 served
  nr_cell_mgr_apply(&m, &w.inv, 0);
  EXPECT_EQ(3, nr_cell_mgr_active(&m));
  EXPECT_EQ(3, r.count(NR_MGR_EV_ADD));
  // the front end refines B's CFO and the receiver log gives k_SSB
  fe.publish({cellj("A", 415, 3680010000, -4849, "maintain"), cellj("B", 923, 3731850000, -4832, "maintain"), cellj("C", 403, 3625290000, 2225, "acquire")});
  ASSERT_EQ(1, nr_cell_inventory_poll(&w, err, sizeof err));
  EXPECT_EQ(1, nr_cell_mgr_apply(&m, &w.inv, 1 * S));  // only B's CFO changed (state change alone is not a retune)
  EXPECT_EQ("B", r.evid.back());
  // C vanishes from the file, A and B stay
  fe.publish({cellj("A", 415, 3680010000, -4849, "maintain"), cellj("B", 923, 3731850000, -4832, "maintain")});
  ASSERT_EQ(1, nr_cell_inventory_poll(&w, err, sizeof err));
  nr_cell_mgr_apply(&m, &w.inv, 2 * S);
  EXPECT_EQ(0, nr_cell_mgr_tick(&m, 3 * S));
  EXPECT_EQ(1, nr_cell_mgr_tick(&m, 5 * S));
  EXPECT_EQ("C", r.evid.back());
  EXPECT_EQ(2, nr_cell_mgr_active(&m));
  EXPECT_EQ(1, r.lines("event=retire"));
  // a half-written file does not disturb the manager
  { std::ofstream f(fe.path, std::ios::trunc); f << "{\"schema\":1,\"wind"; }
  EXPECT_EQ(-1, nr_cell_inventory_poll(&w, err, sizeof err));
  EXPECT_EQ(2, w.inv.n);
}
// ---- must stay last: arms the process-wide RX fence ----
TEST_F(Mgr, ZAllocFence_ActivationNeverAllocates)
{
  Rec r;
  nr_cell_inventory_t inv = load("live_tuned_20261023.json");
  nr_host_caps_t c = caps(4, 102400);
  nr_cell_mgr_cfg_t g = cfg(&r, 1 * S, 8192);
  ASSERT_EQ(4, nr_cell_mgr_plan(&m, &g, &c, descs(inv).data(), inv.n));
  r.ev.reserve(64); r.evid.reserve(64); r.cause.reserve(64); r.log.reserve(64);
  nr_cell_alloc_fence();  // RX fence: from here on no allocation is allowed
  // measure only the manager: the recording callback/log of the test fixture is replaced by plain counters
  nr_cell_mgr_cfg_t quiet;
  memset(&quiet, 0, sizeof quiet);
  static int n_ev;
  static void *last_state;
  n_ev = 0;
  struct L { static void ev(void *, const nr_cell_mgr_event_t *e) { n_ev++; last_state = e->slot_state; }
             static void lg(void *, const char *) {} };
  m.cfg.cb = L::ev;
  m.cfg.log = L::lg;
  g_allocs = 0;
  g_armed = 1;
  nr_cell_mgr_apply(&m, &inv, 0);                       // 4 adds + 3 refusals
  nr_cell_inventory_t u = inv;
  u.cell[0].desc.cfo_hz += 5;
  nr_cell_mgr_apply(&m, &u, 1 * S);                     // retune
  u.cell[1].desc.state = NR_CELLD_LOST;
  nr_cell_mgr_apply(&m, &u, 2 * S);
  nr_cell_mgr_tick(&m, 4 * S);                          // retire
  nr_cell_mgr_apply(&m, &u, 5 * S);                     // re-activation of the freed slot by another cell
  g_armed = 0;
  EXPECT_EQ(0, g_allocs.load());
  EXPECT_GE(n_ev, 8);
  EXPECT_NE(nullptr, last_state);
  EXPECT_TRUE(nr_cell_alloc_fenced());
  (void)quiet;
}
