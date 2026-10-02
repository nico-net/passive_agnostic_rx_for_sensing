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
 * 48 KB: sizeof 180244 -> 229416 B (233520 B with the BC3 dormant masks); the fast-path levers add ok_unique, fp_trials and sib_trials
 * (3 x uint16 arrays, +48 KB), the GEOM mask (+1 KB) and small state: sizeof is now 283840 B (measured), of which fp_trials + sib_trials are +32 KB/state; at 1024 contexts + 4 templates + 1 spare + the legacy singleton
 * (1030 states) that is at most 292 MB (vs 186 MB at 180244 B), and only for slots actually opened. */
#define NR_PDSCH_SWEEP_MAX_HYP 8192
#define NR_PDSCH_SWEEP_MAX_CONTEXTS 1024 /* one per (layout x TDA index) under the wide search; 256 thrashed at 809 layouts */

/* Dormant causes: one mask per cause so independent reasons (a cell prior, each field-book field) can be cleared
 * independently. FIELD_BASE + nr_td_field_t (nr_td_fieldbook.h); FIELD causes 1..3; NR_TD_DORMANT_GEOM = 4 (lever P). */
#define NR_TD_DORMANT_PRIOR 0
#define NR_TD_DORMANT_FIELD_BASE 1
#define NR_TD_DORMANT_GEOM 4 /* lever P: a CRC-pass-pinned geometry group (BC2b); every other geometry is dormant for this cause */
#define NR_TD_DORMANT_CAUSES 5
#define NR_TD_GEOM_SLOTS 8
#define NR_TD_DWORDS ((NR_PDSCH_SWEEP_MAX_HYP + 63) / 64)

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
  /* Lever C (CRC-pass acceptance, spec 2026-10-01 section 3, experimental, default off). ok_unique[h] counts passes on NEW-DATA
   * grants where h was the only ACTIVE hypothesis credited by that nr_pdsch_config_sweep_feed_equiv() call. Evidence-like:
   * cleared together with trials/ok (every prune, rebuild, context reopen). Lever C requires the caller to use feed_equiv for
   * the main decodes: feed / feed_k never touch ok_unique, and becoming blocked does not undo an existing winner. */
  uint16_t ok_unique[NR_PDSCH_SWEEP_MAX_HYP]; ///< passes on grants where the hypothesis was alone in its equivalence class
  bool     crc_accept_blocked; ///< a second active hypothesis has a unique pass: lever C off until the next prune/rebuild
  /* Lever P (partition / geometry acceptance, spec 2026-10-01 section 3b, experimental, default off). Counts new-data CRC passes per
   * GEOMETRY group (nr_td_geom_key: S, L, k0, mapping type, DM-RS mask; the MCS table is NOT in the key). When all passes
   * so far sit in ONE group G and ok_geom[G] >= nr_pdsch_config_sweep_crc_accept_m(n_groups_active, T_g,max) (T_g = sum of fp_trials over the group's active members), every other geometry
   * becomes dormant for cause NR_TD_DORMANT_GEOM (reversible: fail-open or clear_dormant). It never decides a winner. Evidence-like: the
   * slots are cleared everywhere ok_unique is (lever_c_restart, clear_probe_stats, rebuild, new context) and at the hypothesis-adding
   * sites; the pin's own active-set change restarts them (so a pin cannot loop). Two geometries with a pass, or more than
   * NR_TD_GEOM_SLOTS, block it (sticky until the next restart). Not run while fail_open. */
  uint64_t geom_key[NR_TD_GEOM_SLOTS];
  uint16_t ok_geom[NR_TD_GEOM_SLOTS];
  int      n_geom;
  bool     geom_blocked;
  bool     winner_by_crc; ///< the winner was decided by lever C (diagnostic; false after a reset of the winner)
  /* FAST-PATH EVIDENCE STREAM (fix A, round 1). The fast-path levers (C and P) count only passes of EXPLORATION picks (a round-robin slot of the
   * shuffled round, nr_td_pick_t NR_TD_PICK_EXPLORE) and their m* uses T_max over fp_trials = explore trials only. Argument: every active
   * hypothesis receives at most one exploration slot per round, the slot order is fixed by the shuffle/ordering and does not depend on any
   * decode outcome, so for a WRONG hypothesis its fast-path passes are Binomial(fp_trials, p_f) and the union bound C(T, m) p_f^m applies.
   * Exploit (hot) and sibling-test picks are real KL trials but never fast-path evidence: a hot hypothesis gets 3/4 of the trials after one
   * pass, which a per-hypothesis trial count cannot bound. uint16, saturating (+16 KB/state; with sib_trials +32 KB). Cleared wherever ok_unique is. */
  uint16_t fp_trials[NR_PDSCH_SWEEP_MAX_HYP];
  /* K0-SIBLING GUARD (fix B). Siblings of a lead (lever C leader L / lever P group G): ACTIVE hypotheses with identical tda_start,
   * tda_length, mapping_type and dmrs_mask but a different k0 (table, add_pos, max_len free). Before a fast accept/pin every sibling needs
   * N_sib = nr_pdsch_config_sweep_sib_n(n_sib, sib_pmin, sib_eps) sibling-test trials (picks of kind NR_TD_PICK_SIBLING, scheduled
   * deliberately by next_ex while a lead waits) with ZERO passes; a pass on a sibling-test trial sets sib_blocked (fast path off until the
   * next evidence restart). If a sibling were the truth its per-trial pass probability is >= sib_pmin whenever the test runs, so
   * P(0 passes in N_sib) <= (1 - p_min)^N_sib <= eps / n_sib per sibling. sib_pmin <= 0 disables the guard (fix A only).
   * DORMANT siblings are ignored by the guard (only ACTIVE hypotheses are siblings). That is safe only under CORRECT dormancy (the GEOM cause
   * right after a guarded pin; later, k0-certified causes). A wrong k0 FIELD (a field-book/prior mask that hides the true k0) is a known
   * hole: the guard cannot test a sibling that is dormant; BC8/BC9 address it. */
  uint16_t sib_trials[NR_PDSCH_SWEEP_MAX_HYP];
  bool     sib_blocked;
  bool     sib_skip;   ///< BC9: the last sibling pick could not be decoded; the next pick is a normal one
  uint16_t sib_skips;  ///< BC9 M6: skips since the last evidence restart; NR_TD_SIB_SKIP_MAX blocks the fast path
  struct { bool valid; uint64_t skey; uint8_t k0; } sib_t[2]; ///< pending sibling-test targets: [0] lever C leader, [1] lever P group
  /* CONFIGURATION, not catalog/evidence: preserved across catalog rebuilds (nr_pdsch_config_sweep_rebuild(),
   * i.e. context reopen and prior restore); a brand-new runtime context starts with NULL/false. */
  const struct nr_td_side_info_s *side; ///< ordering side information (nr_td_order.h); NULL = neutral (today's order)
  bool     p2;        ///< failure-only probe evidence enabled
  bool     crc_accept; ///< lever C enabled (configuration: preserved across rebuild like side/p2; a new context starts false)
  bool     geom_pin;   ///< lever P enabled (configuration: preserved across rebuild like crc_accept; a new context starts false)
  float    sib_pmin;   ///< sibling guard p_min (configuration; default 0.05; <= 0 disables the guard)
  float    sib_eps;    ///< sibling guard error budget eps_sib (configuration; default 1e-6)
  /* DORMANT (reversible) hypothesis masks, blind-convergence spec 2026-10-01 section 4. CONFIGURATION+MEMBERSHIP, not
   * evidence: preserved by nr_pdsch_config_sweep_rebuild(), compacted with the same keep-index mapping by every destructive
   * prune (prune_commit, prune_keep). One bit per hypothesis index per cause; bits at indices >= n_hyp are always 0.
   * active(i) = fail_open || no cause marks i. A dormant hypothesis is never selected (next/next_k, incl. the exploit "hot"
   * pick and K-probes) and accumulates no evidence (feed/feed_k/feed_equiv ignore it, including probe counters). The
   * acceptance (leader search, separation test, union-bound class count, SWEEP_MIN_TRIALS fallback, ratio test) ranges over
   * the ACTIVE set only. Invariant: at least one hypothesis is active under the masks alone (set_dormant refuses to empty
   * the catalogue; a destructive prune that would leave none clears all masks). A decided winner is returned by next()
   * unconditionally. A new runtime context starts with all masks clear and fail_open false. */
  uint64_t dormant[NR_TD_DORMANT_CAUSES][NR_TD_DWORDS];
  bool     fail_open; ///< all hypotheses active regardless of dormant masks (per context)
  /* EVIDENCE-like (cleared with trials/ok by every prune, rebuild, context reopen): credited TRIALS, never wall-clock time,
   * since the last PASS of an active hypothesis. +1 per feed / feed_k / feed_equiv CALL that credited at least one active
   * hypothesis (feed_k: one call = main outcome + its probes, counted once; probe outcomes count only when they add KL
   * evidence, i.e. a P2-admissible FAIL); reset to 0 by a PASS credited to an active hypothesis (a feed_k probe PASS is not
   * evidence and never resets it). Also reset to 0 whenever the active set changes: set_fail_open toggling, and set_dormant / clear_dormant calls that
   * change at least one mask bit (these also clear ok_unique and crc_accept_blocked). Saturates at UINT32_MAX. Input of nr_pdsch_config_sweep_fail_open_due(). */
  uint32_t since_pass;
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

