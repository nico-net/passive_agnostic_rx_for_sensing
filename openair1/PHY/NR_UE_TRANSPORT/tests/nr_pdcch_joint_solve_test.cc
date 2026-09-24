// Offline tests for nr_pdcch_joint_solve. Ground truth = the REAL gNB encoder (polar_encoder_fast) and the
// REAL polar decoder (polar_decoder_int16) as an independent oracle; scrambling truth = a bit-serial TS 38.211
// 5.2.1 LFSR that shares no code with the module (whose scrambling uses openair1/PHY/gold.h).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
#include <gtest/gtest.h>

extern "C" {
void crcTableInit(void);
#include "PHY/CODING/nrPolar_tools/nr_polar_dci_defs.h"
#include "PHY/CODING/nrPolar_tools/nr_polar_defs.h"
#include "common/config/config_userapi.h"
#include "common/utils/LOG/log.h"
#include "nr_pdcch_joint_solve.h"
}

extern "C" configmodule_interface_t* uniqCfg = nullptr;
extern "C" void exit_function(const char* file, const char* function, const int line, const char* s, const int assert)
{
  if (s != nullptr) fprintf(stderr, "%s:%d %s() Exiting: %s\n", file, line, function, s);
  if (assert) abort(); else exit(EXIT_SUCCESS);
}

namespace {
struct EncCtx { int A; int L; };

int RealEncode(void* vctx, uint64_t payload, uint16_t crc_mask, uint8_t* coded, int E)
{
  const EncCtx* c = static_cast<EncCtx*>(vctx);
  std::vector<uint32_t> out((E + 31) / 32 + 1, 0);
  polar_encoder_fast(&payload, out.data(), (int32_t)crc_mask, /*ones_flag=*/1, NR_POLAR_DCI_MESSAGE_TYPE, c->A, c->L);
  nr_bit2byte_uint32_8(out.data(), E, coded);
  return 0;
}

int EncLen(int A, int L)
{
  t_nrPolar_params* p = nr_polar_params(NR_POLAR_DCI_MESSAGE_TYPE, A, L);
  const int e = p->encoderLength;
  polarReturn(p);
  return e;
}

// Independent bit-serial Gold sequence, TS 38.211 5.2.1, c_init taken mod 2^31 as dci_nr.c:1073 does.
std::vector<uint8_t> RefScramble(uint32_t rnti_for_scrambling, uint16_t nid, int n)
{
  const uint32_t c_init = (uint32_t)((((uint64_t)rnti_for_scrambling << 16) + nid) % (1ULL << 31));
  const int Nc = 1600, total = Nc + n;
  std::vector<uint8_t> x1(total + 31, 0), x2(total + 31, 0);
  x1[0] = 1;
  for (int i = 0; i < 31; i++) x2[i] = (c_init >> i) & 1;
  for (int i = 0; i < total; i++) {
    x1[i + 31] = x1[i + 3] ^ x1[i];
    x2[i + 31] = x2[i + 3] ^ x2[i + 2] ^ x2[i + 1] ^ x2[i];
  }
  std::vector<uint8_t> c(n);
  for (int i = 0; i < n; i++) c[i] = x1[Nc + i] ^ x2[Nc + i];
  return c;
}

// tx scrambled bits -> BPSK (bit0 = +1/sqrt2) + AWGN at Es/N0 snr_db -> int16 LLR (>0 => bit 0)
std::vector<int16_t> Channel(const std::vector<uint8_t>& bits, double snr_db, std::mt19937& rng)
{
  const double snr = std::pow(10.0, snr_db / 10.0), sigma = 1.0 / std::sqrt(2.0 * snr);
  std::normal_distribution<double> nz(0.0, sigma);
  std::vector<int16_t> l(bits.size());
  for (size_t i = 0; i < bits.size(); i++) {
    const double rx = (bits[i] ? -1.0 : 1.0) / std::sqrt(2.0) + nz(rng);
    l[i] = (int16_t)std::max(-30000.0, std::min(30000.0, rx * 64.0));
  }
  return l;
}

std::vector<uint8_t> TxBits(uint64_t payload, uint16_t rnti, uint16_t nid, int A, int L, bool swr)
{
  const int E = EncLen(A, L);
  std::vector<uint8_t> coded(E);
  EncCtx c{A, L};
  RealEncode(&c, payload, rnti, coded.data(), E);
  auto s = RefScramble(swr ? rnti : 0, nid, E);
  for (int i = 0; i < E; i++) coded[i] ^= s[i];
  return coded;
}
uint64_t Mask(int A) { return A == 64 ? ~0ULL : ((1ULL << A) - 1); }
}  // namespace

