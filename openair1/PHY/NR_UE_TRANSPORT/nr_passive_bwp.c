/* Passive DL BWP tracking -- see nr_passive_bwp.h. */
#include "nr_passive_bwp.h"
#include <math.h>

#include <stdlib.h>
#include <string.h>

uint8_t nr_pbwp_riv_bits(uint16_t n)
{
  const uint32_t span = (uint32_t)n * (n + 1u) / 2u;
  uint8_t b = 0;
  while (b < 31 && (1u << b) < span)
    b++;
  return b;
}

bool nr_pbwp_riv_decode(uint32_t riv, uint16_t n, uint16_t *start, uint16_t *len)
{
  if (n == 0 || riv >= (uint32_t)n * (n + 1u) / 2u)
    return false;
  const uint32_t a = riv / n, b = riv % n;
  const uint32_t l = (a + b < n) ? a + 1 : n + 1 - a;
  const uint32_t s = (a + b < n) ? b : n - 1 - b;
  if (l < 1 || s + l > n)
    return false;
  *start = (uint16_t)s;
  *len = (uint16_t)l;
  return true;
}

/* K: the layout's width excluding the FDRA and the BWP indicator. */
static int layout_k(const nr_pbwp_t *t)
{
  return (int)t->base_len - nr_pbwp_riv_bits(t->base_size) - t->base_ind_bits;
}

uint16_t nr_pbwp_len_for(const nr_pbwp_t *t, uint16_t n, uint8_t d)
{
  return (uint16_t)(layout_k(t) + nr_pbwp_riv_bits(n) + d);
}

static uint16_t len_for_dk(const nr_pbwp_t *t, uint16_t n, uint8_t d, uint8_t dk)
{
  return (uint16_t)(layout_k(t) - (int)dk + nr_pbwp_riv_bits(n) + d);
}

uint32_t nr_pbwp_riv_field(nr_dci_bits_t payload, uint16_t len, uint8_t d, uint8_t rb)
{
  /* MSB first: identifier (1) | indicator (d) | FDRA (rb) | ... */
  const int pos = (int)len - 1 - d - rb;
  if (pos < 0 || rb == 0 || rb > 31)
    return 0xffffffffu;
  return nr_dci_bits_field(&payload, len, 1 + d, rb);
}

static bool len_registered(const nr_pbwp_t *t, uint16_t len)
{
  for (int i = 0; i < t->n; i++)
    if (t->e[i].dci_len == len)
      return true;
  return false;
}

void nr_pbwp_init(nr_pbwp_t *t, uint16_t carrier_rbs, uint16_t base_start, uint16_t base_size,
                  uint16_t base_len, uint8_t base_ind_bits)
{
  nr_pbwp_init_ex(t, carrier_rbs, base_start, base_size, base_len, base_ind_bits, 0);
}

void nr_pbwp_init_ex(nr_pbwp_t *t, uint16_t carrier_rbs, uint16_t base_start, uint16_t base_size,
                     uint16_t base_len, uint8_t base_ind_bits, uint8_t k_slack)
{
  nr_pbwp_free(t);
  memset(t, 0, sizeof(*t));
  t->k_slack = k_slack > 5 ? 5 : k_slack;
  t->carrier_rbs = carrier_rbs;
  t->base_len = base_len;
  t->base_size = base_size;
  t->base_ind_bits = base_ind_bits;
  t->e[0] = (nr_pbwp_entry_t){.dci_len = base_len, .start = (int16_t)base_start, .size = base_size,
                              .ind_bits = base_ind_bits};
  t->n = 1;
  for (uint8_t dk = 0; dk <= t->k_slack; dk++)
    for (uint8_t d = 0; d <= 2; d++)
      for (uint16_t n = NR_PBWP_MIN_SIZE; n <= carrier_rbs && t->n_cand < NR_PBWP_MAX_CAND; n++) {
        const uint16_t len = len_for_dk(t, n, d, dk);
        bool have = len == base_len;
        for (int k = 0; k < t->n_cand && !have; k++)
          have = t->cand_len[k] == len;
        if (!have)
          t->cand_len[t->n_cand++] = len;
      }
}

static void free_groups(nr_pbwp_entry_t *e)
{
  for (int k = 0; k < NR_PBWP_MAX_GROUPS; k++) {
    free(e->g[k].score);
    e->g[k].score = NULL;
  }
  e->ng = 0;
}

void nr_pbwp_free(nr_pbwp_t *t)
{
  for (int i = 0; i < NR_PBWP_MAX; i++)
    free_groups(&t->e[i]);
}

