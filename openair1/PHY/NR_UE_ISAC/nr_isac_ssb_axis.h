/* SSB CFR coordinates: fp->ssb_start_subcarrier + RE offset, relative to
 * CRB0 / Point A. k_ssb is metadata and does not shift the estimator buffer.
 * The retained k_ssb and FFT-size arguments are ignored by the axis helper. */
#ifndef NR_ISAC_SSB_AXIS_H
#define NR_ISAC_SSB_AXIS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Fill k_abs_out[0..239] with the CRB0-relative absolute subcarrier index of each RE in
 *        SSB's own 240-subcarrier (20 RB) channel estimate.
 *
 * @param ssb_start_subcarrier fp->ssb_start_subcarrier (frame_parms), the FFT-relative subcarrier
 *                              the SSB's own RE 0 sits at.
 * @param k_ssb                Sub-RB SSB shift, ALREADY NORMALIZED (see openair2/LAYER2/NR_MAC_UE/
 *                              nr_ue_dci_configuration.c's ssb_sc_offset_norm derivation: mac->
 *                              ssb_subcarrier_offset is raw 15kHz-unit kSSB and must be right-shifted
 *                              by scs for FR1 before being passed here — NOT read directly from
 *                              frame_parms, which has no field of this name). 0 for this deployment.
 * @param ofdm_symbol_size     fp->ofdm_symbol_size, the FFT size k_abs must be taken modulo.
 * @param k_abs_out            Caller-owned array of at least 240 uint32_t.
 */
void nr_isac_ssb_k_abs(int ssb_start_subcarrier, int k_ssb, int ofdm_symbol_size, uint32_t* k_abs_out);
int nr_isac_ssb_normalize_k_ssb(int raw_k_ssb, int numerology, int fr1);

#ifdef __cplusplus
}
#endif

#endif
