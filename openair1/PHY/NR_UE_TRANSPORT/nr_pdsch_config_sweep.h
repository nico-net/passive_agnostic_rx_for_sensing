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
  uint8_t mcs_table;   ///< 0 = 64QAM, 1 = 256QAM, 2 = 64QAM-LowSE
} nr_pdsch_cfg_hypothesis_t;

#define NR_PDSCH_SWEEP_MAX_HYP 64

typedef struct {
  nr_pdsch_cfg_hypothesis_t hyp[NR_PDSCH_SWEEP_MAX_HYP];
  uint32_t trials[NR_PDSCH_SWEEP_MAX_HYP];
  uint32_t ok[NR_PDSCH_SWEEP_MAX_HYP];
  int      n_hyp;
  int      cursor;    ///< round-robin position
  int      winner;    ///< -1 until decided
} nr_pdsch_config_sweep_state_t;

/** Build the hypothesis set. `tda_count` comes from Technique C (the solved dci_length pins the
 * TDA field width, hence the list size), so the search is over the CONTENTS of a list whose LENGTH
 * is already known. Returns the number of hypotheses enumerated. */
int nr_pdsch_config_sweep_init(nr_pdsch_config_sweep_state_t *st, int tda_count);

/** Next hypothesis to try, round-robin. Returns its index and fills *out. */
int nr_pdsch_config_sweep_next(nr_pdsch_config_sweep_state_t *st, nr_pdsch_cfg_hypothesis_t *out);

/** Report the TB-CRC outcome of the grant decoded under hypothesis `idx`.
 * Returns the winning index once one is established, else -1. */
int nr_pdsch_config_sweep_feed(nr_pdsch_config_sweep_state_t *st, int idx, bool tb_crc_ok);

/** Winner, or -1 if undecided. */
int nr_pdsch_config_sweep_winner(const nr_pdsch_config_sweep_state_t *st);

/* ---- Process-wide singleton -------------------------------------------------------------------
 * The hypothesis is chosen on the PHY receive thread and scored on a PDSCH consumer thread, i.e.
 * in two different translation units and two different threads, so the state cannot be a static in
 * either one. The functions above stay pure and unit-testable; these are the thin shared layer.
 * Counters are plain: a torn read costs at most one mis-attributed trial out of thousands, which is
 * far cheaper than serialising the decode path. */
void nr_pdsch_config_sweep_enable_global(int tda_count);
int  nr_pdsch_config_sweep_next_global(nr_pdsch_cfg_hypothesis_t *out);
int  nr_pdsch_config_sweep_feed_global(int idx, bool tb_crc_ok);
int  nr_pdsch_config_sweep_winner_global(void);
/** Fills *out with the winning hypothesis; false while undecided. */
bool nr_pdsch_config_sweep_result_global(nr_pdsch_cfg_hypothesis_t *out);

#endif
