/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Blind DM-RS scrambling-identity recovery by GF(2) solving of the Gold sequence state. See nr_dmrs_nid_solve.h for the method. */
#include "nr_dmrs_nid_solve.h"
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define NC 1600
#define NSEQ (2 * NR_NID_SOLVE_MAX_PILOT + 2) /* c(n) indices needed */
#define NSTREAM (NSEQ + NC + 40)

/* x2(j) as a mask over the 31 bits of c_init, and x1(j) as a bit, for j in [0, NSTREAM). */
static uint32_t s_x2mask[NSTREAM];
static uint8_t s_x1[NSTREAM];
static pthread_once_t s_once = PTHREAD_ONCE_INIT;

static void tables_init(void)
{
  for (int j = 0; j < 31 && j < NSTREAM; j++)
    s_x2mask[j] = 1u << j;
  for (int j = 31; j < NSTREAM; j++) /* x2(n+31) = x2(n+3) + x2(n+2) + x2(n+1) + x2(n) */
    s_x2mask[j] = s_x2mask[j - 28] ^ s_x2mask[j - 29] ^ s_x2mask[j - 30] ^ s_x2mask[j - 31];
  memset(s_x1, 0, sizeof(s_x1));
  s_x1[0] = 1;
  for (int j = 31; j < NSTREAM; j++) /* x1(n+31) = x1(n+3) + x1(n) */
    s_x1[j] = s_x1[j - 28] ^ s_x1[j - 31];
}

int nr_dmrs_nid_predict_bits(uint32_t cinit, int m)
{
  pthread_once(&s_once, tables_init);
  if (m < 0 || 2 * m + 1 >= NSEQ)
    return 0;
  const int b0 = s_x1[2 * m + NC] ^ (__builtin_parity(s_x2mask[2 * m + NC] & cinit) & 1);
  const int b1 = s_x1[2 * m + 1 + NC] ^ (__builtin_parity(s_x2mask[2 * m + 1 + NC] & cinit) & 1);
  return b0 | (b1 << 1);
}

uint32_t nr_dmrs_nid_cinit(int nid, int slot, int symbol, int symbols_per_slot, int nscid)
{
  const uint64_t a = (uint64_t)symbols_per_slot * (uint64_t)slot + (uint64_t)symbol + 1;
  uint64_t v = (a << 17) * ((uint64_t)nid * 2 + 1) + (uint64_t)nid * 2;
  if (nscid > 0)
    v += (uint64_t)nscid;
  return (uint32_t)(v & 0x7fffffffu);
}

/* N_ID from c_init, or -1 when c_init is not the image of any N_ID in [0, 65535]. */
static int nid_from_cinit(uint32_t cinit, int slot, int symbol, int symbols_per_slot, int nscid)
{
  const uint32_t mod31 = 0x7fffffffu;
  const uint64_t a = (uint64_t)symbols_per_slot * (uint64_t)slot + (uint64_t)symbol + 1;
  const uint32_t k = (uint32_t)((1 + (a << 17)) & mod31); /* odd */
  uint32_t val = (cinit - (uint32_t)((a << 17) & mod31) - (uint32_t)(nscid > 0 ? nscid : 0)) & mod31;
  if (val & 1)
    return -1;
  const uint32_t half = val >> 1; /* mod 2^30 */
  uint32_t inv = k; /* Newton iteration for k^-1 mod 2^32: each step doubles the correct bits (k is its own inverse mod 8) */
  for (int i = 0; i < 5; i++)
    inv *= 2 - k * inv;
  const uint32_t nid = (half * inv) & 0x3fffffffu;
  if (nid >= 65536u)
    return -1;
  return nr_dmrs_nid_cinit((int)nid, slot, symbol, symbols_per_slot, nscid) == cinit ? (int)nid : -1;
}

typedef struct {
  uint32_t row; /* mask over c_init bits */
  uint8_t rhs;  /* hard bit xor a(n) */
  float rel;    /* reliability of the decision */
} eq_t;

static uint32_t xs_next(uint32_t *s)
{
  uint32_t x = *s;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  return *s = x;
}

static int cmp_eq_desc(const void *a, const void *b)
{
  const float ra = ((const eq_t *)a)->rel, rb = ((const eq_t *)b)->rel;
  return (ra < rb) - (ra > rb);
}

/* Rearrange a[0..n) so that a[0..k) hold the k most reliable equations (unordered): quickselect, O(n) against O(n log n) for a full sort.
 * Only the leading ~40 equations of a reliability ordering ever enter the solve, so ordering the other ~1600 is wasted work. */
