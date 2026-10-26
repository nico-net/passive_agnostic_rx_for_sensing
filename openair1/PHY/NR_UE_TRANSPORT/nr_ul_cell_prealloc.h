/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* mpci A6: entry points the owner of the receiver (A8: cell manager / nr-ue.c) calls for every ACTIVE cell, cell 0 included,
 * BEFORE nr_cell_alloc_fence() (a first access of a heap cell object after the fence is fatal, see common/utils/nr_cell.h).
 * Cell-indexed UL state of the PUSCH / UL-layout modules (declaration-only conversion; the objects keep their names). */
#ifndef NR_UL_CELL_PREALLOC_H
#define NR_UL_CELL_PREALLOC_H
#ifdef __cplusplus
extern "C" {
#endif
void nr_ul_layout_cell_prealloc(int cell); /* nr_pdcch_ul_layout_lock.c: g_len (180 KiB), g_only (72 KiB) */
void nr_ul_config_disc_cell_prealloc(int cell); /* nr_pdcch_ul_config_disc.c: g_ue (1.1 MiB) */
void nr_ul_discovery_cell_prealloc(int cell);   /* nr_pdcch_ul_discovery.c: g_contexts (NR_PDCCH_BLIND_MAX_UE x 2.3 MiB, calloc: pages are touched lazily) */
void nr_ul_nid_bg_cell_prealloc(int cell);      /* nr_ul_nid_bg.c: g_rnti (132 KiB) */
/* All of the above for one cell (A8 calls this once per active cell, cell 0 included, before nr_cell_alloc_fence()). */
static inline void nr_ul_cell_prealloc_all(int cell)
{
  nr_ul_layout_cell_prealloc(cell);
  nr_ul_config_disc_cell_prealloc(cell);
  nr_ul_discovery_cell_prealloc(cell);
  nr_ul_nid_bg_cell_prealloc(cell);
}
#ifdef __cplusplus
}
#endif
#endif
