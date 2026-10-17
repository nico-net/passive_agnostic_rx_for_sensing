/* CRC-first trial variants of the UL layout lock (ISAC_UL_MCS_TABLE_SWEEP / ISAC_UL_K2_PAIR / ISAC_UL_BWP_SEED). Pure helpers.
 * A trial slot tests the top-K layout hypotheses under ONE variant v; the slots rotate over v = 0..count-1 (v = 0: default table, k2). */
#ifndef NR_UL_TRIAL_VARIANT_H
#define NR_UL_TRIAL_VARIANT_H
#include <stdbool.h>
#include <stdint.h>
#include "nr_ul_layout_lookalike.h"

static inline int nr_ul_variant_ntables(bool sweep, bool tp) { return sweep ? (tp ? 2 : 3) : 1; }
static inline int nr_ul_variant_count(bool sweep, bool k2pair, bool tp)
{
  return nr_ul_variant_ntables(sweep, tp) * (k2pair ? 2 : 1);
}
/* *tbl = -1 (the lock's default table) or the UL MCS table to force (0..2, TP 3..4); *k2d = 0 or 1 (k2 + 1 on every TDRA row) */
static inline void nr_ul_variant_get(int v, bool sweep, bool k2pair, bool tp, int *tbl, int *k2d)
{
  const int nt = nr_ul_variant_ntables(sweep, tp), ti = v % nt;
  *tbl = ti > 0 ? (tp ? 3 : 0) + ti : -1;
  *k2d = (k2pair && v >= nt) ? 1 : 0;
}
/* a non-default variant latches only on two independent CRC passes (distinct slots): a CRC16 grant false-alarms at ~3e-4 per candidate */
static inline bool nr_ul_variant_latch_ok(uint32_t passes, uint32_t distinct_slots) { return passes >= 2 && distinct_slots >= 2; }

/* look-alike partners of (n, s) inside a carrier of `carrier` RBs: (n+1,s) (n+1,s-1) (n-1,s) (n-1,s+1). Returns the count (<= 4). */
static inline int nr_ul_lookalike_partners(uint16_t n, uint16_t s, uint16_t carrier, uint16_t out[4][2])
{
  const int dn[4] = {1, 1, -1, -1}, ds[4] = {0, -1, 0, 1}; /* N+1 variants first */
  int c = 0;
  for (int i = 0; i < 4; i++) {
    const int pn = n + dn[i], ps = s + ds[i];
    if (pn < 1 || ps < 0 || ps + pn > carrier || !nr_ul_layout_lookalike(n, s, (uint16_t)pn, (uint16_t)ps)) continue;
    out[c][0] = (uint16_t)pn;
    out[c][1] = (uint16_t)ps;
    c++;
  }
  return c;
}
#endif
