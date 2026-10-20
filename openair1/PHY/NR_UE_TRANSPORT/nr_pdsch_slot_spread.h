#pragma once
/* ISAC_PDSCH_SLOT_SPREAD=1: how a dequeued slot group is split between this consumer and idle ones.
 * Pure helpers (unit-tested in tests/nr_pdsch_slot_spread_test.cc); the queue applies them under g_lock. */
#include <stdint.h>

/* order[0..n-1] = member indexes, cheapest first (stable). */
static inline void nr_slot_spread_order(const uint64_t cost[], int n, int order[])
{
  for (int i = 0; i < n; i++) {
    int j = i;
    while (j > 0 && cost[order[j - 1]] > cost[i]) { order[j] = order[j - 1]; j--; }
    order[j] = i;
  }
}

/* Members handed to idle consumers: one per idle consumer, the caller always keeps at least one. */
static inline int nr_slot_spread_give(int n_group, int idle)
{
  const int g = idle < n_group - 1 ? idle : n_group - 1;
  return g > 0 ? g : 0;
}
