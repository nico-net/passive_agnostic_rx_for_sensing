/* mpci A3: two-cell interleave test for the per-cell state of the acquisition / config / UE-context / observation /
 * reconfiguration / CORESET-map modules (plan 2026-10-23-mpci-main-plan.md, Review Focus 1).
 *
 * For every module a workload X (variant 0) and a different workload Y (variant 1) is run
 *   (1) solo: X alone on one cell, Y alone on another cell  -> the reference results;
 *   (2) interleaved on ONE thread that re-binds between the steps of the two cells (nr_cell_bind);
 *   (3) interleaved on TWO threads, each bound to its cell, taking turns step by step.
 * Each cell's result must equal its solo result. A shared (unconverted) static makes (2) and (3) differ.
 * Cells 1..4 are used so that the cell-0 (single-cell) tests are untouched. */
#include <gtest/gtest.h>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
#include <sys/stat.h>
extern "C" {
#include "common/utils/nr_cell.h"
#include "common/utils/LOG/log.h"
#include "common/config/config_userapi.h"
#include "nr_passive_acq_state.h"
#include "nr_tdd_pattern.h"
#include "nr_passive_cfg_epoch.h"
#include "nr_passive_obs.h"
#include "nr_passive_ue_ctx.h"
#include "nr_passive_reconf_rules.h"
#include "nr_pdcch_coreset_map.h"
configmodule_interface_t *uniqCfg = nullptr;
void exit_function(const char *, const char *, int, const char *, int) { std::abort(); }
}

