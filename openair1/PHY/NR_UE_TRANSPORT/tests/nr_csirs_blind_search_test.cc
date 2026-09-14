#include <cmath>
#include <cstdlib>
#include <vector>
#include <gtest/gtest.h>
extern "C" {
#include "nr_csirs_blind_search.h"
}

// A reference grid: QPSK-ish symbols on `occupied` REs of `n`, zero elsewhere.
static void make_ref(std::vector<int16_t> &ref, int n, int stride, unsigned seed = 7)
{
  ref.assign(2 * n, 0);
  for (int i = 0; i < n; i += stride) {
    ref[2 * i]     = (rand_r(&seed) & 1) ? 2896 : -2896;
    ref[2 * i + 1] = (rand_r(&seed) & 1) ? 2896 : -2896;
  }
}

TEST(CsirsBlindCorrelate, PerfectMatchScoresOne) {
  std::vector<int16_t> ref;
  make_ref(ref, 600, 12);
  EXPECT_NEAR(nr_csirs_blind_correlate(ref.data(), ref.data(), 600), 1.0, 1e-9);
}

TEST(CsirsBlindCorrelate, AChannelRotationStillScoresOne) {
  // The oracle must survive the propagation channel: a common phase/gain on every RE is exactly
  // what a flat channel does, and it must NOT look like a wrong hypothesis.
  std::vector<int16_t> ref;
  make_ref(ref, 600, 12);
  std::vector<int16_t> rx(ref.size());
  const double th = 0.7, g = 0.35;
  for (size_t i = 0; i < ref.size(); i += 2) {
    const double r = ref[i], m = ref[i + 1];
    rx[i]     = (int16_t)lround(g * (r * cos(th) - m * sin(th)));
    rx[i + 1] = (int16_t)lround(g * (r * sin(th) + m * cos(th)));
  }
  EXPECT_GT(nr_csirs_blind_correlate(rx.data(), ref.data(), 600), 0.99);
}

TEST(CsirsBlindCorrelate, AWrongSequenceScoresNearTheNoiseFloor) {
  std::vector<int16_t> ref, other;
  make_ref(ref, 600, 12, 7);
  make_ref(other, 600, 12, 99);   // same REs, different sequence -- the real confusion case
  const double rho = nr_csirs_blind_correlate(other.data(), ref.data(), 600);
  EXPECT_GE(rho, 0.0);
  // 50 occupied REs -> null correlation ~1/sqrt(50) ~ 0.14. Well clear of a match.
  EXPECT_LT(rho, 0.5);
}

TEST(CsirsBlindCorrelate, AnEmptyReferenceIsUnscorableNotZero) {
  std::vector<int16_t> rx(1200, 5), empty(1200, 0);
  // -1 and 0 must differ: 0 would rank an unscorable candidate alongside a genuine mismatch.
  EXPECT_LT(nr_csirs_blind_correlate(rx.data(), empty.data(), 600), 0.0);
  EXPECT_LT(nr_csirs_blind_correlate(nullptr, empty.data(), 600), 0.0);
  EXPECT_LT(nr_csirs_blind_correlate(rx.data(), rx.data(), 0), 0.0);
}

TEST(CsirsBlindCorrelate, UnoccupiedReferenceRElsDoNotDiluteTheScore) {
  // The reference is mostly zeros. Scoring those REs would drag a true match toward 0 as the
  // allocation widens, which would make the threshold bandwidth-dependent.
  std::vector<int16_t> ref;
  make_ref(ref, 3276, 12);
  std::vector<int16_t> rx = ref;
  unsigned seed = 3;
  for (int i = 0; i < 3276; i++) {
    if (ref[2 * i] == 0 && ref[2 * i + 1] == 0) {   // PDSCH energy where CSI-RS is not
      rx[2 * i]     = (int16_t)(rand_r(&seed) % 4000 - 2000);
      rx[2 * i + 1] = (int16_t)(rand_r(&seed) % 4000 - 2000);
    }
  }
  EXPECT_NEAR(nr_csirs_blind_correlate(rx.data(), ref.data(), 3276), 1.0, 1e-9);
}

// ---- periodicity ------------------------------------------------------------------------------

