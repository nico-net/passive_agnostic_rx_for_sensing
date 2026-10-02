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

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_dci_history.c
 * \brief BC9 DL DCI history ring and DCI-adjacency k0 evidence. See the header for the evidence classes and A1-A3.
 */

#include "nr_dci_history.h"
#include <string.h>

/* OAI MCS tables / TBS (openair2/LAYER2/NR_MAC_COMMON). Declared here rather than through nr_mac_common.h, which pulls the
 * generated ASN.1 headers into a module that needs none of them. */
uint8_t nr_get_Qm_dl(uint8_t Imcs, uint8_t table_idx);
uint32_t nr_get_code_rate_dl(uint8_t Imcs, uint8_t table_idx);
uint32_t nr_compute_tbs(uint16_t Qm, uint16_t R, uint16_t nb_rb, uint16_t nb_symb_sch, uint16_t nb_dmrs_prb,
                        uint16_t nb_rb_oh, uint8_t tb_scaling, uint8_t Nl);

void nr_dci_hist_init(nr_dci_hist_t *h, uint32_t period)
{
  memset(h, 0, sizeof(*h));
  pthread_mutex_init(&h->lock, NULL);
  h->period = period;
}

int32_t nr_dci_hist_diff(const nr_dci_hist_t *h, uint32_t a, uint32_t b)
{
  if (!h->period)
    return (int32_t)(a - b);
  int64_t d = ((int64_t)a - (int64_t)b) % (int64_t)h->period;
  if (d > (int64_t)h->period / 2)
    d -= h->period;
  else if (d <= -(int64_t)h->period / 2)
    d += h->period;
  return (int32_t)d;
}
static uint32_t slot_add(const nr_dci_hist_t *h, uint32_t s, int32_t d)
{
  if (!h->period)
    return s + (uint32_t)d;
  int64_t v = ((int64_t)s + d) % (int64_t)h->period;
  return (uint32_t)(v < 0 ? v + h->period : v);
}

static nr_dci_hist_rnti_t *find_rnti(nr_dci_hist_t *h, uint16_t rnti)
{
  for (int i = 0; i < NR_DCI_HIST_RNTIS; i++)
    if (h->r[i].n > 0 && h->r[i].rnti == rnti)
      return &h->r[i];
  return NULL;
}

void nr_dci_hist_push(nr_dci_hist_t *h, const nr_dci_hist_entry_t *e)
{
  if (h == NULL || e == NULL || e->rnti == 0)
    return;
  pthread_mutex_lock(&h->lock);
  nr_dci_hist_rnti_t *r = find_rnti(h, e->rnti);
  if (r == NULL) { /* a free slot, else the least recently written RNTI */
    int v = 0;
    for (int i = 0; i < NR_DCI_HIST_RNTIS; i++) {
      if (h->r[i].n == 0) {
        v = i;
        break;
      }
      if (h->r[i].touched < h->r[v].touched)
        v = i;
    }
    r = &h->r[v];
    memset(r, 0, sizeof(*r));
    r->rnti = e->rnti;
  }
  r->e[r->w] = *e;
  r->w = (r->w + 1) % NR_DCI_HIST_DEPTH;
  if (r->n < NR_DCI_HIST_DEPTH)
    r->n++;
  if (r->n == 1 || nr_dci_hist_diff(h, e->abs_slot, r->newest) > 0)
    r->newest = e->abs_slot;
  r->touched = ++h->clock;
  pthread_mutex_unlock(&h->lock);
}

/* Visible entries of r with |slot - around| <= span, newest written first. Lock held. */
static int collect(const nr_dci_hist_t *h, const nr_dci_hist_rnti_t *r, uint32_t around, int span, nr_dci_hist_entry_t *out, int max)
{
  int n = 0;
  for (int k = 0; k < r->n && n < max; k++) {
    const nr_dci_hist_entry_t *e = &r->e[(r->w - 1 - k + NR_DCI_HIST_DEPTH) % NR_DCI_HIST_DEPTH];
    const int32_t age = nr_dci_hist_diff(h, r->newest, e->abs_slot);
    if (age < 0 || age >= NR_DCI_HIST_WINDOW)
      continue;
    const int32_t d = nr_dci_hist_diff(h, e->abs_slot, around);
    if (d >= -span && d <= span)
      out[n++] = *e;
  }
  return n;
}
int nr_dci_hist_near(nr_dci_hist_t *h, uint16_t rnti, uint32_t around, int span, nr_dci_hist_entry_t *out, int max)
{
  if (h == NULL || out == NULL || max <= 0 || span < 0)
    return 0;
  pthread_mutex_lock(&h->lock);
  const nr_dci_hist_rnti_t *r = find_rnti(h, rnti);
  const int n = r ? collect(h, r, around, span, out, max) : 0;
  pthread_mutex_unlock(&h->lock);
  return n;
}
int nr_dci_hist_at(nr_dci_hist_t *h, uint16_t rnti, uint32_t abs_slot, nr_dci_hist_entry_t *out, int max)
{
  return nr_dci_hist_near(h, rnti, abs_slot, 0, out, max);
}