namespace {

struct Work {
  std::vector<std::function<void()>> steps;
  std::function<void()> setup, teardown;
  std::function<std::string()> result;
};
using Maker = std::function<Work(int variant)>;

void run_solo_steps(const Work &w)
{
  for (const auto &s : w.steps) s();
}
std::string solo(const Maker &mk, int variant, int cell)
{
  nr_cell_bind(cell);
  Work w = mk(variant);
  w.setup();
  run_solo_steps(w);
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
    w.setup();
    for (size_t i = 0; i < n; i++) {
      std::unique_lock<std::mutex> lk(m);
      cv.wait(lk, [&] { return turn == me; });
      if (i < w.steps.size()) w.steps[i]();
      turn = 1 - me;
      cv.notify_all();
    }
    out = w.result();
    w.teardown();
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
  // swapping the cells' roles gives the same results again (no dependence on the cell index itself)
  auto sw = interleave_1thread(mk, 4, 3);
  EXPECT_EQ(sw.first, sa);
  EXPECT_EQ(sw.second, sb);
}

/* ---- acquisition state ---- */
Work mk_acq(int v)
{
  Work w;
  w.setup = [] { nr_passive_acq_reset(); };
  w.teardown = [] { nr_passive_acq_reset(); };
  const int n_rb = v ? 51 : 106, mu = v ? 0 : 1;
  w.steps.push_back([=] { nr_passive_acq_set_phy_geometry(n_rb, mu, v ? 30 : 0, v ? 2.1e9 : 3.7e9); });
  w.steps.push_back([] { nr_passive_acq_note_pbch_locked(); });
  w.steps.push_back([=] {
    nr_passive_acq_inputs_t in{};
    in.pdcch_length_found = true;
    in.dl_search_winners = v ? 0 : 2;
    nr_passive_acq_update(&in);
  });
  w.steps.push_back([=] { nr_passive_acq_note_sib1(); });
  w.steps.push_back([=] { nr_passive_acq_note_sib1_carrier(v ? 52 : n_rb, mu, v ? 3 : 0, 0, v ? 4 : 0); });
  w.steps.push_back([=] {
    nr_tdd_pattern_t p{};
    p.period_slots = v ? 10 : 20;
    p.dl_slots = v ? 6 : 14;
    p.dl_symbols = v ? 6 : 2;
    p.ul_slots = v ? 3 : 4;
    p.ul_symbols = v ? 2 : 4;
    nr_passive_acq_note_sib1_tdd(&p, nullptr);
    nr_passive_acq_note_sib1_tdd_ref_mu(mu);
  });
  w.steps.push_back([=] {
    if (v) nr_passive_acq_note_sync_loss();
  });
  w.result = [=] {
    const nr_passive_acq_snapshot_t s = nr_passive_acq_snapshot();
    std::ostringstream o;
    o << nr_passive_acq_state_name(s.state) << ' ' << s.updates << ' ' << s.transitions << ' ' << s.sync_losses << ' ' << s.pbch_locks << ' '
      << s.sib1_decodes << ' ' << s.carrier_verified << ' ' << s.carrier.bw_match << s.carrier.mu_match << s.carrier.grid_match << ' '
      << s.carrier.point_a_subcarrier << ' ' << s.carrier.carrier_end_subcarrier << ' ' << s.carrier.derived_centre_hz << ' '
      << nr_passive_acq_tdd_known() << ' ' << nr_passive_acq_tdd_period_slots(mu);
    for (uint32_t sl = 0; sl < 40; sl++) o << nr_passive_acq_tdd_slot_has_downlink(sl);
    return o.str();
  };
  return w;
}

/* ---- configuration epoch ---- */
std::atomic<int> g_listener_calls[NR_CELL_MAX];
void epoch_listener(const nr_cfg_epoch_snapshot_t *) { g_listener_calls[nr_cell_idx()]++; }
Work mk_epoch(int v)
{
  Work w;
  w.setup = [] {
    nr_cfg_epoch_reset();
    g_listener_calls[nr_cell_idx()] = 0;
    nr_cfg_epoch_subscribe(epoch_listener);
  };
  w.teardown = [] { nr_cfg_epoch_reset(); };
  w.steps.push_back([=] { nr_cfg_epoch_note_identity(v ? 421 : 415, v ? 640000 : 636666, v ? 7000 : 6000); });
  w.steps.push_back([=] { nr_cfg_epoch_note_mib(v ? 0xaaaa : 0x1234); });
  w.steps.push_back([=] { nr_cfg_epoch_note_sib1(v ? 0x5151 : 0x4242, 100); });
  w.steps.push_back([=] { nr_cfg_epoch_note_identity(v ? 422 : 415, v ? 640000 : 636666, v ? 7000 : 6000); });
  w.steps.push_back([=] { nr_cfg_epoch_note_sib1(v ? 0x5152 : 0x4242, 200); });
  w.steps.push_back([=] { if (v) nr_cfg_epoch_note_bwp_change(); });
  w.steps.push_back([=] { nr_cfg_epoch_tick(300 + (v ? 11 : 0)); });
  w.steps.push_back([] { nr_cfg_epoch_drain(); });
  w.result = [] {
    const nr_cfg_epoch_snapshot_t s = nr_cfg_epoch_snapshot();
    std::ostringstream o;
    o << s.epoch << ' ' << s.identity_gen << ' ' << s.dedicated_epoch << ' ' << s.last_class << ' ' << s.last_cause << ' ' << nr_cfg_epoch_current()
      << ' ' << nr_cfg_epoch_identity_gen() << ' ' << nr_cfg_epoch_cfr_stamp() << ' ' << nr_cfg_epoch_current_slot() << ' ' << g_listener_calls[nr_cell_idx()].load();
    return o.str();
  };
  return w;
}

/* ---- observation writer ---- */
std::string slurp(const std::string &p)
{
  std::ifstream f(p, std::ios::binary);
  std::ostringstream s;
  s << f.rdbuf();
  return s.str();
}
Work mk_obs(int v)
{
  struct St { std::string path; };
  auto st = std::make_shared<St>();
  Work w;
  w.setup = [st] {
    char name[] = "/tmp/mpci-a3-obs-XXXXXX";
    const int fd = mkstemp(name);
    close(fd);
    st->path = name;
    EXPECT_TRUE(nr_passive_obs_open(name, 64));
  };
  w.teardown = [st] { nr_passive_obs_close(); unlink(st->path.c_str()); };
  for (int i = 0; i < 12; i++)
    w.steps.push_back([=] {
      nr_passive_obs_t o{};
      o.abs_slot = 1000 + i + (v ? 5000 : 0);
      o.t_mono_ns = 1000000ull * (i + 1);
      o.rnti = v ? 0x2222 : 0x1111;
      o.dir = NR_OBS_DIR_DL;
      o.mcs = v ? 9 : 3;
      o.nb_rb = v ? 50 : 20;
      o.pci = v ? 421 : 415;
      EXPECT_TRUE(nr_passive_obs_push(&o));
    });
  w.result = [st] {
    nr_passive_obs_close(); // drains the cell's ring into its file
    uint64_t pushed = 0, written = 0, dropped = 0;
    nr_passive_obs_stats(&pushed, &written, &dropped);
    std::ostringstream o;
    o << pushed << ' ' << written << ' ' << dropped << '\n' << slurp(st->path);
    return o.str();
  };
  return w;
}

/* ---- UE context ---- */
Work mk_uectx(int v)
{
  Work w;
  w.setup = [] { EXPECT_TRUE(nr_ue_ctx_open(nullptr, 64, 0)); };
  w.teardown = [] { nr_ue_ctx_close(); };
  w.steps.push_back([=] { nr_ue_ctx_on_sib1_param(NR_UEP_SIB1_HASH, v ? 0x77 : 0x33, 5); });
  for (int i = 0; i < 6; i++)
    w.steps.push_back([=] {
      nr_passive_obs_t o{};
      o.rnti = v ? 0x4444 : 0x3333;
      o.dir = NR_OBS_DIR_DL;
      o.abs_slot = 100 + i * 2;
      o.t_mono_ns = 1000000000ull + 10000000ull * i;
      o.crc = NR_OBS_CRC_OK;
      o.mcs = v ? 12 : 4;
      o.nl = 1;
      o.nb_rb = v ? 40 : 20;
      o.start_rb = 10;
      o.tbs = v ? 1600 : 800;
      o.scs_khz = v ? 30 : 15;
      nr_ue_ctx_on_obs(&o);
    });
  w.result = [=] {
    nr_ue_ctx_close(); // joins the worker: every queued event is applied
    std::ostringstream o;
    nr_ue_ctx_t c{};
    const uint16_t mine = v ? 0x4444 : 0x3333, other = v ? 0x3333 : 0x4444;
    const bool got = nr_ue_ctx_get(mine, &c);
    nr_ue_ctx_t x{};
    o << got << ' ' << nr_ue_ctx_get(other, &x) << ' ';
    if (got) o << c.state << ' ' << c.st.grants_dl << ' ' << c.rnti << ' ' << c.cfg[NR_UEP_SIB1_HASH].value;
    uint64_t ev = 0, lines = 0, drops = 0;
    nr_ue_ctx_stats(&ev, &lines, &drops);
    o << ' ' << ev << ' ' << drops;
    return o.str();
  };
  return w;
}

/* ---- reconfiguration rules ---- */
Work mk_reconf(int v)
{
  Work w;
  w.setup = [] { nr_reconf_reset(); };
  w.teardown = [] { nr_reconf_reset(); };
  w.steps.push_back([=] { nr_reconf_rrc_expect(0x1234, v ? 5000 : 100, v ? NR_RECONF_RRC_SETUP : NR_RECONF_RRC_RECONF, v ? 0 : 1, v); });
  w.steps.push_back([=] { nr_reconf_bwp_switch_dci(0x1234, v ? 2 : 1, v ? 6000 : 300, v ? 0 : 1); });
  w.steps.push_back([=] { nr_reconf_rrc_expect(0x2345, 777 + v, NR_RECONF_RRC_RESUME, 1, false); });
  w.result = [=] {
    std::ostringstream o;
    for (int64_t s = 0; s < 9000; s += 53) o << nr_reconf_rrc_phase(0x1234, s, v ? 0 : 1) << nr_reconf_rrc_phase(0x2345, s, 1);
    int64_t f = -1, l = -1;
    const bool exp = nr_reconf_bwp_expected_window(0x1234, &f, &l);
    uint16_t rn = 0;
    int en = -1;
    const bool due = nr_reconf_bwp_due(100000, &rn, &en);
    o << ' ' << nr_reconf_rrc_effective_slot(0x1234) << ' ' << exp << ' ' << f << ' ' << l << ' ' << due << ' ' << rn << ' ' << en;
    return o.str();
  };
  return w;
}

/* ---- CORESET map (phase hint, CSS0 span) ---- */
Work mk_cormap(int v)
{
  Work w;
  w.setup = [] { nr_pdcch_coreset_map_set_phase_hint(0); nr_pdcch_coreset_map_set_css0(-1, 0); };
  w.teardown = w.setup;
  w.steps.push_back([=] { nr_pdcch_coreset_map_set_phase_hint(v ? 13 : 8); });
  w.steps.push_back([=] { nr_pdcch_coreset_map_set_css0(v ? 12 : 3, v ? 48 : 24); });
  w.steps.push_back([=] { nr_pdcch_coreset_map_idsweep_gate_bypass(v); });
  w.result = [] {
    int f = 0, n = 0;
    nr_pdcch_coreset_map_get_css0(&f, &n);
    uint64_t a = 0, b = 0, c = 0;
    float nz = 0;
    nr_pdcch_nid_capture_metrics(&a, &b, &c, &nz);
    std::ostringstream o;
    o << nr_pdcch_coreset_map_get_phase() << ' ' << f << ' ' << n << ' ' << a << b << c;
    return o.str();
  };
  return w;
}

} // namespace

