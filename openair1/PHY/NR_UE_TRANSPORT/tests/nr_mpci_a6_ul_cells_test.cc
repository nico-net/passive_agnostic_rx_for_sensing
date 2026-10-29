// mpci A6: per-cell UL state (UL layout lock, UL data-id lock, UL background discovery). Two cells with DIFFERENT UL BWPs run on
// DIFFERENT cell slots; every module must give each cell exactly the result of its solo run, whether the calls alternate on ONE thread
// that re-binds between calls (nr_cell_bind) or run on TWO threads bound to their cells, and a lock / reset of one cell never reaches the other.
// Fixtures: the synthetic DCI 0_1 banks of nr_pdcch_blind_monitor_test.cc (UlLayout* suites): PCI-415 shape, RIV width follows the BWP
// (217 PRB: 15 bits, DCI 44; 106 PRB: 13 bits, DCI 42). Written field by field, not through any receiver packer.
// [OFFLINE VERIFIED, DGX GB10 aarch64 unit]
#include <gtest/gtest.h>
#include <atomic>
#include <cstring>
#include <random>
#include <thread>
#include <unistd.h>
#include <vector>
extern "C" {
#include "common/utils/nr_cell.h"
#include "nr_pdcch_ul_layout_lock.h"
#include "nr_pusch_passive_ul_ids.h"
#include "nr_pusch_ul_bgdisc.h"
#include "nr_ul_cell_prealloc.h"
#include "common/config/config_userapi.h"
#include "common/utils/LOG/log.h"
#include "executables/softmodem-common.h"
configmodule_interface_t *uniqCfg = nullptr;
void exit_function(const char *, const char *, int, const char *, int) { std::abort(); }
softmodem_params_t *get_softmodem_params(void) { static softmodem_params_t p; return &p; }
}

