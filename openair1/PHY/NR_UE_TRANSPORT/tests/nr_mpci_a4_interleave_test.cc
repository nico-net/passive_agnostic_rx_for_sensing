/* mpci A4: two-cell interleave test for the per-cell state of the blind PDCCH monitor modules (plan 2026-10-23-mpci-main-plan.md,
 * Review Focus 1; Task A4 STOP: "PDCCH accept sets of cell A unchanged when cell B runs").
 *
 * For every module a workload X (variant 0) and a different workload Y (variant 1) is run
 *   (1) solo: X alone on one cell, Y alone on another cell  -> the reference results;
 *   (2) interleaved on ONE thread that re-binds between the steps of the two cells (nr_cell_bind);
 *   (3) interleaved on TWO threads, each bound to its cell, taking turns step by step;
 *   (4) the same with the cells' roles swapped (no dependence on the cell index itself).
 * Each cell's result must equal its solo result. A shared (unconverted) static makes (2)-(4) differ.
 * Cells 1..4 are used, so the cell-0 (single-cell) behaviour is covered by the existing module tests.
 * The rt-file objects (nr_pdcch_blind_monitor_rt.c) are not linkable into a unit test; the cell DCI length hint and the UL
 * cell-length table, which that file keeps per cell, live in nr_pdcch_cell_len_state.h and are driven here directly. */
#include <gtest/gtest.h>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "common/utils/nr_cell.h"
#include "common/utils/LOG/log.h"
#include "openair2/LAYER2/NR_MAC_COMMON/nr_mac_common.h"
#include "common/config/config_userapi.h"
#include "nr_pdcch_blind_monitor.h"
#include "nr_pdcch_blind_phase2.h"
#include "nr_pdcch_coreset_bank.h"
#include "nr_pdcch_dci_length_sweep.h"
#include "nr_pdcch_sib1_prior.h"
#include "nr_dl_traffic_probe.h"
#include "nr_pdcch_cell.h"
void a4_cll_reset(void);
void a4_cll_note(int len);
int a4_cll_get(int i);
void a4_cll_ul_note(unsigned rnti, int len);
int a4_cll_ul_get(unsigned rnti);
configmodule_interface_t *uniqCfg = nullptr;
void exit_function(const char *, const char *, int, const char *, int) { std::abort(); }
}
/* the linked modules read the softmodem parameters (passive-RX mode flags): all zero, as in nr_pdcch_blind_monitor_test.cc */
extern "C" softmodem_params_t *get_softmodem_params(void)
{
  static softmodem_params_t p;
  return &p;
}
#define NR_CELL_LEN_HINT_N 4 /* nr_pdcch_cell_len_hint.h */

