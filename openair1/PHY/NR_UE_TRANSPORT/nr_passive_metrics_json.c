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
  return (w < 0 || (size_t)w >= n) ? -1 : w;
}

int nr_passive_metrics_to_json(const nr_passive_metrics_t *m, char *buf, size_t n)
{
  const char *st = m->acq_state ? m->acq_state : "UNKNOWN";
  char cb0[2048];
  if (cb0_json(m, cb0, sizeof(cb0)) < 0)
    return -1;
  const int w = snprintf(buf, n,
      "{\"schema\":%d,\"t_mono_ns\":%" PRIu64 ",\"abs_slot\":%" PRId64 ",\"pci\":%d,\"acq_state\":\"%s\","
      "\"acq_transitions\":%" PRIu64 ",\"acq_sync_losses\":%" PRIu64 ",\"acq_pbch_locks\":%" PRIu64 ",\"acq_sib1_decodes\":%" PRIu64 ","
      "\"pdcch_occasions\":%" PRIu64 ",\"pdcch_candidates\":%" PRIu64 ",\"pdcch_accepts\":%" PRIu64 ",\"pdcch_accepts_c\":%" PRIu64 ","
      "\"scanq_queued\":%" PRIu64 ",\"scanq_processed\":%" PRIu64 ",\"scanq_drop_full\":%" PRIu64 ",\"scanq_drop_stale\":%" PRIu64 ",\"scanq_max_lag\":%" PRIu64 ","
      "\"pdschq_queued\":%" PRIu64 ",\"pdschq_decoded\":%" PRIu64 ",\"pdschq_crc_ok\":%" PRIu64 ",\"pdschq_drop_full\":%" PRIu64 ",\"pdschq_drop_stale\":%" PRIu64 ",\"pdschq_stale_after_decode\":%" PRIu64 ",\"pdschq_max_lag\":%" PRIu64 ","
      "\"td_sib1_tdra_match_10\":%" PRIu64 ",\"td_sib1_tdra_mismatch_10\":%" PRIu64 ",\"td_sib1_tdra_none_10\":%" PRIu64 ","
      "\"td_sib1_tdra_match_11\":%" PRIu64 ",\"td_sib1_tdra_mismatch_11\":%" PRIu64 ",\"td_sib1_tdra_none_11\":%" PRIu64 ","
      "\"td_sib1_tdra_match_unk\":%" PRIu64 ",\"td_sib1_tdra_mismatch_unk\":%" PRIu64 ",\"td_sib1_tdra_none_unk\":%" PRIu64 ","
      "\"td_deftab_match_10\":%" PRIu64 ",\"td_deftab_mismatch_10\":%" PRIu64 ",\"td_deftab_match_11\":%" PRIu64 ",\"td_deftab_mismatch_11\":%" PRIu64 ",\"td_deftab_na\":%" PRIu64 ","
      "\"td_excl_restarts\":%" PRIu64 ",\"td_excl_truncs\":%" PRIu64 ",\"td_excl_restart_alarms\":%" PRIu64 ","
      "\"td_fb_promotions\":%" PRIu64 ",\"td_fb_withdrawals\":%" PRIu64 ",\"td_fb_failopens\":%" PRIu64 ",\"td_fb_pruned_contexts\":%" PRIu64 ",\"td_fb_untrusted_ctx\":%" PRIu64 "%s,"
      "\"ldpc_ok\":%" PRIu64 ",\"ldpc_seg_fail\":%" PRIu64 ",\"ldpc_tb_fail\":%" PRIu64 ",\"ldpc_zero_tb\":%" PRIu64 ","
      "\"ldpc_cuda_errors\":%" PRIu64 ",\"ldpc_cuda_fallbacks\":%" PRIu64 ",\"ldpc_cuda_poisoned\":%" PRIu64 ","
      "\"ldpc_cuda_disabled\":%" PRIu64 ",\"ldpc_cuda_breaker_trips\":%" PRIu64 ",\"ldpc_tb_cpu\":%" PRIu64 ",\"ldpc_tb_cuda\":%" PRIu64 ","
      "\"pusch_try\":%" PRIu64 ",\"pusch_crc_ok\":%" PRIu64 ","
      "\"obs_pushed\":%" PRIu64 ",\"obs_written\":%" PRIu64 ",\"obs_dropped\":%" PRIu64 "}",
      NR_PASSIVE_METRICS_SCHEMA, m->t_mono_ns, m->abs_slot, m->pci, st,
      m->acq_transitions, m->acq_sync_losses, m->acq_pbch_locks, m->acq_sib1_decodes,
      m->pdcch_occasions, m->pdcch_candidates, m->pdcch_accepts, m->pdcch_accepts_c,
      m->scanq_queued, m->scanq_processed, m->scanq_drop_full, m->scanq_drop_stale, m->scanq_max_lag,
      m->pdschq_queued, m->pdschq_decoded, m->pdschq_crc_ok, m->pdschq_drop_full, m->pdschq_drop_stale, m->pdschq_stale_after_decode, m->pdschq_max_lag,
      m->td_sib1_tdra_match_10, m->td_sib1_tdra_mismatch_10, m->td_sib1_tdra_none_10,
      m->td_sib1_tdra_match_11, m->td_sib1_tdra_mismatch_11, m->td_sib1_tdra_none_11,
      m->td_sib1_tdra_match_unk, m->td_sib1_tdra_mismatch_unk, m->td_sib1_tdra_none_unk,
      m->td_deftab_match_10, m->td_deftab_mismatch_10, m->td_deftab_match_11, m->td_deftab_mismatch_11, m->td_deftab_na,
      m->td_excl_restarts, m->td_excl_truncs, m->td_excl_restart_alarms,
      m->td_fb_promotions, m->td_fb_withdrawals, m->td_fb_failopens, m->td_fb_pruned_contexts, m->td_fb_untrusted_ctx, cb0,
      m->ldpc_ok, m->ldpc_seg_fail, m->ldpc_tb_fail, m->ldpc_zero_tb,
      m->ldpc_cuda_errors, m->ldpc_cuda_fallbacks, m->ldpc_cuda_poisoned,
      m->ldpc_cuda_disabled, m->ldpc_cuda_breaker_trips, m->ldpc_tb_cpu, m->ldpc_tb_cuda,
      m->pusch_try, m->pusch_crc_ok,
      m->obs_pushed, m->obs_written, m->obs_dropped);
  return (w < 0 || (size_t)w >= n) ? -1 : w;
}
