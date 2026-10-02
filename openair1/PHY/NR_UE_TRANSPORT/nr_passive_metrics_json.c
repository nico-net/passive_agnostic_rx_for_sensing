/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_passive_metrics.h"
#include <inttypes.h>
#include <stdio.h>

int nr_passive_metrics_to_json(const nr_passive_metrics_t *m, char *buf, size_t n)
{
  const char *st = m->acq_state ? m->acq_state : "UNKNOWN";
  int w = snprintf(buf, n,
      "{\"schema\":%d,\"t_mono_ns\":%" PRIu64 ",\"abs_slot\":%" PRId64 ",\"pci\":%d,\"acq_state\":\"%s\","
      "\"acq_transitions\":%" PRIu64 ",\"acq_sync_losses\":%" PRIu64 ",\"acq_pbch_locks\":%" PRIu64 ",\"acq_sib1_decodes\":%" PRIu64 ","
      "\"pdcch_occasions\":%" PRIu64 ",\"pdcch_candidates\":%" PRIu64 ",\"pdcch_accepts\":%" PRIu64 ",\"pdcch_accepts_c\":%" PRIu64 ","
      "\"scanq_queued\":%" PRIu64 ",\"scanq_processed\":%" PRIu64 ",\"scanq_drop_full\":%" PRIu64 ",\"scanq_drop_stale\":%" PRIu64 ",\"scanq_drop_epoch\":%" PRIu64 ",\"scanq_max_lag\":%" PRIu64 ",\"pdcch_inline_drop_epoch\":%" PRIu64 ","
      "\"pdschq_queued\":%" PRIu64 ",\"pdschq_decoded\":%" PRIu64 ",\"pdschq_crc_ok\":%" PRIu64 ",\"pdschq_drop_full\":%" PRIu64 ",\"pdschq_drop_stale\":%" PRIu64 ",\"pdschq_drop_epoch\":%" PRIu64 ",\"pdschq_max_lag\":%" PRIu64 ",\"puschq_drop_epoch\":%" PRIu64 ","
      "\"ldpc_ok\":%" PRIu64 ",\"ldpc_seg_fail\":%" PRIu64 ",\"ldpc_tb_fail\":%" PRIu64 ",\"ldpc_zero_tb\":%" PRIu64 ","
      "\"pusch_try\":%" PRIu64 ",\"pusch_crc_ok\":%" PRIu64 ","
      "\"obs_pushed\":%" PRIu64 ",\"obs_written\":%" PRIu64 ",\"obs_dropped\":%" PRIu64 ","
      "\"csirs_candidates\":%" PRIu64 ",\"csirs_confirmed\":%" PRIu64 ",\"csirs_revoked\":%" PRIu64 ",\"zp_exported\":%" PRIu64 ",\"zp_revoked\":%" PRIu64 ","
      "\"csirs_search_us\":%" PRIu64 ",\"csirs_idsweep_us\":%" PRIu64 ",\"csirs_confirm_us\":%" PRIu64 ",\"csirs_cfr_us\":%" PRIu64 ","
      "\"csirs_time_to_confirm_slots_last\":%" PRIu64 ",\"csirs_time_to_confirm_slots_sum\":%" PRIu64 ","
      "\"csirs_time_to_confirm_resources\":[",
      NR_PASSIVE_METRICS_SCHEMA, m->t_mono_ns, m->abs_slot, m->pci, st,
      m->acq_transitions, m->acq_sync_losses, m->acq_pbch_locks, m->acq_sib1_decodes,
      m->pdcch_occasions, m->pdcch_candidates, m->pdcch_accepts, m->pdcch_accepts_c,
      m->scanq_queued, m->scanq_processed, m->scanq_drop_full, m->scanq_drop_stale, m->scanq_drop_epoch, m->scanq_max_lag, m->pdcch_inline_drop_epoch,
      m->pdschq_queued, m->pdschq_decoded, m->pdschq_crc_ok, m->pdschq_drop_full, m->pdschq_drop_stale, m->pdschq_drop_epoch, m->pdschq_max_lag, m->puschq_drop_epoch,
      m->ldpc_ok, m->ldpc_seg_fail, m->ldpc_tb_fail, m->ldpc_zero_tb,
      m->pusch_try, m->pusch_crc_ok,
      m->obs_pushed, m->obs_written, m->obs_dropped,
      m->csirs_candidates, m->csirs_confirmed, m->csirs_revoked, m->zp_exported, m->zp_revoked,
      m->csirs_search_us, m->csirs_idsweep_us, m->csirs_confirm_us, m->csirs_cfr_us,
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
  const int z = snprintf(buf + w, n - (size_t)w, "]}");
  return (z < 0 || (size_t)z >= n - (size_t)w) ? -1 : w + z;
}
