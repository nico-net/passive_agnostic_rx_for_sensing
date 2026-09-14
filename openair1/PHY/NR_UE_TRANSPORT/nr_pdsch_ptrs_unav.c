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

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_ptrs_unav.c */

#include "nr_pdsch_ptrs_unav.h"

/* K in {2,4}, L in {1,2,4}: TS 38.214 Tables 5.1.6.3-1 (time) and 5.1.6.3-2 (frequency). Ordered
 * densest-first so the most common configurations are tried early in a round-robin sweep. */
const uint8_t nr_ptrs_density_k[NR_PTRS_DENSITY_N] = {2, 2, 2, 4, 4, 4};
const uint8_t nr_ptrs_density_l[NR_PTRS_DENSITY_N] = {1, 2, 4, 1, 2, 4};

uint32_t nr_pdsch_ptrs_unav_res(uint16_t nb_rb, uint8_t start_symbol, uint8_t nb_symbols,
                                uint16_t dmrs_symb_pos, uint8_t k_density, uint8_t l_density,
                                uint8_t n_ports)
{
  if (nb_rb == 0 || nb_symbols == 0 || n_ports == 0
      || (k_density != 2 && k_density != 4)
      || (l_density != 1 && l_density != 2 && l_density != 4)) {
    return 0;
  }
  /* One PT-RS subcarrier per K resource blocks. A partial group still carries one, hence the
   * ceiling -- dropping it would under-count G on any allocation that is not a multiple of K. */
  const uint32_t sc_per_symbol = ((uint32_t)nb_rb + k_density - 1u) / k_density;

  /* Walk the allocation and count the symbols that actually carry PT-RS. The L-th-symbol cadence
   * restarts after each DM-RS symbol rather than running blindly from the allocation start: PT-RS
   * is not mapped on a DM-RS symbol, and the phase reference is re-established there. Counting
   * with a blind stride would mis-count exactly on the configurations with more than one DM-RS
   * symbol -- which is every additionalPosition > 0 cell. */
  uint32_t symbols = 0;
  int since_ref = -1;   /* -1 until the first DM-RS symbol establishes the reference */
  for (int s = start_symbol; s < start_symbol + nb_symbols; s++) {
    const bool is_dmrs = (s >= 0 && s < 16) && ((dmrs_symb_pos >> s) & 0x1u);
    if (is_dmrs) {
      since_ref = 0;    /* cadence restarts after the DM-RS */
      continue;
    }
    if (since_ref < 0) {
      /* Data symbols before any DM-RS carry no phase reference to track. */
      continue;
    }
    if ((since_ref % l_density) == 0) {
      symbols++;
    }
    since_ref++;
  }
  return sc_per_symbol * symbols * (uint32_t)n_ports;
}
