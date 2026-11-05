/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#define _GNU_SOURCE
#include "common/utils/nr_cell.h"
#include "nr_passive_cell_prealloc.h"
#include <string.h>
#include "nr_passive_metrics.h"
#include "PHY/TOOLS/nr_gpu_budget.h"
#include <math.h>
#include "nr_pdcch_blind_monitor_rt.h"
#include "nr_td_grantwork.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/resource.h>
#include <dlfcn.h>
#include "common/utils/LOG/log.h"
#include "nr_passive_acq_state.h"
#include "nr_pdcch_passive_queue.h"
#include "nr_pdsch_passive_queue.h"
#include "nr_pdcch_blind_monitor_rt.h"
#include "nr_pdsch_passive_decode.h"
#include "nr_pusch_passive_decode.h"
#include "nr_td_order.h"
#include "nr_pdsch_config_sweep.h"
#include "nr_td_cb0_wire.h"
#include "nr_pusch_passive_queue.h"
#include "nr_csirs_blind_rt.h"
#include "nr_csirs_disc.h"
#include "nr_prach_passive.h"
#include "nr_sense_api.h"
#include "PHY/TOOLS/nr_admit.h"
#include "PHY/NR_UE_ISAC/nr_isac_mode.h"
#include "nr_sense_unit.h"
#include "nr_sense_sched_rt.h"
#include "nr_pdcch_uss_prune.h"
__attribute__((weak)) void nr_sense_stats(nr_sense_stats_t *out);
__attribute__((weak)) void nr_prach_passive_diag(uint64_t *, uint64_t *, int *);
__attribute__((weak)) void nr_prach_passive_diag_peaks(nr_prach_passive_peaks_t *);
__attribute__((weak)) void nr_prach_passive_metrics(uint64_t *us, uint64_t *drops, uint64_t *skipped);
__attribute__((weak)) void nr_csirs_blind_rt_metrics(nr_passive_metrics_t *m);
__attribute__((weak)) void nr_csirs_disc_get_metrics(nr_csirs_disc_metrics_t *m);
__attribute__((weak)) void nr_srs_passive_metrics(uint64_t *,uint64_t *,uint64_t *,uint64_t *,uint64_t *,uint64_t *);
__attribute__((weak)) void nr_srs_passive_periodic_metrics(nr_ptrack_counters_t *, uint64_t *);
__attribute__((weak)) void nr_srs_passive_gate_metrics(uint64_t *, uint64_t *);
__attribute__((weak)) void nr_srs_passive_gate_hist(uint64_t *, uint64_t *);
__attribute__((weak)) uint64_t nr_pdcch_blind_inline_drop_epoch(void);
__attribute__((weak)) uint32_t nr_cfg_epoch_current(void);
__attribute__((weak)) uint32_t nr_cfg_epoch_identity_gen(void);
__attribute__((weak)) void nr_pdsch_recovery_stats(char *buf, int n);
__attribute__((weak)) bool nr_pdsch_recovery_enabled(void);
__attribute__((weak)) void nr_pdsch_retry_yield_stats(uint64_t *chains, uint64_t *yields, uint64_t *gated);

#include "PHY/NR_UE_TRANSPORT/nr_ue_cell_clock.h" /* per-cell slot clock (A8): replaces the extern of the former process global */
/* mpci A3: the PCI of the cell being reported. The exported global (extern in nr_passive_metrics.h, written by executables/nr-ue.c)
 * is the cell-0 / single-cell slot, so N=1 is unchanged; cells k > 0 use the cell-indexed slot written by nr_passive_metrics_set_pci()
 * (A8: nr-ue.c should call it, with the cell's thread bound, instead of storing the global). */
_Atomic int nr_passive_metrics_pci = -1;
NR_CELL_STATIC_INIT(_Atomic int, nr_passive_metrics_pci_slot, -1);
void nr_passive_metrics_set_pci(int pci)
{
  const int c = nr_cell_idx();
  atomic_store_explicit(&nr_passive_metrics_pci_slot_cells[c], pci, memory_order_relaxed);
  if (c == 0)
    atomic_store_explicit(&nr_passive_metrics_pci, pci, memory_order_relaxed);
}
/* MPCI-DASH P6/P9: per-cell sync / discontinuity event counters of the cell's receive thread (events, not per slot), read by the CELL_TIMING and CELL_SUM lines.
 * Bumped from the cell-bound thread (nr_cell_idx()); the sync age is the CLOCK_MONOTONIC ns of the last "Got synch". */
