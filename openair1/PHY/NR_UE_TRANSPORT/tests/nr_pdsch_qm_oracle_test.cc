#include <cmath>
#include <random>
#include <vector>
#include <gtest/gtest.h>
extern "C" {
#include "nr_pdsch_qm_oracle.h"
}

namespace {
// Square QAM of order qm, unit average power, AWGN at snr_db, scaled into int16 by an arbitrary gain
// (the receiver's fixed-point scale is unknown; the oracle must not depend on it).
std::vector<int16_t> Qam(int qm, double snr_db, int n, unsigned seed)
{
  std::mt19937 g(seed);
  const int lmax = (1 << (qm / 2)) - 1;
  double ms = 0.0; int nl = 0;
  for (int l = 1; l <= lmax; l += 2) { ms += l * l; nl++; }
  const double norm = std::sqrt(2.0 * ms / nl), sigma = std::sqrt(0.5 / std::pow(10.0, snr_db / 10.0));
  std::uniform_int_distribution<int> lev(0, (lmax + 1) / 2 - 1);  // positive odd levels 1..lmax
  std::uniform_int_distribution<int> sgn(0, 1);
  std::normal_distribution<double> nz(0.0, sigma);
  std::vector<int16_t> iq(2 * n);
  for (int i = 0; i < 2 * n; i++) {
    const double a = (2 * lev(g) + 1) * (sgn(g) ? 1.0 : -1.0) / norm;
    iq[i] = (int16_t)std::lround((a + nz(g)) * 2500.0);
  }
  return iq;
}
}  // namespace

TEST(QmOracle, McsTablesMatchTs38214) {
  // Table 5.1.3.1-1 (mcs_table 0, 64QAM), -2 (1, 256QAM), -3 (2, 64QAM LowSE); 28..31 reserved.
  const int t0[32] = {2,2,2,2,2,2,2,2,2,2,4,4,4,4,4,4,4,6,6,6,6,6,6,6,6,6,6,6,6,2,4,6};
  const int t1[32] = {2,2,2,2,2,4,4,4,4,4,4,6,6,6,6,6,6,6,6,6,8,8,8,8,8,8,8,8,2,4,6,8};
  const int t2[32] = {2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,4,4,4,4,4,4,6,6,6,6,6,6,6,6,2,4,6};
  for (int m = 0; m < 32; m++) {
    EXPECT_EQ(nr_pdsch_qm_of_mcs(m, 0), t0[m]) << m;
    EXPECT_EQ(nr_pdsch_qm_of_mcs(m, 1), t1[m]) << m;
    EXPECT_EQ(nr_pdsch_qm_of_mcs(m, 2), t2[m]) << m;
  }
  EXPECT_EQ(nr_pdsch_qm_of_mcs(32, 0), 0);
  EXPECT_EQ(nr_pdsch_qm_of_mcs(0, 3), 0);
}

TEST(QmOracle, ClassifiesEveryOrderAtHighSnr) {
  for (int qm : {2, 4, 6, 8}) {
    auto iq = Qam(qm, 32.0, 2048, 100 + qm);
    double t = -1;
    EXPECT_EQ(nr_pdsch_qm_classify(iq.data(), 2048, &t), qm) << "qm=" << qm << " t=" << t;
  }
}

TEST(QmOracle, PureNoiseAbstains) {
  std::mt19937 g(7);
  std::normal_distribution<double> nz(0.0, 1500.0);
  std::vector<int16_t> iq(4096);
  for (auto &v : iq) v = (int16_t)std::lround(nz(g));
  EXPECT_EQ(nr_pdsch_qm_classify(iq.data(), 2048, nullptr), 0);
}

TEST(QmOracle, LowSnr256QamAbstains) {
  auto iq = Qam(8, 15.0, 2048, 9);
  EXPECT_EQ(nr_pdsch_qm_classify(iq.data(), 2048, nullptr), 0);
}

TEST(QmOracle, TooFewSymbolsAbstains) {
  auto iq = Qam(6, 32.0, 100, 3);
  EXPECT_EQ(nr_pdsch_qm_classify(iq.data(), 100, nullptr), 0);
}

int main(int argc, char **argv)
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
