#include "nr_pdcch_al1_map.h"
#include <stdlib.h>
#include <string.h>

#define AL1_MAX_CCE 135 /* 270 RB x 3 symbols / 6 */

static int shape_ok(int span_rb, int d)
{
  return span_rb > 0 && span_rb % 6 == 0 && d >= 1 && d <= 3 && span_rb * d / 6 <= AL1_MAX_CCE;
}

int nr_pdcch_al1_enumerate(int span_rb, int duration, nr_pdcch_al1_map_t *out, int max_out)
{
  if (!shape_ok(span_rb, duration) || out == NULL || max_out <= 0)
    return 0;
  int n = 0;
  out[n++] = (nr_pdcch_al1_map_t){0, 0, 0};
  const int N = span_rb * duration;
  const int Ls[2] = {duration == 3 ? 3 : 2, 6};
  static const int Rs[3] = {2, 3, 6};
  for (int li = 0; li < 2; li++) {
    const int L = Ls[li];
    if (L % duration)
      continue;
    const int nb = N / L;
    for (int ri = 0; ri < 3; ri++) {
      if (N % (L * Rs[ri]))
        continue;
      for (int s = 0; s < nb && n < max_out; s++)
        out[n++] = (nr_pdcch_al1_map_t){(uint8_t)L, (uint8_t)Rs[ri], (uint16_t)s};
    }
  }
  return n;
}

int nr_pdcch_al1_regset(int span_rb, int duration, nr_pdcch_al1_map_t m, int cce, uint16_t out[6])
{
  if (!shape_ok(span_rb, duration) || out == NULL)
    return 0;
  const int N = span_rb * duration;
  if (cce < 0 || cce >= N / 6)
    return 0;
  if (m.bundle == 0) {
    for (int k = 0; k < 6; k++)
      out[k] = (uint16_t)(6 * cce + k);
    return 6;
  }
  const int L = m.bundle, R = m.interleaver;
  if (L <= 0 || 6 % L || L % duration || R <= 0 || N % (L * R))
    return 0;
  const int nb = N / L, C = nb / R, per = 6 / L;
  if (m.shift >= nb)
    return 0;
  int o = 0;
  for (int k = 0; k < per; k++) {
    const int x = per * cce + k;
    const int f = ((x % R) * C + x / R + m.shift) % nb;
    for (int r = 0; r < L; r++)
      out[o++] = (uint16_t)(f * L + r);
  }
  for (int i = 1; i < 6; i++) { /* insertion sort: canonical order */
    const uint16_t v = out[i];
    int j = i - 1;
    while (j >= 0 && out[j] > v) { out[j + 1] = out[j]; j--; }
    out[j + 1] = v;
  }
  return 6;
}

/* A REG set as one 64-bit key: 6 sorted 10-bit indices (N_REG <= 810 < 1024). */
static uint64_t key6(const uint16_t r[6])
{
  uint64_t k = 0;
  for (int i = 0; i < 6; i++)
    k = (k << 10) | r[i];
  return k;
}

static int family_keys(int span_rb, int d, nr_pdcch_al1_map_t m, uint64_t *keys)
{
  const int ncce = span_rb * d / 6;
  uint16_t r[6];
  for (int j = 0; j < ncce; j++) {
    if (nr_pdcch_al1_regset(span_rb, d, m, j, r) != 6)
      return 0;
    keys[j] = key6(r);
  }
  return ncce;
}

/* Open-addressing set of keys -> dense index. Size is a power of two >= 4x the largest union (1080). */
#define HSZ 8192
typedef struct { uint64_t key[HSZ]; int idx[HSZ]; int n; } kset_t;
static int kset_add(kset_t *s, uint64_t k)
{
  for (uint32_t h = (uint32_t)((k * 0x9E3779B97F4A7C15ULL) >> 51) & (HSZ - 1);; h = (h + 1) & (HSZ - 1)) {
    if (s->idx[h] < 0) { s->key[h] = k; s->idx[h] = s->n; return s->n++; }
    if (s->key[h] == k) return s->idx[h];
  }
}

int nr_pdcch_al1_cover(int span_rb, int duration, nr_pdcch_al1_map_t *out, int max_out)
{
  if (!shape_ok(span_rb, duration) || out == NULL || max_out <= 0)
    return 0;
  nr_pdcch_al1_map_t *maps = malloc(sizeof(*maps) * NR_PDCCH_AL1_MAX_MAPS);
  const int ncce = span_rb * duration / 6;
  int *fam = malloc(sizeof(int) * (size_t)NR_PDCCH_AL1_MAX_MAPS * ncce); /* dense key index per (map, cce) */
  kset_t *set = malloc(sizeof(*set));
  uint8_t *covered = calloc(HSZ, 1);
  int nout = 0;
  if (maps && fam && set && covered) {
    memset(set->idx, 0xff, sizeof(set->idx));
    set->n = 0;
    const int nm = nr_pdcch_al1_enumerate(span_rb, duration, maps, NR_PDCCH_AL1_MAX_MAPS);
    uint64_t keys[AL1_MAX_CCE];
    for (int m = 0; m < nm; m++) {
      family_keys(span_rb, duration, maps[m], keys);
      for (int j = 0; j < ncce; j++)
        fam[m * ncce + j] = kset_add(set, keys[j]);
    }
    int left = set->n;
    while (left > 0 && nout < max_out) {
      int best = -1, best_gain = 0;
      for (int m = 0; m < nm; m++) {
        int gain = 0;
        for (int j = 0; j < ncce; j++)
          gain += !covered[fam[m * ncce + j]];
        if (gain > best_gain) { best_gain = gain; best = m; } /* strict: first maximum wins */
      }
      if (best < 0)
        break;
      for (int j = 0; j < ncce; j++)
        if (!covered[fam[best * ncce + j]]) { covered[fam[best * ncce + j]] = 1; left--; }
      out[nout++] = maps[best];
    }
  }
  free(maps); free(fam); free(set); free(covered);
  return nout;
}