/** What kind of slot a pick was. Fast-path evidence (levers C/P) comes from EXPLORE picks only; SIBLING picks are the guard's deliberate
 *  tests of k0 siblings. A caller of the fast path MUST pass the kind returned by next_ex/next_k_ex to the *_ex feed functions. */
typedef enum { NR_TD_PICK_EXPLORE = 0, NR_TD_PICK_EXPLOIT = 1, NR_TD_PICK_SIBLING = 2 } nr_td_pick_t;
/** nr_pdsch_config_sweep_next() plus the pick kind. Identical RNG/cursor behaviour to next() unless a lever is on and a lead waits for its
 *  sibling tests, in which case the next sibling (fewest sib_trials, lowest index) is returned with kind SIBLING. next() itself never
 *  schedules siblings (it is the lever-off path). */
int nr_pdsch_config_sweep_next_ex(nr_pdsch_config_sweep_state_t *st, nr_pdsch_cfg_hypothesis_t *out, nr_td_pick_t *kind);
/** N_sib = ceil(ln(n_sib / eps) / pmin); 0 when n_sib <= 0 or pmin <= 0 (guard disabled). Clamped to 65535. */
int nr_pdsch_config_sweep_sib_n(int n_sib, double pmin, double eps);
/** Lever P trial accounting: n_groups = distinct geometry keys among ACTIVE hypotheses; t_g_max = max over those groups of the SUM of fp_trials
 *  over the group's active members (T_g: ok_geom sums the passes of every member, so T_g, not a per-hypothesis T, bounds a wrong group).
 *  m_P* = nr_pdsch_config_sweep_crc_accept_m(n_groups, t_g_max). Read-only. Returns 0, or -1 on a NULL argument / allocation failure. */
