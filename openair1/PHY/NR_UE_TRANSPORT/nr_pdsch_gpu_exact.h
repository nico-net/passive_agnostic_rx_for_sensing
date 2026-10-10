/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* GPU6: ISAC_PDSCH_GPU_EXACT_FEP=1 (default off) -- the passive PDSCH decoder's FEP runs on the GPU and
 * returns the CPU's rxdataF bit for bit (nr_pdsch_gpu_exact_fep.cuh); channel estimation, equalisation,
 * LLR and decoding stay on the unchanged CPU chain, so CRC/EVM are those of the CPU receiver by
 * construction. The GPU LLR path (ISAC_PDSCH_GPU / NR_GPU_FEP) is not loaded while this is active.
 * ISAC_PDSCH_GPU_EXACT_CHECK: 1 (default) recomputes one (antenna, symbol) per call on the CPU and
 * disables the GPU path on any difference; 2 checks every symbol; 0 off. */
#ifndef NR_PDSCH_GPU_EXACT_H
#define NR_PDSCH_GPU_EXACT_H
#include "PHY/defs_nr_UE.h"
#include <stdint.h>

/* Warm-up (before the RX allocation fence): returns 1 when the exact path is active. */
int nr_pdsch_gpu_exact_warmup(PHY_VARS_NR_UE *ue);
/* FEP of every symbol in the ranges (rng[g] = {first symbol, count}) into rxdataF[ant * stride + sym * N].
 * 0 = done (bit-exact with nr_slot_fep_ant), -1 = caller runs the CPU FEP. */
int nr_pdsch_gpu_exact_fep_ranges(PHY_VARS_NR_UE *ue, const NR_DL_FRAME_PARMS *fp, int slot, const int (*rng)[2], int n_rng,
                                  c16_t *rxdataF, size_t stride, c16_t **rxdata);
/* ISAC_PDCCH_GPU_EXACT_FEP=1 (default off, needs the PDSCH exact path active): one CORESET symbol, all
 * antennas, into rxdataF[ant][symbol * N]; never waits for the GPU (busy -> -1, CPU FEP). */
int nr_pdcch_gpu_exact_fep_symbol(PHY_VARS_NR_UE *ue, const NR_DL_FRAME_PARMS *fp, int slot, int symbol, c16_t *rxdataF,
                                  c16_t **rxdata);
/* ISAC_CHEST_GPU_DELAY=1: bit-exact nr_passive_est_delay() of up to 16 N=4096 LS rows; out[3 r] =
 * {est_delay, delay_max_pos, delay_max_val} of the candidate. 0, or -1 (CPU). */
int nr_pdsch_gpu_exact_delay_rows(const c16_t *const *rows, int n, int *out);
void nr_pdsch_gpu_exact_delay_note(int gpu_rows, int checked, int bad);
#endif
