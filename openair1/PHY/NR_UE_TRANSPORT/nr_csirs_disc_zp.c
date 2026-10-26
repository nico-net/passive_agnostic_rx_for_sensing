/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_csirs_disc_zp.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define NSYM 14
#define NPAR 2
#define NSC 12
#define CELLS (NSYM * NPAR * NSC)            /* (symbol, parity, subcarrier) */
#define DARK_RATIO 0.25f                     /* a class is dark below this fraction of the tile's 75th percentile class */
#define HALVE_AT 250                         /* residue observations at which its statistics are halved (forgetting) */
#define MIN_ON_SLOTS 8                       /* on-lattice slots observed before a lattice is tested */
#define MIN_ON_HITS 5
#define MIN_ON_RATE 0.15                     /* a ZP is visible only in slots with PDSCH around it */
#define MIN_Z 6.0
#define MIN_TILES_DARK 2                     /* a (cell, slot) is a hit when >= 2 tiles (8 RB) are dark: isolated flags are noise */
#define MAX_CAND 96

static const int periods[] = {4, 5, 8, 10, 16, 20, 32, 40, 64, 80, 160, 320, 640};
#define NPER ((int)(sizeof(periods) / sizeof(periods[0])))

struct nr_csirs_zp_det {
  int n_rb, tiles;
  size_t tile_cells;      /* NSYM * NPAR * tiles * NSC */
  uint64_t slots;
  uint16_t *obs;          /* [FOLD] */
  uint16_t *hit;          /* [FOLD][CELLS] slots in which >= MIN_TILES_DARK tiles of the cell were dark */
  uint8_t *tile;          /* [FOLD][tile_cells] dark flags per tile */
};

/* nr_csirs_zp_feat_len: floats per slot of a feature set. */
size_t nr_csirs_zp_feat_len(int n_rb)
{
  return (size_t)NSYM * NPAR * ((n_rb + NR_CSIRS_ZP_TILE_RB - 1) / NR_CSIRS_ZP_TILE_RB) * NSC;
}

/* nr_csirs_zp_features_cpu: the energy of each subcarrier class of each 4-RB tile, per symbol and RB parity, summed over the antennas.
 *   Reference of the GPU op (nr_csirs_disc_gpu.cu); the sums are in float, the GPU adds in another order, so the two agree to rounding. */
void nr_csirs_zp_features_cpu(const nr_csirs_disc_ring_t *r, int idx, float *out)
{
  const int tiles = (r->n_rb + NR_CSIRS_ZP_TILE_RB - 1) / NR_CSIRS_ZP_TILE_RB;
  memset(out, 0, nr_csirs_zp_feat_len(r->n_rb) * sizeof(*out));
  for (int l = 0; l < NSYM; ++l)
    for (int a = 0; a < r->n_ant; ++a) {
      const int16_t *v = nr_csirs_disc_ring_sym(r, idx, a, l);
      for (int rb = 0; rb < r->n_rb; ++rb) {
        float *dst = out + (((size_t)l * NPAR + (rb & 1)) * tiles + rb / NR_CSIRS_ZP_TILE_RB) * NSC;
        for (int sc = 0; sc < NSC; ++sc) {
          const int k = 12 * rb + sc;
          const float yr = v[2 * k], yi = v[2 * k + 1];
          dst[sc] += yr * yr + yi * yi;
        }
      }
    }
}

nr_csirs_zp_det_t *nr_csirs_zp_new(int n_rb)
{
  if (n_rb < 1 || n_rb > NR_CSIRS_DISC_MAX_RB)
    return NULL;
  nr_csirs_zp_det_t *d = calloc(1, sizeof(*d));
  if (!d)
    return NULL;
  d->n_rb = n_rb;
  d->tiles = (n_rb + NR_CSIRS_ZP_TILE_RB - 1) / NR_CSIRS_ZP_TILE_RB;
  d->tile_cells = (size_t)NSYM * NPAR * d->tiles * NSC;
  d->obs = calloc(NR_CSIRS_ZP_FOLD, sizeof(*d->obs));
  d->hit = calloc((size_t)NR_CSIRS_ZP_FOLD * CELLS, sizeof(*d->hit));
  d->tile = calloc((size_t)NR_CSIRS_ZP_FOLD * d->tile_cells, sizeof(*d->tile));
  if (!d->obs || !d->hit || !d->tile) {
    nr_csirs_zp_free(d);
    return NULL;
  }
  return d;
}

void nr_csirs_zp_free(nr_csirs_zp_det_t *d)
{
  if (!d)
    return;
  free(d->obs);
  free(d->hit);
  free(d->tile);
  free(d);
}

