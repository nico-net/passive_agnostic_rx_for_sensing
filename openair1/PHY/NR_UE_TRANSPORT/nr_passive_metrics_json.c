/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_passive_metrics.h"
#include <inttypes.h>
#include <stdio.h>

/* Keys of td_cb0_inadmissible: nr_td_cb0_reason_name order (nr_td_cb0_sched.h, NR_TD_CB0_R_*). */
static const char *const kCb0Reason[16] = {"not_new_rv0", "gated",      "iq_stale", "lbrm",    "rv_retry", "prg_ptrs",
                                           "member_stale", "llr_scale", "gpu_llr", "ldpc_error", "rank",     "decoder",
                                           "contract",   "budget",     "no_grantwork", "reindexed"};

static int cb0_json(const nr_passive_metrics_t *m, char *buf, size_t n)
{
  int w = snprintf(buf, n,
                   ",\"td_cb0_grants\":%" PRIu64 ",\"td_cb0_batches\":%" PRIu64 ",\"td_cb0_admissible\":%" PRIu64
                   ",\"td_cb0_items\":%" PRIu64 ",\"td_cb0_inadmissible\":{",
                   m->td_cb0_grants, m->td_cb0_batches, m->td_cb0_admissible, m->td_cb0_items);
  for (int r = 0; r < 16 && w >= 0 && (size_t)w < n; r++)
    w += snprintf(buf + w, n - w, "%s\"%s\":%" PRIu64, r ? "," : "", kCb0Reason[r], m->td_cb0_inadmissible[r]);
  if (w >= 0 && (size_t)w < n)
    w += snprintf(buf + w, n - w,
                  "},\"td_cb0_budget_skips\":%" PRIu64 ",\"td_cb0_not_testable\":%" PRIu64 ",\"td_cb0_us_per_item\":%.1f"
                  ",\"td_cb0_backend\":{\"cpu\":%" PRIu64 ",\"gpu\":%" PRIu64 "},\"td_cb0_premise_alarms\":%" PRIu64
                  ",\"td_cb0_eliminations\":%" PRIu64
                  ",\"td_cb0_gpu\":{\"submits\":%" PRIu64 ",\"items\":%" PRIu64 ",\"ok\":%" PRIu64 ",\"errors\":%" PRIu64
                  ",\"timeouts\":%" PRIu64 ",\"bypassed\":%" PRIu64 ",\"sticky\":%" PRIu64 ",\"trips\":%" PRIu64
                  ",\"state\":%" PRIu64 ",\"mode\":%" PRIu64 ",\"failed\":%" PRIu64 ",\"skipped\":%" PRIu64 "}",
                  m->td_cb0_budget_skips, m->td_cb0_not_testable, m->td_cb0_us_per_item, m->td_cb0_backend_cpu,
                  m->td_cb0_backend_gpu, m->td_cb0_premise_alarms, m->td_cb0_eliminations, m->td_cb0_gpu_submits,
                  m->td_cb0_gpu_items, m->td_cb0_gpu_ok, m->td_cb0_gpu_errors, m->td_cb0_gpu_timeouts, m->td_cb0_gpu_bypassed,
                  m->td_cb0_gpu_sticky, m->td_cb0_gpu_trips, m->td_cb0_gpu_state, m->td_cb0_gpu_mode, m->td_cb0_gpu_failed,
                  m->td_cb0_gpu_skipped);
  if (w >= 0 && (size_t)w < n)
    w += snprintf(buf + w, n - w,
                  ",\"dl_gpu_allocs\":%" PRIu64 ",\"dl_gpu_late_alloc_denied\":%" PRIu64
                  ",\"dl_gpu_transfer_bytes\":%" PRIu64 ",\"dl_gpu_transfer_refused\":%" PRIu64
                  ",\"gw_pool_refused\":%" PRIu64,
                  m->dl_gpu_allocs, m->dl_gpu_late_alloc_denied, m->dl_gpu_transfer_bytes, m->dl_gpu_transfer_refused,
                  m->gw_pool_refused);
  return (w < 0 || (size_t)w >= n) ? -1 : w;
}

