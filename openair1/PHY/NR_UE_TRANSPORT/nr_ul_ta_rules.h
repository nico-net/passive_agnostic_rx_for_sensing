/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_UL_TA_RULES_H
#define NR_UL_TA_RULES_H
#include <stdint.h>
#include <stdbool.h>
/* Where a PASSIVE receiver sees a UE's uplink, relative to the downlink grid it follows (TS 38.213 4.2, N_TA = 2 d(gNB,UE)/c):
 *   UL arrival is EARLIER than the DL slot boundary by  N_TA_offset + N_TA/2 + (d_gNB->rx - d_UE->rx).
 * The receiver sitting at the gNB gives N_TA_offset, sitting at the UE N_TA_offset + N_TA, and by the triangle inequality the unknown
 * term is within +-N_TA/2. The advance seed is the midpoint, which halves the worst-case error of either assumption; the blind timing
 * search around the seed does the rest. nominal = N_TA_offset in samples, ta_cmd = N_TA of the UE in samples (< 0: unknown). */
static inline int32_t nr_ul_ta_seed(int32_t nominal, int32_t ta_cmd) { return ta_cmd > 0 ? nominal + ta_cmd / 2 : nominal; }
/* The nominal advance N_TA_offset (TS 38.213 4.2, TS 38.133 Table 7.1.2-2) from the SIB1 n-TimingAdvanceOffset as the UE MAC stored it
 * (cell_config.N_TA_offset in Tc units, get_ta_offset in config_ue.c: n0 -> 0, n25600, n39936; field absent -> 0xFFFFFFFF). Field
 * absent, or no SIB1 configuration applied yet (cfg_applied false: a zeroed config would read as n0), or any other value: the FR1 TDD
 * default dflt_tc (25600). Returns Tc units. */
static inline int32_t nr_ul_nta_offset_tc(uint32_t raw, bool cfg_applied, int32_t dflt_tc)
{
  if (!cfg_applied || raw == 0xFFFFFFFFu) return dflt_tc;
  return (raw == 0u || raw == 25600u || raw == 39936u) ? (int32_t)raw : dflt_tc;
}
/* Tc units to samples at the receiver's rate (T_c = 1 / (480e3 * 4096)). */
static inline int32_t nr_ul_nta_tc_to_samples(int32_t tc, int32_t samples_per_subframe)
{
  return (int32_t)(((int64_t)tc * samples_per_subframe) / (4096ll * 480ll));
}
#endif
