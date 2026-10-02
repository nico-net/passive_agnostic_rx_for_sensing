#include "nr_passive_cfg_epoch.h"
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

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_dci11_pin.c
 * \brief See the header for the contract, the R30/R32 background and the threading note.
 */

#include "nr_dci11_pin.h"
#include <stddef.h>
#include <stdatomic.h>

/* See the header's file comment: `valid` is declared plain `bool` so the struct's layout cannot
 * differ between this (always C) translation unit and a C++ test binary linking the same .o;
 * atomicity is added here, locally, via a cast -- standard C11 (atomic operations on an ordinary
 * object through a pointer to its atomic-qualified type are well-defined and layout-identical). */
static inline _Atomic bool *pin_valid(nr_dci11_pin_t *pin)
{
  return (_Atomic bool *)&pin->valid;
}

int nr_dci11_pin_select(nr_dci11_pin_t *pin, uint64_t current_cfg, const uint16_t *layout_ids, int n,
                        int settled, int preferred, bool has_stats, uint32_t trial_ok, uint32_t trial_tr,
                        uint32_t block_occasions, uint32_t giveup_trials)
{
  if (!nr_cfg_epoch_work_current()) return -1;
  if (settled >= 0)
    return settled;
  if (preferred >= 0)
    return preferred;

  bool valid = atomic_load_explicit(pin_valid(pin), memory_order_acquire);
  if (valid && pin->cfg != current_cfg) {
    /* A real cell-geometry change: the pin's own key no longer means what it used to. */
    atomic_store_explicit(pin_valid(pin), false, memory_order_release);
    pin->occ = 0;
    valid = false;
  }

  int found = -1;
  if (valid && layout_ids != NULL)
    for (int i = 0; i < n; i++)
      if (layout_ids[i] == pin->layout) {
        found = i;
        break;
      }

  if (found >= 0) {
    pin->occ++;
    const bool trial_giveup = has_stats && trial_tr >= giveup_trials && trial_ok == 0;
    const bool rotate = pin->occ >= block_occasions;
    if (trial_giveup || rotate) {
      atomic_store_explicit(pin_valid(pin), false, memory_order_release);
      pin->occ = 0;
      found = -1;
    }
  }
  return found;
}

void nr_dci11_pin_seed(nr_dci11_pin_t *pin, uint64_t current_cfg, uint16_t layout_id)
{
  if (!nr_cfg_epoch_work_current()) return;
  pin->layout = layout_id;
  pin->cfg = current_cfg;
  pin->occ = 0;
  atomic_store_explicit(pin_valid(pin), true, memory_order_release);
}

int nr_dci11_pin_round_robin(uint32_t *cursor, int n)
{
  if (n <= 0)
    return 0;
  const int idx = (int)(*cursor % (uint32_t)n);
  (*cursor)++;
  return idx;
}

bool nr_dci11_pin_is_valid(const nr_dci11_pin_t *pin)
{
  /* Same cast idiom as pin_valid() above; const-qualified here since a read never mutates the
   * struct, and pin_valid() itself takes a non-const pointer for its store-side callers. */
  return atomic_load_explicit((_Atomic bool *)&pin->valid, memory_order_acquire);
}
