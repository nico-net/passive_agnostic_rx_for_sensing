#include "nr_ssb_rate_match.h"
#include <gtest/gtest.h>
#include <array>
#include <cmath>
#include <random>
#include <vector>

TEST(SsbRateMatch, WholeEdgePrbsAndFourSymbols)
{
  auto e = nr_ssb_rm_event(123, 0, 42, 66, 0x3c);
  auto m = nr_ssb_rm_map(&e, 123, 0, 42, 0, 52, nullptr);
  for (int s = 0; s < 14; ++s)
    for (int rb = 0; rb < 52; ++rb)
      EXPECT_EQ(nr_ssb_rm_excluded(&m, s, rb),
                s >= 2 && s < 6 && rb >= 5 && rb <= 25 ? 0xfff : 0);
}

TEST(SsbRateMatch, PhysicalPrbsFollowInterleavedDataOrderAndBwpOffset)
{
  auto e = nr_ssb_rm_event(123, 0, 42, 66, 0xf3c);
  const uint16_t physical[] = {30, 5, 0, 23, 10};
  auto m = nr_ssb_rm_map(&e, 123, 0, 42, 3, 5, physical);
  const bool expected[] = {false, true, false, false, true};
  for (int i = 0; i < 5; ++i) {
    EXPECT_EQ(nr_ssb_rm_excluded(&m, 2, i), expected[i] ? 0xfff : 0);
    EXPECT_EQ(nr_ssb_rm_excluded(&m, 10, i), expected[i] ? 0xfff : 0);
    EXPECT_EQ(nr_ssb_rm_excluded(&m, 7, i), 0);
  }
  m = nr_ssb_rm_map(&e, 124, 0, 42, 3, 5, physical);
  EXPECT_EQ(m.symbols, 0);
}

TEST(SsbRateMatch, NullAndBoundsAreUnchanged)
{
  nr_ssb_rm_mask_t m = {};
  EXPECT_EQ(nr_ssb_rm_excluded(nullptr, 2, 5), 0);
  EXPECT_EQ(nr_ssb_rm_excluded(&m, -1, 5), 0);
  EXPECT_EQ(nr_ssb_rm_excluded(&m, 14, 5), 0);
  EXPECT_EQ(nr_ssb_rm_excluded(&m, 2, -1), 0);
  EXPECT_EQ(nr_ssb_rm_excluded(&m, 2, 275), 0);
}

TEST(SsbRateMatch, TimingIdentityMustMatch)
{
  nr_ssb_rm_event_t e = {123, 0, 42, 0x3c, 5, 25};
  EXPECT_TRUE(nr_ssb_rm_event_valid(&e, 123, 0, 42));
  EXPECT_FALSE(nr_ssb_rm_event_valid(&e, 124, 0, 42));
  EXPECT_FALSE(nr_ssb_rm_event_valid(&e, 123, 1, 42));
  EXPECT_FALSE(nr_ssb_rm_event_valid(&e, 123, 0, 43));
  e.symbols = 0;
  EXPECT_FALSE(nr_ssb_rm_event_valid(&e, 123, 0, 42));
}

/* Independent transmitter sequence generator: TS 38.211 7.4.2.2/7.4.2.3. */
static void make_sync(int pci, std::array<int16_t, 254>& p, std::array<int16_t, 254>& s)
{
  std::array<int, 127> x = {}, a = {}, b = {};
  x[1] = x[2] = x[4] = x[5] = x[6] = 1;
  a[0] = b[0] = 1;
  for (int n = 0; n < 120; ++n) {
    x[n + 7] = x[n + 4] ^ x[n];
    a[n + 7] = a[n + 4] ^ a[n];
    b[n + 7] = b[n + 1] ^ b[n];
  }
  const int id1 = pci / 3, id2 = pci % 3;
  for (int n = 0; n < 127; ++n) {
    const int ps = 1 - 2 * x[(n + 43 * id2) % 127];
    const int ss = (1 - 2 * a[(n + 15 * (id1 / 112) + 5 * id2) % 127])
                 * (1 - 2 * b[(n + id1 % 112) % 127]);
    // Frequency-selective phase and amplitude, and inter-symbol CFO phase.
    const double ph = 0.15 * n, amp = 1000 + 700 * std::sin(n * 0.04);
    p[2*n] = ps * amp * std::cos(ph);
    p[2*n+1] = ps * amp * std::sin(ph);
    s[2*n] = ss * amp * std::cos(ph + 0.8);
    s[2*n+1] = ss * amp * std::sin(ph + 0.8);
  }
}