namespace {
struct Bind {  // bind the calling thread to a cell for a scope; restore the previous one
  int prev;
  explicit Bind(int c) : prev(nr_cell_idx()) { nr_cell_bind(c); }
  ~Bind() { nr_cell_bind(prev); }
};

uint32_t Riv(int n, int s, int l)
{
  return (l - 1) <= n / 2 ? (uint32_t)(n * (l - 1) + s) : (uint32_t)(n * (n - l + 1) + (n - 1 - s));
}

struct Truth { uint8_t mcs, ndi, rv, pid; uint16_t start, len; };

// DCI 0_1 bank generator (the UllFree shape of the layout tests): free HARQ ids, NDI toggled for new data, RV 2 for retransmissions.
struct Gen {
  int n_bwp, rivw, dci_len;
  std::mt19937 rng;
  int8_t ndi[8][16];
  uint8_t spid[8] = {0}, sndi[8] = {0};
  bool step;  // true: the HARQ id of a UE steps by 1..3 and NDI toggles on wrap (UllGen): the MCS offset is significant and the fit is read; false: free ids (UllFree): only CRC trials lock
  int k = 0;
  Gen(int n, uint32_t seed, bool st = false) : n_bwp(n), rivw(n >= 217 ? 15 : 13), dci_len(29 + (n >= 217 ? 15 : 13)), rng(seed), step(st) { memset(ndi, 0, sizeof(ndi)); }
  nr_ul_layout_dci_t next(Truth *t)
  {
    static const int dci_slots[4] = {6, 7, 16, 17};
    int u, pid;
    uint8_t rv, mcs, nd;
    int L, S;
    if (step) {
      u = (int)(rng() % 8);
      const int adv = 1 + (int)(rng() % 3);
      spid[u] = (uint8_t)((spid[u] + adv) % 16);
      if (spid[u] < adv) sndi[u] ^= 1;
      L = (rng() % 10) ? 3 : 1 + (int)(rng() % (n_bwp / 2));
      S = (int)(rng() % (n_bwp - L + 1));
      rv = (rng() % 12) ? 0 : 2;
      mcs = (uint8_t)(rng() % 28);
      pid = spid[u];
      nd = sndi[u];
    } else {
      u = (int)(rng() % 8);
      pid = (int)(rng() % 16);
      const bool retx = rng() % 4 == 0;
      if (!retx) ndi[u][pid] ^= 1;
      rv = retx ? 2 : 0;
      mcs = (uint8_t)(rng() % 11);
      L = (rng() % 10) ? 3 : 1 + (int)(rng() % (n_bwp / 2));
      S = (int)(rng() % (n_bwp - L + 1));
      nd = (uint8_t)ndi[u][pid];
    }
    uint64_t v = 0;
    auto put = [&](uint32_t x, int w) { v = (v << w) | (x & ((1u << w) - 1u)); };
    put(0, 1); put(1, 1); put(Riv(n_bwp, S, L), rivw); put(0, 4); put(mcs, 5); put(nd, 1); put(rv, 2);
    put(pid, 4); put(3, 2); put(1, 2); put(0, 3); put(0, 2); put(0, 1); put(1, 1);
    *t = {mcs, nd, rv, (uint8_t)pid, (uint16_t)S, (uint16_t)L};
    nr_ul_layout_dci_t d;
    d.rnti = (uint16_t)(0x4000 + u);
    d.slot = (long)(50 + k / 4) * 20 + dci_slots[k % 4];
    k++;
    d.p = nr_dci_bits_from_u64(v);
    return d;
  }
};

nr_ul_layout_cell_t CellOf(int n_bwp, uint16_t pci)
{
  nr_ul_layout_cell_t c;
  memset(&c, 0, sizeof(c));
  c.carrier_rb = (uint16_t)n_bwp;
  c.dl_bwp_size = (uint16_t)n_bwp;
  c.pci = pci;
  c.mu = 1;
  c.typeA_pos = 0;
  c.slots_per_frame = 20;
  return c;
}

bool ReadsTruth(const nr_pdcch_blind_ul_result_t &g, const Truth &t)
{
  return g.mcs == t.mcs && g.ndi == t.ndi && g.rv == t.rv && g.harq_pid == t.pid && g.num_rb == t.len && g.start_rb + g.bwp_start == t.start;
}

// One cell's lock driver. Every call binds the slot first: a driver is a pure function of (its cell slot, its generator).
struct Drv {
  int slot;  // cell slot
  Gen gen;
  nr_ul_layout_cell_t cell;
  uint32_t trials_seen = 0;
  Drv(int s, int n_bwp, uint16_t pci, uint32_t seed, bool step = false) : slot(s), gen(n_bwp, seed, step), cell(CellOf(n_bwp, pci)) {}
  int len() const { return gen.dci_len; }
  // one DCI into the module (PDCCH slot occupancy as the live cell: slots 0-6 / 10-16); returns trials booked; *verdict true: PUSCH CRC24 pass for the true layout
  int feed(bool verdict, int *rc = nullptr)
  {
    Bind b(slot);
    Truth t;
    const nr_ul_layout_dci_t d = gen.next(&t);
    nr_pdcch_blind_ul_result_t out, tr[NR_UL_LAYOUT_MAX_TRIALS];
    for (int s2 = 0; s2 < 7; s2++) {
      nr_ul_layout_note_pdcch_slot((d.slot / 20) * 20 + s2);
      nr_ul_layout_note_pdcch_slot((d.slot / 20) * 20 + 10 + s2);
    }
    const int r = nr_ul_layout_on_dci(d.rnti, (uint16_t)len(), d.slot, d.p, &cell, &out);
    if (rc) *rc = r;
    const int nt = nr_ul_layout_crc_trials(d.rnti, (uint16_t)len(), d.slot, d.p, &cell, tr, NR_UL_LAYOUT_MAX_TRIALS);
    if (verdict)
      for (int q = 0; q < nt; q++)
        if (nr_ul_layout_is_trial(&tr[q]) && ReadsTruth(tr[q], t)) nr_ul_layout_crc_verdict(&tr[q], true, 6000, 1, 1, d.slot);
    return nt;
  }
  void eval() { Bind b(slot); nr_ul_layout_eval_once(); }
  bool provisional() { Bind b(slot); return nr_ul_layout_provisional((uint16_t)len()); }
  bool searching() { Bind b(slot); return nr_ul_layout_searching(); }
  void reset() { Bind b(slot); nr_ul_layout_lock_reset(); }
};

struct Fit {
  bool provisional, searching;
  int n = 0, ok = 0;
  std::vector<std::vector<uint32_t>> rd;  // fields of every fitted read
  bool operator==(const Fit &o) const { return provisional == o.provisional && searching == o.searching && n == o.n && ok == o.ok && rd == o.rd; }
};

// phase 1: bank 300 DCIs, one fit pass, then read 200 DCIs of the same stream with the fitted (not locked) layout
Fit ReadFitted(Drv &d)
{
  for (int i = 0; i < 300; i++) d.feed(false);
  d.eval();
  Fit f;
  f.provisional = d.provisional();
  f.searching = d.searching();
  Bind b(d.slot);
  for (int i = 0; i < 200; i++) {
    Truth t;
    const nr_ul_layout_dci_t x = d.gen.next(&t);
    nr_pdcch_blind_ul_result_t out;
    if (!nr_ul_layout_read_fitted(x.rnti, (uint16_t)d.len(), x.p, &out)) continue;
    f.n++;
    f.ok += ReadsTruth(out, t);
    f.rd.push_back({out.mcs, out.ndi, out.rv, out.harq_pid, out.num_rb, (uint32_t)(out.start_rb + out.bwp_start), out.bwp_size, out.bwp_start});
  }
  return f;
}

struct Locked {
  bool locked = false;
  int n = 0, ok = 0;
  uint16_t bwp_size = 0;
};

// drive a driver until its length is locked by CRC evidence (the CrcResolvesAnAmbiguousOffset procedure); then read 200 DCIs with the lock
bool LockStep(Drv &d, int *iter, int budget)
{
  if (*iter == 0)
    for (int i = 0; i < 400; i++) d.feed(false);
  if (*iter == 0) d.eval();
  const int stop = *iter + 64;
  for (; *iter < stop && *iter < budget; (*iter)++) {
    d.feed(true);
    if (*iter % 32 == 0) d.eval();
  }
  return !d.provisional();
}

Locked ReadLocked(Drv &d)
{
  Locked l;
  Bind b(d.slot);
  l.locked = !nr_ul_layout_provisional((uint16_t)d.len());
  for (int i = 0; i < 200; i++) {
    Truth t;
    const nr_ul_layout_dci_t x = d.gen.next(&t);
    nr_pdcch_blind_ul_result_t out;
    if (nr_ul_layout_on_dci(x.rnti, (uint16_t)d.len(), x.slot, x.p, &d.cell, &out) != 1) continue;
    l.n++;
    l.ok += ReadsTruth(out, t);
    l.bwp_size = out.bwp_size;
  }
  return l;
}
}  // namespace