NR_CELL_STATIC(_Atomic uint64_t, nr_dash_ev_sync_n);
NR_CELL_STATIC(_Atomic uint64_t, nr_dash_ev_sync_ns);
NR_CELL_STATIC(_Atomic uint64_t, nr_dash_ev_disc_n);
void nr_dash_note_sync(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  const int c = nr_cell_idx();
  atomic_store_explicit(&nr_dash_ev_sync_ns_cells[c], (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec, memory_order_relaxed);
  atomic_fetch_add_explicit(&nr_dash_ev_sync_n_cells[c], 1, memory_order_relaxed);
}
void nr_dash_note_discont(void)
{
  atomic_fetch_add_explicit(&nr_dash_ev_disc_n_cells[nr_cell_idx()], 1, memory_order_relaxed);
}
void nr_dash_event_counters(int cell, uint64_t *sync_n, uint64_t *sync_ns, uint64_t *disc_n)
{
  const int c = cell >= 0 && cell < NR_CELL_MAX ? cell : 0;
  *sync_n = atomic_load_explicit(&nr_dash_ev_sync_n_cells[c], memory_order_relaxed);
  *sync_ns = atomic_load_explicit(&nr_dash_ev_sync_ns_cells[c], memory_order_relaxed);
  *disc_n = atomic_load_explicit(&nr_dash_ev_disc_n_cells[c], memory_order_relaxed);
}
/* MPCI-DASH: the PCI of cell `cell` (cell 0 = the global, as in collect), -1 until its PBCH lock; for the per-cell log lines that carry pci= explicitly */
int nr_passive_metrics_pci_of(int cell)
{
  if (cell <= 0)
    return atomic_load_explicit(&nr_passive_metrics_pci, memory_order_relaxed);
  if (cell >= NR_CELL_MAX)
    return -1;
  return atomic_load_explicit(&nr_passive_metrics_pci_slot_cells[cell], memory_order_relaxed);
}
/* Filled by Task A3; weak so this file links before A3 lands. */
__attribute__((weak)) void nr_passive_obs_stats(uint64_t *pushed, uint64_t *written, uint64_t *dropped)
{
  *pushed = *written = *dropped = 0;
}

/* Optional when linking the metrics collector without the capture path. */
extern void nr_pdcch_nid_capture_metrics(uint64_t *, uint64_t *, uint64_t *, float *) __attribute__((weak));

/* nr_passive_metrics_collect: gathers a snapshot of every passive-receiver counter into *m: the PDCCH/PDSCH/PUSCH queues, the CSI-RS and SRS discovery, the NID capture, the GPU budget, the CB0 path, the RX recovery and the configuration epochs.
 *   Summary-level header (the function is long: one block per subsystem). Reads the counters through their public getters only. Caller: nr_passive_metrics_emit() in this file.
 */
extern void nr_resync_cfo_stats(uint64_t *taken, uint64_t *kept, uint64_t *unresolved) __attribute__((weak));
void nr_passive_metrics_collect(nr_passive_metrics_t *m)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  *m = (nr_passive_metrics_t){0};
  if (nr_pdcch_nid_capture_metrics) {
    float noise;
    nr_pdcch_nid_capture_metrics(&m->nid_capture_offered, &m->nid_capture_taken, &m->nid_capture_skipped_empty, &noise);
    m->nid_capture_noise_floor_db = noise > 0 ? 10.0 * log10(noise) : -120.0;
  }
  m->t_mono_ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
  clock_gettime(CLOCK_REALTIME, &ts);
  m->t_real_ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
  struct rusage ru;
  if (getrusage(RUSAGE_SELF, &ru) == 0) {
    m->proc_utime_us = (uint64_t)ru.ru_utime.tv_sec * 1000000ull + (uint64_t)ru.ru_utime.tv_usec;
    m->proc_stime_us = (uint64_t)ru.ru_stime.tv_sec * 1000000ull + (uint64_t)ru.ru_stime.tv_usec;
    m->proc_maxrss_kb = (uint64_t)ru.ru_maxrss;
    m->proc_minflt = (uint64_t)ru.ru_minflt;
    m->proc_majflt = (uint64_t)ru.ru_majflt;
  }
  if (nr_cfg_epoch_current && nr_cfg_epoch_identity_gen) {
    m->cfg_epoch = nr_cfg_epoch_current();
    m->cfg_identity_gen = nr_cfg_epoch_identity_gen();
    m->has_cfg_epoch = 1;
  }
  if (nr_pdsch_recovery_stats && nr_pdsch_recovery_enabled && nr_pdsch_recovery_enabled()) { /* text getter: parsed back, a short parse leaves has_recov 0; recovery off = keys absent, not 0 */
    char rs[256];
    nr_pdsch_recovery_stats(rs, sizeof rs);
    unsigned long v[7];
    int p50, p99;
    if (sscanf(rs, "RECOV submitted=%lu rescued=%lu stale=%lu expired=%lu dropped_full=%lu dropped_old=%lu yielded=%lu late_ms_p50/p99=%d/%d",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &p50, &p99) == 9) {
      m->recov_submitted = v[0]; m->recov_rescued = v[1]; m->recov_stale = v[2]; m->recov_expired = v[3];
      m->recov_dropped_full = v[4]; m->recov_dropped_old = v[5]; m->recov_yielded = v[6];
      m->recov_late_ms_p50 = p50; m->recov_late_ms_p99 = p99;
      m->has_recov = 1;
    }
  }
  if (nr_pdsch_retry_yield_stats) {
    nr_pdsch_retry_yield_stats(&m->retry_yield_chains, &m->retry_yield_yields, &m->retry_yield_gated);
    m->has_retry_yield = 1;
  }
  if (nr_isac_mode() == NR_ISAC_MODE_TRAFFIC) { m->has_traffic = 1; nr_traffic_stats_get(&m->traffic); }
  if (nr_isac_mode() == NR_ISAC_MODE_SENSING) {
    nr_sense_unit_cell_stats_t us;
    nr_sense_unit_cell_stats(nr_cell_idx(), &us);
    nr_sense_sched_stats_t ss;
    nr_sense_sched_stats(&ss);
    m->has_sense_sched = 1;
    m->su_units_enabled = us.units_enabled; m->su_violations = us.violations; m->su_disabled_skips = us.skips;
    m->su_wb_valid = us.wb_valid; m->su_wb_valid_prbs = us.wb_valid_prbs; m->su_gap_max_ms = us.gap_max_ns / 1000000ull;
    { bool ig = false; uint64_t g = 0; m->su_floor_prb = (uint64_t)nr_sense_sched_floor_info(nr_cell_idx(), &g, &ig); m->su_cont_gap_p95_ms = g; m->su_in_gap = ig; }
    m->ss_decisions = ss.decisions; m->ss_enables = ss.enables; m->ss_disables = ss.disables; m->ss_bad_cap = ss.bad_cap; m->ss_deferrals = ss.deferrals;
    m->ss_binding = ss.decisions ? ss.binding : -1;
  }
  m->abs_slot = (int64_t)atomic_load_explicit(&nr_ue_diag_producer_absolute_slot, memory_order_relaxed);
  if(nr_srs_passive_metrics) nr_srs_passive_metrics(&m->srs_us,&m->srs_jobs_dropped,
      &m->srs_pregate_skip,&m->srs_backoff_skip,&m->srs_budget_skip,&m->srs_idle_resets);
  if(nr_srs_passive_periodic_metrics) nr_srs_passive_periodic_metrics(&m->periodic[NR_PTRACK_SRS],&m->srs_audits);
  if(nr_srs_passive_gate_metrics) nr_srs_passive_gate_metrics(&m->srs_gate_open_unexplained,&m->srs_gate_open_pusch);
  if(nr_srs_passive_gate_hist) nr_srs_passive_gate_hist(m->srs_gate_hist,m->srs_gate_det);
  m->pci = nr_cell_idx() == 0 ? atomic_load_explicit(&nr_passive_metrics_pci, memory_order_relaxed)
                               : atomic_load_explicit(&nr_passive_metrics_pci_slot_cells[nr_cell_idx()], memory_order_relaxed);
  const nr_passive_acq_snapshot_t a = nr_passive_acq_snapshot();
  m->acq_state = nr_passive_acq_state_name(a.state);
  m->acq_transitions = a.transitions;
  m->acq_sync_losses = a.sync_losses;
  m->acq_pbch_locks = a.pbch_locks;
  m->acq_sib1_decodes = a.sib1_decodes;
  if (nr_resync_cfo_stats) /* weak: absent in binaries without the initial sync */
    nr_resync_cfo_stats(&m->cfo_alias_taken, &m->cfo_alias_kept, &m->cfo_unresolved);
  {
    nr_uss_prune_stats_t us;
    nr_uss_prune_stats(&us);
    m->uss_pruned = us.pruned;
    m->uss_audit_tasks = us.audit_tasks;
    m->uss_audit_miss = us.audit_miss;
    m->uss_rnti_demotions = us.rnti_demotions;
    m->uss_rnti_repromotions = us.rnti_repromotions;
    m->uss_geom_resets = us.geom_resets;
    m->uss_fits = us.fits;
  }
  nr_pdcch_blind_monitor_counters(&m->pdcch_occasions, &m->pdcch_candidates, &m->pdcch_accepts, &m->pdcch_accepts_c);
  nr_pdcch_passive_queue_stats_t sq;
  nr_pdcch_passive_queue_get_stats(&sq);
  m->scanq_queued = sq.queued;
  m->scanq_processed = sq.processed;
  m->scanq_drop_full = sq.dropped_full;
  m->scanq_drop_stale = sq.dropped_stale;
  m->scanq_drop_epoch = sq.dropped_epoch;
  if (nr_pdcch_blind_inline_drop_epoch)
    m->pdcch_inline_drop_epoch = nr_pdcch_blind_inline_drop_epoch();
  m->scanq_max_lag = sq.max_lag_slots;
  nr_pdsch_passive_queue_stats_t pq;
  nr_pdsch_passive_queue_get_stats(&pq);
  m->pdsch_schedule_enabled = nr_csirs_disc_enabled();
  m->pdsch_schedule = pq.schedule;
  m->pdschq_drop_narrow = pq.dropped_narrow;
  m->pdschq_queued = pq.queued;
  m->pdschq_decoded = pq.decoded;
  m->pdschq_crc_ok = pq.crc_ok;
  m->pdschq_drop_full = pq.dropped_full;
  m->pdschq_drop_stale = pq.dropped_stale;
  m->pdschq_stale_after_decode = pq.stale_after_decode;
  m->pdschq_max_lag = pq.max_lag_slots;
  nr_gpu_budget_metrics_t budget;nr_gpu_budget_metrics(&budget);
  m->gpu_budget_used_pct=budget.gpu_budget_used_pct;
  m->discovery_yields=budget.discovery_yields;
  m->rt_gpu_wait_max_us=budget.rt_gpu_wait_max_us;
  m->rt_gpu_fallbacks=budget.rt_gpu_fallbacks;
  m->discovery_overruns=budget.discovery_overruns;
  nr_dl_gpu_get_stats(&m->dl_gpu_allocs, &m->dl_gpu_late_alloc_denied, &m->dl_gpu_transfer_bytes, &m->dl_gpu_transfer_refused);
  m->gw_pool_refused = nr_td_gw_pool_refused();
  m->gpu_fep_stale_pre = pq.gpu_stale_pre;
  m->gpu_fep_stale_post = pq.gpu_stale_post;
  m->gpu_fep_selfcheck_fail = pq.gpu_selfcheck_fail;
  m->gpu_fep_selfcheck_skipped = pq.gpu_selfcheck_skipped;
  m->gpu_fep_disabled = pq.gpu_disabled;
  nr_pdsch_config_sweep_excl_restart_stats(&m->td_excl_restarts, &m->td_excl_truncs, &m->td_excl_restart_alarms);
  nr_pdsch_config_sweep_fieldbook_stats(&m->td_fb_promotions, &m->td_fb_withdrawals, &m->td_fb_failopens, &m->td_fb_pruned_contexts, &m->td_fb_untrusted_ctx);
  uint64_t cs[3][3], cd[3][3];
  nr_td_census_get(cs);
  nr_td_census_get_deftab(cd);
  m->td_sib1_tdra_match_10 = cs[NR_TD_FMT_10][NR_TD_CENSUS_MATCH];
  m->td_sib1_tdra_mismatch_10 = cs[NR_TD_FMT_10][NR_TD_CENSUS_MISMATCH];
  m->td_sib1_tdra_none_10 = cs[NR_TD_FMT_10][NR_TD_CENSUS_NONE];
  m->td_sib1_tdra_match_11 = cs[NR_TD_FMT_11][NR_TD_CENSUS_MATCH];
  m->td_sib1_tdra_mismatch_11 = cs[NR_TD_FMT_11][NR_TD_CENSUS_MISMATCH];
  m->td_sib1_tdra_none_11 = cs[NR_TD_FMT_11][NR_TD_CENSUS_NONE];
  m->td_sib1_tdra_match_unk = cs[NR_TD_FMT_UNK][NR_TD_CENSUS_MATCH];
  m->td_sib1_tdra_mismatch_unk = cs[NR_TD_FMT_UNK][NR_TD_CENSUS_MISMATCH];
  m->td_sib1_tdra_none_unk = cs[NR_TD_FMT_UNK][NR_TD_CENSUS_NONE];
  m->td_deftab_match_10 = cd[NR_TD_FMT_10][NR_TD_CENSUS_MATCH];
  m->td_deftab_mismatch_10 = cd[NR_TD_FMT_10][NR_TD_CENSUS_MISMATCH];
  m->td_deftab_match_11 = cd[NR_TD_FMT_11][NR_TD_CENSUS_MATCH];
  m->td_deftab_mismatch_11 = cd[NR_TD_FMT_11][NR_TD_CENSUS_MISMATCH];
  m->td_deftab_na = cd[0][NR_TD_CENSUS_NONE] + cd[1][NR_TD_CENSUS_NONE] + cd[2][NR_TD_CENSUS_NONE];
  {
    nr_td_cb0_wire_stats_t c;
    nr_td_cb0_wire_stats(&c);
    m->td_cb0_grants = c.grants;
    m->td_cb0_batches = c.batches;
    m->td_cb0_admissible = c.admissible;
    m->td_cb0_items = c.items;
    for (int r = 0; r < 16 && r < NR_TD_CB0_R_COUNT; r++)
      m->td_cb0_inadmissible[r] = c.inadmissible[r];
    m->td_cb0_budget_skips = c.budget_skips;
    m->td_cb0_not_testable = c.not_testable;
    m->td_cb0_us_per_item = c.items ? (double)c.cpu_us / (double)c.items : 0.0;
    m->td_cb0_backend_cpu = c.backend_cpu;
    m->td_cb0_backend_gpu = c.backend_gpu;
    m->td_cb0_premise_alarms = c.premise_alarms;
    m->td_cb0_eliminations = c.eliminations;
    m->td_cb0_gpu_submits = c.gpu_submits;
    m->td_cb0_gpu_items = c.gpu_items;
    m->td_cb0_gpu_ok = c.gpu_ok;
    m->td_cb0_gpu_errors = c.gpu_errors;
    m->td_cb0_gpu_timeouts = c.gpu_timeouts;
    m->td_cb0_gpu_bypassed = c.gpu_bypassed;
    m->td_cb0_gpu_sticky = c.gpu_sticky;
    m->td_cb0_gpu_trips = c.gpu_trips;
    m->td_cb0_gpu_state = c.gpu_state;
    m->td_cb0_gpu_mode = c.gpu_mode;
    m->td_cb0_gpu_failed = c.gpu_failed;
    m->td_cb0_gpu_skipped = c.gpu_skipped;
  }
  m->pdschq_drop_epoch = pq.dropped_epoch;
  nr_pusch_passive_queue_stats_t uq;
  nr_pusch_passive_queue_get_stats(&uq);
  m->puschq_drop_epoch = uq.dropped_epoch;
  nr_pdsch_passive_ldpc_counters(&m->ldpc_ok, &m->ldpc_seg_fail, &m->ldpc_tb_fail, &m->ldpc_zero_tb);
  {
    nr_pdsch_passive_harq_stats_t hq;
    nr_pdsch_passive_harq_stats(&hq);
    m->hq_retx_combined = hq.retx_combined;
    m->hq_retx_crc_ok = hq.retx_crc_ok;
    m->hq_retx_no_history = hq.retx_no_history;
    m->hq_retx_no_history_ok = hq.retx_no_history_ok;
    m->hq_retx_refused = hq.retx_refused;
    m->hq_evicted = hq.evicted_ndi + hq.evicted_lru + hq.evicted_age;
  }
  { /* libldpc_cuda.so is dlopen'd RTLD_GLOBAL when --loader.ldpc.shlibversion _cuda is used; absent = zeros */
    typedef void (*cuda_ctr_t)(uint64_t *, uint64_t *, uint64_t *, uint64_t *);
    static cuda_ctr_t fn;
    if (!fn) /* keep looking until the plugin is loaded */
      fn = (cuda_ctr_t)dlsym(RTLD_DEFAULT, "ldpc_cuda_get_counters4");
    if (fn)
      fn(&m->ldpc_cuda_errors, &m->ldpc_cuda_fallbacks, &m->ldpc_cuda_poisoned, &m->ldpc_cuda_disabled);
  }
  { typedef uint64_t (*trips_t)(void);
    static trips_t tf;
    if (!tf)
      tf = (trips_t)dlsym(RTLD_DEFAULT, "ldpc_cuda_breaker_trips");
    if (tf)
      m->ldpc_cuda_breaker_trips = tf();
  }
  nr_pdsch_passive_ldpc_tb_decoders(&m->ldpc_tb_cpu, &m->ldpc_tb_cuda);
  nr_pusch_passive_counters(&m->pusch_try, &m->pusch_crc_ok);
  nr_passive_obs_stats(&m->obs_pushed, &m->obs_written, &m->obs_dropped);
  if (nr_isac_mode() != NR_ISAC_MODE_LEGACY) /* legacy output unchanged */
  nr_admit_class_counts(NR_ADMIT_JANON, &m->admit_cls[0][0], &m->admit_cls[0][1], &m->admit_cls[0][2], &m->admit_cls[0][3]);
  if (nr_isac_mode() != NR_ISAC_MODE_LEGACY)
  nr_admit_class_counts(NR_ADMIT_DMRS_ID, &m->admit_cls[1][0], &m->admit_cls[1][1], &m->admit_cls[1][2], &m->admit_cls[1][3]);
  if (nr_sense_stats) {
    nr_sense_stats_t sense;
    nr_sense_stats(&sense);
    m->sense_recon_skip = sense.recon_skip;
    memcpy(m->srs_snr_hist, sense.srs_snr_hist, sizeof m->srs_snr_hist);
    m->sense_ul_delay_skip = sense.ul_delay_skip;
    for (int source = 0; source < NR_SENSE_SRC_COUNT; ++source) {
      m->sense_cfr_drop += sense.cfr_dropped[source];
      m->sense_cfr_drop_src[source] = sense.cfr_dropped[source];
      m->sense_cfr_exported_src[source] = sense.cfr_pushed_src[source];
      m->sense_async_queued_src[source] = sense.async_queued[source];
      m->sense_async_drop_src[source] = sense.async_dropped[source];
      m->sense_prod_us[source]=sense.prod_ns[source]/1000;
      m->sense_pack_us[source]=sense.pack_ns[source]/1000;
      m->sense_prod_count[source]=sense.prod_count[source];
      m->sense_pack_count[source]=sense.pack_count[source];
    }
    m->sense_unexported_pdsch_data = m->pdschq_crc_ok > m->sense_cfr_exported_src[NR_SENSE_SRC_PDSCH_DATA]
        ? m->pdschq_crc_ok - m->sense_cfr_exported_src[NR_SENSE_SRC_PDSCH_DATA] : 0;
    m->sense_unexported_pusch_data = m->pusch_crc_ok > m->sense_cfr_exported_src[NR_SENSE_SRC_PUSCH_DATA]
        ? m->pusch_crc_ok - m->sense_cfr_exported_src[NR_SENSE_SRC_PUSCH_DATA] : 0;
    m->sense_rec_drop = sense.rec_dropped;
    m->sense_io_errors = sense.io_errors;
    m->sense_cfr_no_sink = sense.cfr_no_sink;
    m->sense_rec_us=sense.rec_ns/1000;
    m->sense_write_us=sense.write_ns/1000;
    m->sense_rec_count=sense.rec_count;
    m->sense_write_count=sense.write_count;
    m->dci_rec_generated=sense.dci_generated; m->dci_rec_persisted=sense.dci_persisted; m->dci_rec_lost=sense.dci_lost;
  }
  if (nr_prach_passive_metrics)
    nr_prach_passive_metrics(&m->prach_us, &m->prach_drops, &m->prach_skipped);
  if (nr_prach_passive_diag)
    nr_prach_passive_diag(&m->prach_occasions, &m->prach_detections, &m->prach_margin_max);
  if (nr_prach_passive_diag_peaks) {
    nr_prach_passive_peaks_t pk;
    nr_prach_passive_diag_peaks(&pk);
    m->prach_peak_last = pk.peak_last; m->prach_peak_min = pk.peak_min; m->prach_peak_max = pk.peak_max;
    m->prach_peak_mean = pk.peak_mean; m->prach_floor_last = pk.floor_last;
    m->prach_peak_zero = pk.peak_zero; m->prach_seeded = pk.seeded;
    m->prach_lb_tried = pk.lb_tried; m->prach_lb_found = pk.lb_found; m->prach_lb_noring = pk.lb_noring; m->prach_snap_zero = pk.snap_zero; m->prach_snap_db = pk.snap_db;
  }
  if (nr_csirs_blind_rt_metrics) nr_csirs_blind_rt_metrics(m);
  if (nr_csirs_disc_get_metrics) {
    nr_csirs_disc_metrics_t d;
    nr_csirs_disc_get_metrics(&d);
    m->csirs_disc_batches = d.batches;
    m->csirs_disc_footprints = d.footprints;
    m->csirs_disc_hyps = d.hyps;
    m->csirs_disc_winners = d.winners;
    m->csirs_disc_ambiguous = d.ambiguous;
    m->csirs_disc_feed_drops = d.feed_drops;
    m->csirs_disc_gpu_batches = d.gpu_batches;
    m->csirs_disc_cpu_batches = d.cpu_batches;
    m->csirs_disc_batch_us = d.batch_us;
  }
}

