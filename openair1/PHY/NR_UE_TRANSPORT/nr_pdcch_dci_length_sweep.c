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

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_dci_length_sweep.c
 * \brief See nr_pdcch_dci_length_sweep.h for the full design rationale.
 *
 * REWRITTEN 2026-09-06. The original one-shot design (all TRIALS_PER_LENGTH=64 trials drawn from a
 * SINGLE occasion's candidates, decided once, never retried) could not work on live air: this
 * cell's real accept rate is ~2% of occasions (measured on the proven-working manual-conf path,
 * 2801/138000), so a single occasion's ~20-60 candidates essentially never contains the >=3 real
 * decodable candidates the significance test needed. Live-measured consequence: across three 200s
 * captures with confirmed continuous real DL traffic (Technique A's own correlation confirms this),
 * the sweep found "nothing significant" at every length in [30,63], including 47 (this deployment's
 * own known-correct value, confirmed via the proven-working manual conf).
 *
 * Fixed by making the sweep STATEFUL and accumulating across many occasions -- feed() is called
 * once per candidate-bearing occasion instead of once ever. This introduces a new trap (this
 * header's #3): a FIXED absolute pass floor, calibrated for ~64 total trials, is trivially
 * clearable by chance alone once accumulated trials per length reach the hundreds. Fixed by scoring
 * against a floor that scales with accumulated trial count (a z-score above the expected chance-
 * pass rate), not a constant.
 */
#include "nr_pdcch_dci_length_sweep.h"
#include "common/utils/LOG/log.h"
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdatomic.h>
#include <pthread.h>

// Empirically measured false-accept rate of nr_pdcch_blind_decode_and_extract_ex()'s "plausible"
// gate on this project's own prior data (42k/10.9M candidates -- see this file's header and
// TOTAL_PASSIVE_UE_HANDOVER.md). NOT a theoretical CRC-24 rate: "plausible" is a field-sanity
// heuristic weaker than a genuine CRC match, which is exactly why a significance test is needed at
// all rather than trusting any single pass.
#define CHANCE_PASS_RATE (1.0 / 256.0)

// One-sided significance margin, in chance-rate standard deviations, required before ACCUMULATED
// passes at some length are trusted. With up to 34 simultaneous length hypotheses tested every
// call (this project's own [30,63] range), Bonferroni-correcting for that many simultaneous tests
// still leaves 34 * P(Z > 6) far below any usable false-positive budget -- and because this scales
// with sqrt(trials) rather than being a constant, it stays conservative however long the
// observation window runs (unlike the fixed floor it replaces -- see this file's header).
#define Z_SIGMA 6.0

static pthread_once_t length_env_once = PTHREAD_ONCE_INIT;
static bool seen_order_enabled;
static int explicit_max;
static void length_env_init(void)
{
  const char *e = getenv("ISAC_RECONF");
  seen_order_enabled = e && atoi(e) == 1;
  e = getenv("ISAC_DCI_LEN_MAX");
  char *end = NULL;
  const long n = e ? strtol(e, &end, 10) : 0;
  explicit_max = e && end != e && !*end && n >= 1 && n <= NR_DCI_MAX_PAYLOAD ? (int)n : 0;
}

bool nr_pdcch_reconf_enabled(void)
{
  pthread_once(&length_env_once, length_env_init);
  return seen_order_enabled;
}

static _Atomic bool cell_seen[NR_PDCCH_DCI_LENGTH_SWEEP_MAX_LEN];
void nr_pdcch_dci_length_seen_reset(void)
{
  for (int len = 0; len <= NR_DCI_MAX_PAYLOAD; ++len)
    atomic_store_explicit(&cell_seen[len], false, memory_order_relaxed);
}
void nr_pdcch_dci_length_note_seen(int len)
{
  if (len > 0 && len <= NR_DCI_MAX_PAYLOAD)
    atomic_store_explicit(&cell_seen[len], true, memory_order_relaxed);
}
int nr_pdcch_dci_length_active_max(const nr_pdcch_dci_length_sweep_state_t *state,
                                   int min_len, int max_len)
{
  pthread_once(&length_env_once, length_env_init);
  /* Finish the active round before accepting new hints. */
  if (state->round_max && (state->resume_len || state->rot_phase))
    return state->round_max < max_len ? state->round_max : max_len;
  if (max_len <= 63 || min_len > 63 || explicit_max >= min_len || state->wide_range
      || state->relock_old_len)
    return max_len;
  for (int len = 64; len <= NR_DCI_MAX_PAYLOAD; ++len)
    if (atomic_load_explicit(&cell_seen[len], memory_order_relaxed)) return max_len;
  return 63;
}

int nr_pdcch_dci_length_order(int min_len, int max_len, int *out)
{
  if (!out || min_len < 1 || max_len > NR_DCI_MAX_PAYLOAD || min_len > max_len) return 0;
  pthread_once(&length_env_once, length_env_init);
  if (!seen_order_enabled) {
    for (int len = min_len; len <= max_len; ++len) out[len - min_len] = len;
    return max_len - min_len + 1;
  }
  int distance[NR_PDCCH_DCI_LENGTH_SWEEP_MAX_LEN];
  for (int len = min_len; len <= max_len; ++len) distance[len] = NR_DCI_MAX_PAYLOAD;
  for (int seen = 1; seen <= NR_DCI_MAX_PAYLOAD; ++seen) {
    if (!atomic_load_explicit(&cell_seen[seen], memory_order_relaxed)) continue;
    for (int len = min_len; len <= max_len; ++len) {
      int d = abs(len - seen);
      if (d < distance[len]) distance[len] = d;
    }
  }
  int n = 0;
  for (int len = min_len; len <= max_len; ++len) {
    int i = n++;
    while (i > 0 && distance[out[i - 1]] > distance[len]) {
      out[i] = out[i - 1];
      --i;
    }
    out[i] = len; // ascending tie break; no seen lengths preserves the cold order
  }
  return n;
}

int nr_pdcch_dci_length_context_relock_order(const nr_pdcch_dci_length_context_t *c,
    const int *seen, int n_seen, int *out, int max)
{
  if (!c || !out || max <= 0 || n_seen < 0 || (n_seen && !seen)) return 0;
  bool used[NR_PDCCH_DCI_LENGTH_SWEEP_MAX_LEN] = {0};
  int n = 0;
  const int old = c->found[0];
  if (old >= 30 && old <= NR_DCI_MAX_PAYLOAD) {
    out[n++] = old;
    used[old] = true;
  }
  for (int i = 0; i < n_seen && n < max; ++i) {
    const int len = seen[i];
    if (len >= 30 && len <= NR_DCI_MAX_PAYLOAD && !used[len]) {
      out[n++] = len;
      used[len] = true;
    }
  }
  for (int len = 30; len <= NR_DCI_MAX_PAYLOAD && n < max; ++len)
    if (!used[len]) out[n++] = len;
  return n;
}

uint32_t nr_pdcch_dci_length_n_suspect_from_env(void)
{
  const char *e = getenv("ISAC_RECONF_N_SUSPECT");
  if (!e || !*e || *e == '-') return 200;
  char *end = NULL;
  const unsigned long n = strtoul(e, &end, 10);
  return end != e && *end == '\0' && n > 0 && n <= UINT32_MAX ? (uint32_t)n : 200;
}

void nr_pdcch_dci_length_context_note_occasion(nr_pdcch_dci_length_context_t *c,
    bool accepted_at_locked, bool rnti_active_elsewhere, uint32_t n_suspect)
{
  if (!c || c->len_state != NR_LEN_LOCKED || c->found[0] <= 0) return;
  if (accepted_at_locked) {
    c->miss_occasions = 0;
    return;
  }
  if (!rnti_active_elsewhere) return;
  if (c->miss_occasions < UINT32_MAX) ++c->miss_occasions;
  if (c->miss_occasions < (n_suspect ? n_suspect : 200)) return;
  c->len_state = NR_LEN_SUSPECT;
  nr_pdcch_dci_length_sweep_reset(&c->state);
  c->state.preferred_len = c->found[0];
  c->state.relock_old_len = c->found[0];
  c->scout_initialized = false;
  c->exhausted = false;
}

int nr_pdcch_dci_length_context_lock(nr_pdcch_dci_length_context_t *c, int length)
{
  if (!c || length < 1 || length > NR_DCI_MAX_PAYLOAD) return -1;
  const int previous = c->len_state == NR_LEN_SUSPECT ? c->found[0] : 0;
  if (previous && previous != length) {
    memset(&c->layout_pin[0], 0, sizeof(c->layout_pin[0]));
    c->layout_cursor[0] = 0;
  }
  if (c->found[1] == length) {
    /* The retained secondary already owns this length's layout and recency. */
    c->layout_pin[0] = c->layout_pin[1];
    c->layout_cursor[0] = c->layout_cursor[1];
    c->found_recent[0] = c->found_recent[1];
    c->found[1] = 0;
    c->found_recent[1] = 0;
    memset(&c->layout_pin[1], 0, sizeof(c->layout_pin[1]));
    c->layout_cursor[1] = 0;
  }
  c->found[0] = length;
  c->len_state = NR_LEN_LOCKED;
  c->miss_occasions = 0;
  c->state.relock_old_len = 0;
  c->scout_initialized = false;
  return previous != length ? previous : 0;
}

int nr_pdcch_dci_length_context_add(nr_pdcch_dci_length_context_t *c, int length, uint32_t slot)
{
  if (!c || length < 1 || length > NR_DCI_MAX_PAYLOAD) return -1;
  for (int i = 0; i < 2; ++i)
    if (c->found[i] == length) {
      c->found_recent[i] = slot;
      return 0;
    }
  int i = !c->found[0] ? 0 : !c->found[1] ? 1
          : c->found_recent[0] <= c->found_recent[1] ? 0 : 1;
  const int replaced = c->found[i];
  c->found[i] = length;
  c->found_recent[i] = slot;
  memset(&c->layout_pin[i], 0, sizeof(c->layout_pin[i]));
  c->layout_cursor[i] = 0;
  c->len_state = NR_LEN_LOCKED;
  return replaced;
}

void nr_pdcch_dci_length_context_touch(nr_pdcch_dci_length_context_t *c, int length, uint32_t slot)
{
  if (!c) return;
  for (int i = 0; i < 2; ++i)
    if (c->found[i] == length) c->found_recent[i] = slot;
}

nr_dci11_pin_t *nr_pdcch_dci_length_context_pin(nr_pdcch_dci_length_context_t *c, int length)
{
  if (!c) return NULL;
  for (int i = 0; i < 2; ++i)
    if (length > 0 && c->found[i] == length) return &c->layout_pin[i];
  return NULL;
}

uint32_t *nr_pdcch_dci_length_context_pin_cursor(nr_pdcch_dci_length_context_t *c, int length)
{
  if (!c) return NULL;
  for (int i = 0; i < 2; ++i)
    if (length > 0 && c->found[i] == length) return &c->layout_cursor[i];
  return NULL;
}

void nr_pdcch_dci_length_sweep_reset(nr_pdcch_dci_length_sweep_state_t* state)
{
  memset(state, 0, sizeof(*state));
}

// Returns true iff h is new (and records it, space permitting) -- false if already seen.
static bool add_distinct_hash(uint32_t* hashes, int* n_distinct, uint32_t h)
{
  for (int i = 0; i < *n_distinct; i++) {
    if (hashes[i] == h) {
      return false;
    }
  }
  if (*n_distinct < NR_PDCCH_DCI_LENGTH_SWEEP_MAX_HASHES) {
    hashes[(*n_distinct)++] = h;
  }
  return true;
}

/* Record evidence for the (length,RNTI) pair. The bounded Misra-Gries table retains recurrent
 * identities under a stream of one-off false accepts without allocating per-RNTI state. */
static void add_rnti_evidence(nr_pdcch_dci_length_sweep_state_t *state, int len, uint16_t rnti,
                              uint32_t payload_hash, uint32_t feed_serial)
{
  if (rnti == 0 || len <= 0 || len >= NR_PDCCH_DCI_LENGTH_SWEEP_MAX_LEN)
    return;

  nr_pdcch_dci_length_rnti_evidence_t *empty = NULL;
  for (int i = 0; i < NR_PDCCH_DCI_LENGTH_SWEEP_RNTI_TRACKERS; ++i) {
    nr_pdcch_dci_length_rnti_evidence_t *e = &state->rnti_evidence[i];
    if (e->rnti == rnti && e->len == len) {
      /* Several CCEs from one occasion are correlated trials, not recurrence. */
      if (e->last_feed == feed_serial)
        return;
      e->last_feed = feed_serial;
      if (e->support < UINT16_MAX)
        ++e->support;
      for (int j = 0; j < e->n_distinct; ++j)
        if (e->hashes[j] == payload_hash)
          return;
      if (e->n_distinct < NR_PDCCH_DCI_LENGTH_SWEEP_RNTI_HASHES)
        e->hashes[e->n_distinct++] = payload_hash;
      if (e->n_distinct > state->max_rnti_distinct[len])
        state->max_rnti_distinct[len] = e->n_distinct;
      return;
    }
    if (e->rnti == 0 && empty == NULL)
      empty = e;
  }

  if (empty == NULL) {
    /* Standard heavy-hitter cancellation: one-off identities disappear, while a recurrent RNTI's
     * accumulated support protects its distinct-payload history. The new one-off is discarded. */
    for (int i = 0; i < NR_PDCCH_DCI_LENGTH_SWEEP_RNTI_TRACKERS; ++i) {
      nr_pdcch_dci_length_rnti_evidence_t *e = &state->rnti_evidence[i];
      if (e->support > 0 && --e->support == 0)
        memset(e, 0, sizeof(*e));
    }
    return;
  }

  empty->rnti = rnti;
  empty->len = (uint8_t)len;
  empty->n_distinct = 1;
  empty->support = 1;
  empty->last_feed = feed_serial;
  empty->hashes[0] = payload_hash;
  if (state->max_rnti_distinct[len] < 1)
    state->max_rnti_distinct[len] = 1;
}

// Does this length's ACCUMULATED (trials, passes) clear the chance floor by Z_SIGMA standard
// deviations? Scales with trials -- see this file's header comment on why a fixed floor cannot be
// reused here.
/* Minimum ACCUMULATED trials before the statistical path may lock (the bootstrap path is exempt --
 * two independent hits on a known 16-bit value are ~2^-48 by chance whatever the sample size).
 * MEASURED 2026-09-19 on the Salt macro: length 42 locked from occasions_fed=1 (~56 trials) with no
 * bootstrap RNTI, which cannot be significant, and a false lock STICKS (g_length_found gates the
 * sweep off), so the rest of that CORESET walk ran with a wrong length. */
#define MIN_TRIALS_FOR_STATISTICAL_LOCK 256

/* Does this length's ACCUMULATED (trials, passes) clear the noise floor by Z_SIGMA sd?
 *
 * `null_rate` is MEASURED from the length population rather than assumed: at most one of the ~34
 * lengths under test can be the real one, so the median pass rate across the others IS this
 * hypothesis's own noise rate -- and it captures what CHANCE_PASS_RATE cannot, namely STRUCTURED
 * false accepts (a real DCI decoded at the WRONG length passes far more often than noise, which is
 * what produced the measured false lock above; at the assumed 1/256 the same event is ~1e-6). It is
 * floored at CHANCE_PASS_RATE so this can never be laxer than the fixed rate it replaces. */
static bool clears_chance_floor(int trials, int passes, double null_rate)
{
  if (trials < MIN_TRIALS_FOR_STATISTICAL_LOCK) {
    return false;
  }
  const double p    = (null_rate > CHANCE_PASS_RATE) ? null_rate : CHANCE_PASS_RATE;
  const double mean = (double)trials * p;
  const double var  = (double)trials * p * (1.0 - p);
  const double sd   = sqrt(var);
  return (double)passes >= mean + Z_SIGMA * sd + 1.0; // +1: never accept on a razor-thin margin
}

/* Median pass RATE over every length except `skip_len` -- the measured noise null above. */
static double measured_null_rate(const nr_pdcch_dci_length_sweep_state_t *state, int min_len, int max_len,
                                 int skip_len)
{
  double r[NR_PDCCH_DCI_LENGTH_SWEEP_MAX_LEN];
  int n = 0;
  for (int len = min_len; len <= max_len && len < NR_PDCCH_DCI_LENGTH_SWEEP_MAX_LEN; len++) {
    if (len == skip_len || len == state->excluded_len || len == state->tertiary_excluded_len || state->trials[len] <= 0)
      continue;
    r[n++] = (double)state->passes[len] / (double)state->trials[len];
  }
  if (n == 0)
    return 0.0;
  for (int i = 1; i < n; i++) { /* insertion sort: n <= 41 */
    const double v = r[i];
    int j = i - 1;
    while (j >= 0 && r[j] > v) { r[j + 1] = r[j]; j--; }
    r[j + 1] = v;
  }
  return (n & 1) ? r[n / 2] : 0.5 * (r[n / 2 - 1] + r[n / 2]);
}

int nr_pdcch_dci_length_sweep_feed_budget(nr_pdcch_dci_length_sweep_state_t* state,
                                   nr_pdcch_dci_length_scorer_fn decode_one_candidate, void* user_ctx,
                                   int n_trials_this_call, int min_len, int max_len,
                                   uint16_t bootstrap_rnti, uint64_t deadline_ns, int max_trials)
{
  if (state == NULL || decode_one_candidate == NULL || n_trials_this_call <= 0) {
    return -1;
  }
  if (min_len < 1) min_len = 1;
  if (max_len > NR_DCI_MAX_PAYLOAD) max_len = NR_DCI_MAX_PAYLOAD;
  if (min_len > max_len) return -1;
  max_len = nr_pdcch_dci_length_active_max(state, min_len, max_len);
  state->round_max = max_len;
  bool full_round_completed = false;
  /* Rotation (see the header's `stride`): test every stride'th length, phase-shifted per call, so
   * each length is visited exactly once per stride calls -- fair by construction, no length can be
   * starved, and the phase survives the caller restarting the sweep on a new CORESET hypothesis
   * only in the sense that a reset returns it to 0, which is a complete round boundary anyway. */
  int stride = (state->stride > 1) ? state->stride : 1;
  const int n_lengths = max_len - min_len + 1;
  if (stride > n_lengths) {
    stride = (n_lengths > 0) ? n_lengths : 1;
  }
  if (state->rot_phase >= stride) {
    state->rot_phase = 0; // stride shrank under us; restart the round rather than skip lengths
  }
  /* CELL PRIOR: test ONLY the seeded length while its budget lasts. The significance test below is
   * unchanged -- this restricts which lengths get new TRIALS, not what counts as evidence -- so a
   * seeded length still has to earn its win. Two distinct RNTIs already converged on it through
   * independent full sweeps, which is why testing it alone is not weaker evidence than one UE's
   * 34-length statistics; it is the same test applied to a hypothesis with cross-UE support. */
  int prefer = 0;
  if (state->preferred_len >= min_len && state->preferred_len <= max_len
      && state->preferred_len < NR_PDCCH_DCI_LENGTH_SWEEP_MAX_LEN
      && state->preferred_len != state->excluded_len
      && state->preferred_len != state->secondary_excluded_len
      && state->preferred_len != state->tertiary_excluded_len) {
    if (state->preferred_rounds < NR_PDCCH_LENGTH_PREFERRED_ROUNDS) {
      prefer = state->preferred_len;
    } else if (state->preferred_len && !state->alt_full) {
      /* Alternate instead of dropping: one full lap, then the hypothesis again. Dropping it for good made a
       * low-rate UE fall into the full sweep and exhaust before its first two hits arrived. */
      state->alt_full = 1;
    }
  } else {
    state->preferred_len = 0;
  }
  int completed=0;
  uint32_t feed_serial = ++state->feed_serial;
  if (feed_serial == 0)
    feed_serial = ++state->feed_serial;
  /* Freeze the order for a complete rotation and any budget suspensions. New cell
   * hints take effect next round, so a concurrent lock cannot skip/repeat a length. */
  if (!state->resume_len && state->rot_phase == 0) {
    if (state->relock_old_len) {
      nr_pdcch_dci_length_context_t hint = {.found = {state->relock_old_len}};
      int seen[NR_DCI_MAX_PAYLOAD], n_seen = 0;
      for (int len = min_len; len <= max_len; ++len)
        if (atomic_load_explicit(&cell_seen[len], memory_order_relaxed)) seen[n_seen++] = len;
      state->order_count = nr_pdcch_dci_length_context_relock_order(
          &hint, seen, n_seen, state->order, NR_PDCCH_DCI_LENGTH_SWEEP_MAX_LEN);
    } else
      state->order_count = nr_pdcch_dci_length_order(min_len, max_len, state->order);
  }
  int initial_index = state->rot_phase;
  if (state->resume_len)
    for (int i = 0; i < state->order_count; ++i)
      if (state->order[i] == state->resume_len) initial_index = i;
  for (int index = initial_index; prefer || index < state->order_count; index += stride) {
    const int len = prefer ? prefer : state->order[index];
    if (len < min_len || len > max_len) continue;
    if (len == state->tertiary_excluded_len || (state->secondary_excluded_len
        && (len == state->excluded_len || len == state->secondary_excluded_len))) continue;
    const int initial_trial=(state->resume_len==len) ? state->resume_trial : 0;
    for (int t = initial_trial; t < n_trials_this_call; t++) {
      bool stop=max_trials>0 && completed>=max_trials;
      if (deadline_ns) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC,&now);
        stop |= (uint64_t)now.tv_sec*1000000000ull+(uint64_t)now.tv_nsec >= deadline_ns;
      }
      if (stop) { state->resume_len=len; state->resume_trial=t; goto score_evidence; }
      ++completed;
      uint16_t rnti = 0;
      uint32_t payload_hash = 0;
      state->trials[len]++;
      state->decodes++;
      if (!decode_one_candidate(len, t, &rnti, &payload_hash, user_ctx)) {
        continue;
      }
      state->passes[len]++;
      if (bootstrap_rnti != 0 && rnti == bootstrap_rnti) {
        state->bootstrap_hits[len]++;
      }
      add_distinct_hash(state->hashes[len], &state->n_distinct[len], payload_hash);
      add_rnti_evidence(state, len, rnti, payload_hash, feed_serial);
    }
    if (prefer) break;
  }
  state->resume_len=state->resume_trial=0;
  /* One ROUND -- every length visited once -- is what the caller's give-up cap counts, so the
   * per-length trial budget is identical at any stride; rotation redistributes it in time. */
  if (prefer) {
    state->preferred_rounds++;
    state->rot_phase = 0;
    state->occasions_fed++; // one length is the whole round when a prior is seeded
  } else if (++state->rot_phase >= stride) {
    full_round_completed = true;
    state->rot_phase = 0;
    state->occasions_fed++;
    if (state->alt_full) { /* full lap done: back to the hypothesis */
      state->alt_full = 0;
      state->preferred_rounds = 0;
    }
  }

score_evidence:;
  int    best_len   = -1;
  double best_score = 0.0;
  for (int len = min_len; len <= max_len && len < NR_PDCCH_DCI_LENGTH_SWEEP_MAX_LEN; len++) {
    if (state->passes[len] == 0 || len == state->excluded_len
        || len == state->secondary_excluded_len || len == state->tertiary_excluded_len) {
      continue;
    }
    // Degenerate fixed point: repeated passes, but every one decodes to the SAME payload. Reject
    // outright regardless of bootstrap hits (this file's header: trap #2).
    if (state->n_distinct[len] <= 1 && state->passes[len] >= 3) {
      continue;
    }
    // Significance: either it has landed the KNOWN bootstrap RNTI at least once (matching a
    // specific 16-bit value by chance is ~1/65536 per trial -- far below anything needed here,
    // and unaffected by accumulated sample size), or -- with no bootstrap available, or not yet on
    // this length -- its accumulated passes must clear the sample-size-scaled chance floor.
    /* Two bootstrap hits, not one: a single chance match is 2^-24 per trial but a hypothesis runs
     * ~28k trials per length and a full CORESET walk ~29k length-hypotheses, so single hits are
     * expected several times per walk; two are not. */
    const bool recurrent_rnti =
        state->max_rnti_distinct[len] >= NR_PDCCH_DCI_LENGTH_SWEEP_RNTI_LOCK;
    const bool significant = recurrent_rnti || (state->bootstrap_hits[len] > 1)
                           || clears_chance_floor(state->trials[len], state->passes[len],
                                                  measured_null_rate(state, min_len, max_len, len));
    if (!significant) {
      continue;
    }
    const double score = (double)state->max_rnti_distinct[len] * 10000.0
                       + (double)state->bootstrap_hits[len] * 100.0
                       + (double)state->n_distinct[len];
    if (score > best_score) {
      best_score = score;
      best_len   = len;
    }
  }

  /* DIAGNOSTIC (2026-09-06, env-gated): the accumulate-across-occasions rewrite still never
   * converges even with confirmed real DL traffic present -- need to see whether length 47 (this
   * deployment's known-correct value) is accumulating ANY real passes at all, or whether the
   * candidates reaching this function never include it (a wiring/indexing issue upstream) vs.
   * genuinely never passing decode+plausibility at 47 specifically (a DSP/parameter issue). */
  if (getenv("ISAC_DISCOVER_DIAG") != NULL) {
    static int s_feed_calls = 0;
    s_feed_calls++;
    if ((s_feed_calls % 50) == 1 || best_len > 0) {
      int max_len_seen = -1, max_passes = -1, max_trials = 0;
      for (int len = min_len; len <= max_len && len < NR_PDCCH_DCI_LENGTH_SWEEP_MAX_LEN; len++) {
        if (state->passes[len] > max_passes) {
          max_passes = state->passes[len];
          max_len_seen = len;
          max_trials = state->trials[len];
        }
      }
      const int len47 = (47 >= min_len && 47 <= max_len && 47 < NR_PDCCH_DCI_LENGTH_SWEEP_MAX_LEN) ? 47 : -1;
      printf("SWEEPDIAG feed=%d occ=%d best_len=%d max_len=%d max_passes=%d/%d len47_trials=%d "
            "len47_passes=%d len47_distinct=%d len47_bshits=%d\n",
            s_feed_calls, state->occasions_fed, best_len, max_len_seen, max_passes, max_trials,
            len47 > 0 ? state->trials[47] : -1, len47 > 0 ? state->passes[47] : -1,
            len47 > 0 ? state->n_distinct[47] : -1, len47 > 0 ? state->bootstrap_hits[47] : -1);
      fflush(stdout);
    }
  }
  if (full_round_completed && best_len < 0 && min_len <= 63) {
    bool stage_one_exhausted = true;
    for (int len = min_len; len <= 63 && len <= max_len; ++len)
      if (state->trials[len] < MIN_TRIALS_FOR_STATISTICAL_LOCK) {
        stage_one_exhausted = false;
        break;
      }
    if (stage_one_exhausted) state->wide_range = true;
  }
  nr_pdcch_dci_length_note_seen(best_len);
  return best_len;
}

