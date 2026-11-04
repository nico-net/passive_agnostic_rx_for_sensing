/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Accepted-DCI de-duplication ring: a match is a duplicate only within NR_DEDUPE_WINDOW monotonic slots, because the frame-wrapped slot
 * repeats every 1024 frames and a sparse RNTI (SI-RNTI) would otherwise alias onto a stale entry. Caller holds the lock. */
#ifndef NR_DEDUPE_H
#define NR_DEDUPE_H
#include <stdbool.h>
#include <stdint.h>
#define NR_DEDUPE_N 256
#define NR_DEDUPE_WINDOW 64 /* slots: early and end-of-slot jobs of one slot differ by the queue lag at most */
typedef struct { uint32_t slot; int64_t mono; uint16_t rnti; uint8_t dir; } nr_dedupe_ent_t;
/* mono < 0 = unknown: that entry/query matches on the wrapped slot alone (previous behaviour). */
static inline bool nr_dedupe_probe(nr_dedupe_ent_t *tab, unsigned *w, uint32_t abs_slot, int64_t mono, uint16_t rnti, int dir)
{
  for (int i = 0; i < NR_DEDUPE_N; i++) {
    const nr_dedupe_ent_t *e = &tab[i];
    if (e->slot != abs_slot + 1 || e->rnti != rnti || e->dir != dir) continue;
    const int64_t d = mono - e->mono;
    if (mono < 0 || e->mono < 0 || (d < NR_DEDUPE_WINDOW && -d < NR_DEDUPE_WINDOW)) return true;
  }
  nr_dedupe_ent_t *e = &tab[*w % NR_DEDUPE_N]; /* +1: slot 0 never looks like an empty entry */
  *e = (nr_dedupe_ent_t){.slot = abs_slot + 1, .mono = mono, .rnti = rnti, .dir = (uint8_t)dir};
  (*w)++;
  return false;
}
#endif
