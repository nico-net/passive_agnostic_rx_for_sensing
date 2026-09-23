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
#include <stdbool.h>
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
/**
 * @brief Correlate a KNOWN CORESET in a buffer that has just produced a CRC-verified DCI.
 *
 * Paired control for the all-calls statistic: conditioning on an accept proves the CORESET was
 * occupied in that slot, removing the duty-cycle dilution that every averaged measurement suffers.
 * Env-gated by ISAC_COREMAP_ONACCEPT; a no-op otherwise.
 */
void nr_pdcch_coreset_map_accept_probe(const c16_t *rxdataF, int ofdm_symbol_size, int n_rb_carrier,
                                       int first_carrier_offset, uint16_t scrambling_id, int slot,
                                       int symbol, int cs_start, int cs_nrb);

/** Align the scan's 6-RB window grid to a CORESET starting at `start_rb` (uses start_rb % 6).
 *  Measured worth: +1 RB took the on-accept correlation 0.510 -> 0.607 on Swisscom PCI 382. */
void nr_pdcch_coreset_map_set_phase_hint(int start_rb);
/** CORESET#0 extent from the MIB, so stage 1 scores it against its own DM-RS reference. */
void nr_pdcch_coreset_map_set_css0(int first_rb, int n_rb);
/** Stage-1 blind nID sweep (ISAC_COREMAP_IDSWEEP=1): want() says whether to snapshot this slot;
 * push() hands over all 14 FEP'd symbols ([14][fft], antenna 0). Heavy work is on a worker thread. */
int nr_pdcch_coreset_map_idsweep_want(uint32_t abs_slot, int sps); /* symbol to capture, or -1 */
void nr_pdcch_coreset_map_idsweep_push(const c16_t *rxF_sym, int fft, int fco, int n_rb, int slot, int sym, uint16_t pci,
                                       int sps);
/** The phase in force, so callers mapping a window index back to an RB add it too. */
int nr_pdcch_coreset_map_get_phase(void);

int nr_pdcch_coreset_map_scan(const c16_t* rxdataF,
                              int          ofdm_symbol_size,
                              int          n_rb_carrier,
                              int          first_carrier_offset,
                              uint16_t     scrambling_id,
                              int          slot,
                              int          symbol,
                              nr_pdcch_coreset_candidate_t* candidates_out,
                              int          max_candidates);

/** Conjugated PDCCH DM-RS for n_rb RBs of one (slot, symbol); index 0 = the reference RB. */
void nr_pdcch_coreset_pilot(uint16_t scrambling_id, int slot, int symbol, int n_rb, c16_t *pilot);
/** |corr| of the 6-RB window at rb_offset against pilots referenced to ref_rb (0 = CRB 0, the spec;
 *  the BWP start on OAI). -1 when the window is not covered by the pilots. */
double nr_pdcch_coreset_window_corr(const c16_t *rxdataF, int ofdm_symbol_size, int first_carrier_offset,
                                    const c16_t *pilot, int n_pilot_rb, int rb_offset, int ref_rb);

/* Per-occasion prefix sums of Y*conj(DMRS), |Y|^2 and |DMRS|^2. No history, RNTI or
 * detector threshold: scores are a scheduling priority, not proof of a PDCCH. */
#define NR_PDCCH_RANK_MAX_RB 275
#define NR_PDCCH_RANK_MAX_CAND 64
typedef struct {
  int n_rb, duration;
  double re[3][NR_PDCCH_RANK_MAX_RB + 1], im[3][NR_PDCCH_RANK_MAX_RB + 1];
  double py[3][NR_PDCCH_RANK_MAX_RB + 1], px[3][NR_PDCCH_RANK_MAX_RB + 1];
} nr_pdcch_dmrs_rank_grid_t;

bool nr_pdcch_dmrs_rank_grid(nr_pdcch_dmrs_rank_grid_t *grid, const c16_t *rxdataF,
                             int fft_size, int first_carrier_offset, int n_rb, uint16_t id,
                             int slot, int first_symbol, int duration, int reference_rb);
/* Physical RBs in ascending frequency, as in the production demapper. Each RB includes all
 * CORESET symbols. Returns -1 for invalid/unsupported mapping or insufficient output space. */
int nr_pdcch_candidate_rbs(int span, int duration, int bundle, int interleaver, int shift,
                            int cce, int al, uint16_t *rbs, int capacity);
double nr_pdcch_dmrs_candidate_score(const nr_pdcch_dmrs_rank_grid_t *grid, int offset,
                                       int span, int bundle, int interleaver, int shift, int cce, int al);
/* Retain half per AL (at least two if available): strongest plus one rotating exploration
 * candidate per AL. visit is a consecutive opportunity counter, not SFN/slot (TDD can alias).
 * full ranks without dropping. order[] contains original indices, preserving candidate identity. */
int nr_pdcch_dmrs_candidate_order(const double *scores, const uint8_t *al, int n,
                                    uint64_t visit, bool full, uint8_t *order);

/* 38.213 UE-specific search-space prior. The SearchSpace/CORESET IDs and the configured
 * nrofCandidates are not available to a passive receiver, so this marginalizes over their
 * complete standard domains (CORESET ID modulo 3 and {1,2,3,4,5,6,8} candidates). It is a
 * ranking score only: zero support must never be used as a rejection rule. */
void nr_pdcch_uss_candidate_supports(int n_cces, int slot, const uint16_t *rntis, int n_rntis,
                                     const uint16_t *cce, const uint8_t *al, int n_candidates,
                                     uint16_t *support);

#ifdef __cplusplus
}
#endif

#endif
