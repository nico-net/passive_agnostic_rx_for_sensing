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

struct nr_td_side_info_s; /* nr_td_order.h (which includes this header) */

/// One payload-interpretation hypothesis. Deliberately only the fields the TB CRC can actually
/// discriminate -- anything the polar CRC already pins (dci_length, bwp_size) is not swept here.
typedef struct {
  uint8_t tda_start;   ///< S: first PDSCH symbol
  uint8_t tda_length;  ///< L: number of PDSCH symbols
  uint8_t k0;          ///< PDSCH slot offset from the DCI slot (TDRA entry k0), 0..32 (>= 2 only once observed)
  uint8_t dmrs_add_pos;///< dmrs-AdditionalPosition, 0..3
  uint8_t dmrs_max_len;///< maxLength, 1 or 2
  uint16_t dmrs_mask; ///< validated effective mask, zero only in legacy pure tests
  uint8_t mcs_table;   ///< 0 = 64QAM, 1 = 256QAM, 2 = 64QAM-LowSE
  uint8_t mapping_type;///< PDSCH mapping type: 0 = A, 1 = B (dmrs_add_pos/max_len are that type's IE)
} nr_pdsch_cfg_hypothesis_t;

/** TS 38.214 Table 5.1.2.1-1, normal CP: type A S 0..3, L 3..14; type B (Rel-16) S 0..12, L 2..13;
 *  both S+L <= 14. mapping_type 0 = A, 1 = B, anything else is not legal. */
bool nr_pdsch_tda_legal(int mapping_type, int S, int L);

/* Optional non-reentrant diagnostics: callback must not call the sweep API.
 * Pure/offline users have no logger dependency. */
typedef struct {
  uint64_t configuration,outcomes,passes,trials;
  uint16_t rnti;
  uint8_t tda;
  uint32_t minimum;
  bool operational;
  bool invalidated; ///< local health loss, NOT proof of a network configuration change
  uint64_t previous_generation, generation, reacquisitions;
  uint64_t failure_streak;
  double reference_crc_lower;
  int winner;
  nr_pdsch_cfg_hypothesis_t hypothesis;
} nr_pdsch_sweep_report_t;
typedef void (*nr_pdsch_sweep_reporter_t)(const nr_pdsch_sweep_report_t *);
void nr_pdsch_config_sweep_set_reporter(nr_pdsch_sweep_reporter_t);

/* ISAC_PDSCH_TYPEB=0 (read once) hard-disables mapping type B entirely; default on.
 * R30 item 1 (2026-09-26, technique-d-regression.md): a fresh/initial catalog is mapping type A
 * ONLY -- type A pure catalog 2016, runtime (merged by effective mask, pos2) ~750, matching base
 * commit 222f98d072's pre-Task-14 numbers. Type B enters a context only once the DM-RS oracle
 * observes a mask no type-A hypothesis can produce (nr_pdsch_config_sweep_observe(), mirroring the
 * k0-layer mechanism: adds hypotheses, never prunes the incumbent). Diluting every fresh catalog
 * 2.9x regardless of whether the cell uses type B measurably starved the type-A search this cell
 * actually needed (BASE still occasionally decoded on the phy-test rig; the diluted catalog did not,
 * in the same wall time). Full both-types pure catalog, when triggered: (42 type-A + 90 type-B legal
 * (S,L)) x k0 {0,1} x 4 add_pos x 2 max_len x 3 mcs_table = 6336; runtime up to ~2154, i.e. ~1077 per
 * k0 layer, so 8192 leaves room for five observed k0 >= 2 layers on top of it. Per context:
 * 8192 x 22 B = 180 KB, heap-allocated when a context slot is first used (nr-uesoftmodem mlockall()s,
 * so 1024 inline states would pin 185 MB at startup). The 2026-10-01 probe counters (3 x uint16) add
 * 48 KB: sizeof 180244 -> 229416 B; at 1024 contexts + 4 templates + 1 spare + the legacy singleton,
 * at most 50.6 MB more, and only for slots actually opened. */
#define NR_PDSCH_SWEEP_MAX_HYP 8192
#define NR_PDSCH_SWEEP_MAX_CONTEXTS 1024 /* one per (layout x TDA index) under the wide search; 256 thrashed at 809 layouts */

