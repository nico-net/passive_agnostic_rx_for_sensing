#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>
#include <random>
#include <memory>
#include <map>
#include <cstdio>
#include <gtest/gtest.h>
extern "C" {
#include "nr_csirs_blind_search.h"
int nr_csirs_blind_rt_test_export(int rank, int banks, int types[2], int *untouched);
int nr_csirs_blind_rt_test_zp_feed(uint32_t slot, double score, int reset);
int nr_csirs_blind_rt_test_slot(int row, uint32_t slot, int holes, int empty, int reset, int *fep_mask);
int nr_csirs_blind_rt_test_slot_pattern(int row, uint32_t slot, int holes, int empty,
                                      int extra_holes, int reset, int *fep_mask);
void nr_csirs_blind_rt_test_logging(int enabled);
int nr_csirs_blind_rt_test_future_export(uint32_t slot);
int nr_csirs_blind_rt_test_nzp_confirmed(void);
void nr_csirs_blind_rt_test_maintenance_control(int control);
double nr_csirs_blind_rt_test_union_score(void);
void nr_csirs_blind_rt_test_status_next(void);
void nr_csirs_blind_rt_test_two_resources(void);
int nr_csirs_blind_rt_test_bank_index(int k);
void nr_csirs_blind_rt_test_retire_discovery(void);
uint64_t nr_csirs_blind_rt_test_maintenance_count(int idx, const char *key);
int nr_csirs_blind_rt_test_fep_calls(void);
void nr_csirs_blind_rt_test_clear_pins(void);
uint32_t nr_csirs_blind_rt_test_pin_left(int zp);
void nr_csirs_blind_rt_test_eight_probations(void);
/* get_csi_mapping_parms() is linked in for the footprint tests; OAI's AssertFatal and CONFIG code
 * reference these two from the softmodem's main(). */
void *uniqCfg = nullptr;
void exit_function(const char *file, const char *function, const int line, const char *s, const int a)
{
  (void)file; (void)function; (void)line; (void)a;
  fprintf(stderr, "exit_function: %s\n", s ? s : "");
  abort();
}
}

TEST(CsirsBlindRuntime, OrdinaryModeExportsBothBanks) {
  for (int banks : {1, 2, 3}) {
    int types[2] = {}, untouched = 0;
    const int n = nr_csirs_blind_rt_test_export(0, banks, types, &untouched);
    ASSERT_EQ(n, banks == 3 ? 2 : 1);
    EXPECT_FALSE(untouched);
    EXPECT_EQ(types[0], banks == 2 ? 2 : 1);
    if (banks == 3) {
      EXPECT_EQ(types[1], 2);
    }
  }
}

TEST(CsirsBlindRuntime, RankModeExportsNeitherBankAndLeavesOutputUntouched) {
  for (int banks : {1, 2, 3}) {
    int types[2] = {}, untouched = 0;
    EXPECT_EQ(nr_csirs_blind_rt_test_export(1, banks, types, &untouched), 0) << "banks=" << banks;
    EXPECT_TRUE(untouched) << "banks=" << banks;
  }
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
    EXPECT_GT(nr_csirs_blind_row_ports(c[i].row), 0) << "row " << (int)c[i].row << " has no port count";
    EXPECT_EQ(c[i].scramb_id, 2);
    EXPECT_EQ(c[i].nr_of_rbs, 273);
    // one-hot frequency-domain bitmap: a real configuration selects one position, and allowing
    // arbitrary bitmaps would multiply the search by 2^12 for combinations no gNB emits.
    EXPECT_EQ(c[i].freq_domain & (c[i].freq_domain - 1), 0) << "bitmap is not one-hot";
    EXPECT_NE(c[i].freq_domain, 0);
    // symbols 0-13 are enumerated (row 5 stops at 12 due to l0+l1 occupancy; others go to 13).
    EXPECT_GE(c[i].symb_l0, 0);
    EXPECT_LE(c[i].symb_l0, 13);
  }
  EXPECT_EQ(nr_csirs_blind_enumerate(nullptr, 10, 273, 2), -1);
  EXPECT_EQ(nr_csirs_blind_enumerate(c.data(), 10, 0, 2), -1);
}

TEST(CsirsBlindEnum, RowPortsMatchTheSpecTableAllRows) {
  // TS 38.211 Table 7.4.1.5.3-1, every row. Row 6 was 0 while only rows 1-5 were enumerated; rows
  // 6-18 are now reachable through footprint matching, so their port counts are spec, not 0.
  const int ports[18] = {1, 1, 2, 4, 4, 8, 8, 8, 12, 12, 16, 16, 24, 24, 24, 32, 32, 32};
  for (int row = 1; row <= 18; row++)
    EXPECT_EQ(nr_csirs_blind_row_ports((uint8_t)row), ports[row - 1]) << "row " << row;
  EXPECT_EQ(nr_csirs_blind_row_ports(0), 0);
  EXPECT_EQ(nr_csirs_blind_row_ports(19), 0);
}

// ---- rows 6-18: footprint-first search ----------------------------------------------------------

static nr_csirs_candidate_t cand_of(int row, int fd, int l0, int l1, int density = 2)
{
  nr_csirs_candidate_t c{};
  c.row = (uint8_t)row;
  c.freq_domain = (uint16_t)fd;
  c.symb_l0 = (uint8_t)l0;
  c.symb_l1 = (uint8_t)l1;
  c.freq_density = (uint8_t)density;
  c.scramb_id = 382;
  c.nr_of_rbs = 273;
  return c;
}

TEST(CsirsBlindFootprint, EveryRowsFootprintHasPortsRes) {
  // Density 1 puts exactly one RE per port per RB, so OAI's own mapping table must yield `ports`
  // distinct REs for every wide row -- the property footprint matching relies on to tell rows apart.
  for (int row = 6; row <= 18; row++) {
    const int need = nr_csirs_blind_row_needs_bits((uint8_t)row);
    ASSERT_GT(need, 0) << "row " << row;
    const nr_csirs_candidate_t c = cand_of(row, (1 << need) - 1, 4, 8);
    uint16_t m[NR_CSIRS_BLIND_NSYM];
    const int n = nr_csirs_blind_footprint(&c, m);
    EXPECT_EQ(n, nr_csirs_blind_row_ports((uint8_t)row)) << "row " << row;
    int bits = 0;
    for (int l = 0; l < NR_CSIRS_BLIND_NSYM; l++)
      bits += __builtin_popcount(m[l]);
    EXPECT_EQ(bits, n) << "row " << row << ": returned count disagrees with the mask";
  }
}

TEST(CsirsBlindFootprint, RefusesACandidateThatWouldSpinTheGenerator) {
  // row 16 needs 4 set bits; get_csi_mapping_parms()'s walk never ends with fewer.
  const nr_csirs_candidate_t c = cand_of(16, 0x3, 4, 8);
  uint16_t m[NR_CSIRS_BLIND_NSYM];
  EXPECT_EQ(nr_csirs_blind_footprint(&c, m), -1);
}

// One FFT'd symbol in CRB order (rx_shift 0): power `on` at the subcarriers in `mask` of the RBs of
// the chosen parity, `off` elsewhere, with a little deterministic ripple so the classifier cannot
// rely on exact ties.
static std::vector<int16_t> symbol_with(int n_rb, uint16_t mask, int on, int off, bool even, bool odd,
                                        int n_fft, int shift)
{
  std::vector<int16_t> rx((size_t)2 * n_fft, 0);
  unsigned seed = 11;
  for (int i = 0; i < n_rb * 12; i++) {
    const int rb = i / 12, k = i % 12;
    const bool lit = ((mask >> k) & 1) && ((rb % 2 == 0) ? even : odd);
    const int a = (lit ? on : off) + (int)(rand_r(&seed) % 7);
    const int j = (i + shift) % n_fft;
    rx[2 * j] = (int16_t)((rand_r(&seed) & 1) ? a : -a);
    rx[2 * j + 1] = (int16_t)((rand_r(&seed) & 1) ? a : -a);
  }
  return rx;
}

TEST(CsirsBlindFootprint, SymbolProfileFindsTheLitSubcarriersPerParity) {
  const int n_rb = 51, n_fft = 1024, shift = 1024 - 306;   // FFT-ordered like the real grid
  const uint16_t m32 = 0x0F3C;                             // 8 of 12: a 32-port row's symbol
  uint16_t e = 0, o = 0;
  auto rx = symbol_with(n_rb, m32, 400, 40, true, true, n_fft, shift);
  nr_csirs_blind_symbol_on(rx.data(), n_fft, shift, n_rb, &e, &o);
  EXPECT_EQ(e, m32);
  EXPECT_EQ(o, m32);
  // Flat power (PDSCH everywhere, or noise): no structure, no hit.
  rx = symbol_with(n_rb, 0, 400, 400, true, true, n_fft, shift);
  nr_csirs_blind_symbol_on(rx.data(), n_fft, shift, n_rb, &e, &o);
  EXPECT_EQ(e, 0);
  EXPECT_EQ(o, 0);
  // Density 0.5 (even RBs only): the odd-RB profile is flat.
  rx = symbol_with(n_rb, m32, 400, 40, true, false, n_fft, shift);
  nr_csirs_blind_symbol_on(rx.data(), n_fft, shift, n_rb, &e, &o);
  EXPECT_EQ(e, m32);
  EXPECT_EQ(o, 0);
}

// Drive the accumulator the way the RT path does: one symbol per visit, hits only when the visit
// lands on a slot and symbol a resource occupies. The visited symbol is pseudo-random: a plain
// s % 14 aliases against even periods (gcd(40, 14) = 2 would hide every even symbol forever).
struct Res { uint16_t fp[NR_CSIRS_BLIND_NSYM]; int density; uint32_t period, offset; };
static uint32_t xs(uint32_t &x) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; return x; }
static void run_visits(nr_csirs_blind_fp_t *acc, const std::vector<Res> &res, uint32_t n_slots,
                       uint32_t aperiodic_every = 0, uint16_t aperiodic_mask = 0, int aperiodic_sym = -1)
{
  uint32_t x = 2463534242u;
  for (uint32_t s = 0; s < n_slots; s++) {
    const int l = (int)(xs(x) % NR_CSIRS_BLIND_NSYM);
    uint16_t e = 0, o = 0;
    for (const Res &r : res) {
      if (s % r.period != r.offset) continue;
      if (r.density != 1) e |= r.fp[l];
      if (r.density != 0) o |= r.fp[l];
    }
    // traffic-driven energy on symbols aperiodic_sym and aperiodic_sym+1 (a double-symbol DM-RS)
    if (aperiodic_every && (l == aperiodic_sym || l == aperiodic_sym + 1) && xs(x) % aperiodic_every == 0) {
      e |= aperiodic_mask;
      o |= aperiodic_mask;
    }
    nr_csirs_blind_fp_record(acc, l, e, o, s);
  }
}

static bool has(const std::vector<nr_csirs_candidate_t> &v, int row, int fd, int l0, int l1, int d)
{
  for (const auto &c : v)
    if (c.row == row && c.freq_domain == fd && c.symb_l0 == l0 && c.symb_l1 == l1 && c.freq_density == d)
      return true;
  return false;
}

