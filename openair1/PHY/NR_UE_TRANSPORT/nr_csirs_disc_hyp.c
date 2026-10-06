/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */

#include "nr_csirs_disc.h"

#include "PHY/nr_phy_common/inc/nr_phy_common.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* The non-mapping columns of TS 38.211 Table 7.4.1.5.3-1.  The actual RE
 * placement is deliberately obtained from get_csi_mapping_parms() below. */
typedef struct {
  uint8_t fd_bits;
  uint8_t fd_ones;
  uint8_t densities[3];
  uint8_t n_density;
  uint8_t uses_l1;
} row_rule_t;

static const row_rule_t row_rules[18] = {
    {4, 1, {3}, 1, 0},       /*  1 */
    {12, 1, {0, 1, 2}, 3, 0}, /*  2 */
    {6, 1, {0, 1, 2}, 3, 0}, /*  3 */
    {3, 1, {2}, 1, 0},       /*  4 */
    {6, 1, {2}, 1, 0},       /*  5 */
    {6, 4, {2}, 1, 0},       /*  6 */
    {6, 2, {2}, 1, 0},       /*  7 */
    {6, 2, {2}, 1, 0},       /*  8 */
    {6, 6, {2}, 1, 0},       /*  9 */
    {6, 3, {2}, 1, 0},       /* 10 */
    {6, 4, {0, 1, 2}, 3, 0}, /* 11 */
    {6, 4, {0, 1, 2}, 3, 0}, /* 12 */
    {6, 3, {0, 1, 2}, 3, 1}, /* 13 */
    {6, 3, {0, 1, 2}, 3, 1}, /* 14 */
    {6, 3, {0, 1, 2}, 3, 0}, /* 15 */
    {6, 4, {0, 1, 2}, 3, 1}, /* 16 */
    {6, 4, {0, 1, 2}, 3, 1}, /* 17 */
    {6, 4, {0, 1, 2}, 3, 0}, /* 18 */
};

typedef struct {
  nr_csirs_disc_hyp_t hyp;
  uint16_t sym_mask;
  uint16_t sc_mask;
  int next;
} table_entry_t;

#define HYP_HASH_SIZE 32768
static table_entry_t *table;
static int table_n;
static int table_cap;
static int hash_head[HYP_HASH_SIZE];
static pthread_once_t table_once = PTHREAD_ONCE_INIT;

static unsigned pattern_hash(uint16_t sym_mask, uint16_t sc_mask, uint8_t density)
{
  uint32_t x = (uint32_t)sym_mask * 0x9e3779b1u;
  x ^= (uint32_t)sc_mask * 0x85ebca6bu;
  x ^= (uint32_t)density * 0xc2b2ae35u;
  x ^= x >> 16;
  return x & (HYP_HASH_SIZE - 1);
}

static void add_entry(const nr_csirs_disc_hyp_t *hyp, uint16_t sym_mask, uint16_t sc_mask)
{
  if (table_n == table_cap) {
    const int new_cap = table_cap ? table_cap * 2 : 4096;
    table_entry_t *grown = realloc(table, (size_t)new_cap * sizeof(*grown));
    if (!grown)
      return;
    table = grown;
    table_cap = new_cap;
  }
  const unsigned bucket = pattern_hash(sym_mask, sc_mask, hyp->density);
  table[table_n] = (table_entry_t){.hyp = *hyp, .sym_mask = sym_mask, .sc_mask = sc_mask, .next = hash_head[bucket]};
  hash_head[bucket] = table_n++;
}

