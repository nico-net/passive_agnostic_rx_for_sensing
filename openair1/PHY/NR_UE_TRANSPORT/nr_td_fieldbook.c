/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_td_fieldbook.h"
#include <string.h>

/* Add `rnti` to a distinct-RNTI set; false if already present or the set is full (a full set ignores new RNTIs). */
static bool add_rnti(uint16_t *set, int *n, uint16_t rnti)
{
  for (int i = 0; i < *n; i++)
    if (set[i] == rnti)
      return false;
  if (*n >= NR_TD_FB_MAX_RNTI)
    return false;
  set[(*n)++] = rnti;
  return true;
}

static void entry_reset(nr_td_field_entry_t *e)
{
  memset(e, 0, sizeof(*e));
  e->value = -1;
  for (int i = 0; i < NR_TD_FB_MAX_CAND; i++)
    e->cand[i].value = -1;
}

static int clamp_thr(int v)
{
  return v < 1 ? 1 : (v > NR_TD_FB_MAX_RNTI ? NR_TD_FB_MAX_RNTI : v);
}

void nr_td_fieldbook_init(nr_td_fieldbook_t *fb, int promote_rntis, int withdraw_rntis)
{
  memset(fb, 0, sizeof(*fb));
  for (int f = 0; f < NR_TD_F_COUNT; f++)
    entry_reset(&fb->f[f]);
  fb->promote_rntis = clamp_thr(promote_rntis);
  fb->withdraw_rntis = clamp_thr(withdraw_rntis);
}

int32_t nr_td_pack_tdra(int S, int L, int mapping, int k0)
{
  return (int32_t)((S & 0xF) | ((L & 0x1F) << 4) | ((mapping & 1) << 9) | ((k0 & 0x3F) << 10));
}

/* Row for value v, creating it if needed; when the table is full evict the row with the fewest supporters
 * (ties: oldest), never the promoted value's row. */
static nr_td_field_cand_t *cand_row(nr_td_field_entry_t *e, int32_t v)
{
  int free_i = -1, victim = -1;
  for (int i = 0; i < NR_TD_FB_MAX_CAND; i++) {
    if (e->cand[i].value == v)
      return &e->cand[i];
    if (e->cand[i].value == -1) {
      if (free_i < 0)
        free_i = i;
    } else if (e->cand[i].value != e->value) {
      if (victim < 0 || e->cand[i].n_support < e->cand[victim].n_support
          || (e->cand[i].n_support == e->cand[victim].n_support && e->cand[i].born < e->cand[victim].born))
        victim = i;
    }
  }
  int i = free_i >= 0 ? free_i : victim;
  if (i < 0)
    return NULL;
  memset(&e->cand[i], 0, sizeof(e->cand[i]));
  e->cand[i].value = v;
  e->cand[i].born = ++e->tick;
  return &e->cand[i];
}

/* While unpromoted: promote the candidate with the most supporters if it has reached the threshold. */
static void try_promote(nr_td_fieldbook_t *fb, nr_td_field_entry_t *e, uint64_t slot)
{
  if (e->value != -1)
    return;
  int best = -1;
  for (int i = 0; i < NR_TD_FB_MAX_CAND; i++)
    if (e->cand[i].value != -1 && e->cand[i].n_support >= fb->promote_rntis
        && (best < 0 || e->cand[i].n_support > e->cand[best].n_support))
      best = i;
  if (best < 0)
    return;
  e->value = e->cand[best].value;
  e->n_contra = 0;
  e->epoch = fb->epoch;
  e->last_confirmed_slot = slot;
}

static void contradict_entry(nr_td_fieldbook_t *fb, nr_td_field_entry_t *e, uint16_t rnti, uint64_t slot)
{
  if (e->value == -1)
    return;
  add_rnti(e->contra, &e->n_contra, rnti);
  if (e->n_contra >= fb->withdraw_rntis) {
    /* withdraw: the old value's support is void; other candidates keep theirs and may be promoted right now */
    for (int i = 0; i < NR_TD_FB_MAX_CAND; i++)
      if (e->cand[i].value == e->value)
        e->cand[i].value = -1;
    e->value = -1;
    e->n_contra = 0;
    try_promote(fb, e, slot);
  }
}

void nr_td_fieldbook_contradict(nr_td_fieldbook_t *fb, uint16_t rnti, nr_td_field_t f)
{
  if ((int)f < 0 || f >= NR_TD_F_COUNT)
    return;
  contradict_entry(fb, &fb->f[f], rnti, fb->f[f].last_confirmed_slot);
}

static void field_observe(nr_td_fieldbook_t *fb, nr_td_field_t f, uint16_t rnti, int32_t v, uint64_t slot)
{
  if (v < 0)
    return;
  nr_td_field_entry_t *e = &fb->f[f];
  nr_td_field_cand_t *c = cand_row(e, v);
  if (c)
    add_rnti(c->support, &c->n_support, rnti);
  if (e->value == v) {
    e->epoch = fb->epoch;
    e->last_confirmed_slot = slot;
  } else if (e->value != -1) {
    contradict_entry(fb, e, rnti, slot); /* a withdrawal here promotes the contradicting convergers' value in this same call */
  } else {
    try_promote(fb, e, slot);
  }
}

void nr_td_fieldbook_converged(nr_td_fieldbook_t *fb, uint16_t rnti, const nr_pdsch_cfg_hypothesis_t *h, uint64_t slot)
{
  field_observe(fb, NR_TD_F_TDRA, rnti, nr_td_pack_tdra(h->tda_start, h->tda_length, h->mapping_type, h->k0), slot);
  field_observe(fb, NR_TD_F_DMRS_ADD_POS, rnti, h->dmrs_add_pos, slot);
  field_observe(fb, NR_TD_F_DMRS_MAX_LEN, rnti, h->dmrs_max_len, slot);
}

void nr_td_fieldbook_bump_epoch(nr_td_fieldbook_t *fb)
{
  fb->epoch++;
  for (int f = 0; f < NR_TD_F_COUNT; f++)
    fb->f[f].n_contra = 0; /* contradiction evidence is per config epoch */
}

void nr_td_fieldbook_fill_side_info(const nr_td_fieldbook_t *fb, nr_td_side_info_t *si)
{
  bool any = false;
  const nr_td_field_entry_t *t = &fb->f[NR_TD_F_TDRA];
  if (t->value != -1 && t->epoch == fb->epoch) {
    si->f_S = t->value & 0xF;
    si->f_L = (t->value >> 4) & 0x1F;
    si->f_mapping = (t->value >> 9) & 1;
    si->f_k0 = (t->value >> 10) & 0x3F;
    any = true;
  }
  const nr_td_field_entry_t *a = &fb->f[NR_TD_F_DMRS_ADD_POS];
  if (a->value != -1 && a->epoch == fb->epoch) {
    si->f_dmrs_add_pos = a->value;
    any = true;
  }
  const nr_td_field_entry_t *m = &fb->f[NR_TD_F_DMRS_MAX_LEN];
  if (m->value != -1 && m->epoch == fb->epoch) {
    si->f_dmrs_max_len = m->value;
    any = true;
  }
  if (any)
    si->f_conf = 1.0f;
}
