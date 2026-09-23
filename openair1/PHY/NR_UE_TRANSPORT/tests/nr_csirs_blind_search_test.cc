#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>
#include <random>
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

// ---- enumeration and scheduling ----------------------------------------------------------------

TEST(CsirsBlindEnum, EnumeratesRealConfigurationsOnly) {
  std::vector<nr_csirs_candidate_t> c(NR_CSIRS_BLIND_MAX_CAND);
  const int n = nr_csirs_blind_enumerate(c.data(), NR_CSIRS_BLIND_MAX_CAND, 273, 2);
  ASSERT_GT(n, 0);
  for (int i = 0; i < n; i++) {
    EXPECT_TRUE(c[i].row == 1 || c[i].row == 2 || c[i].row == 4);
    EXPECT_EQ(c[i].scramb_id, 2);
    EXPECT_EQ(c[i].nr_of_rbs, 273);
    // one-hot frequency-domain bitmap: a real configuration selects one position, and allowing
    // arbitrary bitmaps would multiply the search by 2^12 for combinations no gNB emits.
    EXPECT_EQ(c[i].freq_domain & (c[i].freq_domain - 1), 0) << "bitmap is not one-hot";
    EXPECT_NE(c[i].freq_domain, 0);
    // symbols 0-1 are excluded: the CORESET occupies the start of the slot.
    EXPECT_GE(c[i].symb_l0, 2);
    EXPECT_LE(c[i].symb_l0, 12);
  }
  EXPECT_EQ(nr_csirs_blind_enumerate(nullptr, 10, 273, 2), -1);
  EXPECT_EQ(nr_csirs_blind_enumerate(c.data(), 10, 0, 2), -1);
}

TEST(CsirsBlindEnum, RoundRobinVisitsEveryCandidate) {
  // Every candidate must see statistically the same channel -- the same reason Technique D
  // interleaves per grant rather than testing in blocks.
  nr_csirs_blind_state_t st{};
  const int n = nr_csirs_blind_init(&st, 273, 2);
  ASSERT_GT(n, 1);
  std::vector<int> seen(n, 0);
  for (int i = 0; i < n * 3; i++) seen[nr_csirs_blind_next(&st)]++;
  for (int i = 0; i < n; i++) EXPECT_EQ(seen[i], 3) << "candidate " << i << " was not visited evenly";
}

TEST(CsirsBlindFeed, NeedsToStandOutFromItsOwnPopulation) {
  nr_csirs_blind_state_t st{};
  ASSERT_GT(nr_csirs_blind_init(&st, 273, 2), 3);
  // A high score that does NOT beat the null population is not a detection: on a loud channel
  // everything scores high, and an absolute bar would fire on all of it.
  EXPECT_FALSE(nr_csirs_blind_feed(&st, 0, 10, 0.80, 0.70));
  EXPECT_EQ(st.hits[0], 0u);
  // An unscorable candidate (-1 from the correlator) can never pass -- the reason it is -1 and not 0.
  EXPECT_FALSE(nr_csirs_blind_feed(&st, 0, 10, -1.0, 0.01));
  EXPECT_EQ(st.hits[0], 0u);
}

TEST(CsirsBlindFeed, ScoringHighIsNotEnoughWithoutPeriodicity) {
  // A one-off correlation spike is not a resource. Requiring a PERIOD is what separates a real
  // configuration from a lucky slot, and it costs nothing extra to demand.
  nr_csirs_blind_state_t st{};
  ASSERT_GT(nr_csirs_blind_init(&st, 273, 2), 3);
  EXPECT_FALSE(nr_csirs_blind_feed(&st, 0, 7, 0.95, 0.05));
  EXPECT_FALSE(nr_csirs_blind_feed(&st, 0, 7, 0.95, 0.05));   // same slot: zero span
  EXPECT_EQ(nr_csirs_blind_confirmed(&st, nullptr, nullptr), nullptr);
}

TEST(CsirsBlindFeed, ConfirmsAPeriodicResourceAndReportsIt) {
  nr_csirs_blind_state_t st{};
  ASSERT_GT(nr_csirs_blind_init(&st, 273, 2), 3);
  bool done = false;
  for (int k = 0; k < 6 && !done; k++) {
    done = nr_csirs_blind_feed(&st, 2, (uint32_t)(20 * k + 13), 0.92, 0.04);
  }
  ASSERT_TRUE(done);
  uint16_t p = 0, o = 0;
  const nr_csirs_candidate_t *c = nr_csirs_blind_confirmed(&st, &p, &o);
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(p, 20);
  EXPECT_EQ(o, 13);
  // Once confirmed the scheduler stops rotating -- there is nothing left to search for.
  EXPECT_EQ(nr_csirs_blind_next(&st), 2);
  // And it formats straight back out as a csirs_monitor line.
  char buf[128];
  ASSERT_GT(nr_csirs_blind_format(c, p, o, buf, sizeof(buf)), 0);
  EXPECT_NE(std::string(buf).find(":20:13"), std::string::npos);
}

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

