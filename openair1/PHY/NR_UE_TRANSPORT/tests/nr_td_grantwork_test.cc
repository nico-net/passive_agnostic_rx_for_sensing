/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* GrantWork-lite unit tests: refcount/lifetime, lazy compute exactly once per signature, concurrent
 * readers, failure stickiness, borrowed-FEP generation, key canonicalisation, the unified allocator,
 * and the CB0 extraction fixture (CB0 input == what the LDPC segment decoder builds for r = 0). */
#include <gtest/gtest.h>
#include <atomic>
#include <cstring>
#include <random>
#include <thread>
#include <vector>
#include <unistd.h>
#include "common/config/config_userapi.h"
#include "common/utils/LOG/log.h"
extern "C" {
#include "nr_td_grantwork.h"
#include "nr_rate_matching.h"
#include "nr_llr_norm.h"
}
/* Link stubs for the LOG/config layer pulled in by the real nr_rate_matching.c. */
configmodule_interface_t *uniqCfg = nullptr;
extern "C" void exit_function(const char *, const char *, const int, const char *, const int) { abort(); }

namespace {

struct Ctx {
  std::atomic<int> *calls;
  std::atomic<int> *inflight;
  std::atomic<int> *max_inflight;
  int sleep_us;
  bool fail;
  bool via_acquire; /* mimic the decoder: acquire(OWNER) then publish */
  int tag;
};

int fill_compute(void *vctx, nr_td_grantwork_t *gw, uint64_t sig, const void *hint)
{
  (void)hint;
  const Ctx *c = (const Ctx *)vctx;
  c->calls->fetch_add(1);
  const int now = c->inflight->fetch_add(1) + 1;
  int m = c->max_inflight->load();
  while (now > m && !c->max_inflight->compare_exchange_weak(m, now)) {}
  if (c->sleep_us)
    usleep(c->sleep_us);
  int rc = 0;
  if (c->via_acquire) {
    nr_td_gw_llr_view_t v;
    const nr_td_gw_acq_t a = nr_td_grantwork_acquire(gw, sig, &v);
    EXPECT_EQ(a, NR_TD_GW_ACQ_OWNER); /* re-entrant: the claiming thread is the owner */
  }
  /* the computation's critical section ends at publish/abandon (they release the compute lock) */
  c->inflight->fetch_sub(1);
  if (c->fail) {
    rc = -42;
    if (c->via_acquire)
      nr_td_grantwork_abandon(gw, sig, rc);
  } else {
    std::vector<int16_t> llr(1000);
    for (int i = 0; i < 1000; i++)
      llr[i] = (int16_t)(sig * 7 + i + c->tag);
    const uint32_t meta = 0xC0FFEE ^ (uint32_t)sig;
    EXPECT_EQ(nr_td_grantwork_publish(gw, sig, llr.data(), 1000, 1, 2, &meta, sizeof(meta)), NR_TD_GW_OK);
  }
  return rc;
}

struct Fixture {
  std::atomic<int> calls{0}, inflight{0}, max_inflight{0};
  Ctx ctx{&calls, &inflight, &max_inflight, 0, false, false, 0};
  uint64_t gen = 5;
  int16_t fep[64];
  nr_td_grantwork_t *begin(long abs_slot = 100)
  {
    nr_td_gw_job_t j = {};
    j.abs_slot = abs_slot;
    j.slots_per_frame = 20;
    j.fep = fep;
    j.fep_gen_src = &gen;
    j.compute = fill_compute;
    j.ctx = &ctx;
    j.ctx_len = sizeof(ctx);
    return nr_td_grantwork_begin(&j);
  }
};

} // namespace

