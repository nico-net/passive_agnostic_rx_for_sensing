/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* E3: exclusivity of a scored DM-RS footprint (header only, unit-tested with the scorer). A co-scheduled or neighbouring UE with the same DM-RS identity must not confirm a grant:
 * the claimed PRBs must explain the slot's DM-RS (E2 occupancy F1 >= 0.5, the threshold dmrs_scan_filter_readings keeps a reading at) AND the same sequence must not be significant on the PRBs
 * next to the allocation (outside it, up to 12 each side of its extent). With fewer neighbours than E2's minimum PRB count (the allocation fills the carrier or sits at its edge) the second
 * test cannot fail. */
#ifndef NR_E3_EXCL_H
#define NR_E3_EXCL_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "nr_dl_layout_score.h"
static inline bool nr_e3_exclusive(const nr_dl_hyp_t *claim, const nr_dl_obs_t *ob, const nr_dl_score_t *s, int n_rb_carrier)
{
  if (s->occ_f1 < 0.5f) return false;
  uint8_t in_a[NR_DL_MAX_PRB];
  memset(in_a, 0, sizeof in_a);
  int lo = n_rb_carrier, hi = -1;
  for (int q = 0; q < claim->n_crb; q++) {
    in_a[claim->crb[q]] = 1;
    if (claim->crb[q] < lo) lo = claim->crb[q];
    if (claim->crb[q] > hi) hi = claim->crb[q];
  }
  uint16_t adj[NR_DL_MAX_PRB];
  int na = 0;
  for (int p = (lo - 12 > 0 ? lo - 12 : 0); p <= hi + 12 && p < n_rb_carrier && p < NR_DL_MAX_PRB; p++)
    if (!in_a[p]) adj[na++] = (uint16_t)p;
  if (na < (claim->dmrs_type == 1 ? 6 : 11)) return true;
  nr_dl_hyp_t h = *claim;
  h.crb = adj;
  h.n_crb = na;
  h.data_sym_mask = 0;
  nr_dl_score_t sa;
  nr_dl_layout_score(&h, ob, &sa);
  return !(sa.n_cells > 0 && sa.n_signif == sa.n_cells);
}
#endif
