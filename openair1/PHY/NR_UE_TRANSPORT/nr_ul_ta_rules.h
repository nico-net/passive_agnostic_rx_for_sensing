/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_UL_TA_RULES_H
#define NR_UL_TA_RULES_H
#include <stdint.h>
/* Where a PASSIVE receiver sees a UE's uplink, relative to the downlink grid it follows (TS 38.213 4.2, N_TA = 2 d(gNB,UE)/c):
 *   UL arrival is EARLIER than the DL slot boundary by  N_TA_offset + N_TA/2 + (d_gNB->rx - d_UE->rx).
 * The receiver sitting at the gNB gives N_TA_offset, sitting at the UE N_TA_offset + N_TA, and by the triangle inequality the unknown
 * term is within +-N_TA/2. The advance seed is the midpoint, which halves the worst-case error of either assumption; the blind timing
 * search around the seed does the rest. nominal = N_TA_offset in samples, ta_cmd = N_TA of the UE in samples (< 0: unknown). */
static inline int32_t nr_ul_ta_seed(int32_t nominal, int32_t ta_cmd) { return ta_cmd > 0 ? nominal + ta_cmd / 2 : nominal; }
#endif
