/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#ifndef NR_ISAC_SSB_AXIS_H
#define NR_ISAC_SSB_AXIS_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void nr_isac_ssb_k_abs(int ssb_start_subcarrier, int k_ssb, int ofdm_symbol_size,
                       uint32_t *k_abs_out);
#ifdef __cplusplus
}
#endif
#endif