int nr_pdcch_dci_length_sweep_feed(nr_pdcch_dci_length_sweep_state_t *state,
    nr_pdcch_dci_length_scorer_fn scorer, void *ctx, int n_trials, int min_len, int max_len,
    uint16_t bootstrap_rnti)
{
  return nr_pdcch_dci_length_sweep_feed_budget(state,scorer,ctx,n_trials,min_len,max_len,bootstrap_rnti,0,0);
}

uint16_t nr_pdcch_dci_length_sweep_winner_rnti(
    const nr_pdcch_dci_length_sweep_state_t *state, int len)
{
  if (!state || len <= 0 || len >= NR_PDCCH_DCI_LENGTH_SWEEP_MAX_LEN)
    return 0;
  const nr_pdcch_dci_length_rnti_evidence_t *best = NULL;
  for (int i = 0; i < NR_PDCCH_DCI_LENGTH_SWEEP_RNTI_TRACKERS; ++i) {
    const nr_pdcch_dci_length_rnti_evidence_t *e = &state->rnti_evidence[i];
    if (e->rnti == 0 || e->len != len
        || e->n_distinct < NR_PDCCH_DCI_LENGTH_SWEEP_RNTI_LOCK)
      continue;
    if (!best || e->n_distinct > best->n_distinct
        || (e->n_distinct == best->n_distinct && e->support > best->support))
      best = e;
  }
  return best ? best->rnti : 0;
}