/* Appended only with T21 enabled: the flag-off JSON remains byte-identical. */
static int pdsch_schedule_json(const nr_passive_metrics_t *m, char *buf, size_t n)
{
  static const char *const stage[] = {"enqueue_dequeue", "enqueue_fep", "fep_ldpc", "enqueue_ldpc", "gpu_wait"};
  static const char *const reason[] = {"queue_expired", "future_unready", "copy_expired",
                                      "unsupported", "decode_error", "gpu_fallback"};
  if (!m->pdsch_schedule_enabled) { if (n) buf[0] = 0; return n ? 0 : -1; }
  int w = snprintf(buf, n, ",\"pdschq_drop_narrow\":%" PRIu64
                   ",\"pdschq_latency_us_bounds\":[100,250,500,1000,2000,4000,8000,16000],\"pdschq_latency\":{",
                   m->pdschq_drop_narrow);
  for (int s = 0; s < NR_PDSCH_LAT_STAGES && w >= 0 && (size_t)w < n; ++s) {
    w += snprintf(buf + w, n - w, "%s\"%s\":[", s ? "," : "", stage[s]);
    for (int b = 0; b < NR_PDSCH_LAT_BINS && w >= 0 && (size_t)w < n; ++b)
      w += snprintf(buf + w, n - w, "%s%" PRIu64, b ? "," : "", m->pdsch_schedule.hist[s][b]);
    if (w >= 0 && (size_t)w < n) w += snprintf(buf + w, n - w, "]");
  }
  if (w >= 0 && (size_t)w < n) w += snprintf(buf + w, n - w, "},\"pdschq_reasons\":{");
  for (int r = 0; r < NR_PDSCH_DROP_REASONS && w >= 0 && (size_t)w < n; ++r)
    w += snprintf(buf + w, n - w, "%s\"%s\":%" PRIu64, r ? "," : "", reason[r], m->pdsch_schedule.reason[r]);
  if (w >= 0 && (size_t)w < n) w += snprintf(buf + w, n - w, "}");
  return (w < 0 || (size_t)w >= n) ? -1 : w;
}

