/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_CELL_MGR_H
#define NR_CELL_MGR_H
/* Cell manager (plan B.7 / B.9, task D1): maps the inventory ids of the blind front end onto the pre-sized cell slots of the process.
 *
 *   nr_cell_mgr_plan()   ONCE, before the RX fence: nr_cell_admit_plan -> N_max, one "MPCI_ADMIT ..." line, the per-slot state objects are
 *                        allocated here (nr_cell_heap_get) so that nothing is ever allocated afterwards.
 *   nr_cell_mgr_apply()  for every accepted inventory: add (new id) -> activate a free slot; update (same id, new centre / ssb / CFO)
 *                        -> RETUNE event for that slot; id gone or state "lost" -> retire after the hysteresis; more cells than slots
 *                        -> REFUSE with the admission reason. Activation allocates nothing (no malloc, no nr_cell_heap_get miss).
 *   nr_cell_mgr_tick()   advances the retire hysteresis with the injected clock.
 *   nr_cell_mgr_pass2_from_log()  the receiver's own MIB/sync log lines -> --ssb / -C refinement -> RETUNE event (what
 *                        `live_chain.sh pass2` does by hand).
 * The manager only decides and reports; it starts no thread and moves no channelizer: it emits events through a callback. The channelizer
 * (C1) and the slot threads (A8) consume them. nr_cell_set_count(N) is NOT called here: A8 calls it with nr_cell_mgr_nmax()/active count
 * when the threads start. The clock is injected (now_ns arguments); the manager has no clock, no environment variable and no option.
 * Not thread safe: one control thread owns it. Logs: "MPCI_ADMIT ..." (once) and "MPCI_CELL id=.. slot=.. event=add|update|retire|refuse reason=..". */