// Cells A (217 PRB) and B (106 PRB): the fitted read of each cell, alternating one DCI at a time on one thread, equals the solo run of the
// same stream on another cell slot. Solo runs use slots 2 and 3, the interleaved ones 0 and 1.
TEST(MpciA6UlLayout, InterleavedOneThreadEqualsSoloRuns)
{
  Drv sa(2, 217, 415, 11, true), sb(3, 106, 222, 22, true);
  const Fit fa = ReadFitted(sa), fb = ReadFitted(sb);
  ASSERT_GT(fa.n, 100);
  ASSERT_GT(fb.n, 100);
  EXPECT_EQ(fa.ok, fa.n);
  EXPECT_EQ(fb.ok, fb.n);
  // the two BWPs read differently: the cells really are different inputs
  EXPECT_NE(fa.rd, fb.rd);

  Drv a(0, 217, 415, 11, true), b(1, 106, 222, 22, true);
  for (int i = 0; i < 300; i++) {  // alternate DCI by DCI
    a.feed(false);
    b.feed(false);
  }
  a.eval();
  b.eval();
  Fit ia, ib;
  ia.provisional = a.provisional(); ia.searching = a.searching();
  ib.provisional = b.provisional(); ib.searching = b.searching();
  for (int i = 0; i < 200; i++) {  // alternate the reads too
    Truth ta, tb;
    nr_pdcch_blind_ul_result_t out;
    const nr_ul_layout_dci_t xa = a.gen.next(&ta), xb = b.gen.next(&tb);
    {
      Bind c(0);
      if (nr_ul_layout_read_fitted(xa.rnti, (uint16_t)a.len(), xa.p, &out)) {
        ia.n++; ia.ok += ReadsTruth(out, ta);
        ia.rd.push_back({out.mcs, out.ndi, out.rv, out.harq_pid, out.num_rb, (uint32_t)(out.start_rb + out.bwp_start), out.bwp_size, out.bwp_start});
      }
    }
    {
      Bind c(1);
      if (nr_ul_layout_read_fitted(xb.rnti, (uint16_t)b.len(), xb.p, &out)) {
        ib.n++; ib.ok += ReadsTruth(out, tb);
        ib.rd.push_back({out.mcs, out.ndi, out.rv, out.harq_pid, out.num_rb, (uint32_t)(out.start_rb + out.bwp_start), out.bwp_size, out.bwp_start});
      }
    }
  }
  EXPECT_TRUE(ia == fa);
  EXPECT_TRUE(ib == fb);
  a.reset(); b.reset(); sa.reset(); sb.reset();
}