TEST(GrantWork, RefcountAndLifetime)
{
  Fixture f;
  nr_td_grantwork_t *gw = f.begin();
  ASSERT_NE(gw, nullptr);
  EXPECT_EQ(nr_td_grantwork_refcount(gw), 1);
  EXPECT_EQ(nr_td_grantwork_retain(gw), gw);
  EXPECT_EQ(nr_td_grantwork_refcount(gw), 2);
  EXPECT_EQ(nr_td_grantwork_abs_slot(gw), 100);
  EXPECT_EQ(nr_td_grantwork_slots_per_frame(gw), 20);
  EXPECT_EQ(nr_td_grantwork_fep(gw), (void *)f.fep);
  nr_td_gw_llr_view_t v;
  ASSERT_EQ(nr_td_grantwork_get_llr(gw, 11, nullptr, &v), NR_TD_GW_OK);
  nr_td_grantwork_release(gw); /* a reader still holds it: the entry buffer must survive */
  EXPECT_EQ(nr_td_grantwork_refcount(gw), 1);
  EXPECT_EQ(v.G, 1000u);
  EXPECT_EQ(v.llr[999], (int16_t)(11 * 7 + 999));
  uint64_t fresh0, reused0, uni0;
  nr_td_gw_alloc_stats(&fresh0, &reused0, &uni0);
  nr_td_grantwork_release(gw); /* last reference: buffers go back to the pool */
  Fixture f2;
  nr_td_grantwork_t *gw2 = f2.begin();
  ASSERT_EQ(nr_td_grantwork_get_llr(gw2, 11, nullptr, &v), NR_TD_GW_OK);
  uint64_t fresh1, reused1, uni1;
  nr_td_gw_alloc_stats(&fresh1, &reused1, &uni1);
  EXPECT_GT(reused1, reused0); /* the released entry buffer was recycled, not re-allocated */
  nr_td_grantwork_release(gw2);
  nr_td_grantwork_release(nullptr); /* NULL-safe */
}

TEST(GrantWork, ContextIsCopied)
{
  Fixture f;
  f.ctx.tag = 3;
  nr_td_grantwork_t *gw = f.begin();
  f.ctx.tag = 99; /* the gw keeps its own copy */
  nr_td_gw_llr_view_t v;
  ASSERT_EQ(nr_td_grantwork_get_llr(gw, 1, nullptr, &v), NR_TD_GW_OK);
  EXPECT_EQ(v.llr[0], (int16_t)(1 * 7 + 0 + 3));
  EXPECT_EQ(((const Ctx *)nr_td_grantwork_ctx(gw))->tag, 3);
  nr_td_grantwork_release(gw);
}

TEST(GrantWork, LazyComputeExactlyOncePerSignature)
{
  for (int via = 0; via < 2; via++) {
    Fixture f;
    f.ctx.via_acquire = via;
    nr_td_grantwork_t *gw = f.begin();
    nr_td_gw_llr_view_t a, b, c;
    ASSERT_EQ(nr_td_grantwork_get_llr(gw, 7, nullptr, &a), NR_TD_GW_OK);
    ASSERT_EQ(nr_td_grantwork_get_llr(gw, 7, nullptr, &b), NR_TD_GW_OK);
    EXPECT_EQ(f.calls.load(), 1);
    EXPECT_EQ(a.llr, b.llr); /* the same immutable buffer */
    EXPECT_EQ(nr_td_grantwork_compute_count(gw, 7), 1);
    ASSERT_EQ(nr_td_grantwork_get_llr(gw, 8, nullptr, &c), NR_TD_GW_OK);
    EXPECT_EQ(f.calls.load(), 2);
    EXPECT_NE(c.llr, a.llr);
    EXPECT_EQ(nr_td_grantwork_n_entries(gw), 2);
    EXPECT_EQ(a.meta_len, 4u);
    EXPECT_EQ(*(const uint32_t *)a.meta, 0xC0FFEEu ^ 7u);
    /* producer view: READY without compute */
    nr_td_gw_llr_view_t d;
    EXPECT_EQ(nr_td_grantwork_acquire(gw, 7, &d), NR_TD_GW_ACQ_READY);
    EXPECT_EQ(d.llr, a.llr);
    EXPECT_EQ(f.calls.load(), 2);
    nr_td_grantwork_release(gw);
  }
}

