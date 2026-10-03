/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* K34 safety tests for the CUDA LDPC shared pool (libldpc_cuda.so, dlopen'd like nr-uesoftmodem does).
 * Needs a CUDA device. Test codewords are the all-zero codeword (valid for every LDPC code, and its CRC is
 * zero so it passes CRC): "good" TB = +40 LLRs everywhere except the 2*Z punctured ones; "noise" TB = random
 * LLRs that cannot decode. A stale all-zero result left in a reused slot therefore looks like a CRC PASS --
 * exactly the false pass K34 describes -- so a noise TB must never pass. */
#include <gtest/gtest.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vector>
#include "ldpc_cuda_pool_test_helper.h"
#include <string>

enum { NRLDPC_DECODER_CPU_LAYERED = LCP_CPU, NRLDPC_DECODER_CUDA_FLOODING = LCP_CUDA };

namespace {
const uint32_t BG = 2, Z = 96, K = 960, E = 4800, KC = 52;
const uint32_t IN_STRIDE = 68 * 384, BITS_STRIDE = (68 * 384 + 7) / 8;

void fill_good(int8_t* slot) { /* BG2 Z=96 slot: Kc*Z LLRs, first 2Z punctured = 0 */
  memset(slot, 0, IN_STRIDE);
  memset(slot + 2 * Z, 40, KC * Z - 2 * Z);
}
void fill_noise(int8_t* slot) {
  memset(slot, 0, IN_STRIDE);
  for (uint32_t i = 2 * Z; i < KC * Z; i++) slot[i] = (int8_t)((rand() % 81) - 40);
}

struct Counters { uint64_t err, fb, poi; };
Counters ctr() { Counters c; lcp_counters(&c.err, &c.fb, &c.poi); return c; }

struct Result { std::vector<bool> ok; uint8_t decoder_used; };
Result run_tb(const std::vector<bool>& noise) {
  const int C = (int)noise.size();
  std::vector<uint8_t> nz(C), ok(C);
  for (int r = 0; r < C; r++) nz[r] = noise[r];
  Result res;
  EXPECT_EQ(lcp_run_tb(C, nz.data(), ok.data(), &res.decoder_used), 0);
  for (int r = 0; r < C; r++) res.ok.push_back(ok[r]);
  return res;
}

class PoolTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() { ASSERT_EQ(lcp_load(), 0) << dlerror(); }
  void TearDown() override {
    unsetenv("LDPC_CUDA_TEST_SKIP_LAUNCH");
    unsetenv("LDPC_CUDA_TEST_STALL_MS");
    unsetenv("LDPC_CUDA_TEST_QUEUE_CAP");
    usleep(400000); /* let an abandoned stalled request drain before the next test */
  }
};

TEST_F(PoolTest, HealthyPathUsesCudaAndDecodes) {
  const Counters b = ctr();
  const Result r = run_tb({false, true, false});
  EXPECT_EQ(r.decoder_used, NRLDPC_DECODER_CUDA_FLOODING);
  EXPECT_TRUE(r.ok[0]); EXPECT_FALSE(r.ok[1]); EXPECT_TRUE(r.ok[2]);
  const Counters a = ctr();
  EXPECT_EQ(a.err, b.err); EXPECT_EQ(a.fb, b.fb); EXPECT_EQ(a.poi, b.poi);
}

/* (a) a skipped launch after a successful decode must never turn into a pass of stale bits */
TEST_F(PoolTest, SkippedLaunchNeverPasses) {
  const Result A = run_tb({false});
  ASSERT_TRUE(A.ok[0]);
  /* pool level: slot 0 still holds A's all-zero (CRC-valid) bits; a skipped launch must poison, not keep them */
  memset(lcp_host_bits(), 0, BITS_STRIDE);
  fill_noise(lcp_host_llr());
  setenv("LDPC_CUDA_TEST_SKIP_LAUNCH", "1", 1);
  int rc_req = 0;
  const Counters b = ctr();
  EXPECT_NE(lcp_pool_decode(BG, Z, 20, 0, 1, K, &rc_req), 0);
  EXPECT_NE(rc_req, 0);
  for (uint32_t i = 0; i < K / 8; i++) ASSERT_EQ(lcp_host_bits()[i], 0xA5) << "slot not poisoned at byte " << i;
  EXPECT_EQ(ctr().poi, b.poi + 1);
  /* TB level: B (noise) must FAIL; and a GOOD TB must still be correctly decoded via the CPU fallback */
  const Result B = run_tb({true});
  EXPECT_FALSE(B.ok[0]);
  EXPECT_EQ(B.decoder_used, NRLDPC_DECODER_CPU_LAYERED);
  const Result G = run_tb({false});
  EXPECT_TRUE(G.ok[0]);
  EXPECT_EQ(G.decoder_used, NRLDPC_DECODER_CPU_LAYERED);
  const Counters a = ctr();
  EXPECT_GE(a.err, b.err + 3); EXPECT_GE(a.fb, b.fb + 2);
}

/* (b) 600 code blocks in ONE request are split over launches, every block decoded in place */
TEST_F(PoolTest, OversizeRequestIsSplit) {
  const uint32_t n = 600;
  int8_t* llr = lcp_host_llr();
  uint8_t* bits = lcp_host_bits();
  memset(bits, 0xFF, (size_t)n * BITS_STRIDE);
  for (uint32_t i = 0; i < n; i++) (i % 3 == 1 ? fill_noise : fill_good)(llr + (size_t)i * IN_STRIDE);
  int rc_req = -1;
  EXPECT_EQ(lcp_pool_decode(BG, Z, 20, 0, n, K, &rc_req), 0);
  EXPECT_EQ(rc_req, 0);
  for (uint32_t i = 0; i < n; i++) {
    const uint8_t* b = bits + (size_t)i * BITS_STRIDE;
    if (i % 3 == 1) { /* noise: must not decode to the zero codeword */
      bool zero = true;
      for (uint32_t j = 0; j < K / 8; j++) zero &= b[j] == 0;
      EXPECT_FALSE(zero) << "noise block " << i;
    } else {
      for (uint32_t j = 0; j < K / 8; j++) ASSERT_EQ(b[j], 0) << "block " << i << " byte " << j;
    }
  }
}

/* (c) queue full -> CPU */
TEST_F(PoolTest, QueueFullFallsBackToCpu) {
  setenv("LDPC_CUDA_TEST_QUEUE_CAP", "0", 1);
  const Counters b = ctr();
  const Result r = run_tb({false, true});
  EXPECT_TRUE(r.ok[0]); EXPECT_FALSE(r.ok[1]);
  EXPECT_EQ(r.decoder_used, NRLDPC_DECODER_CPU_LAYERED);
  EXPECT_EQ(ctr().fb, b.fb + 1);
}

/* (d) stalled GPU worker -> timeout -> CPU, and the pool keeps working afterwards */
TEST_F(PoolTest, TimeoutFallsBackToCpu) {
  setenv("LDPC_CUDA_TEST_STALL_MS", "200", 1);
  const Counters b = ctr();
  const Result r = run_tb({false, true});
  EXPECT_TRUE(r.ok[0]); EXPECT_FALSE(r.ok[1]);
  EXPECT_EQ(r.decoder_used, NRLDPC_DECODER_CPU_LAYERED);
  EXPECT_EQ(ctr().fb, b.fb + 1);
  unsetenv("LDPC_CUDA_TEST_STALL_MS");
  usleep(500000);
  const Result again = run_tb({false});
  EXPECT_TRUE(again.ok[0]);
  EXPECT_EQ(again.decoder_used, NRLDPC_DECODER_CUDA_FLOODING);
}
}  // namespace
