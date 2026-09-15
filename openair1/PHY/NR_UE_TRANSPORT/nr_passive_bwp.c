/* Passive DL BWP tracking -- see nr_passive_bwp.h. */
#include "nr_passive_bwp.h"

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

uint16_t nr_pbwp_len_for_size(const nr_pbwp_t *t, uint16_t n)
{
  return (uint16_t)(t->base_len - nr_pbwp_riv_bits(t->base_size) + nr_pbwp_riv_bits(n));
}

static bool len_registered(const nr_pbwp_t *t, uint16_t len)
{
  for (int i = 0; i < t->n; i++)
    if (t->e[i].dci_len == len)
      return true;
  return false;
}

void nr_pbwp_init(nr_pbwp_t *t, uint16_t carrier_rbs, uint16_t base_start, uint16_t base_size,
                  uint16_t base_len)
{
  nr_pbwp_free(t);
  memset(t, 0, sizeof(*t));
  t->carrier_rbs = carrier_rbs;
  t->base_len = base_len;
  t->base_size = base_size;
  t->e[0] = (nr_pbwp_entry_t){.dci_len = base_len, .size_lo = base_size, .size_hi = base_size,
                              .start = (int16_t)base_start};
  t->n = 1;
  /* One candidate length per distinct RIV width reachable by a BWP of >= NR_PBWP_MIN_SIZE PRBs. */
  for (uint16_t n = NR_PBWP_MIN_SIZE; n <= carrier_rbs && t->n_cand < 16; n++) {
    const uint16_t len = nr_pbwp_len_for_size(t, n);
    bool have = len == base_len;
    for (int k = 0; k < t->n_cand && !have; k++)
      have = t->cand_len[k] == len;
    if (!have)
      t->cand_len[t->n_cand++] = len;
  }
}

void nr_pbwp_free(nr_pbwp_t *t)
{
  for (int i = 0; i < NR_PBWP_MAX; i++) {
    free(t->e[i].score);
    t->e[i].score = NULL;
  }
}