/* One hypothesis group per indicator width d that some size maps onto `len`. */
static bool make_groups(const nr_pbwp_t *t, nr_pbwp_entry_t *e, uint16_t len)
{
  free_groups(e);
  const uint8_t n_excl = e->n_excl;
  typeof(e->excl) excl;
  memcpy(excl, e->excl, sizeof(excl));
  *e = (nr_pbwp_entry_t){.dci_len = len, .start = -1};
  e->n_excl = n_excl;
  memcpy(e->excl, excl, sizeof(excl));
  for (uint8_t dk = 0; dk <= t->k_slack; dk++)
   for (uint8_t d = 0; d <= 2; d++) {
    uint16_t lo = 0, hi = 0;
    for (uint16_t n = NR_PBWP_MIN_SIZE; n <= t->carrier_rbs; n++)
      if (len_for_dk(t, n, d, dk) == len) {
        if (lo == 0)
          lo = n;
        hi = n;
      }
    if (lo == 0 || hi - lo >= 128 || e->ng >= NR_PBWP_MAX_GROUPS)
      continue;
    nr_pbwp_group_t *g = &e->g[e->ng];
    *g = (nr_pbwp_group_t){.d = d, .dk = dk, .lo = lo, .hi = hi, .n_starts = (uint16_t)(t->carrier_rbs - lo + 1)};
    g->score = calloc((size_t)(hi - lo + 1) * g->n_starts, sizeof(float));
    if (g->score == NULL)
      break;
    e->ng++;
  }
  return e->ng > 0;
}

bool nr_pbwp_resolved(const nr_pbwp_t *t, int idx)
{
  return idx >= 0 && idx < t->n && t->e[idx].start >= 0;
}

void nr_pbwp_mark_seen(nr_pbwp_t *t, uint16_t rnti)
{
  t->rnti_seen[rnti >> 3] |= (uint8_t)(1u << (rnti & 7));
}

int nr_pbwp_entry_for_len(const nr_pbwp_t *t, uint16_t rnti, uint16_t len)
{
  const int own = (int)t->rnti_bwp[rnti] - 1;
  if (own >= 0 && own < t->n && t->e[own].dci_len == len)
    return own;
  for (int i = 0; i < t->n; i++)
    if (t->e[i].dci_len == len)
      return i;
  return -1;
}

bool nr_pbwp_on_accept(nr_pbwp_t *t, uint16_t rnti, int idx)
{
  if (idx < 0 || idx >= t->n)
    return false;
  t->e[idx].hits++;
  if (nr_pbwp_resolved(t, idx))
    nr_pbwp_mark_seen(t, rnti);
  const int prev = (int)t->rnti_bwp[rnti] - 1;
  t->rnti_bwp[rnti] = (uint8_t)(idx + 1);
  if (prev >= 0 && prev != idx) {
    t->switches++;
    return true;
  }
  return false;
}

uint16_t nr_pbwp_next_probe_len(nr_pbwp_t *t)
{
  for (int k = 0; k < t->n_cand; k++) {
    const int i = (t->probe_cursor + k) % t->n_cand;
    if (!len_registered(t, t->cand_len[i])) {
      t->probe_cursor = (i + 1) % t->n_cand;
      return t->cand_len[i];
    }
  }
  return 0;
}

uint16_t nr_pbwp_last_rnti(const nr_pbwp_t *t, int idx)
{
  for (uint32_t r = 1; r < 65536; r++)
    if (t->rnti_bwp[r] == idx + 1)
      return (uint16_t)r;
  return 0;
}

