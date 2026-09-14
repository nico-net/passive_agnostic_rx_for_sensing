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

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ss_registry.h
 * \brief Hold SEVERAL CORESET / search-space / BWP configurations at once and learn which are real.
 *
 * THE GAP. The blind monitor carries one CORESET, one search space and one BWP. A real gNB
 * configures several: CORESET#0 plus one or more dedicated ones, a common search space alongside
 * a UE-specific one, and an initial BWP alongside a dedicated BWP. Anything the receiver is not
 * configured for is simply never monitored, and there is no evidence in the logs that it was
 * missed -- absence of accepts looks identical to absence of transmission.
 *
 * WHY A REGISTRY AND NOT A SWEEP. The other unknowns in this receiver are mutually exclusive
 * hypotheses: exactly one TDRA entry, one DCI layout, one PT-RS density is correct, so they are
 * swept and the loser is discarded. CORESETs are NOT mutually exclusive -- several are genuinely
 * active at the same time, and discarding the runner-up would lose real grants. So entries are
 * scored and RETAINED, and the statistic asked of each is "does this one ever yield a real DCI",
 * not "is this one the answer".
 *
 * WHAT COUNTS AS REAL. A CRC-recovered RNTI in range is NOT evidence on its own: a blind search
 * over noise produces one occasionally, which is exactly how a spurious CORESET would look alive.
 * The registry therefore scores CONFIRMED accepts -- an accept whose RNTI has been seen more than
 * once, which noise does not reproduce -- and reports a rate per entry so a configuration that
 * never yields one can be retired from the scan and stop costing CPU.
 */

#ifndef __NR_PDCCH_SS_REGISTRY_H__
#define __NR_PDCCH_SS_REGISTRY_H__

#include <stdbool.h>
#include <stdint.h>

#define NR_PDCCH_SS_MAX 8

typedef struct {
  uint8_t  coreset_id;
  uint8_t  coreset_duration;      ///< 1..3 symbols
  uint16_t coreset_n_rbs;         ///< frequency-domain size, in RBs
  uint8_t  ss_type;               ///< 0 = common (CSS), 1 = UE-specific (USS)
  uint8_t  ss_first_symbol;
  uint16_t ss_period_slots;
  uint16_t ss_offset_slots;
  uint16_t bwp_start, bwp_size;
  uint8_t  al_candidates[5];      ///< candidates at AL 1,2,4,8,16
} nr_pdcch_ss_entry_t;

typedef struct {
  nr_pdcch_ss_entry_t entry[NR_PDCCH_SS_MAX];
  uint64_t occasions[NR_PDCCH_SS_MAX];   ///< monitoring occasions spent
  uint64_t accepts[NR_PDCCH_SS_MAX];     ///< raw accepts, noise included
  uint64_t confirmed[NR_PDCCH_SS_MAX];   ///< accepts whose RNTI repeated -- the real evidence
  bool     retired[NR_PDCCH_SS_MAX];
  int      n;
} nr_pdcch_ss_registry_t;

/** Add a configuration. Returns its index, or -1 if full or already present.
 * Duplicates are rejected on the FIELDS, not on the id: two entries describing the same occasions
 * would double the scan cost and split the evidence for the same physical search space. */
int nr_pdcch_ss_register(nr_pdcch_ss_registry_t *r, const nr_pdcch_ss_entry_t *e);

/** Does this entry monitor `absolute_slot`? */
bool nr_pdcch_ss_monitors_slot(const nr_pdcch_ss_entry_t *e, uint32_t absolute_slot);

/** Record one monitoring occasion and its outcome. `confirmed` means the accept's RNTI has been
 * seen before -- pass false for a first sighting, which noise also produces. */
void nr_pdcch_ss_observe(nr_pdcch_ss_registry_t *r, int idx, bool accepted, bool confirmed);

/** Retire entries that have spent `min_occasions` without ever producing a CONFIRMED accept.
 * Never retires the last live entry, and never retires one that has any confirmed evidence.
 * Returns the number retired by this call. */
int nr_pdcch_ss_retire_barren(nr_pdcch_ss_registry_t *r, uint64_t min_occasions);

/** Live (non-retired) entry count. */
int nr_pdcch_ss_live(const nr_pdcch_ss_registry_t *r);

#endif /* __NR_PDCCH_SS_REGISTRY_H__ */