typedef struct {
  nr_pdsch_cfg_hypothesis_t hyp[NR_PDSCH_SWEEP_MAX_HYP];
  uint32_t trials[NR_PDSCH_SWEEP_MAX_HYP];
  uint32_t ok[NR_PDSCH_SWEEP_MAX_HYP];
  int      n_hyp;
  int      order[NR_PDSCH_SWEEP_MAX_HYP];
  uint32_t random_state;
  uint32_t exploit_tick; ///< 3 of 4 trials go to the hypothesis with the most passes (see _next)
  int      cursor;    ///< position in the shuffled, balanced round
  int      winner;    ///< -1 until decided
  /* Convergence levers (spec 2026-10-01 §5.1-5.3). Code-block PROBE outcomes, kept apart from the KL
   * evidence above: they order rounds (P1) and, only with p2 set, an ADMISSIBLE probe failure adds one
   * KL failure; a probe pass never adds KL evidence. uint16 (saturating) bounds the per-state cost to
   * 48 KB. Cleared together with trials/ok (indices move on every prune). */
  uint16_t probe_pass[NR_PDSCH_SWEEP_MAX_HYP];
  uint16_t probe_fail[NR_PDSCH_SWEEP_MAX_HYP];
  uint16_t probe_inconclusive[NR_PDSCH_SWEEP_MAX_HYP];
  /* CONFIGURATION, not catalog/evidence: preserved across catalog rebuilds (nr_pdsch_config_sweep_rebuild(),
   * i.e. context reopen and prior restore); a brand-new runtime context starts with NULL/false. */
  const struct nr_td_side_info_s *side; ///< ordering side information (nr_td_order.h); NULL = neutral (today's order)
  bool     p2;        ///< failure-only probe evidence enabled
} nr_pdsch_config_sweep_state_t;

/* ---- K-hypothesis selection and probe outcomes (spec 2026-10-01 §5.1-5.3) ------------------------- */
typedef enum { NR_TD_FULL_TB = 0, NR_TD_CB_PROBE = 1 } nr_td_outcome_kind_t;
typedef enum { NR_TD_PASS = 0, NR_TD_FAIL = 1, NR_TD_INCONCLUSIVE = 2 } nr_td_outcome_result_t;
typedef struct {
  int hyp;            ///< hypothesis index in the state that produced it
  uint8_t kind;       ///< nr_td_outcome_kind_t
  uint8_t result;     ///< nr_td_outcome_result_t
  bool p2_admissible; ///< probe failure qualifies as KL evidence (only used when st->p2)
} nr_td_outcome_t;
#define NR_TD_MAX_K 8

/** Build the complete supported mapping-A + mapping-B catalog for pure algorithm tests.
 * Runtime uses init_legal() with the real cell DMRS table. TDA field width remains an
 * extraction input: total DCI length alone does not determine it. */
int nr_pdsch_config_sweep_init(nr_pdsch_config_sweep_state_t *st, int tda_count);

/** Next hypothesis to try, round-robin. Returns its index and fills *out. */
int nr_pdsch_config_sweep_next(nr_pdsch_config_sweep_state_t *st, nr_pdsch_cfg_hypothesis_t *out);

/** Report the TB-CRC outcome of the grant decoded under hypothesis `idx`.
 * Returns the winning index once one is established, else -1. */
int nr_pdsch_config_sweep_feed(nr_pdsch_config_sweep_state_t *st, int idx, bool tb_crc_ok);

/* Credit one full-TB outcome to idx[0] (the decoded hypothesis) and to its grant-equivalent alive hypotheses
 * idx[1..n-1]. Duplicates and out-of-range members are ignored; an invalid idx[0] (or n < 1) credits nothing and
 * returns the current winner, as _feed does. n == 1 is bit-identical to
 * nr_pdsch_config_sweep_feed(st, idx[0], tb_crc_ok). The acceptance check runs once, after crediting, whenever any
 * credited hypothesis reached a multiple of 16 trials. Returns the winner or -1.
 * Equivalence (blind-convergence spec 2026-10-01 section 2) is the caller's job: equal nr_td_equiv_key() on this
 * grant. new_data is accepted but currently IGNORED; lever C (task BC2) will use it. */
int nr_pdsch_config_sweep_feed_equiv(nr_pdsch_config_sweep_state_t *st, const int *idx, int n, bool tb_crc_ok,
                                     bool new_data /* new transmission (NDI toggled); used only by lever C (BC2) */);

/** K = 1 is exactly nr_pdsch_config_sweep_next(). Returns n filled (1..K, K clamped to NR_TD_MAX_K;
 *  0 when nothing can be selected); idx[0]/out[0] = the main hypothesis (unchanged path and RNG use),
 *  idx[1..n-1] = distinct probe hypotheses that are not yet cleared (>= SWEEP_MIN_TRIALS trials, no pass),
 *  taken from the current round order at the cursor WITHOUT advancing it or consuming RNG. Once a winner
 *  exists only the winner is returned (n = 1). */
int nr_pdsch_config_sweep_next_k(nr_pdsch_config_sweep_state_t *st, int K, int idx[], nr_pdsch_cfg_hypothesis_t out[]);

