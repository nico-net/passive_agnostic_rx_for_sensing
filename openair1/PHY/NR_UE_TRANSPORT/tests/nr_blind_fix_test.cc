/* BLINDFIX 2026-10-12 (F1 F3 F6 F7 F8): unit tests of the statistical replacements in nr_blind_stats.h and the UL BWP
 * enumeration. Flags are off in production; these test the decision rules themselves. */
#include <gtest/gtest.h>
#include <cmath>
#include <random>
#include <vector>
extern "C" {
#include "nr_blind_stats.h"
#include "nr_pdcch_scan_score.h"
#include "nr_pdcch_ul_layout_lock.h"
}

TEST(BlindFix, BinomialTail)
{
  EXPECT_NEAR(nbs_binom_sf(10, 10, 0.5), 1.0 / 1024, 1e-12);
  EXPECT_NEAR(nbs_binom_sf(1, 300, 0.01), 1 - std::pow(0.99, 300), 1e-9);
  EXPECT_EQ(nbs_binom_sf(0, 5, 0.3), 1.0);
}

TEST(BlindFix, SweepWinnerMarginalLinkNeedsNoRateFloor)
{
  /* old rule: rate >= 2 % -> 14/1000 = 1.4 % never wins. New: significant against a runner-up with 0 passes. */
  EXPECT_FALSE(nbs_sweep_winner(5, 1000, 0, 1000, 4)); /* 2^-5 = 0.03: not significant */
  EXPECT_TRUE(nbs_sweep_winner(14, 1000, 0, 1000, 4));
  EXPECT_FALSE(nbs_sweep_winner(8, 300, 7, 300, 4)); /* a tie in a field the traffic does not exercise stays undecided */
  EXPECT_TRUE(nbs_sweep_winner(228, 300, 3, 300, 233));
  EXPECT_FALSE(nbs_sweep_winner(1, 300, 0, 300, 233)); /* one pass among 233 hypotheses is not conclusive */
  EXPECT_TRUE(nbs_sweep_winner(20, 300, 0, 300, 233));
}

TEST(BlindFix, CfarScoreFalseAlarmAndPower)
{
  std::mt19937 g(7);
  std::normal_distribution<double> N(0.0, 1.0);
  for (double scale : {1.0, 37.0, 300.0}) { /* scale invariance */
    for (int nbits : {216, 432, 1728}) {
      const double thr = nbs_cfar_score_min(nbits, 3.09);
      int fa = 0, det = 0;
      const int T = 20000;
      std::vector<int16_t> x(nbits);
      for (int t = 0; t < T; t++) {
        for (auto &v : x) v = (int16_t)std::lround(scale * 8 * N(g));
        fa += nr_pdcch_scan_score_scalar(x.data(), nbits) < thr ? 0 : 1; /* noise passes the gate */
        for (auto &v : x) v = (int16_t)std::lround(scale * 8 * (((g() & 1) ? 1.0 : -1.0) * 1.6 + N(g))); /* QPSK, 4 dB per bit */
        det += nr_pdcch_scan_score_scalar(x.data(), nbits) >= thr;
      }
      EXPECT_LT(fa / (double)T, 0.004) << "n=" << nbits << " scale=" << scale; /* design 1e-3, delta-method skew allowance */
      EXPECT_GT(det / (double)T, nbits >= 432 ? 0.9 : 0.5) << "n=" << nbits;
    }
  }
}

TEST(BlindFix, JointSlotsCooccurrence)
{
  const uint16_t a[2] = {24, 104}, c1[1] = {24}, c2[1] = {50};
  EXPECT_EQ(nbs_joint_slots(160, a, 2, 160, c1, 1), 4);  /* co-scheduled in one burst: 4 repeats per 640 slots */
  EXPECT_EQ(nbs_joint_slots(160, a, 2, 160, c2, 1), 0);  /* never together: no block inference */
  EXPECT_EQ(nbs_joint_slots(0, a, 2, 160, c1, 1), 0);
}

TEST(BlindFix, McsTableEvidence)
{
  nbs_table_ev_t e = {{0, 0, 0}};
  EXPECT_EQ(nbs_table_ev_best(&e), -1);
  for (int i = 0; i < 12; i++) nbs_table_ev_feed(&e, 1, true);
  EXPECT_EQ(nbs_table_ev_best(&e), 1);
  nbs_table_ev_t f = {{5, 7, 0}};
  EXPECT_EQ(nbs_table_ev_best(&f), -1);
  nbs_table_ev_t g = {{0, 0, 0}};
  nbs_table_ev_feed(&g, 1, false); /* MCS read the same in every table: no evidence */
  EXPECT_EQ(g.pass[1], 0u);
}

