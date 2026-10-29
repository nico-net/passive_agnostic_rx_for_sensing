/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Per-cell activation of the blind PDCCH monitor modules (MPCI A4). The large per-cell objects (NR_CELL_STATIC_HEAP) of
 * nr_pdcch_blind_monitor_rt.c, nr_pdcch_blind_monitor.c and nr_pdcch_coreset_bank.c are allocated here, once per active
 * cell, BEFORE nr_cell_alloc_fence() (a first access of an unallocated slot after the fence is fatal). */
#ifndef NR_PDCCH_CELL_H
#define NR_PDCCH_CELL_H

#ifdef __cplusplus
extern "C" {
#endif

/* All heap objects of the PDCCH monitor modules of `cell` (calls the two per-file functions below). Idempotent, thread safe. */
void nr_pdcch_cell_prealloc(int cell);
int nr_pdcch_workers_parked_state(int cell, int which); /* which: 0 janon, 1 disc, 2 nid; 0 none, 1 parked, 2 armed */
void nr_pdcch_blind_monitor_cell_prealloc(int cell); /* nr_pdcch_blind_monitor.c: the lookahead lane table */
void nr_pdcch_coreset_bank_cell_prealloc(int cell);  /* nr_pdcch_coreset_bank.c: the verified and dormant CORESET banks */

#ifdef __cplusplus
}
#endif
#endif
