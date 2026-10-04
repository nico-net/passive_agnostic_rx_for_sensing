/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* CB0 elimination runtime wiring (td/cb0-cpu-wiring): per-grant scheduler (hash subset, budget, token bucket), backend
 * selection with GPU failure fallback, admissibility, engine adapter and the per-grant glue. The decode-side GrantWork
 * calls (nr_pdsch_passive_gw_cb0_item & co, nr_pdsch_passive_decode.c) are stubbed here with items from the CB0 fixture:
 * real OAI-encoded TBs decoded by the real CPU LDPC decoder. */
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <vector>
#include <thread>
#include <atomic>
extern "C" {
#include "nr_td_cb0_fixture.h"
#include "nr_td_cb0_sched.h"
#include "nr_td_cb0_wire.h"
#include "nr_td_cb0_adapter.h"
#include "nr_pdsch_config_sweep.h"
#include "nr_td_grantwork.h"
}

namespace {
struct EnvInit {
  EnvInit() { setenv("ISAC_TD_CB0_GPU_BACKOFF", "2", 1); }
} g_env_init;

/* ---------- fixture items: one true TB, one wrong-rv view of it (fails), small and fast (C == 1, BG2) ---------- */
struct Tb {
  cb0_fx_t fx{};
  Tb(uint32_t seed)
  {
    const uint16_t R = 1200;
    fx.A = cb0_fx_tbs(4, R, 6, 12, 6, 1);
    fx.R = R; fx.Qm = 4; fx.Nl = 1; fx.rv = 0; fx.G = cb0_fx_G(6, 12, 6, 1, 4, 1); fx.tbslbrm = 0;
    EXPECT_EQ(cb0_fx_encode(&fx, seed, 64), 0);
  }
  ~Tb() { cb0_fx_free(&fx); }
  nr_td_cb0_item_t pass_item() const { return cb0_fx_item(&fx); }
  nr_td_cb0_item_t fail_item() const
  {
    nr_td_cb0_item_t it = cb0_fx_item(&fx);
    it.rv = 2; /* wrong circular-buffer start: never decodes */
    return it;
  }
};
Tb *g_tb;

/* ---------- stubs of the decode-side calls ---------- */
struct Stub {
  int calls = 0;                   /* gw_cb0_item calls */
  std::vector<uint64_t> requested; /* hyp keys requested this grant */
  std::map<uint64_t, bool> pass;   /* hyp key -> CB0 passes (default fail) */
  int pattern = -1;                /* -1: the map; 0 / 1: keys with (key & 1) == pattern pass */
  uint32_t flags = 0;              /* gw flags returned */
  int rc_for_all = NR_TD_GW_OK;
} g_stub;
}  // namespace

extern "C" int nr_pdsch_passive_gw_cb0_item(nr_td_grantwork_t *gw, const nr_pdsch_cfg_hypothesis_t *h, nr_td_cb0_item_t *it,
                                            uint32_t *flags)
{
  (void)gw;
  g_stub.calls++;
  const uint64_t k = nr_td_cb0_hyp_key(h);
  g_stub.requested.push_back(k);
  if (flags)
    *flags = g_stub.flags;
  if (g_stub.rc_for_all != NR_TD_GW_OK)
    return g_stub.rc_for_all;
  auto f = g_stub.pass.find(k);
  const bool pass = g_stub.pattern >= 0 ? (int)(k & 1) == g_stub.pattern : (f != g_stub.pass.end() && f->second);
  *it = pass ? g_tb->pass_item() : g_tb->fail_item();
  return NR_TD_GW_OK;
}
extern "C" void *nr_pdsch_passive_cb0_cpu_ldpc(void) { return nullptr; /* the fixture registered the decoder */ }
extern "C" void nr_pdsch_passive_force_cpu_tb(bool) {}

