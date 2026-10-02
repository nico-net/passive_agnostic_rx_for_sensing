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

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_dci_history.h
 * \brief BC9: per-RNTI DL DCI history ring and the DCI-adjacency k0 evidence built on it
 *        (k0 speed-recovery notes 2026-10-01, sections 1.1, 1.2, 1.4, 1.5).
 *
 * WHAT IT IS. A bounded ring of the DL DCIs the blind PDCCH monitor ACCEPTED, per RNTI, written at accept time BEFORE
 * any grant drop (rv0-only, per-slot cap, gates): a dropped grant still occupies its PDSCH slot. Readers are the decode
 * queue consumers (certified flag) and the monitor itself (deterministic k0 exclusions). One mutex; a few hundred
 * writes/s.
 *
 * TWO USES, TWO EVIDENCE CLASSES.
 *  1. nr_dci_hist_k0_certified() -- the per-world occupant check of notes 1.2, used ONLY as the `certified` flag of the
 *     fast-path levers C/P (statistical evidence stratum; never a prune). For a grant g (RNTI R, row i, DCI slot t)
 *     decoded under leader offset k_L, and every alive sibling offset k_s != k_L: in the world "truth is k_s" the slot
 *     u = t + k_L carries the TB of the row-i DCI of R observed at t + (k_L - k_s) (or of an observed DCI of a row whose
 *     k0 is deterministically certified and lands on u). g is certified iff for EVERY sibling such an occupant was
 *     OBSERVED and EVERY observed occupant is INCOMPATIBLE with g. Then L's pass on g in that world needs a CRC
 *     accident. Absence of a DCI (missed, not scanned, evicted, outside the window) is AMBIGUOUS, never "trap-free".
 *     An empty sibling set never certifies.
 *  2. nr_dci_hist_adj_exclusions() -- DETERMINISTIC (given A1-A3 below) k0 exclusions used as hard prunes: a DCI X of a
 *     row whose k0 is certified ({k_x}) occupies slot t_x + k_x; any OTHER observed DCI Y of the same RNTI (same
 *     configuration) therefore cannot have its PDSCH there, i.e. row(Y) cannot have k0 = t_x + k_x - t_y. Only POSITIVE
 *     observations are used, so a missed DCI can never produce an exclusion.
 *
 * SOUNDNESS ASSUMPTIONS (notes 1.5 / 7.2).
 *  A1 one unicast PDSCH per RNTI per slot per carrier (Rel-15 FR1 single-TRP; SPS uses CS-RNTI, not the C-RNTI contexts
 *     this feeds). Without A1 neither use is valid.
 *  A2 the truth's k0 is in the catalogue universe ({0,1} plus k0 layers the k0 oracle added): "row k0 certified" means
 *     the deterministic constraints leave exactly one k0 of that universe.
 *  A3 PDCCH false accepts are rare: an exclusion is wrong only if X or Y was a false accept carrying R (~1e-5/slot before
 *     the re-encode mismatch gate, notes 1.5) -- the same class of argument as the CRC-24 budget. Miss rate does not
 *     matter for (2) (absence is never used) and only lowers f_S for (1).
 *  The k0 of a TDRA row is constant under one configuration key (layout x RRC); a reconfiguration is caught by the
 *  sweep's reopen, which clears certifications.
 *
 * COMPATIBILITY (notes 1.2). X and g are INCOMPATIBLE only on a provable difference of the receiver computation:
 * different PRB set (same RA type and VRB mapping; RA type 0 vs 1 is never compared), different DM-RS ports / CDM groups
 * / nSCID, different rv, or -- for a same-row occupant whose symbols equal g's -- (TBS, Qm) different under EVERY pair
 * of alive MCS tables (OAI nr_get_Qm_dl / nr_get_code_rate_dl / nr_compute_tbs). A reserved MCS (R = 0) is never a
 * difference. HARQ pid / NDI are ignored. Unknown fields never make a pair incompatible.
 */

#ifndef __NR_DCI_HISTORY_H__
#define __NR_DCI_HISTORY_H__

#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NR_DCI_HIST_DEPTH 64   /* entries kept per RNTI (oldest overwritten) */
#define NR_DCI_HIST_WINDOW 64  /* slots: an entry older than this, relative to the RNTI's newest write, is not visible */
#define NR_DCI_HIST_RNTIS 16   /* RNTIs tracked; a new RNTI evicts the least recently written one */
#define NR_DCI_HIST_K0_MAX 32
#define NR_DCI_HIST_ROWS 16    /* TDRA rows (tda index < 16) */

