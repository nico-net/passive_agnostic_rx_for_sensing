/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_TD_GATE_H
#define NR_TD_GATE_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { NR_TD_ELIGIBLE = 0, NR_TD_GATED_PHYSICAL = 1, NR_TD_GATED_CHANNEL_QUALITY = 2 } nr_td_gate_t;
/* Everything the gate may look at. All of it must be known BEFORE any decode of this grant (pre-outcome rule). */
typedef struct {
  int layers;            /* layers implied by the DCI antenna-ports field; -1 = layout not pinned yet (unknown) */
  int mcs;               /* DCI MCS index */
  int mcs_table_most_permissive; /* lowest-rate table still alive in this context (0/1/2), -1 unknown */
} nr_td_grant_view_t;
typedef struct {
  int n_rx;              /* receive antennas in use */
  float snr_est_db;      /* post-equaliser SNR estimate for this RNTI from EARLIER grants (or this grant's DM-RS) */
  int snr_samples;       /* how many estimates back snr_est_db */
  float margin_db;       /* ISAC_TD_GATE_SNR_MARGIN_DB, default 6 */
} nr_td_rx_view_t;
/* Required SNR (dB) to decode `mcs` under `table` at 10 % BLER, AWGN, 1 layer: a conservative monotone table. */
float nr_td_required_snr_db(int mcs, int table);
nr_td_gate_t nr_td_grant_gate(const nr_td_grant_view_t *g, const nr_td_rx_view_t *rx);
#ifdef __cplusplus
}
#endif
#endif
