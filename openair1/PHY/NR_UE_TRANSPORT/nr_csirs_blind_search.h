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

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_search.h
 * \brief Recover a cell's NZP CSI-RS resource configuration from the air.
 *
 * WHY THIS EXISTS. Every other parameter this passive receiver needs is now derived: the CORESET
 * and search space (CSS0 autoconf), the DCI length (length sweep), the RNTI (from the DCI CRC
 * mask), and the TDRA entry / DM-RS additional position / MCS table (Technique D). CSI-RS was the
 * last one still requiring hand-written `csirs_monitor` values copied out of the DU log, which is
 * exactly what cannot be done against a commercial gNB.
 *
 * WHY IT NEEDS ITS OWN ORACLE. Techniques A-D all score on a CRC. CSI-RS carries no CRC, and its
 * configuration is dedicated RRC, so it is not broadcast anywhere a passive receiver can read.
 * What it does have is a KNOWN sequence: the gold sequence is a function of
 * (N_RB, symbols_per_slot, slot, symbol, scramblingID) alone -- the row, the frequency-domain
 * bitmap and the density decide only WHICH resource elements carry it. So the oracle is
 * correlation: score a candidate by how well the received REs match the sequence that candidate
 * predicts. A right guess correlates near 1, a wrong one near 1/sqrt(N). This is the same test
 * already used for PDCCH DM-RS (`DMRSSTAT ... real DM-RS ~0.8-0.95`).
 *
 * WHY THE REFERENCE COMES FROM THE REAL GENERATOR. The mapping from resource element to sequence
 * index is intricate (row-dependent k-prime/l-prime, CDM groups, density 0.5 parity). Re-deriving
 * it here would risk a subtly wrong reference, and a wrong reference sequence does not look like a
 * weak signal -- it looks like a dead channel, which is the failure this module exists to avoid
 * diagnosing. `nr_csirs_blind_reference()` therefore calls nr_generate_csi_rs() itself and treats
 * whatever it writes as the truth.
 *
 * STRUCTURE. The two load-bearing pieces are pure and unit-tested without any PHY state:
 *   - nr_csirs_blind_correlate(): the oracle.
 *   - nr_csirs_blind_infer_period(): turns a set of hit slots into (period, offset), constrained
 *     to the periodicities 38.331 actually allows.
 * The PHY-dependent wrapper is a thin shim over those two.
 */

#ifndef __NR_CSIRS_BLIND_SEARCH_H__
#define __NR_CSIRS_BLIND_SEARCH_H__

#include <stdbool.h>
#include <stdint.h>

/// One CSI-RS resource hypothesis. Mirrors the fields `csirs_monitor` takes, so a confirmed
/// candidate can be printed straight back out as a config line.
typedef struct {
  uint8_t  row;          ///< TS 38.211 Table 7.4.1.5.3-1 row index
  uint16_t freq_domain;  ///< frequency-domain allocation bitmap (the "b" of get_csi_mapping_parms)
  uint8_t  symb_l0;      ///< first OFDM symbol
  uint8_t  symb_l1;      ///< second OFDM symbol (rows that use one; else 0)
  uint8_t  cdm_type;
  uint8_t  freq_density; ///< 0/1 = dot5 (even/odd RB), 2 = one, 3 = three
  uint16_t scramb_id;    ///< almost always the PCI, but not required to be
  uint16_t start_rb, nr_of_rbs;
} nr_csirs_candidate_t;

/** Normalised correlation between a received slot and a reference grid, over the REs the
 * reference actually occupies. Returns |<rx,ref>| / (||rx|| ||ref||) in [0,1], or -1.0 when the
 * reference is empty (a candidate that maps no RE cannot be scored, and must not read as 0.0 --
 * that would rank it alongside a genuine mismatch).
 * Pure: no PHY state, no allocation. `n` is the number of REs in both buffers. */
double nr_csirs_blind_correlate(const int16_t *rx_re_im, const int16_t *ref_re_im, int n);

/// Periodicities TS 38.331 CSI-ResourcePeriodicityAndOffset admits, in slots.
#define NR_CSIRS_BLIND_N_PERIODS 13
extern const uint16_t nr_csirs_blind_periods[NR_CSIRS_BLIND_N_PERIODS];

/** Infer (period, offset) from the absolute slots at which a candidate scored a hit.
 *
 * Deliberately NOT a GCD of the deltas: a missed occurrence (fading, a slot the receiver did not
 * observe) makes one delta a multiple of the true period, and a GCD over a sparse or noisy hit set
 * happily returns 1. Instead it is a hypothesis test over the 13-element legal set: keep the
 * periodicities under which every observed hit falls in the same slot of the cycle, and return the
 * LARGEST of them.
 *
 * Largest, not smallest -- this is the part that is easy to get wrong. Every DIVISOR of the true
 * period also passes the mod test (hits at period 20 are equally consistent with 4, 5 and 10), so
 * a smallest-first rule degenerates to 4 almost always. A period LONGER than the truth is rejected
 * on its own, because its hits land at different phases. The largest survivor is therefore the
 * answer, bounded by the observation span so the result stays falsifiable.
 *
 * Requires at least `min_hits` hits, and rejects the degenerate case where the span of the hits is
 * shorter than the period it would claim (one occurrence proves nothing about periodicity).
 * Returns true and fills *period/*offset on success. Pure. */
bool nr_csirs_blind_infer_period(const uint32_t *hit_slots, int n_hits, int min_hits,
                                 uint16_t *period, uint16_t *offset);

/** Format a confirmed candidate as a `csirs_monitor` config entry
 * ("row:start_rb:nr_rbs:freq_domain:symb_l0:symb_l1:cdm_type:freq_density:scramb_id:period:offset").
 * Returns the number of characters written, or 0 if it would not fit. Pure. */
int nr_csirs_blind_format(const nr_csirs_candidate_t *c, uint16_t period, uint16_t offset,
                          char *out, int out_len);

#endif /* __NR_CSIRS_BLIND_SEARCH_H__ */