nr_pdcch_dci_length_bank_t *nr_pdcch_dci_length_store_get(
    nr_pdcch_dci_length_store_t *store, uint64_t key, uint64_t *evicted_key)
{
  if (!store || !key)
    return NULL;
  nr_pdcch_dci_length_coreset_t *victim = &store->coreset[0];
  for (int i = 0; i < NR_PDCCH_LENGTH_CORESETS; ++i) {
    nr_pdcch_dci_length_coreset_t *e = &store->coreset[i];
    if (e->used && e->key == key) {
      e->touched = ++store->clock;
      if (evicted_key)
        *evicted_key = 0;
      return e->bank;
    }
    if (!e->used || (victim->used && e->touched < victim->touched))
      victim = e;
  }
  if (evicted_key)
    *evicted_key = victim->used ? victim->key : 0;
  nr_pdcch_dci_length_bank_t *bank = victim->bank;
  if (bank == NULL) {
    bank = calloc(1, sizeof(*bank));
    if (bank == NULL)
      return NULL;
  } else {
    memset(bank, 0, sizeof(*bank));
  }
  memset(victim, 0, sizeof(*victim));
  victim->bank = bank;
  victim->used = true;
  victim->key = key;
  victim->touched = ++store->clock;
  bank->epoch = key;
  return bank;
}