int nr_pdsch_config_sweep_geom_groups(const nr_pdsch_config_sweep_state_t *st, int *n_groups, uint32_t *t_g_max);
/* Dormant hypotheses are skipped; the per-round shuffle still covers all n_hyp (RNG use unchanged), a round whose remainder is
 * all dormant advances to the next round, and the exploit "hot" hypothesis must be active. */

/** Report the TB-CRC outcome of the grant decoded under hypothesis `idx`.
 * Returns the winning index once one is established, else -1. */
int nr_pdsch_config_sweep_feed(nr_pdsch_config_sweep_state_t *st, int idx, bool tb_crc_ok);

/* Credit one full-TB outcome to idx[0] (the decoded hypothesis) and to its grant-equivalent alive hypotheses
 * idx[1..n-1]. Duplicates and out-of-range members are ignored; an invalid idx[0] (or n < 1) credits nothing and
 * returns the current winner, as _feed does. n == 1 is bit-identical to
 * nr_pdsch_config_sweep_feed(st, idx[0], tb_crc_ok). The acceptance check runs once, after crediting, whenever any
 * credited hypothesis reached a multiple of 16 trials. Returns the winner or -1.
 * Equivalence (blind-convergence spec 2026-10-01 section 2) is the caller's job: equal nr_td_equiv_key() on this
 * grant. The caller passes the FULL grant-equivalence class including dormant members (crediting skips dormant members,
 * but a pass is UNIQUE only when the class has exactly one distinct in-range member, dormant ones counted: a dormant twin
 * may be the truth). new_data matters only with st->crc_accept (lever C): a pass with tb_crc_ok && new_data on a one-member class counts in ok_unique[idx[0]] (saturating); before the KL decision, if exactly one active hypothesis has
 * ok_unique > 0 and it reaches nr_pdsch_config_sweep_crc_accept_m(n_active, max active trials), it wins; two or more such
 * hypotheses set crc_accept_blocked (sticky until the next prune/rebuild/reopen). With crc_accept false the behaviour is
 * bit-identical to the KL-only rule. */