TEST(GrantWork, FailureIsStickyAndNotRecomputed)
{
  for (int via = 0; via < 2; via++) {
    Fixture f;
    f.ctx.fail = true;
    f.ctx.via_acquire = via;
    nr_td_grantwork_t *gw = f.begin();
    nr_td_gw_llr_view_t v;
    EXPECT_EQ(nr_td_grantwork_get_llr(gw, 5, nullptr, &v), NR_TD_GW_E_FAILED);
    EXPECT_EQ(v.status, -42);
    EXPECT_EQ(nr_td_grantwork_get_llr(gw, 5, nullptr, &v), NR_TD_GW_E_FAILED);
    EXPECT_EQ(f.calls.load(), 1);
    EXPECT_EQ(nr_td_grantwork_acquire(gw, 5, &v), NR_TD_GW_ACQ_FAILED);
    nr_td_grantwork_release(gw);
  }
}

TEST(GrantWork, OwnerPublishThenReady)
{
  Fixture f;
  nr_td_grantwork_t *gw = f.begin();
  nr_td_gw_llr_view_t v;
  ASSERT_EQ(nr_td_grantwork_acquire(gw, 9, &v), NR_TD_GW_ACQ_OWNER);
  EXPECT_EQ(nr_td_grantwork_fep_mask(gw, 1.5), 0);
  nr_td_grantwork_fep_done(gw, 1.5, 0x0FFC);
  nr_td_grantwork_fep_done(gw, 1.5, 0x0003); /* lazy extension */
  EXPECT_EQ(nr_td_grantwork_fep_mask(gw, 1.5), 0x0FFF);
  EXPECT_EQ(nr_td_grantwork_fep_mask(gw, 2.0), 0); /* another FEP frequency offset is another FEP */
  nr_td_grantwork_fep_done(gw, 2.0, 0x0004);
  EXPECT_EQ(nr_td_grantwork_fep_mask(gw, 2.0), 0x0004);
  const int16_t llr[4] = {1, -2, 3, -4};
  ASSERT_EQ(nr_td_grantwork_publish(gw, 9, llr, 4, 2, 4, nullptr, 0), NR_TD_GW_OK);
  EXPECT_EQ(nr_td_grantwork_publish(gw, 9, llr, 4, 2, 4, nullptr, 0), NR_TD_GW_E_ARG); /* immutable */
  ASSERT_EQ(nr_td_grantwork_get_llr(gw, 9, nullptr, &v), NR_TD_GW_OK);
  EXPECT_EQ(f.calls.load(), 0); /* published by the owner, never lazily computed */
  EXPECT_EQ(v.G, 4u);
  EXPECT_EQ(v.nl, 2);
  EXPECT_EQ(v.qm, 4);
  EXPECT_EQ(memcmp(v.llr, llr, sizeof(llr)), 0);
  nr_td_grantwork_release(gw);
}

TEST(GrantWork, ConcurrentReadersComputeOnce)
{
  Fixture f;
  nr_td_grantwork_t *gw = f.begin();
  nr_td_gw_llr_view_t v0;
  /* the job thread owns the computation (the decoder's acquire/publish) while readers arrive */
  ASSERT_EQ(nr_td_grantwork_acquire(gw, 33, &v0), NR_TD_GW_ACQ_OWNER);
  const int N = 8;
  std::vector<std::thread> th;
  std::vector<const int16_t *> ptr(N);
  std::vector<int> rc(N);
  for (int t = 0; t < N; t++)
    th.emplace_back([&, t] {
      nr_td_grantwork_t *mine = nr_td_grantwork_retain(gw);
      nr_td_gw_llr_view_t v;
      rc[t] = nr_td_grantwork_get_llr(mine, 33, nullptr, &v); /* waits for the owner */
      ptr[t] = v.llr;
      for (int i = 0; rc[t] == NR_TD_GW_OK && i < 1000; i++)
        if (v.llr[i] != (int16_t)(33 * 7 + i))
          rc[t] = 1000;
      nr_td_grantwork_release(mine);
    });
  usleep(20000);
  std::vector<int16_t> llr(1000);
  for (int i = 0; i < 1000; i++)
    llr[i] = (int16_t)(33 * 7 + i);
  ASSERT_EQ(nr_td_grantwork_publish(gw, 33, llr.data(), 1000, 1, 2, nullptr, 0), NR_TD_GW_OK);
  for (auto &t : th)
    t.join();
  EXPECT_EQ(nr_td_grantwork_compute_count(gw, 33), 1);
  for (int t = 0; t < N; t++) {
    EXPECT_EQ(rc[t], NR_TD_GW_OK);
    EXPECT_EQ(ptr[t], ptr[0]);
  }
  EXPECT_EQ(nr_td_grantwork_refcount(gw), 1);
  nr_td_grantwork_release(gw);
}