static void select_top(eq_t *a, int n, int k)
{
  int lo = 0, hi = n - 1;
  while (lo < hi) {
    const float pv = a[(lo + hi) / 2].rel;
    int i = lo, j = hi;
    while (i <= j) {
      while (a[i].rel > pv) i++;
      while (a[j].rel < pv) j--;
      if (i <= j) { const eq_t t = a[i]; a[i] = a[j]; a[j] = t; i++; j--; }
    }
    if (k - 1 <= j) hi = j;
    else if (k - 1 >= i) lo = i;
    else break;
  }
}

/* Solve from the first 31 independent equations in the given (reliability) order. Returns true and the state in *x. */
static bool solve_ordered(const eq_t *e, int n, uint32_t *x)
{
  uint32_t basis[31];
  uint8_t brhs[31];
  uint8_t have = 0;
  uint32_t used = 0;
  memset(basis, 0, sizeof(basis));
  for (int i = 0; i < n && have < 31; i++) {
    uint32_t r = e[i].row;
    uint8_t b = e[i].rhs;
    for (int p = 0; p < 31 && r; p++) {
      if (!(r >> p & 1))
        continue;
      if (used >> p & 1) {
        r ^= basis[p];
        b ^= brhs[p];
      } else {
        basis[p] = r;
        brhs[p] = b;
        used |= 1u << p;
        have++;
        r = 0;
        b = 0;
        break;
      }
    }
  }
  if (have < 31)
    return false;
  /* back-substitution: basis[p] has its lowest set bit... reduce in descending pivot order so every row only references itself */
  uint32_t sol = 0;
  for (int p = 30; p >= 0; p--) {
    /* row p: bit p plus bits that were already eliminated (pivots < p were chosen as lowest set bits at insertion time) */
    uint32_t r = basis[p];
    uint8_t b = brhs[p];
    uint32_t others = r & ~(1u << p);
    /* every other set bit q in `others` has q > p (a row is reduced against lower pivots first, so its remaining bits are higher) */
    b ^= (uint8_t)(__builtin_parity(others & sol) & 1);
    if (b)
      sol |= 1u << p;
  }
  *x = sol;
  return true;
}

/* Rank-deficient case: the reliable equations (rel >= bar) span r < 31 dimensions of c_init (two RB-aligned bundles span as little as 24).
 * Solve what they determine, then ENUMERATE the 2^(31-r) completions of the free bits and keep the candidates that are a legal N_ID
 * (probability 2^-15 each), so f <= 12 free bits give at most ~0.1 false candidates. Returns the number of candidates written. */
static int solve_nullspace(const eq_t *sorted, int ne, float bar, int slot, int symbol, int sps, int nscid, uint32_t *cands, int max_c,
                           int *checks_out, int *mism_out)
{
  uint32_t basis[31];
  uint8_t brhs[31];
  uint32_t used = 0;
  int r = 0, checks = 0, mism = 0;
  memset(basis, 0, sizeof(basis));
  for (int i = 0; i < ne && sorted[i].rel >= bar && i < 400; i++) {
    uint32_t v = sorted[i].row;
    uint8_t b = sorted[i].rhs;
    for (int p = 0; p < 31 && v; p++) {
      if (!(v >> p & 1))
        continue;
      if (used >> p & 1) {
        v ^= basis[p];
        b ^= brhs[p];
      } else {
        basis[p] = v;
        brhs[p] = b;
        used |= 1u << p;
        r++;
        v = 0;
        b = 0;
        break;
      }
    }
    if (!v && r > 0) {
      /* reduced to zero: consistent (b == 0) or conflicting (b == 1) with the stronger equations */
    }
    /* a row that reduced to zero without becoming a pivot is a check; recompute cheaply */
  }
  /* recount checks/conflicts: re-run reduction against the final basis (rows beyond the pivots) */
  for (int i = 0; i < ne && sorted[i].rel >= bar && i < 400; i++) {
    uint32_t v = sorted[i].row;
    uint8_t b = sorted[i].rhs;
    for (int p = 0; p < 31 && v; p++)
      if ((v >> p & 1) && (used >> p & 1)) {
        v ^= basis[p];
        b ^= brhs[p];
      }
    if (!v) {
      checks++;
      mism += b;
    }
  }
  *checks_out = checks;
  *mism_out = mism;
  const int f = 31 - r;
  if (r < 12 || f < 1 || f > 12 || mism > 1)
    return 0;
  /* particular solution with all free bits 0, then the null-space basis */
  uint32_t x0 = 0;
  for (int p = 30; p >= 0; p--)
    if (used >> p & 1) {
      const uint32_t others = basis[p] & ~(1u << p);
      if ((brhs[p] ^ (uint8_t)(__builtin_parity(others & x0) & 1)) & 1)
        x0 |= 1u << p;
    }
  uint32_t nv[12];
  int nf = 0;
  for (int q = 0; q < 31 && nf < f; q++) {
    if (used >> q & 1)
      continue;
    uint32_t v = 1u << q; /* free bit q set, other free bits 0 */
    for (int p = 30; p >= 0; p--)
      if (used >> p & 1) {
        const uint32_t others = basis[p] & ~(1u << p);
        if (__builtin_parity(others & v) & 1)
          v |= 1u << p;
      }
    nv[nf++] = v;
  }
  int nc = 0;
  for (uint32_t mask = 0; mask < (1u << nf) && nc < max_c; mask++) {
    uint32_t x = x0;
    for (int k = 0; k < nf; k++)
      if (mask >> k & 1)
        x ^= nv[k];
    if (nid_from_cinit(x, slot, symbol, sps, nscid) >= 0)
      cands[nc++] = x;
  }
  return nc;
}