static bool mapping_footprint(
    int row, int bitmap, int l0, int l1, uint16_t *sym_mask, uint16_t *sc_mask, uint8_t *cdm_type)
{
  const csi_mapping_parms_t p = get_csi_mapping_parms(row, bitmap, l0, l1);
  int groups = 0;
  for (int i = 0; i < p.size; i++)
    if (p.j[i] + 1 > groups)
      groups = p.j[i] + 1;
  const int group_size = groups ? p.ports / groups : 0;
  if (group_size != 1 && group_size != 2 && group_size != 4 && group_size != 8)
    return false;
  *cdm_type = group_size == 8 ? 3 : group_size == 4 ? 2 : group_size == 2 ? 1 : 0;
  uint16_t by_symbol[NR_CSIRS_DISC_NSYM] = {0};
  for (int ji = 0; ji < p.size; ji++) {
    for (int kp = 0; kp <= p.kprime; kp++) {
      for (int lp = 0; lp <= p.lprime; lp++) {
        const int k = p.koverline[ji] + kp;
        const int l = p.loverline[ji] + lp;
        if (k < 0 || k >= 12 || l < 0 || l >= NR_CSIRS_DISC_NSYM)
          return false;
        by_symbol[l] |= (uint16_t)(1u << k);
      }
    }
  }

  uint16_t symbols = 0;
  uint16_t subcarriers = 0;
  for (int l = 0; l < NR_CSIRS_DISC_NSYM; l++) {
    if (!by_symbol[l])
      continue;
    symbols |= (uint16_t)(1u << l);
    if (!subcarriers)
      subcarriers = by_symbol[l];
    else if (subcarriers != by_symbol[l])
      return false; /* nr_csirs_disc_fp_t represents a rectangular per-RB footprint. */
  }
  if (!symbols || !subcarriers)
    return false;
  *sym_mask = symbols;
  *sc_mask = subcarriers;
  return true;
}

static void build_table(void)
{
  for (int i = 0; i < HYP_HASH_SIZE; i++)
    hash_head[i] = -1;

  for (int row = 1; row <= 18; row++) {
    const row_rule_t *rule = &row_rules[row - 1];
    const int bitmap_limit = 1 << rule->fd_bits;
    for (int bitmap = 1; bitmap < bitmap_limit; bitmap++) {
      /* get_csi_mapping_parms() has an unbounded set-bit walk.  This equality
       * is both the TS-valid allocation rule and the guard which makes the call safe. */
      if (__builtin_popcount((unsigned)bitmap) != rule->fd_ones)
        continue;
      for (int l0 = 0; l0 < NR_CSIRS_DISC_NSYM; l0++) {
        const int l1_first = rule->uses_l1 ? (l0 + 2 > 2 ? l0 + 2 : 2) : 0;
        const int l1_last = rule->uses_l1 ? 12 : 0;
        for (int l1 = l1_first; l1 <= l1_last; l1++) {
          uint16_t sym_mask = 0;
          uint16_t sc_mask = 0;
          uint8_t cdm_type = 0;
          if (!mapping_footprint(row, bitmap, l0, l1, &sym_mask, &sc_mask, &cdm_type))
            continue;
          for (int d = 0; d < rule->n_density; d++) {
            nr_csirs_disc_hyp_t hyp = {
                .row = (uint8_t)row,
                .freq_domain = (uint16_t)bitmap,
                .symb_l0 = (uint8_t)l0,
                .symb_l1 = (uint8_t)l1,
                .cdm_type = cdm_type,
                .density = rule->densities[d],
            };
            add_entry(&hyp, sym_mask, sc_mask);
          }
        }
      }
    }
  }
}

static bool valid_period(uint16_t period)
{
  static const uint16_t legal[NR_CSIRS_DISC_NUM_PERIODS] = {4, 5, 8, 10, 16, 20, 32, 40, 64, 80, 160, 320, 640};
  for (int i = 0; i < NR_CSIRS_DISC_NUM_PERIODS; i++)
    if (legal[i] == period)
      return true;
  return false;
}

static bool valid_fp(const nr_csirs_disc_fp_t *fp)
{
  return fp && fp->sym_mask && !(fp->sym_mask & ~0x3fffu) && fp->sc_mask && !(fp->sc_mask & ~0x0fffu)
         && fp->density <= 3 && fp->n_rb && fp->rb_start < NR_CSIRS_DISC_MAX_RB
         && (unsigned)fp->rb_start + fp->n_rb <= NR_CSIRS_DISC_MAX_RB && valid_period(fp->period)
         && fp->offset < fp->period;
}