int nr_pbwp_probe_accept(nr_pbwp_t *t, uint16_t rnti, uint16_t len, uint32_t payload_hash)
{
  int k = 0;
  while (k < t->n_cand && t->cand_len[k] != len)
    k++;
  if (k == t->n_cand)
    return -1;
  /* Evidence is counted at an ALREADY-registered length too: a false registration (a repeated artifact RNTI) used to make this
   * return early, so the real UE's DCIs at that same length were never "proven" and its grants never scored. */
  const bool registered = len_registered(t, len);
  bool enough = false;
  if (nr_pbwp_rnti_seen(t, rnti)) {
    enough = ++t->cand_hits[k] >= NR_PBWP_NEW_HITS;
  } else {
    int j = 0; /* repetition proof: the same unproven RNTI NR_PBWP_NEW_HITS times at this length */
    while (j < 4 && !(t->cand_rnti_n[k][j] && t->cand_rnti[k][j] == rnti))
      j++;
    if (j == 4) { /* new: replace the least-seen slot */
      j = 0;
      for (int q = 1; q < 4; q++)
        if (t->cand_rnti_n[k][q] < t->cand_rnti_n[k][j])
          j = q;
      t->cand_rnti[k][j] = rnti;
      t->cand_rnti_n[k][j] = 0;
      t->cand_rnti_h[k][j] = payload_hash;
      t->cand_rnti_d[k][j] = 0;
    }
    /* An invariant payload repeating under one RNTI is a degenerate polar fixed point, not a UE (the sweep module's trap 2): measured on
     * the sa-bed, the same artifact RNTI at length 44 registered a spurious entry in every run and, once rotation probed more
     * lengths, filled every tracker slot with artifacts. A real DCI's payload changes with scheduling. */
    if (t->cand_rnti_n[k][j] > 0 && payload_hash != t->cand_rnti_h[k][j])
      t->cand_rnti_d[k][j] = 1;
    enough = ++t->cand_rnti_n[k][j] >= NR_PBWP_NEW_HITS && t->cand_rnti_d[k][j];
    if (enough)
      nr_pbwp_mark_seen(t, rnti);
  }
  if (registered || t->n >= NR_PBWP_MAX || !enough)
    return -1;
  if (!make_groups(t, &t->e[t->n], len))
    return -1;
  t->rnti_bwp[rnti] = (uint8_t)(t->n + 1);
  return t->n++;
}

static int cmp_float(const void *a, const void *b)
{
  const float x = *(const float *)a, y = *(const float *)b;
  return (x > y) - (x < y);
}

static bool size_dead(const nr_pbwp_group_t *g, int k)
{
  return k < 64 ? (g->dead_lo >> k) & 1u : (g->dead_hi >> (k - 64)) & 1u;
}

static bool is_excluded(const nr_pbwp_entry_t *e, uint8_t ind_bits, uint16_t size, int start)
{
  for (int i = 0; i < e->n_excl; i++)
    if (e->excl[i].ind_bits == ind_bits && e->excl[i].size == size && e->excl[i].start == start)
      return true;
  return false;
}

bool nr_pbwp_score_grant(nr_pbwp_t *t, int idx, nr_dci_bits_t payload, const float *prb_coh)
{
  if (idx <= 0 || idx >= t->n || t->e[idx].ng == 0 || nr_pbwp_resolved(t, idx))
    return false;
  nr_pbwp_entry_t *e = &t->e[idx];
  const int C = t->carrier_rbs;
  /* Matched-filter statistic: SUM over the PRBs a hypothesis claims of (coherence - thr), thr halfway
   * between this grant's noise floor (p10) and lit level (p90). A MEAN would let a wrong size that
   * squeezes the RIV into a 2-PRB window inside the lit band tie with the truth; the sum rewards
   * covering every lit PRB and charges for claiming unlit ones. A grant lighting (nearly) the whole
   * carrier carries no position information and is skipped. */
  float srt[C];
  memcpy(srt, prb_coh, sizeof(float) * C);
  qsort(srt, C, sizeof(float), cmp_float);
  const float p10 = srt[C / 10], p90 = srt[(9 * C) / 10];
  if (p90 - p10 < 0.1f)
    return false;
  const double thr = 0.5 * (p10 + p90);
  double pre[C + 1];
  pre[0] = 0;
  for (int p = 0; p < C; p++)
    pre[p + 1] = pre[p] + (prb_coh[p] - thr);
  for (int gi = 0; gi < e->ng; gi++) {
    nr_pbwp_group_t *g = &e->g[gi];
    const uint8_t rb = nr_pbwp_riv_bits(g->lo);
    const uint32_t riv = nr_pbwp_riv_field(payload, e->dci_len, g->d, rb);
    for (int k = 0; k <= g->hi - g->lo; k++) {
      if (size_dead(g, k))
        continue;
      const uint16_t n = (uint16_t)(g->lo + k);
      uint16_t s0, l;
      if (!nr_pbwp_riv_decode(riv, n, &s0, &l)) {
        /* the true (d, N) never produces an impossible RIV */
        if (k < 64) g->dead_lo |= 1ull << k; else g->dead_hi |= 1ull << (k - 64);
        continue;
      }
      float *row = &g->score[(size_t)k * g->n_starts];
      for (int s = 0; s + n <= C; s++)
        row[s] += (float)(pre[s + s0 + l] - pre[s + s0]);
    }
  }
  if (++e->grants_scored < NR_PBWP_MIN_GRANTS)
    return false;
  float best = -1e30f, second = -1e30f;
  int bg = -1, bk = -1, bs = -1;
  for (int gi = 0; gi < e->ng; gi++) {
    const nr_pbwp_group_t *g = &e->g[gi];
    for (int k = 0; k <= g->hi - g->lo; k++) {
      if (size_dead(g, k))
        continue;
      const int n = g->lo + k;
      const float *row = &g->score[(size_t)k * g->n_starts];
      for (int s = 0; s + n <= C; s++) {
        if (is_excluded(e, e->g[gi].d, (uint16_t)n, s))
          continue;
        if (row[s] > best) { second = best; best = row[s]; bg = gi; bk = k; bs = s; }
        else if (row[s] > second) second = row[s];
      }
    }
  }
  if (bg < 0 || best - second < NR_PBWP_MARGIN)
    return false;
  e->ind_bits = e->g[bg].d;
  e->tda_dk = e->g[bg].dk;
  e->size = (uint16_t)(e->g[bg].lo + bk);
  e->start = (int16_t)bs;
  free_groups(e);
  return true;
}

