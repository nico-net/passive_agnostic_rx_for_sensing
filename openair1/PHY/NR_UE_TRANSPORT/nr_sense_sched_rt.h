/* Sensing-mode scheduler thread (plan Task S7): samples the shared resources every SENSE_DECIDE_NS, calls the pure decision (nr_sense_sched_decide) and applies it
 * with nr_sense_unit_set_enabled. Runs only in the SENSING mode, for N >= 1 cells; scan suspension is not part of it. The thread is SCHED_OTHER, has no fixed core and
 * a 2 MB stack (the decision keeps a ~250 KB candidate table on it). It takes no lock of the decode path. */
#ifndef NR_SENSE_SCHED_RT_H
#define NR_SENSE_SCHED_RT_H
#include "nr_sense_sched.h"

#ifdef __cplusplus
extern "C" {
#endif

void nr_sense_sched_start(void); /* no-op outside the SENSING mode and when already running */
void nr_sense_sched_stop(void);  /* joins the thread; safe when it was never started */

typedef struct {
  uint64_t decisions, enables, disables, bad_cap, deferrals; /* deferral = a decision period without a usable resource sample (the first, priming period; no pool) */
  int binding;                                              /* nr_res_id_t of the last decision */
} nr_sense_sched_stats_t;
void nr_sense_sched_stats(nr_sense_sched_stats_t *out);
const char *nr_sense_res_name(int res);
/* S9: T_cell of a cell after the last decision (0 before the first), its p95 continuity gap (ms) and whether it was in a gap; for ISAC_METRICS. */
int nr_sense_sched_floor_info(int cell, uint64_t *gap_p95_ms, bool *in_gap);

/* The wideband-grant resource of a cell as the last decision measured it: offered wide grants/s and mean CPU per tap (ns); 0 before the first measurement and after a cell reset. */
void nr_sense_sched_wbg_info(int cell, double *offered_hz, double *cost_ns);

/* One decision at time now_ns (the thread calls it every SENSE_DECIDE_NS; tests call it directly). */
void nr_sense_sched_step(uint64_t now_ns);

/* Resource sample of one decision. A test replaces the sampler (NULL restores the production one); a sampler returning false defers the decision. */
typedef struct {
  int n_cells; int pci[NR_SENSE_CELLS];
  nr_sense_res_t res[NR_RES_N];
  nr_sense_res_t cell_res[NR_SENSE_CELLS][NR_RES_N]; bool cell_res_set[NR_SENSE_CELLS]; /* per-cell queues and ring (nr_sense_sched_in_t.cell_res) */
  uint64_t ring_drops[NR_SENSE_CELLS]; /* records lost on the sense ring since the previous sample (informational; it also removes the ring's headroom) */
} nr_sense_sched_sample_t;
void nr_sense_sched_set_sampler(bool (*fn)(nr_sense_sched_sample_t *out, uint64_t now_ns));
void nr_sense_sched_reset(void); /* tests: forget the state, the counters and the sampler */

#ifdef __cplusplus
}
#endif
#endif
