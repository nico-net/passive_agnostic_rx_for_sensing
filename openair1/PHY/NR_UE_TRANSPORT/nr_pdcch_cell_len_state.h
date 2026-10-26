/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Per-cell DCI length state of the blind PDCCH monitor (MPCI A4): the cell-wide DCI 1_1 length hint (g_cell_lens, ranked by
 * corroborating locks, nr_pdcch_cell_len_hint.h) and the UL DCI 0_1 length each RNTI locked on (g_ul_cell_len).
 * One cell's locks never feed another cell's hint: every object is NR_CELL_STATIC and addressed by nr_cell_idx().
 *
 * Moved out of nr_pdcch_blind_monitor_rt.c unchanged (same logic, same names) so a unit test can drive it without the receiver.
 * Header-only with file-local statics: exactly one translation unit of a binary includes it (nr_pdcch_blind_monitor_rt.c in the
 * receiver, the interleave test in the test). The includer adds the `#define g_cell_lens NR_CELL_REF(g_cell_lens)` alias itself;
 * this header defines no macro. */
#ifndef NR_PDCCH_CELL_LEN_STATE_H
#define NR_PDCCH_CELL_LEN_STATE_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

#include "common/utils/nr_cell.h"
#include "nr_pdcch_cell_len_hint.h"

/* T26c LEN_HINT: DCI 1_1 lengths locked on ANY CORESET of this cell (most recent first) -- the hint for a CORESET that
 * has none locked yet (one gNB, one RRC template: the second CORESET's UEs very likely use the same size). */
typedef _Atomic int g_cell_lens_cell_t[NR_CELL_LEN_HINT_N];
NR_CELL_STATIC(g_cell_lens_cell_t, g_cell_lens);
/* UL DCI 0_1 length each RNTI locked on air, on any geometry of the cell (a UE's size does not depend on the CORESET carrying it). */
typedef struct { _Atomic uint16_t rnti, len; } g_ul_cell_len_cell_t[16];
NR_CELL_STATIC(g_ul_cell_len_cell_t, g_ul_cell_len);
NR_CELL_STATIC(nr_cell_len_hint_t, g_cell_len_hint);
NR_CELL_STATIC_INIT(pthread_mutex_t, g_cell_len_hint_mu, PTHREAD_MUTEX_INITIALIZER);

static inline void ul_cell_len_note(uint16_t rnti, int len)
{
  int slot = (int)(rnti % 16u);
  atomic_store(&g_ul_cell_len_cells[nr_cell_idx()][slot].len, (uint16_t)len);
  atomic_store(&g_ul_cell_len_cells[nr_cell_idx()][slot].rnti, rnti);
}
static inline int ul_cell_len(uint16_t rnti)
{
  const int slot = (int)(rnti % 16u);
  return atomic_load(&g_ul_cell_len_cells[nr_cell_idx()][slot].rnti) == rnti
             ? (int)atomic_load(&g_ul_cell_len_cells[nr_cell_idx()][slot].len)
             : 0;
}
/* janon_note_cell_len: remembers a DCI 1_1 length locked on any CORESET of this cell (most recent first, 4 entries) as a length hint for other CORESETs.
 *   Only the LEN_HINT path reads the list. Caller: run_occasion_one() after a length lock.
 */
static inline void janon_note_cell_len(int len)
{
  if (len <= 0) return;
  /* ranked by corroborating locks (nr_pdcch_cell_len_hint.h), not by recency: one contradicting lock must not replace the hint of every CORESET without a lock */
  const int c = nr_cell_idx();
  pthread_mutex_lock(&g_cell_len_hint_mu_cells[c]);
  nr_cell_len_hint_note(&g_cell_len_hint_cells[c], len);
  for (int i = 0; i < NR_CELL_LEN_HINT_N; i++) atomic_store(&g_cell_lens_cells[c][i], g_cell_len_hint_cells[c].len[i]);
  pthread_mutex_unlock(&g_cell_len_hint_mu_cells[c]);
}
#endif
