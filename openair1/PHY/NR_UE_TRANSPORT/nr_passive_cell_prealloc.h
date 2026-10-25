/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* mpci A3: entry points the owner of the receiver (A8: cell manager / nr-ue.c) calls for every ACTIVE cell, cell 0 included,
 * BEFORE nr_cell_alloc_fence() (a first access of a heap cell object after the fence is fatal, see common/utils/nr_cell.h). */
#ifndef NR_PASSIVE_CELL_PREALLOC_H
#define NR_PASSIVE_CELL_PREALLOC_H
#ifdef __cplusplus
extern "C" {
#endif
void nr_passive_reconf_rules_cell_prealloc(int cell); /* g_rrc_rx, g_rrc_eff: 512 KiB each */
void nr_passive_ue_ctx_cell_prealloc(int cell);       /* g_active_index: 128 KiB */
/* Store the PCI reported by the ISAC_METRICS record of the calling thread's cell (cell 0 also keeps the legacy global). */
void nr_passive_metrics_set_pci(int pci);
#ifdef __cplusplus
}
#endif
#endif
