#include <cmath>
#include <complex>
#include <cstdlib>
#include <iostream>
#include <random>
#include <vector>
#include <gtest/gtest.h>
extern "C" {
#include "nr_pdsch_ptrs_detect.h"
#include "nr_pdsch_ptrs_unav.h"
}

typedef std::complex<double> cd;
static constexpr int SYM_SZ = 4096;

struct Grid {
  int n_ant, nb_rb, re0, start, nsym;
  uint16_t dmrs;
  uint16_t rnti;
  std::vector<int16_t> x;  // [ant][14][SYM_SZ][2]
  size_t stride() const { return 14 * SYM_SZ; }
};

// A slot of 16/64-QAM data on Rayleigh channels, a common phase error per symbol (random walk) and, when `has`, PT-RS with
// a QPSK value fixed per subcarrier across symbols (the DM-RS value of TS 38.211 7.4.1.2.2) on (K, k_re, L).
static Grid make_grid(std::mt19937 &g, int n_ant, int nb_rb, int start, int nsym, uint16_t dmrs, uint16_t rnti, bool has,
                      nr_ptrs_hyp_t h, double snr_db, double cpe_sigma, double ptrs_boost_db = 0.0)
{
  Grid G{n_ant, nb_rb, 37, start, nsym, dmrs, rnti, std::vector<int16_t>((size_t)n_ant * 14 * SYM_SZ * 2, 0)};
  std::normal_distribution<double> n01(0.0, 1.0);
  std::uniform_int_distribution<int> q16(0, 3), q4(0, 1);
  const int nre = nb_rb * 12;
  const uint16_t pm = has ? nr_ptrs_symbol_mask(start, nsym, h.L, dmrs) : 0;
  std::vector<cd> p(nre);
  for (auto &v : p) v = cd(q4(g) ? 1 : -1, q4(g) ? 1 : -1) / std::sqrt(2.0);
  std::vector<std::vector<cd>> H(n_ant, std::vector<cd>(nre));
  for (auto &r : H) for (auto &v : r) v = cd(n01(g), n01(g)) / std::sqrt(2.0);
  const double sigma = std::pow(10.0, -snr_db / 20.0) / std::sqrt(2.0);
  const double amp = 3000.0, boost = std::pow(10.0, ptrs_boost_db / 20.0);
  double phi = 0.0;
  for (int s = start; s < start + nsym; s++) {
    phi += cpe_sigma * n01(g);
    const cd rot = std::polar(1.0, phi);
    for (int k = 0; k < nre; k++) {
      cd sym;
      const bool is_p = has && ((pm >> s) & 1) && nr_ptrs_is_re(k, rnti, h.K, nb_rb, h.k_re);
      if (is_p) sym = p[k] * boost;
      else {
        const int a = 2 * q16(g) - 3, b = 2 * q16(g) - 3;  // 16-QAM
        sym = cd(a, b) / std::sqrt(10.0);
      }
      for (int a = 0; a < n_ant; a++) {
        const cd y = amp * (H[a][k] * sym * rot) + amp * sigma * cd(n01(g), n01(g));
        const size_t f = (size_t)a * 14 * SYM_SZ + (size_t)s * SYM_SZ + (G.re0 + k) % SYM_SZ;
        G.x[2 * f] = (int16_t)std::lrint(y.real());
        G.x[2 * f + 1] = (int16_t)std::lrint(y.imag());
      }
    }
  }
  return G;
}

static nr_ptrs_det_t run(const Grid &G)
{
  nr_ptrs_det_t d;
  nr_ptrs_detect(G.x.data(), G.n_ant, G.stride(), SYM_SZ, G.re0, G.nb_rb, G.rnti, G.start, G.nsym, G.dmrs, NR_PTRS_DET_ALPHA, &d);
  return d;
}

