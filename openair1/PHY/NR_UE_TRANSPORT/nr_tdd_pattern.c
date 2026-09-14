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

static bool pattern_fits(const nr_tdd_pattern_t *p)
{
  if (p->period_slots == 0) {
    return false;
  }
  /* The mixed slot exists only if it has symbols in it; otherwise the period is whole slots only.
   * Counting it unconditionally would over-run the period on a pure DL/UL pattern. */
  const uint32_t mixed = (p->dl_symbols || p->ul_symbols) ? 1u : 0u;
  return (uint32_t)p->dl_slots + mixed + (uint32_t)p->ul_slots <= (uint32_t)p->period_slots;
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

static nr_tdd_slot_dir_t direction_in_pattern(const nr_tdd_pattern_t *p, uint16_t s)
{
  if (s < p->dl_slots) {
    return NR_TDD_SLOT_DL;
  }
  const uint16_t mixed = (p->dl_symbols || p->ul_symbols) ? 1u : 0u;
  if (mixed && s == p->dl_slots) {
    return NR_TDD_SLOT_MIXED;
  }
  if (s < (uint16_t)(p->dl_slots + mixed + p->ul_slots)) {
    return NR_TDD_SLOT_UL;
  }
  /* Slots past the configured ones inside the period are flexible. Treating them as DOWNLINK is
   * the safe default for a monitor: a missed downlink slot loses real grants, whereas scanning a
   * flexible slot that turns out to be uplink costs only the CPU this was meant to save. */
  return NR_TDD_SLOT_DL;
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
  const nr_tdd_slot_dir_t d = nr_tdd_slot_direction(cfg, absolute_slot);
  return d == NR_TDD_SLOT_DL || d == NR_TDD_SLOT_MIXED;
}
