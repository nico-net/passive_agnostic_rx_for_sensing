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

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_tdd_pattern.c */

#include "nr_tdd_pattern.h"

#include <stddef.h>

uint16_t nr_tdd_period_slots(uint16_t periodicity_x10_ms, uint8_t mu)
{
  if (periodicity_x10_ms == 0 || mu > 4) {
    return 0;
  }
  /* slots per ms = 2^mu, so slots = periodicity_ms * 2^mu = (x10 * 2^mu) / 10. The division must
   * be exact: 0.5 ms at mu=0 is half a slot and is not a legal configuration at that numerology,
   * and rounding it to 0 or 1 would silently produce a pattern that never matches the cell. */
  const uint32_t num = (uint32_t)periodicity_x10_ms << mu;
  if ((num % 10u) != 0u) {
    return 0;
  }
  return (uint16_t)(num / 10u);
}

/* TS 38.213 11.1 placement (K40): the nrofDownlinkSlots are the FIRST slots of the period and the nrofUplinkSlots the
 * LAST; nrofDownlinkSymbols are the first symbols of the slot right after the last full DL slot, nrofUplinkSymbols the
 * last symbols of the slot right before the first full UL slot; every other symbol is flexible. (The model before K40
 * put the UL slots right after the mixed slot and called the rest of the period DL, which marked flexible slots as UL
 * whenever dl_slots + mixed + ul_slots < period.) */
static bool pattern_fits(const nr_tdd_pattern_t *p)
{
  if (p->period_slots == 0 || p->dl_symbols > 13 || p->ul_symbols > 13)
    return false;
  if ((uint32_t)p->dl_slots + (uint32_t)p->ul_slots > (uint32_t)p->period_slots)
    return false;
  if (p->dl_symbols && p->dl_slots >= p->period_slots)
    return false; /* no slot left for the DL symbols */
  if (p->ul_symbols && p->ul_slots >= p->period_slots)
    return false;
  const uint32_t dl_sym_slot = p->dl_slots, ul_sym_slot = (uint32_t)p->period_slots - p->ul_slots - 1u;
  if (p->dl_symbols && p->ul_symbols) {
    if (dl_sym_slot > ul_sym_slot)
      return false; /* the DL symbols would land in a full UL slot */
    if (dl_sym_slot == ul_sym_slot && p->dl_symbols + p->ul_symbols > 14)
      return false;
  }
  if (p->dl_symbols && dl_sym_slot >= (uint32_t)p->period_slots - p->ul_slots)
    return false;
  if (p->ul_symbols && p->dl_slots > ul_sym_slot)
    return false;
  return true;
}

bool nr_tdd_config_init(nr_tdd_config_t *out, const nr_tdd_pattern_t *p1, const nr_tdd_pattern_t *p2)
{
  if (out == NULL || p1 == NULL) {
    return false;
  }
  out->valid = false;
  out->p1 = *p1;
  if (p2 != NULL) {
    out->p2 = *p2;
  } else {
    out->p2 = (nr_tdd_pattern_t){0};
  }
  if (!pattern_fits(&out->p1)) {
    return false;
  }
  if (out->p2.period_slots != 0 && !pattern_fits(&out->p2)) {
    return false;
  }
  out->valid = true;
  return true;
}

/* Shape of slot s of one pattern: leading DL symbols and trailing UL symbols (14 = the whole slot). */
static void slot_shape(const nr_tdd_pattern_t *p, uint16_t s, int *dl_lead, int *ul_trail)
{
  *dl_lead = *ul_trail = 0;
  if (s < p->dl_slots) {
    *dl_lead = 14;
    return;
  }
  if ((uint32_t)s >= (uint32_t)p->period_slots - p->ul_slots) {
    *ul_trail = 14;
    return;
  }
  if (s == p->dl_slots)
    *dl_lead = p->dl_symbols;
  if ((uint32_t)s == (uint32_t)p->period_slots - p->ul_slots - 1u)
    *ul_trail = p->ul_symbols;
}
static nr_tdd_slot_dir_t direction_in_pattern(const nr_tdd_pattern_t *p, uint16_t s)
{
  int dl, ul;
  slot_shape(p, s, &dl, &ul);
  if (dl == 14)
    return NR_TDD_SLOT_DL;
  if (ul == 14)
    return NR_TDD_SLOT_UL;
  return (dl || ul) ? NR_TDD_SLOT_MIXED : NR_TDD_SLOT_FLEXIBLE;
}

nr_tdd_slot_dir_t nr_tdd_slot_direction(const nr_tdd_config_t *cfg, uint32_t absolute_slot)
{
  if (cfg == NULL || !cfg->valid) {
    return NR_TDD_SLOT_DL;   /* unknown configuration must not suppress monitoring */
  }
  const uint32_t total = (uint32_t)cfg->p1.period_slots + (uint32_t)cfg->p2.period_slots;
  if (total == 0) {
    return NR_TDD_SLOT_DL;
  }
  const uint32_t s = absolute_slot % total;
  if (s < cfg->p1.period_slots) {
    return direction_in_pattern(&cfg->p1, (uint16_t)s);
  }
  return direction_in_pattern(&cfg->p2, (uint16_t)(s - cfg->p1.period_slots));
}

bool nr_tdd_slot_has_downlink(const nr_tdd_config_t *cfg, uint32_t absolute_slot)
{
  /* Flexible symbols may carry PDCCH (and a DCI-scheduled PDSCH): only a slot that is UL in every symbol is skipped. */
  return nr_tdd_slot_direction(cfg, absolute_slot) != NR_TDD_SLOT_UL;
}

int nr_tdd_pdsch_last_symbol(const nr_tdd_config_t *cfg, uint32_t absolute_slot)
{
  if (cfg == NULL || !cfg->valid)
    return 13;
  const uint32_t total = (uint32_t)cfg->p1.period_slots + (uint32_t)cfg->p2.period_slots;
  if (total == 0)
    return 13;
  const uint32_t s = absolute_slot % total;
  const nr_tdd_pattern_t *p = s < cfg->p1.period_slots ? &cfg->p1 : &cfg->p2;
  int dl, ul;
  slot_shape(p, (uint16_t)(s < cfg->p1.period_slots ? s : s - cfg->p1.period_slots), &dl, &ul);
  return 13 - ul; /* -1 for a full UL slot */
}
