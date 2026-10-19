// SPDX-License-Identifier: LicenseRef-CSSL-1.0
// Fast-SSC DCI polar decoder vs polar_decoder_int16(): decision-exact, success >= reference, large-|LLR| correct, batch == single.
#include <gtest/gtest.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <vector>
extern "C" {
#include "PHY/CODING/nrPolar_tools/nr_polar_defs.h"
#include "PHY/CODING/nrPolar_tools/nr_polar_fssc.h"
void polar_encoder_fast(uint64_t *A, void *out, int32_t crcmask, uint8_t ones_flag, int8_t messageType, uint16_t messageLength,
                        uint8_t aggregation_level);
void crcTableInit(void);
struct configmodule_interface_s *uniqCfg = NULL;
void exit_function(const char *, const char *, const int, const char *, const int) { abort(); }
}
static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd() { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng >> 11); }
static double gauss() { double u = (rnd() + 1.0) / 4294967297.0, v = (rnd() + 1.0) / 4294967297.0; return sqrt(-2 * log(u)) * cos(6.283185307 * v); }

struct Vec { std::vector<int16_t> llr; uint64_t pay[3]; uint16_t rnti; };
static Vec make(int len, int al, double amp, double sigma, double zero_frac = 0)
{
  Vec v; v.rnti = (uint16_t)(rnd() | 1);
  uint64_t A[3] = {0, 0, 0};
  for (int w = 0; w < (len + 63) / 64; w++) A[w] = ((uint64_t)rnd() << 32) ^ rnd() ^ ((uint64_t)rnd() << 11);
  if (len % 64) A[(len - 1) / 64] &= (len % 64 == 0) ? ~0ULL : ((1ULL << (len % 64)) - 1);
  memcpy(v.pay, A, sizeof A);
  uint8_t enc[2048 / 8 + 16] = {0};
  polar_encoder_fast(A, enc, v.rnti, 1, NR_POLAR_DCI_MESSAGE_TYPE, len, al);
  v.llr.resize(al * 108);
  for (int i = 0; i < al * 108; i++) {
    const int bit = (enc[i >> 3] >> (i & 7)) & 1;
    double x = (bit ? -amp : amp) + sigma * gauss();
    if (zero_frac > 0 && (rnd() % 1000) < zero_frac * 1000) x = 0;
    const double cl = amp <= 31 ? 31 : 32767; /* production clips the LLR to +-31 (dci_nr.c) */
    v.llr[i] = (int16_t)lrint(fmax(-cl, fmin(cl, x)));
  }
  return v;
}
static const int kAls[] = {1, 2, 4, 8, 16};
static std::vector<int> lens()
{
  std::vector<int> l = {12, 13, 20, 31, 32, 33, 40, 44, 47, 49, 55, 63, 64, 65, 70, 80, 99, 100, 120, 128, 139, 140};
  return l;
}
static uint32_t ref_dec(const Vec &v, int len, int al, uint64_t *out)
{
  std::vector<int16_t> t = v.llr; out[0] = out[1] = out[2] = 0;
  return polar_decoder_int16(t.data(), out, 1, NR_POLAR_DCI_MESSAGE_TYPE, len, al);
}

// sweep: every size x AL x SNR x tie density; decisions (residual and all payload words) identical to the reference
TEST(PolarFssc, DecisionsIdenticalToReference)
{
  crcTableInit();
  long n = 0, okref = 0, okf = 0;
  const double sig[] = {0.2, 0.6, 0.9, 1.2, 1.8};
  for (int len : lens())
    for (int al : kAls) {
      if (len + 24 > al * 108) continue;
      for (double sr : sig)
        for (double zf : {0.0, 0.05}) {
          std::vector<Vec> vs;
          for (int i = 0; i < 24; i++) vs.push_back(make(len, al, 20, 20 * sr, zf));
          for (auto &v : vs) {
            uint64_t ro[3], fo[3];
            const uint32_t rr = ref_dec(v, len, al, ro), rf = nr_polar_dci_decode_fssc(v.llr.data(), fo, len, al);
            ASSERT_EQ(rr, rf) << "len " << len << " AL " << al << " sr " << sr << " zf " << zf;
            for (int w = 0; w < (len + 63) / 64; w++) ASSERT_EQ(ro[w], fo[w]) << "len " << len << " AL " << al << " w " << w;
            n++; okref += rr == v.rnti && !memcmp(ro, v.pay, 8 * ((len + 63) / 64)); okf += rf == v.rnti && !memcmp(fo, v.pay, 8 * ((len + 63) / 64));
          }
          // batch (3 different group sizes) == single
          for (int nb : {1, 5, 8}) {
            const int16_t *p[8]; uint32_t res[8]; uint64_t bo[24];
            for (int j = 0; j < nb; j++) p[j] = vs[j].llr.data();
            nr_polar_dci_decode_fssc_x8(p, nb, res, bo, len, al);
            for (int j = 0; j < nb; j++) {
              uint64_t fo[3]; const uint32_t rf = nr_polar_dci_decode_fssc(p[j], fo, len, al);
              ASSERT_EQ(res[j], rf) << "batch len " << len << " AL " << al;
              for (int w = 0; w < (len + 63) / 64; w++) ASSERT_EQ(bo[3 * j + w], fo[w]);
            }
          }
        }
    }
  EXPECT_GE(okf, okref);
  EXPECT_GT(n, 1000);
}