void nr_csirs_zp_reset(nr_csirs_zp_det_t *d)
{
  if (!d)
    return;
  d->slots = 0;
  memset(d->obs, 0, NR_CSIRS_ZP_FOLD * sizeof(*d->obs));
  memset(d->hit, 0, (size_t)NR_CSIRS_ZP_FOLD * CELLS * sizeof(*d->hit));
  memset(d->tile, 0, (size_t)NR_CSIRS_ZP_FOLD * d->tile_cells);
}

uint64_t nr_csirs_zp_slots(const nr_csirs_zp_det_t *d) { return d ? d->slots : 0; }

/* p75: the 75th percentile (10th smallest of 12) of a tile's subcarrier-class energies. */
static float p75(const float *e)
{
  float s[NSC];
  for (int i = 0; i < NSC; ++i) {
    float v = e[i];
    int j = i;
    while (j > 0 && s[j - 1] > v) {
      s[j] = s[j - 1];
      --j;
    }
    s[j] = v;
  }
  return s[9];
}

void nr_csirs_zp_feed(nr_csirs_zp_det_t *d, uint32_t abs_slot, const float *feat)
{
  if (!d || !feat)
    return;
  const int res = abs_slot % NR_CSIRS_ZP_FOLD;
  if (d->obs[res] >= HALVE_AT) {
    d->obs[res] /= 2;
    uint16_t *h = d->hit + (size_t)res * CELLS;
    for (int c = 0; c < CELLS; ++c)
      h[c] /= 2;
    uint8_t *t = d->tile + (size_t)res * d->tile_cells;
    for (size_t c = 0; c < d->tile_cells; ++c)
      t[c] /= 2;
  }
  d->obs[res]++;
  d->slots++;
  uint16_t *h = d->hit + (size_t)res * CELLS;
  uint8_t *t = d->tile + (size_t)res * d->tile_cells;
  for (int l = 0; l < NSYM; ++l)
    for (int par = 0; par < NPAR; ++par) {
      int dark[NSC] = {0};
      for (int tl = 0; tl < d->tiles; ++tl) {
        const size_t at = (((size_t)l * NPAR + par) * d->tiles + tl) * NSC;
        const float *e = feat + at;
        const float m = p75(e);
        if (!(m > 0.f))
          continue;
        for (int sc = 0; sc < NSC; ++sc)
          if (e[sc] < DARK_RATIO * m) {
            ++dark[sc];
            t[at + sc]++;
          }
      }
      for (int sc = 0; sc < NSC; ++sc)
        if (dark[sc] >= MIN_TILES_DARK)
          h[(l * NPAR + par) * NSC + sc]++;
    }
}

typedef struct {
  int l, par, p, o;
  unsigned sc_mask;
  double z, rate, score;
} cand_t;

static int by_score(const void *a, const void *b)
{
  const cand_t *x = a, *y = b;
  return x->score < y->score ? 1 : x->score > y->score ? -1 : 0;
}

/* intersect: true when the lattices (p1,o1) and (p2,o2) share a slot (they do iff the offsets agree modulo gcd(p1,p2)). A weaker candidate that
 * shares slots with a stronger one on the same symbol and subcarriers is the same hole seen through a coarser or shifted lattice. */
static bool intersect(int p1, int o1, int p2, int o2)
{
  int a = p1, b = p2;
  while (b) {
    const int t = a % b;
    a = b;
    b = t;
  }
  return (o1 - o2) % a == 0;
}

/* extent: RB extent of a candidate from the per-tile dark rates on its lattice against off it. Returns false when no tile is clearly dark. */
static bool extent(const nr_csirs_zp_det_t *d, const cand_t *c, const uint64_t *tile_total, uint64_t n_on, uint64_t n_off,
                   int *rb_start, int *n_rb)
{
  int nsc = __builtin_popcount(c->sc_mask);
  int first = -1, last = -1;
  for (int tl = 0; tl < d->tiles; ++tl) {
    uint64_t on = 0, all = 0;
    for (int sc = 0; sc < NSC; ++sc) {
      if (!((c->sc_mask >> sc) & 1))
        continue;
      const size_t at = (((size_t)c->l * NPAR + c->par) * d->tiles + tl) * NSC + sc;
      for (int r = c->o; r < NR_CSIRS_ZP_FOLD; r += c->p)
        on += d->tile[(size_t)r * d->tile_cells + at];
      all += tile_total[at];
    }
    const double r_on = (double)on / ((double)n_on * nsc);
    const double r_off = n_off ? (double)(all - on) / ((double)n_off * nsc) : 0;
    if (r_on >= 0.10 && r_on >= 3 * r_off + 0.05) {
      if (first < 0)
        first = tl;
      last = tl;
    }
  }
  if (first < 0)
    return false;
  *rb_start = first * NR_CSIRS_ZP_TILE_RB;
  *n_rb = (last + 1) * NR_CSIRS_ZP_TILE_RB > d->n_rb ? d->n_rb - *rb_start : (last + 1 - first) * NR_CSIRS_ZP_TILE_RB;
  return true;
}

