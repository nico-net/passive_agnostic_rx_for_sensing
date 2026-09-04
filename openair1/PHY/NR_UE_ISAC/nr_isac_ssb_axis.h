/*! \file openair1/PHY/NR_UE_ISAC/nr_isac_ssb_axis.h
 * \brief CRB0-relative absolute-subcarrier axis for SSB's 240-subcarrier channel estimate.
 *
 * Every fusion source submitted through nr_isac_submit_cfr*() must place its k_abs values on the
 * SAME absolute subcarrier axis (relative to CRB0 / point A) -- see nr_isac.h's own doc comment
 * on nr_isac_submit_cfr(). This is a pure function, deliberately split out of the RT tap
 * (phy_procedures_nr_ue.c) so the one genuinely error-prone piece of Phase 2 (the axis
 * derivation) has its own unit test, independent of live hardware.
 *
 * The derivation mirrors nr_ue_dci_configuration.c's ssb_offset_point_a computation exactly
 * (Phase 1, PHASE1_CSS0_AUTOCONF_HANDOVER.md): ssb_offset_point_a = (ssb_start_subcarrier -
 * k_ssb) / 12 gives the RB-aligned CRB0-relative offset of the SSB's lowest RB; index 0 of the
 * 240-wide estimate sits at that RB's first subcarrier.
 */
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

#ifdef __cplusplus
}
#endif

#endif
