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
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_map.h
 * \brief Phase 3 Technique A: map a dedicated CORESET's footprint by DM-RS correlation across the
 * WHOLE carrier, with no assumed rb_offset/n_rb. Generalizes the existing
 * nr_pdcch_blind_dmrs_probe diagnostic (dci_nr.c), which already proved this correlation approach
 * live -- see PHASE1_CSS0_AUTOCONF_HANDOVER.md's Task 4 finding ("DM-RS corr 0.974" on real
 * signal an independent occupancy heuristic missed entirely).
 *
 * Detection, not decode: no length hypothesis needed, PCI-derived scrambling only (the DEDICATED
 * CORESET's own DM-RS scrambling ID defaults to the PCI unless the network overrides it -- see
 * TS 38.211 7.4.1.3.1 -- and PCI is already known from PSS/SSS on any cell).
 */
#ifndef NR_PDCCH_CORESET_MAP_H
#define NR_PDCCH_CORESET_MAP_H

#include <stdint.h>
#include "PHY/impl_defs_top.h"  // c16_t

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nr_pdcch_coreset_candidate_s {
  int    rb_offset;  ///< First RB of this candidate 6-RB (one-CCE) window
  double corr;        ///< Normalised correlation magnitude, [0,1]
} nr_pdcch_coreset_candidate_t;

/**
 * @brief Correlate every 6-RB window across [0, n_rb_carrier) at one (slot, symbol) against the
 *        regenerated PDCCH DM-RS, returning windows whose |corr| clears the significance bar.
 *
 * @return number of candidates written to candidates_out (<= max_candidates), sorted by
 *         descending corr.
 */
int nr_pdcch_coreset_map_scan(const c16_t* rxdataF,
                              int          ofdm_symbol_size,
                              int          n_rb_carrier,
                              int          first_carrier_offset,
                              uint16_t     scrambling_id,
                              int          slot,
                              int          symbol,
                              nr_pdcch_coreset_candidate_t* candidates_out,
                              int          max_candidates);

#ifdef __cplusplus
}
#endif

#endif