TEST(PtrsDetect, FindsEveryDensityAndOffset) {
  // The best hypothesis must be the true one in EVERY trial; it must also be significant on the wide grants PT-RS is used on.
  std::mt19937 g(1);
  int tot = 0, right = 0, fired24 = 0, n24 = 0, fired100 = 0, n100 = 0;
  for (int K : {2, 4}) for (int L : {1, 2, 4}) for (int k_re : {0, 2, 7, 11}) for (int nb_rb : {24, 100}) {
    const nr_ptrs_hyp_t h{(uint8_t)K, (uint8_t)L, (uint8_t)k_re};
    for (int rep = 0; rep < 3; rep++) {
      const uint16_t rnti = 0x4601 + 7 * rep;
      Grid G = make_grid(g, 4, nb_rb, 1, 13, 0x804, rnti, true, h, 12.0, 0.08);
      const nr_ptrs_det_t d = run(G);
      tot++;
      const bool same = d.hyp.K == h.K && d.hyp.L == h.L && d.hyp.k_re == h.k_re;
      right += same;
      (nb_rb == 24 ? n24 : n100)++;
      (nb_rb == 24 ? fired24 : fired100) += d.fired && same;
      if (!same || (nb_rb == 100 && !d.fired)) {
        ADD_FAILURE() << "K=" << K << " L=" << L << " k=" << k_re << " rb=" << nb_rb << " got fired=" << d.fired << " K="
                      << (int)d.hyp.K << " L=" << (int)d.hyp.L << " k=" << (int)d.hyp.k_re << " log10p=" << d.log10_p;
      }
    }
  }
  std::cerr << "[ MEASURED ] 12 dB, 4 RX, 13 symbols: best hypothesis right " << right << "/" << tot << "; fired at alpha 1e-4: 100 PRB "
            << fired100 << "/" << n100 << ", 24 PRB " << fired24 << "/" << n24 << "\n";
  EXPECT_EQ(right, tot);
}

TEST(PtrsDetect, NoFalseAlarmOnDataOnly) {
  std::mt19937 g(2);
  int fa = 0, n = 0;
  for (int nb_rb : {8, 24, 60, 100, 273}) for (int rep = 0; rep < 40; rep++) {
    nr_ptrs_hyp_t none{2, 1, 0};
    Grid G = make_grid(g, 4, nb_rb, rep % 3, 13 - rep % 3, 0x804, (uint16_t)(0x4600 + rep), false, none, 6.0 + rep % 10, 0.05);
    if (nb_rb > 100 && rep > 4) continue;
    const nr_ptrs_det_t d = run(G);
    n++;
    fa += d.fired;
  }
  std::cerr << "[ MEASURED ] false alarms on PT-RS-free grants: " << fa << "/" << n << " (alpha 1e-4)\n";
  EXPECT_EQ(fa, 0);
}

TEST(PtrsDetect, RecallFallsGracefullyWithSnr) {
  std::mt19937 g(3);
  const nr_ptrs_hyp_t h{2, 1, 2};
  for (double snr : {-3.0, 0.0, 5.0, 12.0}) {
    int hit = 0;
    for (int rep = 0; rep < 20; rep++) {
      Grid G = make_grid(g, 4, 100, 1, 13, 0x804, 0x4601, true, h, snr, 0.08);
      const nr_ptrs_det_t d = run(G);
      hit += d.fired && d.hyp.K == 2 && d.hyp.L == 1 && d.hyp.k_re == 2;
    }
    std::cerr << "[ MEASURED ] K=2 L=1, 100 PRB, 4 RX, snr " << snr << " dB: recall " << hit << "/20\n";
    if (snr >= 5.0) {
      EXPECT_EQ(hit, 20);
    }
  }
}

TEST(PtrsDetect, SingleAntennaStillDetectsWideGrants) {
  std::mt19937 g(4);
  const nr_ptrs_hyp_t h{4, 2, 6};
  int hit = 0;
  for (int rep = 0; rep < 10; rep++) {
    Grid G = make_grid(g, 1, 273, 1, 13, 0x804, 0x4601 + rep, true, h, 15.0, 0.05);
    const nr_ptrs_det_t d = run(G);
    hit += d.fired && d.hyp.K == 4 && d.hyp.L == 2 && d.hyp.k_re == 6;
  }
  EXPECT_EQ(hit, 10);
}