// |LLR| >= 100: the reference int16 min-sum mis-decodes clean AL4/AL8 vectors there; the new decoder must not.
TEST(PolarFssc, LargeAmplitudeCleanDecodes)
{
  crcTableInit();
  for (double amp : {100.0, 200.0, 2000.0, 12000.0, 32767.0})
    for (int len : {44, 47, 49, 100, 140})
      for (int al : kAls) {
        if (len + 24 > al * 108) continue;
        for (int i = 0; i < 6; i++) {
          Vec v = make(len, al, amp, 0);
          uint64_t fo[3];
          const uint32_t rf = nr_polar_dci_decode_fssc(v.llr.data(), fo, len, al);
          ASSERT_EQ(rf, v.rnti) << "amp " << amp << " len " << len << " AL " << al;
          ASSERT_EQ(0, memcmp(fo, v.pay, 8 * ((len + 63) / 64)));
          const int16_t *p[1] = {v.llr.data()}; uint32_t res[1]; uint64_t bo[3];
          nr_polar_dci_decode_fssc_x8(p, 1, res, bo, len, al);
          ASSERT_EQ(res[0], v.rnti);
        }
      }
}

// decode success (noisy) at least the reference, per shape
TEST(PolarFssc, SuccessNotBelowReference)
{
  crcTableInit();
  for (int len : {44, 47, 49, 140})
    for (int al : kAls) {
      if (len + 24 > al * 108) continue;
      int okr = 0, okf = 0;
      for (int i = 0; i < 300; i++) {
        Vec v = make(len, al, 20, 20 * 1.1);
        uint64_t ro[3], fo[3];
        okr += ref_dec(v, len, al, ro) == v.rnti;
        okf += nr_polar_dci_decode_fssc(v.llr.data(), fo, len, al) == v.rnti;
      }
      EXPECT_GE(okf, okr) << len << "/" << al;
    }
}

#include <time.h>
static double now_us() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e6 + t.tv_nsec / 1e3; }
// run: taskset -c 0-4 ./test_nr_polar_fssc --gtest_also_run_disabled_tests --gtest_filter=*Bench
TEST(PolarFssc, DISABLED_Bench)
{
  crcTableInit();
  for (int al : kAls) {
    const int len = 47;
    std::vector<Vec> vs; for (int i = 0; i < 64; i++) vs.push_back(make(len, al, 20, 12));
    uint64_t o[3]; uint32_t sink = 0; const int R = 200;
    double t0 = now_us();
    for (int r = 0; r < R; r++) for (auto &v : vs) sink += ref_dec(v, len, al, o);
    const double tr = (now_us() - t0) / (R * 64);
    t0 = now_us();
    for (int r = 0; r < R; r++) for (auto &v : vs) sink += nr_polar_dci_decode_fssc(v.llr.data(), o, len, al);
    const double tf = (now_us() - t0) / (R * 64);
    uint32_t res[8]; uint64_t bo[24]; const int16_t *p[8];
    t0 = now_us();
    for (int r = 0; r < R; r++) for (int b = 0; b < 64; b += 8) { for (int j = 0; j < 8; j++) p[j] = vs[b + j].llr.data(); nr_polar_dci_decode_fssc_x8(p, 8, res, bo, len, al); sink += res[0]; }
    const double tb = (now_us() - t0) / (R * 64);
    printf("len %d AL%d: ref %.3f us | fssc %.3f us | x8 %.3f us per decode (%u)\n", len, al, tr, tf, tb, sink & 1);
  }
}
