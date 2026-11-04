/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Traffic-mode decode policy: per-cell, per-direction table deciding which grants get a full decode. */
#ifndef NR_TRAFFIC_DECODE_H
#define NR_TRAFFIC_DECODE_H
#include "nr_traffic_record.h"
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define NR_TRAFFIC_T_CHECK_NS 2000000000ull
#define NR_TRAFFIC_CHECK_EVERY_GRANTS 32
#define NR_TRAFFIC_K_REOPEN 4
#define NR_TRAFFIC_FAIL_MIN_SNR_DB 10.0f
/* dir 0 = DL, 1 = UL. Non-probe decodes only; solver probes (job.layout_probe) never ask. */
bool nr_traffic_decode_wanted(int dir, uint16_t rnti, int rnti_class, nr_layout_state_t ls, bool ul_table_learned,
                              uint64_t t_ns, bool hrv_alert);
/* E4 (traffic mode only; legacy never calls these). dir 1: PUSCH is decoded until the UL layout is locked (ls SOLVED; FIXED for 0_0) AND the UL MCS
 * table is known, then not at all; lock or table lost: again. The decodes still wanted back off exponentially after consecutive failures. */
/* Accepted DCIs of (dir, rnti) in this cell so far; count: this call is one more distinct accept (false: a duplicate of one already counted). */
int nr_traffic_note_dci(int dir, uint16_t rnti, bool count);
/* Probe triage: probe only when (n_dci_seen >= 2 or family_match) and n_alive >= 1 (alive splits of E1; >= 1 before any proof) and the SNR supports the MCS
 * (the solver's cell-measured gate margin; no estimate = supported), and then 1 grant in 2^k after k <= 4 consecutive failed probe grants of the RNTI.
 * Call once per GRANT (not per variant). cell: informational, the state is per cell. */
bool nr_traffic_probe_wanted(int cell, uint16_t rnti, int dir, int n_dci_seen, int n_alive, bool family_match, float snr_db, int mcs, int mcs_table);
/* Outcome of a probe/UL decode with a CRC verdict: a pass resets the backoff, a fail counts once per wanted grant. */
void nr_traffic_probe_result(int dir, uint16_t rnti, bool crc_ok);
/* Returns true when the RNTI must be reopened (K_REOPEN consecutive counted failures: rv 0 and snr >= 10 dB). */
bool nr_traffic_decode_result(int dir, uint16_t rnti, int rv, float snr_db, bool crc_ok);
void nr_traffic_decode_reset(void);   /* HARD_RESET epochs: forget every RNTI's counters */
#ifdef __cplusplus
}
#endif
#endif
