#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "nr_pdcch_joint_solve.h"
#include "openair1/PHY/gold.h"

#define JOINT_PFA 1e-4 /* design false-alarm probability per solve; the acceptance bar is derived from it */
#define EW ((NR_PDCCH_JOINT_MAX_E + 63) / 64)
#define JMAX_K 96 /* A <= 64 payload + 16 RNTI + 16 n_ID */

typedef struct { uint64_t w[2]; } v128;              /* a vector over the K <= 80 unknowns */
typedef struct { uint64_t w[EW]; } ebits;            /* a vector over the E coded bits */

static inline int v_get(v128 a, int j) { return (int)((a.w[j >> 6] >> (j & 63)) & 1u); }
static inline void v_set(v128 *a, int j) { a->w[j >> 6] |= 1ULL << (j & 63); }
static inline v128 v_xor(v128 a, v128 b) { v128 r = {{a.w[0] ^ b.w[0], a.w[1] ^ b.w[1]}}; return r; }
static inline int v_par(v128 a, v128 b)
{
  return (__builtin_popcountll(a.w[0] & b.w[0]) + __builtin_popcountll(a.w[1] & b.w[1])) & 1;
}
static inline int v_zero(v128 a) { return (a.w[0] | a.w[1]) == 0; }
static inline int v_lowbit(v128 a) { return a.w[0] ? __builtin_ctzll(a.w[0]) : 64 + __builtin_ctzll(a.w[1]); }

struct nr_pdcch_joint_model {
  int A, E, K, swr, pre, unk; /* pre: n_RNTI the input LLRs were already descrambled with, -1 = none */
  uint16_t nid;
  v128 *rows;    /* rows[i] = which unknowns coded bit i depends on */
  uint8_t *base; /* scrambled coded bits for the all-zero unknowns */
  nr_pdcch_joint_encode_fn enc;
  void *ctx;
};

/* Consume ALL 32 bits of each gold_generic() word before advancing, exactly like every real caller. */
static void gold_seq(uint32_t c_init, int n_bits, uint8_t *out)
{
  uint32_t x1 = 0, x2 = c_init;
  uint32_t word = gold_generic(&x1, &x2, 1);
  int produced = 0;
  for (;;) {
    const int take = (n_bits - produced) < 32 ? (n_bits - produced) : 32;
    for (int k = 0; k < take; k++)
      out[produced + k] = (uint8_t)((word >> k) & 1);
    produced += take;
    if (produced >= n_bits)
      break;
    word = gold_generic(&x1, &x2, 0);
  }
}

/* The joint forward map for ONE (payload, rnti): real encoder, then the spec scrambling
 * (TS 38.211 7.3.2.3: c_init = (n_RNTI << 16 + n_ID) mod 2^31, as dci_nr.c:1073 does on the UE side). */
static int joint_eval(const nr_pdcch_joint_model_t *m, uint64_t payload, uint16_t rnti, uint16_t nid, uint8_t *out)
{
  uint8_t seq[NR_PDCCH_JOINT_MAX_E];
  if (m->enc(m->ctx, nr_dci_bits_from_u64(payload), rnti, out, m->E) != 0)
    return -1;
  const uint32_t nrnti = m->swr ? rnti : 0;
  gold_seq((uint32_t)((((uint64_t)nrnti << 16) + nid) % (1ULL << 31)), m->E, seq);
  for (int i = 0; i < m->E; i++)
    out[i] ^= seq[i];
  if (m->pre >= 0) { /* the receiver already removed the sequence for n_RNTI = pre; the residue is g(rnti) ^ g(pre) */
    gold_seq((uint32_t)((((uint64_t)m->pre << 16) + nid) % (1ULL << 31)), m->E, seq);
    for (int i = 0; i < m->E; i++)
      out[i] ^= seq[i];
  }
  return 0;
}

static uint64_t xs64(uint64_t *s)
{
  uint64_t x = *s;
  x ^= x << 13; x ^= x >> 7; x ^= x << 17;
  return *s = x;
}

int nr_pdcch_joint_model_unknowns(const nr_pdcch_joint_model_t *m) { return m->K; }

void nr_pdcch_joint_model_free(nr_pdcch_joint_model_t *m)
{
  if (!m)
    return;
  free(m->rows);
  free(m->base);
  free(m);
}

nr_pdcch_joint_model_t *nr_pdcch_joint_model_new(nr_pdcch_joint_encode_fn enc, void *ctx, int A, int E,
                                                 uint16_t n_id, int scramble_with_rnti)
{
  return nr_pdcch_joint_model_new_ex(enc, ctx, A, E, n_id, scramble_with_rnti, -1);
}