TEST(GrantWork, LazyComputeOnlyOnJobThreadWhileAlive)
{
  Fixture f;
  nr_td_grantwork_t *gw = f.begin();
  nr_td_gw_llr_view_t v;
  ASSERT_EQ(nr_td_grantwork_get_llr(gw, 1, nullptr, &v), NR_TD_GW_OK);
  const uint64_t r0 = nr_td_grantwork_refused_count();
  int rc_other = 0, rc_read = -1;
  std::thread t([&] {
    nr_td_gw_llr_view_t w;
    rc_other = nr_td_grantwork_get_llr(gw, 2, nullptr, &w); /* missing: not this thread's to compute */
    rc_read = nr_td_grantwork_get_llr(gw, 1, nullptr, &w);  /* READY: readable anywhere */
  });
  t.join();
  EXPECT_EQ(rc_other, NR_TD_GW_E_NOTOWNER);
  EXPECT_EQ(rc_read, NR_TD_GW_OK);
  nr_td_grantwork_job_end(gw);
  EXPECT_EQ(nr_td_grantwork_get_llr(gw, 3, nullptr, &v), NR_TD_GW_E_NOTOWNER);
  EXPECT_EQ(nr_td_grantwork_acquire(gw, 4, &v), NR_TD_GW_ACQ_ERR);
  EXPECT_EQ(nr_td_grantwork_refused_count(), r0 + 3);
  EXPECT_EQ(nr_td_grantwork_get_llr(gw, 1, nullptr, &v), NR_TD_GW_OK);
  EXPECT_EQ(f.calls.load(), 1);
  EXPECT_EQ(nr_td_grantwork_flags(gw), 0u);
  nr_td_grantwork_release(gw);
}

TEST(GrantWork, BorrowedFepGenerationMakesNewComputesStale)
{
  Fixture f;
  nr_td_grantwork_t *gw = f.begin();
  nr_td_gw_llr_view_t v;
  ASSERT_EQ(nr_td_grantwork_get_llr(gw, 1, nullptr, &v), NR_TD_GW_OK);
  EXPECT_TRUE(nr_td_grantwork_fep_alive(gw));
  __atomic_fetch_add(&f.gen, 1, __ATOMIC_RELEASE); /* the consumer reused its buffer for another slot */
  EXPECT_FALSE(nr_td_grantwork_fep_alive(gw));
  EXPECT_EQ(nr_td_grantwork_get_llr(gw, 1, nullptr, &v), NR_TD_GW_OK); /* computed entries stay readable */
  EXPECT_EQ(nr_td_grantwork_get_llr(gw, 2, nullptr, &v), NR_TD_GW_E_STALE);
  EXPECT_EQ(nr_td_grantwork_acquire(gw, 3, &v), NR_TD_GW_ACQ_ERR);
  EXPECT_EQ(f.calls.load(), 1);
  EXPECT_TRUE(nr_td_grantwork_flags(gw) & NR_TD_GW_F_STALE); /* the whole grant is inadmissible */
  nr_td_grantwork_release(gw);
}

