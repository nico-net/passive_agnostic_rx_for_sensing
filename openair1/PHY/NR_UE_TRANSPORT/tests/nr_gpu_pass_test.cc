#include <gtest/gtest.h>
#include <cstring>
#include <cstdlib>
#include <cuda_runtime.h>
#include <vector>
extern "C" {
#include "PHY/TOOLS/nr_gpu_mem.h"
}
#include <map>
#include "nr_gpu_pass_fixture.h"
#include "nr_gpu_platform.h"
static bool have_gpu() { return nr_gpu_model() >= 0 && getenv("NR_GPU_TEST_SKIP") == nullptr; }

TEST(GpuMem, RoundTripBothModels) {
  if (!have_gpu()) GTEST_SKIP();
  for (int force : {0, 1}) {
    setenv("ISAC_GPU_FORCE_DISCRETE", force ? "1" : "0", 1);
    nr_gpu_buf_t b;
    ASSERT_EQ(0, nr_gpu_buf_alloc(&b, 4096));
    EXPECT_EQ(force ? NR_GPU_MODEL_DISCRETE : nr_gpu_model(), b.model);
    for (int i = 0; i < 4096; i++) ((unsigned char *)b.host)[i] = (unsigned char)i;
    ASSERT_EQ(0, nr_gpu_buf_h2d_async(&b, 0, 4096, nullptr));
    memset(b.host, 0, 4096);
    ASSERT_EQ(0, nr_gpu_buf_d2h_async(&b, 0, 4096, nullptr));
    cudaDeviceSynchronize();
    if (b.model == NR_GPU_MODEL_DISCRETE) {
      for (int i = 0; i < 4096; i++) ASSERT_EQ((unsigned char)i, ((unsigned char *)b.host)[i]);
    }
    nr_gpu_buf_free(&b);
  }
}
TEST(GpuMem, AllocIsFirstTouched) {
  if (!have_gpu()) GTEST_SKIP();
  nr_gpu_buf_t b;
  ASSERT_EQ(0, nr_gpu_buf_alloc(&b, 1 << 20));
  for (size_t i = 0; i < (1u << 20); i += 4096) ASSERT_EQ(0, ((unsigned char *)b.host)[i]);
  nr_gpu_buf_free(&b);
}

/* Stage parity of the launch-only DL entry points: the same slot through FEP -> chest -> demod -> LDPC on one stream, once with the
 * buffers of the platform's own model and once forced DISCRETE (device ring + explicit copies). On GB10 the first run is DIRECT
 * (the kernels read the host IQ ring in place), on sens6 both are DISCRETE. Not compared against the OAI CPU chain: that needs the
 * PHY_NR_UE link set (see the Task 2 report); the end-to-end hard decisions are scored against the transmitted bits instead. */
static void dl_stages_case(int nrb, int nl, int qm, int seed, double max_ber) {
  GpuPassFixture fx = GpuPassFixture::small_grant(nrb, nl, qm, seed);
  std::vector<int16_t> llr_direct, llr_discrete;
  setenv("ISAC_GPU_FORCE_DISCRETE", "0", 1);
  ASSERT_EQ(0, fx.run_stages(&llr_direct));
  const auto rxF0 = fx.rxF_head; const auto chest0 = fx.chest_out; const auto snap0 = fx.snap; const int rc0 = fx.ldpc_rc;
  setenv("ISAC_GPU_FORCE_DISCRETE", "1", 1);
  ASSERT_EQ(0, fx.run_stages(&llr_discrete));
  unsetenv("ISAC_GPU_FORCE_DISCRETE");
  ASSERT_EQ(llr_direct.size(), llr_discrete.size());
  ASSERT_EQ(fx.tx_bits.size(), llr_direct.size());
  EXPECT_EQ(rxF0, fx.rxF_head) << "FEP rxdataF differs between the models";
  EXPECT_EQ(chest0, fx.chest_out) << "chest delays/nvar differ between the models";
  EXPECT_EQ(0, memcmp(llr_direct.data(), llr_discrete.data(), llr_direct.size() * 2)) << "demod LLRs differ between the models";
  EXPECT_EQ(rc0, fx.ldpc_rc);
  if (rc0 == 0) EXPECT_EQ(snap0, fx.snap) << "LDPC snapshots differ between the models";
  else ADD_FAILURE() << "LDPC refused (rc " << rc0 << "): below CC 9 the non-cluster kernels run, nothing may refuse";
  size_t err = 0;
  for (size_t i = 0; i < llr_direct.size(); i++) err += (llr_direct[i] < 0) != (fx.tx_bits[i] != 0);
  printf("nrb=%d nl=%d qm=%d: %zu LLRs, BER vs tx %.4f, LDPC rc %d nsnap %d, parity checked (model 1: %s)\n", nrb, nl, qm, llr_direct.size(),
         (double)err / llr_direct.size(), rc0, fx.ldpc_nsnap, nr_gpu_model() == NR_GPU_MODEL_DIRECT ? "DIRECT" : "DISCRETE");
  EXPECT_LT((double)err / llr_direct.size(), max_ber) << "chain does not demodulate the synthetic slot: " << err << " bit errors";
}
/* GB10 only: the Stage-1 bit-exact reference is today's GPU exact path. Same fixture inputs through the OLD entry points (own waits,
 * host buffers) and through the four _dev functions on the DIRECT model. */