/** outcomes[0] must be the main FULL_TB outcome: PASS/FAIL go through nr_pdsch_config_sweep_feed();
 *  INCONCLUSIVE (or a non-FULL_TB entry) is not fed. outcomes[1..n-1] are probes: they only update the
 *  probe counters, except that with st->p2 an admissible FAIL adds exactly one KL failure. A probe PASS
 *  never adds KL evidence. Returns the winner index or -1 (same contract as _feed). */
int nr_pdsch_config_sweep_feed_k(nr_pdsch_config_sweep_state_t *st, const nr_td_outcome_t *outcomes, int n);

/** Winner, or -1 if undecided. */
int nr_pdsch_config_sweep_winner(const nr_pdsch_config_sweep_state_t *st);

typedef int32_t (*nr_pdsch_legality_fn_t)(int, int, int, int, int, int);
/** Rebuild the full catalog in place exactly as a runtime context does (shared template copy, or
 *  init_legal() when none is available), discarding all evidence but KEEPING the configuration fields
 *  side and p2. st must already be a valid state (side/p2 are read). Returns the hypothesis count. */
int nr_pdsch_config_sweep_rebuild(nr_pdsch_config_sweep_state_t *st, int tda_count, int typeA,
                                  nr_pdsch_legality_fn_t legality);
/** Enumerates the complete catalog, excludes undefined masks, merges identical effective PDUs.
 * The caller-owned pure state is not internally synchronized. */
int nr_pdsch_config_sweep_init_legal(nr_pdsch_config_sweep_state_t *st, int tda_count,
                                   int typeA, nr_pdsch_legality_fn_t legality);

/* ---- CELL-WIDE PRIOR ------------------------------------------------------------------------
 * A context enumerates (S,L) x dmrs_add_pos x dmrs_max_len x mcs_table, but only (S,L) is a
 * property of the TDRA ENTRY. dmrs-AdditionalPosition, maxLength and mcs-Table come from the
 * cell's DM-RS/PDSCH config and are identical for every entry of the same configuration key.
 * MEASURED OTA 2026-09-13: tda=0 converged on S=1 L=13 while tda=1 of the SAME cell still had 61
 * trials on its leader after 7805 outcomes -- it was re-deriving those three cell-wide fields from
 * scratch. Publishing them once cuts a later context's catalog from ~233 entries to the ~8 (S,L)
 * ones, so evidence per hypothesis rises ~29x at no cost in assumptions.
 * It stays a PRIOR, never an assumption: a pruned context that cannot raise any hypothesis above
 * SWEEP_MIN_RATE within its probation window restores the full catalog AND invalidates the prior,
 * so one bad publication cannot poison the rest of the run. */

/** Restrict a catalog to one set of cell-wide fields (every mapping type), discarding evidence.
 *  Returns the new hypothesis count, or 0 leaving the state untouched when nothing matches. When EVERY
 *  entry matches nothing moves, so the count is returned unchanged and the evidence is KEPT (before
 *  Task 14 it was cleared in that case too). */
int nr_pdsch_config_sweep_prune_to(nr_pdsch_config_sweep_state_t *st, uint8_t mcs_table,
                                   uint8_t dmrs_add_pos, uint8_t dmrs_max_len);

/** Drop the published prior (tests, and any external evidence that the cell changed). */
void nr_pdsch_config_sweep_prior_reset(void);

/** True when the CELL-WIDE prior is published (two distinct RNTIs converged on the same fields);
 *  fills any non-NULL outputs. */
bool nr_pdsch_config_sweep_prior_get(uint64_t *configuration, uint8_t *mcs_table,
                                     uint8_t *dmrs_add_pos, uint8_t *dmrs_max_len);
/** Same for one RNTI's own prior (set by its first converged context; seeds its sibling TDA contexts). */
bool nr_pdsch_config_sweep_rnti_prior_get(uint16_t rnti, uint64_t *configuration, uint8_t *mcs_table,
                                          uint8_t *dmrs_add_pos, uint8_t *dmrs_max_len);

/** Value-only feedback identity. A zero generation is never scored. */
typedef struct {
  uint64_t generation;
  uint16_t context_slot;
  uint16_t rnti;
  uint8_t tda_index;
  int hypothesis;
  bool settled; ///< this selection uses an already-converged context
  uint16_t layout_index; ///< DCI 1_1 layout (resolver index) this trial was decoded under; 0xFFFF = none
  uint8_t k0;            ///< the selected hypothesis' k0 (the consumer measures the oracle on slot + k0)
} nr_pdsch_sweep_ticket_t;

/** Thread-safe per-(configuration,RNTI,TDA) controller. No allocation or decoder work under lock.
 * Contexts are PER RNTI: a new RNTI's context is seeded (pruned) from that RNTI's own prior and
 * observations when it has any, else from the cell-wide ones, which exist only once two distinct
 * RNTIs agree. Context exhaustion evicts the least recently selected context; stale queued
 * feedback is ignored. */