TEST(MpciA3Interleave, AcquisitionState) { check(mk_acq, "nr_passive_acq_state"); }
TEST(MpciA3Interleave, ConfigEpoch) { check(mk_epoch, "nr_passive_cfg_epoch"); }
TEST(MpciA3Interleave, ObservationWriter) { check(mk_obs, "nr_passive_obs"); }
TEST(MpciA3Interleave, UeContext) { check(mk_uectx, "nr_passive_ue_ctx"); }
TEST(MpciA3Interleave, ReconfRules) { check(mk_reconf, "nr_passive_reconf_rules"); }
TEST(MpciA3Interleave, CoresetMap) { check(mk_cormap, "nr_pdcch_coreset_map"); }

/* Host-global path of the replay capture / obs files: cell 0 unchanged, cell k > 0 distinct (nr_cell_path). */
TEST(MpciA3Interleave, ObsSessionsAreIndependentPerCell)
{
  char p0[] = "/tmp/mpci-a3-o0-XXXXXX", p1[] = "/tmp/mpci-a3-o1-XXXXXX";
  close(mkstemp(p0));
  close(mkstemp(p1));
  nr_cell_bind(1);
  ASSERT_TRUE(nr_passive_obs_open(p0, 16));
  nr_cell_bind(2);
  ASSERT_TRUE(nr_passive_obs_open(p1, 16)) << "a second cell must be able to open its own session";
  EXPECT_FALSE(nr_passive_obs_open(p1, 16)) << "a cell cannot open twice";
  EXPECT_TRUE(nr_passive_obs_enabled());
  nr_cell_bind(7); // a cell that never opened (cells 1..4 were opened by the tests above): its pushes are refused and are not counted as after-close pushes
  nr_passive_obs_t o{};
  EXPECT_FALSE(nr_passive_obs_push(&o));
  EXPECT_EQ(nr_passive_obs_after_close(), 0u);
  nr_cell_bind(1);
  nr_passive_obs_close();
  EXPECT_TRUE(nr_passive_obs_enabled()) << "cell 2 is still open: the process-wide fast-path hint stays set";
  nr_cell_bind(2);
  nr_passive_obs_close();
  EXPECT_FALSE(nr_passive_obs_enabled());
  nr_cell_bind(0);
  unlink(p0);
  unlink(p1);
}

int main(int argc, char **argv)
{
  setenv("ISAC_RECONF", "1", 1); // existing env switch, as in the cfg_epoch test
  logInit();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
