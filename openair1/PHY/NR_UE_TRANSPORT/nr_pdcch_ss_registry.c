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

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ss_registry.c */

#include "nr_pdcch_ss_registry.h"

#include <stddef.h>
#include <string.h>

static bool same_entry(const nr_pdcch_ss_entry_t *a, const nr_pdcch_ss_entry_t *b)
{
  /* Deliberately ignores coreset_id: two entries with different ids but identical geometry monitor
   * the SAME occasions, so keeping both doubles the scan cost and splits the evidence for one
   * physical search space between two counters -- which can leave each below the retirement bar
   * while the space itself is plainly alive. */
  return a->coreset_duration == b->coreset_duration && a->coreset_n_rbs == b->coreset_n_rbs
         && a->ss_type == b->ss_type && a->ss_first_symbol == b->ss_first_symbol
         && a->ss_period_slots == b->ss_period_slots && a->ss_offset_slots == b->ss_offset_slots
         && a->bwp_start == b->bwp_start && a->bwp_size == b->bwp_size
         && memcmp(a->al_candidates, b->al_candidates, sizeof(a->al_candidates)) == 0;
}

int nr_pdcch_ss_register(nr_pdcch_ss_registry_t *r, const nr_pdcch_ss_entry_t *e)
{
  if (r == NULL || e == NULL || e->ss_period_slots == 0 || e->bwp_size == 0
      || e->coreset_duration < 1 || e->coreset_duration > 3) {
    return -1;
  }
  /* An offset at or beyond the period never matches a slot, so the entry would be scanned forever
   * and never monitor anything -- silently. */
  if (e->ss_offset_slots >= e->ss_period_slots) {
    return -1;
  }
  bool any_candidate = false;
  for (int i = 0; i < 5; i++) {
    if (e->al_candidates[i]) {
      any_candidate = true;
    }
  }
  if (!any_candidate) {
    return -1;   /* a search space with no candidates at any aggregation level decodes nothing */
  }
  for (int i = 0; i < r->n; i++) {
    if (same_entry(&r->entry[i], e)) {
      return -1;
    }
  }
  if (r->n >= NR_PDCCH_SS_MAX) {
    return -1;
  }
  const int idx = r->n++;
  r->entry[idx] = *e;
  r->occasions[idx] = 0;
  r->accepts[idx] = 0;
  r->confirmed[idx] = 0;
  r->retired[idx] = false;
  return idx;
}

bool nr_pdcch_ss_monitors_slot(const nr_pdcch_ss_entry_t *e, uint32_t absolute_slot)
{
  if (e == NULL || e->ss_period_slots == 0) {
    return false;
  }
  return (absolute_slot % e->ss_period_slots) == e->ss_offset_slots;
}

void nr_pdcch_ss_observe(nr_pdcch_ss_registry_t *r, int idx, bool accepted, bool confirmed)
{
  if (r == NULL || idx < 0 || idx >= r->n) {
    return;
  }
  r->occasions[idx]++;
  if (accepted) {
    r->accepts[idx]++;
    if (confirmed) {
      r->confirmed[idx]++;
    }
  }
}

int nr_pdcch_ss_live(const nr_pdcch_ss_registry_t *r)
{
  if (r == NULL) {
    return 0;
  }
  int n = 0;
  for (int i = 0; i < r->n; i++) {
    if (!r->retired[i]) {
      n++;
    }
  }
  return n;
}

int nr_pdcch_ss_retire_barren(nr_pdcch_ss_registry_t *r, uint64_t min_occasions)
{
  if (r == NULL || min_occasions == 0) {
    return 0;
  }
  int retired = 0;
  for (int i = 0; i < r->n; i++) {
    if (r->retired[i] || r->occasions[i] < min_occasions) {
      continue;
    }
    /* CONFIRMED accepts only. A raw accept is not evidence a search space is real: a blind search
     * over noise produces an in-range RNTI occasionally, which is exactly how a spurious CORESET
     * looks alive. An RNTI seen more than once is not something noise reproduces. */
    if (r->confirmed[i] > 0) {
      continue;
    }
    /* Never retire the last live entry. A receiver monitoring nothing cannot recover: it would
     * stop producing the very evidence that would bring an entry back. */
    if (nr_pdcch_ss_live(r) <= 1) {
      break;
    }
    r->retired[i] = true;
    retired++;
  }
  return retired;
}