TEST(PtrsDetect, SymbolMaskMatchesTheSpecRule) {
  // 38.214 5.1.6.3: PT-RS at l_ref + i*L from the allocation start; a DM-RS symbol met in a step restarts the cadence there.
  EXPECT_EQ(nr_ptrs_symbol_mask(1, 13, 1, 0x804), (uint16_t)(0x3FFE & ~0x804));
  // L=2, S=2..13, DM-RS at 2 and 11: PT-RS at 4,6,8,10 (l_ref=2), DM-RS 11 restarts: 13
  EXPECT_EQ(nr_ptrs_symbol_mask(2, 12, 2, 0x804), (uint16_t)((1u << 4) | (1u << 6) | (1u << 8) | (1u << 10) | (1u << 13)));
  // L=4, S=0..13, DM-RS at 2 only: l_ref 0: 0, 4 (window 1..4 holds DM-RS 2 -> l_ref=2, i=1): 6, 10
  EXPECT_EQ(nr_ptrs_symbol_mask(0, 14, 4, 0x004), (uint16_t)((1u << 0) | (1u << 6) | (1u << 10)));
}

// G (TS 38.212 5.4.2.1) = Nl * Qm * (N_sc * N_sym - N_DMRS - N_PTRS): the PT-RS share must equal the REs actually removed.
TEST(PtrsDetect, UnavResEqualsTheRemovedResAndOnlyItMakesGMatch) {
  std::mt19937 g(5);
  for (int rep = 0; rep < 300; rep++) {
    const int K = (g() & 1) ? 4 : 2, L = 1 << (g() % 3), nb_rb = 1 + g() % 273, start = g() % 3, nsym = 4 + g() % (14 - start - 3);
    const uint16_t rnti = (uint16_t)g();
    const uint16_t dmrs = (uint16_t)((1u << (2 + g() % 2)) | ((g() & 1) ? 1u << 11 : 0));
    const nr_ptrs_hyp_t h{(uint8_t)K, (uint8_t)L, (uint8_t)(g() % 12)};
    // brute force: the REs of the grid that are PT-RS
    const uint16_t pm = nr_ptrs_symbol_mask(start, nsym, L, dmrs);
    uint32_t brute = 0;
    for (int s = start; s < start + nsym; s++)
      if ((pm >> s) & 1)
        for (int k = 0; k < nb_rb * 12; k++) brute += nr_ptrs_is_re(k, rnti, K, nb_rb, h.k_re);
    const uint32_t model = nr_ptrs_hyp_unav_res(&h, nb_rb, start, nsym, dmrs);
    ASSERT_EQ(model, brute) << "K=" << K << " L=" << L << " rb=" << nb_rb << " S=" << start << " n=" << nsym << " dmrs=" << dmrs
                            << " rnti=" << rnti;
    const long grid = (long)nb_rb * 12 * nsym - (long)__builtin_popcount(dmrs & (((1u << nsym) - 1) << start)) * nb_rb * 6;
    EXPECT_EQ(grid - model, grid - brute);
    if (brute) {
      EXPECT_NE(grid - 0, grid - (long)brute) << "without the exclusion G is too large by the PT-RS REs";
    }
  }
}

TEST(PtrsClass, BandsAreMonotoneAndBounded) {
  EXPECT_EQ(nr_ptrs_class_index(0, 1), 0);
  EXPECT_LT(nr_ptrs_class_index(0, 50), nr_ptrs_class_index(0, 270));
  EXPECT_LT(nr_ptrs_class_index(5, 270), nr_ptrs_class_index(28, 1));
  EXPECT_LT(nr_ptrs_class_index(31, 273), NR_PTRS_CLASS_N);
}

TEST(PtrsClass, ModalHypothesisNeedsRepeatedPluralityVotes) {
  nr_ptrs_class_t c; nr_ptrs_class_init(&c);
  nr_ptrs_det_t d{}; d.fired = true; d.hyp = {2, 1, 3};
  nr_ptrs_hyp_t h;
  nr_ptrs_class_det_feed(&c, &d);
  EXPECT_FALSE(nr_ptrs_class_hyp(&c, &h)) << "one vote is not evidence";
  nr_ptrs_class_det_feed(&c, &d);
  ASSERT_TRUE(nr_ptrs_class_hyp(&c, &h));
  EXPECT_EQ(h.K, 2); EXPECT_EQ(h.L, 1); EXPECT_EQ(h.k_re, 3);
  nr_ptrs_det_t e = d; e.hyp = {4, 2, 5};
  for (int i = 0; i < 3; i++) nr_ptrs_class_det_feed(&c, &e);  // a rival catches up: no plurality
  EXPECT_FALSE(nr_ptrs_class_hyp(&c, &h));
}