int nr_dmrs_nid_solve(const float *yr, const float *yi, const int *m, int n, int slot, int symbol, int symbols_per_slot, int nscid,
                      nr_nid_solution_t *out, int max_out)
{
  if (n < 16 || n > NR_NID_SOLVE_MAX_PILOT || max_out < 1 || !yr || !yi || !m)
    return 0;
  pthread_once(&s_once, tables_init);
  for (int i = 0; i < n; i++)
    if (m[i] < 0 || 2 * m[i] + 1 >= NSEQ)
      return 0;

  /* Degenerate input: an all-zero (or mostly exactly-zero) window decides every bit as 0 and "solves" to a valid identity,
   * deterministically (noiseless simulation leaves unused REs at exactly 0). No information, no answer. */
  {
    int zeros = 0;
    double en = 0.0;
    for (int i = 0; i < n; i++) {
      en += (double)yr[i] * yr[i] + (double)yi[i] * yi[i];
      zeros += (yr[i] == 0.0f && yi[i] == 0.0f);
    }
    if (en <= 0.0 || zeros * 2 > n)
      return 0;
  }
  /* rotation estimate mod 90 degrees from the 4th power: every QPSK point r has r^4 = -1, so arg(sum y^4) = 4 arg(h) + pi */
  double zr = 0.0, zi = 0.0;
  for (int i = 0; i < n; i++) {
    const double a = yr[i], b = yi[i];
    const double r2 = a * a - b * b, i2 = 2 * a * b; /* y^2 */
    zr += r2 * r2 - i2 * i2;
    zi += 2 * r2 * i2; /* y^4 */
  }
  const double phi0 = (atan2(zi, zr) - M_PI) / 4.0;

  nr_nid_solution_t found[4 * 8];
  int nfound = 0;
  eq_t eq[2 * NR_NID_SOLVE_MAX_PILOT];
  const int ne = 2 * n;
  for (int rot = 0; rot < 4; rot++) {
    const double phi = phi0 + rot * (M_PI / 2.0);
    const double cr = cos(-phi), sr = sin(-phi);
    for (int i = 0; i < n; i++) {
      const double dr = yr[i] * cr - yi[i] * sr, di = yr[i] * sr + yi[i] * cr; /* y * e^{-j phi} */
      const int c0 = 2 * m[i], c1 = 2 * m[i] + 1;
      eq[2 * i].row = s_x2mask[c0 + NC];
      eq[2 * i].rhs = (uint8_t)((dr < 0) ^ s_x1[c0 + NC]);
      eq[2 * i].rel = (float)fabs(dr);
      eq[2 * i + 1].row = s_x2mask[c1 + NC];
      eq[2 * i + 1].rhs = (uint8_t)((di < 0) ^ s_x1[c1 + NC]);
      eq[2 * i + 1].rel = (float)fabs(di);
    }
    uint32_t seed = 0x9e3779b9u ^ (uint32_t)(rot * 2654435761u) ^ (uint32_t)n;
    eq_t work[2 * NR_NID_SOLVE_MAX_PILOT];
    eq_t top[2 * NR_NID_SOLVE_MAX_PILOT];
    /* The K most reliable equations, ordered, fixed per rotation; the trials below only perturb and re-order these. K = 384 keeps the
     * rank of any real system (a CCE group gives >= 16 pilots, noise rows are full rank within a few dozen). */
    const int K = ne < 384 ? ne : 384;
    memcpy(top, eq, sizeof(eq_t) * (size_t)ne);
    select_top(top, ne, K);
    qsort(top, (size_t)K, sizeof(eq_t), cmp_eq_desc);
    float med = 0.0f; /* median reliability of ALL equations = the (ne/2)-th largest */
    {
      memcpy(work, eq, sizeof(eq_t) * (size_t)ne);
      select_top(work, ne, ne / 2 + 1);
      med = work[0].rel;
      for (int i = 1; i <= ne / 2; i++)
        if (work[i].rel < med) med = work[i].rel;
    }
    /* Extra checks: the equations ranked after the 31 used to solve that are still at least half as reliable as the 31st. A fixed count
     * (48) pulled noise equations into the check whenever fewer reliable ones existed (a DCI split over two bundles gives 36). */
    int ntop = 31;
    if (ne > 31) {
      /* A check must be clearly above the noise tail, not just comparable to the solving equations: the strongest noise equations of a
       * whole-carrier system reach ~5x the median reliability. When everything is signal the median is high, so the floor is capped at
       * 90 % of the 31st equation. */
      const float floor_n = 5.0f * med < 0.9f * top[30].rel ? 5.0f * med : 0.9f * top[30].rel;
      const float thr = 0.5f * top[30].rel > floor_n ? 0.5f * top[30].rel : floor_n;
      while (ntop < K && ntop < 31 + 40 && top[ntop].rel >= thr)
        ntop++;
    }
    /* Randomised information sets only help when there are spare equations to choose among: a 36-equation window system has 5. */
    const int ntrials = ne >= 31 + 40 ? 8 : 2;
    for (int trial = 0; trial < ntrials; trial++) {
      memcpy(work, top, sizeof(eq_t) * (size_t)K);
      if (trial > 0) { /* randomised information sets: perturb the reliabilities multiplicatively before ordering */
        for (int i = 0; i < K; i++)
          work[i].rel *= 0.5f + (float)(xs_next(&seed) & 0xffff) / 65536.0f;
        qsort(work, (size_t)K, sizeof(eq_t), cmp_eq_desc);
      }
      uint32_t x;
      if (!solve_ordered(work, K, &x))
        continue;
      const int nid = nid_from_cinit(x, slot, symbol, symbols_per_slot, nscid);
      if (nid < 0)
        continue;
      /* parity checks on the ORIGINAL equations, weighted by reliability */
      int mism = 0;
      double agree = 0.0, tot = 0.0;
      for (int i = 0; i < ne; i++) {
        const int pred = __builtin_parity(eq[i].row & x) & 1;
        const bool ok = pred == eq[i].rhs;
        mism += !ok;
        agree += ok ? eq[i].rel : -eq[i].rel;
        tot += eq[i].rel;
      }
      bool dup = false;
      for (int f = 0; f < nfound; f++)
        dup |= found[f].nid == nid && found[f].rot == rot;
      if (dup || nfound >= 32)
        continue;
      found[nfound].nid = nid;
      found[nfound].rot = rot;
      found[nfound].n_checks = ne - 31;
      found[nfound].mismatches = mism;
      found[nfound].score = tot > 0 ? agree / tot : 0.0;
      int tm = 0;
      for (int i = 0; i < ntop; i++)
        tm += (__builtin_parity(top[i].row & x) & 1) != top[i].rhs;
      found[nfound].top_checks = ntop > 31 ? ntop - 31 : 0;
      found[nfound].top_mismatches = tm;
      found[nfound].cinit = x;
      found[nfound].phi = phi;
      nfound++;
    }
    /* path 2: reliable equations that do not span all 31 dimensions */
    {
      const float bar = 6.0f * med;
      if (ne > 31 && bar < top[0].rel * 0.9f) {
        uint32_t cands[64];
        int chk = 0, mm = 0;
        const int nc = solve_nullspace(top, K, bar, slot, symbol, symbols_per_slot, nscid, cands, 64, &chk, &mm);
        for (int c2 = 0; c2 < nc; c2++) {
          const int nid = nid_from_cinit(cands[c2], slot, symbol, symbols_per_slot, nscid);
          bool dup = false;
          for (int f = 0; f < nfound; f++)
            dup |= found[f].nid == nid && found[f].rot == rot;
          if (dup || nfound >= 32)
            continue;
          found[nfound].nid = nid;
          found[nfound].rot = rot;
          found[nfound].n_checks = chk;
          found[nfound].mismatches = mm;
          found[nfound].score = chk > 0 ? 1.0 - 2.0 * mm / chk : 0.0;
          found[nfound].top_checks = chk;
          found[nfound].top_mismatches = mm;
          found[nfound].cinit = cands[c2];
          found[nfound].phi = phi;
          nfound++;
        }
      }
    }
  }
  /* best score first, distinct N_ID */
  int nout = 0;
  for (int pass = 0; pass < nfound && nout < max_out; pass++) {
    int best = -1;
    for (int f = 0; f < nfound; f++) {
      if (found[f].nid < 0)
        continue;
      if (best < 0 || found[f].score > found[best].score)
        best = f;
    }
    if (best < 0)
      break;
    out[nout++] = found[best];
    const int nid = found[best].nid;
    for (int f = 0; f < nfound; f++)
      if (found[f].nid == nid)
        found[f].nid = -1;
  }
  return nout;
}