int nr_pdsch_config_sweep_feed_equiv(nr_pdsch_config_sweep_state_t *st, const int *idx, int n, bool tb_crc_ok,
                                     bool new_data /* new transmission (NDI toggled); used only by levers C and P */);

/** Attribution-only variant, the RUNTIME API for the levers when grant-equivalence CREDITING is not wanted (lever E off):
 *  credits ONLY idx0 (exactly nr_pdsch_config_sweep_feed(st, idx0, tb_crc_ok), bit-identical while crc_accept and geom_pin
 *  are false), but uses `cls[0..n_cls)` -- the FULL grant-equivalence class of the decode INCLUDING dormant members --
 *  for the lever-C uniqueness test (a pass is unique iff the class has exactly one distinct in-range member; idx0 always counts
 *  as a member) and for the lever-P attribution (a class spanning two geometry groups gives lever P no evidence). Passing the
 *  singleton {idx0} instead would make every pass look unique (UNSAFE). nr_pdsch_config_sweep_feed_equiv() is a thin wrapper
 *  over the same internal function (idx = cls = the class, crediting every active member). Order inside the shared
 *  function: credit -> lever P (may pin; restarts the evidence) -> lever C -> KL decision (sweep_decide). A pin skips the
 *  lever-C accumulation of that same call. */
int nr_pdsch_config_sweep_feed_attr(nr_pdsch_config_sweep_state_t *st, int idx0, const int *cls, int n_cls, bool tb_crc_ok,
                                    bool new_data);
/** The runtime/fast-path forms: `kind` is the pick kind of the decoded hypothesis idx0 (from next_ex / next_k_ex). The legacy feed_equiv /
 *  feed_attr are these with kind = EXPLOIT: without a pick kind no outcome is fast-path evidence, so a lever paired with the old next()
 *  fails safe (never accepts/pins). Tests that need fast-path credit use the _ex forms with NR_TD_PICK_EXPLORE. Lever-off behaviour is bit-identical. */
int nr_pdsch_config_sweep_feed_equiv_ex(nr_pdsch_config_sweep_state_t *st, const int *idx, int n, bool tb_crc_ok, bool new_data,
                                        nr_td_pick_t kind);
int nr_pdsch_config_sweep_feed_attr_ex(nr_pdsch_config_sweep_state_t *st, int idx0, const int *cls, int n_cls, bool tb_crc_ok,
                                       bool new_data, nr_td_pick_t kind);

/** BC9 forms with the DCI-adjacency `certified` flag (nr_dci_hist_k0_certified): levers C and P count a pass, and fp_trials a
 *  trial, only for an EXPLORE pick on a k0-unambiguous grant (certified). Uncertified trials stay normal KL evidence. The _ex
 *  forms above are _cx with certified = false (fail-safe, BC9 review I1): a caller that does not state certification never
 *  builds fast-path evidence. */
int nr_pdsch_config_sweep_feed_equiv_cx(nr_pdsch_config_sweep_state_t *st, const int *idx, int n, bool tb_crc_ok, bool new_data,
                                        nr_td_pick_t kind, bool certified);
int nr_pdsch_config_sweep_feed_attr_cx(nr_pdsch_config_sweep_state_t *st, int idx0, const int *cls, int n_cls, bool tb_crc_ok,
                                       bool new_data, nr_td_pick_t kind, bool certified);
/** Sibling liveness (BC2b carry-forward): the caller could not decode the SIBLING pick idx (its slot was not captured, ...).
 *  No evidence; the next next_ex()/next_k_ex() call returns a normal pick instead of a sibling (at most every other pick is
 *  spent on an undecodable sibling, so the RNTI never stalls). A TDD-impossible sibling is excluded instead (it is gone). */
