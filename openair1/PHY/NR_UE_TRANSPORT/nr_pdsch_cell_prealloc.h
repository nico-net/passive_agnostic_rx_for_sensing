/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* mpci A5: entry points the owner of the receiver (A8: cell manager / nr-ue.c) calls for every ACTIVE cell, cell 0 included,
 * BEFORE nr_cell_alloc_fence() (a first access of a heap cell object after the fence is fatal, see common/utils/nr_cell.h).
 * nr_pdsch_cell_prealloc(cell) does all three; the parts exist so a unit test can link one module only.
 *   decode: g_ded_fails_since_ok 256 KiB, g_rnti_dec 135 KiB, s_pend_cand 256 KiB
 *   queue : g_rnti_dec 256 KiB, g_rnti_ok 256 KiB, g_ring ~111 KiB, g_pending ~111 KiB
 *   solver: g_p ~16.4 MiB, g_r ~162 KiB
 *   recovery: g_seen 512 KiB
 * (sizes: sizeof of the per-cell object, [OFFLINE VERIFIED, DGX GB10 aarch64 nm -S]) */
#ifndef NR_PDSCH_CELL_PREALLOC_H
#define NR_PDSCH_CELL_PREALLOC_H
#ifdef __cplusplus
extern "C" {
#endif
void nr_pdsch_passive_decode_cell_prealloc(int cell);
void nr_pdsch_passive_queue_cell_prealloc(int cell);
void nr_pdsch_solver_cell_prealloc(int cell);
void nr_pdsch_recovery_cell_prealloc(int cell);
void nr_pdsch_cell_prealloc(int cell); /* all of the above (defined in nr_pdsch_passive_queue.c) */
/* test hook of the DL HARQ soft store (nr_pdsch_passive_decode.c), see there */
#include <stdint.h>
int nr_pdsch_passive_harqc_test(int op, uint16_t rnti, uint8_t pid, uint8_t ndi, uint8_t rv, uint32_t tbs);
#ifdef __cplusplus
}
#endif
#endif
