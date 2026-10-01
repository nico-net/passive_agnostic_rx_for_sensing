/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_passive_metrics.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "common/utils/LOG/log.h"
#include "nr_passive_acq_state.h"
#include "nr_pdcch_passive_queue.h"
#include "nr_pdsch_passive_queue.h"
#include "nr_pdcch_blind_monitor_rt.h"
#include "nr_pdsch_passive_decode.h"
#include "nr_pusch_passive_decode.h"

extern _Atomic long nr_ue_diag_producer_absolute_slot; // executables/nr-ue.c
_Atomic int nr_passive_metrics_pci = -1;
/* Filled by Task A3; weak so this file links before A3 lands. */
__attribute__((weak)) void nr_passive_obs_stats(uint64_t *pushed, uint64_t *written, uint64_t *dropped)
{
  *pushed = *written = *dropped = 0;
}

void nr_passive_metrics_collect(nr_passive_metrics_t *m)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  *m = (nr_passive_metrics_t){0};
  m->t_mono_ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
  m->abs_slot = (int64_t)atomic_load_explicit(&nr_ue_diag_producer_absolute_slot, memory_order_relaxed);
  m->pci = atomic_load_explicit(&nr_passive_metrics_pci, memory_order_relaxed);
  const nr_passive_acq_snapshot_t a = nr_passive_acq_snapshot();
  m->acq_state = nr_passive_acq_state_name(a.state);
  m->acq_transitions = a.transitions;
  m->acq_sync_losses = a.sync_losses;
  m->acq_pbch_locks = a.pbch_locks;
  m->acq_sib1_decodes = a.sib1_decodes;
  nr_pdcch_blind_monitor_counters(&m->pdcch_occasions, &m->pdcch_candidates, &m->pdcch_accepts, &m->pdcch_accepts_c);
  nr_pdcch_passive_queue_stats_t sq;
  nr_pdcch_passive_queue_get_stats(&sq);
  m->scanq_queued = sq.queued;
  m->scanq_processed = sq.processed;
  m->scanq_drop_full = sq.dropped_full;
  m->scanq_drop_stale = sq.dropped_stale;
  m->scanq_max_lag = sq.max_lag_slots;
  nr_pdsch_passive_queue_stats_t pq;
  nr_pdsch_passive_queue_get_stats(&pq);
  m->pdschq_queued = pq.queued;
  m->pdschq_decoded = pq.decoded;
  m->pdschq_crc_ok = pq.crc_ok;
  m->pdschq_drop_full = pq.dropped_full;
  m->pdschq_drop_stale = pq.dropped_stale;
  m->pdschq_stale_after_decode = pq.stale_after_decode;
  m->pdschq_max_lag = pq.max_lag_slots;
  nr_pdsch_passive_ldpc_counters(&m->ldpc_ok, &m->ldpc_seg_fail, &m->ldpc_tb_fail, &m->ldpc_zero_tb);
  nr_pusch_passive_counters(&m->pusch_try, &m->pusch_crc_ok);
  nr_passive_obs_stats(&m->obs_pushed, &m->obs_written, &m->obs_dropped);
}

void nr_passive_metrics_emit(void)
{
  /* The file handle is opened lazily by whichever scan consumer first wins summary_due_now(); with N
   * consumers (Task A7) successive winners are different threads, so the open/append is serialised. */
  static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
  static FILE *f = NULL;
  static int tried = 0;
  nr_passive_metrics_t m;
  char buf[2048];
  nr_passive_metrics_collect(&m);
  if (nr_passive_metrics_to_json(&m, buf, sizeof(buf)) < 0) {
    LOG_W(PHY, "SENSING: ISAC_METRICS buffer too small\n");
    return;
  }
  LOG_A(PHY, "SENSING: ISAC_METRICS %s\n", buf);
  pthread_mutex_lock(&mu);
  if (!tried) {
    tried = 1;
    const char *p = getenv("ISAC_METRICS_PATH");
    if (p && *p) {
      f = fopen(p, "a");
      if (!f)
        LOG_W(PHY, "SENSING: ISAC_METRICS_PATH=%s cannot be opened, file output off\n", p);
      else
        setvbuf(f, NULL, _IOLBF, 0);
    }
  }
  if (f)
    fprintf(f, "%s\n", buf);
  pthread_mutex_unlock(&mu);
}
