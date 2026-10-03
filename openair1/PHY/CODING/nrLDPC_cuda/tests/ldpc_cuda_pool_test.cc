/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* K34 safety tests for the CUDA LDPC shared pool (libldpc_cuda.so, dlopen'd like nr-uesoftmodem does).
 * Needs a CUDA device. Test codewords are the all-zero codeword (valid for every LDPC code, and its CRC is
 * zero so it passes CRC): "good" TB = +40 LLRs; "noise" TB = random LLRs that cannot decode. A stale all-zero
 * result left in a reused slot therefore looks like a CRC PASS -- exactly the false pass K34 describes -- so a
 * noise TB must never pass. Fault injection goes through ldpc_cuda_test_hooks(). */
#include <gtest/gtest.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <thread>
#include <vector>
#include "ldpc_cuda_pool_test_helper.h"

namespace {
const uint32_t BG = 2, Z = 96, K = 960, KC = 52;
const uint32_t IN_STRIDE = 68 * 384, BITS_STRIDE = (68 * 384 + 7) / 8;

void fill_good(int8_t* slot) { memset(slot, 0, IN_STRIDE); memset(slot + 2 * Z, 40, KC * Z - 2 * Z); }
void fill_noise(int8_t* slot) {
  memset(slot, 0, IN_STRIDE);
  for (uint32_t i = 2 * Z; i < KC * Z; i++) slot[i] = (int8_t)((rand() % 81) - 40);
}

struct Counters { uint64_t err, fb, poi, dis; };
Counters ctr() { Counters c; lcp_counters(&c.err, &c.fb, &c.poi, &c.dis); return c; }

struct Result { std::vector<bool> ok; uint8_t decoder_used; };
Result run_tb(const std::vector<bool>& noise, int bg1 = 0) {
  const int C = (int)noise.size();
  std::vector<uint8_t> nz(C), ok(C);
  for (int r = 0; r < C; r++) nz[r] = noise[r];
  Result res;
  EXPECT_EQ(lcp_run_tb(bg1, C, nz.data(), ok.data(), &res.decoder_used), 0);
  for (int r = 0; r < C; r++) res.ok.push_back(ok[r]);
  return res;
}
void wait_slots_free() {
  for (int i = 0; i < 400 && lcp_slots_used(); i++) usleep(10000);
}

class PoolTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() { ASSERT_EQ(lcp_load(), 0) << dlerror(); }
  void SetUp() override { lcp_hooks(0, 0, 0, 256, 50, 4, 5000); lcp_reset(); }
  void TearDown() override {
    lcp_hooks(0, 0, 0, 256, 50, 4, 5000);
    wait_slots_free(); EXPECT_EQ(lcp_slots_used(), 0) << "slot leak"; /* let an abandoned stalled request drain before the next test */
    lcp_reset();
  }
};

TEST_F(PoolTest, HealthyPathUsesCudaAndDecodes) {
  const Counters b = ctr();
  const Result r = run_tb({false, true, false});
  EXPECT_EQ(r.decoder_used, LCP_CUDA);
  EXPECT_TRUE(r.ok[0]); EXPECT_FALSE(r.ok[1]); EXPECT_TRUE(r.ok[2]);
  const Counters a = ctr();
  EXPECT_EQ(a.err, b.err); EXPECT_EQ(a.fb, b.fb); EXPECT_EQ(a.poi, b.poi); EXPECT_EQ(a.dis, 0u);
}

/* Critical 1: BG1 Z=384 fills a whole slot (Kc*Z == stride). The prep pack must not write into the NEXT slot. */
TEST_F(PoolTest, Bg1Z384PrepDoesNotOverrunNextSlot) {
  int8_t* next = lcp_host_llr() + IN_STRIDE; /* slot 1; a C=1 TB takes slot 0 (pool idle, first fit) */
  memset(next, 0x5A, IN_STRIDE);
  const Result r = run_tb({false}, 1);
  EXPECT_TRUE(r.ok[0]);
  EXPECT_EQ(r.decoder_used, LCP_CUDA);
  for (uint32_t i = 0; i < IN_STRIDE; i++) ASSERT_EQ((uint8_t)next[i], 0x5A) << "slot 1 clobbered at " << i;
  lcp_hooks(1, 0, 0, 256, 50, 1000, 5000); /* same TB on the CPU fallback (private scratch buffer) */
  const Result f = run_tb({false}, 1);
  EXPECT_TRUE(f.ok[0]);
  EXPECT_EQ(f.decoder_used, LCP_CPU);
}

/* (a) a skipped launch after a successful decode must never turn into a pass of stale bits */
TEST_F(PoolTest, SkippedLaunchNeverPasses) {
  const Result A = run_tb({false});
  ASSERT_TRUE(A.ok[0]);
  memset(lcp_host_bits(), 0, BITS_STRIDE); /* stale, CRC-valid all-zero bits */
  fill_noise(lcp_host_llr());
  lcp_hooks(1, 0, 0, 256, 50, 1000, 5000);
  int rc_req = 0;
  const Counters b = ctr();
  EXPECT_NE(lcp_pool_decode(BG, Z, 20, 0, 1, K, &rc_req), 0);
  EXPECT_NE(rc_req, 0);
  for (uint32_t i = 0; i < K / 8; i++) ASSERT_EQ(lcp_host_bits()[i], 0xA5) << "slot not poisoned at byte " << i;
  EXPECT_EQ(ctr().poi, b.poi + 1);
  const Result B = run_tb({true});
  EXPECT_FALSE(B.ok[0]);
  EXPECT_EQ(B.decoder_used, LCP_CPU);
  const Result G = run_tb({false});
  EXPECT_TRUE(G.ok[0]);
  EXPECT_EQ(G.decoder_used, LCP_CPU);
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
    if (i % 3 == 1) {
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
  lcp_hooks(0, 0, 0, 0, 50, 4, 5000);
  const Counters b = ctr();
  const Result r = run_tb({false, true});
  EXPECT_TRUE(r.ok[0]); EXPECT_FALSE(r.ok[1]);
  EXPECT_EQ(r.decoder_used, LCP_CPU);
  EXPECT_EQ(ctr().fb, b.fb + 1);
}

/* (d) stalled GPU worker -> timeout -> CPU; the timeout trips the breaker; the pool works after it */
TEST_F(PoolTest, TimeoutFallsBackToCpu) {
  lcp_hooks(0, 200, 0, 256, 50, 4, 300);
  const Counters b = ctr();
  const Result r = run_tb({false, true});
  EXPECT_TRUE(r.ok[0]); EXPECT_FALSE(r.ok[1]);
  EXPECT_EQ(r.decoder_used, LCP_CPU);
  EXPECT_EQ(ctr().fb, b.fb + 1);
  EXPECT_EQ(ctr().dis, 1u); /* any timeout bypasses the GPU */
  lcp_hooks(0, 0, 0, 256, 50, 4, 300);
  wait_slots_free();
  usleep(400000); /* bypass over */
  const Result again = run_tb({false});
  EXPECT_TRUE(again.ok[0]);
  EXPECT_EQ(again.decoder_used, LCP_CUDA);
  EXPECT_EQ(ctr().dis, 0u);
}

/* breaker: N consecutive errors bypass the GPU for T, then it comes back */
TEST_F(PoolTest, BreakerTripsAfterConsecutiveErrorsThenRecovers) {
  lcp_hooks(1, 0, 0, 256, 50, 4, 700);
  for (int i = 0; i < 3; i++) { run_tb({false}); EXPECT_EQ(ctr().dis, 0u) << "tripped early at " << i; }
  run_tb({false});
  EXPECT_EQ(ctr().dis, 1u);
  lcp_hooks(0, 0, 0, 256, 50, 4, 700); /* GPU healthy again, but the breaker keeps it bypassed */
  const Counters b = ctr();
  const Result r = run_tb({false});
  EXPECT_TRUE(r.ok[0]);
  EXPECT_EQ(r.decoder_used, LCP_CPU);
  EXPECT_EQ(ctr().err, b.err) << "no GPU attempt while bypassed";
  usleep(800000);
  const Result back = run_tb({false});
  EXPECT_EQ(back.decoder_used, LCP_CUDA);
  EXPECT_EQ(ctr().dis, 0u);
}

/* a success between errors resets the consecutive count */
TEST_F(PoolTest, BreakerCountsConsecutiveOnly) {
  for (int round = 0; round < 3; round++) {
    lcp_hooks(1, 0, 0, 256, 50, 4, 700);
    run_tb({false}); run_tb({false}); run_tb({false});
    lcp_hooks(0, 0, 0, 256, 50, 4, 700);
    EXPECT_EQ(run_tb({false}).decoder_used, LCP_CUDA);
  }
  EXPECT_EQ(ctr().dis, 0u);
}

/* sticky CUDA error -> permanent bypass */
TEST_F(PoolTest, StickyErrorDisablesPermanently) {
  lcp_hooks(0, 0, 3, 256, 50, 4, 300);
  const Result r = run_tb({false});
  EXPECT_TRUE(r.ok[0]);
  EXPECT_EQ(r.decoder_used, LCP_CPU);
  EXPECT_EQ(ctr().dis, 2u);
  lcp_hooks(0, 0, 0, 256, 50, 4, 300);
  usleep(500000);
  EXPECT_EQ(ctr().dis, 2u);
  EXPECT_EQ(run_tb({false}).decoder_used, LCP_CPU);
}

/* fail: path, plain CUDA error: slots poisoned, TB right via CPU, stream still usable afterwards */
TEST_F(PoolTest, InjectedCudaErrorTakesFailPathAndPoolSurvives) {
  lcp_hooks(0, 0, 1, 256, 50, 4, 5000);
  const Counters b = ctr();
  int rc_req = 0;
  fill_noise(lcp_host_llr());
  EXPECT_NE(lcp_pool_decode(BG, Z, 20, 0, 1, K, &rc_req), 0);
  EXPECT_NE(rc_req, 0);
  for (uint32_t i = 0; i < K / 8; i++) ASSERT_EQ(lcp_host_bits()[i], 0xA5);
  const Result r = run_tb({false, true});
  EXPECT_TRUE(r.ok[0]); EXPECT_FALSE(r.ok[1]);
  EXPECT_EQ(r.decoder_used, LCP_CPU);
  EXPECT_GE(ctr().err, b.err + 2);
  lcp_hooks(0, 0, 0, 256, 50, 4, 5000);
  lcp_reset();
  const Result ok = run_tb({false, true});
  EXPECT_EQ(ok.decoder_used, LCP_CUDA);
  EXPECT_TRUE(ok.ok[0]); EXPECT_FALSE(ok.ok[1]);
}

/* fail: path during graph capture (first use of a batch size): capture aborted, no stuck stream */
TEST_F(PoolTest, ErrorDuringGraphCaptureIsRecovered) {
  lcp_hooks(0, 0, 2, 256, 50, 4, 5000);
  const std::vector<bool> tb5 = {false, true, false, false, true}; /* nb = 8: a key no earlier test captured */
  const Result r = run_tb(tb5);
  EXPECT_EQ(r.decoder_used, LCP_CPU);
  EXPECT_TRUE(r.ok[0]); EXPECT_FALSE(r.ok[1]); EXPECT_TRUE(r.ok[3]);
  lcp_hooks(0, 0, 0, 256, 50, 4, 5000);
  lcp_reset();
  const Result again = run_tb(tb5);
  EXPECT_EQ(again.decoder_used, LCP_CUDA);
  EXPECT_TRUE(again.ok[0]); EXPECT_FALSE(again.ok[1]); EXPECT_TRUE(again.ok[3]);
}

/* a second caller during a stall must neither reuse the abandoned slots nor be corrupted by the late GPU write */
TEST_F(PoolTest, SecondCallerDuringStallDoesNotReuseAbandonedSlots) {
  lcp_hooks(0, 400, 0, 256, 50, 4, 5000);
  Result A;
  std::thread t([&] { A = run_tb({false, true}); });
  t.join(); /* A timed out at ~50 ms and fell back; its 2 slots are abandoned, the worker is still stalled */
  EXPECT_EQ(A.decoder_used, LCP_CPU);
  EXPECT_TRUE(A.ok[0]); EXPECT_FALSE(A.ok[1]);
  EXPECT_EQ(lcp_slots_used(), 2) << "abandoned slots must stay held until the worker is done";
  lcp_reset(); /* reopen the breaker so B tries the GPU while the worker is stalled */
  const Result B = run_tb({true, false, true});
  EXPECT_EQ(B.decoder_used, LCP_CPU);
  EXPECT_FALSE(B.ok[0]); EXPECT_TRUE(B.ok[1]); EXPECT_FALSE(B.ok[2]);
  EXPECT_EQ(lcp_slots_used(), 2) << "B took other slots (A's still held) and released its own";
  wait_slots_free();
  EXPECT_EQ(lcp_slots_used(), 0);
}
}  // namespace