int nr_csirs_zp_propose(nr_csirs_zp_det_t *d, nr_csirs_disc_fp_t *out, int max_out)
{
  if (!d || !out || max_out <= 0 || d->slots < 4 * MIN_ON_SLOTS)
    return 0;
  uint64_t n_tot = 0;
  uint64_t hit_tot[CELLS] = {0};
  for (int r = 0; r < NR_CSIRS_ZP_FOLD; ++r) {
    n_tot += d->obs[r];
    for (int c = 0; c < CELLS; ++c)
      hit_tot[c] += d->hit[(size_t)r * CELLS + c];
  }
  cand_t cand[MAX_CAND];
  int nc = 0;
  uint64_t *on_hit = malloc(sizeof(uint64_t) * 640 * CELLS);
  uint64_t *on_n = malloc(sizeof(uint64_t) * 640);
  if (!on_hit || !on_n) {
    free(on_hit);
    free(on_n);
    return 0;
  }
  for (int pi = 0; pi < NPER; ++pi) {
    const int p = periods[pi];
    memset(on_hit, 0, sizeof(uint64_t) * p * CELLS);
    memset(on_n, 0, sizeof(uint64_t) * p);
    for (int r = 0; r < NR_CSIRS_ZP_FOLD; ++r) {
      on_n[r % p] += d->obs[r];
      const uint16_t *h = d->hit + (size_t)r * CELLS;
      uint64_t *a = on_hit + (size_t)(r % p) * CELLS;
      for (int c = 0; c < CELLS; ++c)
        a[c] += h[c];
    }
    for (int o = 0; o < p; ++o) {
      const uint64_t n_on = on_n[o], n_off = n_tot - n_on;
      if (n_on < MIN_ON_SLOTS || n_off < 4 * MIN_ON_SLOTS)
        continue;
      for (int l = 0; l < NSYM; ++l)
        for (int par = 0; par < NPAR; ++par) {
          unsigned seed = 0, ext = 0;
          double zs[NSC], rs[NSC];
          for (int sc = 0; sc < NSC; ++sc) {
            const int c = (l * NPAR + par) * NSC + sc;
            const double s_on = (double)on_hit[(size_t)o * CELLS + c];
            const double q_off = ((double)(hit_tot[c] - (uint64_t)s_on) + 1.0) / ((double)n_off + 1.0);
            const double e = n_on * q_off;
            const double z = (s_on - e) / sqrt(e * (1 - q_off) + 1.0);
            const double rate = s_on / n_on;
            zs[sc] = z;
            rs[sc] = rate;
            if (s_on >= MIN_ON_HITS && rate >= MIN_ON_RATE && rate >= 4 * q_off + 0.05 && z >= MIN_Z)
              seed |= 1u << sc;
            else if (s_on >= MIN_ON_HITS && z >= 0.6 * MIN_Z && rate >= 4 * q_off + 0.05)
              ext |= 1u << sc;
          }
          if (!seed)
            continue;
          double zsum = 0, rsum = 0;
          int n = 0;
          unsigned mask = seed;
          double rmax = 0;
          for (int sc = 0; sc < NSC; ++sc)
            if ((seed >> sc) & 1 && rs[sc] > rmax)
              rmax = rs[sc];
          for (int sc = 0; sc < NSC; ++sc)
            if (((ext >> sc) & 1) && rs[sc] >= 0.5 * rmax)
              mask |= 1u << sc;
          for (int sc = 0; sc < NSC; ++sc)
            if ((mask >> sc) & 1) {
              zsum += zs[sc];
              rsum += rs[sc];
              ++n;
            }
          cand_t c = {.l = l, .par = par, .p = p, .o = o, .sc_mask = mask, .z = zsum / n, .rate = rsum / n};
          c.score = c.z * sqrt(c.rate);
          if (nc < MAX_CAND) {
            cand[nc++] = c;
          } else {
            int worst = 0;
            for (int i = 1; i < nc; ++i)
              if (cand[i].score < cand[worst].score)
                worst = i;
            if (c.score > cand[worst].score)
              cand[worst] = c;
          }
        }
    }
  }
  free(on_hit);
  free(on_n);
  qsort(cand, nc, sizeof(*cand), by_score);

  /* greedy selection: drop a candidate that shares slots with a stronger one of the same (symbol, parity) and subcarriers */
  cand_t sel[MAX_CAND];
  int ns = 0;
  for (int i = 0; i < nc; ++i) {
    bool dup = false;
    for (int j = 0; j < ns && !dup; ++j)
      dup = sel[j].l == cand[i].l && sel[j].par == cand[i].par && (sel[j].sc_mask & cand[i].sc_mask)
            && intersect(sel[j].p, sel[j].o, cand[i].p, cand[i].o);
    if (!dup)
      sel[ns++] = cand[i];
  }

  /* tile totals for the extent test */
  uint64_t *tile_total = calloc(d->tile_cells, sizeof(*tile_total));
  if (!tile_total)
    return 0;
  for (int r = 0; r < NR_CSIRS_ZP_FOLD; ++r) {
    const uint8_t *t = d->tile + (size_t)r * d->tile_cells;
    for (size_t k = 0; k < d->tile_cells; ++k)
      tile_total[k] += t[k];
  }

  typedef struct {
    int l, par, p, o, rb_start, n_rb;
    unsigned sc_mask;
    double z;
  } one_t;
  one_t one[MAX_CAND];
  int n1 = 0;
  for (int i = 0; i < ns; ++i) {
    uint64_t n_on = 0;
    for (int r = sel[i].o; r < NR_CSIRS_ZP_FOLD; r += sel[i].p)
      n_on += d->obs[r];
    int rb0, nrb;
    if (!extent(d, &sel[i], tile_total, n_on, n_tot - n_on, &rb0, &nrb))
      continue;
    one[n1++] = (one_t){sel[i].l, sel[i].par, sel[i].p, sel[i].o, rb0, nrb, sel[i].sc_mask, sel[i].z};
  }
  free(tile_total);

  /* parities with the same pattern are one every-RB (density one) resource */
  nr_csirs_disc_fp_t fps[MAX_CAND * 2];
  double fz[MAX_CAND * 2];
  int nf = 0;
  bool used[MAX_CAND] = {false};
  for (int i = 0; i < n1; ++i) {
    if (used[i])
      continue;
    used[i] = true;
    nr_csirs_disc_fp_t f = {.sym_mask = (uint16_t)(1u << one[i].l), .sc_mask = (uint16_t)one[i].sc_mask,
                            .density = (uint8_t)one[i].par, .rb_start = (uint16_t)one[i].rb_start,
                            .n_rb = (uint16_t)one[i].n_rb, .period = (uint16_t)one[i].p, .offset = (uint16_t)one[i].o,
                            .strength = (float)one[i].z};
    for (int j = i + 1; j < n1; ++j)
      if (!used[j] && one[j].l == one[i].l && one[j].par != one[i].par && one[j].p == one[i].p && one[j].o == one[i].o
          && one[j].sc_mask == one[i].sc_mask) {
        used[j] = true;
        const int lo = one[j].rb_start < one[i].rb_start ? one[j].rb_start : one[i].rb_start;
        const int hi = (one[j].rb_start + one[j].n_rb > one[i].rb_start + one[i].n_rb) ? one[j].rb_start + one[j].n_rb
                                                                                      : one[i].rb_start + one[i].n_rb;
        f.density = 2;
        f.rb_start = (uint16_t)lo;
        f.n_rb = (uint16_t)(hi - lo);
        f.strength = (float)fmin(one[i].z, one[j].z);
      }
    if (nf < MAX_CAND * 2) {
      fz[nf] = f.strength;
      fps[nf++] = f;
    }
  }
  /* multi-symbol footprints: the same lattice, pattern, density and extent on several symbols */
  const int nsingle = nf;
  for (int i = 0; i < nsingle; ++i) {
    nr_csirs_disc_fp_t u = fps[i];
    int members = 1;
    for (int j = 0; j < nsingle; ++j)
      if (j != i && !(fps[j].sym_mask & u.sym_mask) && fps[j].period == u.period && fps[j].offset == u.offset
          && fps[j].sc_mask == u.sc_mask && fps[j].density == u.density && fps[j].rb_start == u.rb_start
          && fps[j].n_rb == u.n_rb) {
        u.sym_mask |= fps[j].sym_mask;
        if (fps[j].strength < u.strength)
          u.strength = fps[j].strength;
        ++members;
      }
    bool dup = false;
    for (int k = nsingle; k < nf && !dup; ++k)
      dup = fps[k].sym_mask == u.sym_mask && fps[k].period == u.period && fps[k].offset == u.offset && fps[k].sc_mask == u.sc_mask
            && fps[k].density == u.density && fps[k].rb_start == u.rb_start && fps[k].n_rb == u.n_rb;
    if (members > 1 && !dup && nf < MAX_CAND * 2) {
      fz[nf] = u.strength;
      fps[nf++] = u;
    }
  }
  /* strongest first */
  for (int i = 1; i < nf; ++i) {
    nr_csirs_disc_fp_t f = fps[i];
    double z = fz[i];
    int j = i;
    while (j > 0 && fz[j - 1] < z) {
      fps[j] = fps[j - 1];
      fz[j] = fz[j - 1];
      --j;
    }
    fps[j] = f;
    fz[j] = z;
  }
  const int n = nf < max_out ? nf : max_out;
  memcpy(out, fps, sizeof(*out) * n);
  return n;
}