TEST(CsirsBlindPeriod, RecoversPeriodAndOffset) {
  std::vector<uint32_t> hits;
  for (int k = 0; k < 8; k++) hits.push_back(20 * k + 13);
  uint16_t p = 0, o = 0;
  ASSERT_TRUE(nr_csirs_blind_infer_period(hits.data(), (int)hits.size(), 3, &p, &o));
  EXPECT_EQ(p, 20);
  EXPECT_EQ(o, 13);
}

TEST(CsirsBlindPeriod, SurvivesMissedOccurrences) {
  // The reason this is not a GCD of deltas: drop occurrences and the deltas become 40, 60, 20 --
  // a GCD still gives 20 here, but a single spurious-free gap of 3P plus one odd delta would not.
  // The mod-P hypothesis test is unaffected by ANY number of missed occurrences.
  const uint32_t hits[] = {13, 53, 113, 133, 293};
  uint16_t p = 0, o = 0;
  ASSERT_TRUE(nr_csirs_blind_infer_period(hits, 5, 3, &p, &o));
  EXPECT_EQ(p, 20);
  EXPECT_EQ(o, 13);
}

TEST(CsirsBlindPeriod, RejectsASpuriousHit) {
  const uint32_t hits[] = {13, 33, 53, 54, 73};  // 54 belongs to no legal period with the rest
  uint16_t p = 0, o = 0;
  // It must not silently "explain" the set with a wrong period.
  if (nr_csirs_blind_infer_period(hits, 5, 3, &p, &o)) {
    for (int i = 0; i < 5; i++) EXPECT_EQ(hits[i] % p, o) << "claimed period does not explain hit " << i;
  }
}

TEST(CsirsBlindPeriod, RefusesWhenEvidenceIsTooThin) {
  const uint32_t one[] = {40};
  const uint32_t same[] = {40, 40, 40};
  uint16_t p = 0, o = 0;
  EXPECT_FALSE(nr_csirs_blind_infer_period(one, 1, 3, &p, &o));       // a single hit
  EXPECT_FALSE(nr_csirs_blind_infer_period(same, 3, 3, &p, &o));      // zero span
  const uint32_t two[] = {10, 30};
  EXPECT_FALSE(nr_csirs_blind_infer_period(two, 2, 5, &p, &o));       // below min_hits
}

TEST(CsirsBlindPeriod, NeverClaimsAPeriodLongerThanTheObservation) {
  // Two hits 5 slots apart are consistent with period 5 -- and also with 320, trivially. Claiming
  // the long one would be unfalsifiable on this evidence, so the smallest legal one must win.
  const uint32_t hits[] = {100, 105, 110};
  uint16_t p = 0, o = 0;
  ASSERT_TRUE(nr_csirs_blind_infer_period(hits, 3, 3, &p, &o));
  EXPECT_EQ(p, 5);
}

TEST(CsirsBlindPeriod, IsNotFooledByDivisorsOfTheTruePeriod) {
  // REGRESSION. The first implementation returned the SMALLEST consistent period and this is what
  // caught it: hits at period 20 are also perfectly consistent with 4, 5 and 10, because every
  // divisor of the true period passes the "same slot mod P" test. A smallest-first rule therefore
  // reports 4 for essentially every real configuration. Only periods LONGER than the truth are
  // self-rejecting, so the largest survivor is the answer.
  std::vector<uint32_t> hits;
  for (int k = 0; k < 10; k++) hits.push_back(160 * k + 7);   // a common real periodicity
  uint16_t p = 0, o = 0;
  ASSERT_TRUE(nr_csirs_blind_infer_period(hits.data(), (int)hits.size(), 3, &p, &o));
  EXPECT_EQ(p, 160) << "returned a divisor of the true period";
  EXPECT_EQ(o, 7);
}

TEST(CsirsBlindFormat, EmitsAParsableMonitorEntry) {
  nr_csirs_candidate_t c{};
  c.row = 1; c.start_rb = 0; c.nr_of_rbs = 273; c.freq_domain = 4;
  c.symb_l0 = 4; c.symb_l1 = 0; c.cdm_type = 0; c.freq_density = 3; c.scramb_id = 2;
  char buf[128];
  const int n = nr_csirs_blind_format(&c, 20, 13, buf, sizeof(buf));
  ASSERT_GT(n, 0);
  EXPECT_STREQ(buf, "1:0:273:4:4:0:0:3:2:20:13");
  // must refuse rather than truncate into a silently wrong config line
  char tiny[8];
  EXPECT_EQ(nr_csirs_blind_format(&c, 20, 13, tiny, sizeof(tiny)), 0);
}

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