#include <stddef.h>
#include <stdint.h>
#include "nr_cell.h"
#include "nr_cell_admit.h"
#include "nr_cell_inventory.h"
#ifdef __cplusplus
extern "C" {
#endif
#define NR_MGR_RETIRE_HYST_DEFAULT_NS 10000000000LL /* policy default when cfg.retire_hysteresis_ns == 0: 10 s of "lost/absent" before a slot is freed */
#define NR_MGR_SSB_FIRST 1171                        /* pass-1 --ssb when k_SSB is unknown (CARRIERS.md rule, as tests/passive_rx/autofront) */

typedef enum { NR_MGR_EV_ADD = 0, NR_MGR_EV_RETUNE = 1, NR_MGR_EV_RETIRE = 2, NR_MGR_EV_REFUSE = 3 } nr_mgr_event_kind_t;
typedef struct {
  int kind;                    /* nr_mgr_event_kind_t */
  int slot;                    /* cell index (-1 for REFUSE) */
  const char *id;              /* inventory id (valid during the callback only) */
  nr_cell_desc_t desc;         /* the cell as now applied (ADD / RETUNE); the last applied one for RETIRE; the requested one for REFUSE */
  double prev_centre_hz, prev_cfo_hz; /* RETUNE: before */
  int ssb;                     /* receiver --ssb in force after the event (NR_MGR_SSB_FIRST until pass 2 knows k_SSB) */
  int prev_ssb;                /* RETUNE: before */
  int reason;                  /* REFUSE: nr_admit_reason_t */
  const char *cause;           /* "new" "inventory" "pass2" | RETIRE: "lost" "removed" | REFUSE: nr_admit_reason_name */
  void *slot_state;            /* the pre-allocated per-slot object (ADD, RETUNE, RETIRE) */
} nr_cell_mgr_event_t;
typedef void (*nr_cell_mgr_cb_t)(void *ctx, const nr_cell_mgr_event_t *ev);

typedef struct {
  int active;                  /* slot holds a cell */
  char id[NR_INV_ID_MAX];
  nr_cell_desc_t desc;         /* applied */
  nr_cell_desc_t inv_desc;     /* as last reported by the inventory (change detection; pass 2 does not touch it) */
  int ssb;                     /* --ssb in force */
  int pending_retire;          /* 0, or 1 lost / 2 removed */
  int64_t pending_since_ns;
  void *state;                 /* pre-allocated at plan time; zeroed at activation */
} nr_mgr_slot_t;
typedef struct {
  char id[NR_INV_ID_MAX];
  int reason;
} nr_mgr_refused_t;

typedef struct {
  nr_cell_mgr_cb_t cb;         /* may be NULL */
  void *ctx;
  int64_t retire_hysteresis_ns; /* 0: NR_MGR_RETIRE_HYST_DEFAULT_NS */
  size_t slot_state_bytes;     /* size of the per-slot object allocated before the fence (0: none) */
  void (*log)(void *ctx, const char *line); /* NULL: printf to stdout */
  void *log_ctx;
} nr_cell_mgr_cfg_t;

typedef struct {
  nr_cell_mgr_cfg_t cfg;
  int planned, nmax, n_active;
  int full_reason;             /* reason a cell beyond nmax is refused with */
  nr_admit_plan_t plan;
  nr_mgr_slot_t slot[NR_CELL_MAX];
  nr_mgr_refused_t refused[NR_ADMIT_PLAN_MAX];
  int n_refused;
} nr_cell_mgr_t;

/* Call once, before nr_cell_alloc_fence(). `wanted` = the cells the process would like to serve (descriptors of the first inventory, or a
 * representative set); N_max = cells served by nr_cell_admit_plan. Returns N_max (>= 1), or -1 if the plan refuses (primary cell does
 * not fit) / bad arguments / called twice / called after the fence. A cell beyond N_max is refused with the reason of the plan's binding
 * limit (NR_ADMIT_MAX if every wanted cell was served and the inventory later grows). */
int nr_cell_mgr_plan(nr_cell_mgr_t *m, const nr_cell_mgr_cfg_t *cfg, const nr_host_caps_t *caps, const nr_cell_desc_t *wanted, int n_wanted);
/* Reconcile with an accepted inventory. Returns the number of events emitted. Adds are taken in admission priority (MAINTAIN, ACQUIRE, NEW; file order inside a class). */
int nr_cell_mgr_apply(nr_cell_mgr_t *m, const nr_cell_inventory_t *inv, int64_t now_ns);
/* Retire cells whose lost/removed state outlived the hysteresis. Returns the number of RETIRE events. */
int nr_cell_mgr_tick(nr_cell_mgr_t *m, int64_t now_ns);
/* Pass 2 from a stored receiver log excerpt of cell `id`: needs "MIB fields sfn=<n> k_ssb=<k>" and, if present, "Initial sync successful, PCI: <p>"
 * (p must equal the cell's pci when both are known). --ssb = 1164 + k/2; -C = ssb_hz + (1182 - ssb) * 30 kHz (autofront.py tune()).
 * Returns 1 and emits RETUNE (cause "pass2") if (-C, --ssb) changed, 0 if pass 2 equals what is in force, -1 unknown id / no k_SSB line /
 * PCI mismatch / cell has no ssb_hz or gscn. *ssb_out and *centre_out (may be NULL) receive the refined values whenever the result is >= 0. */
int nr_cell_mgr_pass2_from_log(nr_cell_mgr_t *m, const char *id, const char *log, size_t len, int64_t now_ns, int *ssb_out, double *centre_out);

int nr_cell_mgr_nmax(const nr_cell_mgr_t *m);
int nr_cell_mgr_active(const nr_cell_mgr_t *m);          /* cells holding a slot (incl. those pending retire) */
int nr_cell_mgr_find(const nr_cell_mgr_t *m, const char *id); /* slot or -1 */
const nr_mgr_slot_t *nr_cell_mgr_slot(const nr_cell_mgr_t *m, int slot); /* NULL if out of range */
#ifdef __cplusplus
}
#endif
#endif