bool nr_pbwp_resolved(const nr_pbwp_t *t, int idx)
{
  return idx >= 0 && idx < t->n && t->e[idx].start >= 0 && t->e[idx].size_lo == t->e[idx].size_hi;
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
    t->rnti_seen[rnti >> 3] |= (uint8_t)(1u << (rnti & 7));
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

int nr_pbwp_probe_accept(nr_pbwp_t *t, uint16_t rnti, uint16_t len)
{
  if (!(t->rnti_seen[rnti >> 3] & (1u << (rnti & 7))) || len_registered(t, len) || t->n >= NR_PBWP_MAX)
    return -1;
  int k = 0;
  while (k < t->n_cand && t->cand_len[k] != len)
    k++;
  if (k == t->n_cand || ++t->cand_hits[k] < NR_PBWP_NEW_HITS)
    return -1;
  uint16_t lo = 0, hi = 0;
  for (uint16_t n = NR_PBWP_MIN_SIZE; n <= t->carrier_rbs; n++)
    if (nr_pbwp_len_for_size(t, n) == len) {
      if (lo == 0)
        lo = n;
      hi = n;
    }
  if (lo == 0 || hi - lo >= 128)
    return -1;
  nr_pbwp_entry_t *e = &t->e[t->n];
  *e = (nr_pbwp_entry_t){.dci_len = len, .size_lo = lo, .size_hi = hi, .start = -1};
  e->n_starts = (uint16_t)(t->carrier_rbs - lo + 1);
  e->score = calloc((size_t)(hi - lo + 1) * e->n_starts, sizeof(float));
  if (e->score == NULL)
    return -1;
  t->rnti_bwp[rnti] = (uint8_t)(t->n + 1);
  return t->n++;
}

static int cmp_float(const void *a, const void *b)
{
  const float x = *(const float *)a, y = *(const float *)b;
  return (x > y) - (x < y);
}

static bool size_dead(const nr_pbwp_entry_t *e, int k)
{
  return k < 64 ? (e->dead_size_lo >> k) & 1u : (e->dead_size_hi >> (k - 64)) & 1u;
}

bool nr_pbwp_score_grant(nr_pbwp_t *t, int idx, uint32_t riv, const float *prb_coh)
{
  if (idx <= 0 || idx >= t->n || t->e[idx].score == NULL)
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
  const int nsz = e->size_hi - e->size_lo + 1;
  for (int k = 0; k < nsz; k++) {
    if (size_dead(e, k))
      continue;
    const uint16_t n = (uint16_t)(e->size_lo + k);
    uint16_t s0, l;
    if (!nr_pbwp_riv_decode(riv, n, &s0, &l)) {
      /* the true size never produces an impossible RIV */
      if (k < 64) e->dead_size_lo |= 1ull << k; else e->dead_size_hi |= 1ull << (k - 64);
      continue;
    }
    float *row = &e->score[(size_t)k * e->n_starts];
    for (int s = 0; s + n <= C; s++) {
      const int a = s + s0, b = a + l;
      row[s] += (float)(pre[b] - pre[a]);
    }
  }
  if (++e->grants_scored < NR_PBWP_MIN_GRANTS)
    return false;
  float best = -1e30f, second = -1e30f;
  int bk = -1, bs = -1;
  for (int k = 0; k < nsz; k++) {
    if (size_dead(e, k))
      continue;
    const int n = e->size_lo + k;
    const float *row = &e->score[(size_t)k * e->n_starts];
    for (int s = 0; s + n <= C; s++) {
      if (row[s] > best) { second = best; best = row[s]; bk = k; bs = s; }
      else if (row[s] > second) second = row[s];
    }
  }
  if (bk < 0 || best - second < NR_PBWP_MARGIN)
    return false;
  e->size_lo = e->size_hi = (uint16_t)(e->size_lo + bk);
  e->start = (int16_t)bs;
  free(e->score);
  e->score = NULL;
  return true;
}

void nr_pbwp_feed_crc(nr_pbwp_t *t, int idx, bool ok)
{
  if (!nr_pbwp_resolved(t, idx) || idx == 0)
    return;
  nr_pbwp_entry_t *e = &t->e[idx];
  e->crc_try++;
  e->crc_ok += ok;
  if (e->crc_try >= 32 && e->crc_ok == 0) {
    /* wrong hypothesis: rescore from scratch over the length's full size range */
    const uint16_t len = e->dci_len;
    uint16_t lo = 0, hi = 0;
    for (uint16_t n = NR_PBWP_MIN_SIZE; n <= t->carrier_rbs; n++)
      if (nr_pbwp_len_for_size(t, n) == len) { if (lo == 0) lo = n; hi = n; }
    *e = (nr_pbwp_entry_t){.dci_len = len, .size_lo = lo, .size_hi = hi, .start = -1};
    e->n_starts = (uint16_t)(t->carrier_rbs - lo + 1);
    e->score = calloc((size_t)(hi - lo + 1) * e->n_starts, sizeof(float));
  }
}

uint32_t nr_pbwp_translate_riv(uint32_t value, uint8_t cur_bits, uint8_t tgt_bits)
{
  (void)cur_bits; /* fewer current bits than needed: the value already reads as zero-prepended */
  return tgt_bits >= 32 ? value : value & ((1u << tgt_bits) - 1u);
}

int nr_pbwp_indicator(nr_pbwp_t *t, uint16_t rnti, int idx, uint8_t ind_value)
{
  (void)rnti;
  if (idx < 0 || idx >= t->n || ind_value > 3)
    return idx;
  const int bound = (int)t->ind_map[ind_value] - 1;
  if (bound >= 0 && bound != idx && nr_pbwp_resolved(t, bound))
    return bound; /* switch grant: allocation is in the indicated BWP */
  if (bound < 0 && ++t->ind_votes[ind_value][idx] >= NR_PBWP_IND_LEARN)
    t->ind_map[ind_value] = (uint8_t)(idx + 1);
  return idx;
}