static float footprint_iou(uint16_t a_sym, uint16_t a_sc, uint16_t b_sym, uint16_t b_sc)
{
  const int a = __builtin_popcount((unsigned)a_sym) * __builtin_popcount((unsigned)a_sc);
  const int b = __builtin_popcount((unsigned)b_sym) * __builtin_popcount((unsigned)b_sc);
  const int intersection = __builtin_popcount((unsigned)(a_sym & b_sym)) * __builtin_popcount((unsigned)(a_sc & b_sc));
  return (float)intersection / (float)(a + b - intersection);
}

static int hyp_order(const nr_csirs_disc_hyp_t *a, const nr_csirs_disc_hyp_t *b)
{
  if (a->fit != b->fit)
    return a->fit > b->fit ? -1 : 1;
  if (a->fp_index != b->fp_index)
    return a->fp_index < b->fp_index ? -1 : 1;
  if (a->row != b->row)
    return a->row < b->row ? -1 : 1;
  if (a->freq_domain != b->freq_domain)
    return a->freq_domain < b->freq_domain ? -1 : 1;
  if (a->symb_l0 != b->symb_l0)
    return a->symb_l0 < b->symb_l0 ? -1 : 1;
  if (a->symb_l1 != b->symb_l1)
    return a->symb_l1 < b->symb_l1 ? -1 : 1;
  return 0;
}

static void insert_ranked(nr_csirs_disc_hyp_t *out, int *n, int max_out, const nr_csirs_disc_hyp_t *candidate)
{
  int pos = *n;
  if (pos == max_out) {
    if (hyp_order(candidate, &out[pos - 1]) >= 0)
      return;
    pos--;
  } else {
    (*n)++;
  }
  while (pos > 0 && hyp_order(candidate, &out[pos - 1]) < 0) {
    if (pos < max_out)
      out[pos] = out[pos - 1];
    pos--;
  }
  out[pos] = *candidate;
}

int nr_csirs_disc_hypotheses(const nr_csirs_disc_fp_t *fps, int n_fp, nr_csirs_disc_hyp_t *out, int max_out)
{
  if (!fps || n_fp <= 0 || !out || max_out <= 0)
    return 0;
  pthread_once(&table_once, build_table);
  if (!table || table_n == 0)
    return 0;

  int n = 0;
  for (int f = 0; f < n_fp; f++) {
    const nr_csirs_disc_fp_t *fp = &fps[f];
    if (!valid_fp(fp))
      continue;

    /* The exact key is hash-indexed.  Near patterns are checked only after it;
     * the legal table is fixed-size and built once, so lookup remains bounded. */
    const unsigned bucket = pattern_hash(fp->sym_mask, fp->sc_mask, fp->density);
    for (int i = hash_head[bucket]; i >= 0; i = table[i].next) {
      const table_entry_t *e = &table[i];
      if (e->sym_mask != fp->sym_mask || e->sc_mask != fp->sc_mask || e->hyp.density != fp->density)
        continue;
      nr_csirs_disc_hyp_t h = e->hyp;
      h.rb_start = fp->rb_start;
      h.n_rb = fp->n_rb;
      h.period = fp->period;
      h.offset = fp->offset;
      h.fp_index = (uint8_t)f;
      h.fit = 1.0f;
      insert_ranked(out, &n, max_out, &h);
    }

    for (int i = 0; i < table_n; i++) {
      const table_entry_t *e = &table[i];
      if (e->hyp.density != fp->density || (e->sym_mask == fp->sym_mask && e->sc_mask == fp->sc_mask))
        continue;
      const float fit = footprint_iou(e->sym_mask, e->sc_mask, fp->sym_mask, fp->sc_mask);
      if (fit < 0.8f)
        continue;
      nr_csirs_disc_hyp_t h = e->hyp;
      h.rb_start = fp->rb_start;
      h.n_rb = fp->n_rb;
      h.period = fp->period;
      h.offset = fp->offset;
      h.fp_index = (uint8_t)f;
      h.fit = fit;
      insert_ranked(out, &n, max_out, &h);
    }
  }
  return n;
}
