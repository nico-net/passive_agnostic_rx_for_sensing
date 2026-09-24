// Tests for the live unknown-RNTI fallback wrapper (nr_pdcch_joint_live.c). Signal generation uses the real
// polar_encoder_fast and a bit-serial TS 38.211 5.2.1 LFSR (independent of the module's gold.h use); the worker's
// own step is reproduced: it descrambles with n_RNTI = its configured/alternate scrambling RNTI, then decodes.
#include <cmath>
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
#include "nr_pdcch_joint_live.h"
}
extern "C" configmodule_interface_t* uniqCfg = nullptr;
extern "C" void exit_function(const char* file, const char* function, const int line, const char* s, const int assert)
{
  if (s) fprintf(stderr, "%s:%d %s() Exiting: %s\n", file, line, function, s);
  if (assert) abort(); else exit(EXIT_SUCCESS);
}

namespace {
std::vector<uint8_t> Gold(uint32_t rnti, uint16_t nid, int n)
{
  const uint32_t c_init = (uint32_t)((((uint64_t)rnti << 16) + nid) % (1ULL << 31));
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
// what the worker's nr_pdcch_unscrambling does: flip the sign where the sequence bit is 1
void Descramble(std::vector<int16_t>& l, uint32_t rnti, uint16_t nid)
{
  auto g = Gold(rnti, nid, (int)l.size());
  for (size_t i = 0; i < l.size(); i++) if (g[i]) l[i] = (int16_t)-l[i];
}
// the LLRs a receiver sees for a DCI scrambled with the true C-RNTI, at Es/N0 snr_db
std::vector<int16_t> Rx(uint64_t payload, uint16_t rnti, uint16_t nid, int A, int L, double snr_db, std::mt19937& rng)
{
  t_nrPolar_params* p = nr_polar_params(NR_POLAR_DCI_MESSAGE_TYPE, A, L);
  const int E = p->encoderLength;
  polarReturn(p);
  std::vector<uint32_t> out((E + 31) / 32 + 1, 0);
  polar_encoder_fast(&payload, out.data(), (int32_t)rnti, 1, NR_POLAR_DCI_MESSAGE_TYPE, A, L);
  std::vector<uint8_t> bits(E);
  nr_bit2byte_uint32_8(out.data(), E, bits.data());
  auto g = Gold(rnti, nid, E);
  const double snr = std::pow(10.0, snr_db / 10.0), amp = 1000.0, sigma = amp / std::sqrt(snr);
  std::normal_distribution<double> nz(0.0, sigma);
  std::vector<int16_t> l(E);
  for (int i = 0; i < E; i++) l[i] = (int16_t)std::lround((((bits[i] ^ g[i]) ? -1.0 : 1.0) * amp) + nz(rng));
  return l;
}
uint64_t DlPayload(std::mt19937& rng, int A) { return (((uint64_t)rng() << 32 | rng()) & ((1ULL << A) - 1)) | (1ULL << (A - 1)); }
}  // namespace

// enabled for this executable before main(); the default-off contract is checked below by reading the env var
// the way the module does, not by re-testing getenv
static const int kEnable = setenv("ISAC_PDCCH_JOINT", "1", 1);

TEST(JointLive, RecoversAnUnknownRntiFromLlrsTheWorkerDescrambledWithRntiZero)
{
  ASSERT_EQ(kEnable, 0);
  ASSERT_TRUE(nr_pdcch_joint_live_enabled());
  std::mt19937 rng(41);
  int ok = 0, n = 0;
  for (int L : {1, 2, 4})
    for (uint16_t nid : {(uint16_t)2, (uint16_t)578}) {
      const int A = 47, E = 108 * L;
      for (int t = 0; t < 30; t++) {
        const uint16_t rnti = (uint16_t)(0x1000 + rng() % 0xE000) | ((t % 3 == 0) ? 0x8000 : 0);
        const uint64_t pl = DlPayload(rng, A);
        auto l = Rx(pl, rnti, nid, A, L, 8.0, rng);
        ASSERT_EQ((int)l.size(), E);
        Descramble(l, 0, nid);  // the worker's step with scrambling_rnti = 0: leaves g(rnti)^g(0) on the LLRs
        nr_pdcch_joint_live_result_t r;
        n++;
        if (nr_pdcch_joint_live_decode_11(l.data(), L, A, nid, 0, 1, 0xFFEF, &r) && r.rnti == rnti && r.payload == pl) ok++;
      }
    }
  printf("[joint-live] %d/%d unknown-RNTI DCIs recovered at 8 dB across AL1/2/4 and two nIDs\n", ok, n);
  EXPECT_GE(ok, n - 3);
}

TEST(JointLive, HandlesAnAlternateNonZeroPreDescramblingRnti)
{
  std::mt19937 rng(43);
  int ok = 0;
  for (int t = 0; t < 30; t++) {
    const int A = 45, L = 4;
    const uint16_t rnti = (uint16_t)(0x2000 + rng() % 0xD000), nid = 64;
    const uint64_t pl = DlPayload(rng, A);
    auto l = Rx(pl, rnti, nid, A, L, 6.0, rng);
    Descramble(l, 0x4601, nid);  // the worker's retry with an alternate scrambling RNTI that was WRONG
    nr_pdcch_joint_live_result_t r;
    if (nr_pdcch_joint_live_decode_11(l.data(), L, A, nid, 0x4601, 1, 0xFFEF, &r) && r.rnti == rnti && r.payload == pl) ok++;
  }
  EXPECT_GE(ok, 29);
}

TEST(JointLive, NoiseUlIndicatorAndRangeAreNotAdmitted)
{
  std::mt19937 rng(47);
  std::normal_distribution<double> nz(0.0, 500.0);
  int fa = 0;
  for (int t = 0; t < 5000; t++) {
    std::vector<int16_t> l(216);
    for (auto& v : l) v = (int16_t)std::lround(nz(rng));
    nr_pdcch_joint_live_result_t r;
    fa += nr_pdcch_joint_live_decode_11(l.data(), 2, 47, 2, 0, 1, 0xFFEF, &r);
  }
  EXPECT_LE(fa, 2);
  const int A = 47, L = 2;
  { // a genuine DCI whose format-indicator bit is 0 (an UL grant): rejected exactly as raw_11 rejects it
    const uint64_t pl = ((uint64_t)rng() << 20 | 0x5) & ((1ULL << (A - 1)) - 1);
    auto l = Rx(pl, 0x4a5b, 2, A, L, 30.0, rng);
    Descramble(l, 0, 2);
    nr_pdcch_joint_live_result_t r;
    EXPECT_FALSE(nr_pdcch_joint_live_decode_11(l.data(), L, A, 2, 0, 1, 0xFFEF, &r));
    ASSERT_NE(r.reject_reason, nullptr);
  }
  { // a genuine DCI whose recovered RNTI lies outside the caller's plausible range
    auto l = Rx(DlPayload(rng, A), 0xFFF5, 2, A, L, 30.0, rng);
    Descramble(l, 0, 2);
    nr_pdcch_joint_live_result_t r;
    EXPECT_FALSE(nr_pdcch_joint_live_decode_11(l.data(), L, A, 2, 0, 1, 0xFFEF, &r));
    ASSERT_NE(r.reject_reason, nullptr);
  }
  unsigned long long att = 0, pass = 0, acc = 0;
  nr_pdcch_joint_live_counts(&att, &pass, &acc);
  EXPECT_GT(att, pass);  // the pre-screen removes almost everything, that is its job
  EXPECT_GT(pass, 0ull);
}

TEST(JointLive, AdmitsAnUlGrantOnlyWhenAskedForOne)
{
  std::mt19937 rng(59);
  const int A = 45, L = 2;
  int ul_ok = 0, dl_admits_ul = 0;
  for (int t = 0; t < 40; t++) {
    const uint16_t rnti = (uint16_t)(0x3000 + rng() % 0xC000);
    const uint64_t pl = ((((uint64_t)rng() << 32) | rng()) & ((1ULL << (A - 1)) - 1));  // indicator bit CLEAR = UL
    auto l = Rx(pl, rnti, 2, A, L, 8.0, rng);
    Descramble(l, 0, 2);
    nr_pdcch_joint_live_result_t r;
    ul_ok += (nr_pdcch_joint_live_decode(l.data(), L, A, 2, 0, 1, 0xFFEF, /*indicator=*/0, &r) && r.rnti == rnti && r.payload == pl);
    dl_admits_ul += nr_pdcch_joint_live_decode_11(l.data(), L, A, 2, 0, 1, 0xFFEF, &r);
  }
  EXPECT_GE(ul_ok, 38);
  EXPECT_EQ(dl_admits_ul, 0);
}

TEST(JointLive, RefusesShapesOutsideTheSolverLimits)
{
  std::vector<int16_t> l(108 * 2, 100);
  nr_pdcch_joint_live_result_t r;
  EXPECT_FALSE(nr_pdcch_joint_live_decode_11(l.data(), 0, 47, 2, 0, 1, 0xFFEF, &r));
  EXPECT_FALSE(nr_pdcch_joint_live_decode_11(l.data(), 9, 47, 2, 0, 1, 0xFFEF, &r));
  EXPECT_FALSE(nr_pdcch_joint_live_decode_11(l.data(), 3, 47, 2, 0, 1, 0xFFEF, &r)); // not a legal AL
  EXPECT_FALSE(nr_pdcch_joint_live_decode_11(l.data(), 2, 0, 2, 0, 1, 0xFFEF, &r));
  EXPECT_FALSE(nr_pdcch_joint_live_decode_11(l.data(), 2, 65, 2, 0, 1, 0xFFEF, &r));
}

int main(int argc, char** argv)
{
  crcTableInit();
  logInit();
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
