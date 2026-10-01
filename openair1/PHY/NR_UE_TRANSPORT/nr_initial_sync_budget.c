/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_initial_sync_budget.h"
#include <errno.h>
#include <stdlib.h>

nr_scan_scratch_parse_t nr_initial_sync_parse_scratch_mb(const char *str, long *mb)
{
  *mb = NR_INITIAL_SYNC_SCRATCH_MB_DEFAULT;
  if (!str || !*str)
    return NR_SCAN_SCRATCH_UNSET;
  char *end = NULL;
  errno = 0;
  long v = strtol(str, &end, 10);
  if (end == str || *end != '\0')
    return NR_SCAN_SCRATCH_INVALID;
  if (errno == ERANGE)
    v = v < 0 ? NR_INITIAL_SYNC_SCRATCH_MB_MIN - 1 : NR_INITIAL_SYNC_SCRATCH_MB_MAX + 1;
  if (v < NR_INITIAL_SYNC_SCRATCH_MB_MIN) {
    *mb = NR_INITIAL_SYNC_SCRATCH_MB_MIN;
    return NR_SCAN_SCRATCH_CLAMPED;
  }
  if (v > NR_INITIAL_SYNC_SCRATCH_MB_MAX) {
    *mb = NR_INITIAL_SYNC_SCRATCH_MB_MAX;
    return NR_SCAN_SCRATCH_CLAMPED;
  }
  *mb = v;
  return NR_SCAN_SCRATCH_OK;
}

long nr_initial_sync_scratch_mb(nr_scan_scratch_parse_t *status)
{
  /* Parsed once; nr_initial_sync() runs on a single thread, but a benign race would only re-parse the same env. */
  static long cached_mb = 0;
  static nr_scan_scratch_parse_t cached_status;
  if (cached_mb == 0)
    cached_status = nr_initial_sync_parse_scratch_mb(getenv("ISAC_SCAN_SCRATCH_MB"), &cached_mb);
  if (status)
    *status = cached_status;
  return cached_mb;
}

int nr_initial_sync_scan_batch(size_t bytes_per_gscn, int len_thr, long scratch_mb)
{
  if (scratch_mb < NR_INITIAL_SYNC_SCRATCH_MB_MIN)
    scratch_mb = NR_INITIAL_SYNC_SCRATCH_MB_MIN;
  if (scratch_mb > NR_INITIAL_SYNC_SCRATCH_MB_MAX)
    scratch_mb = NR_INITIAL_SYNC_SCRATCH_MB_MAX;
  size_t max_by_mem = ((size_t)scratch_mb * 1024UL * 1024UL) / (bytes_per_gscn ? bytes_per_gscn : 1);
  if (max_by_mem < 1)
    max_by_mem = 1; // one GSCN at a time is the floor; below that the scan cannot run at all
  size_t max_by_thread = len_thr < 1 ? 1 : (size_t)len_thr; // a pool with no workers runs tasks inline
  return (int)(max_by_mem < max_by_thread ? max_by_mem : max_by_thread);
}