TEST(BlindFix, UlBwpEnumerationIsComplete)
{
  const int car = 106;
  const int w = nr_ul_layout_riv_bits(100); /* 13 */
  const uint32_t cnt = nr_ul_layout_enum_count(car, w, 0);
  uint32_t brute = 0;
  bool has_95_at_10 = false, beyond_legacy = false;
  for (int n = 1; n <= car; n++)
    if (nr_ul_layout_riv_bits(n) == w) brute += car - n + 1;
  EXPECT_EQ(cnt, brute);
  uint16_t n0, s0;
  ASSERT_TRUE(nr_ul_layout_enum_at(car, w, 0, 0, &n0, &s0));
  EXPECT_EQ(n0, car); /* full carrier first */
  EXPECT_EQ(s0, 0);
  for (uint32_t i = 0; i < cnt; i++) {
    uint16_t n, s;
    ASSERT_TRUE(nr_ul_layout_enum_at(car, w, 0, i, &n, &s));
    EXPECT_EQ(nr_ul_layout_riv_bits(n), w);
    EXPECT_LE(s + n, car);
    has_95_at_10 |= n == 95 && s == 10;
    beyond_legacy |= s > 5;
  }
  EXPECT_TRUE(has_95_at_10);
  EXPECT_TRUE(beyond_legacy);
  uint16_t n, s;
  EXPECT_FALSE(nr_ul_layout_enum_at(car, w, 0, cnt, &n, &s));
  /* evidence prune: an observed RIV of 7000 makes every N with N(N+1)/2 <= 7000 (N <= 101) illegal; 100 PRB hypotheses go */
  EXPECT_EQ(nr_ul_layout_enum_count(car, w, 5200), 15u); /* N = 102..106 only */
}

static void nr_dci_bits_set(nr_dci_bits_t *b, int len, int i, int v)
{
  const int low = len - 1 - i;
  b->w[low / 64] = (b->w[low / 64] & ~(UINT64_C(1) << (low % 64))) | ((uint64_t)v << (low % 64));
}

/* F5: synthetic DCI bank, MCS field at a chosen offset; HARQ pid steps 1..3 and RV mostly 0 at that offset only. */
static int fit_mcs_bank(bool structured, int nrnti, int per, unsigned seed)
{
  std::mt19937 g(seed);
  const uint16_t len = 40;
  std::vector<nr_ul_layout_dci_t> bank;
  std::vector<int> pid(nrnti, 0);
  for (int k = 0; k < per; k++)
    for (int r = 0; r < nrnti; r++) {
      nr_ul_layout_dci_t d = {};
      d.rnti = (uint16_t)(100 + r);
      d.slot = k * 20 + r;
      for (int i = 0; i < len; i++) nr_dci_bits_set(&d.p, len, i, (int)(g() & 1));
      if (structured) {
        pid[r] = (pid[r] + 1 + (int)(g() % 3)) % 16;
        const unsigned rv = (g() % 10) ? 0 : (g() & 3);
        for (int b = 0; b < 2; b++) nr_dci_bits_set(&d.p, len, 20 + 6 + b, (rv >> (1 - b)) & 1);
        for (int b = 0; b < 4; b++) nr_dci_bits_set(&d.p, len, 20 + 8 + b, (pid[r] >> (3 - b)) & 1);
      }
      bank.push_back(d);
    }
  return nr_ul_layout_fit_mcs(bank.data(), (int)bank.size(), len, nullptr, nullptr);
}

TEST(BlindFix, McsOffsetFitIsStatistical)
{
  setenv("ISAC_UL_LOCK_MCS_STAT", "1", 1);
  EXPECT_EQ(fit_mcs_bank(true, 4, 100, 1), 20);   /* 400 DCIs */
  EXPECT_EQ(fit_mcs_bank(true, 3, 30, 2), 20);    /* 90 DCIs, 87 HARQ pairs: locks on significance, not a fixed score */
  EXPECT_EQ(fit_mcs_bank(false, 4, 100, 3), -1);  /* random bits: no offset */
  unsetenv("ISAC_UL_LOCK_MCS_STAT");
}

extern "C" {
void *uniqCfg = nullptr;
/* link stubs: the Technique A code in the monitor library is not exercised here */
unsigned int *nr_gold_pdcch(int, int, unsigned short, int, int) { return nullptr; }
void nr_pdcch_dmrs_ref(const unsigned int *, void *, unsigned short) {}
void *get_softmodem_params(void) { static char z[65536]; return z; }
void exit_function(const char *, const char *, int, const char *s, int) { fprintf(stderr, "%s\n", s ? s : ""); abort(); }
}
int main(int argc, char **argv)
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