namespace {

struct Work {
  std::vector<std::function<void()>> steps;
  std::function<void()> setup, teardown;
  std::function<std::string()> result;
};
using Maker = std::function<Work(int variant)>;

std::string solo(const Maker &mk, int variant, int cell)
{
  nr_cell_bind(cell);
  Work w = mk(variant);
  w.setup();
  for (const auto &s : w.steps) s();
  std::string r = w.result();
  w.teardown();
  nr_cell_bind(0);
  return r;
}

/* One thread, re-binding between the cells' steps. */
std::pair<std::string, std::string> interleave_1thread(const Maker &mk, int ca, int cb)
{
  Work a = mk(0), b = mk(1);
  nr_cell_bind(ca); a.setup();
  nr_cell_bind(cb); b.setup();
  const size_t n = std::max(a.steps.size(), b.steps.size());
  for (size_t i = 0; i < n; i++) {
    if (i < a.steps.size()) { nr_cell_bind(ca); a.steps[i](); }
    if (i < b.steps.size()) { nr_cell_bind(cb); b.steps[i](); }
  }
  nr_cell_bind(ca); std::string ra = a.result(); a.teardown();
  nr_cell_bind(cb); std::string rb = b.result(); b.teardown();
  nr_cell_bind(0);
  return {ra, rb};
}

/* Two threads (each bound to its cell) taking turns step by step: A0 B0 A1 B1 ... */
std::pair<std::string, std::string> interleave_2threads(const Maker &mk, int ca, int cb)
{
  Work a = mk(0), b = mk(1);
  std::mutex m;
  std::condition_variable cv;
  int turn = 0; // 0: A, 1: B
  std::string ra, rb;
  const size_t n = std::max(a.steps.size(), b.steps.size());
  auto body = [&](Work &w, int cell, int me, std::string &out) {
    nr_cell_bind(cell);
    {
      std::unique_lock<std::mutex> lk(m);
      cv.wait(lk, [&] { return turn == me; });
      w.setup();
      turn = 1 - me;
      cv.notify_all();
    }
    for (size_t i = 0; i < n; i++) {
      std::unique_lock<std::mutex> lk(m);
      cv.wait(lk, [&] { return turn == me; });
      if (i < w.steps.size()) w.steps[i]();
      turn = 1 - me;
      cv.notify_all();
    }
    {
      std::unique_lock<std::mutex> lk(m);
      cv.wait(lk, [&] { return turn == me; });
      out = w.result();
      w.teardown();
      turn = 1 - me;
      cv.notify_all();
    }
  };
  std::thread ta(body, std::ref(a), ca, 0, std::ref(ra));
  std::thread tb(body, std::ref(b), cb, 1, std::ref(rb));
  ta.join();
  tb.join();
  return {ra, rb};
}

void check(const Maker &mk, const char *name)
{
  SCOPED_TRACE(name);
  const std::string sa = solo(mk, 0, 3), sb = solo(mk, 1, 4);
  ASSERT_FALSE(sa.empty());
  ASSERT_NE(sa, sb) << "the two workloads must differ for the test to mean anything";
  auto one = interleave_1thread(mk, 1, 2);
  EXPECT_EQ(one.first, sa) << "cell A changed when cell B ran on the same thread";
  EXPECT_EQ(one.second, sb) << "cell B changed when cell A ran on the same thread";
  auto two = interleave_2threads(mk, 1, 2);
  EXPECT_EQ(two.first, sa) << "cell A changed when cell B ran on another thread";
  EXPECT_EQ(two.second, sb) << "cell B changed when cell A ran on another thread";
  auto sw = interleave_1thread(mk, 4, 3);
  EXPECT_EQ(sw.first, sa);
  EXPECT_EQ(sw.second, sb);
  auto sw2 = interleave_2threads(mk, 4, 3);
  EXPECT_EQ(sw2.first, sa);
  EXPECT_EQ(sw2.second, sb);
}

/* A shared trace the steps append to; the result is its text. */
struct Trace {
  std::ostringstream o;
};

/* ---- SIB1 prior ---- */
Work mk_sib1(int v)
{
  auto tr = std::make_shared<Trace>();
  Work w;
  w.setup = [] {};
  w.teardown = [] {};
  w.steps.push_back([=] {
    nr_pdcch_sib1_prior_t p{};
    p.valid = true;
    p.dl_bwp_valid = true;
    p.dl_bwp_start = v ? 12 : 0;
    p.dl_bwp_size = v ? 51 : 106;
    p.coreset_valid = true;
    p.coreset_id = 0;
    p.duration = v ? 3 : 2;
    p.ss_valid = true;
    p.ss_period_slots = v ? 20 : 40;
    p.ss_offset_slots = v ? 3 : 11;
    p.ss_duration = v ? 1 : 2;
    nr_pdcch_sib1_prior_set(&p);
  });
  for (int k = 0; k < 3; k++)
    w.steps.push_back([=] {
      const nr_pdcch_sib1_prior_t *g = nr_pdcch_sib1_prior_get();
      int a = -1, b = -1;
      const bool ok = nr_pdcch_sib1_prior_window(&a, &b);
      tr->o << g->valid << ' ' << g->dl_bwp_start << '+' << g->dl_bwp_size << ' ' << (int)g->duration << ' ' << g->ss_period_slots << '/'
            << g->ss_offset_slots << ' ' << ok << ' ' << a << ".." << b << ';';
    });
  w.result = [=] { return tr->o.str(); };
  return w;
}

/* ---- Phase 2: persistence ring, DL accept gate, energy floor (the "accept set" of the monitor) ---- */
Work mk_phase2(int v)
{
  auto tr = std::make_shared<Trace>();
  auto thres = std::make_shared<int>(v ? 40 : 90);
  Work w;
  w.setup = [] { nr_pdcch_blind_phase2_reset_for_test(); };
  w.teardown = [] { nr_pdcch_blind_phase2_reset_for_test(); };
  const uint16_t real = v ? 0x4602 : 0x4601;
  for (int k = 0; k < 160; k++)
    w.steps.push_back([=] {
      const uint32_t slot = 1000u + (uint32_t)k * (v ? 3u : 2u);
      // the real UE recurs every 4th step, the rest are one-off noise RNTIs
      const uint16_t rnti = (k % 4 == 0) ? real : (uint16_t)(0x1000 + k * (v ? 7 : 5));
      const int mism = (k * (v ? 11 : 13)) % 80;
      nr_pdcch_blind_phase2_lock();
      const bool acc = nr_pdcch_blind_dl_accept_gate(thres.get(), mism, rnti, slot, v ? 200u : 100u, v ? 3 : 2);
      nr_pdcch_blind_phase2_unlock();
      const bool per = (k % 5 == 0) ? (nr_pdcch_blind_phase2_lock(), nr_pdcch_blind_rnti_persistence_check(real, slot, 100, 2)) : false;
      if (k % 5 == 0) nr_pdcch_blind_phase2_unlock();
      uint64_t nseen = 0;
      const float f = nr_pdcch_blind_energy_floor_update(v ? 0.3f + 0.01f * (k % 9) : 5.0f + 0.1f * (k % 7), &nseen);
      char b[128];
      snprintf(b, sizeof(b), "%d:%x:%d:%d:%d:%a:%llu;", k, rnti, (int)acc, (int)per, *thres, (double)f, (unsigned long long)nseen);
      tr->o << b;
    });
  w.result = [=] {
    const int cnt = nr_pdcch_blind_persistence_count_for_test(real, nullptr); /* takes the Phase-2 lock itself */
    return tr->o.str() + "sightings=" + std::to_string(nr_pdcch_blind_persistence_sightings()) + " cnt=" + std::to_string(cnt);
  };
  return w;
}

/* ---- RNTI bootstrap table: the confirmed C-RNTI set ---- */
Work mk_boot(int v)
{
  auto tr = std::make_shared<Trace>();
  Work w;
  w.setup = [] { nr_pdcch_blind_rnti_bootstrap_reset_for_test(); };
  w.teardown = [] { nr_pdcch_blind_rnti_bootstrap_reset_for_test(); };
  for (int k = 0; k < 60; k++)
    w.steps.push_back([=] {
      const uint32_t slot = 2000u + (uint32_t)k * 3u;
      const uint16_t ue = (uint16_t)((v ? 0x5100 : 0x4100) + (k % 3));
      if (k % 2 == 0) nr_pdcch_blind_rnti_bootstrap_record(ue, NR_BLIND_RNTI_CLASS_C, slot);
      if (k % 9 == 0) nr_pdcch_blind_rnti_bootstrap_record_trusted((uint16_t)(v ? 0x5200 : 0x4200), NR_BLIND_RNTI_CLASS_TC, slot);
      uint16_t set[16] = {0};
      const int n = nr_pdcch_blind_monitor_confirmed_rnti_set(slot, set, 16);
      uint16_t r = 0; uint8_t cl = 0; uint32_t age = 0;
      const bool any = nr_pdcch_blind_monitor_confirmed_rnti(slot, &r, &cl, &age);
      char b[160];
      snprintf(b, sizeof(b), "%d:%d[", k, n);
      tr->o << b;
      for (int i = 0; i < n; i++) tr->o << std::hex << set[i] << std::dec << ',';
      tr->o << "]" << any << ':' << std::hex << r << std::dec << ':' << (int)cl << ':' << age << ';';
    });
  w.result = [=] {
    char buf[1024];
    nr_pdcch_blind_rnti_bootstrap_dump(2300, buf, sizeof(buf));
    return tr->o.str() + buf;
  };
  return w;
}

/* ---- CORESET bank ---- */
nr_pdcch_blind_monitor_cfg_t bank_cfg(int offset, int dur)
{
  nr_pdcch_blind_monitor_cfg_t c{};
  c.bwp_size = 106;
  c.coreset_rb_offset = offset;
  c.coreset_freq_domain = 4;
  c.coreset_duration = dur;
  c.dci_length_override = 47;
  return c;
}
void bank_clear()
{
  nr_pdcch_coreset_bank_set_remove_hook(nullptr, nullptr);
  while (nr_pdcch_coreset_bank_count()) nr_pdcch_coreset_bank_remove(0);
}
Work mk_bank(int v)
{
  auto tr = std::make_shared<Trace>();
  Work w;
  w.setup = [] { bank_clear(); };
  w.teardown = [] { bank_clear(); };
  for (int k = 0; k < 6; k++)
    w.steps.push_back([=] {
      auto c = bank_cfg(v ? 30 + 6 * k : 6 * k, v ? 1 : 2);
      const int idx = nr_pdcch_coreset_bank_add(&c, (uint16_t)(0x3000 + v * 0x100 + k));
      if (idx >= 0) {
        nr_pdcch_coreset_bank_note_accept(idx, 100 + k);
        nr_pdcch_coreset_bank_note_dci(idx, 100 + k, (uint16_t)(0x3000 + k), 0x1000u + (uint32_t)k);
      }
      nr_pdcch_coreset_bank_tick(200 + 40 * (uint64_t)k, v != 0, 50, 300);
      tr->o << k << ':' << idx << ':' << nr_pdcch_coreset_bank_count() << ':';
      for (int i = 0; i < nr_pdcch_coreset_bank_count(); i++)
        tr->o << nr_pdcch_coreset_bank_cfg(i)->coreset_rb_offset << '/' << (int)nr_pdcch_coreset_bank_state(i) << ',';
      tr->o << ';';
    });
  w.result = [=] { return tr->o.str() + "len_hint=" + std::to_string(nr_pdcch_coreset_bank_length_hint()); };
  return w;
}

/* ---- DCI length seen / order / learn counters ---- */
Work mk_len(int v)
{
  auto tr = std::make_shared<Trace>();
  Work w;
  w.setup = [] { nr_pdcch_dci_length_seen_reset(); };
  w.teardown = [] { nr_pdcch_dci_length_seen_reset(); };
  for (int k = 0; k < 6; k++)
    w.steps.push_back([=] {
      nr_pdcch_dci_length_note_seen(v ? 41 + k : 47 + 2 * k);
      int ord[NR_PDCCH_DCI_LENGTH_SWEEP_MAX_LEN];
      const int n = nr_pdcch_dci_length_order(30, 64, ord);
      tr->o << k << ':' << n << '[';
      for (int i = 0; i < n && i < 8; i++) tr->o << ord[i] << ',';
      tr->o << "]" << nr_pdcch_dci_length_learn_rounds(true) << nr_pdcch_dci_length_learn_rounds(false) << nr_pdcch_dci_length_wide_probes() << ';';
    });
  w.result = [=] { return tr->o.str(); };
  return w;
}

/* ---- cell DCI 1_1 length hint and UL cell-length table (the state of janon_note_cell_len, nr_pdcch_blind_monitor_rt.c) ---- */
Work mk_cell_len(int v)
{
  auto tr = std::make_shared<Trace>();
  Work w;
  w.setup = [] { a4_cll_reset(); };
  w.teardown = w.setup;
  // cell A locks length 47 (twice) then 49 once; cell B locks 41 then 43 (twice): each hint stays its own
  const int seq[2][5] = {{47, 47, 49, 47, 51}, {41, 43, 43, 43, 44}};
  for (int k = 0; k < 5; k++)
    w.steps.push_back([=] {
      a4_cll_note(seq[v][k]);
      a4_cll_ul_note(0x4600 + v * 16 + k, 36 + 3 * k + v);
      tr->o << k << ':';
      for (int i = 0; i < NR_CELL_LEN_HINT_N; i++) tr->o << a4_cll_get(i) << ',';
      tr->o << " ul=" << a4_cll_ul_get(0x4600 + v * 16 + k) << ',' << a4_cll_ul_get(0x4600 + (1 - v) * 16 + k) << ';';
    });
  w.result = [=] { return tr->o.str(); };
  return w;
}

/* ---- DL traffic probe: granted-RB registry ---- */
/* the grant ring has no reset: overwrite every entry with a sentinel that no query asks for */
void probe_flush()
{
  for (int i = 0; i < NR_DLTP_GRANTS; i++) nr_dltp_note_grant(65535, 255, 0, 1, 0, 1);
}
Work mk_probe(int v)
{
  auto tr = std::make_shared<Trace>();
  Work w;
  w.setup = [] { probe_flush(); };
  w.teardown = [] { probe_flush(); };
  for (int k = 0; k < 8; k++)
    w.steps.push_back([=] {
      // the SAME (frame, slot) in both cells, different RBs: a shared registry would return both grants
      nr_dltp_note_grant((uint16_t)(100 + k), (uint8_t)(k % 10), v ? 20 + k : k, v ? 5 : 8, 2, 12);
      nr_dltp_grant_t g[8];
      const int n = nr_dltp_grants_for((uint16_t)(100 + k), (uint8_t)(k % 10), g, 8);
      tr->o << k << ':' << n << ':';
      for (int i = 0; i < n; i++) tr->o << g[i].rb0 << '+' << g[i].n_rb << ',';
      tr->o << ';';
    });
  w.result = [=] { return tr->o.str(); };
  return w;
}

} // namespace