static void dev_vs_old_case(int nrb, int nl, int qm, int seed) {
  GpuPassFixture fx = GpuPassFixture::small_grant(nrb, nl, qm, seed);
  std::vector<int16_t> llr_old, llr_dev;
  setenv("ISAC_GPU_FORCE_DISCRETE", "0", 1);
  ASSERT_EQ(0, fx.run_stages(&llr_old, true));
  const auto rxF0 = fx.rxF_head; const auto chest0 = fx.chest_out; const auto snap0 = fx.snap; const int rc0 = fx.ldpc_rc, ns0 = fx.ldpc_nsnap;
  ASSERT_EQ(0, fx.run_stages(&llr_dev, false));
  unsetenv("ISAC_GPU_FORCE_DISCRETE");
  EXPECT_EQ(rxF0, fx.rxF_head) << "rxdataF: old vs _dev";
  EXPECT_EQ(chest0, fx.chest_out) << "chest delay/nvar: old vs _dev";
  ASSERT_EQ(llr_old.size(), llr_dev.size());
  EXPECT_EQ(0, memcmp(llr_old.data(), llr_dev.data(), llr_old.size() * 2)) << "LLRs: old vs _dev";
  EXPECT_EQ(rc0, fx.ldpc_rc);
  EXPECT_EQ(ns0, fx.ldpc_nsnap);
  EXPECT_EQ(snap0, fx.snap) << "LDPC snapshots/syn0: old vs _dev";
}
TEST(GpuPassStages, DevEqualsOldEntryPoints) {
  if (!have_gpu()) GTEST_SKIP();
  bool ok = false, lib = false;
  GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok);
  unsetenv("ISAC_GPU_FORCE_DISCRETE"); /* GpuMem.RoundTrip leaves it at 1 */
  if (nr_gpu_model() != NR_GPU_MODEL_DIRECT) GTEST_SKIP() << "old entry points need the DIRECT model (GB10)";
  dev_vs_old_case(24, 2, 4, 7);
  dev_vs_old_case(12, 1, 6, 9);
  dev_vs_old_case(24, 4, 2, 5);
}
TEST(GpuPassStages, DlStagesSameBitsBothModels) {
  if (!have_gpu()) GTEST_SKIP();
  bool ok = false, lib = false;
  GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok) << "libpdsch_gpu.so lacks the _dev entry points or has no device";
  dl_stages_case(/*nrb=*/24, /*nl=*/2, /*qm=*/4, /*seed=*/7, 0.02);
  dl_stages_case(12, 1, 6, 9, 0.02);
  dl_stages_case(24, 4, 2, 5, 0.15); // rank 4 over a 4-RX channel is ill-conditioned (nr_pdsch_gpu_fep_test): sanity only, parity is the check
}

/* The four _dev calls of one (fep slot, chest slot) captured into a CUDA graph (Relaxed: the module calls cudaHostGetDevicePointer inside),
 * instantiated before any fence; then for three different inputs: stage on the host, replay, and compare with a plain (not captured) run of
 * the same inputs on the same buffers. Both models. */
