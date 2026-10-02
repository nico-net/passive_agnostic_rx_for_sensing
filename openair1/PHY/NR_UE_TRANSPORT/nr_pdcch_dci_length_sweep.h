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

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_dci_length_sweep.h
 * \brief Phase 3 Technique C: lock dci_length by histogram, ACCUMULATED ACROSS OCCASIONS (rewritten
 * 2026-09-06 -- see nr_pdcch_dci_length_sweep.c's header for why the original one-shot,
 * single-occasion design could not work on live air). DCI 1_1 lengths land in roughly a 30-70 bit
 * range (roadmap artifact), so a linear sweep is ~40 hypotheses.
 *
 * THREE TRAPS ALREADY PAID FOR IN THIS PROJECT, all of which this module's scoring must reject
 * (see PHASE1_CSS0_AUTOCONF_HANDOVER.md, TOTAL_PASSIVE_UE_HANDOVER.md and
 * PHASE3_DEDICATED_CONFIG_RECOVERY_HANDOVER.md for the history):
 *  1. `upper 8 bits == 0` alone is a 1-in-256 test -- produced 42k chance passes on 10.9M
 *     candidates. Never score a length by pass-count alone.
 *  2. A spiked histogram at some length is NOT sufficient if the decoded payload is INVARIANT
 *     across instances -- that is a degenerate polar fixed point, not traffic (3744 byte-identical
 *     "decodes" were once mistaken for a working length).
 *  3. (NEW, 2026-09-06) A FIXED absolute pass-count floor only holds while total trials per length
 *     stays small (~64, the original one-shot design's whole budget). Accumulated over many
 *     occasions to get enough REAL exposure, total trials per length can reach the hundreds or
 *     thousands, at which point a fixed floor is trivially cleared by CHANCE ALONE (at this
 *     project's own measured ~1/256 chance-pass rate, ~700 trials already has an expected ~2.7
 *     chance passes). The significance test must scale with accumulated trial count.
 *
 * This module owns only the SWEEP + SCORING logic; it does not decode anything itself and does not
 * duplicate the RT monitor's own candidate-iteration loop. It takes a caller-supplied
 * nr_pdcch_dci_length_scorer_fn so it stays testable in isolation (see
 * tests/nr_pdcch_dci_length_sweep_test.cc, which drives it with a synthetic stub) and decoupled
 * from the real candidate stream.
 */
#ifndef NR_PDCCH_DCI_LENGTH_SWEEP_H
#define NR_PDCCH_DCI_LENGTH_SWEEP_H

#include "nr_dci_bits.h"
#include "nr_dci11_pin.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Attempt one candidate decode at the given dci_length.
 *
 * @param dci_length       Hypothesis under test.
 * @param trial_idx        CALL-LOCAL trial counter (0..n_trials_this_call-1) -- accumulation
 *                          across calls is the sweep state's job, not this callback's.
 * @param rnti_out         CRC-recovered RNTI, filled only if the caller returns true.
 * @param payload_hash_out A cheap hash/fingerprint of the decoded payload bits, filled only if the
 *                          caller returns true -- used to detect an invariant (degenerate) payload.
 * @param user_ctx          Opaque, passed through unchanged.
 * @return true iff this trial's CRC-adjacent check passed (e.g. upper 8 bits zero) -- the SWEEP,
 *         not this callback, decides whether the length is real.
 */
typedef bool (*nr_pdcch_dci_length_scorer_fn)(int dci_length, int trial_idx, uint16_t* rnti_out,
                                              uint32_t* payload_hash_out, void* user_ctx);

#define NR_PDCCH_DCI_LENGTH_SWEEP_MAX_LEN (NR_DCI_MAX_PAYLOAD + 1)
                                                 // length, no offset arithmetic to get wrong.
#define NR_PDCCH_DCI_LENGTH_SWEEP_MAX_HASHES 32 // cap on distinct-payload bookkeeping per length;
                                                 // degenerate detection only needs to distinguish
                                                 // "1 distinct value" from "more than 1".

/* Blind evidence table shared by all lengths in one geometry. A real dedicated DCI repeatedly
 * recovers the same CRC-scrambled RNTI while its payload changes with scheduling. Random/structured
 * false accepts do not get to borrow evidence from other RNTIs. */
#define NR_PDCCH_DCI_LENGTH_SWEEP_RNTI_TRACKERS 128
#define NR_PDCCH_DCI_LENGTH_SWEEP_RNTI_HASHES   5
#define NR_PDCCH_DCI_LENGTH_SWEEP_RNTI_LOCK     5
typedef struct {
  uint16_t rnti;
  uint8_t  len;
  uint8_t  n_distinct;
  uint16_t support;
  uint32_t last_feed; /* one identity vote at most per OTA occasion */
  uint32_t hashes[NR_PDCCH_DCI_LENGTH_SWEEP_RNTI_HASHES];
} nr_pdcch_dci_length_rnti_evidence_t;

/* Cell-wide ordering hints only: never acceptance evidence. Thread-safe for scan consumers. */
void nr_pdcch_dci_length_seen_reset(void);
void nr_pdcch_dci_length_note_seen(int len);
int nr_pdcch_dci_length_order(int min_len, int max_len, int *out);
bool nr_pdcch_reconf_enabled(void);

/** Persistent state, accumulated across many nr_pdcch_dci_length_sweep_feed() calls (one call per
 *  candidate-bearing occasion). Plain struct, no hidden allocation -- zero-initialize (static
 *  storage, or nr_pdcch_dci_length_sweep_reset()) before the first feed. */
typedef struct {
  int      trials[NR_PDCCH_DCI_LENGTH_SWEEP_MAX_LEN];         // total attempts at this length
  int      passes[NR_PDCCH_DCI_LENGTH_SWEEP_MAX_LEN];         // of which, CRC-adjacent passes
  int      bootstrap_hits[NR_PDCCH_DCI_LENGTH_SWEEP_MAX_LEN]; // of which, hit the known RNTI
  uint32_t hashes[NR_PDCCH_DCI_LENGTH_SWEEP_MAX_LEN][NR_PDCCH_DCI_LENGTH_SWEEP_MAX_HASHES];
  int      n_distinct[NR_PDCCH_DCI_LENGTH_SWEEP_MAX_LEN];
  nr_pdcch_dci_length_rnti_evidence_t
      rnti_evidence[NR_PDCCH_DCI_LENGTH_SWEEP_RNTI_TRACKERS];
  uint8_t  max_rnti_distinct[NR_PDCCH_DCI_LENGTH_SWEEP_MAX_LEN];
  /* ROUNDS of full exposure, NOT calls: incremented once every `stride` calls, when the rotation
   * below has visited every length exactly once. Identical to the call count at stride<=1. The
   * caller's give-up cap (AUTODISCOVER_LENGTH_SWEEP_MAX_OCCASIONS) counts these, so each length
   * keeps its full trial budget however coarsely the rotation spreads it in time. */
  int      occasions_fed;
  /* A length known to belong to ANOTHER format pair on the same search space (the derived 1_0/0_0
   * size): its CRC passes are real but say nothing about the 1_1/0_1 size the sweep is after. The
   * SA rfsim cell locked 44 = its 1_0 size on 58 format-1_0 accepts (2026-09-16). 0 = none. */
  int      excluded_len;
  int      secondary_excluded_len; /* optional second format length during locked-length scouting */
  /* ROTATION. <=1 (the zero-initialised default) tests every length on every call, which is what
   * this sweep did unconditionally until 2026-09-17 -- and what made it the receiver's dominant
   * cost: 34 lengths x ~6 candidates x ~8us of Polar+CRC is ~1.75ms on EVERY occasion, against a
   * ~667us occasion arrival interval, so 68.5% of PDCCH occasions were dropped unprocessed
   * (scanq drop_full=618213/902224) and the CORESET search saw a third of the air. At stride=N
   * each call tests every Nth length, phase-shifted, so a length is visited once per N calls:
   * the per-occasion cost falls ~N-fold while the trials each length accumulates per ROUND is
   * unchanged. Set it at the call site before the first feed (the same place excluded_len is set). */
  int      stride;
  int      order[NR_PDCCH_DCI_LENGTH_SWEEP_MAX_LEN], order_count;
  int      resume_len, resume_trial; // budget suspension; reset with the geometry epoch
  bool     wide_range; /* every stage-1 length reached the statistical trial floor without a lock */
  int      round_max; /* frozen across rotation/budget suspension */
  int      rot_phase; // 0..stride-1, which interleaved subset this call tests
  /* CELL PRIOR (2026-09-17). When > 0, test ONLY this length for the first
   * NR_PDCCH_LENGTH_PREFERRED_ROUNDS rounds instead of all 34. Set from the bank's cell-wide length
   * once TWO DISTINCT RNTIs have converged on it independently -- so a third UE is not trusting a
   * guess, it is re-testing a hypothesis that two full sweeps already agreed on, under the SAME
   * significance test. If it does not clear inside the budget the prior is dropped and the full
   * sweep resumes, so a wrong prior costs a handful of occasions, not a lockout. */
  int      preferred_len;
  int      preferred_rounds;
  int      alt_full;  /* 1 = running one full-sweep lap between hypothesis rounds */
  /* Total scorer invocations = total polar decodes this sweep has paid for. Paired with BTIM's
   * dlsweep/ulsweep nanoseconds it gives microseconds PER DECODE and the batch actually available
   * per occasion -- the two numbers that decide whether a GPU batch can win here. The failed GPU
   * LDPC attempt lost because consumers blocked and the batcher only ever saw 1.05 TBs; this sweep
   * offers ~34 lengths x ~6 candidates of INDEPENDENT work per occasion, from one work item, over
   * the SAME LLR slice. Measure it rather than assume it. */
  uint64_t decodes;
  uint32_t feed_serial; /* distinct OTA occasions; resumed work never manufactures recurrence */
  int relock_old_len; /* SUSPECT only: old length, then seen lengths, then full range */
} nr_pdcch_dci_length_sweep_state_t;
/* Active range shared by CPU scoring, prefill and UL uniqueness checks. Explicit
 * ISAC_DCI_LEN_MAX or a cell-seen wide length bypasses the cold 63-bit cap. */
int nr_pdcch_dci_length_active_max(const nr_pdcch_dci_length_sweep_state_t *state,
                                   int min_len, int max_len);
/* Rounds a seeded length gets before the full sweep resumes. The sweep's own significance test
 * needs accumulated trials, and one occasion carries only ~6 candidates; 8 rounds is ~50 candidates,
 * comfortably enough for a length that is already right and nowhere near enough to make a wrong one
 * look right. */
#define NR_PDCCH_LENGTH_PREFERRED_ROUNDS 8

/* A caller-serialized bank. Interleaved UEs never reset one another; geometry
 * epoch changes invalidate all entries. Eviction discards evidence, never reuses it. */
#define NR_PDCCH_LENGTH_CONTEXTS 16
typedef enum { NR_LEN_SEARCHING = 0, NR_LEN_LOCKED = 1, NR_LEN_SUSPECT = 2 } nr_len_state_t;
uint32_t nr_pdcch_dci_length_n_suspect_from_env(void);
typedef struct {
  nr_pdcch_dci_length_sweep_state_t state;
  uint16_t rnti;
  int found[2]; /* oldest-compatible primary and one additional significant length */
  uint32_t found_recent[2];
  nr_dci11_pin_t layout_pin[2];
  uint32_t layout_cursor[2];
  uint8_t len_state;
  uint32_t miss_occasions;
  uint32_t epoch_learned;
  uint32_t last_accept_slot, last_note_slot;
  bool scout_initialized;
  bool exhausted;
  uint64_t touched;
} nr_pdcch_dci_length_context_t;
/* Caller serializes with its geometry bank lock. A zero n_suspect uses the default 200. */
void nr_pdcch_dci_length_context_note_occasion(nr_pdcch_dci_length_context_t *c,
    bool accepted_at_locked, bool rnti_active_elsewhere, uint32_t n_suspect);
/* First lock returns 0; re-lock returns the previous length (including same-length confirmation). */
int nr_pdcch_dci_length_context_lock(nr_pdcch_dci_length_context_t *c, int length);
/* Add a significant length, evicting the least recently accepted when both slots are full. */
int nr_pdcch_dci_length_context_add(nr_pdcch_dci_length_context_t *c, int length, uint32_t slot);
void nr_pdcch_dci_length_context_touch(nr_pdcch_dci_length_context_t *c, int length, uint32_t slot);
nr_dci11_pin_t *nr_pdcch_dci_length_context_pin(nr_pdcch_dci_length_context_t *c, int length);
uint32_t *nr_pdcch_dci_length_context_pin_cursor(nr_pdcch_dci_length_context_t *c, int length);
int nr_pdcch_dci_length_context_relock_order(const nr_pdcch_dci_length_context_t *c,
    const int *cell_seen, int n_seen, int *out, int max);
typedef struct {
  nr_pdcch_dci_length_context_t ue[NR_PDCCH_LENGTH_CONTEXTS];
  uint64_t epoch, clock;
  /* Cell-wide UL dci_length is published only once two DISTINCT RNTIs converge. The first
   * result may seed another UE's bounded preferred-length trial, but that UE still has to confirm
   * it with its own CRC/payload evidence and falls back to the full sweep on failure. */
  int      cell_len;    // 0 = not established
  int      first_len;   // the first RNTI to converge, awaiting a second to agree
  uint16_t first_rnti;
  /* Before an RNTI is known, retain joint (length,RNTI) evidence for this exact CORESET.
   * A lock is promoted to a normal per-RNTI context only after distinct-occasion recurrence. */
  nr_pdcch_dci_length_sweep_state_t anonymous;
  int anonymous_found;
  uint16_t anonymous_rnti;
  bool anonymous_exhausted;
} nr_pdcch_dci_length_bank_t;
nr_pdcch_dci_length_context_t *nr_pdcch_dci_length_context(
    nr_pdcch_dci_length_bank_t *bank, uint64_t epoch, uint16_t rnti);
/** Record that @p rnti converged on @p found. Publishes the cell-wide length on agreement between
 *  two distinct RNTIs, so every later context starts from it instead of sweeping. */
void nr_pdcch_dci_length_bank_converged(nr_pdcch_dci_length_bank_t *bank, uint16_t rnti, int found);

/* Persistent LRU of independent CORESET banks. A bank already separates RNTIs; the outer store
 * prevents interleaved physical CORESETs from resetting one another. The exact, caller-computed
 * geometry key is also used as the bank epoch. Eviction loses evidence but can never mix it. */
/* The discovery worker can touch the primary hypothesis plus 127 lookahead
 * geometries in one pass, followed by up to eight already verified CORESETs.
 * The old capacity of 16 was smaller than the normal K=17 working set: every
 * pass evicted the state that the next pass needed, so even repeated valid CRC
 * hits could never reach the two-hit bootstrap lock. */
#define NR_PDCCH_LENGTH_CORESETS 136
typedef struct {
  /* Allocated on first use. Keeping 136 full banks inline would reserve tens of
   * MiB even when discovery only observes one or two physical CORESETs. */
  nr_pdcch_dci_length_bank_t *bank;
  uint64_t key;
  uint64_t touched;
  bool used;
} nr_pdcch_dci_length_coreset_t;
typedef struct {
  nr_pdcch_dci_length_coreset_t coreset[NR_PDCCH_LENGTH_CORESETS];
  uint64_t clock;
} nr_pdcch_dci_length_store_t;
nr_pdcch_dci_length_bank_t *nr_pdcch_dci_length_store_get(
    nr_pdcch_dci_length_store_t *store, uint64_t key, uint64_t *evicted_key);

void nr_pdcch_dci_length_sweep_reset(nr_pdcch_dci_length_sweep_state_t* state);

/**
 * @brief Feed ONE occasion's worth of real candidates into the ongoing sweep and re-check
 *        significance against the ACCUMULATED history (see this header's trap #3 and
 *        nr_pdcch_dci_length_sweep.c's header for why one occasion's candidate volume alone is not
 *        enough). Call this once per candidate-bearing occasion, not once ever.
 *
 * @param state               Persistent state; reset before the first call.
 * @param decode_one_candidate/user_ctx  Per-trial callback, same contract as before.
 * @param n_trials_this_call  This occasion's own candidate count -- every length in
 *                            [min_len,max_len] is tried against all of them.
 * @param min_len/max_len     Hypothesis range.
 * @param bootstrap_rnti      0 = none yet; a hit is near-certain evidence regardless of
 *                            accumulated sample size and short-circuits the chance-floor test.
 * @return the winning dci_length once one clears (sample-size-scaled) significance, or -1 to keep
 *         observing. The caller decides when to give up (a bounded occasion cap of its own).
 */
int nr_pdcch_dci_length_sweep_feed(nr_pdcch_dci_length_sweep_state_t* state,
                                   nr_pdcch_dci_length_scorer_fn decode_one_candidate, void* user_ctx,
                                   int n_trials_this_call, int min_len, int max_len,
                                   uint16_t bootstrap_rnti);

/* Absolute CLOCK_MONOTONIC deadline; zero disables it. max_trials <= 0 is unlimited.
 * Suspended work resumes on fresh candidates on the next occasion. Only completed
 * trials enter evidence, and only completed rounds advance occasions_fed. */
int nr_pdcch_dci_length_sweep_feed_budget(nr_pdcch_dci_length_sweep_state_t *state,
    nr_pdcch_dci_length_scorer_fn scorer, void *ctx, int n_trials, int min_len, int max_len,
    uint16_t bootstrap_rnti, uint64_t deadline_ns, int max_trials);

/* RNTI whose distinct-payload, distinct-occasion evidence caused a blind length lock; zero if none. */
uint16_t nr_pdcch_dci_length_sweep_winner_rnti(
    const nr_pdcch_dci_length_sweep_state_t *state, int len);

#ifdef __cplusplus
}
#endif

#endif
