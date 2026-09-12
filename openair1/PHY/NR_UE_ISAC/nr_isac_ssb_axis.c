/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#include "nr_isac_ssb_axis.h"

/* Element i of the caller's PBCH channel estimate is the estimate AT subcarrier
 * ssb_start_subcarrier + i, not at the start of the CRB containing it. Traced, not assumed, in two
 * independent places that walk the array and the grid together:
 *   - nr_dl_channel_estimation.c:644-773 (nr_pbch_channel_estimation): re_offset starts at
 *     first_carrier_offset + ssb_start_subcarrier and dl_ch starts at index 0; both then advance
 *     by 12 per RB in lockstep (and both skip 144 together for the SSS in symbol 1).
 *   - nr_pbch.c:45-62 (nr_pbch_extract): rx_offset = first_carrier_offset + ssb_start_subcarrier
 *     is walked against dl_ch_estimates index 0.
 * So the coordinate is ssb_start_subcarrier + i, with no k_SSB term and no CRB flooring.
 *
 * P11fix, item 2: this is WHY the former k_ssb parameter is gone rather than plumbed. It existed
 * only to form base_sc = ((ssb_start_subcarrier - k_ssb)/12)*12, i.e. to floor the axis to the
 * containing CRB -- which is the wrong target for this array (it labels element 0 as the CRB
 * boundary when element 0 is k_SSB subcarriers above it). See docs/cfr_support_and_reference_
 * contract.md for the full derivation, including the proof that recovering k_ssb as
 * ssb_start_subcarrier % 12 was an algebraic no-op against the hardcoded 0 it would have replaced.
 */
void nr_isac_ssb_k_abs(int ssb_start_subcarrier, int carrier_bandwidth_sc, uint32_t *k_abs_out)
{
  if (!k_abs_out || carrier_bandwidth_sc <= 0)
    return;
  for (int i = 0; i < NR_ISAC_SSB_NOF_RE; ++i) {
    int value = (ssb_start_subcarrier + i) % carrier_bandwidth_sc;
    if (value < 0)
      value += carrier_bandwidth_sc;
    k_abs_out[i] = (uint32_t)value;
  }
}
