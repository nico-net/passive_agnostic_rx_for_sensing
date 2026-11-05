/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include <math.h>
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

int nr_cfo_alias_k(int est_hz, int prior_hz, int period_hz)
{
  if (period_hz <= 0)
    return 0;
  const double q = (double)(prior_hz - est_hz) / period_hz;
  return (int)(q < 0 ? q - 0.5 : q + 0.5);
}

int nr_resync_cfo_decide(const nr_resync_hyp_t *h0, const nr_resync_hyp_t *h1, int *neither)
{
  *neither = 0;
  if (h0->crc != h1->crc)
    return h1->crc ? 1 : 0;
  if (!h0->crc) {
    *neither = 1;
    return 0;
  }
  const double s0 = 2 * h0->m * h0->mu0 + h0->mu0 * h0->mu0, s1 = 2 * h1->m * h1->mu0 + h1->mu0 * h1->mu0;
  const double lead = 3.0 * sqrt(s0 + s1);
  if (h0->m > h1->m + lead)
    return 0;
  return 1; /* decisive for the alias, or a tie: the alias is the hypothesis nearer the prior */
}

void nr_cfo_verify_record_lock(nr_cfo_verify_t *v, int total_hz, int pci)
{
  v->last_valid = 1;
  v->last_total = total_hz;
  v->last_pci = pci;
}

int nr_cfo_verify_select_prior(nr_cfo_verify_t *v, unsigned long long lock_crc_ok, int cont_fo_comp, int radio_fo_hz)
{
  int dropped = 0;
  if (v->last_valid) {
    if (lock_crc_ok > 0) {
      v->prior_valid = 1;
      v->prior_total = v->last_total;
      v->prior_pci = v->last_pci;
      v->unverified_streak = 0;
    } else if (v->prior_valid && v->prior_pci != v->last_pci) {
      v->unverified_streak = 0; /* another cell's lock says nothing about this prior */
    } else if (v->prior_valid && ++v->unverified_streak >= NR_CFO_UNVERIFIED_MAX) {
      v->prior_valid = 0;
      v->unverified_streak = 0;
      dropped = 1;
    }
    v->last_valid = 0; /* each lock is judged once */
  }
  v->prior_est_hz = v->prior_total - (cont_fo_comp ? 0 : radio_fo_hz);
  return dropped;
}

void nr_cfo_verify_clear(nr_cfo_verify_t *v)
{
  v->last_valid = v->prior_valid = 0;
  v->unverified_streak = 0;
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
