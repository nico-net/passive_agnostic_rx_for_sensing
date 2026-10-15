#include <gtest/gtest.h>
#include <cstring>
#include <cstdlib>
#include <cuda_runtime.h>
#include <vector>
extern "C" {
#include "PHY/TOOLS/nr_gpu_mem.h"
}
#include "nr_gpu_pass_fixture.h"
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
  else EXPECT_EQ(-2, rc0) << "LDPC may only refuse with -2 (CC < 9)";
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