nr_pdcch_joint_model_t *nr_pdcch_joint_model_new_ex(nr_pdcch_joint_encode_fn enc, void *ctx, int A, int E,
                                                    uint16_t n_id, int scramble_with_rnti, int pre_descrambled_rnti)
{
  return nr_pdcch_joint_model_new_full(enc, ctx, A, E, n_id, scramble_with_rnti, pre_descrambled_rnti, 0);
}

nr_pdcch_joint_model_t *nr_pdcch_joint_model_new_full(nr_pdcch_joint_encode_fn enc, void *ctx, int A, int E,
                                                      uint16_t n_id, int scramble_with_rnti, int pre_descrambled_rnti,
                                                      int unknown_nid)
{
  if (!enc || A < 1 || A > NR_PDCCH_JOINT_MAX_A || E < A + 16 + (unknown_nid ? 16 : 0) || E > NR_PDCCH_JOINT_MAX_E)
    return NULL;
  nr_pdcch_joint_model_t *m = calloc(1, sizeof(*m));
  if (!m)
    return NULL;
  m->A = A; m->E = E; m->K = A + 16 + (unknown_nid ? 16 : 0); m->unk = unknown_nid ? 1 : 0; m->nid = unknown_nid ? 0 : n_id; m->swr = scramble_with_rnti; m->pre = pre_descrambled_rnti; m->enc = enc; m->ctx = ctx;
  m->rows = calloc((size_t)E, sizeof(v128));
  m->base = calloc((size_t)E, 1);
  if (!m->rows || !m->base) {
    nr_pdcch_joint_model_free(m);
    return NULL;
  }
  uint8_t t[NR_PDCCH_JOINT_MAX_E];
  if (joint_eval(m, 0, 0, m->nid, m->base) != 0) {
    nr_pdcch_joint_model_free(m);
    return NULL;
  }
  /* PROBE the columns: never assume the structure of the real encoder. */
  for (int j = 0; j < m->K; j++) {
    const uint64_t pl = j < A ? (1ULL << j) : 0;
    const uint16_t rn = (j >= A && j < A + 16) ? (uint16_t)(1u << (j - A)) : 0;
    const uint16_t nd = (uint16_t)(m->nid ^ (j >= A + 16 ? (1u << (j - A - 16)) : 0)); /* n_ID columns (unknown mode) */
    if (joint_eval(m, pl, rn, nd, t) != 0) {
      nr_pdcch_joint_model_free(m);
      return NULL;
    }
    for (int i = 0; i < E; i++)
      if (t[i] ^ m->base[i])
        v_set(&m->rows[i], j);
  }
  /* AFFINITY CHECK: random combinations must equal base XOR M*x, else the callback is not GF(2)-affine (or the
   * encoder has a data-dependent branch) and every conclusion drawn from M would be wrong. */
  uint64_t seed = 0x9E3779B97F4A7C15ULL ^ ((uint64_t)A << 32) ^ (uint64_t)E ^ ((uint64_t)n_id << 8);
  for (int trial = 0; trial < 16; trial++) {
    const uint64_t pl = xs64(&seed) & (A == 64 ? ~0ULL : ((1ULL << A) - 1));
    const uint16_t rn = (uint16_t)xs64(&seed);
    const uint16_t nd = m->unk ? (uint16_t)xs64(&seed) : m->nid;
    v128 x = {{pl, 0}};
    for (int r = 0; r < 16; r++)
      if ((rn >> r) & 1)
        v_set(&x, A + r);
    if (m->unk)
      for (int r = 0; r < 16; r++)
        if ((nd >> r) & 1)
          v_set(&x, A + 16 + r);
    if (joint_eval(m, pl, rn, nd, t) != 0) {
      nr_pdcch_joint_model_free(m);
      return NULL;
    }
    for (int i = 0; i < E; i++)
      if ((m->base[i] ^ v_par(m->rows[i], x)) != t[i]) {
        nr_pdcch_joint_model_free(m);
        return NULL;
      }
  }
  /* RANK: every unknown must be observable, otherwise the answer is not unique. */
  v128 basis[JMAX_K];
  int piv[JMAX_K], r = 0;
  for (int i = 0; i < E && r < m->K; i++) {
    v128 v = m->rows[i];
    for (int p = 0; p < r; p++)
      if (v_get(v, piv[p]))
        v = v_xor(v, basis[p]);
    if (!v_zero(v)) {
      basis[r] = v;
      piv[r++] = v_lowbit(v);
    }
  }
  if (r < m->K) {
    nr_pdcch_joint_model_free(m);
    return NULL;
  }
  return m;
}