void nr_pdsch_config_sweep_sib_skip(nr_pdsch_config_sweep_state_t *st, int idx);
/** Review M6: after this many skips since the last evidence restart the fast path is blocked (fail-safe: KL decides). */
#define NR_TD_SIB_SKIP_MAX 64

/** Lever C threshold: smallest m >= 2 with n_alive * C(t_max, m) * 2^(-24 m) <= 1e-6 (log domain, lgamma). m = 2 when
 *  n_alive <= 1 or t_max == 0; for t_max < m, C = 0 so m qualifies at once (result max(2, m)). */
int nr_pdsch_config_sweep_crc_accept_m(int n_alive, uint32_t t_max);

/** K = 1 is exactly nr_pdsch_config_sweep_next(). Returns n filled (1..K, K clamped to NR_TD_MAX_K;
 *  0 when nothing can be selected); idx[0]/out[0] = the main hypothesis (unchanged path and RNG use),
 *  idx[1..n-1] = distinct probe hypotheses that are not yet cleared (>= SWEEP_MIN_TRIALS trials, no pass),
 *  taken from the current round order at the cursor WITHOUT advancing it or consuming RNG. Once a winner
 *  exists only the winner is returned (n = 1). */
int nr_pdsch_config_sweep_next_k(nr_pdsch_config_sweep_state_t *st, int K, int idx[], nr_pdsch_cfg_hypothesis_t out[]);
/** next_k with the pick kind of the MAIN hypothesis idx[0] (see next_ex). K = 1 is exactly next_ex(). */
int nr_pdsch_config_sweep_next_k_ex(nr_pdsch_config_sweep_state_t *st, int K, int idx[], nr_pdsch_cfg_hypothesis_t out[], nr_td_pick_t *kind);

/** outcomes[0] must be the main FULL_TB outcome: PASS/FAIL go through nr_pdsch_config_sweep_feed();
 *  INCONCLUSIVE (or a non-FULL_TB entry) is not fed. outcomes[1..n-1] are probes: they only update the
 *  probe counters, except that with st->p2 an admissible FAIL adds exactly one KL failure. A probe PASS
 *  never adds KL evidence. Returns the winner index or -1 (same contract as _feed). */
int nr_pdsch_config_sweep_feed_k(nr_pdsch_config_sweep_state_t *st, const nr_td_outcome_t *outcomes, int n);

/* ---- Dormant masks and fail-open (blind-convergence spec section 4) ------------------------------------------ */
typedef bool (*nr_td_keep_fn_t)(const nr_pdsch_cfg_hypothesis_t *h, const void *arg);
/** Marks every hypothesis with !keep(h) dormant for `cause` (additive: bits already set stay set). Returns the number
 *  of hypotheses newly dormant FOR THIS CAUSE (resets since_pass when a bit changed), or -1 (nothing changed) if st/keep is NULL, `cause` is out of range, or
 *  the result would leave zero active hypotheses under the masks (fail_open is ignored for this test). */
int nr_pdsch_config_sweep_set_dormant(nr_pdsch_config_sweep_state_t *st, int cause, nr_td_keep_fn_t keep, const void *arg);
/** Clears `cause`; returns the number of hypotheses that thereby became active under the masks (0 if cause is out of range). */
int nr_pdsch_config_sweep_clear_dormant(nr_pdsch_config_sweep_state_t *st, int cause);
void nr_pdsch_config_sweep_set_fail_open(nr_pdsch_config_sweep_state_t *st, bool on);
/** active(i) = fail_open || no cause marks i; false for an out-of-range i. */
bool nr_pdsch_config_sweep_is_active(const nr_pdsch_config_sweep_state_t *st, int i);
/** Number of active hypotheses (n_hyp while fail_open or with no mask set). */
int  nr_pdsch_config_sweep_n_active(const nr_pdsch_config_sweep_state_t *st);
/** !fail_open && since_pass > 0 && since_pass >= ceil(n_active * ln(1/alpha) / p_min); counts trials, not time. A state with no
 *  credited trial is never due. alpha outside (0,1) or p_min <= 0 is never due. */
