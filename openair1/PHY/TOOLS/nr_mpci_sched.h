/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_MPCI_SCHED_H
#define NR_MPCI_SCHED_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Cross-process GPU discovery arbiter for one receiver process per cell (MPCI M3).
 * Lock-free atomics in POSIX shm; default OFF (ISAC_MPCI_SCHED=<segment name> enables it, only consulted by
 * nr_gpu_budget.c when ISAC_GPU_BUDGET=1). RT work is published, never throttled. */
#define NR_MPCI_MAX_CELLS 16
#define NR_MPCI_SLICE_US 5000 /* same hard slice ceiling reserved by nr_gpu_budget.c */
typedef struct nr_mpci nr_mpci_t;
typedef struct {
  uint64_t scanq_full, scanq_stale, scanq_epoch, pdschq_full, pdschq_stale, pdschq_epoch, pdschq_narrow;
} nr_mpci_drops_t;
typedef struct {
  int slot, pid, share, acquire, rt_inflight;
  char tag[24];
  uint64_t window_used_us, total_used_us, yields, rt_jobs, heartbeat_age_us;
  nr_mpci_drops_t drops;
} nr_mpci_cell_stat_t;
/* Explicit attach (tests, tools). disc_pct: global discovery budget % of wall time, honoured only by the segment creator.
 * share: relative weight. NULL on failure. */
nr_mpci_t *nr_mpci_attach(const char *name, const char *tag, unsigned share, unsigned disc_pct);
/* Read-only attach for the stat tool (claims no slot). */
nr_mpci_t *nr_mpci_attach_ro(const char *name);
void nr_mpci_detach(nr_mpci_t *);
/* Process-wide handle from ISAC_MPCI_SCHED / ISAC_MPCI_GPU_SHARE_PCT / ISAC_MPCI_GPU_DISCOVERY_PCT (default: the
 * local ISAC_GPU_DISCOVERY_BUDGET_PCT, read from the env; def_pct is ignored). NULL when unset: every caller treats NULL as "arbiter off". */
nr_mpci_t *nr_mpci_default(unsigned def_pct);
/* 1 admit one discovery slice, 0 yield. Counts demand and yields. acquire: ACQUIRE-mode cell (discovery_prefer). */
int nr_mpci_discovery_admit(nr_mpci_t *, uint64_t now_us, int acquire);
void nr_mpci_discovery_charge(nr_mpci_t *, uint64_t now_us, uint64_t gpu_us);
void nr_mpci_rt_begin(nr_mpci_t *, uint64_t now_us);
void nr_mpci_rt_end(nr_mpci_t *);
void nr_mpci_publish_drops(nr_mpci_t *, const nr_mpci_drops_t *);
/* Fills out[] with live cells (frees dead slots on the way); returns count. */
int nr_mpci_snapshot(nr_mpci_t *, uint64_t now_us, nr_mpci_cell_stat_t *out, int max);
#ifdef __cplusplus
}
#endif
#endif
