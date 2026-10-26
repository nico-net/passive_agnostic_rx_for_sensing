/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Cell-wide DCI 1_1 length hint: the lengths locked on air on any CORESET of the cell, ranked by how many locks corroborate them
 * (ties: most recent first). A CORESET that has locked no length of its own decodes its known RNTIs at the first entry.
 *
 * Ranking by recency alone let ONE lock of a different size (a false-alarm RNTI or a fallback-format size) replace the hint of every
 * such CORESET at once: the banked PCI-nID USS pass of the lab cell decoded its UEs at len 47 until a single len 49 (D1) or len 39 (D2)
 * lock, then at the wrong size for the rest of the run, and its DL DCI stream stopped within the same log line
 * [MEASURED, sens6 OTA lab cell, i6D1_lab_20261026_012309 / i6D2_lab_20261026_013014: bank=0 pass=2 accepts 2000..4000 per 5 s, then 0
 * right after "DCI 1_1 length locked ... len=49 / len=39"; DL recall 0.45/0.33 and 0.22/0.20]. A real reconfiguration still wins: every
 * UE then locks the new size, its count reaches the old one's and the tie goes to the newer lock.
 * Header-only and pure so the unit test links it directly. */
#ifndef NR_PDCCH_CELL_LEN_HINT_H
#define NR_PDCCH_CELL_LEN_HINT_H
#include <stdint.h>

#define NR_CELL_LEN_HINT_N 4

typedef struct {
  int len[NR_CELL_LEN_HINT_N];     /* 0 = empty; index 0 is the hint */
  uint32_t cnt[NR_CELL_LEN_HINT_N];
} nr_cell_len_hint_t;

/* one lock of `len` was observed */
static inline void nr_cell_len_hint_note(nr_cell_len_hint_t *h, int len)
{
  if (len <= 0) return;
  int at = -1;
  for (int i = 0; i < NR_CELL_LEN_HINT_N && at < 0; i++)
    if (h->len[i] == len) at = i;
  if (at < 0) { /* replace the weakest entry (the last, lists are kept sorted) */
    at = NR_CELL_LEN_HINT_N - 1;
    h->len[at] = len;
    h->cnt[at] = 0;
  }
  if (h->cnt[at] < 0x7fffffffu) h->cnt[at]++;
  /* move up past every entry with a count not above ours (ties go to the newer lock) */
  while (at > 0 && h->cnt[at - 1] <= h->cnt[at]) {
    const int l = h->len[at - 1];
    const uint32_t c = h->cnt[at - 1];
    h->len[at - 1] = h->len[at];
    h->cnt[at - 1] = h->cnt[at];
    h->len[at] = l;
    h->cnt[at] = c;
    at--;
  }
}
#endif
