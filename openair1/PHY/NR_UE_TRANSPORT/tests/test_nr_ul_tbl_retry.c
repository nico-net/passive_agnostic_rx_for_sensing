#include <assert.h>
#include <string.h>
#include "../nr_ul_tbl_retry.h"

int main(void)
{
  nr_ul_tblr_ent_t tab[NR_UL_TBLR_UES];
  memset(tab, 0, sizeof tab);
  nr_ul_tblr_ent_t *e = nr_ul_tblr_get(tab, 0x4698, true);
  assert(e && nr_ul_tblr_get(tab, 0x4698, true) == e && !nr_ul_tblr_get(tab, 0x1111, false));
  /* high SNR, MCS 27 failed in table 0: qam256 (1) and LowSE (2) are both feasible at 39 dB; first two offers try all */
  assert(nr_ul_tblr_pick(e, 27, 0, 39.0, 2.0) == 0x6u);
  assert(nr_ul_tblr_pick(e, 27, 0, 39.0, 2.0) == 0x6u);
  /* then 1 offer in 8, one table at a time, rotating */
  unsigned seen = 0; int offers = 0;
  for (int i = 0; i < 32; i++) { const unsigned m = nr_ul_tblr_pick(e, 27, 0, 39.0, 2.0); if (m) { assert(__builtin_popcount(m) == 1); seen |= m; offers++; } }
  assert(offers == 4 && seen == 0x6u);
  /* low SNR: nothing feasible for qam256 MCS 27 (needs ~25 dB+) */
  nr_ul_tblr_ent_t *f = nr_ul_tblr_get(tab, 0x2222, true);
  assert(nr_ul_tblr_pick(f, 27, 0, 12.0, 2.0) == 0);
  /* an invalid index (MCS 29 exists in no table) never retries into 1/2 */
  assert(nr_ul_tblr_pick(f, 29, 0, 39.0, 2.0) == 0);
  /* learning: two passes make the table the first choice, a lone pass does not */
  assert(nr_ul_tblr_first(e, 0) == 0);
  nr_ul_tblr_pass(e, 1);
  assert(nr_ul_tblr_first(e, 0) == 0);
  nr_ul_tblr_pass(e, 1);
  assert(nr_ul_tblr_first(e, 0) == 1);
  nr_ul_tblr_pass(e, 0); /* the other table passing replaces it, one pass is not yet learned */
  assert(nr_ul_tblr_first(e, 0) == 0 && e->learned == 1);
  return 0;
}
