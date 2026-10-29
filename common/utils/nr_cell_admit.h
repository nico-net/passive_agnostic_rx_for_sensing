/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_CELL_ADMIT_H
#define NR_CELL_ADMIT_H
#include <stddef.h>
#include <stdint.h>
#include "nr_cell_desc.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Cell admission model (plan B.9, as revised by R4): which cells of an inventory the process serves, and what each gets of the scalable budgets.
 * Pure: no probe, no allocation, no global state. The host reports what it has (nr_host_caps_t); the model reuses the pure functions of nr_host_membudget.h
 * (calc / fit / fit_pct / hard reserve) and nr_gpu_membudget.h (calc / share_of / fit) and the per-module sizing expressions of the receiver, so with ONE cell it
 * reproduces today's sizing decisions exactly (test: NrCellAdmit.OneCellHostEqualsMembudgetReplay and OneCellGpuEqualsMembudget replay the real nr_host_membudget state API).
 *
 * Policy:
 *   - The primary cell (the first non-LOST cell of the inventory, normally cell 0) is always served, exactly as today: it follows nr_host_membudget's rules
 *     unchanged (a module below its floor is off, a committed total above the hard limit is a REFUSE: plan->refuse = 1, nothing served).
 *     No other cell can make the process refuse or exit.
 *   - The other cells are admitted one by one in priority order: MAINTAIN, then ACQUIRE, then NEW, inventory order inside a class; LOST cells are never admitted.
 *     A cell is served iff, with it added, (a) the count stays below NR_CELL_MAX and the core limit, (b) the channelizer budget holds, (c) the GPU modules of
 *     every served cell still reach their floors and the GPU total fits, (d) the host modules (fixed arenas + floors of the scalable ones) still reach their
 *     floors for every served cell and the committed host bytes stay under the hard limit. A module that was ready before the newcomer must stay ready.
 *     (e) B6: the core pool holds the decode demand of the served cells (nr_cell_pool_demand, measured on the running cells): 2 tokens per cell (a DECODE token and the scan reserve) and the
 *     probability that a lease has to wait (Erlang C of the pool tokens left after the channelizer's reserve, offered load = sum of the demands) stays at or below what
 *     the running cells measurably get now (pool_meas_wait, never below the model of the running state). Skipped when the host gives no pool table (pool_tokens[] zero).
 *     The first failing check in the order max, cores, pool, channelizer, gpu_mem, ram names the reason. A refused cell never changes the others.
 *   - The scalable shares (everything above the fixed arenas) are split among the served cells by nr_cell_desc_t.weight. */
typedef enum { NR_ADMIT_OK = 0, NR_ADMIT_CORES = 1, NR_ADMIT_RAM = 2, NR_ADMIT_GPU_MEM = 3, NR_ADMIT_CHANNELIZER = 4, NR_ADMIT_MAX = 5, NR_ADMIT_LOST = 6, NR_ADMIT_POOL = 7 } nr_admit_reason_t;
#define NR_ADMIT_PLAN_MAX 64 /* inventory entries the planner accepts */
#define NR_ADMIT_UNLIMITED_CELLS (-1)
#define NR_ADMIT_POOL_K 16 /* = NR_CELL_MAX: cells the pool table covers */
/* B6 pool model constants, sources in nr_cell_admit.c */
/* What the device reports. Every field comes from a probe or a derivation layer; none is a constant of a host. */
typedef struct {
  size_t mem_avail;          /* min(MemAvailable, cgroup headroom) as nr_host_probe_t.avail; SIZE_MAX = unknown (unlimited) */
  size_t host_shared_bytes;  /* host bytes the process already holds before any cell is planned (USRP ring reservation, pinned mirrors of the pass): nr_host_membudget_committed() */
  int gpu_tier;              /* nr_gpu_platform_model_t: 0 NONE (CPU), 1 SHARED, 2 DISCRETE */
  int unified;               /* the GPU shares the host RAM (SHARED tier): the GPU bytes planned here are deducted from the host budget */
  size_t gpu_free_bytes;     /* free device memory at probe (nr_gpu_platform_free_at_probe); 0 = unknown (unlimited) */
  size_t gpu_shared_bytes;   /* 0: use the model of the process-wide GPU modules; else the measured total of those modules (G1 ledger) */
  int max_cells;             /* cells the core layout can run (nr_cl_max_cells); NR_ADMIT_UNLIMITED_CELLS = no core limit */
  double chan_budget_msps;   /* channelizer capacity in output MS/s summed over the cells that need a frequency shift; < 0 = unlimited (zero-shift-only inventory) */
  int consumers, depth;      /* PDSCH queue shape (config): decode threads and ring depth */
  int csirs_enabled;         /* CSI-RS discovery on (ring + time-domain pool on the host, ring on the GPU) */
  int recovery_enabled;      /* async recovery IQ snapshot holders */
  int gw_enabled;            /* GrantWork LLR pool wanted (pdsch_decode >= 1) */
  /* B6 core pool vs demand. pool_tokens[k] = tokens of the core pool when k cells are served (the core layout built for k cells, nr_cl_build_nf(..).npool; index 0 unused);
   * 0 = unknown: the pool check is skipped for that count. chan_tokens = tokens the in-process channelizer reserves from the pool while >= 2 cells are served. */
  int pool_tokens[NR_ADMIT_POOL_K + 1];
  int chan_tokens;
  /* B6 measured pool state (filled from the running receiver by nr_pool_cell_demand / nr_pool_essential_wait; pool_meas_n = 0: nothing running or measured yet).
   * pool_meas_ess[i] = measured essential demand (tokens) of running cell i, pool_meas_grid[i] = its n_rb x n_rx x scs/30 (nr_cell_pool_grid),
   * pool_meas_wait = fraction of the essential leases that waited so far, < 0 unknown (set -1 by the filler; a zeroed struct reads as 0 = no wait seen). */
  int pool_meas_n;
  double pool_meas_ess[NR_ADMIT_POOL_K], pool_meas_grid[NR_ADMIT_POOL_K];
  double pool_meas_wait;
  /* B6 per-cell host bytes that the cell features added (CELLFEAT): the PRACH heap (0 = PRACH source not configured) and the sense-API rings (a sink is open when
   * sense_frames > 0: frame ring sense_frames x (48 + 3 re + 3 + 8 ant re) bytes, record ring sense_rec_slots x 4096; sense_max_re 46200 / frames 32 / slots 1024 are the defaults) */
  size_t prach_heap_bytes;
  int sense_frames, sense_max_re, sense_rec_slots;
} nr_host_caps_t;
/* Per-cell cost as a function of n_rb, n_rx and SCS (bytes). fixed = taken whatever the share; floor/full = the scalable modules at their floors / full size. */
typedef struct {
  int n_rb, scs_khz, n_rx;     /* effective values after the unknown-field rules */
  int shifted;                 /* needs the channelizer */
  double msps;                 /* output sample rate of the cell, MS/s */
  size_t host_fixed, host_floor, host_full;
  size_t gpu_fixed, gpu_floor, gpu_full;
} nr_cell_cost_t;
typedef struct {
  int served;                  /* 1 served, 0 unserved */
  int reason;                  /* nr_admit_reason_t (NR_ADMIT_OK when served) */
  int order;                   /* admission rank (0 = primary), -1 never considered */
  int snap_holders, csirs_slots, td_slots;  /* host module sizes; 0 = off (below its floor or disabled) */
  size_t gw_buffers;
  int gpu_csirs_slots;         /* device ring slots (0 = GPU discovery not ready) */
  size_t host_bytes, gpu_bytes; /* this cell's committed bytes */
} nr_admit_cell_t;
typedef struct {
  int n, n_served;
  int refuse;                  /* the primary cell does not fit the hard limit: the process refuses to start (today's HOST_MEM REFUSE) */
  int binding;                 /* reason of the first unserved cell, NR_ADMIT_OK when all served */
  size_t host_budget, host_hard, host_used;   /* B, hard limit, committed (shared + all served cells); SIZE_MAX = unlimited */
  size_t gpu_budget, gpu_used, gpu_shared;    /* GPU B, planned committed (served cells + shared modules, graph excluded), shared modules */
  int pool_checked;            /* B6: the pool check ran for the served set (caps carried a pool table) */
  int pool_tokens_eff;         /* pool tokens of the served set minus the channelizer's reserve */
  int pool_meas_n;             /* running cells the measurement covered */
  double pool_meas_load, pool_meas_wait;
  double pool_target, pool_unit, pool_model_now; /* wait target derived from the running cells, tokens per grid unit measured, Erlang C of the running state */
  double pool_load, pool_wait; /* offered decode load of the served set (tokens) and the modelled probability that a lease waits */
  int nid_window;              /* shared PDCCH nID context window (batches), 0 off */
  size_t gpu_graph_bytes;      /* what is left for the split-graph executables (capped), 0 when the floor does not fit */
  nr_admit_cell_t cell[NR_ADMIT_PLAN_MAX];
} nr_admit_plan_t;

int nr_cell_cost(const nr_cell_desc_t *c, const nr_host_caps_t *caps, nr_cell_cost_t *out);   /* 0, or -1 on NULL */
/* Plan: returns the number of served cells (>= 0), or -1 for NULL / n < 0 / n > NR_ADMIT_PLAN_MAX. n == 0: empty plan. */
int nr_cell_admit_plan(const nr_host_caps_t *caps, const nr_cell_desc_t *cells, int n, nr_admit_plan_t *plan);
const char *nr_admit_reason_name(int reason);
/* B6. Decode demand of one cell in pool tokens, derived from the running receiver: the essential token-time per second the running cells were measured to use (pool_meas_*)
 * per unit of n_rb x n_rx x scs/30, times this cell's own grid. 0 when nothing is measured (the caller then checks the structural floor only). Placeholders: 0. */
double nr_cell_pool_grid(const nr_cell_desc_t *c); /* n_rb x n_rx x scs/30; 0 for NULL / placeholder */
double nr_cell_pool_demand(const nr_host_caps_t *caps, const nr_cell_desc_t *c);
/* Erlang C: probability that a request finds all `servers` busy, for `load` erlangs. 1.0 when load >= servers; 0 for load <= 0. */
double nr_pool_wait_prob(int servers, double load);
/* Host bytes of the per-cell PRACH heap and sense rings as the plan books them (for the tests and the report) */
size_t nr_cell_sense_ring_bytes(int frames, int max_re, int rec_slots, int ant);
/* "MPCI_ADMIT_POOL cells=<n> tokens_eff=<..> load=<..> wait_pct=<..> target_pct=<..> meas_cells=<..> meas_load=<..> unit=<tokens per grid unit> meas_wait_pct=<..> model_now_pct=<..>"; 0 when the plan did not run the pool check */
int nr_admit_pool_format(const nr_admit_plan_t *plan, char *out, size_t n);
/* "MPCI_ADMIT cells=<served> of <n> reason=<binding|none> refuse=<0|1> host_budget_mb=.. host_used_mb=.. gpu_budget_mb=.. gpu_used_mb=.. nid_window=.. unserved=[i:reason ...]" */
int nr_admit_plan_format(const nr_admit_plan_t *plan, char *out, size_t n);
#ifdef __cplusplus
}
#endif
#endif
