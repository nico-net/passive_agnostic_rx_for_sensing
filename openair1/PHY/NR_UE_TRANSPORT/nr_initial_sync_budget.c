/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_initial_sync_budget.h"
#include <errno.h>
#include <stdatomic.h>
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
  /* Parsed once into locals, then published (value last, release/acquire): a concurrent caller either sees "not yet
   * published" and parses the same env itself, or sees the final (status, mb) pair; never a partial value. */
  static _Atomic long cached_mb = 0;
  static _Atomic int cached_status = 0;
  long mb = atomic_load_explicit(&cached_mb, memory_order_acquire);
  if (mb == 0) {
    nr_scan_scratch_parse_t st = nr_initial_sync_parse_scratch_mb(getenv("ISAC_SCAN_SCRATCH_MB"), &mb);
    atomic_store_explicit(&cached_status, (int)st, memory_order_relaxed);
    atomic_store_explicit(&cached_mb, mb, memory_order_release);
    if (status)
      *status = st;
    return mb;
  }
  if (status)
    *status = (nr_scan_scratch_parse_t)atomic_load_explicit(&cached_status, memory_order_relaxed);
  return mb;
}

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
