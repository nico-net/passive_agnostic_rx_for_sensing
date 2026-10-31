/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this
 * file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.openairinterface.org/?page_id=698
 */

/* Blind PT-RS presence / density detector and per-class evidence: see nr_pdsch_ptrs_detect.h. */

#include <math.h>
#include <stddef.h>
#include <string.h>
#include "nr_pdsch_ptrs_detect.h"
#include "nr_pdsch_ptrs_unav.h"

#define NR_PTRS_MAX_RE (273 * 12)

static const uint8_t kL[3] = {1, 2, 4};

int nr_ptrs_hyp_index(const nr_ptrs_hyp_t *h)
{
  const int li = h->L == 4 ? 2 : (h->L == 2 ? 1 : 0);
  return ((h->K == 4 ? 12 : 0) + h->k_re) * 3 + li;
}

void nr_ptrs_hyp_from_index(int idx, nr_ptrs_hyp_t *h)
{
  h->L = kL[idx % 3];
  const int kk = idx / 3;
  h->K = kk >= 12 ? 4 : 2;
  h->k_re = (uint8_t)(kk % 12);
}

/* The rule of set_ptrs_symb_idx() (PHY/NR_REFSIG/ptrs_nr.c): l_ref restarts at a DM-RS symbol met inside a PT-RS step. */
uint16_t nr_ptrs_symbol_mask(uint8_t start_symbol, uint8_t nb_symbols, uint8_t L, uint16_t dmrs_symb_pos)
{
  uint16_t mask = 0;
  if (L == 0)
    return 0;
  int i = 0, l_ref = start_symbol;
  const int last = start_symbol + nb_symbols - 1;
  while (l_ref + i * L <= last) {
    int is_dmrs = 0, l;
    for (l = l_ref + i * L; l >= (l_ref + (i - 1) * L + 1 > l_ref ? l_ref + (i - 1) * L + 1 : l_ref); l--) {
      if ((dmrs_symb_pos >> l) & 1) {
        is_dmrs = 1;
        break;
      }
    }
    if (is_dmrs) {
      l_ref = l;
      i = 1;
      continue;
    }
    mask |= (uint16_t)(1u << (l_ref + i * L));
    i++;
  }
  return mask;
}

/* k_RB_ref of TS 38.211 7.4.1.2.2 */
static inline int ptrs_krb(uint16_t rnti, uint8_t K, uint16_t nb_rb) { return nb_rb % K == 0 ? rnti % K : rnti % (nb_rb % K); }

bool nr_ptrs_is_re(uint16_t re, uint16_t rnti, uint8_t K, uint16_t nb_rb, uint8_t k_re)
{
  const int first = k_re + ptrs_krb(rnti, K, nb_rb) * 12;
  return re >= first && (re - first) % (K * 12) == 0;
}

uint32_t nr_ptrs_hyp_unav_res(const nr_ptrs_hyp_t *h, uint16_t nb_rb, uint8_t start_symbol, uint8_t nb_symbols,
                              uint16_t dmrs_symb_pos)
{
  return nr_pdsch_ptrs_unav_res(nb_rb, start_symbol, nb_symbols, dmrs_symb_pos, h->K, h->L, 1);
}

/* log of the Gamma(P,1) upper tail, P integer: ln( e^-T sum_{i<P} T^i / i! ) */
static double ptrs_log_tail(int P, double T)
{
  if (T <= 0.0 || P <= 0)
    return 0.0;
  double mx = -1e300, t[16];
  const double lt = log(T);
  for (int i = 0; i < P && i < 16; i++) {
    t[i] = i * lt - lgamma(i + 1.0);
    if (t[i] > mx)
      mx = t[i];
  }
  double s = 0.0;
  for (int i = 0; i < P && i < 16; i++)
    s += exp(t[i] - mx);
  const double r = -T + mx + log(s);
  return r > 0.0 ? 0.0 : r;
}

