/* CRC-first trial variants of the UL layout lock (ISAC_UL_MCS_TABLE_SWEEP / ISAC_UL_K2_PAIR / ISAC_UL_BWP_SEED). Pure helpers.
 * A trial slot tests the top-K layout hypotheses under ONE variant v; the slots rotate over v = 0..count-1 (v = 0: default table, k2). */
#ifndef NR_UL_TRIAL_VARIANT_H
#define NR_UL_TRIAL_VARIANT_H
#include <stdbool.h>
#include <stdint.h>
#include "nr_ul_layout_lookalike.h"

static inline int nr_ul_variant_ntables(bool sweep, bool tp) { return sweep ? (tp ? 2 : 3) : 1; }
/* variants = MCS tables x k2 {+0,+1} x symbol allocation {S0 L14 type A, S0 L13 type B} */
static inline int nr_ul_variant_count(bool sweep, bool k2pair, bool tdapair, bool tp)
{
  return nr_ul_variant_ntables(sweep, tp) * (k2pair ? 2 : 1) * (tdapair ? 2 : 1);
}
/* *tbl = -1 (the lock's default table) or the UL MCS table to force (0..2, TP 3..4); *k2d = 0 or 1 (k2 + 1 on every TDRA row);
 * *tda = 0 (S0 L14 type A) or 1 (S0 L13 type B) */
static inline void nr_ul_variant_get(int v, bool sweep, bool k2pair, bool tdapair, bool tp, int *tbl, int *k2d, int *tda)
{
  const int nt = nr_ul_variant_ntables(sweep, tp), ti = v % nt, r = v / nt, nk = k2pair ? 2 : 1;
  *tbl = ti > 0 ? (tp ? 3 : 0) + ti : -1;
  *k2d = k2pair ? r % 2 : 0;
  *tda = tdapair ? (r / nk) % 2 : 0;
}

/* PUSCH time-domain allocation rows a trial can force on every row of the locked layout (variant bits 4..6). Rows 0 and 1 are the two-row core of the
 * variant cycle; 2..7 are the other symbol allocations a cell can configure, from TS 38.214 Table 6.1.2.1.1-2 (default PUSCH TDRA A, normal CP) and the
 * L = 13 allocations that leave the last symbol to SRS: {S, L, mapping type A = 0 / B = 1}. A cell with its own pusch-TimeDomainAllocationList uses
 * the same shapes (S, L, mapping); the CRC decides which one the cell uses (nr_pdcch_ul_layout_lock.c evidence rule), nothing here is tuned to a cell.
 * Not covered: Table rows S4 L6 B and S8 L6 B, and the K2 > j rows (the k2 pair covers k2 + 1 only). */
#define NR_UL_TDA_ROWS 8
typedef struct { uint8_t s, l, map; } nr_ul_tda_row_t;
static inline nr_ul_tda_row_t nr_ul_tda_row(int i)
{
  static const nr_ul_tda_row_t t[NR_UL_TDA_ROWS] = {{0, 14, 0}, {0, 13, 1}, {0, 13, 0}, {0, 12, 0}, {0, 10, 0}, {2, 10, 1}, {4, 10, 1}, {4, 8, 1}};
  return t[i >= 0 && i < NR_UL_TDA_ROWS ? i : 0];
}
/* TS 38.214 6.1.2.1: type A starts at symbol 0 (DM-RS at l0 = 2 or 3) with L in 4..14; type B has S + L <= 14 */
static inline bool nr_ul_tda_legal(int s, int l, int map)
{
  return map == 0 ? (s == 0 && l >= 4 && l <= 14) : (s >= 0 && l >= 1 && s + l <= 14);
}
/* variant cycle over `tda_rows` allocation rows (2 = the core); same ordering as the two-row functions above, further rows appended after the core */
static inline int nr_ul_variant_count_rows(bool sweep, bool k2pair, int tda_rows, bool tp)
{
  return nr_ul_variant_ntables(sweep, tp) * (k2pair ? 2 : 1) * (tda_rows > 0 ? tda_rows : 1);
}
static inline void nr_ul_variant_get_rows(int v, bool sweep, bool k2pair, int tda_rows, bool tp, int *tbl, int *k2d, int *tda)
{
  const int nt = nr_ul_variant_ntables(sweep, tp), ti = v % nt, r = v / nt, nk = k2pair ? 2 : 1;
  *tbl = ti > 0 ? (tp ? 3 : 0) + ti : -1;
  *k2d = k2pair ? r % 2 : 0;
  *tda = (r / nk) % (tda_rows > 0 ? tda_rows : 1);
}
/* Live trials (one variant per slot): the core rows in 3 of 4 variant cycles; in the 4th, the extra rows (2..7), the two core rows of that cycle
 * standing for two of them each time round. `round` = the cycle counter, base = the core row the cycle position chose. */
static inline int nr_ul_variant_tda_round(uint32_t round, int base)
{
  if ((round & 3u) != 3u) return base;
  return 2 + (int)((((round >> 2) << 1) + (uint32_t)(base & 1)) % (NR_UL_TDA_ROWS - 2));
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