bool nr_pdsch_config_sweep_select(uint64_t configuration, uint16_t rnti, uint8_t tda_index,
                                 int tda_count, int typeA, nr_pdsch_legality_fn_t legality,
                                 nr_pdsch_sweep_ticket_t *ticket, nr_pdsch_cfg_hypothesis_t *out);
/** Restrict a catalog to the hypotheses whose effective DM-RS mask equals an OBSERVED one; 0 leaves
 *  it untouched (no match). Pure. */
int nr_pdsch_config_sweep_prune_mask(nr_pdsch_config_sweep_state_t *st, uint16_t dmrs_mask);
/** Same, on the ticket's live context (no-op once it has a winner). Returns the surviving count. */
int nr_pdsch_config_sweep_observe_mask(const nr_pdsch_sweep_ticket_t *ticket, uint16_t dmrs_mask);
/** Qm oracle (nr_pdsch_qm_oracle.h): keep only hypotheses whose MCS table maps `mcs` to the measured
 *  order `qm`. Same contract as prune_mask: 0 = nothing matched (state untouched); unchanged = count. */
int nr_pdsch_config_sweep_prune_qm(nr_pdsch_config_sweep_state_t *st, uint8_t mcs, int qm);
/** Live context, two-observation rule: the tables consistent with each observation are intersected per
 *  context and applied once two agree; a conflict (empty intersection) resets the evidence. Returns the
 *  surviving count only when this call removed hypotheses, else 0. ISAC_QM_ORACLE=0 disables. */
int nr_pdsch_config_sweep_observe_qm(const nr_pdsch_sweep_ticket_t *ticket, uint8_t mcs, int qm);
/** Full oracle observation: the DM-RS mask, the last PDSCH symbol carrying energy on the grant's
 *  PRBs (-1 = unmeasured) and the k0 of the job it was measured on (-1 = unknown). Records it
 *  cell-wide and prunes the ticket's context to the admitted entries. */
int nr_pdsch_config_sweep_observe(const nr_pdsch_sweep_ticket_t *ticket, uint16_t dmrs_mask, int last_symbol, int k0);
/** k0 oracle: the air showed DM-RS on this grant's PRBs `k0` slots after the DCI (and not in the
 *  catalog's k0 {0,1} slots). Appends the k0 layer to the ticket's live context (unsettled only) and
 *  remembers it for this RNTI's later contexts. Returns the number of hypotheses added: 0 when the
 *  layer is already there, k0 > 32, the context is settled/stale, or the layer would not fit. */
int nr_pdsch_config_sweep_add_k0(const nr_pdsch_sweep_ticket_t *t, uint8_t k0);
/** The PDSCH slot a hypothesis with slot offset `k0` targets for a DCI received in (frame, slot):
 *  slot + k0, carried into the frame (and SFN, mod 1024). Every decode path must use it: decoding a
 *  k0 = 1 hypothesis on the DCI's own slot makes it an exact twin of its k0 = 0 sibling, and the sweep
 *  (correctly) never picks between two indistinguishable hypotheses. */
void nr_pdsch_k0_slot(int frame, int slot, int slots_per_frame, int k0, int *frame_out, int *slot_out);
/** Returns true exactly once on convergence; fills winner when supplied. */
bool nr_pdsch_config_sweep_feedback(const nr_pdsch_sweep_ticket_t *ticket, bool crc_ok,
                                   nr_pdsch_cfg_hypothesis_t *winner);
bool nr_pdsch_config_sweep_is_settled(uint64_t configuration, uint16_t rnti, uint8_t tda, int typeA);
/** CRC evidence held by one keyed context: total passes and trials over all its hypotheses.
 * Zero/zero when the context does not exist. Lets the caller prefer a DL layout FAMILY that has
 * ever decoded over one that never has, without waiting for the per-hypothesis winner. */
void nr_pdsch_config_sweep_context_stats(uint64_t configuration, uint16_t rnti, uint8_t tda, int typeA,
                                         uint32_t *passes, uint32_t *trials);
/** Diagnostic: number of live keyed contexts with a winner (acquisition-state tracker input). */
int  nr_pdsch_config_sweep_settled_count(void);
void nr_pdsch_config_sweep_reset_all(void);
/** Local recovery policy; never changes the hypothesis winner/validation criteria.
 * A failure streak must exceed the minimum AND contradict the conservative learned
 * CRC lower bound. This is a health trigger, not an inferred BWP-change assertion.
 * Defaults: 32 failures minimum, 1e-6 run probability budget. Process-wide, locked.
 * Invalid arguments leave the active policy unchanged. */
bool nr_pdsch_config_sweep_set_recovery_policy(uint32_t minimum_failures, double probability_budget);

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