TEST(MpciA4Interleave, Sib1Prior) { check(mk_sib1, "nr_pdcch_sib1_prior"); }
TEST(MpciA4Interleave, Phase2AcceptGateAndPersistence) { check(mk_phase2, "nr_pdcch_blind_phase2: accept sets"); }
TEST(MpciA4Interleave, RntiBootstrapConfirmedSet) { check(mk_boot, "nr_pdcch_blind_rnti_bootstrap"); }
TEST(MpciA4Interleave, CoresetBank) { check(mk_bank, "nr_pdcch_coreset_bank"); }
TEST(MpciA4Interleave, DciLengthSeen) { check(mk_len, "nr_pdcch_dci_length_sweep"); }
TEST(MpciA4Interleave, CellLengthHintAndUlLengths) { check(mk_cell_len, "cell DCI length state"); }
TEST(MpciA4Interleave, TrafficProbeGrants) { check(mk_probe, "nr_dl_traffic_probe"); }

/* The hint of one cell is never fed by the locks of another (explicit statement of the PER_CELL rule for the cell-wide length hint). */
TEST(MpciA4Interleave, CellLengthHintsNeverCrossCells)
{
  nr_cell_bind(5);
  a4_cll_reset();
  a4_cll_note(47);
  a4_cll_note(47);
  nr_cell_bind(6);
  a4_cll_reset();
  a4_cll_note(41);
  EXPECT_EQ(a4_cll_get(0), 41);
  EXPECT_EQ(a4_cll_get(1), 0) << "cell 6 must not see the 47 of cell 5";
  nr_cell_bind(5);
  EXPECT_EQ(a4_cll_get(0), 47);
  EXPECT_EQ(a4_cll_get(1), 0);
  a4_cll_ul_note(0x4601, 39);
  nr_cell_bind(6);
  EXPECT_EQ(a4_cll_ul_get(0x4601), 0) << "a UL length locked in cell 5 is not known in cell 6";
  nr_cell_bind(0);
}

/* Heap objects of the PDCCH monitor (lane table, CORESET banks, ...) are per cell and allocated before the RX fence. */
TEST(MpciA4Interleave, PreallocIsIdempotentAndPerCell)
{
  nr_pdcch_blind_monitor_cell_prealloc(7);
  nr_pdcch_blind_monitor_cell_prealloc(7);
  nr_pdcch_coreset_bank_cell_prealloc(7);
  nr_pdcch_coreset_bank_cell_prealloc(8);
  nr_cell_bind(7);
  EXPECT_EQ(nr_pdcch_coreset_bank_count(), 0);
  auto c = bank_cfg(0, 2);
  EXPECT_EQ(nr_pdcch_coreset_bank_add(&c, 0x1234), 0);
  nr_cell_bind(8);
  EXPECT_EQ(nr_pdcch_coreset_bank_count(), 0) << "cell 8 has its own empty bank";
  nr_cell_bind(7);
  bank_clear();
  nr_cell_bind(0);
}

int main(int argc, char **argv)
{
  setenv("ISAC_RECONF", "1", 1); // existing env switch, as in the cfg_epoch / A3 tests
  logInit();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
