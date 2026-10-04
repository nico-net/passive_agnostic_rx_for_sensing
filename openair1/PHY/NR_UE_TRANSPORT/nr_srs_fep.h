/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_SRS_FEP_H
#define NR_SRS_FEP_H
#include "nr_srs_blind.h"
#include "PHY/nr_phy_common/inc/nr_phy_common.h"
/* Rotation inputs are captured with the IQ, never read from the live UE by the worker. */
static inline void nr_srs_fep_symbol(const nr_srs_grid_t *g, int slots_per_subframe,
    const c16_t *shift, const c16_t *rot, int slot, int symbol, double fo_hz,
    double samples_per_ms, int64_t sample, const c16_t *raw, c16_t *out)
{
  c16_t corrected[g->fft_size] __attribute__((aligned(32)));
  nr_fo_compensation(fo_hz,samples_per_ms,sample,raw,corrected,g->fft_size);
  dft(get_dft(g->fft_size),(int16_t *)corrected,(int16_t *)out,1);
  apply_nr_rotation_symbol_RX(14,slots_per_subframe,shift,g->first_carrier_offset,out,rot,g->n_rb,slot,symbol);
}
#endif
