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

typedef enum {
  NR_SCAN_SCRATCH_UNSET = 0, /* env not set: default */
  NR_SCAN_SCRATCH_OK,        /* parsed, in range */
  NR_SCAN_SCRATCH_CLAMPED,   /* numeric but out of range: clamped */
  NR_SCAN_SCRATCH_INVALID    /* not a number: default */
} nr_scan_scratch_parse_t;

/* Parse the ISAC_SCAN_SCRATCH_MB value (NULL = unset). *mb is always set to a value in [MIN, MAX]. */
nr_scan_scratch_parse_t nr_initial_sync_parse_scratch_mb(const char *str, long *mb);

/* Scratch budget in MB, parsed once from the environment (cached). *status receives the parse result. */
long nr_initial_sync_scratch_mb(nr_scan_scratch_parse_t *status);

/* Number of GSCNs scanned concurrently: min(budget / bytes_per_gscn, len_thr), each floored at 1.
 * scratch_mb is clamped to [MIN, MAX]. The caller still caps the result at numGscn. */
int nr_initial_sync_scan_batch(size_t bytes_per_gscn, int len_thr, long scratch_mb);

#ifdef __cplusplus
}
#endif
#endif
