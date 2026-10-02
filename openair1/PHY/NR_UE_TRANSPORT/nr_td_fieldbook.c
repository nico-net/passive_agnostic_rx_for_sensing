/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_td_fieldbook.h"
#include <string.h>

static bool has_rnti(const uint16_t *set, int n, uint16_t rnti)
{
  for (int i = 0; i < n; i++)
    if (set[i] == rnti)
      return true;
  return false;
}

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
  e->hint_value = -1;
  e->k0_alt = -1;
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

static void set_state(nr_td_fieldbook_t *fb, nr_td_field_entry_t *e, nr_td_field_state_t s)
{
  if (e->state != s) {
    e->state = s;
    fb->generation++;
  }
}

/* While unpromoted: promote the candidate with the most supporters if it has reached the threshold. */
static void try_promote(nr_td_fieldbook_t *fb, nr_td_field_entry_t *e, uint64_t slot)
{
  if (e->state == NR_TD_FS_PROMOTED || e->state == NR_TD_FS_SUSPECT)
    return;
  int best = -1;
  for (int i = 0; i < NR_TD_FB_MAX_CAND; i++)
    if (e->cand[i].value != -1 && e->cand[i].n_support >= fb->promote_rntis
        && (best < 0 || e->cand[i].n_support > e->cand[best].n_support))
      best = i;
  if (best < 0)
    return;
  e->value = e->cand[best].value;
  e->hint_value = e->value;
  e->n_contra = 0;
  e->k0_alt = -1;
  e->n_k0_alt = 0;
  e->epoch = fb->epoch;
  e->last_confirmed_slot = slot;
  set_state(fb, e, NR_TD_FS_PROMOTED);
}

