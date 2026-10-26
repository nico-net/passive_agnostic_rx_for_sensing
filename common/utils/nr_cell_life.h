/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_CELL_LIFE_H
#define NR_CELL_LIFE_H
/* Cell lifecycle of the receiver (multi-PCI plan task A8): inventory (D1) -> admission (B3, through the cell manager) -> per-cell
 * pre-allocation -> RX fence -> cell count -> per-cell start, then at run time retire / re-activate / retune from inventory updates.
 *
 * The receiver main (executables/nr-uesoftmodem.c) supplies the hooks; this module owns the ORDER and the BINDING, which is what the unit
 * test (common/utils/tests/test_nr_cell_life.cpp) checks:
 *   - every hook of cell k runs on the calling thread bound to cell k (nr_cell_bind), the previous binding is restored afterwards;
 *   - every prealloc runs before the fence, the fence runs once, nr_cell_set_count(N) runs after the fence and before the first start;
 *   - a cell beyond N_max is refused by the manager and gets no hook; a lost / removed cell is retired after the manager's hysteresis;
 *     an id that comes back into a slot prepared before the fence is started again without any allocation; a slot never prepared before
 *     the fence is not started (logged), because nothing may be allocated after the fence.
 * The primary cell is the cell the process was STARTED on (its command-line tuning): it is always cell 0 (admission rule of B3: the primary is
 * always served, exactly as today). Inventory entries that describe the same cell (same centre within the tolerance) are merged into it.
 * No thread, no clock (injected now_ns), no environment variable, no option. Not thread safe: one control thread owns it. */
#include <stdint.h>
#include "nr_cell.h"
#include "nr_cell_mgr.h"
#ifdef __cplusplus
extern "C" {
#endif
#define NR_CELL_LIFE_PRIMARY_ID "primary" /* id of the started cell when the inventory does not list it */

typedef struct {
  /* All hooks may be NULL. They run with the calling thread bound to `cell`. */
  int (*prealloc)(void *ctx, int cell, const nr_mgr_slot_t *slot); /* before the fence; != 0: the cell is not started */
  void (*fence)(void *ctx);                                         /* once, after every prealloc (the receiver: nr_cell_alloc_fence) */
  int (*start)(void *ctx, int cell, const nr_mgr_slot_t *slot, int reactivation); /* after the fence and nr_cell_set_count */
  void (*retire)(void *ctx, int cell, const nr_mgr_slot_t *slot);
  void (*retune)(void *ctx, int cell, const nr_cell_mgr_event_t *ev);
  void (*log)(void *ctx, const char *line);                         /* NULL: printf */
  void *ctx;
} nr_cell_life_ops_t;

typedef struct {
  nr_cell_mgr_t mgr;
  nr_cell_life_ops_t ops;
  char primary_id[NR_INV_ID_MAX];
  nr_cell_desc_t primary;
  double match_tol_hz;
  int n;                       /* N_max of the plan */
  int activated;
  int phase;                   /* 0 planned, 1 admitted, 2 prepared (fence done), 3 started */
  int prepared[NR_CELL_MAX];   /* prealloc done before the fence */
  int started[NR_CELL_MAX];
  /* activation collects the ADD events of the first inventory here (hooks run after the manager returns) */
  int pending_add[NR_CELL_MAX], n_pending;
  int collecting;
} nr_cell_life_t;

/* Pure: the inventory as the manager must see it. out->cell[0] is the primary cell (state MAINTAIN, so the manager puts it in slot 0): the
 * inventory entry whose centre_hz is within tol_hz of primary->centre_hz (its id and fields are kept; pci/cfo/n_rb unknown in the primary
 * descriptor are taken from it), else a synthetic entry NR_CELL_LIFE_PRIMARY_ID from *primary. The other entries follow in file order.
 * Returns out->n, or -1 on NULL. *matched (may be NULL) = index of the merged entry in `in`, -1 when synthetic. */
int nr_cell_life_order(const nr_cell_desc_t *primary, const nr_cell_inventory_t *in, double tol_hz, nr_cell_inventory_t *out, int *matched);

/* Once, before the fence. `inv` may be NULL (no inventory). Returns N_max (>= 1) or -1 (the plan refuses the primary / bad arguments). */
int nr_cell_life_plan(nr_cell_life_t *L, const nr_cell_mgr_cfg_t *cfg, const nr_host_caps_t *caps, const nr_cell_life_ops_t *ops,
                      const nr_cell_desc_t *primary, const nr_cell_inventory_t *inv, double tol_hz);
/* Activation in three steps (the receiver runs them at three points of its start-up), each once and in this order:
 *   admit:   ADD events of the first inventory -> the slot of every served cell is known (nr_cell_mgr_slot); returns the number of cells added.
 *   prepare: prealloc hook of each added cell (bound) -> fence hook -> nr_cell_set_count(highest prepared slot + 1); returns the prepared count.
 *   start:   start hook of each prepared cell (bound); returns the started count.
 * activate = the three in a row. Each returns -1 when called out of order. */
int nr_cell_life_admit(nr_cell_life_t *L, const nr_cell_inventory_t *inv, int64_t now_ns);
int nr_cell_life_prepare(nr_cell_life_t *L);
int nr_cell_life_start(nr_cell_life_t *L);
int nr_cell_life_activate(nr_cell_life_t *L, const nr_cell_inventory_t *inv, int64_t now_ns);
/* slot ids of the admitted cells in admission order (valid after admit) */
static inline int nr_cell_life_admitted(const nr_cell_life_t *L, int i) { return i >= 0 && i < L->n_pending ? L->pending_add[i] : -1; }
/* Run time: a new accepted inventory (or NULL: only the hysteresis tick). Returns the number of manager events. */
int nr_cell_life_poll(nr_cell_life_t *L, const nr_cell_inventory_t *inv, int64_t now_ns);
int nr_cell_life_started(const nr_cell_life_t *L, int cell);
#ifdef __cplusplus
}
#endif
#endif