/* The resolved hypothesis failed its TB-CRC test: remember it as refuted and rescore from scratch. */
static void refute_resolved(nr_pbwp_t *t, nr_pbwp_entry_t *e)
{
  if (e->n_excl < NR_PBWP_MAX_EXCL) {
    e->excl[e->n_excl].size = e->size;
    e->excl[e->n_excl].start = e->start;
    e->excl[e->n_excl].ind_bits = e->ind_bits;
    e->n_excl++;
  }
  make_groups(t, e, e->dci_len);
}

void nr_pbwp_feed_crc(nr_pbwp_t *t, int idx, bool ok)
{
  if (!nr_pbwp_resolved(t, idx) || idx == 0)
    return;
  nr_pbwp_entry_t *e = &t->e[idx];
  e->crc_try++;
  e->crc_ok += ok;
  if (e->crc_try >= 32 && e->crc_ok == 0)
    refute_resolved(t, e); /* wrong hypothesis: rescore from scratch, never to the same one */
}

void nr_pbwp_feed_crc_search(nr_pbwp_t *t, int idx, bool ok)
{
  if (!nr_pbwp_resolved(t, idx) || idx == 0)
    return;
  nr_pbwp_entry_t *e = &t->e[idx];
  e->crc_try++;
  e->crc_ok += ok;
  if (e->crc_try >= NR_PBWP_SEARCH_REFUTE_TRIES && e->crc_ok == 0)
    refute_resolved(t, e);
}

uint32_t nr_pbwp_translate_riv(uint32_t value, uint8_t cur_bits, uint8_t tgt_bits)
{
  (void)cur_bits; /* fewer current bits than needed: the value already reads as zero-prepended */
  return tgt_bits >= 32 ? value : value & ((1u << tgt_bits) - 1u);
}

int nr_pbwp_indicator(nr_pbwp_t *t, int idx, uint8_t ind_value)
{
  if (idx < 0 || idx >= t->n || ind_value > 3)
    return idx;
  const int bound = (int)t->ind_map[ind_value] - 1;
  if (bound >= 0 && bound != idx && nr_pbwp_resolved(t, bound))
    return bound; /* switch grant: allocation is in the indicated BWP */
  if (bound < 0 && ++t->ind_votes[ind_value][idx] >= NR_PBWP_IND_LEARN)
    t->ind_map[ind_value] = (uint8_t)(idx + 1);
  return idx;
}

void nr_pbwp_coreset_observe(nr_pbwp_t *t, int n_win, int base_lo, int base_hi, int base_ref,
                             const float *corr, const int16_t *ref, int symbol, float threshold)
{
  if (symbol < 0 || symbol > 1)
    return;
  if (symbol == 0)
    t->cs.occ++;
  /* Fade old evidence (x3/4 every 4096 observations, ~10 s of occasions). The CORESET evidence is cumulative, so after the UE moves to
   * another BWP the old CORESET's counts dominate for good, and the relative lit threshold (a quarter of the strongest window) never lets
   * the new one in: the receiver then kept decoding a BWP the UE had left. */
  if (symbol == 0 && (t->cs.occ & 4095) == 0) {
    for (int w = 0; w < NR_PBWP_CS_MAXWIN; w++)
      for (int sy = 0; sy < 2; sy++) {
        t->cs.hits[w][sy] -= t->cs.hits[w][sy] >> 2;
        t->cs.base_hits[w][sy] -= t->cs.base_hits[w][sy] >> 2;
      }
    for (int k = 0; k < 276; k++)
      t->cs.ref_votes[k] -= t->cs.ref_votes[k] >> 2;
  }
  t->cs.n_win = n_win < NR_PBWP_CS_MAXWIN ? n_win : NR_PBWP_CS_MAXWIN;
  t->cs.base_lo = base_lo;
  t->cs.base_hi = base_hi;
  t->cs.base_ref = base_ref;
  for (int w = 0; w < n_win && w < NR_PBWP_CS_MAXWIN; w++) {
    if (corr[w] < threshold)
      continue;
    if (w >= base_lo && w <= base_hi && ref[w] == base_ref) {
      if (t->n > 1) /* only after a new BWP is evidenced: before, the configured CORESET carries everyone */
        t->cs.base_hits[w][symbol]++;
      continue;
    }
    t->cs.hits[w][symbol]++;
    if (symbol == 0 && ref[w] >= 0 && ref[w] < 276)
      t->cs.ref_votes[ref[w]]++;
  }
}

