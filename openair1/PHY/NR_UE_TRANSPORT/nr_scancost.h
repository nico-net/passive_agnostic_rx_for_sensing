/* SCANCOST (2026-10-14): bit-exact cost cuts on the blind PDCCH scan path, all behind ISAC_SCANCOST=1 (default off).
 * Off = the previous code path unchanged. See docs/superpowers/reports/csd/SCANCOST-20261014.md. */
#ifndef NR_SCANCOST_H
#define NR_SCANCOST_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* nr_scancost_on: ISAC_SCANCOST != 0, read once per process (racing first callers agree). */
static inline bool nr_scancost_on(void)
{
  static int on = -1;
  if (on < 0) {
    const char *e = getenv("ISAC_SCANCOST");
    on = e && atoi(e) != 0;
  }
  return on;
}

/* SCAN2 (2026-10-14): the second tier, default off. ISAC_SCANCOST=2 enables all three; each also has its own flag
 * (ISAC_SCAN2_UL, ISAC_SCAN2_ULSSH, ISAC_SCAN2_TDD); a flag set to 0 wins over the umbrella. The SCANCOST=1 cuts stay on
 * nr_scancost_on() (any non-zero value, so =2 includes them). */
enum { NR_SCAN2_UL = 0, NR_SCAN2_ULSSH = 1, NR_SCAN2_TDD = 2, NR_SCAN2_N = 3 };
static inline bool nr_scan2_on(int id)
{
  static const char *const names[NR_SCAN2_N] = {"ISAC_SCAN2_UL", "ISAC_SCAN2_ULSSH", "ISAC_SCAN2_TDD"};
  static int on[NR_SCAN2_N] = {-1, -1, -1};
  if (on[id] < 0) {
    const char *e = getenv(names[id]);
    const char *u = getenv("ISAC_SCANCOST");
    on[id] = e ? atoi(e) != 0 : (u && atoi(u) >= 2);
  }
  return on[id];
}

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