bool nr_pdsch_config_sweep_fail_open_due(const nr_pdsch_config_sweep_state_t *st, double alpha, double p_min);
/** Destructive prune by predicate, exactly prune_commit() semantics (0 = nothing kept: untouched; unchanged count = all kept:
 *  evidence retained; else the new count with all evidence and since_pass cleared, cursor 0, winner -1), compacting the dormant
 *  masks with the same keep-index mapping. keep(h) true retains h; dormant hypotheses are kept or dropped by the predicate too. */
int nr_pdsch_config_sweep_prune_keep(nr_pdsch_config_sweep_state_t *st, nr_td_keep_fn_t keep, const void *arg);

/** Pure k0-layer append on a state (what nr_pdsch_config_sweep_add_k0 does on a live context): copies the lowest-k0 layer with
 *  k0 replaced; each new entry inherits its source entry's dormant bits. Returns the number added (0 = nothing/does not fit). */
int nr_pdsch_config_sweep_add_k0_layer(nr_pdsch_config_sweep_state_t *st, uint8_t k0);

/** Winner, or -1 if undecided. */
int nr_pdsch_config_sweep_winner(const nr_pdsch_config_sweep_state_t *st);

typedef int32_t (*nr_pdsch_legality_fn_t)(int, int, int, int, int, int);
/** Rebuild the full catalog in place exactly as a runtime context does (shared template copy, or
 *  init_legal() when none is available), discarding all evidence but KEEPING the configuration fields
 *  side and p2 and the dormant masks / fail_open (masks are by index: the caller re-applies them if the catalogue
 *  changed; bits >= the new count are dropped and an all-dormant result is cleared). st must already be a valid state. Returns the hypothesis count. */
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
  uint64_t configuration; ///< BC9 M3: the context's configuration key (matches the DCI history entry)
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
 *  PRBs (-1 = unmeasured) and k0_plausible, the k0 the measuring job hypothesised (-1 = unknown). K39: prunes on
 *  mask and last symbol ONLY; k0_plausible is recorded as plausible (ordering/logging) and pins nothing, because DM-RS
 *  in a slot does not prove which slot offset the grant has. (Legacy: ISAC_TD_K0_ORACLE_LEGACY=1 pins it as before.) */
int nr_pdsch_config_sweep_observe(const nr_pdsch_sweep_ticket_t *ticket, uint16_t dmrs_mask, int last_symbol, int k0_plausible);
/** K39: k0 certified by deterministic evidence (BC9 DCI adjacency / TDD direction): the ONLY call that may prune k0.
 *  Keeps the entries whose k0 is in k0_allowed_mask (bit k = k0 k), for this ticket's (configuration, RNTI, TDA row) only: k0 is a per-row field,
 *  so a certification never binds another row. Persists per that key across context eviction; cleared on reopen and on a
 *  configuration change. Binds the context against later observations, k0 layers and restores. Prunes nothing when no entry would survive. Returns the live hypothesis count (0 = no context). */
int nr_pdsch_config_sweep_certify_k0(const nr_pdsch_sweep_ticket_t *t, uint64_t k0_allowed_mask);
/* ---- BC9: deterministic per-hypothesis exclusion (TDD slot direction, DCI adjacency) ----------------------------------
 * A hypothesis (S, L, k0) of a TDRA row is IMPOSSIBLE when the PDSCH it implies would end after the last symbol the slot
 * s + k0 can carry (s = the DCI slot): a UL slot carries none, a mixed slot none on its common UL symbols (TS 38.213 11.1,
 * 38.214 5.1.2). The rule is per hypothesis, not a k0 mask: a mixed slot removes only the long entries. last[k] is the
 * highest symbol a k0 = k entry may END on (13 = unconstrained, -1 = k0 impossible); DCI adjacency (nr_dci_history.h)
 * expresses "k0 = k impossible for this row" as last[k] = -1. Constraints of several DCIs of one row intersect (element-wise
 * minimum: the row's (S, L, k0) is the same for all its DCIs). Deterministic, so it is a destructive prune (never a
 * dormant cause, which fail-open would reopen), applied through the BC7 certification path: scoped to the (configuration,
 * RNTI, TDA row) context, persisted per key in the RNTI's LRU certification set, inherited on context creation, re-applied
 * after observe / k0 layers / restores, cleared on reopen (re-derived from the next DCI). Without TDD knowledge (NSA, a
 * cell without SIB1, an unverified pattern) the caller passes nothing: no exclusion. */
