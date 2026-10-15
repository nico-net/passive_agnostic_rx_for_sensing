/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* GPUPASS Task 7 test reference (nr_gpu_pass_ul_ref.c): the CPU chest and equaliser of one layer of the passive PUSCH receiver. */
#ifndef NR_GPU_PASS_UL_REF_H
#define NR_GPU_PASS_UL_REF_H
#include <stdint.h>
#include "PHY/TOOLS/tools_defs.h"
#ifdef __cplusplus
extern "C" {
#endif
/* one chest row: rx = the symbol row of rxdataF (N c16), pil = 6 * nb_rb pilots, delay_table = c16 [2 * max_delay_comp + 1][dt_stride]; ul_ch = N c16 (delta realigned); out_delay = {est_delay,
 * delay_max_pos, delay_max_val} of nr_passive_est_delay; noise = nr_pusch_antenna_processing's noise_amp2 of the row */
void nr_gpu_pass_ul_ref_chest(const c16_t *rx, const c16_t *pil, int k0, int nb_rb, int delta, const c16_t *delay_table, int dt_stride, int max_delay_comp, c16_t *ul_ch, int out_delay[3],
                              uint64_t *noise);
/* the equaliser of one grant: returns the number of LLRs (not descrambled) or -1 */
int nr_gpu_pass_ul_ref_eq(int nbRx, int nb_rb, int qm, int keep, int delta, int start_symbol, int nsym, uint32_t dmrs_mask, int cdm, int start_re, const c16_t *rxF, const c16_t *est, int16_t *llr,
                          int *log2_maxh_out, int *avg_out);
#ifdef __cplusplus
}
#endif
#endif