TEST(GrantWork, TableFullAndBadArgs)
{
  Fixture f;
  nr_td_grantwork_t *gw = f.begin();
  nr_td_gw_llr_view_t v;
  for (int s = 0; s < NR_TD_GW_MAX_SIG; s++)
    ASSERT_EQ(nr_td_grantwork_get_llr(gw, 1000 + s, nullptr, &v), NR_TD_GW_OK);
  EXPECT_EQ(nr_td_grantwork_get_llr(gw, 5000, nullptr, &v), NR_TD_GW_E_FULL);
  EXPECT_TRUE(nr_td_grantwork_flags(gw) & NR_TD_GW_F_FULL);
  nr_td_grantwork_flag(gw, NR_TD_GW_F_HARQ);
  EXPECT_EQ(nr_td_grantwork_flags(gw), NR_TD_GW_F_FULL | NR_TD_GW_F_HARQ);
  EXPECT_EQ(nr_td_grantwork_get_llr(nullptr, 1, nullptr, &v), NR_TD_GW_E_ARG);
  nr_td_grantwork_release(gw);
  nr_td_gw_job_t j = {};
  j.abs_slot = 1;
  j.slots_per_frame = 0;
  EXPECT_EQ(nr_td_grantwork_begin(&j), nullptr);
  /* no callback: a missing entry cannot be computed */
  j.slots_per_frame = 20;
  nr_td_grantwork_t *g2 = nr_td_grantwork_begin(&j);
  ASSERT_NE(g2, nullptr);
  EXPECT_EQ(nr_td_grantwork_get_llr(g2, 1, nullptr, &v), NR_TD_GW_E_NOCOMPUTE);
  nr_td_grantwork_release(g2);
}

TEST(GrantWork, KeyCanonicalisesMaskDeterminedFields)
{
  nr_pdsch_cfg_hypothesis_t a = {};
  a.tda_start = 1;
  a.tda_length = 13;
  a.k0 = 0;
  a.dmrs_mask = 0x884;
  a.dmrs_add_pos = 2;
  a.dmrs_max_len = 1;
  a.mapping_type = 0;
  a.mcs_table = 0;
  nr_pdsch_cfg_hypothesis_t b = a;
  b.dmrs_add_pos = 3; /* same effective mask */
  b.mcs_table = 1;    /* table enters through Qm only */
  EXPECT_EQ(nr_td_grantwork_key(&a, 1, 6), nr_td_grantwork_key(&b, 1, 6));
  EXPECT_NE(nr_td_grantwork_key(&a, 1, 6), nr_td_grantwork_key(&b, 1, 8));
  EXPECT_NE(nr_td_grantwork_key(&a, 1, 6), nr_td_grantwork_key(&a, 2, 6));
  b = a;
  b.dmrs_mask = 0x804;
  EXPECT_NE(nr_td_grantwork_key(&a, 1, 6), nr_td_grantwork_key(&b, 1, 6));
  b = a;
  b.k0 = 1;
  EXPECT_NE(nr_td_grantwork_key(&a, 1, 6), nr_td_grantwork_key(&b, 1, 6));
  b = a;
  b.tda_length = 12;
  EXPECT_NE(nr_td_grantwork_key(&a, 1, 6), nr_td_grantwork_key(&b, 1, 6));
  /* legacy catalogues (mask 0): the IE fields are the only DM-RS description, keep them */
  a.dmrs_mask = b.dmrs_mask = 0;
  b = a;
  b.dmrs_add_pos = 1;
  EXPECT_NE(nr_td_grantwork_key(&a, 1, 6), nr_td_grantwork_key(&b, 1, 6));
}

