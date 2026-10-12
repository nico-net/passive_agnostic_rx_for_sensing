/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_PDSCH_REAL_FIRST_H
#define NR_PDSCH_REAL_FIRST_H
/* GPUSCHED ISAC_PDSCHQ_REAL_FIRST=1: CPU-side priority of the passive PDSCH ring (nr_pdsch_passive_queue.c).
 * When the oldest entry is a solver probe and a full decode is queued behind it, every entry of that full decode's slot
 * (probes of the same slot included, so the consumer still shares the slot's FEP) moves to the head in its order; the
 * other entries keep their order. Generic over the ring element (tested with a fake element in nr_pdsch_real_first_test). */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* nr_real_first_promote: reorders ring[tail .. tail+count) (circular, depth entries of elem bytes; ns[] moves along).
 * is_probe(e) / slot_of(e) read an element; scratch holds one element. Returns the number of entries moved to the head
 * (0 = nothing to do). */
static inline int nr_real_first_promote(void *ring, size_t elem, uint64_t *ns, int tail, int count, int depth,
                                        bool (*is_probe)(const void *), long (*slot_of)(const void *), void *scratch)
{
#define RF_AT(p) ((char *)ring + (size_t)((tail + (p)) % depth) * elem)
  if (count < 2 || !is_probe(RF_AT(0)))
    return 0;
  int i = 1;
  while (i < count && is_probe(RF_AT(i)))
    i++;
  if (i == count)
    return 0;
  const long slot = slot_of(RF_AT(i));
  if (slot == slot_of(RF_AT(0))) /* the head's own slot: the consumer takes it as one group anyway */
    return 0;
  int k = 0;
  for (int p = 0; p < count; p++) {
    if (slot_of(RF_AT(p)) != slot)
      continue;
    if (p != k) { /* rotate entry p down to position k */
      memcpy(scratch, RF_AT(p), elem);
      const uint64_t t_ns = ns[(tail + p) % depth];
      for (int q = p; q > k; q--) {
        memcpy(RF_AT(q), RF_AT(q - 1), elem);
        ns[(tail + q) % depth] = ns[(tail + q - 1) % depth];
      }
      memcpy(RF_AT(k), scratch, elem);
      ns[(tail + k) % depth] = t_ns;
    }
    k++;
  }
  return k;
#undef RF_AT
}
#endif
