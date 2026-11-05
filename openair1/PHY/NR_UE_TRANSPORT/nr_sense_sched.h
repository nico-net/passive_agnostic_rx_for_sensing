/* Sensing-mode scheduler constants (spec 2026-11-03-two-modes-design.md section 5). Compile-time only. */
#ifndef NR_SENSE_SCHED_H
#define NR_SENSE_SCHED_H
#include <stdbool.h>
#include <stdint.h>
#include "nr_sense_unit.h"

#define SENSE_BUCKET_NS 50000000ull      /* cost bucket */
#define SENSE_DECIDE_NS 4000000000ull    /* decision period */
#define SENSE_CONFIRM 2                  /* consecutive agreeing windows before a change */
#define SENSE_WIN_DECISIONS 4            /* cost window, in decisions */
#define SENSE_HEADROOM 0.40              /* share of each resource a unit may use at its p99 */
#define SENSE_HYST 0.20                  /* hysteresis margin */
#define SENSE_MAX_ENABLES 2              /* units enabled per decision */
#define SENSE_ELIG_VALID_MIN 0.2f        /* a unit is eligible only above this valid ratio (and with wb_rate_hz > 0) */
#define SENSE_AGE_CAP 2.0                /* fairness multiplier cap ... */
#define SENSE_AGE_FULL_NS 10000000000ull /* ... reached at this age since the last valid wideband CFR */
/* buckets in the cost window: 4 decisions x 4 s / 50 ms = 320 */
#define SENSE_WIN_BUCKETS ((SENSE_WIN_DECISIONS * SENSE_DECIDE_NS) / SENSE_BUCKET_NS)

