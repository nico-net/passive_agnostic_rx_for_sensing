/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#pragma once
#include "nr_pdcch_nid_gpu.h"
#ifdef __cplusplus
extern "C" {
#endif
/* 0: frozen --selftest, 1/2: --selftest2 ni/il, 3/4: --selftest4 ni/il,
 * 5: --selftest5, 6: --selftest3, 7: compact D1, 8/9: compact D2/D3.
 * 10/11: D2 sym0/1; 12/13: D3 L6 R2/3; 14: D3 NI; 15: mixed D1/D3 IDs;
 * 16/17: Gaussian noise / PDSCH-like load; 18: D3 L3; 19/20: slot-end D3/D2;
 * 21: D1 in a three-symbol capture (cost/regression control). */
int nid_reference_original_main(int argc, char **argv);
void nid_reference_make(int fixture, nr_pdcch_nid_batch_t *batch);
void nid_reference_batch_stats(const nr_pdcch_nid_batch_t *,uint16_t *,float *);
void nid_reference_run(int stage1, nr_pdcch_nid_result_t *out);
#ifdef __cplusplus
}
#endif