TEST(GrantWork, AllocatorPoolsAndIsUsable)
{
  void *p = nr_td_gw_alloc(12345);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(((uintptr_t)p) % 64, 0u);
  memset(p, 0x5a, 12345);
  nr_td_gw_free(p);
  uint64_t f0, r0, u0;
  nr_td_gw_alloc_stats(&f0, &r0, &u0);
  void *q = nr_td_gw_alloc(12000);
  uint64_t f1, r1, u1;
  nr_td_gw_alloc_stats(&f1, &r1, &u1);
  EXPECT_EQ(r1, r0 + 1);
  EXPECT_EQ(f1, f0);
  nr_td_gw_free(q);
  nr_td_gw_free(nullptr);
#ifdef NR_TD_GW_CUDA
  /* CUDA build on a host with libcudart: the buffers are unified (managed) memory. */
  if (getenv("ISAC_TD_GW_UNIFIED") == nullptr) {
    EXPECT_TRUE(nr_td_gw_unified()) << "CUDA build but cudaMallocManaged unavailable";
  }
#else
  EXPECT_FALSE(nr_td_gw_unified());
#endif
}

/* ---- CB0 extraction fixture -------------------------------------------------------------------
 * TX: a random circular buffer w (N = 66Z bits, filler F) rate-matched + bit-interleaved for CB0 with
 * the OAI TX functions, mapped to LLRs (bit 0 -> +, bit 1 -> -) at offset 0 of a G-long TB LLR stream
 * (the other code blocks random). RX: nr_td_gw_cb0_extract() must (1) equal what the LDPC segment
 * decoder computes for r = 0 from the same TB LLRs (its exact calls, its own E0), and (2) recover w:
 * every selected position has the right sign, everything else is 0. */
namespace {
struct Cb0Case {
  int BG, Z, C, Kprime, Qm, Nl, rv;
  uint32_t G, tbslbrm;
};

void run_cb0_case(const Cb0Case &cs)
{
  const int Kb = cs.BG == 1 ? 22 : 10;
  const int K = Kb * cs.Z;
  const int F = K - cs.Kprime;
  const int N = (cs.BG == 1 ? 66 : 50) * cs.Z;
  nr_td_cb0_params_t p = {};
  p.G = cs.G;
  p.C = cs.C;
  p.K = K;
  p.Z = cs.Z;
  p.F = F;
  p.BG = cs.BG;
  p.Qm = cs.Qm;
  p.Nl = cs.Nl;
  p.rv = cs.rv;
  p.tbslbrm = cs.tbslbrm;
  /* nr_get_E(G, C, Qm, Nl, r = 0) */
  const uint32_t q = cs.G / (cs.Nl * cs.Qm);
  p.E = (0 <= cs.C - (int)(q % cs.C) - 1) ? cs.Nl * cs.Qm * (q / cs.C) : cs.Nl * cs.Qm * (q / cs.C + 1);
  std::mt19937 rng(cs.BG * 1000 + cs.Z + cs.rv);
  std::vector<uint8_t> w(N + 64, 0), e(p.E + 64), f(p.E + 64);
  const int Foffset = K - F - 2 * cs.Z;
  for (int i = 0; i < N; i++)
    w[i] = (i >= Foffset && i < Foffset + F) ? 0 : (rng() & 1);
  ASSERT_EQ(nr_rate_matching_ldpc(cs.tbslbrm, cs.BG, cs.Z, w.data(), e.data(), cs.C, F, Foffset, cs.rv, p.E), 0);
  nr_interleaving_ldpc(p.E, cs.Qm, e.data(), f.data());
  std::vector<int16_t> llr(cs.G);
  for (uint32_t i = 0; i < cs.G; i++)
    llr[i] = (int16_t)((int)(rng() % 200) - 100);
  for (uint32_t i = 0; i < p.E; i++)
    llr[i] = (int16_t)((f[i] ? -1 : 1) * (5 + (int)(rng() % 40)));
  const int dlen = nr_td_gw_cb0_dlen(&p);
  std::vector<int16_t> d(dlen, 77);
  ASSERT_EQ(nr_td_gw_cb0_extract(llr.data(), cs.G, &p, d.data()), NR_TD_GW_OK);
  /* (1) the segment decoder's own r = 0 computation */
  std::vector<int16_t> ref(dlen, 0), harq_e(p.E);
  nr_deinterleaving_ldpc(p.E, cs.Qm, harq_e.data(), llr.data());
  ASSERT_EQ(nr_rate_matching_ldpc_rx(cs.tbslbrm, cs.BG, cs.Z, ref.data(), harq_e.data(), cs.C, cs.rv, 1, p.E, F, Foffset), 0);
  EXPECT_EQ(memcmp(d.data(), ref.data(), (size_t)N * sizeof(int16_t)), 0);
  /* (2) TX/RX round trip: the bits selected for CB0 come back with the right sign */
  int checked = 0;
  for (int i = 0; i < N; i++) {
    if (d[i] == 0)
      continue;
    ASSERT_FALSE(i >= Foffset && i < Foffset + F) << "filler position carries LLR " << i;
    ASSERT_EQ(d[i] < 0, w[i] == 1) << "position " << i;
    checked++;
  }
  /* positions reachable: the circular buffer (N, or N_ref when LBRM binds) minus the filler bits */
  const uint32_t Ncb = cs.tbslbrm ? std::min<uint32_t>(N, 3 * cs.tbslbrm / (2 * cs.C)) : N;
  EXPECT_GT(checked, ((int)std::min<uint32_t>(p.E, Ncb) - F) * 9 / 10);
  EXPECT_LE(checked, (int)Ncb - F);
}
} // namespace

