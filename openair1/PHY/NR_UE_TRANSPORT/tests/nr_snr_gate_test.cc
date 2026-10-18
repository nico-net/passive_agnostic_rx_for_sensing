#include <gtest/gtest.h>
#include <cmath>
extern "C" {
#include "nr_snr_gate.h"
}
/* required_snr_db of tests/passive_rx/goal_metrics.py, evaluated for every MCS of T64 / T256 / TLOW (generated from the script) */
static const double k_py0[] = {-6.035088, -4.755737, -3.749154, -2.428957, -1.362080, -0.236354, 0.724621, 1.663095, 2.500781, 3.282184, 3.291959, 4.011145, 4.999513, 5.922741, 6.902835, 7.835640, 8.436725, 8.422581, 9.010259, 10.051627, 11.043554, 11.994035, 12.946614, 13.941318, 14.923835, 15.841891, 16.771248, 17.441837, 18.127882};
static const double k_py1[] = {-6.035088, -3.749154, -1.362080, 0.724621, 2.500781, 4.011145, 4.999513, 5.922741, 6.902835, 7.835640, 8.436725, 9.010259, 10.051627, 11.043554, 11.994035, 12.946614, 13.941318, 14.923835, 15.841891, 16.771248, 17.441837, 18.127882, 19.158721, 20.185434, 21.232669, 22.277240, 23.023747, 23.769356};
static const double k_py2[] = {-12.324742, -11.045725, -10.046929, -8.933143, -8.032181, -6.933808, -6.035088, -4.755737, -3.749154, -2.428957, -1.362080, -0.236354, 0.724621, 1.663095, 2.500781, 3.291959, 4.011145, 4.999513, 5.922741, 6.902835, 7.835640, 8.422581, 9.010259, 10.051627, 11.043554, 11.994035, 12.946614, 13.941318, 14.923835};

static void check(int tbl, const double *py, int n)
{
  for (int m = 0; m < n; m++) EXPECT_NEAR(py[m], nr_required_snr_db(m, tbl), 0.01) << "tbl " << tbl << " mcs " << m;
  EXPECT_TRUE(std::isnan(nr_required_snr_db(n, tbl))) << "reserved MCS has no requirement";
}
TEST(SnrGate, RequiredMatchesPythonT64) { check(0, k_py0, sizeof k_py0 / sizeof *k_py0); }
TEST(SnrGate, RequiredMatchesPythonT256) { check(1, k_py1, sizeof k_py1 / sizeof *k_py1); }
TEST(SnrGate, RequiredMatchesPythonTLow) { check(2, k_py2, sizeof k_py2 / sizeof *k_py2); }
TEST(SnrGate, UnknownTableReadsT64) { EXPECT_DOUBLE_EQ(nr_required_snr_db(10, 0), nr_required_snr_db(10, 7)); }

TEST(SnrGate, ThresholdIsRequiredMinusFive)
{
  const double req = nr_required_snr_db(10, 0);
  EXPECT_TRUE(nr_snr_no_chance(req - 5.01, 10, 0));
  EXPECT_FALSE(nr_snr_no_chance(req - 4.99, 10, 0));
  EXPECT_FALSE(nr_snr_no_chance(req + 3.0, 10, 0));
}
TEST(SnrGate, NoEstimateOrReservedMcsIsNeverGated)
{
  EXPECT_FALSE(nr_snr_no_chance(-99.0, 10, 0)); /* chest gave no estimate */
  EXPECT_FALSE(nr_snr_no_chance(NAN, 10, 0));
  EXPECT_FALSE(nr_snr_no_chance(-50.0, 31, 0)); /* reserved MCS: no requirement */
  EXPECT_TRUE(nr_snr_no_chance(-50.0, 10, 0));
}
TEST(SnrGate, ProbesAndNonDedicatedAreExempt)
{
  EXPECT_TRUE(nr_snr_gate_applies(false, true, 0, false));
  EXPECT_FALSE(nr_snr_gate_applies(true, true, 0, false));  /* layout probe: discovery */
  EXPECT_FALSE(nr_snr_gate_applies(false, false, 0, false)); /* SI / P / RA */
}
TEST(SnrGate, RetransmissionsFollowHarqCombining)
{
  EXPECT_TRUE(nr_snr_gate_applies(false, true, 2, false));  /* combining off: same rule as rv 0 */
  EXPECT_FALSE(nr_snr_gate_applies(false, true, 2, true));  /* combining on: always decoded */
  EXPECT_TRUE(nr_snr_gate_applies(false, true, 0, true));   /* first transmission is gated either way */
}