TEST(CsirsBlindFootprint, MatchesA32PortResourceAndOnlyItsFootprintTwins) {
  const nr_csirs_candidate_t truth = cand_of(16, 0x17, 5, 9);   // k-pairs 0,1,2,4; symbols 5,6,9,10
  Res r{};
  ASSERT_EQ(nr_csirs_blind_footprint(&truth, r.fp), 32);
  r.density = 2; r.period = 40; r.offset = 3;
  // A TRS (row 1, comb 4) in the SAME slots: a real cell carries several resources, so the measured
  // footprint is a union and exact equality would find nothing.
  Res trs{};
  const nr_csirs_candidate_t t = cand_of(1, 0x8, 5, 0, 3);   // subcarriers 3,7,11: 7 and 11 lie outside the row-16 mask
  ASSERT_EQ(nr_csirs_blind_footprint(&t, trs.fp), 3);
  trs.density = 2; trs.period = 40; trs.offset = 3;
  auto acc = std::make_unique<nr_csirs_blind_fp_t>();
  run_visits(acc.get(), {r, trs}, 40 * 14 * 12);
  std::vector<nr_csirs_candidate_t> out(64);
  const int n = nr_csirs_blind_fp_match(acc.get(), 273, 382, out.data(), (int)out.size());
  out.resize(n > 0 ? n : 0);
  // Exactly the two rows with this RE set: row 18 needs 4 consecutive symbols, and every narrower
  // row (13, 11, 6, ...) fits inside it and must be dropped as a strict sub-footprint.
  ASSERT_EQ(n, 2);
  EXPECT_TRUE(has(out, 16, 0x17, 5, 9, 2)) << "the transmitted resource was not matched";
  // Row 17 (cdm4) has the identical RE set; only the sequence stage can separate the two.
  EXPECT_TRUE(has(out, 17, 0x17, 5, 9, 2));
  for (const auto &c : out) {
    uint16_t m[NR_CSIRS_BLIND_NSYM];
    ASSERT_EQ(nr_csirs_blind_footprint(&c, m), 32) << "row " << (int)c.row << ": a strict sub-footprint survived";
    for (int l = 0; l < NR_CSIRS_BLIND_NSYM; l++) EXPECT_EQ(m[l], r.fp[l]) << "row " << (int)c.row;
    EXPECT_EQ(c.scramb_id, 382);
    EXPECT_EQ(c.nr_of_rbs, 273);
    EXPECT_TRUE(nr_csirs_blind_candidate_safe(&c));
  }
  // cdm_type follows the row: row 16 fd-CDM2 (1), row 17 cdm4-FD2-TD2 (2).
  for (const auto &c : out) EXPECT_EQ(c.cdm_type, c.row == 16 ? 1 : 2);
}

TEST(CsirsBlindFootprint, DensityHalfIsMatchedOnItsOwnParity) {
  const nr_csirs_candidate_t truth = cand_of(18, 0x0F, 6, 0, 0);   // cdm8, symbols 6-9, even RBs
  Res r{};
  ASSERT_EQ(nr_csirs_blind_footprint(&truth, r.fp), 32);
  r.density = 0; r.period = 20; r.offset = 7;
  auto acc = std::make_unique<nr_csirs_blind_fp_t>();
  run_visits(acc.get(), {r}, 20 * 14 * 12);
  std::vector<nr_csirs_candidate_t> out(64);
  const int n = nr_csirs_blind_fp_match(acc.get(), 273, 382, out.data(), (int)out.size());
  out.resize(n > 0 ? n : 0);
  // Rows 16/17 with l1 = l0 + 2 occupy the same four consecutive symbols as row 18.
  EXPECT_EQ(n, 3);
  EXPECT_TRUE(has(out, 18, 0x0F, 6, 0, 0));
  EXPECT_TRUE(has(out, 16, 0x0F, 6, 8, 0));
  EXPECT_TRUE(has(out, 17, 0x0F, 6, 8, 0));
  for (const auto &c : out) EXPECT_EQ(c.freq_density, 0) << "row " << (int)c.row;
}

TEST(CsirsBlindFootprint, RowEighteenDensityOneIsNotCrowdedOutByItsSubFootprints) {
  // REGRESSION (review of the first cut): a row-18 density-1 pattern admits 66+ narrower fits
  // (rows 6-15 inside it). Enumerating narrow rows first filled the 64-entry scratch before rows
  // 15-18 were reached, so the maximality filter never saw the truth and returned rows 11-14 --
  // which the confirm path could then confirm with symbols 8-9 missing from rate matching.
  const nr_csirs_candidate_t truth = cand_of(18, 0x0F, 6, 0, 2);   // cdm8, symbols 6-9, all RBs
  Res r{};
  ASSERT_EQ(nr_csirs_blind_footprint(&truth, r.fp), 32);
  r.density = 2; r.period = 40; r.offset = 3;
  auto acc = std::make_unique<nr_csirs_blind_fp_t>();
  run_visits(acc.get(), {r}, 40 * 14 * 12);
  std::vector<nr_csirs_candidate_t> out(64);
  const int n = nr_csirs_blind_fp_match(acc.get(), 273, 382, out.data(), (int)out.size());
  out.resize(n > 0 ? n : 0);
  // Row 18 plus its same-footprint twins (rows 16/17 with l1 = l0 + 2), nothing narrower.
  EXPECT_EQ(n, 3);
  EXPECT_TRUE(has(out, 18, 0x0F, 6, 0, 2));
  EXPECT_TRUE(has(out, 17, 0x0F, 6, 8, 2));
  EXPECT_TRUE(has(out, 16, 0x0F, 6, 8, 2));
  for (const auto &c : out) {
    uint16_t m[NR_CSIRS_BLIND_NSYM];
    ASSERT_EQ(nr_csirs_blind_footprint(&c, m), 32) << "row " << (int)c.row << " is a sub-footprint";
  }
}

TEST(CsirsBlindFootprint, RowsOneToFiveAndAperiodicEnergyProduceNoWideCandidate) {
  // A cell with only a TRS pair and a row-5 CQI resource must leave the search exactly as it was.
  Res trs4{}, trs8{}, r5{};
  nr_csirs_candidate_t c = cand_of(1, 0x4, 4, 0, 3);
  nr_csirs_blind_footprint(&c, trs4.fp);
  c = cand_of(1, 0x4, 8, 0, 3);
  nr_csirs_blind_footprint(&c, trs8.fp);
  c = cand_of(5, 0x1, 6, 0);
  nr_csirs_blind_footprint(&c, r5.fp);
  trs4.density = trs8.density = r5.density = 2;
  trs4.period = trs8.period = 40; trs4.offset = trs8.offset = 31;
  r5.period = 160; r5.offset = 31;
  auto acc = std::make_unique<nr_csirs_blind_fp_t>();
  // ...plus type-2 double-symbol DM-RS-like pairs (subcarriers 0,1,6,7 on symbols 11,12) at
  // traffic-driven slots: exactly a row-7/8 footprint, but not periodic, so it must not match.
  run_visits(acc.get(), {trs4, trs8, r5}, 160 * 14 * 6, 3, 0x00C3, 11);
  std::vector<nr_csirs_candidate_t> out(64);
  EXPECT_EQ(nr_csirs_blind_fp_match(acc.get(), 273, 382, out.data(), (int)out.size()), 0);
  // Control: the SAME pair energy made periodic IS matched, so it was periodicity that rejected it.
  Res dm{};
  for (int l = 11; l <= 12; l++) dm.fp[l] = 0x00C3;
  dm.density = 2; dm.period = 40; dm.offset = 9;
  auto acc2 = std::make_unique<nr_csirs_blind_fp_t>();
  run_visits(acc2.get(), {dm}, 40 * 14 * 12);
  const int n2 = nr_csirs_blind_fp_match(acc2.get(), 273, 382, out.data(), (int)out.size());
  out.resize(n2 > 0 ? n2 : 0);
  EXPECT_TRUE(has(out, 7, 0x9, 11, 0, 2));
}

TEST(CsirsBlindFootprint, AppendSkipsDuplicatesAndRespectsCapacity) {
  auto st = std::make_unique<nr_csirs_blind_state_t>();
  const int n0 = nr_csirs_blind_init(st.get(), 273, 382);
  const nr_csirs_candidate_t c = cand_of(16, 0x17, 5, 9);
  EXPECT_EQ(nr_csirs_blind_append(st.get(), &c), n0);
  EXPECT_EQ(st->n, n0 + 1);
  nr_csirs_candidate_t again = c;
  again.scramb_id = 7;   // IDSWEEP may have patched the stored copy; still the same resource
  EXPECT_EQ(nr_csirs_blind_append(st.get(), &again), -1);
  EXPECT_EQ(st->n, n0 + 1);
  st->n = NR_CSIRS_BLIND_MAX_CAND;
  const nr_csirs_candidate_t d = cand_of(17, 0x17, 5, 9);
  EXPECT_EQ(nr_csirs_blind_append(st.get(), &d), -1);
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
  // A cell carries several CSI-RS resources (OCUDU bed: TRS l4, TRS l8, a CQI row 2, a ZP), and
  // PDSCH is rate-matched around ALL of them -- so a confirmation no longer ends the search: the
  // confirmed candidate simply leaves the rotation (it was 2 == "stops rotating" before 2026-09-27).
  for (int k = 0; k < st.n + 2; k++)
    EXPECT_NE(nr_csirs_blind_next(&st), 2);
  // And it formats straight back out as a csirs_monitor line.
  char buf[128];
  ASSERT_GT(nr_csirs_blind_format(c, p, o, buf, sizeof(buf)), 0);
  EXPECT_NE(std::string(buf).find(":20:13"), std::string::npos);
}

// ---- several resources per cell (OCUDU ZMQ bed, 2026-09-27) --------------------------------------
/* Truth from that bed's RRC Setup: TRS = row 1 at symbols 4 and 8 in BOTH slot 2 and slot 3 of a
 * 40-slot period (38.214 5.1.6.1.1: a TRS set spans two consecutive slots), NZP row 2 and ZP row 4 in
 * slot 4. The gNB drops the MCS one step in exactly those slots (10 instead of 11), and every such
 * grant failed its CRC because the passive rate-matched around none of it. */

TEST(CsirsBlindPeriod, TrsPairOneSlotApartIsTwoPhasesOfOnePeriod) {
  const uint32_t hits[] = {2, 3, 42, 43, 82, 83};
  uint16_t p = 0, o = 0;
  EXPECT_FALSE(nr_csirs_blind_infer_period(hits, 6, 3, &p, &o));  // no single phase explains it
  uint16_t off[2] = {0, 0};
  int n_off = 0;
  ASSERT_TRUE(nr_csirs_blind_infer_period2(hits, 6, 3, &p, off, &n_off));
  EXPECT_EQ(p, 40);
  ASSERT_EQ(n_off, 2);
  EXPECT_EQ(off[0], 2);
  EXPECT_EQ(off[1], 3);
}

TEST(CsirsBlindPeriod, TwoPhaseInferencePrefersASinglePhase) {
  const uint32_t hits[] = {13, 33, 53, 73};
  uint16_t p = 0, off[2] = {0, 0};
  int n_off = 0;
  ASSERT_TRUE(nr_csirs_blind_infer_period2(hits, 4, 3, &p, off, &n_off));
  EXPECT_EQ(p, 20);
  EXPECT_EQ(n_off, 1);
  EXPECT_EQ(off[0], 13);
}

TEST(CsirsBlindPeriod, DuplicateSlotsAreNotIndependentPhaseRepeats) {
  const uint32_t hits[] = {2, 2, 43, 43};
  uint16_t p = 0, off[2] = {};
  int n_off = 0;
  EXPECT_FALSE(nr_csirs_blind_infer_period2(hits, 4, 3, &p, off, &n_off));
}

TEST(CsirsBlindPeriod, TrsPairMayStraddleThePeriodBoundary) {
  const uint32_t hits[] = {39, 40, 79, 80, 119, 120};
  uint16_t p = 0, off[2] = {};
  int n_off = 0;
  ASSERT_TRUE(nr_csirs_blind_infer_period2(hits, 6, 3, &p, off, &n_off));
  EXPECT_EQ(p, 40);
  EXPECT_EQ(n_off, 2);
  EXPECT_EQ(off[0], 0);
  EXPECT_EQ(off[1], 39);
}

TEST(CsirsBlindPeriod, TwoPhaseInferenceRejectsInvalidEvidenceThreshold) {
  const uint32_t hits[] = {2, 3, 42, 43};
  EXPECT_FALSE(nr_csirs_blind_infer_period2(hits, 4, 1, nullptr, nullptr, nullptr));
}

TEST(CsirsBlindFeed, DuplicateSlotCannotConfirmAResource) {
  auto st = std::make_unique<nr_csirs_blind_state_t>();
  ASSERT_GT(nr_csirs_blind_init(st.get(), 51, 1), 0);
  nr_csirs_blind_feed(st.get(), 0, 2, 6.0, 4.0 / 3.0);
  nr_csirs_blind_feed(st.get(), 0, 42, 6.0, 4.0 / 3.0);
  EXPECT_FALSE(nr_csirs_blind_feed(st.get(), 0, 42, 6.0, 4.0 / 3.0));
  EXPECT_EQ(st->n_conf, 0);
  EXPECT_EQ(st->n_hit_slot[0], 2);
}

