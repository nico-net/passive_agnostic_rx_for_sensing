/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_CSIRS_DISC_FP_INTERNAL_H
#define NR_CSIRS_DISC_FP_INTERNAL_H
#include "nr_csirs_disc.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Fixed-point contrast makes CPU/CUDA reductions independent of summation order.
 * Host clustering is deliberately shared; no CUDA dependency in the CPU reference. */
typedef int (*nr_disc_fold_fn)(void *, int, uint64_t *, uint64_t *);
int nr_disc_fp_valid(const nr_csirs_disc_ring_t *r);
int nr_disc_fp_cluster(const nr_csirs_disc_ring_t *, const nr_csirs_disc_fp_cfg_t *,
                       nr_csirs_disc_fp_t *, int, nr_disc_fold_fn, void *);
void nr_disc_fp_normalize(const nr_csirs_disc_ring_t *, const nr_csirs_disc_fp_cfg_t *, uint32_t *);
#ifdef __cplusplus
}
#endif
#endif
