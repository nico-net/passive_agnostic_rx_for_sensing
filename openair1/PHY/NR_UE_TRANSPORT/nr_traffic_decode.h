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
/* As above with the grant's PRB count: in SENSING mode a wideband grant (>= sense_min_prb) is always decoded, narrower ones follow the unit state / traffic policy / sample. */
bool nr_traffic_decode_wanted_prbs(int dir, uint16_t rnti, int rnti_class, nr_layout_state_t ls, bool ul_table_learned,
                                   uint64_t t_ns, bool hrv_alert, uint16_t est_prbs);
/* Returns true when the RNTI must be reopened (K_REOPEN consecutive counted failures: rv 0 and snr >= 10 dB). */
bool nr_traffic_decode_result(int dir, uint16_t rnti, int rv, float snr_db, bool crc_ok);
void nr_traffic_decode_reset(void);   /* HARD_RESET epochs: forget every RNTI's counters */
#ifdef __cplusplus
}
#endif
#endif
