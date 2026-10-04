/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_td_gate.h"
/* Conservative AWGN 1-layer requirements (dB, ~10 % BLER) per MCS index. Coarse and deliberately
 * pessimistic-for-gating: the margin and the 20-sample floor make gating rare; it only removes grants
 * that are far beyond the link. Table 0 = 64QAM, 1 = 256QAM, 2 = 64QAM-LowSE (TS 38.214 Tables 5.1.3.1-1/2/3). */
static const float k_req[3][32] = {
  {-6.5f, -5.5f, -4.5f, -3.5f, -2.5f, -1.5f, -0.5f, 0.5f, 1.5f, 2.5f, 3.5f, 4.5f, 5.5f, 6.5f, 7.5f, 8.5f, 9.5f,
   10.5f, 11.5f, 12.5f, 13.5f, 14.5f, 15.5f, 16.5f, 17.5f, 18.5f, 19.5f, 20.5f, 20.5f, 20.5f, 20.5f, 20.5f},
  {-6.5f, -4.5f, -2.5f, -0.5f, 1.5f, 3.0f, 4.5f, 6.0f, 7.5f, 9.0f, 10.5f, 12.0f, 13.0f, 14.0f, 15.0f, 16.0f, 17.0f,
   18.0f, 19.0f, 20.0f, 21.0f, 22.0f, 23.0f, 24.0f, 25.0f, 26.0f, 27.0f, 28.0f, 28.0f, 28.0f, 28.0f, 28.0f},
  {-9.0f, -8.5f, -8.0f, -7.5f, -7.0f, -6.5f, -6.0f, -5.5f, -5.0f, -4.5f, -4.0f, -3.5f, -3.0f, -2.5f, -2.0f, -1.5f,
   -0.5f, 0.5f, 1.5f, 2.5f, 3.5f, 4.5f, 5.5f, 6.5f, 7.5f, 8.5f, 9.5f, 10.5f, 10.5f, 10.5f, 10.5f, 10.5f},
};
float nr_td_required_snr_db(int mcs, int table)
{
  if (mcs < 0)
    mcs = 0;
  if (mcs > 31)
    mcs = 31;
  if (table < 0 || table > 2)
    table = 2;
  return k_req[table][mcs];
}
nr_td_gate_t nr_td_grant_gate(const nr_td_grant_view_t *g, const nr_td_rx_view_t *rx)
{
  if (g->layers > 0 && g->layers > rx->n_rx)
    return NR_TD_GATED_PHYSICAL;
  if (rx->snr_samples >= 20) {
    const int table = g->mcs_table_most_permissive < 0 ? 2 : g->mcs_table_most_permissive;
    if (rx->snr_est_db + rx->margin_db < nr_td_required_snr_db(g->mcs, table))
      return NR_TD_GATED_CHANNEL_QUALITY;
  }
  return NR_TD_ELIGIBLE;
}