#define NR_TD_K0_MAX 32
typedef struct {
  int8_t last[NR_TD_K0_MAX + 1];
} nr_td_excl_t;
/** Every last[k] = 13 (no constraint). */
void nr_td_excl_none(nr_td_excl_t *e);
/** True when h survives e: k0 <= 32 and tda_start + tda_length - 1 <= last[k0]. */
bool nr_td_excl_admits(const nr_td_excl_t *e, const nr_pdsch_cfg_hypothesis_t *h);
/** Pure: prune_keep(st, admits) (0 = nothing would survive: untouched; unchanged count = evidence kept). */
int nr_pdsch_config_sweep_exclude(nr_pdsch_config_sweep_state_t *st, const nr_td_excl_t *e);
/** Live, keyed by (configuration, RNTI, TDA row): intersects e into the key's persisted constraint and binds every live
 *  context of that key. Refuses (returns -1, nothing changes) a constraint that would leave no k0 of the row's universe
 *  ({0,1}, the RNTI's k0-oracle layers) with any legal entry -- evidence that an assumption (A1-A3) failed. Else returns
 *  the number of hypotheses removed from live contexts (0 = none open or nothing to remove). */
int nr_pdsch_config_sweep_exclude_key(uint64_t configuration, uint16_t rnti, uint8_t tda, const nr_td_excl_t *e);
/** The deterministic allowed-k0 set of a row: the universe ({0,1} | the RNTI's k0-oracle layers), intersected with the
 *  key's k0 certification and with the k0 values its persisted exclusion leaves any legal entry (end symbol >= 1).
 *  An over-approximation of the truth's k0 (A2); popcount 1 = certified. */
uint64_t nr_pdsch_config_sweep_row_k0_allowed(uint64_t configuration, uint16_t rnti, uint8_t tda);
/** For the DCI-adjacency certified flag of a decoded job: the ticket's hypothesis, the k0 values of the hypotheses that are
 *  k0 siblings of it (same S, L, mapping type, DM-RS mask; active or dormant through a non-GEOM cause), and the MCS tables of
 *  the active hypotheses. False for a stale ticket or a settled context. */
/** Pure form of ticket_siblings on a state and a hypothesis index. tables = MCS tables of every hypothesis that may be the
 *  truth: active, or dormant through a non-GEOM cause (PRIOR / FIELD can be wrong; GEOM follows a guarded pin). */
bool nr_pdsch_config_sweep_siblings_of(const nr_pdsch_config_sweep_state_t *st, int idx, nr_pdsch_cfg_hypothesis_t *h,
                                       uint64_t *sib_k0, uint8_t *tables);
bool nr_pdsch_config_sweep_ticket_siblings(const nr_pdsch_sweep_ticket_t *t, nr_pdsch_cfg_hypothesis_t *h, uint64_t *sib_k0,
                                           uint8_t *tables);
/** True when some row of (rnti, configuration) carries a k0 certification or an exclusion (else every row's allowed set
 *  is the bare universe and no DCI-adjacency exclusion can follow). One lock; the accept hook's fast path. */
/** Lock-free epoch: changes whenever a persisted certification / exclusion may have disappeared (reopen, RNTI or LRU eviction,
 *  reset). Callers caching "constraint already applied" must drop the cache when it moves. */
uint64_t nr_pdsch_config_sweep_cert_epoch(void);
bool nr_pdsch_config_sweep_rnti_constrained(uint16_t rnti, uint64_t configuration);
/** Diagnostics: hypotheses removed by exclusions with k0 < 2 / k0 >= 2 (probe layers), refused contradictions. */
void nr_pdsch_config_sweep_excl_stats(uint64_t *removed_k0_lt2, uint64_t *removed_k0_ge2, uint64_t *refused);
/** Test hook: force the ISAC_TD_K0_ORACLE_LEGACY decision (1 = old k0 pinning, 0 = default, -1 = re-read the env). */
void nr_pdsch_config_sweep_k0_legacy_set(int legacy);
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
