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

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_config_sweep.h
 * \brief Phase 3 Technique D: recover the PDSCH payload-INTERPRETATION config by search,
 *        scored on the TRANSPORT-BLOCK CRC.
 *
 * WHY A SECOND ORACLE IS NEEDED. Techniques A-C recover WHERE the PDCCH is, HOW LONG the DCI is and
 * WHICH RNTI it carries, all scored on the polar CRC. That oracle cannot reach the remaining
 * unknowns -- the TDRA list, the DM-RS additional position, and the MCS table -- because the polar
 * CRC validates the DCI payload BITS and says nothing about how those bits are INTERPRETED. A wrong
 * TDRA entry still yields a CRC-valid DCI; it just points the PDSCH decode at the wrong symbols.
 *
 * The transport-block CRC is exactly that missing oracle, and it only became usable once the
 * passive PDSCH decode worked: a wrong hypothesis gives ~0 % TB CRC while the right one gives the
 * rate the link supports (76 % measured on this rig at 4 antennas). That is a far larger separation
 * than the chance-pass margin Technique C has to work with.
 *
 * WHY ROUND-ROBIN PER GRANT AND NOT A BLOCK PER HYPOTHESIS. This receiver's TB-CRC swings between
 * 5 % and 88 % across otherwise identical captures, drifting on a timescale of minutes (measured
 * repeatedly 2026-09-07). Testing hypothesis A now and hypothesis B a minute later would compare
 * them across that drift and is exactly the confound that produced several wrong conclusions
 * earlier the same day. Hypotheses are therefore INTERLEAVED at per-grant granularity, so every
 * hypothesis sees statistically the same channel; with ~200k grants a run and a few dozen
 * hypotheses each still collects thousands of trials.
 *
 * SCOPE: recovers interpretation, not geometry. It assumes Techniques A-C have already converged,
 * because a wrong dci_length or CORESET makes every hypothesis score zero and the sweep would
 * (correctly) report that it cannot tell them apart.
 */

#ifndef __NR_PDSCH_CONFIG_SWEEP_H__
#define __NR_PDSCH_CONFIG_SWEEP_H__

#include <stdbool.h>
#include <stdint.h>

/// One payload-interpretation hypothesis. Deliberately only the fields the TB CRC can actually
/// discriminate -- anything the polar CRC already pins (dci_length, bwp_size) is not swept here.
typedef struct {
  uint8_t tda_start;   ///< S: first PDSCH symbol
  uint8_t tda_length;  ///< L: number of PDSCH symbols
  uint8_t dmrs_add_pos;///< dmrs-AdditionalPosition, 0..3
  uint8_t dmrs_max_len;///< maxLength, 1 or 2
  uint16_t dmrs_mask; ///< validated effective mask, zero only in legacy pure tests
  uint8_t mcs_table;   ///< 0 = 64QAM, 1 = 256QAM, 2 = 64QAM-LowSE
} nr_pdsch_cfg_hypothesis_t;

/* Optional non-reentrant diagnostics: callback must not call the sweep API.
 * Pure/offline users have no logger dependency. */
typedef struct {
  uint64_t configuration,outcomes,passes,trials;
  uint16_t rnti;
  uint8_t tda;
  uint32_t minimum;
  bool operational;
  int winner;
  nr_pdsch_cfg_hypothesis_t hypothesis;
} nr_pdsch_sweep_report_t;
typedef void (*nr_pdsch_sweep_reporter_t)(const nr_pdsch_sweep_report_t *);
void nr_pdsch_config_sweep_set_reporter(nr_pdsch_sweep_reporter_t);

#define NR_PDSCH_SWEEP_MAX_HYP 192
#define NR_PDSCH_SWEEP_MAX_CONTEXTS (16 * 16)

typedef struct {
  nr_pdsch_cfg_hypothesis_t hyp[NR_PDSCH_SWEEP_MAX_HYP];
  uint32_t trials[NR_PDSCH_SWEEP_MAX_HYP];
  uint32_t ok[NR_PDSCH_SWEEP_MAX_HYP];
  int      n_hyp;
  int      cursor;    ///< round-robin position
  int      winner;    ///< -1 until decided
} nr_pdsch_config_sweep_state_t;