int nr_passive_metrics_to_json(const nr_passive_metrics_t *m, char *buf, size_t n)
{
  const char *st = m->acq_state ? m->acq_state : "UNKNOWN";
  char cb0[2048];
  if (cb0_json(m, cb0, sizeof(cb0)) < 0)
    return -1;
  static const char *const source[] = {"ssb","csirs","pdsch_dmrs","pdsch_data","pusch_dmrs","pusch_data","prach","srs"};
  char sense_cost[2048];
  size_t cost_len=0;
  for (int s=0;s<NR_SENSE_SRC_COUNT;s++) {
    int z=snprintf(sense_cost+cost_len,sizeof(sense_cost)-cost_len,
      ",\"sense_prod_us_%s\":%" PRIu64 ",\"sense_pack_us_%s\":%" PRIu64
      ",\"sense_prod_count_%s\":%" PRIu64 ",\"sense_pack_count_%s\":%" PRIu64,
      source[s],m->sense_prod_us[s],source[s],m->sense_pack_us[s],
      source[s],m->sense_prod_count[s],source[s],m->sense_pack_count[s]);
    if (z<0 || (size_t)z>=sizeof(sense_cost)-cost_len) return -1;
    cost_len+=(size_t)z;
  }
  int cost_z=snprintf(sense_cost+cost_len,sizeof(sense_cost)-cost_len,
    ",\"sense_rec_us\":%" PRIu64 ",\"sense_write_us\":%" PRIu64
    ",\"sense_rec_count\":%" PRIu64 ",\"sense_write_count\":%" PRIu64,
    m->sense_rec_us,m->sense_write_us,m->sense_rec_count,m->sense_write_count);
  if (cost_z<0 || (size_t)cost_z>=sizeof(sense_cost)-cost_len) return -1;
  int w = snprintf(buf, n,
      "{\"schema\":%d,\"t_mono_ns\":%" PRIu64 ",\"abs_slot\":%" PRId64 ",\"pci\":%d,\"acq_state\":\"%s\","
      "\"acq_transitions\":%" PRIu64 ",\"acq_sync_losses\":%" PRIu64 ",\"acq_pbch_locks\":%" PRIu64 ",\"acq_sib1_decodes\":%" PRIu64 ","
      "\"pdcch_occasions\":%" PRIu64 ",\"pdcch_candidates\":%" PRIu64 ",\"pdcch_accepts\":%" PRIu64 ",\"pdcch_accepts_c\":%" PRIu64 ","
      "\"scanq_queued\":%" PRIu64 ",\"scanq_processed\":%" PRIu64 ",\"scanq_drop_full\":%" PRIu64 ",\"scanq_drop_stale\":%" PRIu64 ",\"scanq_drop_epoch\":%" PRIu64 ",\"scanq_max_lag\":%" PRIu64 ",\"pdcch_inline_drop_epoch\":%" PRIu64 ","
      "\"pdschq_queued\":%" PRIu64 ",\"pdschq_decoded\":%" PRIu64 ",\"pdschq_crc_ok\":%" PRIu64 ",\"pdschq_drop_full\":%" PRIu64 ",\"pdschq_drop_stale\":%" PRIu64 ",\"pdschq_drop_epoch\":%" PRIu64 ",\"pdschq_stale_after_decode\":%" PRIu64 ",\"pdschq_max_lag\":%" PRIu64 ",\"puschq_drop_epoch\":%" PRIu64 ","
      "\"gpu_fep_stale_pre\":%" PRIu64 ",\"gpu_fep_stale_post\":%" PRIu64 ",\"gpu_fep_selfcheck_fail\":%" PRIu64 ",\"gpu_fep_selfcheck_skipped\":%" PRIu64 ",\"gpu_fep_disabled\":%" PRIu64 ","
      "\"td_sib1_tdra_match_10\":%" PRIu64 ",\"td_sib1_tdra_mismatch_10\":%" PRIu64 ",\"td_sib1_tdra_none_10\":%" PRIu64 ","
      "\"td_sib1_tdra_match_11\":%" PRIu64 ",\"td_sib1_tdra_mismatch_11\":%" PRIu64 ",\"td_sib1_tdra_none_11\":%" PRIu64 ","
      "\"td_sib1_tdra_match_unk\":%" PRIu64 ",\"td_sib1_tdra_mismatch_unk\":%" PRIu64 ",\"td_sib1_tdra_none_unk\":%" PRIu64 ","
      "\"td_deftab_match_10\":%" PRIu64 ",\"td_deftab_mismatch_10\":%" PRIu64 ",\"td_deftab_match_11\":%" PRIu64 ",\"td_deftab_mismatch_11\":%" PRIu64 ",\"td_deftab_na\":%" PRIu64 ","
      "\"td_excl_restarts\":%" PRIu64 ",\"td_excl_truncs\":%" PRIu64 ",\"td_excl_restart_alarms\":%" PRIu64 ","
      "\"td_fb_promotions\":%" PRIu64 ",\"td_fb_withdrawals\":%" PRIu64 ",\"td_fb_failopens\":%" PRIu64 ",\"td_fb_pruned_contexts\":%" PRIu64 ",\"td_fb_untrusted_ctx\":%" PRIu64 "%s,"
      "\"ldpc_ok\":%" PRIu64 ",\"ldpc_seg_fail\":%" PRIu64 ",\"ldpc_tb_fail\":%" PRIu64 ",\"ldpc_zero_tb\":%" PRIu64 ","
      "\"ldpc_cuda_errors\":%" PRIu64 ",\"ldpc_cuda_fallbacks\":%" PRIu64 ",\"ldpc_cuda_poisoned\":%" PRIu64 ","
      "\"ldpc_cuda_disabled\":%" PRIu64 ",\"ldpc_cuda_breaker_trips\":%" PRIu64 ",\"ldpc_tb_cpu\":%" PRIu64 ",\"ldpc_tb_cuda\":%" PRIu64 ","
      "\"srs_us\":%" PRIu64 ",\"srs_jobs_dropped\":%" PRIu64 ","
      "\"srs_pregate_skip\":%" PRIu64 ",\"srs_backoff_skip\":%" PRIu64 ","
      "\"srs_budget_skip\":%" PRIu64 ",\"srs_idle_resets\":%" PRIu64 ","
      "\"pusch_try\":%" PRIu64 ",\"pusch_crc_ok\":%" PRIu64 ","
      "\"obs_pushed\":%" PRIu64 ",\"obs_written\":%" PRIu64 ",\"obs_dropped\":%" PRIu64 ","
      "\"sense_cfr_drop\":%" PRIu64 ",\"sense_rec_drop\":%" PRIu64 ",\"sense_io_errors\":%" PRIu64 ",\"sense_cfr_no_sink\":%" PRIu64 "%s,"
      "\"prach_us\":%" PRIu64 ",\"prach_drops\":%" PRIu64 ",\"prach_skipped\":%" PRIu64 ","
      "\"csirs_candidates\":%" PRIu64 ",\"csirs_confirmed\":%" PRIu64 ",\"csirs_revoked\":%" PRIu64 ",\"zp_exported\":%" PRIu64 ",\"zp_revoked\":%" PRIu64 ","
      "\"csirs_search_us\":%" PRIu64 ",\"csirs_idsweep_us\":%" PRIu64 ",\"csirs_confirm_us\":%" PRIu64 ",\"csirs_cfr_us\":%" PRIu64 ","
      "\"csirs_disc_batches\":%" PRIu64 ",\"csirs_disc_footprints\":%" PRIu64 ",\"csirs_disc_hyps\":%" PRIu64 ",\"csirs_disc_winners\":%" PRIu64 ","
      "\"csirs_disc_ambiguous\":%" PRIu64 ",\"csirs_disc_feed_drops\":%" PRIu64 ",\"csirs_disc_gpu_batches\":%" PRIu64 ",\"csirs_disc_cpu_batches\":%" PRIu64 ",\"csirs_disc_batch_us\":%" PRIu64 ","
      "\"gpu_budget_used_pct\":%.3f,\"discovery_yields\":%" PRIu64 ",\"rt_gpu_wait_max_us\":%" PRIu64 ",\"rt_gpu_fallbacks\":%" PRIu64 ",\"discovery_overruns\":%" PRIu64 ","
      "\"csirs_time_to_confirm_slots_last\":%" PRIu64 ",\"csirs_time_to_confirm_slots_sum\":%" PRIu64 ","
      "\"csirs_time_to_confirm_resources\":[",
      NR_PASSIVE_METRICS_SCHEMA, m->t_mono_ns, m->abs_slot, m->pci, st,
      m->acq_transitions, m->acq_sync_losses, m->acq_pbch_locks, m->acq_sib1_decodes,
      m->pdcch_occasions, m->pdcch_candidates, m->pdcch_accepts, m->pdcch_accepts_c,
      m->scanq_queued, m->scanq_processed, m->scanq_drop_full, m->scanq_drop_stale, m->scanq_drop_epoch, m->scanq_max_lag, m->pdcch_inline_drop_epoch,
      m->pdschq_queued, m->pdschq_decoded, m->pdschq_crc_ok, m->pdschq_drop_full, m->pdschq_drop_stale, m->pdschq_drop_epoch, m->pdschq_stale_after_decode, m->pdschq_max_lag, m->puschq_drop_epoch,
      m->gpu_fep_stale_pre, m->gpu_fep_stale_post, m->gpu_fep_selfcheck_fail, m->gpu_fep_selfcheck_skipped, m->gpu_fep_disabled,
      m->td_sib1_tdra_match_10, m->td_sib1_tdra_mismatch_10, m->td_sib1_tdra_none_10,
      m->td_sib1_tdra_match_11, m->td_sib1_tdra_mismatch_11, m->td_sib1_tdra_none_11,
      m->td_sib1_tdra_match_unk, m->td_sib1_tdra_mismatch_unk, m->td_sib1_tdra_none_unk,
      m->td_deftab_match_10, m->td_deftab_mismatch_10, m->td_deftab_match_11, m->td_deftab_mismatch_11, m->td_deftab_na,
      m->td_excl_restarts, m->td_excl_truncs, m->td_excl_restart_alarms,
      m->td_fb_promotions, m->td_fb_withdrawals, m->td_fb_failopens, m->td_fb_pruned_contexts, m->td_fb_untrusted_ctx, cb0,
      m->ldpc_ok, m->ldpc_seg_fail, m->ldpc_tb_fail, m->ldpc_zero_tb,
      m->ldpc_cuda_errors, m->ldpc_cuda_fallbacks, m->ldpc_cuda_poisoned,
      m->ldpc_cuda_disabled, m->ldpc_cuda_breaker_trips, m->ldpc_tb_cpu, m->ldpc_tb_cuda,
      m->srs_us,m->srs_jobs_dropped,m->srs_pregate_skip,m->srs_backoff_skip,
      m->srs_budget_skip,m->srs_idle_resets,
      m->pusch_try, m->pusch_crc_ok,
      m->obs_pushed, m->obs_written, m->obs_dropped,
      m->sense_cfr_drop, m->sense_rec_drop, m->sense_io_errors, m->sense_cfr_no_sink, sense_cost,
      m->prach_us, m->prach_drops, m->prach_skipped,
      m->csirs_candidates, m->csirs_confirmed, m->csirs_revoked, m->zp_exported, m->zp_revoked,
      m->csirs_search_us, m->csirs_idsweep_us, m->csirs_confirm_us, m->csirs_cfr_us,
      m->csirs_disc_batches, m->csirs_disc_footprints, m->csirs_disc_hyps, m->csirs_disc_winners,
      m->csirs_disc_ambiguous, m->csirs_disc_feed_drops, m->csirs_disc_gpu_batches,
      m->csirs_disc_cpu_batches, m->csirs_disc_batch_us,
      m->gpu_budget_used_pct,m->discovery_yields,m->rt_gpu_wait_max_us,m->rt_gpu_fallbacks,m->discovery_overruns,
      m->csirs_time_to_confirm_slots_last, m->csirs_time_to_confirm_slots_sum);
  if (w < 0 || (size_t)w >= n) return -1;
  const uint32_t count = m->csirs_confirm_resource_count < NR_PASSIVE_CSIRS_RESOURCE_METRICS_MAX
      ? m->csirs_confirm_resource_count : NR_PASSIVE_CSIRS_RESOURCE_METRICS_MAX;
  for (uint32_t i = 0; i < count; ++i) {
    const uint64_t k = m->csirs_confirm_resource_key[i];
    const int z = snprintf(buf + w, n - (size_t)w,
        "%s{\"zp\":%u,\"row\":%u,\"ports\":%u,\"density\":%u,\"period\":%u,\"offset\":%u,\"offset2\":%u,\"freq_domain\":%u,\"symb_l0\":%u,\"slots\":%" PRIu64 "}",
        i ? "," : "", (unsigned)(k & 1u), (unsigned)((k >> 1) & 31u),
        (unsigned)((k >> 6) & 63u), (unsigned)((k >> 12) & 15u),
        (unsigned)((k >> 16) & 1023u), (unsigned)((k >> 26) & 1023u),
        (unsigned)((k >> 36) & 1023u), (unsigned)((k >> 46) & 16383u), (unsigned)((k >> 60) & 15u),
        m->csirs_confirm_resource_slots[i]);
    if (z < 0 || (size_t)z >= n - (size_t)w) return -1;
    w += z;
  }
  if ((size_t)w + 1 >= n) return -1;
  buf[w++] = ']';
  const int sched = pdsch_schedule_json(m, buf + w, n - (size_t)w);
  if (sched < 0) return -1;
  w += sched;
  if (m->nid_capture_offered) {
    int z = snprintf(buf + w, n - (size_t)w,
                     ",\"nid_capture_offered\":%" PRIu64 ",\"nid_capture_taken\":%" PRIu64
                     ",\"nid_capture_skipped_empty\":%" PRIu64 ",\"nid_capture_noise_floor_db\":%.3f",
                     m->nid_capture_offered, m->nid_capture_taken, m->nid_capture_skipped_empty,
                     m->nid_capture_noise_floor_db);
    if (z < 0 || (size_t)z >= n - (size_t)w) return -1;
    w += z;
  }
  char discovery[96] = "";
  if (m->csirs_disc_readmit_skipped)
    snprintf(discovery, sizeof(discovery), ",\"csirs_disc_readmit_skipped\":%" PRIu64,
             m->csirs_disc_readmit_skipped);
  const int z = snprintf(buf + w, n - (size_t)w, "%s}", discovery);
  return (z < 0 || (size_t)z >= n - (size_t)w) ? -1 : w + z;
}