TEST(CsirsBlindFeed, RotationVisitsEveryLegalPhaseAfterAConfirmation) {
  for (const int population : {5, 6, 9, 11}) {
    auto st = std::make_unique<nr_csirs_blind_state_t>();
    ASSERT_GT(nr_csirs_blind_init(st.get(), 51, 1), population);
    st->n = population;
    for (uint32_t slot : {2u, 42u, 82u})
      nr_csirs_blind_feed(st.get(), 0, slot, 6.0, 4.0 / 3.0);
    ASSERT_EQ(st->n_conf, 1);
    // Test the longest legal period, not the period of this particular deployment.
    bool seen[NR_CSIRS_BLIND_MAX_CAND][640] = {};
    for (uint32_t slot = 0; slot < 640u * (population + 5); slot++) {
      const int idx = nr_csirs_blind_next(st.get());
      ASSERT_GE(idx, 1) << "population=" << population << " slot=" << slot;
      seen[idx][slot % 640] = true;
    }
    for (int idx = 1; idx < population; idx++)
      for (int phase = 0; phase < 640; phase++)
        ASSERT_TRUE(seen[idx][phase]) << "population=" << population << " candidate=" << idx << " phase=" << phase;
  }
}

TEST(CsirsBlindZp, StructuralHoleCannotRenewItsOwnPinForever) {
  auto st = std::make_unique<nr_csirs_blind_state_t>();
  ASSERT_GT(nr_csirs_blind_init(st.get(), 51, 1), 3);
  nr_csirs_blind_pin(st.get(), 2, 3);
  bool left = false;
  for (uint32_t slot = 0; slot < NR_CSIRS_BLIND_PIN_CONFIRM_CALLS + 10; slot++) {
    const int idx = nr_csirs_blind_next(st.get());
    nr_csirs_blind_zp_feed(st.get(), idx, slot, idx == 2 ? 0.98 : 0.02, 0.02);
    left |= idx != 2;
  }
  EXPECT_TRUE(left);
  EXPECT_EQ(st->n_conf, 0);
}

TEST(CsirsBlindPeriod, ASecondPhaseNeedsItsOwnRepeat) {
  // One stray hit must not be promoted to a second phase: each phase needs >= 2 hits.
  const uint32_t hits[] = {13, 33, 53, 54, 73};
  uint16_t p = 0, off[2] = {0, 0};
  int n_off = 0;
  if (nr_csirs_blind_infer_period2(hits, 5, 3, &p, off, &n_off)) {
    for (int i = 0; i < 5; i++) {
      EXPECT_TRUE(hits[i] % p == off[0] || (n_off == 2 && hits[i] % p == off[1])) << i;
    }
  }
  EXPECT_FALSE(n_off == 2 && p == 20);
}

TEST(CsirsBlindFeed, KeepsSearchingAndConfirmsEveryResourceOfTheCell) {
  auto st = std::make_unique<nr_csirs_blind_state_t>();
  ASSERT_GT(nr_csirs_blind_init(st.get(), 51, 1), 10);
  for (int k = 0; k < 4; k++)
    nr_csirs_blind_feed(st.get(), 2, (uint32_t)(40 * k + 4), 6.0, 4.0 / 3.0);   // row-2-like, slot 4
  ASSERT_TRUE(nr_csirs_blind_is_confirmed(st.get(), 2));
  const uint32_t trs[] = {2, 3, 42, 43, 82, 83};
  for (uint32_t s : trs)
    nr_csirs_blind_feed(st.get(), 5, s, 6.0, 4.0 / 3.0);                        // TRS, slots 2+3
  ASSERT_TRUE(nr_csirs_blind_is_confirmed(st.get(), 5));
  EXPECT_EQ(st->n_conf, 2);
  EXPECT_EQ(st->confirmed, 2);   // the legacy single-resource fields keep the FIRST confirmation
  for (int k = 0; k < 2 * st->n; k++) {
    const int idx = nr_csirs_blind_next(st.get());
    EXPECT_NE(idx, 2);
    EXPECT_NE(idx, 5);
  }
  // Which resources occur in which slot -- what the rate-matcher asks every grant.
  int got[NR_CSIRS_BLIND_MAX_CONF];
  EXPECT_EQ(nr_csirs_blind_occurring(st.get(), 122, got, NR_CSIRS_BLIND_MAX_CONF), 1);  // 122 = 40*3+2
  EXPECT_EQ(got[0], 5);
  EXPECT_EQ(nr_csirs_blind_occurring(st.get(), 123, got, NR_CSIRS_BLIND_MAX_CONF), 1);
  EXPECT_EQ(got[0], 5);
  EXPECT_EQ(nr_csirs_blind_occurring(st.get(), 124, got, NR_CSIRS_BLIND_MAX_CONF), 1);
  EXPECT_EQ(got[0], 2);
  EXPECT_EQ(nr_csirs_blind_occurring(st.get(), 125, got, NR_CSIRS_BLIND_MAX_CONF), 0);
}

TEST(CsirsBlindFeed, AHitPinsTheCandidateSoItsRepeatsAreSeen) {
  // Round-robin revisits a candidate every n slots, i.e. at a FIXED phase of the period when n shares
  // a factor with it -- a resource first seen by luck might never be seen again. A hit pins it.
  auto st = std::make_unique<nr_csirs_blind_state_t>();
  ASSERT_GT(nr_csirs_blind_init(st.get(), 51, 1), 10);
  nr_csirs_blind_feed(st.get(), 7, 4, 6.0, 4.0 / 3.0);
  EXPECT_EQ(nr_csirs_blind_next(st.get()), 7);
  EXPECT_EQ(nr_csirs_blind_next(st.get()), 7);
}

TEST(CsirsBlindFeed, SearchContinuesUntilCapacityOrAllCandidatesConfirmed) {
  auto st = std::make_unique<nr_csirs_blind_state_t>();
  ASSERT_GT(nr_csirs_blind_init(st.get(), 51, 1), 10);
  EXPECT_GE(nr_csirs_blind_next(st.get()), 0);  // nothing confirmed: search never ends on its own
  for (int k = 0; k < 4; k++)
    nr_csirs_blind_feed(st.get(), 2, (uint32_t)(40 * k + 4), 6.0, 4.0 / 3.0);
  ASSERT_EQ(st->n_conf, 1);
  for (int calls = 0; calls < st->n * 81; calls++)
    ASSERT_GE(nr_csirs_blind_next(st.get()), 0);
  st->n_conf = NR_CSIRS_BLIND_MAX_CONF;
  EXPECT_EQ(nr_csirs_blind_next(st.get()), -1);
  st->n_conf = st->n = 1;
  EXPECT_EQ(nr_csirs_blind_next(st.get()), -1);
}

TEST(CsirsBlindZp, ConfirmsSeveralHoles) {
  auto st = std::make_unique<nr_csirs_blind_state_t>();
  ASSERT_GT(nr_csirs_blind_init(st.get(), 24, 1), 3);
  for (uint32_t slot = 0; slot < 1600; slot++) {
    nr_csirs_blind_zp_feed(st.get(), 0, slot, (slot % 40 == 4) ? 0.98 : 0.02, 0.02);
    nr_csirs_blind_zp_feed(st.get(), 1, slot + 1, ((slot + 1) % 20 == 9) ? 0.98 : 0.02, 0.02);
  }
  EXPECT_TRUE(nr_csirs_blind_is_confirmed(st.get(), 0));
  EXPECT_TRUE(nr_csirs_blind_is_confirmed(st.get(), 1));
  EXPECT_EQ(st->n_conf, 2);
}

// ---- pinning ------------------------------------------------------------------------------------

TEST(CsirsBlindPin, PinnedCandidateIsServedUntilBudgetThenRoundRobinResumes) {
  auto st = std::make_unique<nr_csirs_blind_state_t>();
  ASSERT_GT(nr_csirs_blind_init(st.get(), 273, 2), 10);
  nr_csirs_blind_pin(st.get(), 7, 3);
  EXPECT_EQ(nr_csirs_blind_next(st.get()), 7);
  EXPECT_EQ(nr_csirs_blind_next(st.get()), 7);
  EXPECT_EQ(nr_csirs_blind_next(st.get()), 7);
  EXPECT_EQ(nr_csirs_blind_next(st.get()), 0);  // budget spent: round-robin resumes from its cursor
  EXPECT_EQ(nr_csirs_blind_next(st.get()), 1);
}