/* ---- compatibility ---------------------------------------------------------------------------------------------- */
static bool prb_differs(const nr_dci_hist_entry_t *a, const nr_dci_hist_entry_t *b)
{
  if (a->ra_type0 != b->ra_type0 || a->vrb_to_prb != b->vrb_to_prb)
    return false; /* not comparable without expanding: never a difference */
  if (!a->ra_type0)
    return a->start_rb != b->start_rb || a->num_rb != b->num_rb;
  if (a->rbg_size != b->rbg_size || a->rbg_bwp_start != b->rbg_bwp_start)
    return false;
  return a->rbg_bitmap != b->rbg_bitmap;
}
/* (Qm, TBS) of one MCS under one table on the shared geometry; false for a reserved / invalid entry (R = 0). */
static bool mcs_key(uint8_t mcs, uint8_t table, uint16_t nb_rb, uint8_t n_cdm, const nr_dci_geom_t *geo, uint8_t *qm, uint32_t *tbs)
{
  *qm = nr_get_Qm_dl(mcs, table);
  const uint32_t r = nr_get_code_rate_dl(mcs, table);
  if (*qm == 0 || r == 0 || nb_rb == 0 || geo->nl == 0)
    return false;
  const int re_per_cdm = geo->dmrs_type ? 4 : 6;
  const uint16_t nb_dmrs_prb = (uint16_t)(__builtin_popcount(geo->dmrs_mask) * re_per_cdm * (n_cdm ? n_cdm : 1));
  *tbs = nr_compute_tbs(*qm, (uint16_t)r, nb_rb, geo->nb_symb, nb_dmrs_prb, geo->xoh, 0, geo->nl);
  return *tbs > 0;
}
bool nr_dci_hist_incompatible(const nr_dci_hist_entry_t *g, const nr_dci_hist_entry_t *x, uint8_t table_mask,
                              const nr_dci_geom_t *geo)
{
  if (g == NULL || x == NULL)
    return false;
  if (g->rv != x->rv)
    return true;
  if (g->dmrs_ports != x->dmrs_ports || g->n_cdm != x->n_cdm || g->nscid != x->nscid)
    return true;
  if (prb_differs(g, x))
    return true;
  if (geo == NULL || (table_mask & 0x7) == 0)
    return false; /* symbols of the occupant unknown, or no table information */
  /* Same PRB set (or not comparable) and same symbols: compatible iff SOME pair of alive tables gives the same (Qm, TBS). */
  for (int ta = 0; ta < 3; ta++) {
    if (!(table_mask >> ta & 1))
      continue;
    for (int tb = 0; tb < 3; tb++) {
      if (!(table_mask >> tb & 1))
        continue;
      uint8_t qa, qb;
      uint32_t ka, kb;
      const bool va = mcs_key(g->mcs, (uint8_t)ta, g->num_rb, g->n_cdm, geo, &qa, &ka);
      const bool vb = mcs_key(x->mcs, (uint8_t)tb, x->num_rb, x->n_cdm, geo, &qb, &kb);
      if (!va || !vb)
        return false; /* a reserved MCS (retransmission, TBS from an earlier grant) is never a provable difference */
      if (qa == qb && ka == kb)
        return false;
    }
  }
  return true;
}

/* ---- per-world occupant check ----------------------------------------------------------------------------------- */
#define NEAR_MAX 64
static bool singleton(uint64_t m, int *k)
{
  if (m == 0 || (m & (m - 1)))
    return false;
  *k = __builtin_ctzll(m);
  return true;
}