bool nr_pbwp_coreset_hypothesis(const nr_pbwp_t *t, int *start_rb, int *n_rb, int *duration, int *ref_rb)
{
  if (t->cs.occ < NR_PBWP_CS_MIN_OCC)
    return false;
  /* Once a new BWP is evidenced, a dedicated CORESET inside the configured one AND referenced like it
   * (CRB 0, measured on OAI rfsim 2026-09-15) is invisible to the reference test. It shows as windows of
   * the configured range lit at the rate of the hottest one: all DCIs of the moved UEs land there, while
   * the configured CORESET keeps only common traffic (a median fails: most of a wide range is empty). */
  const nr_pbwp_coreset_t *cs = &t->cs;
  uint32_t base_thr = UINT32_MAX;
  if (t->n > 1 && cs->base_hi >= cs->base_lo) {
    uint32_t hot = 0;
    for (int w = cs->base_lo; w <= cs->base_hi && w < NR_PBWP_CS_MAXWIN; w++)
      hot = cs->base_hits[w][0] > hot ? cs->base_hits[w][0] : hot;
    base_thr = hot / 2 > NR_PBWP_CS_MIN_HITS ? hot / 2 : NR_PBWP_CS_MIN_HITS;
  }
  /* "Lit" is absolute (MIN_HITS) AND relative to the strongest window. Absolute alone saturates: every window collects 8 chance hits
   * eventually, and with the half-window statistic (a weaker test than the full window) the proposal grew to the whole carrier within
   * minutes. The strongest windows of a real CORESET carry thousands of hits, noise windows tens. */
  uint32_t top = 0;
  for (int w = 0; w < NR_PBWP_CS_MAXWIN; w++)
    top = cs->hits[w][0] > top ? cs->hits[w][0] : top;
  const uint32_t lit_min = top / 4 > NR_PBWP_CS_MIN_HITS ? top / 4 : NR_PBWP_CS_MIN_HITS;
  /* largest run of lit windows, one unlit window of slack (DM-RS only where a PDCCH was sent) */
  int best_lo = -1, best_hi = -1, lo = -1, last = -100;
  for (int w = 0; w < NR_PBWP_CS_MAXWIN; w++) {
    if (cs->hits[w][0] < lit_min && cs->base_hits[w][0] < base_thr)
      continue;
    if (w - last > 2)
      lo = w;
    last = w;
    if (best_lo < 0 || w - lo > best_hi - best_lo) {
      best_lo = lo;
      best_hi = w;
    }
  }
  if (best_lo < 0)
    return false;
  uint32_t h0 = 0, h1 = 0;
  bool own_ref = false; /* the run holds a window at a reference other than the configured one */
  for (int w = best_lo; w <= best_hi; w++) {
    h0 += cs->hits[w][0] + cs->base_hits[w][0];
    h1 += cs->hits[w][1] + cs->base_hits[w][1];
    own_ref |= cs->hits[w][0] >= lit_min;
  }
  if (!own_ref && best_lo <= cs->base_lo && best_hi >= cs->base_hi)
    return false; /* that is the configured CORESET itself */
  int r = cs->base_ref;
  if (own_ref)
    for (int k = 0; k < 276; k++)
      if (cs->ref_votes[k] > cs->ref_votes[r])
        r = k;
  *start_rb = best_lo * 6;
  *n_rb = (best_hi - best_lo + 1) * 6;
  *duration = (h1 * 10 >= h0 * 3) ? 2 : 1; /* symbol 1 lit in >= 30 % of symbol-0 hits */
  *ref_rb = r;
  return true;
}