nr_pdcch_dci_length_context_t *nr_pdcch_dci_length_context(
    nr_pdcch_dci_length_bank_t *bank, uint64_t epoch, uint16_t rnti)
{
  if (!bank || !rnti) return NULL;
  if (bank->epoch != epoch) {
    memset(bank, 0, sizeof(*bank));
    bank->epoch = epoch;
  }
  nr_pdcch_dci_length_context_t *oldest=&bank->ue[0];
  for (int i=0; i<NR_PDCCH_LENGTH_CONTEXTS; ++i) {
    nr_pdcch_dci_length_context_t *c=&bank->ue[i];
    if (c->rnti == rnti) {
      c->touched=++bank->clock;
      return c;
    }
    if (c->touched < oldest->touched) oldest=c;
  }
  memset(oldest, 0, sizeof(*oldest));
  oldest->rnti=rnti;
  oldest->touched=++bank->clock;
  if (bank->cell_len > 0) {
    oldest->state.preferred_len = bank->cell_len;
    LOG_I(PHY, "SENSING: UL dci_length -- rnti=0x%04x seeded from the cell prior %d (skipping the "
               "34-length sweep unless it fails to clear)\n", rnti, bank->cell_len);
  } else if (bank->first_len > 0 && bank->first_rnti != rnti) {
    /* One UE's width is only a hypothesis for another UE. preferred_len preserves that distinction:
     * this RNTI must still produce its own recurrent CRC/payload evidence, and automatically falls
     * back to the full sweep if the hypothesis is wrong. */
    oldest->state.preferred_len = bank->first_len;
  }
  return oldest;
}

