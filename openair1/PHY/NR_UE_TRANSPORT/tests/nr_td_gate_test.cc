/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include <gtest/gtest.h>
extern "C" {
#include "nr_td_gate.h"
}
static nr_td_rx_view_t rx(int n_rx, float snr, int samples)
{
  return {n_rx, snr, samples, 6.0f};
}

TEST(TdGate, UnknownLayersNeverGatesOnRank)
{
  nr_td_grant_view_t g = {-1, 5, 0};
  const nr_td_rx_view_t r = rx(1, 30, 50);
  EXPECT_EQ(nr_td_grant_gate(&g, &r), NR_TD_ELIGIBLE);
}
TEST(TdGate, RankAboveRxIsPhysical)
{
  nr_td_grant_view_t g = {2, 5, 0};
  const nr_td_rx_view_t r = rx(1, 30, 50);
  EXPECT_EQ(nr_td_grant_gate(&g, &r), NR_TD_GATED_PHYSICAL);
  const nr_td_rx_view_t r4 = rx(4, 30, 50);
  EXPECT_EQ(nr_td_grant_gate(&g, &r4), NR_TD_ELIGIBLE);
}
TEST(TdGate, ChannelQualityNeedsEnoughSamplesAndMargin)
{
  nr_td_grant_view_t g = {1, 27, 1}; /* high MCS, 256QAM table */
  const float need = nr_td_required_snr_db(27, 1);
  const nr_td_rx_view_t few = rx(1, need - 20, 5);
  EXPECT_EQ(nr_td_grant_gate(&g, &few), NR_TD_ELIGIBLE); /* < 20 samples: never gate on SNR */
  const nr_td_rx_view_t low = rx(1, need - 6.5f, 50);
  EXPECT_EQ(nr_td_grant_gate(&g, &low), NR_TD_GATED_CHANNEL_QUALITY); /* beyond the 6 dB margin */
  const nr_td_rx_view_t edge = rx(1, need - 5.5f, 50);
  EXPECT_EQ(nr_td_grant_gate(&g, &edge), NR_TD_ELIGIBLE); /* within margin: still a trial */
}
TEST(TdGate, UnknownTableUsesMostPermissiveAssumption)
{
  nr_td_grant_view_t g = {1, 20, -1};
  const nr_td_rx_view_t r = rx(1, nr_td_required_snr_db(20, 2) - 5.0f, 50);
  EXPECT_EQ(nr_td_grant_gate(&g, &r), NR_TD_ELIGIBLE); /* table 2 (LowSE) is the lowest rate: no gating */
}
TEST(TdGate, RequiredSnrIsMonotoneInMcs)
{
  for (int t = 0; t < 3; t++)
    for (int m = 1; m < 28; m++)
      EXPECT_GE(nr_td_required_snr_db(m, t), nr_td_required_snr_db(m - 1, t)) << "table " << t << " mcs " << m;
}