TEST(CsirsBlindPin, PinnedPeriodicResourceConfirmsWithinThreePeriods) {
  auto st = std::make_unique<nr_csirs_blind_state_t>();
  ASSERT_GT(nr_csirs_blind_init(st.get(), 273, 2), 10);
  const int truth = 7, period = 40, offset = 2;
  nr_csirs_blind_pin(st.get(), truth, NR_CSIRS_BLIND_PIN_CONFIRM_CALLS);
  uint32_t s = 0;
  for (; s < 4 * 640; s++) {
    const int idx = nr_csirs_blind_next(st.get());
    const double z = (idx == truth && s % period == offset) ? 6.0 : 1.0;  // bar is 3 x 4/3 = 4
    if (nr_csirs_blind_feed(st.get(), idx, s, z, 4.0 / 3.0))
      break;
  }
  uint16_t p = 0, o = 0;
  ASSERT_NE(nr_csirs_blind_confirmed(st.get(), &p, &o), nullptr);
  EXPECT_EQ(st->confirmed, truth);
  EXPECT_EQ(p, period);
  EXPECT_EQ(o, offset);
  EXPECT_LE(s, (uint32_t)(2 * period + offset));  // hits at 2, 42, 82
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
  // Measure every slot so all strict-divisor-only occasions are distinguishable.
  for (uint32_t slot = 0; slot < 1600; slot++) {
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

TEST(CsirsBlindZp, SparsePilotsDoNotConfirmOrExportZeroPowerGeometry) {
  std::vector<int16_t> rx, ref;
  for (uint32_t slot = 0; slot < 100; slot++) {
    zp_symbol(false, false, rx, ref);
    if (slot % 20 == 13) {
      // A periodically scheduled pilot in an otherwise quiet symbol. There is no data hole:
      // the candidate's k=0 REs have exactly the same background as most other REs.
      std::fill(rx.begin(), rx.end(), 2);
      for (int rb = 0; rb < 4; rb++)
        rx[2 * (12 * rb + 4)] = rx[2 * (12 * rb + 4) + 1] = 700;
    }
    const double score = nr_csirs_blind_zero_score(rx.data(), ref.data(), 48);
    EXPECT_EQ(nr_csirs_blind_rt_test_zp_feed(slot, score, slot == 0), 0) << "slot=" << slot << " score=" << score;
  }
}

TEST(CsirsBlindZp, BoostedPilotsDoNotConfirmOrExportZeroPowerGeometry) {
  std::vector<int16_t> rx, ref;
  for (uint32_t slot = 0; slot < 100; slot++) {
    zp_symbol(false, false, rx, ref);
    if (slot % 20 == 13) {
      // All candidate REs still carry full-power data. One boosted off-pattern pilot per RB
      // must not manufacture a ZP hole by raising the off-pattern arithmetic mean.
      for (int rb = 0; rb < 4; rb++)
        rx[2 * (12 * rb + 4)] = rx[2 * (12 * rb + 4) + 1] = 5000;
    }
    const double score = nr_csirs_blind_zero_score(rx.data(), ref.data(), 48);
    EXPECT_EQ(nr_csirs_blind_rt_test_zp_feed(slot, score, slot == 0), 0) << "slot=" << slot << " score=" << score;
  }
}

TEST(CsirsBlindZp, RepeatedStructuralObservationsDoNotConfirmOrExport) {
  // Every distinct observed slot has the same structural hole. Multiple occasions inspecting
  // one slot must not turn its hit fraction into 1/4 and accept an apparent periodic phase.
  for (uint32_t slot : {13u, 33u, 53u, 73u}) {
    for (int duplicate = 0; duplicate < 4; duplicate++) {
      EXPECT_EQ(nr_csirs_blind_rt_test_zp_feed(slot, 0.98, slot == 13 && duplicate == 0), 0);
    }
  }
}

TEST(CsirsBlindZp, PopulationThresholdCannotHideStructuralHoles) {
  auto st = std::make_unique<nr_csirs_blind_state_t>();
  ASSERT_GT(nr_csirs_blind_init(st.get(), 24, 17), 0);
  // The candidate is a hole in every observation. A varying population threshold suppresses
  // some detections; those suppressed holes are not evidence of occupied candidate REs.
  for (uint32_t slot = 0; slot < 100; slot++) {
    const double population = slot % 20 == 13 ? 0.02 : 0.98;
    EXPECT_FALSE(nr_csirs_blind_zp_feed(st.get(), 0, slot, 0.98, population));
  }
  EXPECT_EQ(st->n_conf, 0);
}

TEST(CsirsBlindZp, PopulationSuppressedRawHoleDoesNotContradictPeriodicEvidence) {
  auto st = std::make_unique<nr_csirs_blind_state_t>();
  ASSERT_GT(nr_csirs_blind_init(st.get(), 24, 17), 0);
  for (uint32_t slot = 0; slot <= 713; slot++) {
    const bool hole = slot % 20 == 13;
    // Both required symbols remain raw holes at 53; only the population threshold
    // suppresses that hit. Ordinary off-phase data rules out a structural hole.
    const double score = hole ? 0.98 : 0.0;
    const double other_score = hole ? 0.75 : 0.0;
    const double population = slot == 53 ? 0.98 : 0.02;
    EXPECT_EQ(nr_csirs_blind_zp_feed_pair(st.get(), 0, slot, score, other_score, population), slot == 713)
        << "slot=" << slot;
  }
  EXPECT_EQ(st->tried[0], 714u);
  EXPECT_EQ(st->zp_holes[0], 36u);
  EXPECT_EQ(st->hits[0], 35u);
  EXPECT_EQ(st->period, 20);
  EXPECT_EQ(st->offset, 13);
}

TEST(CsirsBlindZp, PeriodicHoleUnderDataStillConfirmsAndExports) {
  std::vector<int16_t> rx, ref;
  bool exported = false;
  for (uint32_t slot = 0; slot < 800; slot++) {
    const bool hole = slot % 20 == 13;
    zp_symbol(hole, false, rx, ref);
    const double score = nr_csirs_blind_zero_score(rx.data(), ref.data(), 48);
    const int count = nr_csirs_blind_rt_test_zp_feed(slot, score, slot == 0);
    if (!hole) {
      EXPECT_EQ(count, 0);
    }
    exported |= count == 1;
  }
  EXPECT_TRUE(exported);
}

TEST(CsirsBlindZp, CompletePatternSurvivesVariableDataAndIsolatedFadedSamples) {
  std::vector<int16_t> rx, ref;
  zp_symbol(true, false, rx, ref);
  for (int k = 0; k < 48; k++) {
    if (k % 12 == 0)
      continue;
    // Unequal QAM amplitudes and RB gains: aggregate each class before comparing, so
    // the one faded sample per class cannot be mistaken for a whole extra quiet tone.
    const int16_t amp = k / 12 == 0 ? 0 : 100 * (1 + k / 12) * (1 + 2 * (k % 4));
    rx[2 * k] = amp;
    rx[2 * k + 1] = -amp;
  }
  EXPECT_GT(nr_csirs_blind_zero_score(rx.data(), ref.data(), 48), 0.99);
}

TEST(CsirsBlindRuntime, Row5FirstSymbolHoleCannotExportTwoSymbols) {
  for (uint32_t slot = 0; slot < 100; slot++) {
    int mask = 0;
    EXPECT_EQ(nr_csirs_blind_rt_test_slot(5, slot, slot % 20 == 13 ? 1 : 0, 0, slot == 0, &mask), 0)
        << "slot=" << slot;
  }
}

TEST(CsirsBlindRuntime, Row5PeriodicHolesOnBothSymbolsExport) {
  bool exported = false;
  for (uint32_t slot = 0; slot < 800; slot++) {
    const bool hole = slot % 20 == 13;
    int mask = 0;
    const int n = nr_csirs_blind_rt_test_slot(5, slot, hole ? 3 : 0, 0, slot == 0, &mask);
    ASSERT_GE(n, 0);
    if (!hole) {
      EXPECT_EQ(n, 0);
    }
    exported |= n == 1;
  }
  EXPECT_TRUE(exported);
}

TEST(CsirsBlindRuntime, Row2PeriodicHoleStillExportsThroughSlotPath) {
  bool exported = false;
  for (uint32_t slot = 0; slot < 800; slot++) {
    const bool hole = slot % 20 == 13;
    int mask = 0;
    const int n = nr_csirs_blind_rt_test_slot(2, slot, hole ? 1 : 0, 0, slot == 0, &mask);
    ASSERT_GE(n, 0);
    EXPECT_EQ(mask, 1 << 7);
    if (!hole) {
      EXPECT_EQ(n, 0);
    }
    exported |= n == 1;
  }
  EXPECT_TRUE(exported);
}

TEST(CsirsBlindRuntime, ExactZeroRow4PeriodicHoleStillReachesIndependentZpScoring) {
  bool exported = false;
  for (uint32_t slot = 0; slot < 800; slot++) {
    const bool hole = slot % 20 == 13;
    int mask = 0;
    const int n = nr_csirs_blind_rt_test_slot_pattern(4, slot, hole ? 1 : 0, 0,
                                                       4 /* exact zero on the full port union */,
                                                       slot == 0, &mask);
    ASSERT_GE(n, 0);
    EXPECT_EQ(mask, 1 << 7);
    if (hole)
      EXPECT_GT(nr_csirs_blind_rt_test_union_score(), 0.99) << slot;
    else
      EXPECT_EQ(n, 0) << slot;
    exported |= n == 1;
  }
  EXPECT_TRUE(exported);
}

TEST(CsirsBlindRuntime, ConfirmedZpWithdrawsAfterTwoOccupiedOccasionsAndRecovers) {
  for (int row : {2, 4}) {
    for (uint32_t slot = 0; slot <= 1473; slot++) {
      int mask = 0;
      const bool hole = slot % 20 == 13 && slot != 713 && slot != 733;
      const int n = nr_csirs_blind_rt_test_slot(row, slot, hole, 0, slot == 0, &mask);
      ASSERT_EQ(nr_csirs_blind_rt_test_nzp_confirmed(), 0); // recovery has no active NZP at this geometry
      const bool exported = slot % 20 == 13 && (slot == 693 || slot == 713 || slot >= 1453);
      EXPECT_EQ(n, exported ? 1 : 0) << "row=" << row << " slot=" << slot;
    }
  }
}

TEST(CsirsBlindRuntime, ConfirmedZpUnscorableOccasionDoesNotRevoke) {
  for (int row : {2, 4}) {
    for (uint32_t slot = 0; slot <= 753; slot++) {
      int mask = 0;
      const bool hole = slot % 20 == 13 && slot != 713 && slot != 733;
      const int n = nr_csirs_blind_rt_test_slot(row, slot, hole, slot == 733, slot == 0, &mask);
      EXPECT_EQ(n, slot >= 693 && slot % 20 == 13 ? 1 : 0) << "row=" << row << " slot=" << slot;
    }
  }
}

TEST(CsirsBlindZp, ConfirmedZpPopulationSuppressionAndDuplicatesAreInconclusive) {
  auto st = std::make_unique<nr_csirs_blind_state_t>();
  nr_csirs_blind_init(st.get(), 24, 17);
  for (uint32_t slot = 0; slot <= 693; slot++)
    nr_csirs_blind_zp_feed(st.get(), 0, slot, slot % 20 == 13 ? 0.98 : 0.0, 0.02);
  ASSERT_TRUE(nr_csirs_blind_is_confirmed(st.get(), 0));
  nr_csirs_blind_zp_feed(st.get(), 0, 713, 0.0, 0.02);
  nr_csirs_blind_zp_feed(st.get(), 0, 713, 0.0, 0.02);
  nr_csirs_blind_zp_feed(st.get(), 0, 733, 0.98, 0.9);
  EXPECT_TRUE(nr_csirs_blind_is_confirmed(st.get(), 0));
  nr_csirs_blind_zp_feed(st.get(), 0, 753, 0.0, 0.02);
  EXPECT_FALSE(nr_csirs_blind_is_confirmed(st.get(), 0));
  nr_csirs_blind_zp_feed(st.get(), 0, 753, 0.98, 0.02);
  EXPECT_EQ(st->n_hit_slot[0], 0); // compaction/reset must not make this same slot fresh evidence
}

TEST(CsirsBlindZp, ConfirmedZpCompactionPreservesOtherResourceAndLegacyView) {
  auto st = std::make_unique<nr_csirs_blind_state_t>();
  nr_csirs_blind_init(st.get(), 24, 17);
  for (uint32_t slot = 0; slot <= 1453; slot++) {
    nr_csirs_blind_zp_feed(st.get(), 0, slot,
                         slot % 20 == 13 && slot != 713 && slot != 733 ? 0.98 : 0.0, 0.02);
    nr_csirs_blind_zp_feed(st.get(), 1, slot, slot % 20 == 14 ? 0.98 : 0.0, 0.02);
    if (slot == 733) {
      EXPECT_EQ(st->n_conf, 1);
      EXPECT_EQ(st->confirmed, 1);
      EXPECT_EQ(st->period, 20);
      EXPECT_EQ(st->offset, 14);
      int index = -1;
      EXPECT_EQ(nr_csirs_blind_occurring(st.get(), 734, &index, 1), 1);
      EXPECT_EQ(index, 1);
    }
  }
  EXPECT_EQ(st->n_conf, 2);
  EXPECT_TRUE(nr_csirs_blind_is_confirmed(st.get(), 0));
  EXPECT_TRUE(nr_csirs_blind_is_confirmed(st.get(), 1));
}

TEST(CsirsBlindRuntime, ZpLifecycleTelemetryReportsEpochHitsAndRevocationOnce) {
  int mask = 0;
  nr_csirs_blind_rt_test_slot(2, 0, 0, 0, 1, &mask);
  nr_csirs_blind_rt_test_logging(1);
  testing::internal::CaptureStdout();
  for (uint32_t slot = 1; slot <= 733; slot++) {
    const bool hole = slot % 20 == 13 && slot <= 693;
    nr_csirs_blind_rt_test_slot(2, slot, hole, 0, 0, &mask);
    nr_csirs_blind_rt_test_slot(2, slot, hole, 0, 0, &mask);
  }
  nr_csirs_blind_rt_test_logging(0);
  const std::string log = testing::internal::GetCapturedStdout();
  EXPECT_NE(log.find("ZP_EVIDENCE"), std::string::npos) << log;
  EXPECT_NE(log.find("hits=13,33,53"), std::string::npos) << log;
  EXPECT_NE(log.find("ZP REVOKED idx=0 abs_slot=733"), std::string::npos) << log;
  const auto at = log.find("ZP REVOKED");
  if (at != std::string::npos) {
    EXPECT_EQ(log.find("ZP REVOKED", at + 1), std::string::npos) << log;
  }
}

TEST(CsirsBlindRuntime, ZpRevocationAffectsOnlySubsequentK0Decisions) {
  int mask = 0;
  for (uint32_t slot = 0; slot <= 732; slot++)
    nr_csirs_blind_rt_test_slot(2, slot, slot % 20 == 13 && slot <= 693, 0, slot == 0, &mask);
  const int decision_before_observation = nr_csirs_blind_rt_test_future_export(733);
  ASSERT_EQ(decision_before_observation, 1);
  nr_csirs_blind_rt_test_slot(2, 733, 0, 0, 0, &mask);
  EXPECT_EQ(nr_csirs_blind_rt_test_future_export(753), 0);
  EXPECT_EQ(decision_before_observation, 1);
}

TEST(CsirsBlindRuntime, ZpLifecycleDetailsAreBoundedAcrossRepeatedRevocations) {
  int mask = 0;
  nr_csirs_blind_rt_test_slot(2, 0, 0, 0, 1, &mask);
  nr_csirs_blind_rt_test_logging(1);
  testing::internal::CaptureStdout();
  for (uint32_t slot = 1; slot < 5000; slot++)
    nr_csirs_blind_rt_test_slot(2, slot, slot % 20 == 13 && slot % 1000 <= 893, 0, 0, &mask);
  nr_csirs_blind_rt_test_logging(0);
  const std::string log = testing::internal::GetCapturedStdout();
  size_t at = 0, count = 0;
  while ((at = log.find("ZP_EVIDENCE", at)) != std::string::npos) {
    count++;
    at++;
  }
  EXPECT_EQ(count, 32u);
  EXPECT_NE(log.find("detail=32/32"), std::string::npos);
  /* Cumulative contradicted-support debt deliberately stretches each later
   * reacquisition. Two complete revoke/recover cycles still exceed the detail
   * cap and prove lifetime telemetry survives across epochs. */
  EXPECT_NE(log.find("revocations=2"), std::string::npos) << log;
  EXPECT_NE(log.find("ZP CONFIRMED #0"), std::string::npos) << log;
}

namespace {
std::map<std::string, uint64_t> maintenance_base;
// Existing maintenance scenario offsets start after a full legal-lattice probation.
int maintenance_slot(int row, uint32_t relative_slot, int holes, int empty, int reset, int *mask) {
  return nr_csirs_blind_rt_test_slot(row, relative_slot + nr_csirs_blind_zp_lattice_horizon(),
                                    holes, empty, reset, mask);
}
void confirm_runtime_zp(int row) {
  int mask = 0;
  const uint32_t end = 53 + nr_csirs_blind_zp_lattice_horizon();
  for (uint32_t slot = 0; slot <= end; slot++) {
    const int n = nr_csirs_blind_rt_test_slot(row, slot, slot % 20 == 13 ? 3 : 0, 0, slot == 0, &mask);
    if (slot == end) {
      EXPECT_EQ(n, 1);
    }
  }
  maintenance_base.clear();
  for (const char *key : {"scheduled", "reached", "first_slot", "duplicate", "setup_invalid",
                          "rho_unscorable", "score_invalid", "population_unknown",
                          "population_suppressed", "qualified_hole", "occupied"})
    maintenance_base[key] = nr_csirs_blind_rt_test_maintenance_count(0, key);
}
std::string maintenance_summary(const std::string &log) {
  const size_t at = log.rfind("ZP_MAINT_SUMMARY idx=0 ");
  return at == std::string::npos ? "" : log.substr(at, log.find('\n', at) - at);
}
uint64_t diagnostic_count(const std::string &line, const std::string &key) {
  const size_t at = line.find(" " + key + "=");
  if (at == std::string::npos) { ADD_FAILURE() << "missing " << key << ": " << line; return 0; }
  return std::strtoull(line.c_str() + at + key.size() + 2, nullptr, 10);
}
uint64_t maintenance_delta(const std::string &line, const std::string &key) {
  const uint64_t value = diagnostic_count(line, key);
  EXPECT_GE(value, maintenance_base.at(key));
  return value - maintenance_base.at(key);
}
void balanced_maintenance(const std::string &line) {
  uint64_t sum = 0;
  for (const char *outcome : {"duplicate", "setup_invalid", "rho_unscorable", "score_invalid",
                            "population_unknown", "population_suppressed", "qualified_hole", "occupied"})
    sum += diagnostic_count(line, outcome);
  EXPECT_EQ(sum, diagnostic_count(line, "reached"));
  EXPECT_EQ(sum, diagnostic_count(line, "scheduled"));
}
}

TEST(CsirsBlindRuntime, MaintenanceOutcomesAreDisjointAndPopulationDoesNotHideOccupied) {
  confirm_runtime_zp(5);
  nr_csirs_blind_rt_test_logging(1);
  testing::internal::CaptureStdout();
  int mask = 0;
  nr_csirs_blind_rt_test_maintenance_control(1);
  EXPECT_EQ(maintenance_slot(5, 73, 3, 0, 0, &mask), 1); // unknown population
  EXPECT_EQ(maintenance_slot(5, 73, 3, 0, 0, &mask), 1); // duplicate takes precedence
  nr_csirs_blind_rt_test_maintenance_control(2);
  EXPECT_EQ(maintenance_slot(5, 93, 3, 0, 0, &mask), 1); // suppressed
  nr_csirs_blind_rt_test_maintenance_control(4);
  EXPECT_EQ(maintenance_slot(5, 113, 3, 0, 0, &mask), 1); // invalid row5 setup
  nr_csirs_blind_rt_test_maintenance_control(3);
  EXPECT_EQ(maintenance_slot(5, 133, 3, 1, 0, &mask), 1); // first symbol rho unavailable
  EXPECT_EQ(maintenance_slot(5, 153, 3, 2, 0, &mask), 1); // second symbol score unavailable
  EXPECT_EQ(maintenance_slot(5, 173, 3, 0, 0, &mask), 1); // qualified
  nr_csirs_blind_rt_test_maintenance_control(1);
  EXPECT_EQ(maintenance_slot(5, 193, 0, 0, 0, &mask), 1); // occupied despite unknown population
  nr_csirs_blind_rt_test_maintenance_control(2);
  nr_csirs_blind_rt_test_status_next();
  EXPECT_EQ(maintenance_slot(5, 213, 0, 0, 0, &mask), 0); // occupied despite suppression; revokes
  nr_csirs_blind_rt_test_logging(0);
  const std::string log = testing::internal::GetCapturedStdout();
  const std::string summary = maintenance_summary(log);
  ASSERT_FALSE(summary.empty()) << log;
  balanced_maintenance(summary);
  EXPECT_EQ(maintenance_delta(summary, "scheduled"), 9u);
  for (const char *outcome : {"duplicate", "setup_invalid", "population_unknown",
                            "population_suppressed", "qualified_hole"})
    EXPECT_EQ(maintenance_delta(summary, outcome), 1u) << outcome;
  EXPECT_EQ(maintenance_delta(summary, "rho_unscorable"), 0u);
  EXPECT_EQ(maintenance_delta(summary, "score_invalid"), 2u);
  EXPECT_EQ(maintenance_delta(summary, "occupied"), 2u);
  EXPECT_NE(summary.find("first_slot=" + std::to_string(maintenance_base["first_slot"]) + " last_slot=853"), std::string::npos);
  EXPECT_NE(summary.find("active=0"), std::string::npos);
  size_t at = 0, details = 0;
  while ((at = log.find("ZP_MAINT_DETAIL", at)) != std::string::npos) { details++; at++; }
  EXPECT_EQ(details, 5u); // qualified/occupied details already occurred during probation; no counter reset
}

TEST(CsirsBlindRuntime, MaintenanceExactZeroRow4Port0IsMeasuredAsOccupiedAndRevokes) {
  confirm_runtime_zp(4);
  nr_csirs_blind_rt_test_logging(1);
  testing::internal::CaptureStdout();
  int mask = 0;
  for (uint32_t slot : {73u, 93u}) {
    nr_csirs_blind_rt_test_status_next();
    EXPECT_EQ(nr_csirs_blind_rt_test_slot_pattern(4, slot + nr_csirs_blind_zp_lattice_horizon(), 0, 0, 3, 0, &mask),
              slot == 73 ? 1 : 0);
    EXPECT_DOUBLE_EQ(nr_csirs_blind_rt_test_union_score(), 0.5);
    EXPECT_EQ(nr_csirs_blind_rt_test_future_export(slot + 20), slot == 73 ? 1 : 0);
  }
  nr_csirs_blind_rt_test_logging(0);
  const std::string log = testing::internal::GetCapturedStdout();
  const std::string summary = maintenance_summary(log);
  ASSERT_FALSE(summary.empty()) << log;
  balanced_maintenance(summary);
  EXPECT_EQ(maintenance_delta(summary, "rho_unscorable"), 0u);
  EXPECT_EQ(maintenance_delta(summary, "occupied"), 2u);
  /* Occupied was already detailed during probation, so lifetime detail capping
   * suppresses a duplicate line; the summary and actual withdrawal are authoritative. */
  EXPECT_NE(log.find("ZP REVOKED"), std::string::npos) << log;
}

TEST(CsirsBlindRuntime, MaintenanceInvalidObservationsPrecedeSameSlotDuplicateClassification) {
  confirm_runtime_zp(5);
  nr_csirs_blind_rt_test_logging(1);
  testing::internal::CaptureStdout();
  nr_csirs_blind_rt_test_maintenance_control(3); // qualifying population
  int mask = 0;
  EXPECT_EQ(maintenance_slot(5, 73, 3, 0, 0, &mask), 1);
  EXPECT_EQ(maintenance_slot(5, 73, 3, 1, 0, &mask), 1); // rho fails before ZP feed
  EXPECT_EQ(maintenance_slot(5, 73, 3, 2, 0, &mask), 1); // invalid second-symbol ZP score
  nr_csirs_blind_rt_test_status_next();
  EXPECT_EQ(maintenance_slot(5, 73, 3, 0, 0, &mask), 1); // valid duplicate
  nr_csirs_blind_rt_test_logging(0);
  const std::string log = testing::internal::GetCapturedStdout();
  const std::string summary = maintenance_summary(log);
  ASSERT_FALSE(summary.empty()) << log;
  balanced_maintenance(summary);
  EXPECT_EQ(maintenance_delta(summary, "scheduled"), 4u);
  EXPECT_EQ(maintenance_delta(summary, "qualified_hole"), 1u);
  EXPECT_EQ(maintenance_delta(summary, "rho_unscorable"), 0u);
  EXPECT_EQ(maintenance_delta(summary, "score_invalid"), 2u);
  EXPECT_EQ(maintenance_delta(summary, "duplicate"), 1u);
  EXPECT_EQ(maintenance_delta(summary, "occupied"), 0u);
  EXPECT_NE(summary.find("epoch=1 active=1"), std::string::npos);
  EXPECT_EQ(nr_csirs_blind_rt_test_future_export(93), 1);
}

TEST(CsirsBlindRuntime, MaintenanceLifetimeSurvivesTwoResourceCompactionAndReconfirmation) {
  int mask = 0;
  nr_csirs_blind_rt_test_slot(2, 0, 0, 0, 1, &mask);
  nr_csirs_blind_rt_test_two_resources();
  nr_csirs_blind_rt_test_logging(1);
  testing::internal::CaptureStdout();
  std::map<std::string, uint64_t> before[2];
  for (uint32_t slot = 1; slot <= 1473; slot++) {
    const bool phase = slot % 20 == 13;
    const int holes = phase ? (slot == 713 || slot == 733 ? 2 : 3) : 0;
    if (slot == 1473) nr_csirs_blind_rt_test_status_next();
    const int n = nr_csirs_blind_rt_test_slot(2, slot, holes, 0, 0, &mask);
    if (slot == 693) {
      EXPECT_EQ(n, 2);
      for (int idx = 0; idx < 2; ++idx)
        for (const char *key : {"scheduled", "occupied", "qualified_hole", "first_slot"})
          before[idx][key] = nr_csirs_blind_rt_test_maintenance_count(idx, key);
    }
    if (slot == 733) {
      EXPECT_EQ(n, 1);
      EXPECT_EQ(nr_csirs_blind_rt_test_bank_index(0), 1);
    }
    if (slot == 1453 || slot == 1473) {
      EXPECT_EQ(n, 2);
    }
  }
  nr_csirs_blind_rt_test_logging(0);
  const std::string log = testing::internal::GetCapturedStdout();
  const std::string zero = maintenance_summary(log);
  balanced_maintenance(zero);
  // 2 export-maintenance slots +60 reacquisition slots +264 probation/divisor
  // probes +1 post-reconfirmation slot. Counters are never reset at compaction.
  EXPECT_EQ(diagnostic_count(zero, "scheduled") - before[0]["scheduled"], 327u);
  EXPECT_EQ(diagnostic_count(zero, "occupied") - before[0]["occupied"], 290u);
  EXPECT_EQ(diagnostic_count(zero, "qualified_hole") - before[0]["qualified_hole"], 37u);
  EXPECT_NE(zero.find("epoch=2 active=1"), std::string::npos);
  EXPECT_NE(zero.find("first_slot=" + std::to_string(before[0]["first_slot"]) + " last_slot=1473"), std::string::npos);
  const auto at = log.rfind("ZP_MAINT_SUMMARY idx=1 ");
  ASSERT_NE(at, std::string::npos) << log;
  const std::string one = log.substr(at, log.find('\n', at) - at);
  balanced_maintenance(one);
  EXPECT_EQ(diagnostic_count(one, "scheduled") - before[1]["scheduled"], 39u);
  EXPECT_EQ(diagnostic_count(one, "qualified_hole") - before[1]["qualified_hole"], 39u);
  EXPECT_EQ(diagnostic_count(one, "occupied") - before[1]["occupied"], 0u);
  EXPECT_NE(one.find("epoch=1 active=1"), std::string::npos);
  EXPECT_EQ(log.find("first_slot=NA last_slot=NA"), std::string::npos); // probation already measured before export
}

TEST(CsirsBlindRuntime, MaintenanceDetailsStayCappedButLifetimeSummariesContinueAcrossEpochs) {
  int mask = 0;
  nr_csirs_blind_rt_test_slot(2, 0, 0, 0, 1, &mask);
  nr_csirs_blind_rt_test_logging(1);
  testing::internal::CaptureStdout();
  for (uint32_t slot = 1; slot < 5000; slot++) {
    if (slot == 4999) nr_csirs_blind_rt_test_status_next();
    nr_csirs_blind_rt_test_slot(2, slot, slot % 20 == 13 && slot % 1000 <= 893, 0, 0, &mask);
  }
  nr_csirs_blind_rt_test_logging(0);
  const std::string log = testing::internal::GetCapturedStdout();
  size_t at = 0, details = 0, summaries = 0;
  while ((at = log.find("ZP_MAINT_DETAIL", at)) != std::string::npos) { details++; at++; }
  at = 0;
  while ((at = log.find("ZP_MAINT_SUMMARY", at)) != std::string::npos) { summaries++; at++; }
  EXPECT_EQ(details, 2u); // qualified+occupied once each across every later epoch
  EXPECT_GE(summaries, 5u); // repeated promotions/withdrawals plus the forced lifetime summary
  const std::string summary = maintenance_summary(log);
  balanced_maintenance(summary);
  EXPECT_GT(diagnostic_count(summary, "scheduled"), 2000u);
  EXPECT_GT(diagnostic_count(summary, "qualified_hole"), 200u);
  EXPECT_GT(diagnostic_count(summary, "occupied"), diagnostic_count(summary, "qualified_hole"));
  EXPECT_GE(diagnostic_count(summary, "epoch"), 2u);
  EXPECT_NE(summary.find("active=0"), std::string::npos);
  EXPECT_NE(summary.find("first_slot=57 last_slot=4999"), std::string::npos);
}

TEST(CsirsBlindRuntime, Row2PeriodicHalfCombCannotExportItsQuietSubset) {
  for (uint32_t slot = 0; slot < 100; slot++) {
    int mask = 0;
    EXPECT_EQ(nr_csirs_blind_rt_test_slot_pattern(2, slot, slot % 20 == 13 ? 1 : 0,
                                                 0, 1, slot == 0, &mask), 0)
        << "slot=" << slot;
    EXPECT_EQ(mask, 1 << 7);
  }
}

TEST(CsirsBlindRuntime, Row4CompletePatternIncludesEveryPortGroup) {
  bool exported = false;
  for (uint32_t slot = 0; slot < 800; slot++) {
    const bool hole = slot % 20 == 13;
    int mask = 0;
    const int n = nr_csirs_blind_rt_test_slot(4, slot, hole ? 1 : 0, 0, slot == 0, &mask);
    ASSERT_GE(n, 0);
    EXPECT_EQ(mask, 1 << 7);
    if (!hole) {
      EXPECT_EQ(n, 0);
    }
    exported |= n == 1;
  }
  EXPECT_TRUE(exported);
}

TEST(CsirsBlindRuntime, Row2OnPhaseGeometryRejectionPreventsConfirmation) {
  for (uint32_t slot = 0; slot <= 73; slot++) {
    int mask = 0;
    // Complete holes at 13/33/73 contradict the wider half-comb observed at 53.
    const int holes = slot % 20 == 13;
    EXPECT_EQ(nr_csirs_blind_rt_test_slot_pattern(2, slot, holes, 0, slot == 53,
                                                slot == 0, &mask), 0) << "slot=" << slot;
    EXPECT_EQ(mask, 1 << 7);
  }
}

TEST(CsirsBlindRuntime, Row4OnPhaseGeometryRejectionPreventsConfirmation) {
  for (uint32_t slot = 0; slot <= 73; slot++) {
    int mask = 0;
    EXPECT_EQ(nr_csirs_blind_rt_test_slot_pattern(4, slot, slot % 20 == 13, 0, slot == 53,
                                                slot == 0, &mask), 0) << "slot=" << slot;
    EXPECT_EQ(mask, 1 << 7);
  }
}

TEST(CsirsBlindRuntime, CompleteProbationConfirmsWithOffPhaseGeometryRejection) {
  for (int row : {2, 4}) {
    for (int reject_slot : {-1, 34}) {
      for (uint32_t slot = 0; slot <= 693; slot++) {
        int mask = 0;
        const bool rejected = static_cast<int>(slot) == reject_slot;
        const int holes = slot % 20 == 13 || rejected;
        EXPECT_EQ(nr_csirs_blind_rt_test_slot_pattern(row, slot, holes, 0, rejected,
                                                    slot == 0, &mask), slot == 693 ? 1 : 0)
            << "row=" << row << " reject_slot=" << reject_slot << " slot=" << slot;
      }
    }
  }
}

TEST(CsirsBlindZp, PhaseContradictionRecoversWithNewEvidence) {
  auto st = std::make_unique<nr_csirs_blind_state_t>();
  ASSERT_GT(nr_csirs_blind_init(st.get(), 24, 17), 0);
  for (uint32_t slot = 0; slot <= 753; slot++) {
    const bool hole = slot % 20 == 13 && slot != 53;
    const bool done = nr_csirs_blind_zp_feed(st.get(), 0, slot, hole ? 0.98 : 0.0, 0.02);
    EXPECT_EQ(done, slot == 753) << "slot=" << slot;
  }
  EXPECT_EQ(st->period, 20);
  EXPECT_EQ(st->offset, 13);
}

TEST(CsirsBlindZp, OnPhaseRejectionBeforeFirstHitIsOutsideEvidenceSpan) {
  auto st = std::make_unique<nr_csirs_blind_state_t>();
  ASSERT_GT(nr_csirs_blind_init(st.get(), 24, 17), 0);
  for (uint32_t slot = 0; slot <= 713; slot++) {
    const bool hole = slot % 20 == 13 && slot >= 33;
    EXPECT_EQ(nr_csirs_blind_zp_feed(st.get(), 0, slot, hole ? 0.98 : 0.0, 0.02), slot == 713);
  }
  EXPECT_EQ(st->period, 20);
  EXPECT_EQ(st->offset, 13);
}

TEST(CsirsBlindZp, UnscorableOnPhaseObservationIsNotAContradiction) {
  for (double invalid : {-1.0, static_cast<double>(NAN), static_cast<double>(INFINITY)}) {
    auto st = std::make_unique<nr_csirs_blind_state_t>();
    ASSERT_GT(nr_csirs_blind_init(st.get(), 24, 17), 0);
    for (uint32_t slot = 0; slot <= 713; slot++) {
      const double score = slot == 53 ? invalid : slot % 20 == 13 ? 0.98 : 0.0;
      EXPECT_EQ(nr_csirs_blind_zp_feed(st.get(), 0, slot, score, 0.02), slot == 713)
          << "slot=" << slot << " invalid=" << invalid;
    }
  }
}

TEST(CsirsBlindZp, ContradictionChecksBothInferredOffsets) {
  // Both sets infer period 20 with offsets 13 and 14, but have a missing middle
  // occurrence on a different offset. A single-offset-only veto would miss one.
  for (int missing : {33, 34}) {
    auto st = std::make_unique<nr_csirs_blind_state_t>();
    ASSERT_GT(nr_csirs_blind_init(st.get(), 24, 17), 0);
    for (uint32_t slot = 0; slot <= (missing == 33 ? 53u : 54u); slot++) {
      if (slot == 53 && missing == 34)
        continue; // unobserved, rather than a second contradiction
      const bool hole = slot == 13 || slot == 14
          || (missing == 33 ? slot == 34 || slot == 53 : slot == 33 || slot == 54);
      EXPECT_FALSE(nr_csirs_blind_zp_feed(st.get(), 0, slot, hole ? 0.98 : 0.0, 0.02))
          << "missing=" << missing << " slot=" << slot;
    }
  }
}

TEST(CsirsBlindZp, LongPeriodContradictionSurvivesDenseOffPhaseObservations) {
  auto st = std::make_unique<nr_csirs_blind_state_t>();
  ASSERT_GT(nr_csirs_blind_init(st.get(), 24, 17), 0);
  for (uint32_t slot = 0; slot <= 1933; slot++) {
    const bool hole = slot == 13 || slot == 653 || slot == 1933;
    EXPECT_FALSE(nr_csirs_blind_zp_feed(st.get(), 0, slot, hole ? 0.98 : 0.0, 0.02))
        << "slot=" << slot;
  }
}

TEST(CsirsBlindZp, LongPeriodCompleteEvidenceStillConfirms) {
  auto st = std::make_unique<nr_csirs_blind_state_t>();
  ASSERT_GT(nr_csirs_blind_init(st.get(), 24, 17), 0);
  for (uint32_t slot = 0; slot <= 3213; slot++) {
    EXPECT_EQ(nr_csirs_blind_zp_feed(st.get(), 0, slot, slot % 640 == 13 ? 0.98 : 0.0, 0.02),
              slot == 3213) << "slot=" << slot;
  }
  EXPECT_EQ(st->period, 640);
  EXPECT_EQ(st->offset, 13);
}

TEST(CsirsBlindZp, FullUnresolvedHitWindowRecoversWithNewEvidence) {
  auto st = std::make_unique<nr_csirs_blind_state_t>();
  ASSERT_GT(nr_csirs_blind_init(st.get(), 24, 17), 0);
  for (uint32_t slot = 0; slot <= 700; slot++) {
    const bool hole = (slot >= 13 && slot <= 20) || (slot >= 40 && slot % 20 == 0);
    EXPECT_EQ(nr_csirs_blind_zp_feed(st.get(), 0, slot, hole ? 0.98 : 0.0, 0.02), slot == 700)
        << "slot=" << slot;
  }
  EXPECT_EQ(st->period, 20);
  EXPECT_EQ(st->offset, 0);
}

TEST(CsirsBlindZp, PhaseStorageCoversLegalTableAndInitClearsEveryCandidate) {
  unsigned phase_bits = 0;
  for (unsigned period : nr_csirs_blind_periods)
    phase_bits += period;
  ASSERT_EQ(phase_bits, NR_CSIRS_BLIND_ZP_PHASE_BITS);
  static_assert(NR_CSIRS_BLIND_ZP_PHASE_WORDS == 22, "Bound ZP phase evidence to 176 bytes per candidate");
  auto st = std::make_unique<nr_csirs_blind_state_t>();
  for (auto &candidate : st->zp_rejected_phase)
    for (auto &word : candidate)
      word = UINT64_MAX;
  ASSERT_GT(nr_csirs_blind_init(st.get(), 24, 17), 0);
  for (const auto &candidate : st->zp_rejected_phase)
    for (uint64_t word : candidate)
      ASSERT_EQ(word, 0u);
  RecordProperty("state_bytes", static_cast<int>(sizeof(*st)));
  RecordProperty("phase_storage_bytes", static_cast<int>(sizeof(st->zp_rejected_phase)));
}

TEST(CsirsBlindRuntime, Row2PeriodicTwoToneHoleCannotExportItsSubset) {
  for (uint32_t slot = 0; slot < 100; slot++) {
    int mask = 0;
    EXPECT_EQ(nr_csirs_blind_rt_test_slot_pattern(2, slot, slot % 20 == 13 ? 1 : 0,
                                                 0, 2, slot == 0, &mask), 0)
        << "slot=" << slot;
  }
}

namespace {
std::string capture_zp_telemetry(int extra_holes, int duplicates, bool *exported) {
  int mask = 0;
  nr_csirs_blind_rt_test_slot_pattern(2, 0, 0, 0, extra_holes, 1, &mask);
  *exported = false;
  nr_csirs_blind_rt_test_logging(1);
  testing::internal::CaptureStdout();
  for (uint32_t call = 1; call < 20000; call++) {
    const uint32_t slot = call / duplicates;
    const int n = nr_csirs_blind_rt_test_slot_pattern(2, slot, slot % 20 == 13 ? 1 : 0,
                                                    0, extra_holes, 0, &mask);
    *exported |= n > 0;
  }
  nr_csirs_blind_rt_test_logging(0);
  return testing::internal::GetCapturedStdout();
}
}

TEST(CsirsBlindRuntime, HalfCombVetoTelemetryIsBoundedAndDeduplicated) {
  bool exported = false;
  const std::string log = capture_zp_telemetry(1, 2, &exported);
  EXPECT_FALSE(exported);
  const std::string event = "CSIRS_BLIND ZP_GEOMETRY_VETO";
  const auto first = log.find(event);
  EXPECT_NE(first, std::string::npos) << log;
  if (first != std::string::npos) {
    EXPECT_EQ(log.find(event, first + event.size()), std::string::npos) << log;
  }
  EXPECT_NE(log.find("row=2 fd=2 l0=7 l1=0 density=2 start_rb=0 nrb=4 abs_slot=13"), std::string::npos) << log;
  EXPECT_NE(log.find("old_score=0.999992 completeness_score=0.000000 null=0.000000"), std::string::npos) << log;
  EXPECT_NE(log.find("zp_geometry_veto=500"), std::string::npos) << log;
}

TEST(CsirsBlindRuntime, TwoToneVetoTelemetryCountsEveryDistinctSlot) {
  bool exported = false;
  const std::string log = capture_zp_telemetry(2, 1, &exported);
  EXPECT_FALSE(exported);
  EXPECT_NE(log.find("CSIRS_BLIND ZP_GEOMETRY_VETO"), std::string::npos) << log;
  EXPECT_NE(log.find("zp_geometry_veto=1000"), std::string::npos) << log;
}

TEST(CsirsBlindRuntime, CompleteHoleConfirmsWithoutGeometryVeto) {
  bool exported = false;
  const std::string log = capture_zp_telemetry(0, 1, &exported);
  EXPECT_TRUE(exported);
  EXPECT_NE(log.find("CSIRS_BLIND ZP CONFIRMED"), std::string::npos) << log;
  EXPECT_EQ(log.find("CSIRS_BLIND ZP_GEOMETRY_VETO"), std::string::npos) << log;
  EXPECT_NE(log.find("zp_geometry_veto=0"), std::string::npos) << log;
}

TEST(CsirsBlindRuntime, Row5UnmeasuredSecondSymbolCannotExport) {
  for (uint32_t slot = 0; slot < 100; slot++) {
    int mask = 0;
    EXPECT_EQ(nr_csirs_blind_rt_test_slot(5, slot, slot % 20 == 13 ? 1 : 0, 2, slot == 0, &mask), 0);
  }
}

TEST(CsirsBlindRuntime, Row5StructuralHoleOnEitherSymbolCannotExport) {
  for (int structural : {1, 2}) {
    for (uint32_t slot = 0; slot < 100; slot++) {
      int mask = 0;
      const int holes = structural | (slot % 20 == 13 ? 3 : 0);
      EXPECT_EQ(nr_csirs_blind_rt_test_slot(5, slot, holes, 0, slot == 0, &mask), 0)
          << "structural=" << structural << " slot=" << slot;
    }
  }
}

TEST(CsirsBlindRuntime, Row5DifferentPeriodicPhasesCannotExport) {
  for (uint32_t slot = 0; slot < 100; slot++) {
    int mask = 0;
    const int holes = (slot % 20 == 13 ? 1 : 0) | (slot % 20 == 14 ? 2 : 0);
    EXPECT_EQ(nr_csirs_blind_rt_test_slot(5, slot, holes, 0, slot == 0, &mask), 0);
  }
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
  /* Unrelated REs carry energy on air (noise, other channels). Exact zeros no longer model that: the
   * block correlator skips zero-energy blocks by design, so a zero-filled band stopped diluting it. */
  std::mt19937 gz(4242);
  std::normal_distribution<double> ndz(0.0, 500.0);
  for (int i = (int)(0.2 * n); i < n; i++) { rx[2 * i] = (int16_t)ndz(gz); rx[2 * i + 1] = (int16_t)ndz(gz); }

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

// Round-3 RED checkpoint: proposed policy, not implemented yet. These traces call
// the real production state machine; they do not emulate a replacement algorithm.
TEST(CsirsBlindZpProbation, DiscoveryNeedsIndependentHeldOutOccasions)
{
  auto st = std::make_unique<nr_csirs_blind_state_t>();
  ASSERT_GT(nr_csirs_blind_init(st.get(), 24, 17), 0);
  int premature = 0;
  for (uint32_t slot = 0; slot <= 2000; ++slot) {
    nr_csirs_blind_zp_feed(st.get(), 0, slot, slot % 20 == 13 ? 0.98 : 0.0, 0.02);
    premature += slot <= 53 && st->n_conf != 0;
  }
  // Discovery 13/33/53 must not export. A long stable positive control must
  // eventually resolve; do not specify a three-held-out promotion deadline.
  EXPECT_EQ(premature, 0);
  ASSERT_EQ(st->n_conf, 1);
  EXPECT_EQ(st->period, 20);
  EXPECT_EQ(st->offset, 13);
}

TEST(CsirsBlindZpProbation, DuplicatesAndUnknownsCannotFinishProbation)
{
  auto st = std::make_unique<nr_csirs_blind_state_t>();
  ASSERT_GT(nr_csirs_blind_init(st.get(), 24, 17), 0);
  int premature = 0;
  for (uint32_t slot = 0; slot <= 2500; ++slot) {
    const double score = slot > 53 && slot < 1000 && slot % 20 == 13
        ? NAN : slot % 20 == 13 ? 0.98 : 0.0;
    nr_csirs_blind_zp_feed(st.get(), 0, slot, score, 0.02);
    if (slot == 53) {
      for (int duplicate = 0; duplicate < 5; ++duplicate)
        nr_csirs_blind_zp_feed(st.get(), 0, slot, 0.98, 0.02);
    }
    premature += slot < 1000 && st->n_conf != 0;
  }
  // Unknown predicted occasions cannot complete probation. Later measured
  // consistent observations must still recover; duplicates never supply votes.
  EXPECT_EQ(premature, 0);
  ASSERT_EQ(st->n_conf, 1);
  EXPECT_EQ(st->period, 20);
}

TEST(CsirsBlindZpProbation, LongBurstsCannotResetAwayOccupiedHistory)
{
  auto st = std::make_unique<nr_csirs_blind_state_t>();
  ASSERT_GT(nr_csirs_blind_init(st.get(), 24, 17), 0);
  int exported = 0;
  for (uint32_t slot = 0; slot < 4000; ++slot) {
    // Eight plausible P20 hits (3 discovery +5 qualified held-out) followed by
    // two measured occupied occasions defeat a fixed three-held-out gate.
    // Repeated fresh epochs must not export any burst.
    const bool hole = slot % 20 == 13 && (slot / 20) % 10 < 8;
    nr_csirs_blind_zp_feed(st.get(), 0, slot, hole ? 0.98 : 0.0, 0.02);
    exported += st->n_conf != 0;
  }
  EXPECT_EQ(exported, 0);
  EXPECT_EQ(st->n_conf, 0);
}

TEST(CsirsBlindZpProbation, RepeatedRejectedTrialsAccumulateDebtBeforeTransientExport)
{
  auto st = std::make_unique<nr_csirs_blind_state_t>();
  ASSERT_GT(nr_csirs_blind_init(st.get(), 24, 17), 0);
  uint32_t slot = 0;
  for (int trial = 0; trial < 20; ++trial) {
    const uint32_t start = slot;
    for (; slot < start + 400; ++slot) {
      const uint32_t rel = slot - start;
      const bool hole = rel == 4 || rel == 84 || rel == 164 || rel == 244;
      nr_csirs_blind_zp_feed(st.get(), 0, slot, hole ? 0.98 : 0.0, 0.02);
      ASSERT_EQ(st->n_conf, 0) << "trial=" << trial << " slot=" << slot;
    }
  }
  const uint32_t transient_start = slot;
  for (; slot <= transient_start + 804; ++slot) {
    const bool hole = (slot - transient_start) % 80 == 4;
    nr_csirs_blind_zp_feed(st.get(), 0, slot, hole ? 0.98 : 0.0, 0.02);
    EXPECT_EQ(st->n_conf, 0) << "transient slot=" << slot;
  }
  // A genuinely stable continuation must not be blacklisted. It supplies enough
  // measured occasions to exceed all accumulated contradicted-trial evidence.
  for (; slot < transient_start + 12000 && st->n_conf == 0; ++slot)
    nr_csirs_blind_zp_feed(st.get(), 0, slot, slot % 80 == 4 ? 0.98 : 0.0, 0.02);
  EXPECT_EQ(st->n_conf, 1);
}

TEST(CsirsBlindZpProbation, MeasuredNewPeriodRecoversWithoutBlacklist)
{
  auto st = std::make_unique<nr_csirs_blind_state_t>();
  ASSERT_GT(nr_csirs_blind_init(st.get(), 24, 17), 0);
  for (uint32_t slot = 0; slot <= 2053; ++slot)
    nr_csirs_blind_zp_feed(st.get(), 0, slot, slot < 2014 && slot % 20 == 13 ? 0.98 : 0.0, 0.02);
  ASSERT_EQ(st->n_conf, 0); // occupied2033/2053 withdraw the old P20 hypothesis
  int premature = 0;
  for (uint32_t slot = 2054; slot <= 10000; ++slot) {
    nr_csirs_blind_zp_feed(st.get(), 0, slot, slot % 40 == 17 ? 0.98 : 0.0, 0.02);
    premature += slot <= 2137 && st->n_conf != 0;
  }
  // New discovery2057/2097/2137 is not held-out evidence. A long measured
  // P40 epoch must recover without hardcoding the previous P20 configuration.
  EXPECT_EQ(premature, 0);
  ASSERT_EQ(st->n_conf, 1);
  EXPECT_EQ(st->period, 40);
  EXPECT_EQ(st->offset, 17);
}

TEST(CsirsBlindZpProbation, UnmeasuredDivisorWithholdsHarmonicUntilResolved)
{
  auto st = std::make_unique<nr_csirs_blind_state_t>();
  ASSERT_GT(nr_csirs_blind_init(st.get(), 24, 17), 0);
  int premature = 0;
  for (uint32_t slot = 0; slot <= 3000; ++slot) {
    // P80-visible hits4/84/.../484 do not distinguish trueP80 from P40 with
    // unknown intermediate occasions44/124/.../964. Never export either claim
    // from that missing evidence. Qualified1004 resolves the interstitial phase;
    // subsequent P40-only and shared occasions independently validate it.
    double score = 0.0;
    if (slot % 40 == 4)
      score = slot < 1004 && slot % 80 == 44 ? NAN : 0.98;
    nr_csirs_blind_zp_feed(st.get(), 0, slot, score, 0.02);
    premature += slot < 1004 && st->n_conf != 0;
  }
  EXPECT_EQ(premature, 0);
  ASSERT_EQ(st->n_conf, 1);
  EXPECT_EQ(st->period, 40);
  EXPECT_EQ(st->offset, 4);
}

TEST(CsirsBlindZpProbation, LatticeMemoryAndSharedAdmissionAreBounded)
{
  unsigned horizon = 0, phase_bits = 0;
  for (unsigned p : nr_csirs_blind_periods) { horizon = std::max(horizon, p); phase_bits += p; }
  EXPECT_EQ(nr_csirs_blind_zp_lattice_horizon(), horizon);
  for (unsigned p : nr_csirs_blind_periods) EXPECT_EQ(horizon % p, 0u);
  EXPECT_EQ(phase_bits, NR_CSIRS_BLIND_ZP_PHASE_BITS);
  static_assert(sizeof(nr_csirs_zp_probation_t) <= NR_CSIRS_BLIND_ZP_PHASE_BITS * 4 + 64);
  auto st = std::make_unique<nr_csirs_blind_state_t>();
  ASSERT_GT(nr_csirs_blind_init(st.get(), 24, 17), 8);
  for (uint32_t slot = 0; slot <= 2000; ++slot)
    for (int idx = 0; idx <= NR_CSIRS_BLIND_MAX_CONF; ++idx)
      nr_csirs_blind_zp_feed(st.get(), idx, slot, slot % 20 == 13 ? 0.98 : 0.0, 0.02);
  EXPECT_EQ(st->n_conf, NR_CSIRS_BLIND_MAX_CONF);
  EXPECT_FALSE(nr_csirs_blind_is_confirmed(st.get(), NR_CSIRS_BLIND_MAX_CONF));
  int admitted = 0, due[NR_CSIRS_BLIND_MAX_CONF + 1];
  for (const auto &b : st->zp_bank) admitted += b.owner != 0;
  ASSERT_EQ(admitted, NR_CSIRS_BLIND_MAX_CONF);
  ASSERT_EQ(nr_csirs_blind_zp_due(st.get(), 2013, due, NR_CSIRS_BLIND_MAX_CONF + 1), admitted);
  for (int j = 0; j < NR_CSIRS_BLIND_MAX_CONF; ++j) {
    EXPECT_EQ(due[j], j); // deterministic admission order and exact membership
    for (int k = 0; k < j; ++k) EXPECT_NE(due[j], due[k]);
  }
}

TEST(CsirsBlindZpProbation, EachOffsetNeedsItsOwnProspectiveVotes)
{
  auto st = std::make_unique<nr_csirs_blind_state_t>();
  nr_csirs_blind_init(st.get(), 24, 17);
  for (uint32_t slot = 0; slot < 2000; ++slot) {
    const bool on = slot % 20 == 13 || slot % 20 == 14;
    const double score = slot > 54 && slot < 1000 && slot % 20 == 14 ? NAN : on ? 0.98 : 0.0;
    nr_csirs_blind_zp_feed(st.get(), 0, slot, score, 0.02);
    if (slot < 1000) {
      ASSERT_EQ(st->n_conf, 0) << slot;
    }
  }
  ASSERT_EQ(st->n_conf, 1);
  EXPECT_EQ(st->conf_n_off[0], 2);
  EXPECT_EQ(st->period, 20);
  EXPECT_EQ(st->conf_off[0][0], 13);
  EXPECT_EQ(st->conf_off[0][1], 14);
}

TEST(CsirsBlindZpProbation, OverflowAndBackwardsSlotsFailClosed)
{
  for (int mode = 0; mode < 4; ++mode) {
    auto st = std::make_unique<nr_csirs_blind_state_t>();
    nr_csirs_blind_init(st.get(), 24, 17);
    for (uint32_t slot = 0; slot <= 693; ++slot)
      nr_csirs_blind_zp_feed(st.get(), 0, slot, slot % 20 == 13 ? 0.98 : 0.0, 0.02);
    ASSERT_EQ(st->n_conf, 1);
    if (mode == 0) st->zp_failed_run[0] = UINT64_MAX;
    if (mode == 1) st->tried[0] = UINT32_MAX;
    const uint32_t slot = mode == 2 ? UINT32_MAX : mode == 3 ? 692 : 713;
    EXPECT_FALSE(nr_csirs_blind_zp_feed(st.get(), 0, slot, 0.98, 0.02));
    EXPECT_EQ(st->n_conf, 0);
    EXPECT_EQ(st->zp_failed_run[0], UINT64_MAX);
  }
}

TEST(CsirsBlindZpProbation, NzpStillUsesItsUnchangedThreeHitPolicy)
{
  auto st = std::make_unique<nr_csirs_blind_state_t>();
  nr_csirs_blind_init(st.get(), 24, 17);
  for (uint32_t slot = 0; slot <= 53; ++slot)
    EXPECT_EQ(nr_csirs_blind_feed(st.get(), 0, slot, slot % 20 == 13 ? 20.0 : 0.0, 1.0), slot == 53);
  EXPECT_EQ(st->n_conf, 1);
  for (const auto &b : st->zp_bank) EXPECT_EQ(b.owner, 0);
}

TEST(CsirsBlindZpProbation, RuntimeSchedulesUnexportedProbationAfterNzpRetiresDiscovery)
{
  int mask = 0;
  for (uint32_t slot = 0; slot <= 53; ++slot)
    EXPECT_EQ(nr_csirs_blind_rt_test_slot(2, slot, slot % 20 == 13, 0, slot == 0, &mask), 0);
  EXPECT_EQ(nr_csirs_blind_rt_test_future_export(73), 0);
  nr_csirs_blind_rt_test_retire_discovery();
  bool exported = false;
  for (uint32_t slot = 54; slot <= 800; ++slot) {
    int n = nr_csirs_blind_rt_test_slot(2, slot, slot % 20 == 13, 0, 0, &mask);
    if (slot < 693) {
      EXPECT_EQ(n, 0) << slot;
    }
    exported |= n == 1;
  }
  EXPECT_TRUE(exported);
}

TEST(CsirsBlindZpProbation, MaintenanceEvidenceCannotRepinEitherDiscoveryRotation)
{
  int mask = 0;
  for (uint32_t slot = 0; slot <= 73; ++slot) {
    const bool discovery_hole = slot == 13 || slot == 33 || slot == 53;
    nr_csirs_blind_rt_test_slot_pattern(2, slot, discovery_hole ? 1 : 0, 0, 0,
                                        slot == 0, &mask);
  }
  nr_csirs_blind_rt_test_clear_pins();
  // The contradicted admission now has period zero and is maintained every slot.
  // Its private due path already guarantees revisits; it must not seize either
  // ordinary discovery rotation when a new raw hole is observed.
  const uint64_t qualified_before = nr_csirs_blind_rt_test_maintenance_count(0, "qualified_hole");
  nr_csirs_blind_rt_test_slot_pattern(2, 74, 1, 0, 0, 0, &mask);
  EXPECT_EQ(nr_csirs_blind_rt_test_maintenance_count(0, "qualified_hole"), qualified_before + 1);
  EXPECT_EQ(nr_csirs_blind_rt_test_pin_left(0), 0u);
  EXPECT_EQ(nr_csirs_blind_rt_test_pin_left(1), 0u);
}

TEST(CsirsBlindZpProbation, RuntimeBudgetIsEightAdmittedPlusOneDiscoveryScore)
{
  int mask = 0;
  for (uint32_t slot = 0; slot <= 53; ++slot)
    nr_csirs_blind_rt_test_slot(2, slot, slot % 20 == 13, 0, slot == 0, &mask);
  nr_csirs_blind_rt_test_eight_probations();
  EXPECT_EQ(nr_csirs_blind_rt_test_slot(2, 73, 1, 0, 0, &mask), 0);
  EXPECT_EQ(nr_csirs_blind_rt_test_fep_calls(), NR_CSIRS_BLIND_MAX_CONF + 1);
  EXPECT_EQ(nr_csirs_blind_rt_test_future_export(93), 0);
}

TEST(CsirsBlindZpProbation, PopulationAndInvalidScoresAreNonVotes)
{
  auto st = std::make_unique<nr_csirs_blind_state_t>();
  nr_csirs_blind_init(st.get(), 24, 17);
  for (uint32_t slot = 0; slot <= 53; ++slot)
    nr_csirs_blind_zp_feed(st.get(), 0, slot, slot % 20 == 13 ? 0.98 : 0.0, 0.02);
  auto &bank = st->zp_bank[0];
  ASSERT_EQ(bank.owner, 1);
  nr_csirs_blind_zp_feed(st.get(), 0, 73, 0.98, -1.0);
  nr_csirs_blind_zp_feed(st.get(), 0, 93, 0.98, 0.9);
  nr_csirs_blind_zp_feed_pair(st.get(), 0, 113, 0.98, NAN, 0.02);
  EXPECT_EQ(bank.votes[0], 0u);
  EXPECT_EQ(st->zp_failed_run[0], 0u);
  EXPECT_EQ(bank.period, 20);
  nr_csirs_blind_zp_feed(st.get(), 0, 133, 0.98, 0.02);
  nr_csirs_blind_zp_feed(st.get(), 0, 133, 0.98, 0.02);
  EXPECT_EQ(bank.votes[0], 1u);
  EXPECT_EQ(st->n_conf, 0);
}

TEST(CsirsBlindZpProbation, MeasuredOccupiedInterstitialAllowsTrueLargerPeriod)
{
  auto st = std::make_unique<nr_csirs_blind_state_t>();
  nr_csirs_blind_init(st.get(), 24, 17);
  for (uint32_t slot = 0; slot <= 1004; ++slot)
    nr_csirs_blind_zp_feed(st.get(), 0, slot, slot % 80 == 4 ? 0.98 : 0.0, 0.02);
  ASSERT_EQ(st->n_conf, 1);
  EXPECT_EQ(st->period, 80);
  EXPECT_EQ(st->offset, 4);
}

TEST(CsirsBlindZpProbation, RevokedPromotionRaisesDurableRecoveryRequirement)
{
  auto st = std::make_unique<nr_csirs_blind_state_t>();
  nr_csirs_blind_init(st.get(), 24, 17);
  for (uint32_t slot = 0; slot <= 733; ++slot)
    nr_csirs_blind_zp_feed(st.get(), 0, slot, slot % 20 == 13 && slot <= 693 ? 0.98 : 0.0, 0.02);
  ASSERT_EQ(st->n_conf, 0);
  ASSERT_EQ(st->zp_failed_run[0], 32u);
  for (uint32_t slot = 734; slot <= 1453; ++slot) {
    nr_csirs_blind_zp_feed(st.get(), 0, slot, slot % 20 == 13 ? 0.98 : 0.0, 0.02);
    EXPECT_EQ(st->zp_failed_run[0], 32u);
    EXPECT_EQ(st->n_conf, slot == 1453 ? 1 : 0) << slot;
  }
}

TEST(CsirsBlindZpProbation, FailedAdmissionsCannotStarveLaterGeometryOrReadmission)
{
  auto st = std::make_unique<nr_csirs_blind_state_t>();
  nr_csirs_blind_init(st.get(), 24, 17);
  // Eight candidates reach discovery, then are contradicted. They must not own
  // all admission storage forever while a ninth consistent resource is observed.
  for (uint32_t slot = 0; slot <= 200; ++slot)
    for (int idx = 0; idx < NR_CSIRS_BLIND_MAX_CONF; ++idx)
      nr_csirs_blind_zp_feed(st.get(), idx, slot, slot <= 53 && slot % 20 == 13 ? 0.98 : 0.0, 0.02);
  for (uint32_t slot = 201; slot <= 6000; ++slot) {
    for (int idx = 0; idx < NR_CSIRS_BLIND_MAX_CONF; ++idx)
      nr_csirs_blind_zp_feed(st.get(), idx, slot, 0.0, 0.02);
    nr_csirs_blind_zp_feed(st.get(), 8, slot, slot % 20 == 13 ? 0.98 : 0.0, 0.02);
  }
  EXPECT_TRUE(nr_csirs_blind_is_confirmed(st.get(), 8));
  int evicted = -1;
  for (int idx = 0; idx < 8; ++idx) {
    bool present = false;
    for (const auto &b : st->zp_bank) present |= b.owner == idx + 1;
    if (!present) { evicted = idx; break; }
  }
  ASSERT_GE(evicted, 0);
  for (uint32_t slot = 6001; slot <= 12000; ++slot)
    nr_csirs_blind_zp_feed(st.get(), evicted, slot, slot % 40 == 17 ? 0.98 : 0.0, 0.02);
  EXPECT_TRUE(nr_csirs_blind_is_confirmed(st.get(), evicted));
  EXPECT_TRUE(nr_csirs_blind_is_confirmed(st.get(), 8));
}
