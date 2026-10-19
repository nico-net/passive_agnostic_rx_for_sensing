/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_initial_sync_budget.h"

int nr_initial_sync_scan_batch(size_t bytes_per_gscn, size_t len_thr, long scratch_mb)
{
  if (scratch_mb < NR_INITIAL_SYNC_SCRATCH_MB_MIN)
    scratch_mb = NR_INITIAL_SYNC_SCRATCH_MB_MIN;
  if (scratch_mb > NR_INITIAL_SYNC_SCRATCH_MB_MAX)
    scratch_mb = NR_INITIAL_SYNC_SCRATCH_MB_MAX;
  size_t max_by_mem = ((size_t)scratch_mb * 1024UL * 1024UL) / (bytes_per_gscn ? bytes_per_gscn : 1);
  if (max_by_mem < 1)
    max_by_mem = 1; // one GSCN at a time is the floor; below that the scan cannot run at all
  size_t max_by_thread = len_thr < 1 ? 1 : len_thr; // a pool with no workers runs tasks inline
  return (int)(max_by_mem < max_by_thread ? max_by_mem : max_by_thread);
}