typedef struct { float w; int idx; } wi_t;
static int cmp_wi(const void *a, const void *b)
{
  const wi_t *x = a, *y = b;
  return (x->w < y->w) - (x->w > y->w); /* descending */
}

static inline void e_flip(ebits *e, int i) { e->w[i >> 6] ^= 1ULL << (i & 63); }
static inline int e_get(const ebits *e, int i) { return (int)((e->w[i >> 6] >> (i & 63)) & 1u); }

int nr_pdcch_joint_solve(const nr_pdcch_joint_model_t *m, const int16_t *llr, int order, nr_pdcch_joint_result_t *out)
{
  const int E = m->E, K = m->K;
  memset(out, 0, sizeof(*out));
  if (order < 0) order = 0;
  if (order > 2) order = 2;

  double w[NR_PDCCH_JOINT_MAX_E];
  ebits yb = {{0}};
  wi_t ord[NR_PDCCH_JOINT_MAX_E];
  for (int i = 0; i < E; i++) {
    w[i] = fabs((double)llr[i]);
    if ((llr[i] < 0) ^ m->base[i]) /* observation with the baseline removed: y' = y XOR b */
      e_flip(&yb, i);
    ord[i].w = (float)w[i];
    ord[i].idx = i;
  }
  qsort(ord, (size_t)E, sizeof(wi_t), cmp_wi);

  /* the K most reliable LINEARLY INDEPENDENT observations (the most reliable basis, MRIP) */
  v128 basis[JMAX_K];
  int piv[JMAX_K], I[JMAX_K], r = 0;
  for (int o = 0; o < E && r < K; o++) {
    v128 v = m->rows[ord[o].idx];
    for (int p = 0; p < r; p++) {
      const uint64_t msk = 0 - (uint64_t)v_get(v, piv[p]);
      v.w[0] ^= basis[p].w[0] & msk;
      v.w[1] ^= basis[p].w[1] & msk;
    }
    if (!v_zero(v)) {
      basis[r] = v;
      piv[r] = v_lowbit(v);
      I[r++] = ord[o].idx;
    }
  }
  if (r < K)
    return 0;

  /* invert B = rows at I (Gauss-Jordan on [B | Id]) */
  v128 Bm[JMAX_K], Iv[JMAX_K];
  for (int t = 0; t < K; t++) {
    Bm[t] = m->rows[I[t]];
    memset(&Iv[t], 0, sizeof(Iv[t]));
    v_set(&Iv[t], t);
  }
  for (int c = 0; c < K; c++) {
    int p = c;
    while (p < K && !v_get(Bm[p], c))
      p++;
    if (p == K)
      return 0;
    if (p != c) {
      v128 tmp = Bm[p]; Bm[p] = Bm[c]; Bm[c] = tmp;
      tmp = Iv[p]; Iv[p] = Iv[c]; Iv[c] = tmp;
    }
    const v128 bc = Bm[c], ic = Iv[c];
    for (int q = 0; q < K; q++) {
      const uint64_t msk = (0 - (uint64_t)v_get(Bm[q], c)) & (0 - (uint64_t)(q != c));
      Bm[q].w[0] ^= bc.w[0] & msk; Bm[q].w[1] ^= bc.w[1] & msk;
      Iv[q].w[0] ^= ic.w[0] & msk; Iv[q].w[1] ^= ic.w[1] & msk;
    }
  }
  /* Iv[j] (a vector over t) is row j of B^-1. Columns: colInv[t] is a vector over j. */
  v128 colInv[JMAX_K];
  memset(colInv, 0, sizeof(colInv));
  for (int j = 0; j < K; j++)
    for (int t = 0; t < K; t++)
      if (v_get(Iv[j], t))
        v_set(&colInv[t], j);

  v128 yI = {{0, 0}};
  for (int t = 0; t < K; t++)
    if (e_get(&yb, I[t]))
      v_set(&yI, t);
  v128 x0 = {{0, 0}};
  for (int j = 0; j < K; j++)
    if (v_par(Iv[j], yI))
      v_set(&x0, j);

  /* d0 = M x0 XOR y' (where the exact solution disagrees with the observation), and T_t = M colInv[t] */
  ebits d0 = {{0}}, T[JMAX_K];
  if (order >= 1)
    memset(T, 0, sizeof(T)); /* only orders 1/2 read T */
  for (int i = 0; i < E; i++) {
    if (v_par(m->rows[i], x0) ^ e_get(&yb, i))
      e_flip(&d0, i);
    if (order >= 1) /* the flip tables are the dominant cost and only orders 1/2 use them */
      for (int t = 0; t < K; t++)
        if (v_par(m->rows[i], colInv[t]))
          e_flip(&T[t], i);
  }
  ebits inIb = {{0}};
  for (int t = 0; t < K; t++)
    e_flip(&inIb, I[t]); /* I[] holds distinct positions */

  double WO = 0.0, W2O = 0.0;
  for (int i = 0; i < E; i++)
    if (!e_get(&inIb, i)) {
      WO += w[i];
      W2O += w[i] * w[i];
    }

  /* enumerate candidates; ML metric = total weighted disagreement */
  double bestD = 1e300, bestDO = 0.0;
  int bt = -1, bu = -1, ncand = 0;
#define EVAL(D_EXPR_BLOCK)                                                      \
  do {                                                                          \
    ebits d = D_EXPR_BLOCK;                                                     \
    double D = 0.0, DO = 0.0;                                                   \
    for (int q = 0; q < EW; q++) {                                              \
      uint64_t v = d.w[q];                                                      \
      while (v) {                                                               \
        const int i = q * 64 + __builtin_ctzll(v);                              \
        v &= v - 1;                                                             \
        D += w[i];                                                              \
        if (!e_get(&inIb, i)) DO += w[i];                                       \
      }                                                                         \
    }                                                                           \
    ncand++;                                                                    \
    if (D < bestD) { bestD = D; bestDO = DO; bt = ct; bu = cu; }                \
  } while (0)
  int ct = -1, cu = -1;
  EVAL(d0);
  if (order >= 1)
    for (int t = 0; t < K; t++) {
      ct = t; cu = -1;
      ebits d = d0;
      for (int q = 0; q < EW; q++) d.w[q] ^= T[t].w[q];
      EVAL(d);
    }
  if (order >= 2)
    for (int t = 0; t < K; t++)
      for (int u = t + 1; u < K; u++) {
        ct = t; cu = u;
        ebits d = d0;
        for (int q = 0; q < EW; q++) d.w[q] ^= T[t].w[q] ^ T[u].w[q];
        EVAL(d);
      }
#undef EVAL

  v128 x = x0;
  if (bt >= 0) x = v_xor(x, colInv[bt]);
  if (bu >= 0) x = v_xor(x, colInv[bu]);
  out->payload = nr_dci_bits_from_u64( x.w[0] & (m->A == 64 ? ~0ULL : ((1ULL << m->A) - 1)));
  uint16_t rn = 0;
  for (int b = 0; b < 16; b++)
    if (v_get(x, m->A + b))
      rn |= (uint16_t)(1u << b);
  out->rnti = rn;
  out->nid = m->nid;
  if (m->unk) {
    uint16_t nd = 0;
    for (int b = 0; b < 16; b++)
      if (v_get(x, m->A + 16 + b))
        nd |= (uint16_t)(1u << b);
    out->nid = nd;
  }

  {
    ebits d = d0;
    if (bt >= 0) for (int q = 0; q < EW; q++) d.w[q] ^= T[bt].w[q];
    if (bu >= 0) for (int q = 0; q < EW; q++) d.w[q] ^= T[bu].w[q];
    int mm = 0;
    for (int q = 0; q < EW; q++) mm += __builtin_popcountll(d.w[q]);
    out->mismatched_bits = mm;
  }
  out->n_candidates = ncand;
  if (WO > 0.0) {
    out->corr = 1.0 - 2.0 * bestDO / WO;
    const double sigma = sqrt(W2O) / WO;
    out->threshold = sigma * sqrt(2.0 * log((double)ncand / JOINT_PFA));
    out->accepted = out->corr >= out->threshold;
  }
  return out->accepted;
}

int nr_pdcch_joint_prescreen(const int16_t *llr, int E)
{
  double a = 0.0, b = 0.0;
  for (int i = 0; i < E; i++) {
    const double v = fabs((double)llr[i]);
    a += v;
    b += v * v;
  }
  if (b <= 0.0)
    return 0;
  const double r = (a * a) / ((double)E * b);
  return r >= 2.0 / M_PI + 3.09 * 0.339 / sqrt((double)E);
}
