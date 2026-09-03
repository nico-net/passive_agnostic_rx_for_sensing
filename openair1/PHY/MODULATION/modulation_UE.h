/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#ifndef __MODULATION_DEFS__H__
#define __MODULATION_DEFS__H__
#include "PHY/defs_common.h"
#include "modulation_common.h"
#include "PHY/defs_UE.h"
#include "PHY/defs_nr_UE.h"
/** @addtogroup _PHY_MODULATION_
 * @{
*/



/*!
\brief This function implements the OFDM front end processor on reception (FEP)
\param phy_vars_ue Pointer to PHY variables
\param l symbol within slot (0..6/7)
\param Ns Slot number (0..19)
\param sample_offset offset within rxdata (points to beginning of subframe)
\param no_prefix if 1 prefix is removed by HW
\param reset_freq_est if non-zero it resets the frequency offset estimation loop
*/

int slot_fep(PHY_VARS_UE *phy_vars_ue,
             unsigned char l,
             unsigned char Ns,
             int sample_offset,
             int no_prefix,
	     int reset_freq_est);

int nr_slot_fep(PHY_VARS_NR_UE *ue,
                const NR_DL_FRAME_PARMS *frame_parms,
                unsigned int slot,
                unsigned int symbol,
                c16_t rxdataF[][frame_parms->samples_per_slot_wCP],
                enum nr_Link linktype,
                uint32_t sample_offset,
                c16_t **rxdata);

/* Per-branch residual frequency offset, in Hz, added to the common de-rotation inside
 * nr_slot_fep_ant(). Written by the DM-RS phase-slope estimator in nr_pdsch_passive_decode.c,
 * read by the FEP. All-zero (the default) reproduces the previous behaviour exactly.
 * See slot_fep_nr.c's definition-site comment for why per-branch de-rotation is needed at all. */
#define NR_MAX_BRANCH_FO 8
void nr_ue_set_branch_fo_hz(int ant, double hz);
double nr_ue_get_branch_fo_hz(int ant);

// Single-antenna variant of nr_slot_fep(), for passive-rx to dispatch across the thread pool --
// see its definition-site comment in slot_fep_nr.c for why this exists as a separate function.
int nr_slot_fep_ant(PHY_VARS_NR_UE *ue,
                    const NR_DL_FRAME_PARMS *frame_parms,
                    unsigned int slot,
                    unsigned int symbol,
                    unsigned int ant,
                    c16_t rxdataF[][frame_parms->samples_per_slot_wCP],
                    enum nr_Link linktype,
                    uint32_t sample_offset,
                    c16_t **rxdata);

// TEMPORARY DIAGNOSTIC (2026-08-05): see slot_fep_nr.c's definition-site comment. Valid only
// immediately after a synchronous nr_slot_fep() call on the same thread.
/// See slot_fep_nr.c. NAN = read the offset from `ue` (default for every thread).
extern __thread double nr_slot_fep_fo_override_hz;

extern __thread unsigned int nr_slot_fep_diag_rx_offset;
extern __thread unsigned int nr_slot_fep_diag_nb_prefix_samples;
extern __thread unsigned int nr_slot_fep_diag_nb_prefix_samples0;
extern __thread int nr_slot_fep_diag_is_synchronized;

int slot_fep_mbsfn(PHY_VARS_UE *phy_vars_ue,
                   unsigned char l,
                   int subframe,
                   int sample_offset,
                   int no_prefix);

int slot_fep_mbsfn_khz_1dot25(PHY_VARS_UE *phy_vars_ue,
                   int subframe,
                   int sample_offset);

int front_end_fft(PHY_VARS_UE *ue,
             unsigned char l,
             unsigned char Ns,
             int sample_offset,
             int no_prefix);

int front_end_chanEst(PHY_VARS_UE *ue,
             unsigned char l,
             unsigned char Ns,
            int reset_freq_est);

void apply_7_5_kHz(PHY_VARS_UE *phy_vars_ue,int32_t*txdata,uint8_t subframe);


int compute_BF_weights(int32_t **beam_weights, int32_t **calib_dl_ch_estimates, PRECODE_TYPE_t precode_type, int nb_ant, int nb_freq);



/** @}*/
#endif