TEST(GrantWorkCb0, ExtractEqualsSegmentDecoderInputBG1)
{
  run_cb0_case({1, 384, 3, 8000, 4, 1, 0, 36000, 0});
  run_cb0_case({1, 384, 3, 8000, 4, 1, 2, 36000, 0});
  run_cb0_case({1, 384, 3, 8000, 6, 2, 0, 3 * 12 * 1500, 0});
}

TEST(GrantWorkCb0, ExtractEqualsSegmentDecoderInputLbrmBinds)
{
  /* N_ref = 3 * tbslbrm / (2C) below N: the circular buffer is truncated (the rank-4 'MCS-25 wall') */
  run_cb0_case({1, 384, 2, 8000, 8, 4, 0, 2 * 32 * 600, 12000});
  run_cb0_case({1, 384, 2, 8000, 8, 4, 3, 2 * 32 * 600, 12000});
}

TEST(GrantWorkCb0, ExtractEqualsSegmentDecoderInputBG2Repetition)
{
  /* E > Ncb: the bit selection wraps and accumulates (low code rate) */
  run_cb0_case({2, 64, 1, 600, 2, 1, 0, 8000, 0});
  run_cb0_case({2, 64, 1, 600, 2, 1, 1, 8000, 0});
}

/* K38 x GrantWork (fix round 1, C1): one signature, two hypotheses with different TBS -> different C -> different
 * normalisation spans. The shared buffer is un-normalised; each hypothesis's CB0 input from it must equal what a
 * full decode WITHOUT GrantWork computes for that hypothesis: shift k = nr_llr_norm_shift(llr, ceil(G / C)) applied
 * to all G LLRs (the decode's ISAC_LLR_NORM block), then the segment decoder's r = 0 deinterleave + rate de-match. */
