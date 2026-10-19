/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_INITIAL_SYNC_BUDGET_H
#define NR_INITIAL_SYNC_BUDGET_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

#define NR_INITIAL_SYNC_SCRATCH_MB_DEFAULT 512L
#define NR_INITIAL_SYNC_SCRATCH_MB_MIN 64L
#define NR_INITIAL_SYNC_SCRATCH_MB_MAX 16384L

/* Number of GSCNs scanned concurrently: min(budget / bytes_per_gscn, len_thr), each floored at 1.
 * scratch_mb is clamped to [MIN, MAX]. The caller still caps the result at numGscn. */
int nr_initial_sync_scan_batch(size_t bytes_per_gscn, size_t len_thr, long scratch_mb);

#ifdef __cplusplus
}
#endif
#endif
