/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_initial_sync_budget.h"
#include <stdlib.h>

int nr_auto_acquire_on;

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

int nr_acq_cfo_ladder(int centre_hz, int scs_hz, int limit_hz, int *out, int max)
{
  int n = 0;
  if (scs_hz <= 0 || max <= 0)
    return 0;
#define ADD(d) do { if (n < max && abs(d) <= limit_hz) out[n++] = centre_hz + (d); } while (0)
  ADD(0);
  ADD(NR_ACQ_ALIAS_HZ);
  ADD(-NR_ACQ_ALIAS_HZ);
  for (int k = 1; k * scs_hz <= limit_hz + NR_ACQ_ALIAS_HZ; k++) {
    ADD(k * scs_hz);
    ADD(-k * scs_hz);
    ADD(k * scs_hz + NR_ACQ_ALIAS_HZ);
    ADD(k * scs_hz - NR_ACQ_ALIAS_HZ);
    ADD(-k * scs_hz + NR_ACQ_ALIAS_HZ);
    ADD(-k * scs_hz - NR_ACQ_ALIAS_HZ);
  }
#undef ADD
  return n;
}
