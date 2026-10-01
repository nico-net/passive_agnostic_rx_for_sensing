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
  e->candidate = -1;
}

void nr_td_fieldbook_init(nr_td_fieldbook_t *fb, int promote_rntis, int withdraw_rntis)
{
  memset(fb, 0, sizeof(*fb));
  for (int f = 0; f < NR_TD_F_COUNT; f++)
    entry_reset(&fb->f[f]);
  fb->promote_rntis = promote_rntis > 0 ? promote_rntis : 2;
  fb->withdraw_rntis = withdraw_rntis > 0 ? withdraw_rntis : 2;
}

int32_t nr_td_pack_tdra(int S, int L, int mapping, int k0)
{
  return (int32_t)(S | (L << 4) | (mapping << 9) | (k0 << 10));
}

void nr_td_fieldbook_contradict(nr_td_fieldbook_t *fb, uint16_t rnti, nr_td_field_t f)
{
  if ((int)f < 0 || f >= NR_TD_F_COUNT)
    return;
  nr_td_field_entry_t *e = &fb->f[f];
  if (e->value == -1)
    return;
  add_rnti(e->contra, &e->n_contra, rnti);
  if (e->n_contra >= fb->withdraw_rntis) {
    entry_reset(e);
  }
}

static void field_observe(nr_td_fieldbook_t *fb, nr_td_field_t f, uint16_t rnti, int32_t v, uint64_t slot)
{
  nr_td_field_entry_t *e = &fb->f[f];
  if (e->value == v) {
    add_rnti(e->support, &e->n_support, rnti);
    e->epoch = fb->epoch;
    e->last_confirmed_slot = slot;
  } else if (e->value != -1) {
    nr_td_fieldbook_contradict(fb, rnti, f);
  } else {
    if (e->candidate == v) {
      add_rnti(e->support, &e->n_support, rnti);
    } else {
      e->candidate = v;
      e->n_support = 0;
      add_rnti(e->support, &e->n_support, rnti);
    }
    if (e->n_support >= fb->promote_rntis) {
      e->value = v;
      e->n_contra = 0;
      e->epoch = fb->epoch;
      e->last_confirmed_slot = slot;
    }
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
}

void nr_td_fieldbook_fill_side_info(const nr_td_fieldbook_t *fb, nr_td_side_info_t *si)
{
  bool any = false;
  const nr_td_field_entry_t *t = &fb->f[NR_TD_F_TDRA];
  if (t->value != -1 && t->epoch == fb->epoch) {
    si->f_S = t->value & 0xF;
    si->f_L = (t->value >> 4) & 0x1F;
    si->f_mapping = (t->value >> 9) & 1;
    si->f_k0 = t->value >> 10;
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