void nr_pdcch_dci_length_bank_converged(nr_pdcch_dci_length_bank_t *bank, uint16_t rnti, int found)
{
  nr_pdcch_dci_length_note_seen(found);
  if (!bank || !rnti || found <= 0 || bank->cell_len > 0)
    return;
  if (bank->first_rnti == 0 || bank->first_rnti == rnti) {
    /* First converged RNTI stays PRIVATE: one UE's dedicated config is not evidence about the cell
     * (a UE's DCI 0_1 width depends on its own configured features, not only on the BWP). */
    bank->first_rnti = rnti;
    bank->first_len  = found;
    /* Existing contexts may predate the first convergence. Give each unresolved peer the same
     * bounded hypothesis fast path that a newly-created context receives. Completed evidence is
     * retained; only an unfinished budget cursor is restarted. */
    for (int i = 0; i < NR_PDCCH_LENGTH_CONTEXTS; ++i) {
      nr_pdcch_dci_length_context_t *c = &bank->ue[i];
      if (c->rnti && c->rnti != rnti && !c->found[0] && !c->exhausted && !c->state.preferred_len) {
        c->state.preferred_len = found;
        c->state.preferred_rounds = 0;
        c->state.resume_len = c->state.resume_trial = 0;
      }
    }
    return;
  }
  if (bank->first_len == found) {
    bank->cell_len = found;
    LOG_W(PHY, "SENSING: UL dci_length cell prior PUBLISHED = %d, by agreement rnti=0x%04x + "
               "rnti=0x%04x -- later RNTIs skip the sweep\n", found, bank->first_rnti, rnti);
  } else {
    /* Two UEs genuinely differ: there is no cell-wide answer. Keep the newer as the candidate so a
     * third UE can still agree with one of them, but never publish a value two UEs contradict. */
    LOG_W(PHY, "SENSING: UL dci_length DISAGREEMENT rnti=0x%04x says %d, rnti=0x%04x says %d -- no "
               "cell prior published\n", bank->first_rnti, bank->first_len, rnti, found);
    bank->first_rnti = rnti;
    bank->first_len  = found;
  }
}

bool nr_pdcch_dci_length_scout_due(nr_pdcch_dci_length_bank_t *bank)
{
  return (++bank->scout_occasions % 20) == 0;
}