namespace {
int32_t legal_a(int, int length, int start, int mapping_b, int add, int maxlen)
{
  return mapping_b ? 0 : 1 + start * 1000 + length * 40 + add * 3 + maxlen;
}

class Cb0Wire : public ::testing::Test {
 protected:
  static void SetUpTestSuite()
  {
    /* these tests drive the CPU backend through a stub decoder (and register fake GPU backends themselves): keep the
     * wiring from auto-registering the real CUDA CB0 adapter when libldpc_cuda.so is present (td/cb0-gpu-entry) */
    setenv("ISAC_TD_CB0_GPU_AUTOREG", "0", 1);
    cb0_fx_init();
    nr_td_cb0_use_gpu(0);
    nr_td_cb0_use_cuda_ldpc(0);
    g_tb = new Tb(7);
  }
  static void TearDownTestSuite()
  {
    delete g_tb;
    g_tb = nullptr;
  }
  void SetUp() override
  {
    unsetenv("ISAC_TD_CB0_BUDGET_US");
    unsetenv("ISAC_TD_CB0_CPU_PCT");
    nr_pdsch_config_sweep_reset_all();
    nr_pdsch_config_sweep_cb0_elim_env_set(1);
    nr_td_cb0_backend_mode_set(NR_TD_CB0_BE_AUTO);
    nr_td_cb0_register_gpu_backend(nullptr);
    nr_td_cb0_wire_reset();
    nr_td_cb0_wire_test_freeze(true);
    nr_td_cb0_wire_test_unlimited(true);
    cb0_fx_reinit_decoder();
    g_stub = Stub{};
    gw = make_gw();
  }
  void TearDown() override
  {
    nr_td_grantwork_release(gw);
    nr_pdsch_config_sweep_cb0_elim_env_set(-1);
  }
  static nr_td_grantwork_t *make_gw()
  {
    nr_td_gw_job_t j{};
    j.abs_slot = 100;
    j.slots_per_frame = 20;
    return nr_td_grantwork_begin(&j);
  }
  nr_pdsch_sweep_ticket_t ticket(uint16_t rnti = 0x4601, uint8_t tda = 0)
  {
    nr_pdsch_sweep_ticket_t t{};
    nr_pdsch_cfg_hypothesis_t h{};
    EXPECT_TRUE(nr_pdsch_config_sweep_select(1, rnti, tda, 2, 0, legal_a, &t, &h));
    return t;
  }
  /* One grant through pre / run / feed. Returns pre's verdict. */
  bool grant(const nr_pdsch_sweep_ticket_t &t, int64_t slot, bool tb_pass = false, bool iq_ok = true, bool tb_fed = true,
             uint8_t tb_dec = 1, int nl = 1)
  {
    nr_td_cb0_job_t j{};
    j.abs_slot = slot;
    j.job_k0 = t.k0;
    j.gw_on = true;
    bool tb_cpu = false;
    const bool on = nr_td_cb0_wire_pre(&t, gw, &j, &tb_cpu);
    if (on)
      nr_td_cb0_wire_run(gw);
    nr_td_cb0_tb_t tb{};
    tb.tb_fed = tb_fed;
    tb.tb_pass = tb_pass;
    tb.tb_decoder = tb_dec;
    tb.iq_ok_after = iq_ok;
    tb.rv = 0;
    tb.nl = nl;
    if (on)
      nr_td_cb0_wire_feed(&t, &tb);
    return on;
  }
  nr_td_cb0_wire_stats_t stats()
  {
    nr_td_cb0_wire_stats_t s;
    nr_td_cb0_wire_stats(&s);
    return s;
  }
  nr_td_grantwork_t *gw = nullptr;
};

/* ======================= pure scheduler ======================= */

TEST(Cb0Sched, HashSubsetUnbiasedAcrossSlotTypes)
{
  /* 600 hypotheses over 60 geometries; m1 = 3, m2 = 4 -> rate 1/12 on every slot. TDD DDDSU (period 5) and a
   * 10-slot pattern: each slot class must see every hypothesis at ~1/12, and no hypothesis may be starved in a class. */
  const int n = 600;
  std::vector<uint64_t> key(n), gkey(n);
  for (int i = 0; i < n; i++) {
    nr_pdsch_cfg_hypothesis_t h{};
    h.tda_start = (uint8_t)(i % 4);
    h.tda_length = (uint8_t)(4 + (i / 4) % 10);
    h.dmrs_mask = (uint16_t)(0x4 | ((i / 40) % 2 ? 0x800 : 0));
    h.mcs_table = (uint8_t)((i / 80) % 3);
    h.dmrs_add_pos = (uint8_t)((i / 240) % 3);
    h.dmrs_max_len = 1;
    key[i] = nr_td_cb0_hyp_key(&h);
    gkey[i] = nr_td_cb0_geo_key(&h);
  }
  const uint64_t seed = nr_td_cb0_ctx_seed(1, 0x4601, 0);
  const uint32_t m1 = 3, m2 = 4;
  const int slots = 120000;
  for (int period : {5, 10}) {
    std::vector<std::vector<int>> hits(period, std::vector<int>(n, 0));
    std::vector<int> per_class(period, 0);
    for (int s = 0; s < slots; s++) {
      per_class[s % period]++;
      for (int i = 0; i < n; i++)
        hits[s % period][i] += nr_td_cb0_subset_hit2(seed, 1000000 + s, gkey[i], m1, key[i], m2);
    }
    for (int c = 0; c < period; c++) {
      const double p = 1.0 / (m1 * m2), N = per_class[c];
      const double sd = std::sqrt(N * p * (1 - p));
      double tot = 0;
      for (int i = 0; i < n; i++) {
        EXPECT_NEAR(hits[c][i], N * p, 6 * sd) << "period " << period << " class " << c << " hyp " << i;
        tot += hits[c][i];
      }
      EXPECT_NEAR(tot / (n * N), p, 0.01 * p * 10) << "class mean";
    }
  }
  /* Discrimination: a slot ROTATION (abs_slot mod m == i mod m) starves hypotheses in a TDD class. */
  const uint32_t m = 5;
  int starved = 0;
  for (int i = 0; i < 25; i++) {
    int on_d = 0;
    for (int s = 0; s < 1000; s++)
      on_d += (s % 5 < 3) && ((uint32_t)(s % m) == (uint32_t)(i % m));
    starved += on_d == 0;
  }
  EXPECT_GT(starved, 0) << "the rotation must alias with DDDSU (test discrimination)";
}

TEST(Cb0Sched, SelectKeepsForcedAndIsOutcomeFree)
{
  std::vector<uint64_t> key(200), gkey(200);
  for (int i = 0; i < 200; i++) {
    key[i] = nr_td_cb0_mix64(i);
    gkey[i] = nr_td_cb0_mix64(i / 10 + 777);
  }
  std::vector<int> a(200), b(200);
  const int na = nr_td_cb0_subset_select(42, 77, gkey.data(), key.data(), 200, 4, 3, 123, a.data(), 200);
  const int nb = nr_td_cb0_subset_select(42, 77, gkey.data(), key.data(), 200, 4, 3, 123, b.data(), 200);
  ASSERT_EQ(na, nb);
  EXPECT_EQ(std::vector<int>(a.begin(), a.begin() + na), std::vector<int>(b.begin(), b.begin() + nb));
  EXPECT_NE(std::find(a.begin(), a.begin() + na, 123), a.begin() + na) << "scheduled hypothesis always in the set";
  for (int k = 1; k < na; k++)
    EXPECT_LT(a[k - 1], a[k]);
  /* the key is the VALUE: permuting the catalogue permutes the set, it does not change which values are in it */
  std::vector<uint64_t> key2(key.rbegin(), key.rend()), gkey2(gkey.rbegin(), gkey.rend());
  std::vector<int> c(200);
  const int nc = nr_td_cb0_subset_select(42, 77, gkey2.data(), key2.data(), 200, 4, 3, 199 - 123, c.data(), 200);
  std::set<uint64_t> va, vc;
  for (int k = 0; k < na; k++) va.insert(key[a[k]]);
  for (int k = 0; k < nc; k++) vc.insert(key2[c[k]]);
  EXPECT_EQ(va, vc);
}

TEST(Cb0Sched, CostEstimateIgnoresPassPattern)
{
  /* Same per-iteration cost, different outcomes (passing items stop early: fewer iterations AND less time) ->
   * the same estimate, so the next grants' subset sizes are the same. */
  nr_td_cb0_sched_t a, b;
  nr_td_cb0_sched_init(&a, 10000, 30, 20, 8);
  nr_td_cb0_sched_init(&b, 10000, 30, 20, 8);
  const double us_it = 40.0;
  for (int g = 0; g < 50; g++) {
    const int n = 16, passes_a = g % 3, passes_b = 7 - (g % 5); /* different pass counts per grant */
    const uint32_t it_a = (uint32_t)(passes_a * 2 + (n - passes_a) * 8), it_b = (uint32_t)(passes_b * 2 + (n - passes_b) * 8);
    const int T = 8;
    nr_td_cb0_sched_account(&a, 0, (uint64_t)(it_a * us_it / T * 1000), T, n, it_a, 0, 0);
    nr_td_cb0_sched_account(&b, 0, (uint64_t)(it_b * us_it / T * 1000), T, n, it_b, 0, 0);
  }
  EXPECT_NEAR(a.us_per_iter, us_it, 0.5);
  EXPECT_NEAR(b.us_per_iter, us_it, 0.5);
  nr_td_cb0_sizes_t za, zb;
  nr_td_cb0_sched_sizes(&a, 700, 120, &za);
  nr_td_cb0_sched_sizes(&b, 700, 120, &zb);
  EXPECT_EQ(za.m1, zb.m1);
  EXPECT_EQ(za.m2, zb.m2);
  EXPECT_EQ(za.b_items, zb.b_items);
}

TEST(Cb0Sched, BudgetSizesAndTokenBucket)
{
  nr_td_cb0_sched_t s;
  nr_td_cb0_sched_init(&s, 10000, 30, 20, 8);
  EXPECT_EQ(nr_td_cb0_sched_B(&s), (int)(10000 / (NR_TD_CB0_US_PER_ITER_INIT * 8)));
  nr_td_cb0_sizes_t z;
  nr_td_cb0_sched_sizes(&s, 700, 100, &z);
  EXPECT_EQ(z.g_target, 1); /* floor(0.4 * 10000 / (2 * 6000)) = 0 -> 1 */
  EXPECT_EQ(z.b_items, 1);  /* 10000 - 12000 < 0: one item */
  EXPECT_EQ(z.m1, 100u);
  EXPECT_EQ(z.m2, 7u); /* ceil(700 / 100) = 7 items per geometry, 1 affordable */
  nr_td_cb0_sched_t dflt;
  nr_td_cb0_sched_init(&dflt, 0, 30, 20, 8); /* default budget: sized for 4 RX */
  EXPECT_EQ(dflt.budget_us, NR_TD_CB0_BUDGET_US_DEFAULT);
  nr_td_cb0_sched_sizes(&dflt, 700, 100, &z);
  EXPECT_EQ(z.g_target, 1);
  EXPECT_EQ(z.b_items, (int)((NR_TD_CB0_BUDGET_US_DEFAULT - 2 * NR_TD_CB0_SIG_US_INIT) / (NR_TD_CB0_US_PER_ITER_INIT * 8)));
  EXPECT_EQ(z.m2, 1u);
  nr_td_cb0_sched_t big;
  nr_td_cb0_sched_init(&big, 160000, 30, 20, 8);
  nr_td_cb0_sched_sizes(&big, 700, 100, &z);
  EXPECT_EQ(z.g_target, 5);
  EXPECT_EQ(z.m1, 20u);
  EXPECT_EQ(z.m2, (uint32_t)((35 + z.b_items - 1) / z.b_items));
  /* bucket: capacity 2 x budget; a grant that does not fit is refused WHOLE and takes no tokens */
  double planned = -1;
  const double before = s.tokens_us;
  EXPECT_FALSE(nr_td_cb0_sched_admit(&s, 1000, 1000, 0, &planned));
  EXPECT_EQ(planned, 0.0);
  EXPECT_EQ(s.tokens_us, before);
  EXPECT_TRUE(nr_td_cb0_sched_admit(&s, 1000, 10, 1, &planned));
  EXPECT_NEAR(planned, 10 * 360.0 + NR_TD_CB0_SIG_US_INIT, 1e-6);
  /* refill at cpu_pct * ncpu: 30 % of 20 CPUs = 6 CPU-us per us */
  nr_td_cb0_sched_account(&s, planned, 0, 0, 0, 0, 0, 0); /* nothing ran: full refund */
  EXPECT_NEAR(s.tokens_us, before, 1e-6);
  s.tokens_us = 0;
  EXPECT_FALSE(nr_td_cb0_sched_admit(&s, 2000, 1, 0, &planned)); /* 1 us later: 6 tokens < 360 */
  EXPECT_TRUE(nr_td_cb0_sched_admit(&s, 2000 + 61000, 1, 0, &planned)); /* +61 us: 6 + 366 */
}

TEST(Cb0Sched, AdmissibilityTable)
{
  nr_td_cb0_adm_in_t ok{};
  ok.tb_path_fed = true;
  ok.iq_ok_after = true;
  ok.nl = 1;
  ok.rank_max = 4;
  ok.cb0_decoder = 1;
  ok.tb_decoder = 1;
  EXPECT_EQ(nr_td_cb0_admissibility(&ok), 0u);
  auto bit = [](int r) { return 1u << r; };
  struct Case {
    const char *name;
    void (*mut)(nr_td_cb0_adm_in_t *);
    uint32_t want;
  };
  const Case cases[] = {
      {"harq", [](nr_td_cb0_adm_in_t *x) { x->gw_flags = NR_TD_GW_F_HARQ; }, bit(NR_TD_CB0_R_NOT_NEW_RV0)},
      {"rv", [](nr_td_cb0_adm_in_t *x) { x->rv = 2; }, bit(NR_TD_CB0_R_NOT_NEW_RV0)},
      {"gate", [](nr_td_cb0_adm_in_t *x) { x->tb_path_fed = false; }, bit(NR_TD_CB0_R_GATED)},
      {"iq", [](nr_td_cb0_adm_in_t *x) { x->iq_ok_after = false; }, bit(NR_TD_CB0_R_IQ_STALE)},
      {"lbrm", [](nr_td_cb0_adm_in_t *x) { x->gw_flags = NR_TD_GW_F_LBRM; }, bit(NR_TD_CB0_R_LBRM)},
      {"rvretry", [](nr_td_cb0_adm_in_t *x) { x->gw_flags = NR_TD_GW_F_RV_RETRY; }, bit(NR_TD_CB0_R_RV_RETRY)},
      {"arm", [](nr_td_cb0_adm_in_t *x) { x->gw_flags = NR_TD_GW_F_ARM; }, bit(NR_TD_CB0_R_PRG_PTRS)},
      {"stale", [](nr_td_cb0_adm_in_t *x) { x->gw_flags = NR_TD_GW_F_STALE; }, bit(NR_TD_CB0_R_MEMBER_STALE)},
      {"full", [](nr_td_cb0_adm_in_t *x) { x->gw_flags = NR_TD_GW_F_FULL; }, bit(NR_TD_CB0_R_MEMBER_STALE)},
      {"member", [](nr_td_cb0_adm_in_t *x) { x->member_error = true; }, bit(NR_TD_CB0_R_MEMBER_STALE)},
      {"scale", [](nr_td_cb0_adm_in_t *x) { x->llr_scale = true; }, bit(NR_TD_CB0_R_LLR_SCALE)},
      {"gpullr", [](nr_td_cb0_adm_in_t *x) { x->gpu_llr = true; }, bit(NR_TD_CB0_R_GPU_LLR)},
      {"ldpc", [](nr_td_cb0_adm_in_t *x) { x->backend_failed = true; }, bit(NR_TD_CB0_R_LDPC_ERROR)},
      {"rank", [](nr_td_cb0_adm_in_t *x) { x->nl = 4; x->rank_max = 1; }, bit(NR_TD_CB0_R_RANK)},
      {"cpu_cb0_cuda_tb", [](nr_td_cb0_adm_in_t *x) { x->tb_decoder = 2; }, bit(NR_TD_CB0_R_DECODER)},
      {"unknown_cb0", [](nr_td_cb0_adm_in_t *x) { x->cb0_decoder = 0; }, bit(NR_TD_CB0_R_DECODER)},
      {"unknown_tb", [](nr_td_cb0_adm_in_t *x) { x->tb_decoder = 0; }, bit(NR_TD_CB0_R_DECODER)},
      {"contract", [](nr_td_cb0_adm_in_t *x) { x->contract = true; }, bit(NR_TD_CB0_R_CONTRACT)},
      {"cuda_cb0_cpu_tb_ok", [](nr_td_cb0_adm_in_t *x) { x->cb0_decoder = 2; }, 0u},
      {"rank4_ok", [](nr_td_cb0_adm_in_t *x) { x->nl = 4; }, 0u},
  };
  for (const Case &c : cases) {
    nr_td_cb0_adm_in_t x = ok;
    c.mut(&x);
    EXPECT_EQ(nr_td_cb0_admissibility(&x), c.want) << c.name;
  }
  EXPECT_STREQ(nr_td_cb0_reason_name[NR_TD_CB0_R_BUDGET], "budget");
  EXPECT_EQ(NR_TD_CB0_R_ENGINE_MASK, (1u << 13) - 1);
}

/* ======================= backends ======================= */

struct FakeGpu {
  int calls = 0, fail_on = -1;
  bool healthy = true, mix = false;
};
FakeGpu g_fake;
int fake_healthy(void *) { return g_fake.healthy ? 1 : 0; }
int fake_decode(void *, const nr_td_cb0_item_t *it, int n, nr_td_cb0_result_t *out)
{
  const int call = g_fake.calls++;
  nr_td_cb0_batch_cpu(it, n, out); /* same verdicts, relabelled as the CUDA decoder */
  for (int i = 0; i < n; i++)
    out[i].decoder_used = (g_fake.mix && i == 0) ? NR_TD_CB0_DEC_CPU_LAYERED : NR_TD_CB0_DEC_CUDA_FLOODING;
  return call == g_fake.fail_on ? -5 : 0;
}

TEST_F(Cb0Wire, BackendFallbackOnInjectedGpuFailure)
{
  std::vector<nr_td_cb0_item_t> it = {g_tb->pass_item(), g_tb->fail_item(), g_tb->pass_item()};
  std::vector<nr_td_cb0_result_t> out(3);
  nr_td_cb0_exec_t ex;
  /* no GPU registered: CPU */
  ASSERT_EQ(nr_td_cb0_exec(it.data(), 3, out.data(), &ex), 3);
  EXPECT_EQ(ex.backend, NR_TD_CB0_BE_CPU);
  EXPECT_EQ(ex.decoder, NR_TD_CB0_DEC_CPU_LAYERED);
  EXPECT_EQ(out[0].pass, 1);
  EXPECT_EQ(out[1].pass, 0);
  for (auto &r : out)
    EXPECT_EQ(r.decoder_used, NR_TD_CB0_DEC_CPU_LAYERED);
  g_fake = FakeGpu{};
  g_fake.fail_on = 1;
  nr_td_cb0_backend_t be{"fake", fake_healthy, fake_decode, nullptr};
  nr_td_cb0_register_gpu_backend(&be);
  /* auto + healthy GPU: GPU */
  ASSERT_EQ(nr_td_cb0_exec(it.data(), 3, out.data(), &ex), 3);
  EXPECT_EQ(ex.backend, NR_TD_CB0_BE_GPU);
  EXPECT_EQ(ex.decoder, NR_TD_CB0_DEC_CUDA_FLOODING);
  EXPECT_FALSE(ex.failed);
  /* injected GPU failure: the WHOLE batch is void, no partial verdict */
  EXPECT_EQ(nr_td_cb0_exec(it.data(), 3, out.data(), &ex), 0);
  EXPECT_EQ(ex.backend, NR_TD_CB0_BE_GPU);
  EXPECT_TRUE(ex.failed);
  for (auto &r : out) {
    EXPECT_EQ(r.pass, -1);
    EXPECT_EQ(r.err, NR_TD_CB0_ERR_GPU);
  }
  /* the NEXT batches fall back to the CPU (back-off 2), then the GPU again */
  for (int k = 0; k < 2; k++) {
    ASSERT_EQ(nr_td_cb0_exec(it.data(), 3, out.data(), &ex), 3);
    EXPECT_EQ(ex.backend, NR_TD_CB0_BE_CPU) << k;
    EXPECT_EQ(ex.decoder, NR_TD_CB0_DEC_CPU_LAYERED);
    EXPECT_EQ(out[0].pass, 1);
  }
  ASSERT_EQ(nr_td_cb0_exec(it.data(), 3, out.data(), &ex), 3);
  EXPECT_EQ(ex.backend, NR_TD_CB0_BE_GPU);
  /* mixed decoders inside a GPU batch (pool CPU fallback for one item) = failure */
  g_fake.mix = true;
  EXPECT_EQ(nr_td_cb0_exec(it.data(), 3, out.data(), &ex), 0);
  EXPECT_TRUE(ex.failed);
  EXPECT_TRUE(ex.mixed);
  g_fake.mix = false;
  nr_td_cb0_backend_mode_set(NR_TD_CB0_BE_AUTO); /* clears the back-off */
  /* unhealthy GPU: CPU */
  g_fake.healthy = false;
  ASSERT_EQ(nr_td_cb0_exec(it.data(), 3, out.data(), &ex), 3);
  EXPECT_EQ(ex.backend, NR_TD_CB0_BE_CPU);
  g_fake.healthy = true;
  /* cpu mode: never the GPU */
  nr_td_cb0_backend_mode_set(NR_TD_CB0_BE_CPU);
  const int calls = g_fake.calls;
  ASSERT_EQ(nr_td_cb0_exec(it.data(), 3, out.data(), &ex), 3);
  EXPECT_EQ(ex.backend, NR_TD_CB0_BE_CPU);
  EXPECT_EQ(g_fake.calls, calls);
  nr_td_cb0_backend_stats_t bs;
  nr_td_cb0_backend_get_stats(&bs);
  EXPECT_GE(bs.gpu_failed, 2u);
  nr_td_cb0_register_gpu_backend(nullptr);
}

TEST_F(Cb0Wire, GpuFailureMakesTheGrantInadmissibleAndNextGrantRunsOnCpu)
{
  g_fake = FakeGpu{};
  g_fake.fail_on = 0;
  nr_td_cb0_backend_t be{"fake", fake_healthy, fake_decode, nullptr};
  nr_td_cb0_register_gpu_backend(&be);
  const auto t = ticket();
  ASSERT_TRUE(grant(t, 1000));
  auto s = stats();
  EXPECT_EQ(s.backend_gpu, 1u);
  EXPECT_EQ(s.inadmissible[NR_TD_CB0_R_LDPC_ERROR], 1u);
  EXPECT_EQ(s.admissible, 0u);
  EXPECT_EQ(s.items, 0u) << "a failed GPU batch credits no item";
  ASSERT_TRUE(grant(t, 1001));
  s = stats();
  EXPECT_EQ(s.backend_cpu, 1u) << "the next grant falls back to the CPU";
  EXPECT_EQ(s.admissible, 1u);
  nr_td_cb0_register_gpu_backend(nullptr);
}

/* ======================= per-grant glue ======================= */

TEST_F(Cb0Wire, FlagOffDoesNothing)
{
  nr_pdsch_config_sweep_cb0_elim_env_set(0);
  const auto t = ticket();
  nr_pdsch_config_sweep_state_t *a = (nr_pdsch_config_sweep_state_t *)malloc(sizeof(*a));
  nr_pdsch_config_sweep_state_t *b = (nr_pdsch_config_sweep_state_t *)malloc(sizeof(*b));
  ASSERT_TRUE(nr_pdsch_config_sweep_snapshot(&t, a));
  nr_td_cb0_job_t j{};
  j.abs_slot = 5;
  j.gw_on = true;
  bool tb_cpu = true;
  EXPECT_FALSE(nr_td_cb0_wire_pre(&t, gw, &j, &tb_cpu));
  EXPECT_FALSE(tb_cpu) << "flag off: the TB decoder is never changed";
  nr_td_cb0_wire_run(gw);
  nr_td_cb0_tb_t tb{};
  tb.tb_fed = true;
  nr_td_cb0_wire_feed(&t, &tb);
  EXPECT_EQ(g_stub.calls, 0);
  EXPECT_STREQ(nr_td_cb0_wire_converged_suffix(&t), "");
  ASSERT_TRUE(nr_pdsch_config_sweep_snapshot(&t, b));
  EXPECT_EQ(memcmp(a, b, sizeof(*a)), 0) << "engine state untouched";
  const auto s = stats();
  EXPECT_EQ(s.grants + s.batches + s.items + s.budget_skips, 0u);
  free(a);
  free(b);
}

TEST_F(Cb0Wire, SetIsFixedBeforeOutcomes)
{
  /* Two runs over the same grants with OPPOSITE CB0 outcome patterns: the requested set of every grant is identical
   * (estimates frozen; the engine is not wired on this base, so no outcome reaches the active set either). */
  std::vector<std::vector<uint64_t>> run[2];
  size_t passes[2] = {0, 0};
  for (int r = 0; r < 2; r++) {
    TearDown();
    SetUp(); /* fresh engine, wire and stub */
    g_stub.pattern = r;
    const auto tt = ticket();
    for (int g = 0; g < 40; g++) {
      g_stub.requested.clear();
      ASSERT_TRUE(grant(tt, 5000 + g));
      run[r].push_back(g_stub.requested);
      for (uint64_t k : g_stub.requested)
        passes[r] += (int)(k & 1) == r;
    }
  }
  EXPECT_EQ(run[0], run[1]);
  size_t tot = 0;
  for (auto &v : run[0])
    tot += v.size();
  EXPECT_GT(tot, 40u);
  EXPECT_NE(passes[0], passes[1]) << "the two runs really saw different outcomes";
}

TEST_F(Cb0Wire, ScheduledHypothesisAlwaysTestedAndPremiseAlarm)
{
  const auto t = ticket();
  nr_pdsch_config_sweep_state_t *st = (nr_pdsch_config_sweep_state_t *)malloc(sizeof(*st));
  ASSERT_TRUE(nr_pdsch_config_sweep_snapshot(&t, st));
  const uint64_t sk = nr_td_cb0_hyp_key(&st->hyp[t.hypothesis]);
  free(st);
  /* scheduled hypothesis: CB0 PASS, TB PASS -> no alarm */
  g_stub.pass[sk] = true;
  ASSERT_TRUE(grant(t, 7000, /*tb_pass=*/true));
  EXPECT_NE(std::find(g_stub.requested.begin(), g_stub.requested.end(), sk), g_stub.requested.end());
  EXPECT_EQ(stats().premise_alarms, 0u);
  /* CB0 FAIL with TB PASS on an admissible grant -> alarm */
  g_stub.pass[sk] = false;
  g_stub.requested.clear();
  ASSERT_TRUE(grant(t, 7001, true));
  EXPECT_EQ(stats().premise_alarms, 1u);
  EXPECT_NE(std::string(nr_td_cb0_wire_converged_suffix(&t)).find("cb0_alarms=1"), std::string::npos);
  /* same, but inadmissible (IQ stale after decode) -> no alarm */
  ASSERT_TRUE(grant(t, 7002, true, /*iq_ok=*/false));
  EXPECT_EQ(stats().premise_alarms, 1u);
  /* CPU CB0 with a CUDA TB: decoder dominance fails -> inadmissible, no alarm */
  ASSERT_TRUE(grant(t, 7003, true, true, true, /*tb_dec=*/2));
  EXPECT_EQ(stats().premise_alarms, 1u);
  EXPECT_EQ(stats().inadmissible[NR_TD_CB0_R_DECODER], 1u);
}

TEST_F(Cb0Wire, AdmissibilityGatingAtRuntime)
{
  const auto t = ticket();
  ASSERT_TRUE(grant(t, 100));
  EXPECT_EQ(stats().admissible, 1u);
  g_stub.flags = NR_TD_GW_F_HARQ;
  ASSERT_TRUE(grant(t, 101));
  g_stub.flags = NR_TD_GW_F_ARM;
  ASSERT_TRUE(grant(t, 102));
  g_stub.flags = 0;
  ASSERT_TRUE(grant(t, 103, false, false)); /* IQ re-check failed */
  ASSERT_TRUE(grant(t, 104, false, true, false)); /* TB path did not feed */
  g_stub.rc_for_all = NR_TD_GW_E_STALE;
  ASSERT_TRUE(grant(t, 105));
  g_stub.rc_for_all = NR_TD_GW_OK;
  const auto s = stats();
  EXPECT_EQ(s.admissible, 1u);
  EXPECT_EQ(s.inadmissible[NR_TD_CB0_R_NOT_NEW_RV0], 1u);
  EXPECT_EQ(s.inadmissible[NR_TD_CB0_R_PRG_PTRS], 1u);
  EXPECT_EQ(s.inadmissible[NR_TD_CB0_R_IQ_STALE], 1u);
  EXPECT_EQ(s.inadmissible[NR_TD_CB0_R_GATED], 1u);
  EXPECT_EQ(s.inadmissible[NR_TD_CB0_R_MEMBER_STALE], 1u);
  /* no GrantWork: refused before decoding */
  nr_td_cb0_job_t j{};
  j.abs_slot = 200;
  j.gw_on = false;
  bool tb_cpu = false;
  g_stub.calls = 0;
  EXPECT_TRUE(nr_td_cb0_wire_pre(&t, nullptr, &j, &tb_cpu));
  nr_td_cb0_wire_run(nullptr);
  nr_td_cb0_tb_t tb{};
  tb.tb_fed = true;
  tb.tb_decoder = 1;
  tb.iq_ok_after = true;
  tb.nl = 1;
  nr_td_cb0_wire_feed(&t, &tb);
  EXPECT_EQ(g_stub.calls, 0);
  EXPECT_EQ(stats().inadmissible[NR_TD_CB0_R_NO_GRANTWORK], 1u);
  /* GPU-fed LLRs: refused before decoding */
  j.gw_on = true;
  j.gpu_job = true;
  EXPECT_TRUE(nr_td_cb0_wire_pre(&t, nullptr, &j, &tb_cpu));
  nr_td_cb0_wire_run(nullptr);
  nr_td_cb0_wire_feed(&t, &tb);
  EXPECT_EQ(g_stub.calls, 0);
  EXPECT_EQ(stats().inadmissible[NR_TD_CB0_R_GPU_LLR], 1u);
  /* layout probe / settled / no ticket: not an acquiring grant at all */
  j.gpu_job = false;
  j.layout_probe = true;
  EXPECT_FALSE(nr_td_cb0_wire_pre(&t, gw, &j, &tb_cpu));
  nr_pdsch_sweep_ticket_t none{};
  j.layout_probe = false;
  EXPECT_FALSE(nr_td_cb0_wire_pre(&none, gw, &j, &tb_cpu));
}

TEST_F(Cb0Wire, BudgetSkipIsWholeGrant)
{
  setenv("ISAC_TD_CB0_CPU_PCT", "0.0001", 1); /* the bucket barely refills */
  nr_td_cb0_wire_reset();
  nr_td_cb0_wire_test_unlimited(false);
  const auto t = ticket();
  int ran = 0, skipped = 0;
  for (int g = 0; g < 12; g++) {
    nr_td_cb0_wire_test_set_tokens((g % 3 == 0) ? 1e12 : 0.0); /* every third grant affordable */
    g_stub.calls = 0;
    ASSERT_TRUE(grant(t, 9000 + g));
    const bool skip = (nr_td_cb0_wire_test_last_reasons() >> NR_TD_CB0_R_BUDGET) & 1;
    int idx[NR_TD_CB0_PLAN_MAX];
    const int nset = nr_td_cb0_wire_test_last_set(idx, NR_TD_CB0_PLAN_MAX);
    ASSERT_GT(nset, 0);
    if (skip) {
      skipped++;
      EXPECT_EQ(g_stub.calls, 0) << "a budget-skipped grant decodes NOTHING (never partially)";
    } else {
      ran++;
      EXPECT_EQ(g_stub.calls, nset) << "an admitted grant decodes its whole set";
      EXPECT_EQ(g % 3, 0) << "no tokens: never admitted";
    }
  }
  EXPECT_GE(ran, 2);
  EXPECT_GE(skipped, 8);
  const auto s = stats();
  EXPECT_EQ(s.budget_skips, (uint64_t)skipped);
  EXPECT_EQ(s.inadmissible[NR_TD_CB0_R_BUDGET], (uint64_t)skipped);
  unsetenv("ISAC_TD_CB0_CPU_PCT");
}

TEST_F(Cb0Wire, ReindexedContextCreditsNothing)
{
  const auto t = ticket();
  nr_td_cb0_job_t j{};
  j.abs_slot = 300;
  j.job_k0 = t.k0;
  j.gw_on = true;
  bool tb_cpu = false;
  ASSERT_TRUE(nr_td_cb0_wire_pre(&t, gw, &j, &tb_cpu));
  EXPECT_TRUE(tb_cpu) << "acquiring context with the channel on: CPU TB decoder by default";
  nr_td_cb0_wire_run(gw);
  /* the context's catalogue is pruned between the set and the feed (a k0 certification keeps one k0 layer) */
  nr_pdsch_config_sweep_state_t *st = (nr_pdsch_config_sweep_state_t *)malloc(sizeof(*st));
  ASSERT_TRUE(nr_pdsch_config_sweep_snapshot(&t, st));
  const int n0 = st->n_hyp;
  const int n1 = nr_pdsch_config_sweep_certify_k0(&t, 1ull << t.k0);
  free(st);
  ASSERT_GT(n1, 0);
  ASSERT_LT(n1, n0) << "the certification must re-index the catalogue";
  nr_td_cb0_tb_t tb{};
  tb.tb_fed = true;
  tb.tb_decoder = 1;
  tb.iq_ok_after = true;
  tb.nl = 1;
  nr_td_cb0_wire_feed(&t, &tb);
  const auto s = stats();
  EXPECT_EQ(s.inadmissible[NR_TD_CB0_R_REINDEXED], 1u);
  EXPECT_EQ(s.admissible, 0u);
}
}  // namespace