TEST(JointSolve, ModelIsAffineAndFullRankAtDeployedShapes)
{
  for (int A : {38, 45, 47, 48, 55})
    for (int L : {1, 2, 4}) {
      EncCtx c{A, L};
      auto* m = nr_pdcch_joint_model_new(RealEncode, &c, A, EncLen(A, L), 2, 1);
      ASSERT_NE(m, nullptr) << "A=" << A << " L=" << L << " (affine check or rank failed)";
      EXPECT_EQ(nr_pdcch_joint_model_unknowns(m), A + 16);
      nr_pdcch_joint_model_free(m);
    }
}

TEST(JointSolve, RejectsANonAffineEncoder)
{
  auto bad = [](void*, uint64_t p, uint16_t, uint8_t* coded, int E) -> int {
    for (int i = 0; i < E; i++) coded[i] = (uint8_t)(((p >> (i % 40)) & 1) & ((p >> ((i + 3) % 40)) & 1)); // AND: not affine
    return 0;
  };
  EXPECT_EQ(nr_pdcch_joint_model_new(bad, nullptr, 40, 108, 2, 1), nullptr);
}

TEST(JointSolve, NoiselessRecoveryOfPayloadAndAllSixteenRntiBits)
{
  std::mt19937 rng(7);
  int n = 0, ok = 0;
  for (int A : {45, 47})
    for (int L : {1, 2, 4}) {
      EncCtx c{A, L};
      const int E = EncLen(A, L);
      for (uint16_t nid : {(uint16_t)2, (uint16_t)64, (uint16_t)578, (uint16_t)65535}) {
        auto* m = nr_pdcch_joint_model_new(RealEncode, &c, A, E, nid, 1);
        ASSERT_NE(m, nullptr);
        for (int t = 0; t < 40; t++) {
          const uint64_t pl = ((uint64_t)rng() << 32 | rng()) & Mask(A);
          uint16_t rnti = (uint16_t)rng();
          if (t % 4 == 0) rnti |= 0x8000;  // bit 15: invisible to scrambling, only the CRC mask carries it
          auto llr = Channel(TxBits(pl, rnti, nid, A, L, true), 40.0, rng);
          nr_pdcch_joint_result_t r;
          nr_pdcch_joint_solve(m, llr.data(), 1, &r);
          n++;
          if (r.payload == pl && r.rnti == rnti && r.accepted) ok++;
          else ADD_FAILURE() << "A=" << A << " L=" << L << " nid=" << nid << " rnti=0x" << std::hex << rnti
                             << " got rnti=0x" << r.rnti << " payload ok=" << (r.payload == pl)
                             << " corr=" << std::dec << r.corr << " thr=" << r.threshold;
        }
        nr_pdcch_joint_model_free(m);
      }
    }
  EXPECT_EQ(ok, n);
}

TEST(JointSolve, WrongNidIsNotAccepted)
{
  std::mt19937 rng(11);
  const int A = 47, L = 2;
  EncCtx c{A, L};
  auto* m = nr_pdcch_joint_model_new(RealEncode, &c, A, EncLen(A, L), /*assumed*/ 3, 1);
  ASSERT_NE(m, nullptr);
  int acc = 0;
  for (int t = 0; t < 50; t++) {
    auto llr = Channel(TxBits((uint64_t)rng() & Mask(A), (uint16_t)rng(), /*true*/ 2, A, L, true), 40.0, rng);
    nr_pdcch_joint_result_t r;
    acc += nr_pdcch_joint_solve(m, llr.data(), 2, &r);
  }
  EXPECT_EQ(acc, 0);
  nr_pdcch_joint_model_free(m);
}

TEST(JointSolve, PureNoiseFalseAcceptRate)
{
  std::mt19937 rng(13);
  std::normal_distribution<double> nz(0.0, 20.0);
  for (int L : {1, 2, 4}) {
    const int A = 47;
    EncCtx c{A, L};
    const int E = EncLen(A, L);
    auto* m = nr_pdcch_joint_model_new(RealEncode, &c, A, E, 2, 1);
    ASSERT_NE(m, nullptr);
    int fa = 0;
    const int N = 3000;
    for (int t = 0; t < N; t++) {
      std::vector<int16_t> l(E);
      for (auto& v : l) v = (int16_t)nz(rng);
      nr_pdcch_joint_result_t r;
      fa += nr_pdcch_joint_solve(m, l.data(), 2, &r);
    }
    printf("[noise] AL%d order2: %d false accepts / %d (design PFA 1e-4)\n", L, fa, N);
    EXPECT_LE(fa, 3) << "AL" << L;
    nr_pdcch_joint_model_free(m);
  }
}

