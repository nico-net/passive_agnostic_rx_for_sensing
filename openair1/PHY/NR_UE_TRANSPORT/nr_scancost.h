/* SCANCOST / SCAN2 / ANONLEN cost cuts (bit-exact on the blind PDCCH scan path) are always on. */
#ifndef NR_SCANCOST_H
#define NR_SCANCOST_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static inline bool nr_scancost_on(void) { return true; }

enum { NR_SCAN2_UL = 0, NR_SCAN2_ULSSH = 1, NR_SCAN2_TDD = 2, NR_SCAN2_N = 3 };
static inline bool nr_scan2_on(int id) { (void)id; return true; }

enum { NR_ANONLEN_SCREEN = 0, NR_ANONLEN_LEARN = 1, NR_ANONLEN_N = 2 };
static inline bool nr_anonlen_on(int id) { (void)id; return true; }

/* Open-addressing index over an append-only key array (keys[0..n-1], n <= NR_KEYHASH_CAP / 2): the same answer as a
 * linear "first q with keys[q] == key" search, in O(1). Used by the PDSCH solver's layout offset-key grouping. */
#define NR_KEYHASH_CAP 512
typedef struct {
  int16_t slot[NR_KEYHASH_CAP];
} nr_keyhash_t;

static inline void nr_keyhash_init(nr_keyhash_t *h) { memset(h->slot, 0xff, sizeof(h->slot)); }

/* Returns the index q of key in keys[], or -1; *pos is where nr_keyhash_put() stores it if absent. */
static inline int nr_keyhash_find(const nr_keyhash_t *h, const uint64_t *keys, uint64_t key, int *pos)
{
  unsigned p = (unsigned)((key * 0x9E3779B97F4A7C15ull) >> 55) & (NR_KEYHASH_CAP - 1);
  while (h->slot[p] >= 0) {
    if (keys[h->slot[p]] == key) {
      *pos = (int)p;
      return h->slot[p];
    }
    p = (p + 1) & (NR_KEYHASH_CAP - 1);
  }
  *pos = (int)p;
  return -1;
}

static inline void nr_keyhash_put(nr_keyhash_t *h, int pos, int q) { h->slot[pos] = (int16_t)q; }

#endif