// ---- Zero-power CSI-RS: energy hole under a scheduled PDSCH -------------------------------------
namespace {
// A 4-RB symbol: the "reference" occupies subcarrier 0 of every RB (a density-1 comb); rx carries
// unit-amplitude data everywhere except where `hole` says the pattern REs are silent.
void zp_symbol(bool hole, bool empty, std::vector<int16_t> &rx, std::vector<int16_t> &ref) {
  const int n = 48;
  rx.assign(2 * n, 0);
  ref.assign(2 * n, 0);
  for (int i = 0; i < n; i++) {
    const bool on = (i % 12) == 0;
    if (on) { ref[2 * i] = 100; ref[2 * i + 1] = -100; }
    if (empty) continue;
    const int16_t a = (on && hole) ? 2 : 700; // residual noise in the hole, data elsewhere
    rx[2 * i] = (i & 1) ? a : -a;
    rx[2 * i + 1] = (i & 2) ? a : -a;
  }
}
}
TEST(CsirsBlindZp, HoleUnderDataScoresOneDataScoresZeroSilenceScoresZero) {
  std::vector<int16_t> rx, ref;
  zp_symbol(true, false, rx, ref);
  EXPECT_GT(nr_csirs_blind_zero_score(rx.data(), ref.data(), 48), 0.99);
  zp_symbol(false, false, rx, ref);
  EXPECT_NEAR(nr_csirs_blind_zero_score(rx.data(), ref.data(), 48), 0.0, 1e-9);
  zp_symbol(false, true, rx, ref);            // nothing scheduled: unscorable, never a hit
  EXPECT_LT(nr_csirs_blind_zero_score(rx.data(), ref.data(), 48), 0.0);
  std::vector<int16_t> none(2 * 48, 0);       // reference maps no RE
  EXPECT_LT(nr_csirs_blind_zero_score(rx.data(), none.data(), 48), 0.0);
}
TEST(CsirsBlindZp, ConfirmsAPeriodicHoleAndRejectsAStructuralOne) {
  nr_csirs_blind_state_t st;
  ASSERT_GT(nr_csirs_blind_init(&st, 24, 1), 0);
  // periodic: candidate 0 tested every 4 slots, hole every 20 slots -> hit on 1 test in 5
  for (uint32_t slot = 0; slot < 400; slot += 4) {
    const double s = (slot % 20 == 0) ? 0.98 : 0.02;
    const bool done = nr_csirs_blind_zp_feed(&st, 0, slot, s, 0.02);
    if (done) {
      uint16_t p = 0, o = 0;
      ASSERT_NE(nr_csirs_blind_confirmed(&st, &p, &o), nullptr);
      EXPECT_EQ(p, 20);
      EXPECT_EQ(o, 0);
      break;
    }
  }
  EXPECT_GE(st.confirmed, 0);
  // structural: a hole on EVERY test (a DM-RS symbol's data-free CDM group) never confirms
  nr_csirs_blind_state_t st2;
  ASSERT_GT(nr_csirs_blind_init(&st2, 24, 1), 0);
  for (uint32_t slot = 0; slot < 400; slot += 4)
    EXPECT_FALSE(nr_csirs_blind_zp_feed(&st2, 0, slot, 0.98, 0.02));
  EXPECT_LT(st2.confirmed, 0);
}

/* ---- Channel robustness: the whole reason the OTA correlation read as noise ------------------
 * MEASURED 2026-09-19 on a live 100 MHz cell: the sequence-free energy test found a TRS pair at
 * 4-6x its neighbours while the flat correlation sat at the noise floor for EVERY scramblingID.
 * The flat oracle sums y*conj(x) coherently across the whole band, but the channel rotates each
 * subcarrier, so a CORRECT sequence averages to nothing. These two cases pin that down: same
 * samples, same sequence, only a channel phase ramp added. */
namespace {
/* A candidate-shaped reference: every 4th RE occupied (density 3), QPSK-ish, over `n` REs. */
void make_ref(std::vector<int16_t> &ref, int n)
{
  ref.assign((size_t)2 * n, 0);
  std::mt19937 g(4242);
  for (int i = 0; i < n; i += 4) {
    ref[2 * i]     = (g() & 1) ? 800 : -800;
    ref[2 * i + 1] = (g() & 1) ? 800 : -800;
  }
}
/* rx = ref rotated by a linear phase ramp of `rad_total` radians end to end, plus noise. */
void make_rx(std::vector<int16_t> &rx, const std::vector<int16_t> &ref, int n, double rad_total,
             double noise_amp)
{
  rx.assign((size_t)2 * n, 0);
  std::mt19937 g(99);
  std::normal_distribution<double> nd(0.0, noise_amp);
  for (int i = 0; i < n; i++) {
    const double ph = rad_total * ((double)i / (double)n);
    const double xr = ref[2 * i], xi = ref[2 * i + 1];
    rx[2 * i]     = (int16_t)(xr * cos(ph) - xi * sin(ph) + nd(g));
    rx[2 * i + 1] = (int16_t)(xr * sin(ph) + xi * cos(ph) + nd(g));
  }
}
} // namespace

