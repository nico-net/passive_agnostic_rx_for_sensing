/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Key of the passive PDSCH decoder's per-slot DM-RS channel-estimate cache (K32, 2026-10-01).
 *
 * A cache hit must mean "a miss would have computed a bit-identical estimate", so every input of
 * nr_pdsch_channel_estimation() as the passive decoder drives it is in the key: the slot (IQ and
 * pilot sequence), the FULL-SLOT DM-RS symbol mask (the cached estimate always covers every DM-RS
 * symbol of the slot, never a probe horizon or an [S, S+L) window), DM-RS type / nSCID / CDM groups
 * / layer count / all 12 port bits (type-2 ports 8-11 must not alias 0-3) / scrambling id, the RB
 * range the estimate was built over (indexed from its own first RB), BWP start/size and the DM-RS
 * reference point (the pilot offset), the single estimated branch (-1 = all), the receive antenna
 * count and the FEP frequency offset the samples were transformed with. A segmented (PRB-list / PRG)
 * estimate is data-ordered, not RB-indexed: seg != 0 never hits. Pure header: unit-tested by
 * test_nr_pdsch_chest_key. */
#ifndef NR_PDSCH_CHEST_KEY_H
#define NR_PDSCH_CHEST_KEY_H
#include <stdbool.h>
#include <stdint.h>

typedef struct {
  long slot;
  uint16_t dmrs_pos; /* FULL slot DM-RS symbol mask, not [S, S+L) */
  uint8_t cfg_type, nscid, cdm, nl;
  uint16_t dmrs_ports; /* all 12 bits */
  uint16_t scr;
  int rb_lo, rb_n;
  int bwp_start, bwp_size, ref_point;
  int only_ant;
  int n_ant;
  double fo_hz;
  int seg; /* seg != 0 => never cached */
} nr_pdsch_chest_key_t;

static inline bool nr_pdsch_chest_key_eq(const nr_pdsch_chest_key_t *a, const nr_pdsch_chest_key_t *b)
{
  return !a->seg && !b->seg && a->slot == b->slot && a->dmrs_pos == b->dmrs_pos && a->cfg_type == b->cfg_type
         && a->nscid == b->nscid && a->cdm == b->cdm && a->nl == b->nl && a->dmrs_ports == b->dmrs_ports
         && a->scr == b->scr && a->rb_lo == b->rb_lo && a->rb_n == b->rb_n && a->bwp_start == b->bwp_start
         && a->bwp_size == b->bwp_size && a->ref_point == b->ref_point && a->only_ant == b->only_ant
         && a->n_ant == b->n_ant && a->fo_hz == b->fo_hz;
}
#endif
