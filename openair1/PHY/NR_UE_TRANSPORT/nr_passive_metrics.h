/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_PASSIVE_METRICS_H
#define NR_PASSIVE_METRICS_H
#ifndef __cplusplus
#include <stdatomic.h>
#endif
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define NR_PASSIVE_METRICS_SCHEMA 1
typedef struct {
  uint64_t t_mono_ns;       // CLOCK_MONOTONIC at snapshot
  int64_t abs_slot;         // producer absolute slot (nr_ue_diag_producer_absolute_slot), -1 unknown
  int pci;                  // -1 unknown
  const char *acq_state;    // nr_passive_acq_state_name(), never NULL ("UNKNOWN")
  uint64_t acq_transitions, acq_sync_losses, acq_pbch_locks, acq_sib1_decodes;
  uint64_t pdcch_occasions, pdcch_candidates, pdcch_accepts, pdcch_accepts_c;
  uint64_t scanq_queued, scanq_processed, scanq_drop_full, scanq_drop_stale, scanq_max_lag;
  uint64_t pdschq_queued, pdschq_decoded, pdschq_crc_ok, pdschq_drop_full, pdschq_drop_stale, pdschq_stale_after_decode, pdschq_max_lag;
  /* BC12a SIB1 common-TDRA census at first convergence of a context, per DCI format of the context (_10 / _11; _unk = format not
   * recorded). td_deftab_*: same against default table A (td_deftab_na = MIB dmrs-TypeA-Position unknown). Log/metrics only. */
  uint64_t td_sib1_tdra_match_10, td_sib1_tdra_mismatch_10, td_sib1_tdra_none_10;
  uint64_t td_sib1_tdra_match_11, td_sib1_tdra_mismatch_11, td_sib1_tdra_none_11;
  uint64_t td_sib1_tdra_match_unk, td_sib1_tdra_mismatch_unk, td_sib1_tdra_none_unk;
  uint64_t td_deftab_match_10, td_deftab_mismatch_10, td_deftab_match_11, td_deftab_mismatch_11, td_deftab_na;
  /* TD_EXCL census totals (sweep): evidence restarts caused by exclusions, tail truncations without a wipe, contexts that raised
   * TD_EXCL_RESTART_ALARM (restarts > distinct DCI phases). */
  uint64_t td_excl_restarts, td_excl_truncs, td_excl_restart_alarms;
  uint64_t ldpc_ok, ldpc_seg_fail, ldpc_tb_fail, ldpc_zero_tb;
  /* CUDA LDPC pool (K34): launch/CUDA errors, TBs sent to the CPU decoder, poisoned slots; 0 when libldpc_cuda is not loaded */
  uint64_t ldpc_cuda_errors, ldpc_cuda_fallbacks, ldpc_cuda_poisoned;
  uint64_t ldpc_cuda_disabled;        /* breaker state: 0 closed, 1 bypassed for a while, 2 permanently off */
  uint64_t ldpc_tb_cpu, ldpc_tb_cuda; /* TBs decoded by CPU layered / CUDA flooding (stratify evidence by decoder) */
  uint64_t pusch_try, pusch_crc_ok;
  uint64_t obs_pushed, obs_written, obs_dropped; // filled by Task A3, 0 until then
} nr_passive_metrics_t;
#ifndef __cplusplus
/* PCI of the locked cell, -1 before PBCH lock. Written by nr-ue.c (relaxed), read by collect (relaxed). */
extern _Atomic int nr_passive_metrics_pci;
#endif
/* Serialize to one JSON object (no newline). Returns bytes written (excl. NUL) or -1 if buf too small. */
int nr_passive_metrics_to_json(const nr_passive_metrics_t *m, char *buf, size_t n);
/* Fill a snapshot from all live getters (receiver side). */
void nr_passive_metrics_collect(nr_passive_metrics_t *m);
/* LOG_A "SENSING: ISAC_METRICS {json}" and, if env ISAC_METRICS_PATH is set, append the JSON line to that file
 * (opened once, line-buffered; this runs every 20 s on the summary path, not per slot). */
void nr_passive_metrics_emit(void);
#ifdef __cplusplus
}
#endif
#endif