namespace {
/* End to end through the merged engine (ELIM fix round 1): a truth whose TB and CB0 pass, every other hypothesis
 * failing both. The engine must eliminate wrong hypotheses through the CB0 channel, never the truth, with 0 premise
 * alarms; the CONVERGED suffix carries the counts. */
TEST_F(Cb0Wire, EngineEliminatesWrongNeverTruth)
{
  ASSERT_TRUE(nr_td_cb0a_engine_wired());
  auto t0 = ticket();
  nr_pdsch_config_sweep_state_t *st = (nr_pdsch_config_sweep_state_t *)malloc(sizeof(*st));
  ASSERT_TRUE(nr_pdsch_config_sweep_snapshot(&t0, st));
  ASSERT_TRUE(st->cb0_elim) << "new runtime contexts take ISAC_TD_CB0_ELIM";
  const nr_pdsch_cfg_hypothesis_t truth = st->hyp[t0.hypothesis];
  const uint64_t tk = nr_td_cb0_hyp_key(&truth);
  g_stub.pass[tk] = true;
  int grants = 0;
  bool converged = false;
  for (int g = 0; g < 20000 && !converged; g++) {
    nr_pdsch_sweep_ticket_t t{};
    nr_pdsch_cfg_hypothesis_t h{};
    ASSERT_TRUE(nr_pdsch_config_sweep_select(1, 0x4601, 0, 2, 0, legal_a, &t, &h));
    if (t.settled)
      break;
    const bool tb = nr_td_cb0_hyp_key(&h) == tk;
    nr_td_cb0_job_t j{};
    j.abs_slot = 20000 + g;
    j.job_k0 = t.k0;
    j.gw_on = true;
    bool tb_cpu = false;
    const bool on = nr_td_cb0_wire_pre(&t, gw, &j, &tb_cpu);
    ASSERT_TRUE(on);
    nr_td_cb0_wire_run(gw);
    nr_pdsch_cfg_hypothesis_t w{};
    converged = nr_pdsch_config_sweep_feedback(&t, tb, &w);
    nr_td_cb0_tb_t r{};
    r.tb_fed = true;
    r.tb_pass = tb;
    r.tb_decoder = 1;
    r.iq_ok_after = true;
    r.nl = 1;
    nr_td_cb0_wire_feed(&t, &r);
    grants++;
    if (converged)
      EXPECT_EQ(nr_td_cb0_hyp_key(&w), tk) << "wrong winner";
  }
  const auto s = stats();
  EXPECT_EQ(s.premise_alarms, 0u);
  EXPECT_GT(s.eliminations, 0u) << "after " << grants << " grants";
  EXPECT_GT(s.admissible, 0u);
  ASSERT_TRUE(nr_pdsch_config_sweep_snapshot(&t0, st));
  for (int i = 0; i < st->n_hyp; i++)
    if (nr_td_cb0_hyp_key(&st->hyp[i]) == tk)
      EXPECT_FALSE(nr_pdsch_config_sweep_is_eliminated(st, i)) << "truth eliminated";
  EXPECT_NE(std::string(nr_td_cb0_wire_converged_suffix(&t0)).find("cb0_elim="), std::string::npos);
  free(st);
}

/* A CUDA full-TB outcome noted WITHOUT a batch (budget skip) must make later CPU CB0 batches inadmissible in the engine
 * (dominance), and the engine counts the rejection. */
TEST_F(Cb0Wire, TbDecoderNotedWithoutBatchBlocksCpuBatches)
{
  nr_pdsch_config_sweep_cb0_stats_reset();
  nr_td_cb0_wire_test_unlimited(false);
  const auto t = ticket();
  nr_td_cb0_wire_test_set_tokens(0.0);
  setenv("ISAC_TD_CB0_CPU_PCT", "0.0001", 1);
  ASSERT_TRUE(grant(t, 30000, false, true, true, /*tb_dec=*/2)); /* skipped: only the TB decoder note reaches the engine */
  EXPECT_EQ(stats().budget_skips, 1u);
  nr_td_cb0_wire_test_unlimited(true);
  ASSERT_TRUE(grant(t, 30001, false, true, true, /*tb_dec=*/1)); /* CPU TB, CPU batch: locally admissible */
  uint64_t alarms = 0, rej[NR_TD_CB0_X_COUNT] = {0};
  nr_pdsch_config_sweep_cb0_stats(&alarms, rej);
  EXPECT_EQ(rej[11], 1u) << "engine DECODER rejection: the epoch saw a CUDA TB";
  unsetenv("ISAC_TD_CB0_CPU_PCT");
}
}  // namespace

