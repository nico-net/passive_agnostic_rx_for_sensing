/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#ifndef NR_ISAC_SSB_AXIS_H
#define NR_ISAC_SSB_AXIS_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Fills k_abs_out[0 .. NR_ISAC_SSB_NOF_RE-1] with the CRB/Point-A carrier-grid coordinate of each
 * of the SSB's 240 contiguous subcarriers, in the convention nr_isac.h:85-86 declares:
 * "[0, carrier->nof_prb*12) ... never an FFT-buffer index".
 *
 * ssb_start_subcarrier  fp->ssb_start_subcarrier -- ALREADY Point-A-referenced
 *                       (nr_get_ssb_start_sc() returns 12*prb_offset + k_SSB_post_shift; the FFT
 *                       address is formed separately as first_carrier_offset + this).
 * carrier_bandwidth_sc  nof_prb*12. P11-A1: this, NOT fp->ofdm_symbol_size -- the two differ
 *                       (3276 vs 4096 at 273 PRB) and wrapping modulo the FFT size can return a
 *                       value outside the declared range while looking normalised.
 */
#define NR_ISAC_SSB_NOF_RE 240
void nr_isac_ssb_k_abs(int ssb_start_subcarrier, int carrier_bandwidth_sc, uint32_t *k_abs_out);

#ifdef __cplusplus
}
#endif
#endif