/** Build the complete supported mapping-A catalog for pure algorithm tests.
 * Runtime uses init_legal() with the real cell DMRS table. TDA field width remains an
 * extraction input: total DCI length alone does not determine it. */
int nr_pdsch_config_sweep_init(nr_pdsch_config_sweep_state_t *st, int tda_count);

/** Next hypothesis to try, round-robin. Returns its index and fills *out. */
int nr_pdsch_config_sweep_next(nr_pdsch_config_sweep_state_t *st, nr_pdsch_cfg_hypothesis_t *out);

/** Report the TB-CRC outcome of the grant decoded under hypothesis `idx`.
 * Returns the winning index once one is established, else -1. */
int nr_pdsch_config_sweep_feed(nr_pdsch_config_sweep_state_t *st, int idx, bool tb_crc_ok);

/** Winner, or -1 if undecided. */
int nr_pdsch_config_sweep_winner(const nr_pdsch_config_sweep_state_t *st);

typedef int32_t (*nr_pdsch_legality_fn_t)(int, int, int, int, int, int);
/** Enumerates the complete catalog, excludes undefined masks, merges identical effective PDUs.
 * The caller-owned pure state is not internally synchronized. */
int nr_pdsch_config_sweep_init_legal(nr_pdsch_config_sweep_state_t *st, int tda_count,
                                   int typeA, nr_pdsch_legality_fn_t legality);

/** Value-only feedback identity. A zero generation is never scored. */
typedef struct {
  uint64_t generation;
  uint16_t context_slot;
  uint16_t rnti;
  uint8_t tda_index;
  int hypothesis;
  bool settled; ///< this selection uses an already-converged context
} nr_pdsch_sweep_ticket_t;

/** Thread-safe per-(configuration,RNTI,TDA) controller. No allocation or decoder work under lock.
 * Context exhaustion evicts the least recently selected context; stale queued feedback is ignored. */
bool nr_pdsch_config_sweep_select(uint64_t configuration, uint16_t rnti, uint8_t tda_index,
                                 int tda_count, int typeA, nr_pdsch_legality_fn_t legality,
                                 nr_pdsch_sweep_ticket_t *ticket, nr_pdsch_cfg_hypothesis_t *out);
/** Returns true exactly once on convergence; fills winner when supplied. */
bool nr_pdsch_config_sweep_feedback(const nr_pdsch_sweep_ticket_t *ticket, bool crc_ok,
                                   nr_pdsch_cfg_hypothesis_t *winner);
bool nr_pdsch_config_sweep_is_settled(uint64_t configuration, uint16_t rnti, uint8_t tda, int typeA);
/** CRC evidence held by one keyed context: total passes and trials over all its hypotheses.
 * Zero/zero when the context does not exist. Lets the caller prefer a DL layout FAMILY that has
 * ever decoded over one that never has, without waiting for the per-hypothesis winner. */
void nr_pdsch_config_sweep_context_stats(uint64_t configuration, uint16_t rnti, uint8_t tda, int typeA,
                                         uint32_t *passes, uint32_t *trials);
void nr_pdsch_config_sweep_reset_all(void);
/** Diagnostic: number of live keyed contexts with a winner (acquisition-state tracker input). */
int  nr_pdsch_config_sweep_settled_count(void);
/** Consistent snapshot for diagnostics/offline regression tests. */
bool nr_pdsch_config_sweep_snapshot(const nr_pdsch_sweep_ticket_t *ticket,
                                   nr_pdsch_config_sweep_state_t *out);

/* ---- Process-wide singleton -------------------------------------------------------------------
 * The hypothesis is chosen on the PHY receive thread and scored on a PDSCH consumer thread, i.e.
 * in two different translation units and two different threads, so the state cannot be a static in
 * either one. The functions above stay pure and unit-testable; these are the thin shared layer.
 * Legacy compatibility helpers below serialize access; production uses keyed tickets above. */
void nr_pdsch_config_sweep_enable_global(int tda_count);
int  nr_pdsch_config_sweep_next_global(nr_pdsch_cfg_hypothesis_t *out);
int  nr_pdsch_config_sweep_feed_global(int idx, bool tb_crc_ok);
int  nr_pdsch_config_sweep_winner_global(void);
/** Fills *out with the winning hypothesis; false while undecided. */
bool nr_pdsch_config_sweep_result_global(nr_pdsch_cfg_hypothesis_t *out);

#endif