static void graph_case(int force) {
  setenv("ISAC_GPU_FORCE_DISCRETE", force ? "1" : "0", 1);
  bool ok = false;
  GpuPassModule &m = GpuPassModule::get(&ok);
  ASSERT_TRUE(ok);
  GpuPassFixture in[3] = {GpuPassFixture::small_grant(24, 2, 4, 11), GpuPassFixture::small_grant(24, 2, 4, 12), GpuPassFixture::small_grant(24, 2, 4, 13)};
  GpuPassFixture::Pass P;
  ASSERT_TRUE(P.setup(m, in[0], /*nvar_fixed=*/400));
  std::map<std::pair<int, int>, cudaGraphExec_t> ge;
  GpuPassFixture::Out ref[3], rep[3];
  int rc_ref = 0, rc_rep = 0;
  for (int pass = 0; pass < 2; pass++) /* 0: plain reference of all three inputs, 1: graph replays */
    for (int i = 0; i < 3; i++) {
      ASSERT_TRUE(P.load(in[i]));
      int fs, cs, rc = 0;
      ASSERT_EQ(0, P.stage(&fs, &cs));
      if (pass == 0) {
        ASSERT_EQ(0, P.enqueue_dev(fs, cs, P.s, &rc));
        rc_ref = rc;
      } else {
        auto key = std::make_pair(fs, cs);
        if (!ge.count(key)) {
          cudaGraph_t g;
          ASSERT_EQ(cudaSuccess, cudaStreamBeginCapture(P.s, cudaStreamCaptureModeRelaxed));
          const int er = P.enqueue_dev(fs, cs, P.s, &rc);
          ASSERT_EQ(cudaSuccess, cudaStreamEndCapture(P.s, &g));
          ASSERT_EQ(0, er);
          ASSERT_EQ(cudaSuccess, cudaGraphInstantiate(&ge[key], g, 0));
          cudaGraphDestroy(g);
        } else
          rc = rc_ref;
        ASSERT_EQ(cudaSuccess, cudaGraphLaunch(ge[key], P.s));
        rc_rep = rc;
        EXPECT_EQ(rc_ref, rc_rep);
      }
      ASSERT_TRUE(P.fetch(pass == 0 ? &ref[i] : &rep[i], pass == 0 ? rc : rc_ref));
    }
  for (int i = 0; i < 3; i++) {
    EXPECT_TRUE(ref[i] == rep[i]) << "replayed graph differs from the plain run, input " << i << " force=" << force;
    EXPECT_FALSE(ref[i].llr == ref[(i + 1) % 3].llr) << "inputs should differ";
  }
  for (auto &e : ge) cudaGraphExecDestroy(e.second);
}
TEST(GpuPassStages, DevCapturableAndReplayable) {
  if (!have_gpu()) GTEST_SKIP();
  bool ok = false, lib = false;
  GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok);
  graph_case(0);
  graph_case(1);
  unsetenv("ISAC_GPU_FORCE_DISCRETE");
}

/* Runs last: raises the allocation fence of the platform. A (BG, Z, R) the init did not build is refused and counted, nothing is
 * allocated or built on the launch path. */
extern "C" void nr_dl_gpu_memory_stats(uint64_t *, uint64_t *);
TEST(GpuPassStages, LdpcDevNoAllocOnMiss) {
  if (!have_gpu()) GTEST_SKIP();
  bool ok = false, lib = false;
  GpuPassModule &m = GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok);
  void *dl = dlopen("./libpdsch_gpu.so", RTLD_NOW | RTLD_NOLOAD);
  auto misses = (unsigned (*)(void))dlsym(dl, "nr_pdsch_gpu_ldpc_dev_misses");
  auto stats = (void (*)(uint64_t *, uint64_t *))dlsym(dl, "nr_dl_gpu_memory_stats");
  ASSERT_TRUE(misses && stats);
  unsetenv("ISAC_GPU_FORCE_DISCRETE");
  nr_gpu_buf_t lin = {}, bits = {};
  ASSERT_EQ(0, nr_gpu_buf_alloc(&lin, NR_GPU_LDPC_DEV_IN_BYTES));
  ASSERT_EQ(0, nr_gpu_buf_alloc(&bits, NR_GPU_LDPC_DEV_BITS_BYTES));
  nr_gpu_rx_started(); /* the fence */
  uint64_t a0, d0, a1, d1;
  stats(&a0, &d0);
  const unsigned m0 = misses();
  nr_gpu_ldpc_job_t bad = {};
  bad.BG = 2; bad.R = 7; bad.Z = 64; bad.K = 640; bad.it_max = 8; /* R = 7 is no code rate of BG2: never built */
  EXPECT_EQ(-1, m.ldpc(&bad, 1, &lin, &bits, nullptr));
  EXPECT_EQ(m0 + 1, misses());
  nr_gpu_ldpc_job_t good = bad;
  good.R = 15;
  EXPECT_EQ(0, m.ldpc(&good, 1, &lin, &bits, nullptr)); /* a prepared key still launches behind the fence */
  cudaDeviceSynchronize();
  stats(&a1, &d1);
  EXPECT_EQ(a0, a1) << "the launch path allocated";
  EXPECT_EQ(d0, d1) << "an allocation was attempted behind the fence";
  EXPECT_EQ(m0 + 1, misses());
}
