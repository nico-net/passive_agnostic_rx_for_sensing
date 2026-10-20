/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_PUSCH_UL_BGDISC_H
#define NR_PUSCH_UL_BGDISC_H
/* ISAC_UL_BGDISC=1 (default off): UL timing / DM-RS-identity discovery leaves the per-grant path. A grant of an RNTI without a usable
 * ul_hyp_cache entry is decoded with its default hypothesis and, in parallel, a PRIVATE COPY of its IQ snapshot goes to one low-priority
 * worker that runs the search and feeds ul_hyp_cache (nr_gpu_pass_ul.h). A copy (not a reference to the shared ULQ snapshot) means the samples
 * can never be overwritten under the worker; a job older than ISAC_UL_BGDISC_MAX_AGE_MS (default 100) or of an older config epoch is dropped
 * before it runs (bg_expired). The search itself is the caller's runner (nr_pusch_passive_decode.c). */
#include <stdint.h>
#include "PHY/TOOLS/tools_defs.h"
#include "nr_pdcch_blind_monitor.h"

typedef struct {
  uint16_t rnti;
  uint8_t slot, nant, flags;     /* flags: 1 = timing search, 2 = DM-RS id search */
  uint32_t epoch;                /* nr_cfg_epoch_dedicated_current() at submit */
  int64_t abs_slot;
  int32_t ta_offset;             /* nominal timing advance of the grant */
  double fo_hz;
  uint32_t origin, length;       /* the snapshot window (nr_pusch_passive_snapshot_copy convention) */
  const c16_t *iq[4];            /* submit: the caller's snapshot; in the runner: the worker's private copy */
  nr_pdcch_blind_ul_result_t grant;
  uint64_t submit_ns;
} nr_ul_bgdisc_job_t;

/* returns 0 = done, 1 = expired on its own terms (e.g. epoch changed): counted bg_expired */
typedef int (*nr_ul_bgdisc_run_fn)(const nr_ul_bgdisc_job_t *j);

typedef struct { uint64_t nohyp, submitted, dup, full, expired, done; } nr_ul_bgdisc_stats_t;

int nr_ul_bgdisc_enabled(void); /* ISAC_UL_BGDISC=1 */
/* depth jobs; the worker thread is started only when run != NULL (tests start it later to hold jobs back). 0 / -1 */
int nr_ul_bgdisc_start(int depth, nr_ul_bgdisc_run_fn run);
void nr_ul_bgdisc_stop(void);
/* Counts ul_nohyp, then queues the job (copying its IQ). Never searches, never blocks beyond a short mutex. 1 = queued, 0 = dropped (duplicate RNTI in flight, queue full, no memory). */
int nr_ul_bgdisc_submit(const nr_ul_bgdisc_job_t *j);
void nr_ul_bgdisc_stats(nr_ul_bgdisc_stats_t *s);
/* Latch rule for a CRC-passing hypothesis (key != 0) of an RNTI whose cache holds cur_key (0 = none). 1 = store it now: the same
 * hypothesis as the cached one, a second pass of it in a DIFFERENT slot, or another RNTI with the same key pending. 0 = remembered, not stored. */
int nr_ul_bgdisc_latch(uint16_t rnti, uint32_t key, int64_t abs_slot, uint32_t cur_key);
#endif