typedef struct {
  uint32_t abs_slot;     ///< DCI slot, frame * slots_per_frame + slot (wraps at the ring's slot period)
  uint64_t cfg;          ///< Technique D configuration key the DCI was interpreted under (rows are per key)
  uint16_t rnti;
  bool     dci11;        ///< format 1_1 (only these carry a sweep TDRA row)
  uint8_t  tda;          ///< TDRA row index
  uint8_t  mcs, rv, ndi, harq_pid;
  uint8_t  ra_type0, vrb_to_prb, rbg_size;
  uint16_t start_rb, num_rb, rbg_bwp_start;
  uint32_t rbg_bitmap;
  uint16_t dmrs_ports;
  uint8_t  n_cdm, nscid;
} nr_dci_hist_entry_t;

typedef struct {
  uint16_t rnti;
  uint64_t touched;      ///< LRU clock of the last write
  uint32_t newest;       ///< abs slot of the newest entry
  int      n, w;         ///< valid entries, next write index
  nr_dci_hist_entry_t e[NR_DCI_HIST_DEPTH];
} nr_dci_hist_rnti_t;

typedef struct {
  pthread_mutex_t lock;
  uint32_t period;       ///< slot numbering period (1024 * slots_per_frame); 0 = no wrap
  uint64_t clock;
  nr_dci_hist_rnti_t r[NR_DCI_HIST_RNTIS];
} nr_dci_hist_t;

/** Geometry of the decode under the leader hypothesis (same-row occupants share it): number of PDSCH symbols, DM-RS
 *  symbol mask, DM-RS config type (0 = type 1), layers, xOverhead PRBs. */
typedef struct {
  uint8_t  nb_symb;
  uint16_t dmrs_mask;
  uint8_t  dmrs_type;
  uint8_t  nl;
  uint16_t xoh;
} nr_dci_geom_t;

/** Deterministic allowed-k0 set of a row (over-approximation including the universe of A2); popcount 1 = certified. */
typedef uint64_t (*nr_dci_row_k0_fn)(void *arg, uint64_t cfg, uint16_t rnti, uint8_t tda);

void nr_dci_hist_init(nr_dci_hist_t *h, uint32_t period);
void nr_dci_hist_push(nr_dci_hist_t *h, const nr_dci_hist_entry_t *e);
/** Visible entries of `rnti` at exactly `abs_slot` (newest first). Returns the count (<= max). */
int nr_dci_hist_at(nr_dci_hist_t *h, uint16_t rnti, uint32_t abs_slot, nr_dci_hist_entry_t *out, int max);
/** Visible entries of `rnti` within +-span slots of `around` (newest first). Returns the count (<= max). */
int nr_dci_hist_near(nr_dci_hist_t *h, uint16_t rnti, uint32_t around, int span, nr_dci_hist_entry_t *out, int max);
/** Signed slot difference a - b in the ring's numbering. */
int32_t nr_dci_hist_diff(const nr_dci_hist_t *h, uint32_t a, uint32_t b);

/** True on a provable computation difference (see the header). geo != NULL: x is a same-row occupant whose symbols
 *  equal g's, so the MCS fields are compared over every pair of tables in table_mask (bit t = table t alive). */
bool nr_dci_hist_incompatible(const nr_dci_hist_entry_t *g, const nr_dci_hist_entry_t *x, uint8_t table_mask,
                              const nr_dci_geom_t *geo);

/** Use 1 (statistical stratum): is g k0-unambiguous for leader offset k_lead against every sibling offset in
 *  sib_k0_mask (bit k_lead ignored)? row_k0 may be NULL (no certified neighbour rows). */
bool nr_dci_hist_k0_certified(nr_dci_hist_t *h, const nr_dci_hist_entry_t *g, int k_lead, uint64_t sib_k0_mask,
                              uint8_t table_mask, const nr_dci_geom_t *geo, nr_dci_row_k0_fn row_k0, void *arg);

/** Use 2 (deterministic): k0 values made impossible by the just-pushed DCI x and the visible DCIs of the same RNTI and
 *  configuration. forbid[row] |= bit k. Returns the number of rows given a non-zero mask. */
int nr_dci_hist_adj_exclusions(nr_dci_hist_t *h, const nr_dci_hist_entry_t *x, nr_dci_row_k0_fn row_k0, void *arg,
                               uint64_t forbid[NR_DCI_HIST_ROWS]);

/** Process-wide ring used by the receiver (lazily initialised with period 0 until nr_dci_hist_global_init()). */
nr_dci_hist_t *nr_dci_hist_global(void);
void nr_dci_hist_global_init(uint32_t period);

#ifdef __cplusplus
}
#endif
#endif
