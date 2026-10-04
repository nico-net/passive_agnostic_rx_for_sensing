/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* nr_td_cb0_gpu_backend: the CUDA CB0 entry of libldpc_cuda.so behind the nr_td_cb0 backend interface, on real OAI
 * codewords (fixture: random TB, TB CRC, segmentation, OAI encoder, rate matching, interleaving).
 * Oracles: the CPU backend (nr_td_cb0_batch, CPU LDPC) on clean hypotheses, and round 2's CUDA path through G1's TB
 * entry (same decoder, same iterations: verdicts must be identical item by item). Skips without a CUDA device. */
#include <gtest/gtest.h>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <unistd.h>
#include <vector>
#include "nr_td_cb0_fixture.h"
#include "nr_td_cb0_sched.h"

namespace {

struct Fx {
  cb0_fx_t fx{};
  explicit Fx(uint32_t A, uint16_t R, uint8_t Qm, uint8_t Nl, uint8_t rv, uint32_t G, uint32_t seed = 1)
  {
    fx.A = A; fx.R = R; fx.Qm = Qm; fx.Nl = Nl; fx.rv = rv; fx.G = G;
    EXPECT_EQ(cb0_fx_encode(&fx, seed, 64), 0);
  }
  ~Fx() { cb0_fx_free(&fx); }
  nr_td_cb0_item_t item() const { return cb0_fx_item(&fx); }
};
Fx *big(uint8_t rv = 0) /* 106 PRB 64QAM R 0.505 rank 1: C = 11, BG1 Z = 384 */
{
  const uint32_t A = cb0_fx_tbs(6, 5170, 106, 12, 6, 1), G = cb0_fx_G(106, 12, 6, 1, 6, 1);
  return new Fx(A, 5170, 6, 1, rv, G);
}
Fx *small(uint8_t rv = 0) /* 6 PRB 16QAM R 0.12: C = 1, BG2 */
{
  const uint32_t A = cb0_fx_tbs(4, 1200, 6, 12, 6, 1), G = cb0_fx_G(6, 12, 6, 1, 4, 1);
  return new Fx(A, 1200, 4, 1, rv, G);
}
Fx *mid() /* 24 PRB QPSK R 0.3: C = 1, a third Z */
{
  const uint32_t A = cb0_fx_tbs(2, 3080, 24, 12, 6, 1), G = cb0_fx_G(24, 12, 6, 1, 2, 1);
  return new Fx(A, 3080, 2, 1, 0, G);
}

const nr_td_cb0_gpu_entry_t *B;

class Cb0Gpu : public ::testing::Test {
 protected:
  void SetUp() override
  {
    cb0_fx_init();
    nr_td_cb0_use_cuda_ldpc(0);
    nr_td_cb0_set_threads(4);
    B = nr_td_cb0_gpu_backend();
    if (!B)
      GTEST_SKIP() << "no CUDA CB0 backend (libldpc_cuda.so / device)";
  }
};

struct GpuRun {
  std::vector<nr_td_cb0_meta_t> meta;
  std::vector<int8_t> l;
  std::vector<nr_td_cb0_result_t> out;
  int rc = 0;
};
GpuRun gpu(const std::vector<nr_td_cb0_item_t> &it)
{
  GpuRun r;
  const int n = (int)it.size();
  r.meta.resize(n);
  r.out.assign(n, nr_td_cb0_result_t{});
  r.l.assign((size_t)n * NR_TD_CB0_L_STRIDE, 0);
  std::vector<int8_t> st(n);
  for (int i = 0; i < n; i++)
    if (nr_td_cb0_meta(&it[i], &r.meta[i]) != 0) {
      r.out[i].pass = -1;
      r.out[i].err = r.meta[i].err;
    }
  EXPECT_EQ(nr_td_cb0_dematch(it.data(), n, nr_td_cb0_use_gpu(1), r.l.data(), nullptr, st.data()), 0);
  r.rc = B->decode(r.meta.data(), r.l.data(), n, r.out.data());
  return r;
}
std::vector<nr_td_cb0_result_t> batch(const std::vector<nr_td_cb0_item_t> &it)
{
  std::vector<nr_td_cb0_result_t> o(it.size());
  nr_td_cb0_batch(it.data(), (int)it.size(), o.data());
  return o;
}

struct Set {
  std::vector<nr_td_cb0_item_t> it;
  std::vector<int> truth; /* 1 true, 0 wrong rv, 2 TBS twin */
  std::vector<std::vector<int16_t>> copies;
};
void build(Set &s, std::initializer_list<Fx *> fxs, int replicas)
{
  for (Fx *f : fxs) {
    const nr_td_cb0_item_t ok = f->item();
    s.it.push_back(ok);
    s.truth.push_back(1);
    for (uint8_t rv = 0; rv < 4; rv++)
      if (rv != ok.rv) {
        nr_td_cb0_item_t w = ok;
        w.rv = rv;
        s.it.push_back(w);
        s.truth.push_back(0);
      }
    nr_td_cb0_item_t w = ok;
    w.tbs = ok.tbs + 8 * 6;
    s.it.push_back(w);
    s.truth.push_back(2);
  }
  const size_t n0 = s.it.size();
  s.copies.reserve(n0 * replicas);
  for (int k = 0; k < replicas; k++)
    for (size_t i = 0; i < n0; i++) {
      s.copies.emplace_back(s.it[i].llr, s.it[i].llr + s.it[i].G);
      nr_td_cb0_item_t x = s.it[i];
      x.llr = s.copies.back().data();
      s.it.push_back(x);
      s.truth.push_back(s.truth[i]);
    }
}

/* GPU backend == G1-TB-entry CUDA path (same decoder and iterations), == CPU backend on these clean hypotheses;
 * true hypotheses pass, wrong rv fail; mixed BG / Z in one submission. */
TEST_F(Cb0Gpu, VerdictsEqualRoundTwoCudaAndCpu)
{
  Fx *b = big(), *s = small(), *m = mid(), *b2 = big(2);
  Set set;
  build(set, {b, s, m, b2}, 4);
  const auto cpu = batch(set.it); /* CPU backend */
  ASSERT_EQ(nr_td_cb0_use_cuda_ldpc(1), 1);
  const auto r2 = batch(set.it); /* round 2: G1's TB entry, probe form */
  nr_td_cb0_use_cuda_ldpc(0);
  const GpuRun g = gpu(set.it);
  ASSERT_EQ(g.rc, 0);
  int twins = 0;
  for (size_t i = 0; i < set.it.size(); i++) {
    EXPECT_EQ(g.out[i].decoder_used, NR_TD_CB0_DEC_CUDA_FLOODING) << i;
    EXPECT_EQ(g.out[i].err, 0) << i;
    EXPECT_GE(g.out[i].iters, 1);
    EXPECT_LE(g.out[i].iters, nr_td_cb0_gpu_iters(set.it[i].max_iter));
    EXPECT_EQ(g.out[i].pass, r2[i].pass) << "item " << i << " vs G1 TB entry";
    EXPECT_EQ(g.out[i].pass, cpu[i].pass) << "item " << i << " vs CPU backend";
    if (set.truth[i] == 1)
      EXPECT_EQ(g.out[i].pass, 1) << i;
    if (set.truth[i] == 0)
      EXPECT_EQ(g.out[i].pass, 0) << i;
    twins += set.truth[i] == 2 && g.out[i].pass == 1;
    if (set.truth[i] == 1)
      EXPECT_LT(g.out[i].iters, nr_td_cb0_gpu_iters(set.it[i].max_iter)) << "a clean truth converges early";
  }
  printf("[cb0-gpu] TBS-twin passes %d\n", twins);
  delete b; delete s; delete m; delete b2;
}

/* all-zero codeword: the decoder converges, the guard rejects it (as cb0_guard / the receiver) */
TEST_F(Cb0Gpu, AllZeroCodewordIsRejected)
{
  for (int which = 0; which < 2; which++) {
    Fx *f = which ? small() : big();
    std::vector<int16_t> zero(f->fx.G, 64);
    nr_td_cb0_item_t it = f->item();
    it.llr = zero.data();
    const GpuRun g = gpu({it});
    EXPECT_EQ(g.out[0].pass, 0) << which;
    EXPECT_LT(g.out[0].iters, nr_td_cb0_gpu_iters(it.max_iter)) << "the decoder converged: only the guard rejects";
    EXPECT_EQ(g.out[0].pass, batch({it})[0].pass);
    delete f;
  }
}

/* async submit / collect == sync; invalid meta items keep the caller's verdict */
TEST_F(Cb0Gpu, AsyncAndInvalidItems)
{
  Fx *b = big(), *s = small();
  Set set;
  build(set, {b, s}, 2);
  nr_td_cb0_item_t bad = set.it[0];
  bad.Qm = 3;
  set.it.push_back(bad);
  set.truth.push_back(-1);
  const GpuRun ref = gpu(set.it);
  const int n = (int)set.it.size();
  std::vector<nr_td_cb0_result_t> out(n);
  for (int i = 0; i < n; i++)
    out[i] = ref.out[i].pass == -1 ? ref.out[i] : nr_td_cb0_result_t{};
  void *t = nullptr;
  ASSERT_EQ(B->submit(ref.meta.data(), ref.l.data(), n, &t), 0);
  ASSERT_EQ(B->collect(t, out.data()), 0);
  for (int i = 0; i < n; i++) {
    EXPECT_EQ(out[i].pass, ref.out[i].pass) << i;
    EXPECT_EQ(out[i].iters, ref.out[i].iters) << i;
  }
  EXPECT_EQ(out[n - 1].pass, -1);
  EXPECT_EQ(out[n - 1].err, NR_TD_CB0_ERR_ARG);
  delete b; delete s;
}

/* a GPU failure makes the whole batch inadmissible (every valid item -1 / ERR_GPU), the call returns < 0 */
TEST_F(Cb0Gpu, GpuFailureMakesBatchInadmissible)
{
  void *h = dlopen("libldpc_cuda.so", RTLD_NOW | RTLD_NOLOAD);
  if (!h)
    h = dlopen("./libldpc_cuda.so", RTLD_NOW | RTLD_NOLOAD);
  ASSERT_NE(h, nullptr);
  auto hooks = (void (*)(int, int, int, int, int))dlsym(h, "ldpc_cb0_test_hooks");
  auto reset = (void (*)(void))dlsym(h, "ldpc_cb0_test_reset");
  ASSERT_TRUE(hooks && reset);
  Fx *b = big();
  Set set;
  build(set, {b}, 1);
  for (int inject : {1, 2, 3}) {
    hooks(inject, 300, 30, 100, 1000);
    const GpuRun g = gpu(set.it);
    EXPECT_LT(g.rc, 0) << "inject " << inject;
    for (size_t i = 0; i < set.it.size(); i++) {
      EXPECT_EQ(g.out[i].pass, -1) << inject << " " << i;
      EXPECT_EQ(g.out[i].err, NR_TD_CB0_ERR_GPU) << inject << " " << i;
    }
    hooks(0, 0, 2000, 4, 5000);
    reset();
    usleep(400000); /* a stalled (abandoned) submission drains */
  }
  const GpuRun ok = gpu(set.it);
  EXPECT_EQ(ok.rc, 0);
  EXPECT_EQ(ok.out[0].pass, 1);
  delete b;
}


/* ---- integration with the CPU wiring's backend interface (nr_td_cb0_exec) ---- */
struct Hooks {
  void (*hooks)(int, int, int, int, int);
  void (*reset)(void);
  Hooks()
  {
    void *h = dlopen("libldpc_cuda.so", RTLD_NOW | RTLD_NOLOAD);
    if (!h)
      h = dlopen("./libldpc_cuda.so", RTLD_NOW | RTLD_NOLOAD);
    hooks = h ? (void (*)(int, int, int, int, int))dlsym(h, "ldpc_cb0_test_hooks") : nullptr;
    reset = h ? (void (*)(void))dlsym(h, "ldpc_cb0_test_reset") : nullptr;
  }
};

/* auto: the registered adapter runs the batch on the GPU (decoder CUDA, verdicts = CPU backend's on these clean items);
 * an injected GPU failure voids THAT batch only (every item -1 / ERR_GPU, info.failed), and the NEXT grant runs on the
 * CPU backend (back-off); after the back-off the GPU is used again. */
TEST_F(Cb0Gpu, ExecUsesGpuAndFallsBackToCpuAfterFailure)
{
  Hooks hk;
  ASSERT_TRUE(hk.hooks && hk.reset);
  ASSERT_EQ(nr_td_cb0_gpu_register(), 1);
  setenv("ISAC_TD_CB0_GPU_BACKOFF", "2", 1);
  nr_td_cb0_backend_mode_set(NR_TD_CB0_BE_AUTO);
  Fx *b = big(), *s = small();
  Set set;
  build(set, {b, s}, 1);
  const int n = (int)set.it.size();
  std::vector<nr_td_cb0_result_t> cpu(n), out(n);
  nr_td_cb0_batch_cpu(set.it.data(), n, cpu.data());
  nr_td_cb0_backend_stats_t s0, s1;
  nr_td_cb0_backend_get_stats(&s0);
  nr_td_cb0_exec_t ex;
  ASSERT_GT(nr_td_cb0_exec(set.it.data(), n, out.data(), &ex), 0);
  EXPECT_EQ(ex.backend, NR_TD_CB0_BE_GPU);
  EXPECT_EQ(ex.decoder, NR_TD_CB0_DEC_CUDA_FLOODING);
  EXPECT_EQ(ex.failed, 0);
  for (int i = 0; i < n; i++) {
    EXPECT_EQ(out[i].pass, cpu[i].pass) << i;
    EXPECT_EQ(out[i].decoder_used, NR_TD_CB0_DEC_CUDA_FLOODING) << i;
    EXPECT_EQ(out[i].dematch_gpu, 1) << i;
    EXPECT_EQ(out[i].C, cpu[i].C) << i;
    EXPECT_EQ(out[i].tb_result, cpu[i].tb_result) << i;
  }
  /* grant k: GPU fails -> the whole batch is void */
  hk.hooks(1, 0, 0, 100, 1000);
  nr_td_cb0_exec(set.it.data(), n, out.data(), &ex);
  hk.hooks(0, 0, 2000, 4, 5000);
  EXPECT_EQ(ex.backend, NR_TD_CB0_BE_GPU);
  EXPECT_EQ(ex.failed, 1);
  for (int i = 0; i < n; i++) {
    EXPECT_EQ(out[i].pass, -1) << i;
    EXPECT_EQ(out[i].err, NR_TD_CB0_ERR_GPU) << i;
  }
  /* grant k+1 (and k+2): back-off -> CPU backend, valid CPU verdicts */
  for (int g = 0; g < 2; g++) {
    nr_td_cb0_exec(set.it.data(), n, out.data(), &ex);
    EXPECT_EQ(ex.backend, NR_TD_CB0_BE_CPU) << g;
    EXPECT_EQ(ex.failed, 0);
    EXPECT_EQ(ex.decoder, NR_TD_CB0_DEC_CPU_LAYERED);
    for (int i = 0; i < n; i++)
      EXPECT_EQ(out[i].pass, cpu[i].pass) << g << " " << i;
  }
  /* back-off over: the GPU again */
  nr_td_cb0_exec(set.it.data(), n, out.data(), &ex);
  EXPECT_EQ(ex.backend, NR_TD_CB0_BE_GPU);
  EXPECT_EQ(ex.failed, 0);
  /* CB0 breaker open -> healthy() = 0 -> CPU without a failed batch */
  hk.hooks(1, 0, 0, 1, 60000);
  nr_td_cb0_exec(set.it.data(), n, out.data(), &ex); /* fails, trips the CB0 breaker (N = 1) */
  hk.hooks(0, 0, 2000, 4, 5000);
  nr_td_cb0_backend_mode_set(NR_TD_CB0_BE_AUTO); /* clears the dispatcher's back-off: only health decides now */
  nr_td_cb0_exec(set.it.data(), n, out.data(), &ex);
  EXPECT_EQ(ex.backend, NR_TD_CB0_BE_CPU) << "breaker open: unhealthy";
  EXPECT_EQ(ex.failed, 0);
  hk.reset();
  nr_td_cb0_exec(set.it.data(), n, out.data(), &ex);
  EXPECT_EQ(ex.backend, NR_TD_CB0_BE_GPU);
  nr_td_cb0_backend_get_stats(&s1);
  EXPECT_EQ(s1.gpu_failed - s0.gpu_failed, 2u);
  EXPECT_GE(s1.gpu_skipped - s0.gpu_skipped, 3u);
  nr_td_cb0_gpu_counters_t gc;
  nr_td_cb0_gpu_get_counters(&gc);
  EXPECT_GE(gc.errors, 2u);
  EXPECT_GE(gc.trips, 1u);
  nr_td_cb0_register_gpu_backend(nullptr);
  nr_td_cb0_backend_mode_set(-1);
  unsetenv("ISAC_TD_CB0_GPU_BACKOFF");
  delete b;
  delete s;
}

/* Memory lifetime (2026-10-04 review): after a CB0 TIMEOUT (fault injection: stalled stream) or a CUDA error the adapter
 * ABANDONS its thread-local input buffer -- the GPU may still read it -- instead of reusing / freeing it: the next batch gets a
 * fresh buffer (a different address: the old one is leaked, never freed), and its verdicts are right. A failure the GPU never
 * saw (CB0 breaker open: rejected before any enqueue) keeps the buffer. */
TEST_F(Cb0Gpu, AdapterAbandonsInputBufferAfterTimeoutOrCudaError)
{
  Hooks hk;
  ASSERT_TRUE(hk.hooks && hk.reset);
  const nr_td_cb0_backend_t *ad = nr_td_cb0_gpu_backend_adapter();
  ASSERT_NE(ad, nullptr);
  Fx *b = big();
  Set set;
  build(set, {b}, 1);
  const int n = (int)set.it.size();
  std::vector<nr_td_cb0_result_t> cpu(n), out(n);
  nr_td_cb0_batch_cpu(set.it.data(), n, cpu.data());
  hk.reset();
  ASSERT_EQ(ad->decode(ad->ctx, set.it.data(), n, out.data()), 0);
  const void *l0 = nullptr, *l1 = nullptr, *l2 = nullptr, *l3 = nullptr;
  unsigned long ab0 = 0, ab1 = 0, ab2 = 0, ab3 = 0;
  nr_td_cb0_gpu_adapter_test_scratch(&l0, &ab0);
  ASSERT_NE(l0, nullptr);
  /* TIMEOUT: stall the stream 300 ms with a 30 ms deadline */
  hk.hooks(2, 300, 30, 100, 1000);
  EXPECT_NE(ad->decode(ad->ctx, set.it.data(), n, out.data()), 0);
  hk.hooks(0, 0, 2000, 4, 5000);
  nr_td_cb0_gpu_adapter_test_scratch(&l1, &ab1);
  EXPECT_EQ(l1, nullptr) << "abandoned after the timeout";
  EXPECT_EQ(ab1, ab0 + 1);
  for (int i = 0; i < n; i++)
    EXPECT_EQ(out[i].pass, -1) << i;
  hk.reset();
  usleep(400000); /* the stalled submission drains (it may still read the abandoned buffer meanwhile) */
  ASSERT_EQ(ad->decode(ad->ctx, set.it.data(), n, out.data()), 0);
  nr_td_cb0_gpu_adapter_test_scratch(&l2, &ab2);
  ASSERT_NE(l2, nullptr);
  EXPECT_NE(l2, l0) << "a fresh buffer: the abandoned one is never freed, so its address cannot come back";
  for (int i = 0; i < n; i++)
    EXPECT_EQ(out[i].pass, cpu[i].pass) << i;
  /* CUDA error: abandoned too */
  hk.hooks(1, 0, 0, 100, 1000);
  EXPECT_NE(ad->decode(ad->ctx, set.it.data(), n, out.data()), 0);
  hk.hooks(0, 0, 2000, 4, 5000);
  nr_td_cb0_gpu_adapter_test_scratch(&l3, &ab3);
  EXPECT_EQ(l3, nullptr);
  EXPECT_EQ(ab3, ab2 + 1);
  hk.reset();
  /* breaker open (N = 1 failure trips it): the next submit is rejected before any GPU work -> the buffer is kept */
  ASSERT_EQ(ad->decode(ad->ctx, set.it.data(), n, out.data()), 0);
  hk.hooks(1, 0, 0, 1, 60000);
  EXPECT_NE(ad->decode(ad->ctx, set.it.data(), n, out.data()), 0); /* CUDA error, trips the breaker: abandoned */
  hk.hooks(0, 0, 2000, 1, 60000);
  EXPECT_NE(ad->decode(ad->ctx, set.it.data(), n, out.data()), 0) << "breaker open: rejected";
  const void *lb = nullptr, *lb2 = nullptr;
  unsigned long abb = 0, abb2 = 0;
  nr_td_cb0_gpu_adapter_test_scratch(&lb, &abb);
  ASSERT_NE(lb, nullptr);
  EXPECT_NE(ad->decode(ad->ctx, set.it.data(), n, out.data()), 0) << "breaker open: rejected";
  nr_td_cb0_gpu_adapter_test_scratch(&lb2, &abb2);
  EXPECT_EQ(lb2, lb) << "a rejected (never enqueued) submission keeps the buffer";
  EXPECT_EQ(abb2, abb);
  hk.hooks(0, 0, 2000, 4, 5000);
  hk.reset();
  ASSERT_EQ(ad->decode(ad->ctx, set.it.data(), n, out.data()), 0);
  delete b;
}

/* Dominance: a CUDA CB0 batch is admissible against the CPU TB decoder the wiring forces while acquiring, and against a
 * CUDA TB; a CPU CB0 against a CUDA TB is not (decoder reason). */
TEST_F(Cb0Gpu, DominanceRuleAdmitsCudaCb0OverCpuTb)
{
  nr_td_cb0_adm_in_t in;
  memset(&in, 0, sizeof(in));
  in.nl = 1;
  in.rank_max = 4;
  in.cb0_decoder = NR_TD_CB0_DEC_CUDA_FLOODING;
  in.tb_decoder = 1; /* NRLDPC_DECODER_CPU */
  EXPECT_EQ(nr_td_cb0_admissibility(&in) & (1u << NR_TD_CB0_R_DECODER), 0u);
  in.tb_decoder = 2;
  EXPECT_EQ(nr_td_cb0_admissibility(&in) & (1u << NR_TD_CB0_R_DECODER), 0u);
  in.cb0_decoder = NR_TD_CB0_DEC_CPU_LAYERED;
  EXPECT_NE(nr_td_cb0_admissibility(&in) & (1u << NR_TD_CB0_R_DECODER), 0u);
  /* GPU iterations: G1's TB rule by default */
  EXPECT_EQ(nr_td_cb0_gpu_iters(8), 16);
}

} // namespace