TEST(MpciA6UlLayout, TwoThreadsBoundToTheirCellsEqualSoloRuns)
{
  Drv sa(4, 217, 415, 33, true), sb(5, 106, 222, 44, true);
  const Fit fa = ReadFitted(sa), fb = ReadFitted(sb);
  ASSERT_GT(fa.n, 100);
  ASSERT_GT(fb.n, 100);
  Drv a(0, 217, 415, 33, true), b(1, 106, 222, 44, true);
  Fit ia, ib;
  std::thread ta([&] { nr_cell_bind(0); ia = ReadFitted(a); });
  std::thread tb([&] { nr_cell_bind(1); ib = ReadFitted(b); });
  ta.join();
  tb.join();
  EXPECT_TRUE(ia == fa);
  EXPECT_TRUE(ib == fb);
  a.reset(); b.reset(); sa.reset(); sb.reset();
}

// The CRC lock of each cell (A: 217 PRB, B: 106 PRB) is reached on its own evidence, alternating DCI by DCI, and never leaks: a reset
// of cell A releases A only, B keeps its lock; B's lock reads B's BWP, A's lock reads A's.
TEST(MpciA6UlLayout, LocksNeverLeakBetweenCellsOfDifferentBwp)
{
  Drv a(0, 217, 415, 5), b(1, 106, 222, 6);
  int ia = 0, ib = 0;
  bool la = false, lb = false;
  while ((!la || !lb) && ia < 40000 && ib < 40000) {  // alternate chunks of 64 DCIs
    if (!la) la = LockStep(a, &ia, 40000);
    if (!lb) lb = LockStep(b, &ib, 40000);
  }
  ASSERT_TRUE(la);
  ASSERT_TRUE(lb);
  const Locked ra = ReadLocked(a), rb = ReadLocked(b);
  EXPECT_TRUE(ra.locked);
  EXPECT_TRUE(rb.locked);
  EXPECT_GT(ra.n, 150);
  EXPECT_GT(rb.n, 150);
  EXPECT_EQ(ra.ok, ra.n);
  EXPECT_EQ(rb.ok, rb.n);
  EXPECT_LE(ra.bwp_size, 217);
  EXPECT_LE(rb.bwp_size, 106);
  EXPECT_GT(ra.bwp_size, 106);  // A's lock is a wide BWP: B's 106-PRB layout did not overwrite it
  a.reset();                    // release cell A only
  EXPECT_FALSE(a.searching());  // no cell known after the reset
  EXPECT_EQ(ReadLocked(a).n, 0);  // and no lock to read with
  EXPECT_FALSE(b.provisional());  // B is still locked
  const Locked rb2 = ReadLocked(b);
  EXPECT_GT(rb2.n, 150);
  EXPECT_EQ(rb2.ok, rb2.n);
  b.reset();
}