TEST(PtrsClass, CrcArbitrationLatchesPresentWhenOnlyThePtrsArmDecodes) {
  nr_ptrs_class_t c; nr_ptrs_class_init(&c);
  std::mt19937 g(6);
  int n = 0;
  while (c.latched < 0 && n < 200) {
    nr_ptrs_class_crc_feed(&c, 0, false);                 // decoding without the exclusion fails
    nr_ptrs_class_crc_feed(&c, nr_ptrs_class_pick_mode(&c), (g() % 100) < 70);      // with it, 70 %
    n++;
  }
  EXPECT_EQ(c.latched, 1);
  std::cerr << "[ MEASURED ] PT-RS class latched 'present' after " << n << " grant pairs\n";
}

TEST(PtrsClass, CrcArbitrationLatchesAbsentWhenOnlyThePlainArmDecodes) {
  nr_ptrs_class_t c; nr_ptrs_class_init(&c);
  std::mt19937 g(7);
  int n = 0;
  while (c.latched < 0 && n < 400) {
    nr_ptrs_class_crc_feed(&c, 0, (g() % 100) < 70);
    nr_ptrs_class_crc_feed(&c, nr_ptrs_class_pick_mode(&c), false);
    n++;
  }
  EXPECT_EQ(c.latched, 0);
}

TEST(PtrsClass, DetectorSilenceLatchesAbsentAndIsReProbed) {
  nr_ptrs_class_t c; nr_ptrs_class_init(&c);
  nr_ptrs_det_t none{};
  for (int i = 0; i < 64; i++) { EXPECT_TRUE(nr_ptrs_class_should_detect(&c)); nr_ptrs_class_det_feed(&c, &none); }
  EXPECT_EQ(c.latched, 0);
  int probes = 0;
  for (int i = 0; i < 160; i++) {
    if (nr_ptrs_class_should_detect(&c)) { probes++; nr_ptrs_class_det_feed(&c, &none); }
    else c.det_since_hit++;
  }
  EXPECT_GE(probes, 8);
  EXPECT_LE(probes, 12);
  nr_ptrs_det_t hit{}; hit.fired = true; hit.hyp = {2, 1, 0};
  nr_ptrs_class_det_feed(&c, &hit);
  EXPECT_EQ(c.latched, -1) << "a hit on the back-off probe reopens the class";
}

TEST(PtrsClass, LatchedPresentStopsRunningTheDetector) {
  nr_ptrs_class_t c; nr_ptrs_class_init(&c);
  c.latched = 1;
  EXPECT_FALSE(nr_ptrs_class_should_detect(&c));
}

TEST(PtrsClass, TheModeWhoseCrcPassesWins) {
  // A cell whose PT-RS sequence differs from the CPE reference: only 'exclusion only' (mode 2) decodes.
  nr_ptrs_class_t c; nr_ptrs_class_init(&c);
  std::mt19937 g(8);
  int n2 = 0, n1 = 0;
  for (int i = 0; i < 300 && c.latched < 0; i++) {
    nr_ptrs_class_crc_feed(&c, 0, false);
    const int m = nr_ptrs_class_pick_mode(&c);
    (m == 1 ? n1 : n2)++;
    nr_ptrs_class_crc_feed(&c, m, m == 2 && (g() % 100) < 80);
  }
  EXPECT_EQ(c.latched, 1);
  EXPECT_GT(c.ok[2], 0u);
  EXPECT_EQ(c.ok[1], 0u);
  EXPECT_EQ(nr_ptrs_class_pick_mode(&c), 2) << "the winning mode keeps being picked";
  std::cerr << "[ MEASURED ] mode 1 tried " << n1 << "x, mode 2 tried " << n2 << "x before latching\n";
}