void nr_ptrs_detect(const int16_t *rxdataF, int n_ant, size_t ant_stride, int symbol_sz, int re0, int nb_rb,
                    uint16_t rnti, uint8_t start_symbol, uint8_t nb_symbols, uint16_t dmrs_symb_pos, double alpha,
                    nr_ptrs_det_t *out)
{
  memset(out, 0, sizeof(*out));
  out->log10_p = 0.f;
  if (nb_rb <= 0 || nb_rb > 273 || n_ant <= 0 || start_symbol + nb_symbols > 14 || nb_symbols < 2)
    return;
  const int nre = nb_rb * 12;
  float Pr[NR_PTRS_MAX_RE], Pi[NR_PTRS_MAX_RE], Q[NR_PTRS_MAX_RE];
  double T[NR_PTRS_HYP_N];
  int pairs[3] = {0, 0, 0};
  memset(T, 0, sizeof(T));
  for (int li = 0; li < 3; li++) {
    const uint16_t mask = nr_ptrs_symbol_mask(start_symbol, nb_symbols, kL[li], dmrs_symb_pos);
    int prev = -1;
    for (int s = start_symbol; s < start_symbol + nb_symbols; s++) {
      if (!((mask >> s) & 1))
        continue;
      if (prev >= 0) {
        memset(Pr, 0, sizeof(float) * nre);
        memset(Pi, 0, sizeof(float) * nre);
        memset(Q, 0, sizeof(float) * nre);
        for (int a = 0; a < n_ant; a++) {
          const int16_t *ra = rxdataF + (size_t)a * ant_stride * 2 + (size_t)prev * symbol_sz * 2;
          const int16_t *rb = rxdataF + (size_t)a * ant_stride * 2 + (size_t)s * symbol_sz * 2;
          for (int k = 0; k < nre; k++) {
            const int f = ((re0 + k) % symbol_sz) * 2;
            const float xr = ra[f], xi = ra[f + 1], yr = rb[f], yi = rb[f + 1];
            Pr[k] += xr * yr + xi * yi;  /* x * conj(y) */
            Pi[k] += xi * yr - xr * yi;
          }
        }
        /* The antennas' products share one phase under no PT-RS too (|H|^2 x_a conj(x_b)), so they are summed BEFORE the
         * normalisation: the null variance of the sum over REs is the sum of |P(k)|^2, not of the per-antenna |t|^2. */
        for (int k = 0; k < nre; k++)
          Q[k] = Pr[k] * Pr[k] + Pi[k] * Pi[k];
        for (int Kc = 0; Kc < 2; Kc++) {
          const int K = Kc ? 4 : 2;
          const int krb = ptrs_krb(rnti, (uint8_t)K, (uint16_t)nb_rb);
          for (int k_re = 0; k_re < 12; k_re++) {
            double sr = 0, si = 0, sq = 0;
            for (int k = k_re + krb * 12; k < nre; k += K * 12) {
              sr += Pr[k];
              si += Pi[k];
              sq += Q[k];
            }
            if (sq > 0.0)
              T[(Kc * 12 + k_re) * 3 + li] += (sr * sr + si * si) / sq;
          }
        }
        pairs[li]++;
      }
      prev = s;
    }
  }
  int tested = 0;
  for (int li = 0; li < 3; li++)
    tested += pairs[li] ? 24 : 0;
  if (!tested)
    return;
  double best = 1e300;
  int besti = -1;
  for (int h = 0; h < NR_PTRS_HYP_N; h++) {
    const int P = pairs[h % 3];
    if (!P)
      continue;
    const double lq = ptrs_log_tail(P, T[h]) + log((double)tested);
    if (lq < best) {
      best = lq;
      besti = h;
    }
  }
  if (besti < 0)
    return;
  out->log10_p = (float)(best / M_LN10);
  out->pairs = (uint8_t)pairs[besti % 3];
  nr_ptrs_hyp_from_index(besti, &out->hyp);
  out->fired = best < log(alpha);
}

/* ---- evidence per class ---------------------------------------------------------------------------- */
int nr_ptrs_class_index(unsigned mcs, unsigned nb_rb)
{
  const int mb = mcs / 6 >= NR_PTRS_MCS_BANDS ? NR_PTRS_MCS_BANDS - 1 : (int)(mcs / 6);
  static const unsigned edge[NR_PTRS_NRB_BANDS - 1] = {4, 8, 16, 32, 64, 128, 200};
  int rb = 0;
  while (rb < NR_PTRS_NRB_BANDS - 1 && nb_rb > edge[rb])
    rb++;
  return mb * NR_PTRS_NRB_BANDS + rb;
}

void nr_ptrs_class_init(nr_ptrs_class_t *c)
{
  memset(c, 0, sizeof(*c));
  c->latched = -1;
}

bool nr_ptrs_class_should_detect(const nr_ptrs_class_t *c)
{
  if (c->latched < 0)
    return true;
  return c->latched == 0 && (c->det_since_hit & 15) == 0;
}