#ifdef __cplusplus
extern "C" {
#endif

#define SENSE_COST_MEM_DECAY 0.9f       /* a disabled unit whose cost history has left the window is charged its last measured cost, x0.9 per decision (half after ~7 decisions = ~26 s), then it is probed again */
#define SENSE_REPROBE_BASE 4             /* a unit shed by the streak (resources / eligibility) is not enabled again for 4 decisions, doubling at each consecutive shed up to ... */
#define SENSE_REPROBE_MAX 64             /* ... 64 decisions (~4 min); the count restarts after SENSE_REPROBE_OK decisions enabled and wanted */
#define SENSE_REPROBE_OK 8
#define SENSE_VIOL_HOLDOFF 2 /* decisions a unit removed for a violation stays off after the removal decision */
#define NR_SENSE_CELLS 8
/* S9 adaptive continuity floor T_cell (spec addendum 2026-11-05, coordinator ruling fix round 1). Levels: sense_min_prb, 96, 64, 48 (those below sense_min_prb and above the floor), then sense_min_prb_floor.
 * T_cell = the highest level whose p95 gap between DCI arrivals of grants of >= that many PRBs is within the continuity target; none meets it: the floor; no arrival of the cell at all: sense_min_prb.
 * A move needs SENSE_CONFIRM agreeing decisions; a move to a higher level also needs that level to beat the target by SENSE_HYST. */
#define SENSE_FLOOR_MAX_STEPS 5
#define SENSE_GAP_PREC 2.0        /* a cell in a continuity gap ranks x2 in the shared-pool admission (rank only; the unit values stay true) */
int nr_sense_floor_steps(int min_prb, int floor_prb, int *steps); /* fills steps[] (descending), returns their number (1..5) */
#define NR_SENSE_SLOTS 64 /* RNTI slots per cell in the state: slot 0 is the per-cell SRS unit (rnti 0), 1..63 hold the other RNTIs */

/* One shared resource, in ns per 50 ms bucket (the unit of cpu_p99_ns, so no further scaling). For NR_RES_QUEUE_*: cap_ns = the queue's deadline budget and
 * base_p99_ns = its baseline wait p99, both in ns. cap_ns <= 0 means the resource does not exist on this host (GPU on x86) and is not constrained;
 * for every other resource cap_ns <= 0 (or NaN) is invalid input: no unit using it is admitted and out->bad_cap counts it. */
typedef struct { float cap_ns, base_p99_ns; } nr_sense_res_t;
typedef enum { NR_RES_POOL = 0, NR_RES_QUEUE_DL, NR_RES_QUEUE_UL, NR_RES_GPU, NR_RES_SENSE_RING, NR_RES_DA, NR_RES_N } nr_res_id_t;
/* Resources a unit uses (demand): POOL by every unit (cpu_p99_ns, the CPU of pool-leased threads), SENSE_RING by every unit (ring_p99_ns: its writer cost, record formatting + quality),
 * DA (per cell: the daBg workers, cap = workers x one bucket) by PDSCH_DATA (da_p99_ns: its daBg CPU),
 * GPU by PDSCH_DATA (cpu_p99_ns as the proxy of the launch/wait time: the GPU time per unit is not in the view), QUEUE_DL by PDSCH_DATA and QUEUE_UL by SRS
 * (wait_p99_ns; waits do not add, the queue is charged the maximum over the admitted units). DM-RS units are inline (no queue). */

typedef struct {
  int n_cells; int pci[NR_SENSE_CELLS];
  const nr_sense_unit_view_t *units[NR_SENSE_CELLS]; int n_units[NR_SENSE_CELLS]; /* per cell, from nr_sense_unit_snapshot (max 256 per cell, 2048 in all are considered) */
  nr_sense_res_t res[NR_RES_N];
  /* per-cell override of the per-cell resources (NR_RES_QUEUE_DL, NR_RES_QUEUE_UL, NR_RES_SENSE_RING, NR_RES_DA): used for cell c when cell_res_set[c], else res[] applies to every cell.
   * POOL and GPU are shared by all cells and always come from res[]. */
  nr_sense_res_t cell_res[NR_SENSE_CELLS][NR_RES_N]; bool cell_res_set[NR_SENSE_CELLS];
  int min_prb; uint64_t now_ns;
  /* S9: per cell, per level of nr_sense_floor_steps(min_prb, floor_prb): the p95 gap and the rate of the cell's DCI arrivals of >= that many PRBs over the decision window
   * (nr_sense_unit_arrival_gaps); arrivals[c] = the cell's arrivals of any size in the window (0 = no evidence: T_cell = sense_min_prb). gap_valid[c] false (or cont_target_ns 0): the cell's floor does not move. */
  nr_sense_gapstat_t gap[NR_SENSE_CELLS][SENSE_FLOOR_MAX_STEPS]; uint32_t arrivals[NR_SENSE_CELLS]; bool gap_valid[NR_SENSE_CELLS];
  uint64_t cont_target_ns; int floor_prb;
} nr_sense_sched_in_t;

/* Kept between decisions by the caller (zero-initialised at start). st->enabled is the decision: the caller applies it with nr_sense_unit_set_enabled
 * (cell, st->rnti[cell][slot], src) where it differs from the unit's view. */
typedef struct {
  uint8_t fl_idx[NR_SENSE_CELLS], fl_pend[NR_SENSE_CELLS], fl_cnt[NR_SENSE_CELLS], fl_warm[NR_SENSE_CELLS]; /* fl_warm: an earlier decision saw arrivals of the cell, so this decision's window is full */ /* S9: level index of T_cell, the level the last decisions asked for, how many decisions in a row asked for it */
  uint8_t streak[NR_SENSE_CELLS][NR_SENSE_SLOTS][NR_UNIT_SRC_N];
  bool enabled[NR_SENSE_CELLS][NR_SENSE_SLOTS][NR_UNIT_SRC_N];
  uint16_t rnti[NR_SENSE_CELLS][NR_SENSE_SLOTS];
  uint64_t last_pci_valid_wb_ns[NR_SENSE_CELLS];
  uint64_t pci_elig_since[NR_SENSE_CELLS]; bool pci_was_elig[NR_SENSE_CELLS], pci_seen[NR_SENSE_CELLS]; /* the age of a PCI/unit does not accumulate while it is ineligible */
  uint64_t elig_since[NR_SENSE_CELLS][NR_SENSE_SLOTS][NR_UNIT_SRC_N]; bool was_elig[NR_SENSE_CELLS][NR_SENSE_SLOTS][NR_UNIT_SRC_N];
  float mem[NR_SENSE_CELLS][NR_SENSE_SLOTS][NR_UNIT_SRC_N][4]; /* cpu, wait, ring, da */ /* last measured p99 demand while enabled, decayed while the unit is off (SENSE_COST_MEM_DECAY per decision) */
  uint8_t shed_n[NR_SENSE_CELLS][NR_SENSE_SLOTS][NR_UNIT_SRC_N], run_n[NR_SENSE_CELLS][NR_SENSE_SLOTS][NR_UNIT_SRC_N]; /* consecutive sheds (re-probe back-off) / decisions enabled and wanted */
  uint8_t holdoff[NR_SENSE_CELLS][NR_SENSE_SLOTS][NR_UNIT_SRC_N]; /* decisions a violation-removed unit must still wait before it can be enabled */
  bool known[NR_SENSE_CELLS][NR_SENSE_SLOTS][NR_UNIT_SRC_N];     /* below: the violation baseline of the previous decision (re-baselined when the row generation changes) */
  uint32_t gen[NR_SENSE_CELLS][NR_SENSE_SLOTS][NR_UNIT_SRC_N], viol[NR_SENSE_CELLS][NR_SENSE_SLOTS][NR_UNIT_SRC_N];
} nr_sense_sched_state_t;

#define NR_SENSE_ADMIT_LOG 16
typedef struct { int cell; uint16_t rnti; nr_unit_src_t src; float val; } nr_sense_admit_t;
typedef struct {
  int floor_prb[NR_SENSE_CELLS]; bool in_gap[NR_SENSE_CELLS]; uint64_t gap_p95_ns[NR_SENSE_CELLS]; /* S9: T_cell after this decision, whether the cell is in a continuity gap at it, its p95 gap */
  int enables, disables, bad_cap; nr_res_id_t binding; float value_last_admitted; /* value of the lowest-valued admitted unit */
  int n_admitted; nr_sense_admit_t admitted[NR_SENSE_ADMIT_LOG]; /* the admitted set in rank order (n_admitted counts all, the list holds the first NR_SENSE_ADMIT_LOG) */
} nr_sense_sched_out_t;

void nr_sense_sched_decide(const nr_sense_sched_in_t *in, nr_sense_sched_state_t *st, nr_sense_sched_out_t *out);

/* S9: the current continuity floor T_cell of a cell (PRBs), published by the scheduler after each decision; sense_min_prb until one exists. Lock-free, any thread. */
int nr_sense_cell_floor(int cell);
void nr_sense_cell_floor_publish(int cell, int prbs);
void nr_sense_cell_floor_reset(void);
/* S9: a cell changed (re-sync, unit reset): its T_cell goes back to sense_min_prb at once; the scheduler restarts its level/confirm/warm-up state at the next decision (take_cell_resets: bitmask of cells, clears it). Any thread. */
void nr_sense_sched_cell_reset(int cell);
uint32_t nr_sense_sched_take_cell_resets(void);

/* exposed for tests */
bool nr_sense_unit_ratio_known(const nr_sense_unit_view_t *v, uint64_t now_ns); /* n_cfr > 0 and the latest CFR is not older than one cost window (SENSE_WIN_DECISIONS x SENSE_DECIDE_NS) */
bool nr_sense_unit_eligible(const nr_sense_unit_view_t *v, uint64_t now_ns);   /* wb_rate_hz > 0 over the window, and valid_ratio >= SENSE_ELIG_VALID_MIN when the ratio is known (unknown passes) */
double nr_sense_age_boost(bool eligible, uint64_t last_valid_ns, uint64_t now_ns); /* 1 + (CAP-1) x min(1, age / FULL); exactly 1 when not eligible */

#ifdef __cplusplus
}
#endif
#endif
