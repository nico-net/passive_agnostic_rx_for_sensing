/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Per-cell slot clock and timing hand-offs of the receive path (multi-PCI plan task A8).
 *
 * The cell's slot thread (UE_thread, executables/nr-ue.c) publishes its un-wrapped absolute slot and the wall time of the slot arrival;
 * the scan / PDSCH / PUSCH consumers of the SAME cell compare their job slots with it (age, freshness, credit). The absolute slot is
 * frame * slots_per_frame + slot of THAT cell (its own SFN), so two cells never share it. The timing loop (nr_adjust_synch_ue.c, on the
 * cell's PBCH path) hands a deferred coarse rebase to the cell's slot thread, and the slot thread's RF census tells the cell's timing
 * loop when its stream carries no signal. All five are therefore one slot per cell, selected by nr_cell_idx() of the calling thread
 * (every thread that reads them is bound to its cell: A4/A5/A6/A8).
 *
 * The names are kept: `nr_ue_diag_producer_absolute_slot` is an lvalue of the calling thread's cell (atomic_load(&x) etc. unchanged).
 * N = 1: cell 0's slot, same values as the former process globals.
 * Definitions: executables/nr-ue.c (slot clock, rebase) and nr_adjust_synch_ue.c (signal absent); test stubs and the physical simulators
 * define the same arrays with the NR_UE_CELL_CLOCK_DEFINE_* macros. Never declare these names with `extern` elsewhere. */
#ifndef NR_UE_CELL_CLOCK_H
#define NR_UE_CELL_CLOCK_H
#include <stdatomic.h>
#include "common/utils/nr_cell.h"

extern _Atomic long nr_ue_diag_producer_absolute_slot_cells[NR_CELL_MAX]; /* -1 = no slot produced yet */
extern _Atomic long nr_ue_diag_producer_wall_ns_cells[NR_CELL_MAX];       /* CLOCK_REALTIME ns of the last slot arrival */
extern _Atomic long nr_ue_pending_rebase_delta_cells[NR_CELL_MAX];
extern _Atomic int nr_ue_pending_rebase_valid_cells[NR_CELL_MAX];
extern _Atomic int nr_ue_rf_signal_absent_cells[NR_CELL_MAX];

#define nr_ue_diag_producer_absolute_slot (nr_ue_diag_producer_absolute_slot_cells[nr_cell_idx()])
#define nr_ue_diag_producer_wall_ns (nr_ue_diag_producer_wall_ns_cells[nr_cell_idx()])
#define nr_ue_pending_rebase_delta (nr_ue_pending_rebase_delta_cells[nr_cell_idx()])
#define nr_ue_pending_rebase_valid (nr_ue_pending_rebase_valid_cells[nr_cell_idx()])
#define nr_ue_rf_signal_absent (nr_ue_rf_signal_absent_cells[nr_cell_idx()])

/* Definitions (exactly one translation unit per binary each). */
#define NR_UE_CELL_CLOCK_DEFINE_PRODUCER()                                                                  \
  _Atomic long nr_ue_diag_producer_absolute_slot_cells[NR_CELL_MAX] = {[0 ... NR_CELL_MAX - 1] = -1}; \
  _Atomic long nr_ue_diag_producer_wall_ns_cells[NR_CELL_MAX]
#define NR_UE_CELL_CLOCK_DEFINE_REBASE()                         \
  _Atomic long nr_ue_pending_rebase_delta_cells[NR_CELL_MAX]; \
  _Atomic int nr_ue_pending_rebase_valid_cells[NR_CELL_MAX]
#define NR_UE_CELL_CLOCK_DEFINE_SIGNAL() _Atomic int nr_ue_rf_signal_absent_cells[NR_CELL_MAX]
#endif
