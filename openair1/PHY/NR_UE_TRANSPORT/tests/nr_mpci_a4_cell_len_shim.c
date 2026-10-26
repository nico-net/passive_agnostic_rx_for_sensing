/* C shim for tests/nr_mpci_a4_interleave_test.cc: nr_pdcch_cell_len_state.h is C only (_Atomic, NR_CELL_STATIC_INIT).
 * It is the cell DCI length state of nr_pdcch_blind_monitor_rt.c, which cannot be linked into a unit test. */
#include <string.h>
#include "nr_pdcch_cell_len_state.h"

#define g_cell_lens NR_CELL_REF(g_cell_lens)

void a4_cll_reset(void)
{
  const int c = nr_cell_idx();
  memset(&g_cell_len_hint_cells[c], 0, sizeof(g_cell_len_hint_cells[c]));
  for (int i = 0; i < NR_CELL_LEN_HINT_N; i++) atomic_store(&g_cell_lens[i], 0);
  for (int i = 0; i < 16; i++) {
    atomic_store(&g_ul_cell_len_cells[c][i].rnti, (uint16_t)0);
    atomic_store(&g_ul_cell_len_cells[c][i].len, (uint16_t)0);
  }
}
void a4_cll_note(int len) { janon_note_cell_len(len); }
int a4_cll_get(int i) { return atomic_load(&g_cell_lens[i]); }
void a4_cll_ul_note(unsigned rnti, int len) { ul_cell_len_note((uint16_t)rnti, len); }
int a4_cll_ul_get(unsigned rnti) { return ul_cell_len((uint16_t)rnti); }