void nr_ptrs_class_det_feed(nr_ptrs_class_t *c, const nr_ptrs_det_t *d)
{
  if (c->det_n < 0xffff)
    c->det_n++;
  if (d->fired) {
    c->det_hit++;
    c->det_since_hit = 0;
    const int i = nr_ptrs_hyp_index(&d->hyp);
    if (c->votes[i] == 255)
      for (int j = 0; j < NR_PTRS_HYP_N; j++)
        c->votes[j] >>= 1;
    c->votes[i]++;
    if (c->latched == 0)
      c->latched = -1;  /* the back-off probe found PT-RS: back to arbitration */
  } else {
    if (c->det_since_hit < 0xffff)
      c->det_since_hit++;
    /* 64 detector runs on failing grants of this class without a single hit: no PT-RS here (soft: re-probed 1 in 16) */
    if (c->latched < 0 && c->det_hit == 0 && c->det_n >= 64)
      c->latched = 0;
  }
}

bool nr_ptrs_class_hyp(const nr_ptrs_class_t *c, nr_ptrs_hyp_t *h)
{
  int b1 = -1, v1 = 0, v2 = 0;
  for (int i = 0; i < NR_PTRS_HYP_N; i++) {
    if (c->votes[i] > v1) {
      v2 = v1;
      v1 = c->votes[i];
      b1 = i;
    } else if (c->votes[i] > v2) {
      v2 = c->votes[i];
    }
  }
  if (b1 < 0 || v1 < 2 || v1 < 2 * v2)
    return false;
  nr_ptrs_hyp_from_index(b1, h);
  return true;
}

static void wilson(unsigned ok, unsigned n, double *lo, double *hi)
{
  if (n == 0) {
    *lo = 0.0;
    *hi = 1.0;
    return;
  }
  const double z = 1.96, nn = n, p = ok / nn, d = 1.0 + z * z / nn;
  const double m = p + z * z / (2.0 * nn), q = z * sqrt(p * (1.0 - p) / nn + z * z / (4.0 * nn * nn));
  *lo = fmax(0.0, (m - q) / d);
  *hi = fmin(1.0, (m + q) / d);
}

int nr_ptrs_class_pick_mode(const nr_ptrs_class_t *c)
{
  double lo, hi1, hi2;
  wilson(c->ok[1], c->tr[1], &lo, &hi1);
  wilson(c->ok[2], c->tr[2], &lo, &hi2);
  if (c->tr[1] == 0 || c->tr[2] == 0)
    return c->tr[1] <= c->tr[2] ? 1 : 2;
  if (hi1 == hi2)
    return c->tr[1] <= c->tr[2] ? 1 : 2;
  return hi1 > hi2 ? 1 : 2;
}

int nr_ptrs_class_best_mode(const nr_ptrs_class_t *c)
{
  const double r1 = c->tr[1] ? (double)c->ok[1] / c->tr[1] : 0.0, r2 = c->tr[2] ? (double)c->ok[2] / c->tr[2] : 0.0;
  return r2 > r1 ? 2 : 1;
}

void nr_ptrs_class_crc_feed(nr_ptrs_class_t *c, int mode, bool tb_ok)
{
  const int w = mode < 0 ? 0 : (mode > 2 ? 2 : mode);
  c->tr[w]++;
  if (tb_ok)
    c->ok[w]++;
  if (c->tr[w] >= 4096) {  /* recent evidence dominates */
    c->tr[w] >>= 1;
    c->ok[w] >>= 1;
  }
  double lo0, hi0, lo1, hi1, lo2, hi2;
  wilson(c->ok[0], c->tr[0], &lo0, &hi0);
  wilson(c->ok[1], c->tr[1], &lo1, &hi1);
  wilson(c->ok[2], c->tr[2], &lo2, &hi2);
  const int b = lo1 >= lo2 ? 1 : 2;
  const double blo = b == 1 ? lo1 : lo2;
  if (c->ok[b] >= 4 && blo > hi0)
    c->latched = 1;
  else if (c->ok[0] >= 8 && lo0 > fmax(hi1, hi2) && c->tr[1] + c->tr[2] >= 8)
    c->latched = 0;
  else if (c->latched == 1 && c->tr[1] + c->tr[2] >= 16 && (c->ok[1] + c->ok[2]) * 4 < c->tr[1] + c->tr[2])
    c->latched = -1;  /* PT-RS decodes stopped working: back to arbitration */
}