void nr_passive_metrics_emit(void)
{
  /* The file handle is opened lazily by whichever scan consumer first wins summary_due_now(); with N
   * consumers (Task A7) successive winners are different threads, so the open/append is serialised. */
  static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
  static FILE *f = NULL;
  static int tried = 0;
  nr_passive_metrics_t m;
  char buf[20480]; /* levers, robustness, and per-source sensing metrics */
  nr_passive_metrics_collect(&m);
  if (nr_passive_metrics_to_json(&m, buf, sizeof(buf)) < 0) {
    LOG_W(PHY, "SENSING: ISAC_METRICS buffer too small\n");
    return;
  }
  /* multi-cell process (nr_cell_count() > 1): the record names its cell, so the metrics file can be split per cell.
   * One cell: no "cell" key, the record is byte-identical to the pre-multi-cell receiver (N=1 equivalence). */
  if (nr_cell_count() > 1 && buf[0] == '{') {
    char tag[24];
    const int tl = snprintf(tag, sizeof(tag), "\"cell\":%d,", nr_cell_idx());
    const size_t bl = strlen(buf);
    if (bl + (size_t)tl + 1 <= sizeof(buf)) {
      memmove(buf + 1 + tl, buf + 1, bl - 1 + 1);
      memcpy(buf + 1, tag, (size_t)tl);
    }
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