namespace {
/* Round 2: the CPU backend's persistent pool. Concurrent callers (PDSCH consumers) get exactly the sequential verdicts,
 * and the pool does not grow per batch. */
TEST_F(Cb0Wire, PersistentPoolConcurrentCallersMatchSequential)
{
  nr_td_cb0_set_threads(8);
  std::vector<nr_td_cb0_item_t> it;
  for (int i = 0; i < 24; i++)
    it.push_back((i % 3 == 0) ? g_tb->pass_item() : g_tb->fail_item());
  std::vector<nr_td_cb0_result_t> ref(it.size());
  ASSERT_EQ(nr_td_cb0_batch_cpu(it.data(), (int)it.size(), ref.data()), (int)it.size());
  const int pool0 = nr_td_cb0_pool_threads();
  EXPECT_EQ(pool0, 7) << "threads - 1 workers, the caller is the 8th";
  std::atomic<int> bad{0};
  std::vector<std::thread> th;
  for (int c = 0; c < 4; c++)
    th.emplace_back([&]() {
      for (int r = 0; r < 50; r++) {
        std::vector<nr_td_cb0_result_t> out(it.size());
        if (nr_td_cb0_batch_cpu(it.data(), (int)it.size(), out.data()) != (int)it.size())
          bad++;
        for (size_t k = 0; k < it.size(); k++)
          if (out[k].pass != ref[k].pass || out[k].iters != ref[k].iters || out[k].decoder_used != NR_TD_CB0_DEC_CPU_LAYERED)
            bad++;
      }
    });
  for (auto &t : th)
    t.join();
  EXPECT_EQ(bad.load(), 0);
  EXPECT_EQ(nr_td_cb0_pool_threads(), pool0) << "no thread creation per batch";
  nr_td_cb0_set_threads(1);
}
}  // namespace