bool nr_dci_hist_k0_certified(nr_dci_hist_t *h, const nr_dci_hist_entry_t *g, int k_lead, uint64_t sib_k0_mask,
                              uint8_t table_mask, const nr_dci_geom_t *geo, nr_dci_row_k0_fn row_k0, void *arg)
{
  if (h == NULL || g == NULL || !g->dci11 || k_lead < 0 || k_lead > NR_DCI_HIST_K0_MAX)
    return false;
  const uint64_t sib = sib_k0_mask & ~(UINT64_C(1) << k_lead);
  if (sib == 0)
    return false; /* an empty sibling set never certifies */
  const uint32_t u = slot_add(h, g->abs_slot, k_lead);
  nr_dci_hist_entry_t nb[NEAR_MAX];
  const int n = nr_dci_hist_near(h, g->rnti, u, 2 * NR_DCI_HIST_K0_MAX, nb, NEAR_MAX);
  for (int ks = 0; ks <= NR_DCI_HIST_K0_MAX; ks++) {
    if (!(sib >> ks & 1))
      continue;
    /* World "truth is ks": the same-row DCI at t + (k_lead - ks) lands on u. */
    const uint32_t t_occ = slot_add(h, g->abs_slot, k_lead - ks);
    int found = 0;
    for (int j = 0; j < n; j++) {
      const nr_dci_hist_entry_t *x = &nb[j];
      if (!x->dci11 || x->cfg != g->cfg || x->tda != g->tda || x->abs_slot != t_occ)
        continue;
      found++;
      if (!nr_dci_hist_incompatible(g, x, table_mask, geo))
        return false;
    }
    if (found)
      continue;
    /* No same-row occupant observed: a DCI of a row with a CERTIFIED k0 that lands on u is the occupant in every world. */
    for (int j = 0; j < n && row_k0; j++) {
      const nr_dci_hist_entry_t *y = &nb[j];
      if (!y->dci11 || y->cfg != g->cfg || y->tda == g->tda)
        continue;
      int ky;
      if (!singleton(row_k0(arg, y->cfg, y->rnti, y->tda), &ky) || slot_add(h, y->abs_slot, ky) != u)
        continue;
      found++;
      if (!nr_dci_hist_incompatible(g, y, table_mask, NULL)) /* other row: its symbols are unknown */
        return false;
    }
    if (!found)
      return false; /* missed / unseen / evicted: ambiguous */
  }
  return true;
}

/* ---- deterministic DCI-adjacency exclusions ---------------------------------------------------------------------- */
int nr_dci_hist_adj_exclusions(nr_dci_hist_t *h, const nr_dci_hist_entry_t *x, nr_dci_row_k0_fn row_k0, void *arg,
                               uint64_t forbid[NR_DCI_HIST_ROWS])
{
  if (h == NULL || x == NULL || row_k0 == NULL || forbid == NULL || !x->dci11 || x->tda >= NR_DCI_HIST_ROWS)
    return 0;
  uint64_t add[NR_DCI_HIST_ROWS] = {0};
  int kx = -1;
  const bool x_cert = singleton(row_k0(arg, x->cfg, x->rnti, x->tda), &kx);
  nr_dci_hist_entry_t nb[NEAR_MAX];
  const int n = nr_dci_hist_near(h, x->rnti, x->abs_slot, NR_DCI_HIST_K0_MAX, nb, NEAR_MAX);
  for (int j = 0; j < n; j++) {
    const nr_dci_hist_entry_t *y = &nb[j];
    if (!y->dci11 || y->cfg != x->cfg || y->tda >= NR_DCI_HIST_ROWS)
      continue;
    const int32_t dy = nr_dci_hist_diff(h, y->abs_slot, x->abs_slot); /* t_y - t_x */
    if (dy == 0 && y->tda == x->tda)
      continue; /* x itself (or a duplicate of it): no second PDSCH */
    if (x_cert) { /* x occupies t_x + kx, so row(y) cannot have k0 = t_x + kx - t_y */
      const int k = kx - dy;
      if (k >= 0 && k <= NR_DCI_HIST_K0_MAX)
        add[y->tda] |= UINT64_C(1) << k;
    }
    int ky; /* y occupies t_y + ky, so row(x) cannot have k0 = t_y + ky - t_x */
    if (singleton(row_k0(arg, y->cfg, y->rnti, y->tda), &ky)) {
      const int k = ky + dy;
      if (k >= 0 && k <= NR_DCI_HIST_K0_MAX)
        add[x->tda] |= UINT64_C(1) << k;
    }
  }
  int rows = 0;
  for (int r = 0; r < NR_DCI_HIST_ROWS; r++) {
    rows += add[r] != 0;
    forbid[r] |= add[r];
  }
  return rows;
}

static nr_dci_hist_t g_hist;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static void g_init(void) { nr_dci_hist_init(&g_hist, 0); }
nr_dci_hist_t *nr_dci_hist_global(void)
{
  pthread_once(&g_once, g_init);
  return &g_hist;
}
void nr_dci_hist_global_init(uint32_t period)
{
  nr_dci_hist_t *h = nr_dci_hist_global();
  pthread_mutex_lock(&h->lock);
  h->period = period;
  pthread_mutex_unlock(&h->lock);
}