// The solver's answer must agree with the REAL receiver decode path: descramble the same LLRs with the
// recovered RNTI and let polar_decoder_int16 (which recovers the RNTI from the CRC by itself) decode them.
TEST(JointSolve, AgreesWithTheRealPolarDecoderOnNoisyInput)
{
  std::mt19937 rng(17);
  const int A = 47, L = 2;
  EncCtx c{A, L};
  const int E = EncLen(A, L);
  auto* m = nr_pdcch_joint_model_new(RealEncode, &c, A, E, 2, 1);
  ASSERT_NE(m, nullptr);
  int checked = 0, agree = 0;
  for (int t = 0; t < 200; t++) {
    const uint64_t pl = ((uint64_t)rng() << 32 | rng()) & Mask(A);
    const uint16_t rnti = (uint16_t)rng();
    auto llr = Channel(TxBits(pl, rnti, 2, A, L, true), 7.0, rng);
    nr_pdcch_joint_result_t r;
    if (!nr_pdcch_joint_solve(m, llr.data(), 2, &r)) continue;
    auto s = RefScramble(r.rnti, 2, E);
    std::vector<int16_t> d(llr);
    for (int i = 0; i < E; i++) if (s[i]) d[i] = (int16_t)-d[i];
    for (auto& v : d) v = (int16_t)std::max(-127.0, std::min(127.0, v / 8.0));
    uint64_t est[2] = {0, 0};
    const uint32_t crc = polar_decoder_int16(d.data(), est, 8, NR_POLAR_DCI_MESSAGE_TYPE, A, L);
    checked++;
    if (crc == r.rnti && est[0] == r.payload) agree++;
  }
  printf("[oracle] accepted %d of 200 at 7 dB; real decoder agrees on %d of them\n", checked, agree);
  EXPECT_GT(checked, 20);
  EXPECT_EQ(agree, checked);
  nr_pdcch_joint_model_free(m);
}

TEST(JointSolve, SuccessVersusSnrAndCost)
{
  std::mt19937 rng(19);
  for (int L : {1, 2, 4}) {
    const int A = 47;
    EncCtx c{A, L};
    const int E = EncLen(A, L);
    auto t0 = std::chrono::steady_clock::now();
    auto* m = nr_pdcch_joint_model_new(RealEncode, &c, A, E, 2, 1);
    auto t1 = std::chrono::steady_clock::now();
    ASSERT_NE(m, nullptr);
    printf("[cost] AL%d E=%d K=%d model build %.2f ms\n", L, E, A + 16,
           std::chrono::duration<double, std::milli>(t1 - t0).count());
    for (double snr : {2.0, 4.0, 6.0, 8.0, 10.0})
      for (int order : {0, 1, 2}) {
        int good = 0;
        const int N = 150;
        std::vector<std::vector<int16_t>> ls;
        std::vector<std::pair<uint64_t, uint16_t>> tr;
        for (int t = 0; t < N; t++) {
          const uint64_t pl = ((uint64_t)rng() << 32 | rng()) & Mask(A);
          const uint16_t rn = (uint16_t)rng();
          tr.push_back({pl, rn});
          ls.push_back(Channel(TxBits(pl, rn, 2, A, L, true), snr, rng));
        }
        auto s0 = std::chrono::steady_clock::now();
        for (int t = 0; t < N; t++) {
          nr_pdcch_joint_result_t r;
          nr_pdcch_joint_solve(m, ls[t].data(), order, &r);
          if (r.accepted && r.payload == tr[t].first && r.rnti == tr[t].second) good++;
        }
        auto s1 = std::chrono::steady_clock::now();
        printf("[snr] AL%d Es/N0=%4.1f dB order%d: %3d/%d recovered+accepted, %.1f us/solve\n", L, snr, order, good, N,
               std::chrono::duration<double, std::micro>(s1 - s0).count() / N);
      }
    nr_pdcch_joint_model_free(m);
  }
}

// n_RNTI = 0 cell (scrambling does not depend on the RNTI): only the CRC mask carries it, and the joint
// solve still recovers it -- with no RNTI list at all.
TEST(JointSolve, RecoversRntiFromTheCrcMaskAloneWhenScramblingIsRntiFree)
{
  std::mt19937 rng(23);
  const int A = 45, L = 2;
  EncCtx c{A, L};
  auto* m = nr_pdcch_joint_model_new(RealEncode, &c, A, EncLen(A, L), 2, /*swr=*/0);
  ASSERT_NE(m, nullptr);
  for (int t = 0; t < 60; t++) {
    const uint64_t pl = ((uint64_t)rng() << 32 | rng()) & Mask(A);
    const uint16_t rn = (uint16_t)rng();
    auto llr = Channel(TxBits(pl, rn, 2, A, L, false), 40.0, rng);
    nr_pdcch_joint_result_t r;
    ASSERT_EQ(nr_pdcch_joint_solve(m, llr.data(), 1, &r), 1);
    EXPECT_EQ(r.rnti, rn);
    EXPECT_EQ(r.payload, pl);
  }
  nr_pdcch_joint_model_free(m);
}