// UL data-id lock table (nr_pusch_passive_ul_ids.c g_data_lock): the SAME RNTI locks a different n_ID in each cell; a release in one cell
// leaves the other. Also the per-cell TB-pass window (nr_pusch_passive_ul_crc_note) is not shared.
TEST(MpciA6UlIds, DataIdLockPerCell)
{
  const nr_ue_ul_ticket_t t = {.identity_gen = 1, .epoch = 1, .incarnation = 1, .rnti = 0x4abc};
  uint16_t id = 0;
  {
    Bind b(0);
    nr_pusch_passive_ul_ids_reset_all();
    EXPECT_FALSE(nr_pusch_passive_ul_data_id_locked(t.rnti, &id));
  }
  {
    Bind b(1);
    nr_pusch_passive_ul_ids_reset_all();
    EXPECT_FALSE(nr_pusch_passive_ul_data_id_locked(t.rnti, &id));
  }
  { Bind b(0); nr_pusch_passive_ul_ids_confirm(&t, 0, 415, 100, 6000, 1000); }
  {
    Bind b(1);
    EXPECT_FALSE(nr_pusch_passive_ul_data_id_locked(t.rnti, &id)) << "cell 0's lock leaked into cell 1";
    nr_pusch_passive_ul_ids_confirm(&t, 0, 222, 200, 6000, 1001);
  }
  {
    Bind b(0);
    ASSERT_TRUE(nr_pusch_passive_ul_data_id_locked(t.rnti, &id));
    EXPECT_EQ(id, 100);
  }
  {
    Bind b(1);
    ASSERT_TRUE(nr_pusch_passive_ul_data_id_locked(t.rnti, &id));
    EXPECT_EQ(id, 200);
  }
  { Bind b(0); nr_pusch_passive_ul_ids_reset(t.rnti); EXPECT_FALSE(nr_pusch_passive_ul_data_id_locked(t.rnti, &id)); }
  {
    Bind b(1);
    ASSERT_TRUE(nr_pusch_passive_ul_data_id_locked(t.rnti, &id));  // cell 1 keeps its lock
    EXPECT_EQ(id, 200);
    nr_pusch_passive_ul_ids_reset_all();
  }
  // the lock-free reads of two threads bound to their cells
  { Bind b(0); nr_pusch_passive_ul_ids_confirm(&t, 0, 415, 101, 6000, 2000); }
  { Bind b(1); nr_pusch_passive_ul_ids_confirm(&t, 0, 222, 201, 6000, 2001); }
  uint16_t i0 = 0, i1 = 0;
  std::thread th0([&] { nr_cell_bind(0); nr_pusch_passive_ul_data_id_locked(t.rnti, &i0); });
  std::thread th1([&] { nr_cell_bind(1); nr_pusch_passive_ul_data_id_locked(t.rnti, &i1); });
  th0.join();
  th1.join();
  EXPECT_EQ(i0, 101);
  EXPECT_EQ(i1, 201);
  { Bind b(0); nr_pusch_passive_ul_ids_reset_all(); }
  { Bind b(1); nr_pusch_passive_ul_ids_reset_all(); }
}

// UL background discovery: the worker is started by the cell that calls nr_ul_bgdisc_start and runs bound to it (its runner sees the
// starter's cell index), the queues and the statistics are per cell.
namespace {
std::atomic<int> g_ran_rnti_cell0{0}, g_ran_rnti_cell1{0}, g_bad_cell{0};
int Runner(const nr_ul_bgdisc_job_t *j)
{
  const int c = nr_cell_idx();
  if (c == 0) g_ran_rnti_cell0 = j->rnti;
  else if (c == 1) g_ran_rnti_cell1 = j->rnti;
  else g_bad_cell++;
  return 0;
}
nr_ul_bgdisc_job_t Job(uint16_t rnti, std::vector<c16_t> &iq)
{
  nr_ul_bgdisc_job_t j;
  memset(&j, 0, sizeof(j));
  j.rnti = rnti;
  j.nant = 1;
  j.length = (uint32_t)iq.size();
  j.iq[0] = iq.data();
  return j;
}
}  // namespace

TEST(MpciA6UlBgdisc, WorkerAndStatsPerCell)
{
  std::vector<c16_t> iq(4096);
  { Bind b(0); ASSERT_EQ(nr_ul_bgdisc_start(4, nullptr), 0); }  // queue up, no worker yet: jobs are held back
  { Bind b(1); ASSERT_EQ(nr_ul_bgdisc_start(4, nullptr), 0); }
  { Bind b(0); const nr_ul_bgdisc_job_t j = Job(0x1111, iq); ASSERT_EQ(nr_ul_bgdisc_submit(&j), 1); const nr_ul_bgdisc_job_t k = Job(0x1111, iq); EXPECT_EQ(nr_ul_bgdisc_submit(&k), 0); /* duplicate RNTI */ }
  { Bind b(1); const nr_ul_bgdisc_job_t j = Job(0x2222, iq); ASSERT_EQ(nr_ul_bgdisc_submit(&j), 1);
    const nr_ul_bgdisc_job_t k = Job(0x1111, iq); EXPECT_EQ(nr_ul_bgdisc_submit(&k), 1); /* the same RNTI is NOT a duplicate in another cell */ }
  nr_ul_bgdisc_stats_t s0, s1;
  { Bind b(0); nr_ul_bgdisc_stats(&s0); }
  { Bind b(1); nr_ul_bgdisc_stats(&s1); }
  EXPECT_EQ(s0.submitted, 1u);
  EXPECT_EQ(s0.dup, 1u);
  EXPECT_EQ(s1.submitted, 2u);
  EXPECT_EQ(s1.dup, 0u);
  { Bind b(0); ASSERT_EQ(nr_ul_bgdisc_start(4, Runner), 0); }  // the worker of cell 0 only
  for (int i = 0; i < 300 && g_ran_rnti_cell0 == 0; i++) usleep(10000);
  EXPECT_EQ(g_ran_rnti_cell0.load(), 0x1111);
  usleep(30000);
  EXPECT_EQ(g_ran_rnti_cell1.load(), 0) << "cell 1 has no worker yet: its jobs must wait";
  { Bind b(1); const nr_ul_bgdisc_job_t j = Job(0x3333, iq); ASSERT_EQ(nr_ul_bgdisc_submit(&j), 1); }  // fresh (the held ones are older than the age limit)
  { Bind b(1); ASSERT_EQ(nr_ul_bgdisc_start(4, Runner), 0); }
  for (int i = 0; i < 300 && g_ran_rnti_cell1 == 0; i++) usleep(10000);
  EXPECT_EQ(g_ran_rnti_cell1.load(), 0x3333);
  EXPECT_EQ(g_ran_rnti_cell0.load(), 0x1111) << "cell 1's job ran on cell 0's worker";
  EXPECT_EQ(g_bad_cell.load(), 0);
  { Bind b(0); nr_ul_bgdisc_stop(); }
  { Bind b(1); nr_ul_bgdisc_stop(); }
}