TEST(CsirsBlindCorrelate, FlatCorrelationDiesUnderARealChannelButTheSubBandScoreSurvives)
{
  const int n = 3276;                 // one 273 RB symbol
  std::vector<int16_t> ref, rx;
  make_ref(ref, n);
  make_rx(rx, ref, n, 40.0, 80.0);    // 40 rad end to end ~ 100 ns delay spread at 100 MHz

  int used = 0;
  const double flat = nr_csirs_blind_correlate_n(rx.data(), ref.data(), n, &used);
  ASSERT_GT(used, 0);
  const double z_flat = flat * sqrt((double)used);      // the old scale-free score
  const double z_block = nr_csirs_blind_correlate_blocks(rx.data(), ref.data(), n, 32, &used);

  EXPECT_LT(z_flat, 3.0);    // the CORRECT sequence looks like noise to the flat oracle
  EXPECT_GT(z_block, 4.0);   // and is still found once the sub-bands are combined non-coherently
}

TEST(CsirsBlindCorrelate, SubBandScoreReadsAboutOneOnNoise)
{
  const int n = 3276;
  std::vector<int16_t> ref, rx;
  make_ref(ref, n);
  make_rx(rx, ref, n, 0.0, 0.0);
  std::mt19937 g(7);                       // replace rx with pure noise: wrong sequence entirely
  std::normal_distribution<double> nd(0.0, 500.0);
  for (auto &v : rx) v = (int16_t)nd(g);
  int used = 0;
  const double z = nr_csirs_blind_correlate_blocks(rx.data(), ref.data(), n, 32, &used);
  EXPECT_GT(used, 0);
  EXPECT_LT(z, 2.5);                       // noise floor ~1.0; the confirmation bar is 4
}

/* ---- Partial-band resource: the leading explanation of the OTA scores -------------------------
 * A candidate asserts start_rb=0, nr_of_rbs=N_RB_DL, but a real CSI-RS (a TRS especially) is often
 * configured over part of the BWP. The whole-band mean then reads ~fraction * perfect. MEASURED on
 * air after BOTH the scramblingID (complete 1024 sweep) and the slot index (complete 20 sweep) had
 * been excluded: the best score sat at 1.31-1.40, not at the 1.0 noise floor -- exactly what a
 * correct sequence covering ~20 % of the band produces. These two cases pin that behaviour down. */
TEST(CsirsBlindCorrelate, WholeBandMeanDilutesAPartialBandResource)
{
  const int n = 3276;
  std::vector<int16_t> ref, rx;
  make_ref(ref, n);
  make_rx(rx, ref, n, 8.0, 60.0);     // correct sequence, mild channel
  /* The truth only occupies the first 20 % of the band; everywhere else the reference points at REs
   * that carry nothing related. */
  for (int i = (int)(0.2 * n); i < n; i++) { rx[2 * i] = (int16_t)0; rx[2 * i + 1] = (int16_t)0; }

  int used = 0;
  const double z_mean = nr_csirs_blind_correlate_blocks(rx.data(), ref.data(), n, 32, &used);
  int fb = -1, nbk = 0;
  const double z_run = nr_csirs_blind_correlate_bestrun(rx.data(), ref.data(), n, 32, &fb, &nbk, &used);
  printf("PARTIAL-BAND: whole-band z=%.2f, best-run z=%.2f over blocks [%d..%d]\n", z_mean, z_run, fb,
         fb + nbk - 1);

  EXPECT_LT(z_mean, 2.5);   // the mean cannot see it: this is the OTA symptom
  EXPECT_GT(z_run, 4.0);    // the contiguous run does
  EXPECT_GE(fb, 0);
  EXPECT_LE(fb, 2);         // and it localises the resource to the low end of the band
}

TEST(CsirsBlindCorrelate, BestRunDoesNotManufactureASignalFromNoise)
{
  const int n = 3276;
  std::vector<int16_t> ref, rx;
  make_ref(ref, n);
  std::mt19937 g(31337);
  std::normal_distribution<double> nd(0.0, 500.0);
  rx.assign((size_t)2 * n, 0);
  for (auto &v : rx) v = (int16_t)nd(g);
  int fb = -1, nbk = 0, used = 0;
  const double z = nr_csirs_blind_correlate_bestrun(rx.data(), ref.data(), n, 32, &fb, &nbk, &used);
  printf("NOISE best-run z=%.2f (blocks %d..%d)\n", z, fb, fb + nbk - 1);
  EXPECT_LT(z, 4.0);        // picking the best of many runs must not clear the bar on pure noise
}