TEST(JointSolve, AllZeroLlrsAreNeverAccepted)
{
  const int A = 47, L = 2;
  EncCtx c{A, L};
  const int E = EncLen(A, L);
  auto* m = nr_pdcch_joint_model_new(RealEncode, &c, A, E, 2, 1);
  ASSERT_NE(m, nullptr);
  std::vector<int16_t> z(E, 0);
  nr_pdcch_joint_result_t r;
  EXPECT_EQ(nr_pdcch_joint_solve(m, z.data(), 2, &r), 0);
  nr_pdcch_joint_model_free(m);
}

// The live worker always removes the n_RNTI = (its configured scrambling RNTI, usually 0) sequence before
// decoding, so the solver must handle input that is ALREADY partially descrambled.
TEST(JointSolve, HandlesLlrsAlreadyDescrambledWithAnotherRnti)
{
  std::mt19937 rng(29);
  const int A = 47, L = 2;
  EncCtx c{A, L};
  const int E = EncLen(A, L);
  for (int pre : {0, 0x4601}) {
    auto* m = nr_pdcch_joint_model_new_ex(RealEncode, &c, A, E, 578, 1, pre);
    ASSERT_NE(m, nullptr);
    for (int t = 0; t < 60; t++) {
      const uint64_t pl = ((uint64_t)rng() << 32 | rng()) & Mask(A);
      const uint16_t rn = (uint16_t)rng();
      auto llr = Channel(TxBits(pl, rn, 578, A, L, true), 40.0, rng);
      auto g = RefScramble((uint32_t)pre, 578, E);
      for (int i = 0; i < E; i++) if (g[i]) llr[i] = (int16_t)-llr[i];
      nr_pdcch_joint_result_t r;
      ASSERT_EQ(nr_pdcch_joint_solve(m, llr.data(), 0, &r), 1) << "pre=" << pre;
      EXPECT_EQ(r.rnti, rn);
      EXPECT_EQ(r.payload, pl);
      EXPECT_EQ(r.mismatched_bits, 0);
    }
    nr_pdcch_joint_model_free(m);
  }
}

TEST(JointSolve, PrescreenMatchesItsDerivationAndKeepsSolvableCandidates)
{
  std::mt19937 rng(31);
  std::normal_distribution<double> nz(0.0, 500.0); // large scale + rounding: int16 truncation at a small sigma biases |l|
  for (int L : {1, 2, 4}) {
    const int A = 47, E = EncLen(A, L);
    const int N = 100000;
    int pass = 0;
    double sum = 0, sum2 = 0;
    for (int t = 0; t < N; t++) {
      std::vector<int16_t> l(E);
      double a = 0, b = 0;
      for (auto& v : l) { v = (int16_t)std::lround(nz(rng)); a += std::fabs((double)v); b += (double)v * v; }
      const double r = a * a / ((double)E * b);
      sum += r; sum2 += r * r;
      pass += nr_pdcch_joint_prescreen(l.data(), E);
    }
    const double mean = sum / N, sd = std::sqrt(sum2 / N - mean * mean);
    printf("[prescreen] AL%d noise: mean r=%.4f (2/pi=%.4f) sd=%.4f (derived %.4f) pass rate %.2e (design 1e-3)\n", L,
           mean, 2.0 / M_PI, sd, 0.339 / std::sqrt((double)E), (double)pass / N);
    EXPECT_NEAR(mean, 2.0 / M_PI, 0.5 / E); // a ratio of sample means has an O(1/E) bias (measured ~0.36/E), not a formula error
    EXPECT_NEAR(sd, 0.339 / std::sqrt((double)E), 0.08 * 0.339 / std::sqrt((double)E));
    EXPECT_LE((double)pass / N, 3e-3);
    for (double snr : {2.0, 4.0, 6.0, 8.0}) {
      int kept = 0;
      const int M = 2000;
      for (int t = 0; t < M; t++) {
        auto llr = Channel(TxBits((uint64_t)rng() & Mask(A), (uint16_t)rng(), 2, A, L, true), snr, rng);
        kept += nr_pdcch_joint_prescreen(llr.data(), E);
      }
      printf("[prescreen] AL%d Es/N0=%.0f dB: %.1f%% of true PDCCHs pass\n", L, snr, 100.0 * kept / M);
      if (snr >= 8.0) EXPECT_GE(kept, (int)(0.97 * M));
    }
  }
}

int main(int argc, char** argv)
{
  crcTableInit();
  logInit();
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