static void contradict_entry(nr_td_fieldbook_t *fb, nr_td_field_entry_t *e, uint16_t rnti, uint64_t slot)
{
  if (e->state != NR_TD_FS_PROMOTED && e->state != NR_TD_FS_SUSPECT)
    return;
  add_rnti(e->contra, &e->n_contra, rnti);
  if (e->n_contra >= fb->withdraw_rntis) {
    /* withdraw: the old value's support is void; other candidates keep theirs and may be promoted right now */
    for (int i = 0; i < NR_TD_FB_MAX_CAND; i++)
      if (e->cand[i].value == e->value)
        e->cand[i].value = -1;
    e->value = -1;
    e->n_contra = 0;
    fb->n_withdrawn++;
    set_state(fb, e, NR_TD_FS_CANDIDATE);
    try_promote(fb, e, slot);
  } else if (e->n_contra >= 1) {
    set_state(fb, e, NR_TD_FS_SUSPECT);
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
  /* "new" = not already a supporter, independent of whether the (capped) insert succeeds */
  const bool is_new = c && !has_rnti(c->support, c->n_support, rnti);
  if (c)
    add_rnti(c->support, &c->n_support, rnti);
  if (e->state == NR_TD_FS_UNSEEN)
    e->state = NR_TD_FS_CANDIDATE; /* first evidence; not a pruning-relevant change, generation untouched */
  if (e->state == NR_TD_FS_PROMOTED || e->state == NR_TD_FS_SUSPECT) {
    if (e->value == v) {
      e->epoch = fb->epoch;
      e->last_confirmed_slot = slot;
      if (e->state == NR_TD_FS_SUSPECT && is_new && !has_rnti(e->contra, e->n_contra, rnti)) { /* re-confirmed by a further independent RNTI */
        e->n_contra = 0;
        set_state(fb, e, NR_TD_FS_PROMOTED);
      }
    } else {
      contradict_entry(fb, e, rnti, slot); /* a withdrawal here promotes the contradicting convergers' value in this same call */
    }
  } else {
    try_promote(fb, e, slot);
  }
}

#define TDRA_SLM_MASK 0x3FF /* S (4 bits), L (5), mapping (1): the pruning key; k0 is the bits above */
/* The k0 part of a PROMOTED/SUSPECT TDRA: a winner on the same (S, L, mapping) with another k0 is evidence against the k0 part only. */
static void tdra_k0_contradict(nr_td_fieldbook_t *fb, nr_td_field_entry_t *e, uint16_t rnti, int k0)
{
  if (e->k0_alt != k0) { /* contradicters must agree on ONE other k0 to re-learn it */
    e->k0_alt = k0;
    e->n_k0_alt = 0;
  }
  add_rnti(e->k0_alt_rnti, &e->n_k0_alt, rnti);
  if (e->n_k0_alt < fb->withdraw_rntis)
    return;
  const int32_t old = e->value, nv = nr_td_pack_tdra((old & 0xF), (old >> 4) & 0x1F, (old >> 9) & 1, k0);
  nr_td_field_cand_t *oldrow = NULL, *newrow = NULL;
  for (int i = 0; i < NR_TD_FB_MAX_CAND; i++) {
    if (e->cand[i].value == old)
      oldrow = &e->cand[i];
    if (e->cand[i].value == nv)
      newrow = &e->cand[i];
  }
  if (oldrow && !newrow)
    oldrow->value = nv; /* the S/L/mapping supporters carry over: only the k0 part changed */
  else if (oldrow)
    oldrow->value = -1;
  e->value = nv;
  e->hint_value = nv;
  e->k0_alt = -1;
  e->n_k0_alt = 0;
  fb->n_k0_relearned++;
  fb->generation++; /* hints (ordering) changed; pruning did not */
}

void nr_td_fieldbook_converged(nr_td_fieldbook_t *fb, uint16_t rnti, const nr_pdsch_cfg_hypothesis_t *h, uint64_t slot,
                               uint32_t pruned_fields)
{
  nr_td_field_entry_t *te = &fb->f[NR_TD_F_TDRA];
  const int32_t tv = nr_td_pack_tdra(h->tda_start, h->tda_length, h->mapping_type, h->k0);
  if ((te->state == NR_TD_FS_PROMOTED || te->state == NR_TD_FS_SUSPECT) && ((te->value ^ tv) & TDRA_SLM_MASK) == 0) {
    /* same (S, L, mapping): k0 is the only open question, and it was never pruned, so the winner is independent evidence for it */
    if (te->value == tv) {
      te->k0_alt = -1;
      te->n_k0_alt = 0;
    } else {
      tdra_k0_contradict(fb, te, rnti, h->k0);
    }
    if (!(pruned_fields & (1u << NR_TD_F_TDRA)) && te->value == tv)
      field_observe(fb, NR_TD_F_TDRA, rnti, tv, slot);
  } else if (!(pruned_fields & (1u << NR_TD_F_TDRA))) {
    field_observe(fb, NR_TD_F_TDRA, rnti, tv, slot);
  }
  if (!(pruned_fields & (1u << NR_TD_F_DMRS_ADD_POS)))
    field_observe(fb, NR_TD_F_DMRS_ADD_POS, rnti, h->dmrs_add_pos, slot);
  if (!(pruned_fields & (1u << NR_TD_F_DMRS_MAX_LEN)))
    field_observe(fb, NR_TD_F_DMRS_MAX_LEN, rnti, h->dmrs_max_len, slot);
}

void nr_td_fieldbook_bump_epoch(nr_td_fieldbook_t *fb)
{
  fb->epoch++;
  for (int f = 0; f < NR_TD_F_COUNT; f++) {
    nr_td_field_entry_t *e = &fb->f[f];
    const nr_td_field_state_t was = e->state;
    const int32_t hint = (was == NR_TD_FS_PROMOTED || was == NR_TD_FS_SUSPECT) ? e->value : e->hint_value;
    const uint32_t tick = e->tick, ep = e->epoch;
    entry_reset(e); /* all support and contradiction sets cleared: old-epoch evidence never counts */
    e->tick = tick;
    e->epoch = ep; /* config_epoch when last confirmed: kept */
    e->hint_value = hint;
    e->state = (was == NR_TD_FS_UNSEEN) ? NR_TD_FS_UNSEEN : NR_TD_FS_CANDIDATE;
  }
  fb->generation++;
}

nr_td_field_state_t nr_td_fieldbook_state(const nr_td_fieldbook_t *fb, nr_td_field_t f)
{
  return ((int)f < 0 || f >= NR_TD_F_COUNT) ? NR_TD_FS_UNSEEN : fb->f[f].state;
}

bool nr_td_fieldbook_prunes(const nr_td_fieldbook_t *fb, nr_td_field_t f, int32_t *value)
{
  if ((int)f < 0 || f >= NR_TD_F_COUNT || fb->f[f].state != NR_TD_FS_PROMOTED)
    return false;
  if (value)
    *value = fb->f[f].value;
  return true;
}

bool nr_td_fieldbook_hyp_matches(nr_td_field_t f, int32_t value, const nr_pdsch_cfg_hypothesis_t *h)
{
  switch (f) {
    case NR_TD_F_TDRA: /* (S, L, mapping) only: k0 is never a pruning key (see the header, BC6b) */
      return ((value ^ nr_td_pack_tdra(h->tda_start, h->tda_length, h->mapping_type, h->k0)) & TDRA_SLM_MASK) == 0;
    case NR_TD_F_DMRS_ADD_POS:
      return value == h->dmrs_add_pos;
    case NR_TD_F_DMRS_MAX_LEN:
      return value == h->dmrs_max_len;
    default:
      return false;
  }
}

uint32_t nr_td_fieldbook_generation(const nr_td_fieldbook_t *fb)
{
  return fb->generation;
}

void nr_td_fieldbook_force_promote(nr_td_fieldbook_t *fb, nr_td_field_t f, int32_t value)
{
  if ((int)f < 0 || f >= NR_TD_F_COUNT || value < 0)
    return;
  nr_td_field_entry_t *e = &fb->f[f];
  e->value = value;
  e->hint_value = value;
  e->n_contra = 0;
  e->k0_alt = -1;
  e->n_k0_alt = 0;
  e->epoch = fb->epoch;
  set_state(fb, e, NR_TD_FS_PROMOTED);
  fb->generation++; /* a forced re-promote of an already PROMOTED field must still be visible */
}

/* value while PROMOTED/SUSPECT, else the epoch-bump hint (ordering only) */
static int32_t shown_value(const nr_td_field_entry_t *e)
{
  return (e->state == NR_TD_FS_PROMOTED || e->state == NR_TD_FS_SUSPECT) ? e->value : e->hint_value;
}

void nr_td_fieldbook_fill_side_info(const nr_td_fieldbook_t *fb, nr_td_side_info_t *si)
{
  bool any = false;
  const int32_t t = shown_value(&fb->f[NR_TD_F_TDRA]);
  if (t != -1) {
    si->f_S = t & 0xF;
    si->f_L = (t >> 4) & 0x1F;
    si->f_mapping = (t >> 9) & 1;
    si->f_k0 = (t >> 10) & 0x3F;
    any = true;
  }
  const int32_t a = shown_value(&fb->f[NR_TD_F_DMRS_ADD_POS]);
  if (a != -1) {
    si->f_dmrs_add_pos = a;
    any = true;
  }
  const int32_t m = shown_value(&fb->f[NR_TD_F_DMRS_MAX_LEN]);
  if (m != -1) {
    si->f_dmrs_max_len = m;
    any = true;
  }
  if (any)
    si->f_conf = 1.0f;
}
