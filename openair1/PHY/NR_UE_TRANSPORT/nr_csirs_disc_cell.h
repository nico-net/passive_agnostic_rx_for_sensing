/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Per-cell entry points of the CSI-RS discovery host side (mpci A7, plan 2026-10-23-mpci-main-plan.md B.6 item 4).
 *
 * One discovery instance per cell: the worker ring, the time-domain slot queue, the footprint history and the pipeline state live in the cell's slot of the
 * NR_CELL_STATIC objects of nr_csirs_disc{,_feed,_td,_fp}.c. Their big buffers are heap blocks sized at start from the CELL's N_RB and RX count and from the host-RAM
 * budget (nr_host_membudget), never from a compile-time maximum, so an idle cell costs a few KB.
 *
 * Pre-allocation: nr_csirs_disc_cell_prealloc(cell, fp, n_ant) once per ACTIVE cell, before nr_cell_alloc_fence() / the RX fence (A8 calls it from the cell activation,
 * after the cell's frame parameters are known; it replaces the direct nr_csirs_disc_start() call of the single-cell receiver, which stays valid and is cell 0 when the
 * calling thread is not bound). It binds the calling thread to `cell` for the duration of the call, runs nr_csirs_disc_start() (which allocates the rings, the TD queue
 * and starts the cell's discovery worker bound to `cell`), and restores the binding. */
#ifndef NR_CSIRS_DISC_CELL_H
#define NR_CSIRS_DISC_CELL_H

#include <stdbool.h>
#include <stddef.h>
#include "PHY/defs_nr_common.h"
#include "nr_csirs_disc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Host and device bytes the cell's discovery instance holds (the G1 ledger reads this; sizes are real, taken at start). */
typedef struct {
  bool active;           /* the cell's worker is running */
  int n_rb, n_ant;       /* the cell's carrier the buffers were sized for */
  int ring_slots;        /* worker ring capacity after the host-RAM budget */
  int td_slots;          /* time-domain queue capacity after the host-RAM budget */
  size_t ring_host_bytes;/* FD ring (+ snapshot ring when the TD front end is absent) */
  size_t td_host_bytes;  /* TD slot pool + FFT batch buffers */
  size_t gpu_bytes;      /* device bytes committed by nr_csirs_disc_pipeline_init for this cell's ring (0 on the CPU path) */
} nr_csirs_disc_cell_mem_t;

/* Start the discovery instance of `cell` for the carrier `fp` / `n_ant`. Returns 0, or what nr_csirs_disc_start() returns (-1: refused, logged). cell outside
 * 0..NR_CELL_MAX-1 returns -1. Must run before the RX fence. */
int nr_csirs_disc_cell_prealloc(int cell, const NR_DL_FRAME_PARMS *fp, int n_ant);
/* Stop the instance of `cell` (joins its worker, frees its buffers, returns its bytes to the host ledger). Idempotent. */
void nr_csirs_disc_cell_release(int cell);
/* Bytes held by `cell` (false when the cell has no instance). */
bool nr_csirs_disc_cell_mem(int cell, nr_csirs_disc_cell_mem_t *out);
/* Host bytes an instance for (fp, n_ant) takes at FULL size (default ring + default TD queue); pure, for the admission model before the cell is activated. */
size_t nr_csirs_disc_cell_host_need(const NR_DL_FRAME_PARMS *fp, int n_ant);
/* Host bytes nr_csirs_disc_td_init(fp, n_ant, capacity) allocates (nr_csirs_disc_td.c); pure. */
size_t nr_csirs_disc_td_bytes(const NR_DL_FRAME_PARMS *fp, int n_ant, int capacity);
/* nr_csirs_disc_td_reset() for an explicit cell (nr_csirs_disc_td.c). */
void nr_csirs_disc_td_reset_cell(int cell);
/* The discovery metrics of `cell`; nr_csirs_disc_get_metrics() is this for the calling thread's cell. */
void nr_csirs_disc_get_metrics_cell(int cell, nr_csirs_disc_metrics_t *out);

#ifdef __cplusplus
}
#endif
#endif