TEST(SsbRateMatch, ObservesOnlyThePresentPci)
{
  std::array<int16_t, 254> p = {}, s = {};
  for (int pci : {0, 42, 500, 1007}) {
    make_sync(pci, p, s);
    for (int expected = 0; expected < 1008; ++expected)
      EXPECT_EQ(nr_ssb_rm_sync_present(p.data(), s.data(), expected), expected == pci)
          << "present=" << pci << " expected=" << expected;
  }
}

TEST(SsbRateMatch, EmptyNoiseAndContinuousWaveDoNotCreateEvents)
{
  std::array<int16_t, 254> p = {}, s = {};
  EXPECT_FALSE(nr_ssb_rm_sync_present(p.data(), s.data(), 42));
  p.fill(1000); s.fill(1000);
  EXPECT_FALSE(nr_ssb_rm_sync_present(p.data(), s.data(), 42));
  std::mt19937 rng(72031);
  std::normal_distribution<double> normal(0, 3000);
  for (int trial = 0; trial < 10000; ++trial) {
    for (int n = 0; n < 254; ++n) { p[n] = normal(rng); s[n] = normal(rng); }
    ASSERT_FALSE(nr_ssb_rm_sync_present(p.data(), s.data(), trial % 1008)) << trial;
  }
}

TEST(SsbRateMatch, UnionDoesNotDoubleCountCsiOrDmrs)
{
  nr_ssb_rm_mask_t m = {};
  m.symbols = 0x3c;
  m.prb[5] = 1;
  EXPECT_EQ(__builtin_popcount(nr_ssb_rm_excluded(&m, 2, 5) | 0x555 | 0x88), 12);
  EXPECT_EQ(__builtin_popcount(nr_ssb_rm_excluded(&m, 6, 5) | 0x555 | 0x88), 8);
}

/* The false-alarm contract of the gate, numerically. Noise only, the detector's statistic is a sum of
 * N independent uniform unit phasors; its tail must stay under the Bernstein/direction-union bound
 * nr_ssb_rm_sync_r2() is solved from, and that bound must never undercut the asymptotic (Gaussian)
 * tail exp(-r^2/N) of the true distribution. */
TEST(SsbRateMatch, NoiseTailStaysUnderTheFalseAlarmBound)
{
  std::mt19937 rng(20260928);
  std::uniform_real_distribution<double> phase(0, 2 * M_PI);
  const int trials = 200000;
  const double pfa[] = {1e-1, 1e-2, 1e-3, 1e-9};
  int exceed[4] = {0, 0, 0, 0};
  for (int t = 0; t < trials; ++t) {
    double sr = 0, si = 0;
    for (int n = 0; n < 127; ++n) {
      const double ph = phase(rng);
      sr += std::cos(ph);
      si += std::sin(ph);
    }
    for (int i = 0; i < 4; ++i)
      exceed[i] += sr * sr + si * si > nr_ssb_rm_sync_r2(127, pfa[i]);
  }
  for (int i = 0; i < 4; ++i)
    EXPECT_LE(exceed[i], pfa[i] * trials) << "pfa " << pfa[i];
  EXPECT_EQ(exceed[3], 0);
  for (int n = 1; n <= 127; ++n)
    for (double p : pfa)
      EXPECT_GE(nr_ssb_rm_sync_r2(n, p), n * std::log(1 / p)) << "n " << n << " pfa " << p;
  // Sensitivity this buys at the operating false-alarm rate: a coherence of about 0.51 over 127 bins.
  EXPECT_LT(nr_ssb_rm_sync_r2(127, 1e-9), std::pow(0.52 * 127, 2));
}
