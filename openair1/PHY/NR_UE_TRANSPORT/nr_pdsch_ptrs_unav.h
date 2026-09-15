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

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_ptrs_unav.h
 * \brief PT-RS resource-element count, so a PT-RS grant can be DECODED instead of refused.
 *
 * WHY GRANTS WITH PT-RS WERE REFUSED. nr_pdsch_passive_decode() returned early on
 * `pduBitmap & 0x1`, and nr_get_G() was called with unav_res = 0 on the strength of that guard.
 * PT-RS steals resource elements from PDSCH, so with unav_res = 0 the rate matcher is handed the
 * wrong G and the transport block cannot decode. Refusing was therefore the honest thing to do --
 * but it costs every such grant entirely: no TB CRC evidence for Technique D or the DCI-1_1 layout
 * sweep, and no data-aided row.
 *
 * WHY THE MISSING RRC CONFIG IS NOT A BLOCKER. The PT-RS densities come from
 * ptrs-DensityRecommendationDL (frequency thresholds n_rb0/n_rb1, time thresholds
 * ptrs_mcs1/2/3), which is dedicated RRC and invisible to a passive receiver. But the densities
 * those thresholds SELECT are a tiny discrete set -- K in {2,4} and L in {1,2,4}, six combinations
 * in total (TS 38.214 Tables 5.1.6.3-1 and 5.1.6.3-2). So the density is swept and the TB CRC
 * arbitrates, exactly as every other unknown in this receiver is handled. A wrong density yields a
 * wrong G and simply fails to decode; it cannot corrupt anything.
 */

#ifndef __NR_PDSCH_PTRS_UNAV_H__
#define __NR_PDSCH_PTRS_UNAV_H__

#include <stdbool.h>
#include <stdint.h>

/// The six (K, L) density pairs TS 38.214 can select. Swept, not assumed.
#define NR_PTRS_DENSITY_N 6
extern const uint8_t nr_ptrs_density_k[NR_PTRS_DENSITY_N];
extern const uint8_t nr_ptrs_density_l[NR_PTRS_DENSITY_N];

/** Number of REs PT-RS removes from PDSCH in one slot, for nr_get_G()'s `unav_res`.
 *
 * PT-RS occupies one subcarrier every K PRBs, on every L-th symbol of the allocation, and is NOT
 * mapped on a DM-RS symbol (TS 38.211 7.4.1.2.2) -- the count therefore depends on where the DM-RS
 * sits, which is why `dmrs_symb_pos` is an input rather than a constant.
 *
 * `n_ports` scales the result: each PT-RS port costs its own REs.
 * Returns 0 for arguments that describe no PT-RS, which is also the correct `unav_res` then. Pure.
 */
uint32_t nr_pdsch_ptrs_unav_res(uint16_t nb_rb, uint8_t start_symbol, uint8_t nb_symbols,
                                uint16_t dmrs_symb_pos, uint8_t k_density, uint8_t l_density,
                                uint8_t n_ports);

/* ---- PT-RS DENSITY SWEEP ------------------------------------------------------------------------
 * Whether a grant carries PT-RS, and at which density, is set by dedicated RRC (phaseTrackingRS,
 * ptrs-DensityRecommendationDL) that a passive receiver never sees. But the choice is a SEVEN-element
 * set -- absent, or K in {2,4} x L in {1,2,4} -- so it is swept against the TB CRC like everything
 * else here: each eligible grant is decoded under one arm, arms are chosen by their Wilson UPPER
 * bound (optimistic, deterministic, concentrates on the winner without deleting anyone), and the
 * cell-wide answer is LATCHED once one arm's lower bound clears every rival's upper bound. Without
 * this, a PT-RS cell's wide high-MCS grants cannot decode at all: G is wrong by the PT-RS REs. */
#define NR_PTRS_ARMS 7
typedef struct {
  uint32_t ok[NR_PTRS_ARMS], tr[NR_PTRS_ARMS];
  int latched;   /* -1 until decided */
} nr_ptrs_sweep_t;
void nr_ptrs_sweep_init(nr_ptrs_sweep_t *s);
int  nr_ptrs_sweep_pick(const nr_ptrs_sweep_t *s);                 /* arm to decode this grant under */
int  nr_ptrs_sweep_feed(nr_ptrs_sweep_t *s, int arm, bool tb_ok);   /* returns the latched arm or -1 */
/* arm 0 = no PT-RS; 1..6 = (K, L). Returns false for arm 0 (nothing to set). */
bool nr_ptrs_sweep_arm(int arm, uint8_t *K, uint8_t *L);
#endif
