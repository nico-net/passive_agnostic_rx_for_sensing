/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* E5: the per-thread slot FEP cache bookkeeping (header only, unit-tested). One FEP per (antenna set, symbol) of a slot, whichever caller asks first: the layout scorer, the
 * DM-RS fit and the pdsch_dmrs CFR tap. A claim names the slot, the IQ buffer and the de-rotation frequency the FEP would use; any of the three changing drops the cache. */
#ifndef NR_FEP_CACHE_H
#define NR_FEP_CACHE_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
typedef struct {
  int64_t key;
  uint16_t done;      /* bit l = symbol l is in the buffer */
  const void *iq;
  uint64_t fo_bits;
  unsigned n_fep;     /* FEPs performed (test counter) */
} nr_fep_cache_t;
#define NR_FEP_CACHE_INIT {-1, 0, NULL, 0, 0}
/* true: the caller must run the FEP of `sym` now (the symbol is marked present); false: already in the buffer. */
static inline bool nr_fep_cache_claim(nr_fep_cache_t *c, int64_t key, const void *iq, double fo, int sym)
{
  uint64_t b = ~0ull; /* every NaN is one key */
  if (fo == fo) memcpy(&b, &fo, sizeof b);
  if (c->key != key || c->iq != iq || c->fo_bits != b) {
    c->key = key; c->iq = iq; c->fo_bits = b; c->done = 0;
  }
  if ((c->done >> sym) & 1) return false;
  c->done |= (uint16_t)(1u << sym);
  c->n_fep++;
  return true;
}
/* true when (key, iq, fo) differs from the last call (or on the first): a second-level cache keyed like the FEP cache above uses it to drop its own contents. */
static inline bool nr_fep_cache_changed(nr_fep_cache_t *c, int64_t key, const void *iq, double fo)
{
  uint64_t b = ~0ull;
  if (fo == fo) memcpy(&b, &fo, sizeof b);
  const bool ch = c->key != key || c->iq != iq || c->fo_bits != b;
  c->key = key; c->iq = iq; c->fo_bits = b;
  return ch;
}
#endif