// MPCI-LAZY: the lazily started workers are created parked per cell BEFORE the fence; the first use only arms them; a late start (after stop) still works.
TEST(MpciLazy, WorkersParkedPerCellBeforeTheFenceAndArmedLater)
{
  for (int cell = 0; cell < 2; cell++) {
    Bind b(cell);
    nr_ul_layout_cell_prealloc(cell);
    nr_ul_bgdisc_cell_prealloc(cell);
    nr_pdcch_uss_tracker_cell_prealloc(cell);
  }
  for (int cell = 0; cell < 2; cell++) {
    EXPECT_EQ(nr_ul_layout_parked_state(cell), 1) << "ul_layout eval, cell " << cell;
    EXPECT_EQ(nr_ul_bgdisc_parked_state(cell), 1) << "ulBgDisc, cell " << cell;
    EXPECT_EQ(nr_pdcch_uss_tracker_parked_state(cell), 1) << "ussTracker, cell " << cell;
  }
  // arming: cell 1's ulBgDisc runs a job through the parked worker (bound to cell 1), cell 0's stays parked
  std::vector<c16_t> iq(4096);
  { Bind b(1); ASSERT_EQ(nr_ul_bgdisc_start(4, nullptr), 0); }  // queue up, no worker: the parked thread stays parked
  g_ran_rnti_cell0 = 0; g_ran_rnti_cell1 = 0; g_bad_cell = 0;
  { Bind b(1); const nr_ul_bgdisc_job_t j = Job(0x4444, iq); ASSERT_EQ(nr_ul_bgdisc_submit(&j), 1); ASSERT_EQ(nr_ul_bgdisc_start(4, Runner), 0); }
  for (int i = 0; i < 300 && g_ran_rnti_cell1 == 0; i++) usleep(10000);
  EXPECT_EQ(g_ran_rnti_cell1.load(), 0x4444);
  EXPECT_EQ(nr_ul_bgdisc_parked_state(1), 2);
  EXPECT_EQ(nr_ul_bgdisc_parked_state(0), 1);
  EXPECT_EQ(g_ran_rnti_cell0.load(), 0);
  EXPECT_EQ(g_bad_cell.load(), 0);
  { Bind b(1); nr_ul_bgdisc_stop(); }
  // a late start (re-add after stop): no parked thread left, the old lazy start still works
  { Bind b(1); ASSERT_EQ(nr_ul_bgdisc_start(4, nullptr), 0); const nr_ul_bgdisc_job_t j = Job(0x5555, iq); ASSERT_EQ(nr_ul_bgdisc_submit(&j), 1); ASSERT_EQ(nr_ul_bgdisc_start(4, Runner), 0); }
  for (int i = 0; i < 300 && g_ran_rnti_cell1 != 0x5555; i++) usleep(10000);
  EXPECT_EQ(g_ran_rnti_cell1.load(), 0x5555);
  { Bind b(1); nr_ul_bgdisc_stop(); }
}

int main(int argc, char **argv)
{
  logInit();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