TEST(GrantWorkCb0, SameSignatureDifferentCNormalisesPerHypothesis)
{
  const uint32_t G = 3 * 12 * 1600; /* Qm 4, Nl 1 */
  std::mt19937 rng(7);
  std::vector<int16_t> shared(G);
  /* power that varies along the buffer: code block 0's span alone sees a different mean than the whole G */
  for (uint32_t i = 0; i < G; i++) {
    const int amp = i < G / 3 ? 900 : 120;
    shared[i] = (int16_t)((int)(rng() % (2 * amp + 1)) - amp);
  }
  struct H {
    uint32_t A;
    int BG, C, K, Z, F;
  } hyps[2] = {{3000, 1, 1, 8448, 384, 8448 - 3024}, {20000, 1, 3, 8448, 384, 8448 - 6704}};
  int ks[2];
  for (int h = 0; h < 2; h++) {
    nr_td_cb0_params_t p = {};
    p.G = G;
    p.A = hyps[h].A;
    p.C = hyps[h].C;
    p.K = hyps[h].K;
    p.Z = hyps[h].Z;
    p.F = hyps[h].F;
    p.BG = hyps[h].BG;
    p.Qm = 4;
    p.Nl = 1;
    p.rv = 0;
    const uint32_t q = G / (p.Nl * p.Qm);
    p.E = (0 <= p.C - (int)(q % p.C) - 1) ? p.Nl * p.Qm * (q / p.C) : p.Nl * p.Qm * (q / p.C + 1);
    ASSERT_EQ(nr_llr_norm_num_cb(p.A, p.BG), (uint32_t)p.C);
    const int dlen = nr_td_gw_cb0_dlen(&p);
    /* reference: the decode without GrantWork */
    std::vector<int16_t> full = shared;
    const int k = nr_llr_norm_shift(full.data(), nr_llr_norm_span(G, nr_llr_norm_num_cb(p.A, p.BG)));
    for (uint32_t i = 0; i < G; i++)
      full[i] = (int16_t)(full[i] >> k);
    std::vector<int16_t> ref(dlen, 0), harq_e(p.E);
    nr_deinterleaving_ldpc(p.E, p.Qm, harq_e.data(), full.data());
    ASSERT_EQ(nr_rate_matching_ldpc_rx(0, p.BG, p.Z, ref.data(), harq_e.data(), p.C, p.rv, 1, p.E, p.F, p.K - p.F - 2 * p.Z), 0);
    /* GrantWork: the shared, un-normalised buffer */
    std::vector<int16_t> e0(p.E), d(dlen, 99);
    int kg = -2;
    ASSERT_EQ(nr_td_gw_cb0_input(shared.data(), G, &p, true, e0.data(), d.data(), &kg), NR_TD_GW_OK);
    EXPECT_EQ(kg, k);
    EXPECT_EQ(memcmp(e0.data(), full.data(), (size_t)p.E * sizeof(int16_t)), 0);
    EXPECT_EQ(memcmp(d.data(), ref.data(), (size_t)dlen * sizeof(int16_t)), 0);
    ks[h] = k;
    /* normalisation off: the raw LLRs */
    ASSERT_EQ(nr_td_gw_cb0_input(shared.data(), G, &p, false, e0.data(), d.data(), &kg), NR_TD_GW_OK);
    EXPECT_EQ(kg, -1);
    EXPECT_EQ(memcmp(e0.data(), shared.data(), (size_t)p.E * sizeof(int16_t)), 0);
  }
  EXPECT_NE(ks[0], ks[1]); /* the fixture does exercise a C-dependent shift */
}

TEST(GrantWorkCb0, RejectsWhatTheDecoderRejects)
{
  nr_td_cb0_params_t p = {};
  p.G = 1000;
  p.C = 0;
  p.K = 8448;
  p.Z = 384;
  p.F = 0;
  p.BG = 1;
  p.Qm = 2;
  p.Nl = 1;
  p.E = 500;
  std::vector<int16_t> llr(1000), d(68 * 384);
  EXPECT_EQ(nr_td_gw_cb0_extract(llr.data(), 1000, &p, d.data()), NR_TD_GW_E_ARG);
  p.C = 1;
  p.E = 2000; /* more than the LLRs available */
  EXPECT_EQ(nr_td_gw_cb0_extract(llr.data(), 1000, &p, d.data()), NR_TD_GW_E_ARG);
  p.E = 500;
  p.tbslbrm = 100; /* N_ref tiny: Foffset > Ncb */
  EXPECT_EQ(nr_td_gw_cb0_extract(llr.data(), 1000, &p, d.data()), NR_TD_GW_E_RM);
}

int main(int argc, char **argv)
{
  logInit(); /* the real rate matching reports rejects through LOG_E */
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