int nr_pdcch_al1_narrow(int span_rb, int duration, const uint16_t (*obs)[6], int n_obs,
                        nr_pdcch_al1_map_t *cand, int n)
{
  if (!shape_ok(span_rb, duration) || cand == NULL || n <= 0 || n_obs <= 0 || obs == NULL)
    return n > 0 ? n : 0;
  uint64_t want[16];
  const int nw = n_obs < 16 ? n_obs : 16;
  for (int k = 0; k < nw; k++)
    want[k] = key6(obs[k]);
  uint64_t keys[AL1_MAX_CCE];
  int kept = 0;
  for (int i = 0; i < n; i++) {
    const int nk = family_keys(span_rb, duration, cand[i], keys);
    int all = nk > 0;
    for (int k = 0; k < nw && all; k++) {
      int hit = 0;
      for (int j = 0; j < nk && !hit; j++)
        hit = keys[j] == want[k];
      all = hit;
    }
    if (all)
      cand[kept++] = cand[i];
  }
  return kept;
}

static int cmp_u64(const void *a, const void *b)
{
  const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
  return (x > y) - (x < y);
}

/* FNV-1a over the sorted family = the family's fingerprint. */
static uint64_t family_fp(int span_rb, int d, nr_pdcch_al1_map_t m)
{
  uint64_t keys[AL1_MAX_CCE];
  const int nk = family_keys(span_rb, d, m, keys);
  qsort(keys, (size_t)nk, sizeof(keys[0]), cmp_u64);
  uint64_t h = 1469598103934665603ULL;
  for (int j = 0; j < nk; j++)
    h = (h ^ keys[j]) * 1099511628211ULL;
  return h;
}

int nr_pdcch_al1_family_count(int span_rb, int duration, const nr_pdcch_al1_map_t *cand, int n)
{
  if (!shape_ok(span_rb, duration) || cand == NULL || n <= 0)
    return 0;
  uint64_t *fp = malloc(sizeof(uint64_t) * (size_t)n);
  if (fp == NULL)
    return 0;
  for (int i = 0; i < n; i++)
    fp[i] = family_fp(span_rb, duration, cand[i]);
  qsort(fp, (size_t)n, sizeof(fp[0]), cmp_u64);
  int d = 1;
  for (int i = 1; i < n; i++)
    d += fp[i] != fp[i - 1];
  free(fp);
  return d;
}

int nr_pdcch_al1_family_reps(int span_rb, int duration, nr_pdcch_al1_map_t *cand, int n)
{
  if (!shape_ok(span_rb, duration) || cand == NULL || n <= 0)
    return 0;
  uint64_t *fp = malloc(sizeof(uint64_t) * (size_t)n);
  if (fp == NULL)
    return 0;
  int kept = 0; /* ponytail: O(n * kept) duplicate test; one-shot per verification, n <= 1081 */
  for (int i = 0; i < n; i++) {
    const uint64_t h = family_fp(span_rb, duration, cand[i]);
    int dup = 0;
    for (int k = 0; k < kept && !dup; k++)
      dup = fp[k] == h;
    if (!dup) {
      fp[kept] = h;
      cand[kept++] = cand[i];
    }
  }
  free(fp);
  return kept;
}

int nr_pdcch_al1_union(int span_rb, int duration, const nr_pdcch_al1_map_t *cand, int n, uint16_t (*regsets)[6], int max)
{
  if (!shape_ok(span_rb, duration) || cand == NULL || n <= 0)
    return 0;
  kset_t *set = malloc(sizeof(*set));
  if (set == NULL)
    return 0;
  memset(set->idx, 0xff, sizeof(set->idx));
  set->n = 0;
  const int ncce = span_rb * duration / 6;
  for (int i = 0; i < n; i++)
    for (int j = 0; j < ncce; j++) {
      uint16_t r[6];
      if (nr_pdcch_al1_regset(span_rb, duration, cand[i], j, r) != 6)
        continue;
      const int prev = set->n;
      if (kset_add(set, key6(r)) == prev && prev < max && regsets != NULL)
        memcpy(regsets[prev], r, sizeof(r));
    }
  const int total = set->n; /* <= 1080 distinct sets for any legal shape: the table never fills */
  free(set);
  return total;
}

int nr_pdcch_al1_demap(int duration, const uint16_t regs[6], const void *llr, int llr_stride, void *e_rx)
{
  if (duration < 1 || duration > 3 || regs == NULL || llr == NULL || e_rx == NULL)
    return 0;
  int rb[6], nrb = 0;
  for (int k = 0; k < 6; k++) { /* regs ascending -> RBs ascending; each RB holds `duration` REGs */
    const int b = regs[k] / duration;
    if (nrb == 0 || rb[nrb - 1] != b)
      rb[nrb++] = b;
  }
  if (nrb * duration != 6)
    return 0;
  const size_t re = 4, per_rb = 9; /* sizeof(c16_t), data REs per RB outside the DM-RS */
  char *o = e_rx;
  for (int s = 0; s < duration; s++)
    for (int i = 0; i < nrb; i++, o += per_rb * re)
      memcpy(o, (const char *)llr + ((size_t)s * llr_stride + (size_t)rb[i] * per_rb) * re, per_rb * re);
  return 54;
}
