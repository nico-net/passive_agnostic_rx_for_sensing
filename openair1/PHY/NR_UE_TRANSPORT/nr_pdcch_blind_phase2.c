/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_phase2.c
 * \brief See nr_pdcch_blind_phase2.h (Task A7: N blind-PDCCH scan consumers).
 */
#include "nr_pdcch_blind_phase2.h"
#include <pthread.h>
#include <stdatomic.h>

/* ---- Phase-2 lock (Task A7 design point 2) -------------------------------------------------------
 * One occasion = Phase 1 (FEP, LLR, demap, pre-pass, candidate decode: the ~80 % of the cost) and
 * Phase 2 (everything after the decode join: accepts, dci_thres EMA, RNTI persistence, AL census,
 * CFR/PDSCH submission, the periodic summary). Phase 1 runs unlocked and in parallel on every scan
 * consumer; Phase 2 runs under this one lock, so its sequential semantics are exactly those of the
 * single-consumer build. Exception, ruled in Task A7: an occasion of the AUTODISCOVER pass (root cfg,
 * autodiscover=1) takes this lock for the whole occasion, because its Phase 1 drives the discovery state
 * machine (nr_pdcch_blind_monitor.c), which rewrites the very cfg that occasion reads. Verified bank
 * passes, the CORESET#0-USS pass and the CSS0 snapshot carry autodiscover=0 and run Phase 1 unlocked.
 *
 * ORDERING (design point 3). With N consumers Phase 2 of occasion k may run before Phase 2 of
 * occasion k-1. This is harmless for the state it guards:
 *   - RNTI persistence counts sightings whose abs_slot lies within window_slots of the current one
 *     (abs_slot - seen.abs_slot, unsigned). An out-of-order sighting from an EARLIER slot is counted
 *     like an in-order one; one from a LATER slot wraps to a huge difference and is not counted --
 *     i.e. at worst one fewer sighting for the occasion that lost the race, which the next sighting
 *     repays. Consumers race by at most the queue depth (8-16 slots) against windows of hundreds.
 *   - The dci_thres EMA and the energy-floor estimator are order-insensitive at the 1e-2 level (both
 *     are averages over many samples, not functions of their order).
 * IN-LINE PDSCH: with the PDSCH decode deferral off (no nr_pdsch_passive_queue), Phase 2 decodes PDSCH in
 * line and so holds this lock across the LDPC decode; N > 1 scan consumers then serialise on it. The N > 1
 * speed-up assumes deferred PDSCH (nr_pdcch_blind_monitor_rt.c warns at scan-pool start).
 * Lock order: this lock is taken by the occasion body only, never while holding another blind-PDCCH
 * lock, and the body takes its leaf locks (dedupe, length sweep, bank, ...) inside it. */
static pthread_mutex_t g_phase2_mu = PTHREAD_MUTEX_INITIALIZER;

void nr_pdcch_blind_phase2_lock(void)
{
  pthread_mutex_lock(&g_phase2_mu);
}

void nr_pdcch_blind_phase2_unlock(void)
{
  pthread_mutex_unlock(&g_phase2_mu);
}

// ---- RNTI persistence tracking (2026-07-28): a real UE's RNTI recurs across many grants; a noise
// accept is a one-off. Small ring buffer of recent (rnti, abs_slot) sightings -- linear scan is fine
// given the raw accept rate is on the order of ~1/s (measured), so the buffer holds at most a few
// seconds of history regardless of window size. See nr_pdcch_blind_monitor_rt.h's rnti_persist_k/
// rnti_persist_window_ms field comments. Guarded by g_phase2_mu (held by the caller). ----
#define NR_PDCCH_BLIND_PERSIST_MAX 64
static struct {
  uint16_t rnti;
  uint32_t abs_slot;
} g_recent[NR_PDCCH_BLIND_PERSIST_MAX];
static int g_recent_head = 0;
static int g_recent_count = 0;
static uint64_t g_recent_sightings = 0; /* sightings ever recorded; same lock */

// Always records the current sighting regardless of the outcome, so a candidate that fails today can
// contribute toward tomorrow's threshold.
bool nr_pdcch_blind_rnti_persistence_check(uint16_t rnti, uint32_t abs_slot, uint32_t window_slots, int min_k)
{
  if (min_k <= 1) {
    return true; // gate disabled -- accept-on-first-sighting, matches pre-2026-07-28 behaviour
  }
  int seen = 0;
  for (int i = 0; i < g_recent_count; i++) {
    if (g_recent[i].rnti == rnti && (abs_slot - g_recent[i].abs_slot) <= window_slots) {
      seen++;
    }
  }
  g_recent_sightings++;
  g_recent[g_recent_head].rnti = rnti;
  g_recent[g_recent_head].abs_slot = abs_slot;
  g_recent_head = (g_recent_head + 1) % NR_PDCCH_BLIND_PERSIST_MAX;
  if (g_recent_count < NR_PDCCH_BLIND_PERSIST_MAX) {
    g_recent_count++;
  }
  return (seen + 1) >= min_k; // +1 counts the sighting just recorded
}

bool nr_pdcch_blind_dl_accept_gate(int *dci_thres, int mismatched_bits, uint16_t rnti, uint32_t abs_slot,
                                   uint32_t window_slots, int min_k)
{
  *dci_thres = (*dci_thres + mismatched_bits) / 2;
  return !(mismatched_bits > *dci_thres + 30)
         && nr_pdcch_blind_rnti_persistence_check(rnti, abs_slot, window_slots, min_k);
}

uint64_t nr_pdcch_blind_persistence_sightings(void)
{
  nr_pdcch_blind_phase2_lock();
  const uint64_t n = g_recent_sightings;
  nr_pdcch_blind_phase2_unlock();
  return n;
}

// ---- Adaptive energy floor (cfg->energy_adapt_factor). See the long rationale at its use in
// nr_pdcch_blind_monitor_rt.c (median, frugal streaming update, relative step). Fed per candidate
// from the PRE-PASS, i.e. Phase 1, which is NOT under g_phase2_mu: its own leaf lock. A per-candidate
// lock/unlock (tens of ns, ~100 candidates per occasion) keeps the exact per-sample update sequence. ----
#define ENERGY_FLOOR_STEP 0.01f // fractional step per candidate toward the running median
#define ENERGY_FLOOR_MIN 1e-6f  // keep strictly positive: the threshold is multiplicative
static pthread_mutex_t g_energy_mu = PTHREAD_MUTEX_INITIALIZER;
static float g_energy_floor = 0.0f;
static uint64_t g_energy_nseen = 0;

float nr_pdcch_blind_energy_floor_update(float x, uint64_t *nseen_out)
{
  pthread_mutex_lock(&g_energy_mu);
  g_energy_nseen++;
  if (g_energy_floor <= 0.0f) {
    // Seed on the first sample rather than from 0, so the relative step has something to scale.
    g_energy_floor = (x > ENERGY_FLOOR_MIN) ? x : ENERGY_FLOOR_MIN;
  } else {
    const float step = g_energy_floor * ENERGY_FLOOR_STEP;
    g_energy_floor += (x > g_energy_floor) ? step : -step;
    if (g_energy_floor < ENERGY_FLOOR_MIN)
      g_energy_floor = ENERGY_FLOOR_MIN;
  }
  const float f = g_energy_floor;
  if (nseen_out)
    *nseen_out = g_energy_nseen;
  pthread_mutex_unlock(&g_energy_mu);
  return f;
}

float nr_pdcch_blind_energy_floor_get(uint64_t *nseen_out)
{
  pthread_mutex_lock(&g_energy_mu);
  const float f = g_energy_floor;
  if (nseen_out)
    *nseen_out = g_energy_nseen;
  pthread_mutex_unlock(&g_energy_mu);
  return f;
}

/* ---- Test hooks ---- */
void nr_pdcch_blind_phase2_reset_for_test(void)
{
  nr_pdcch_blind_phase2_lock();
  g_recent_head = g_recent_count = 0;
  g_recent_sightings = 0;
  nr_pdcch_blind_phase2_unlock();
  pthread_mutex_lock(&g_energy_mu);
  g_energy_floor = 0.0f;
  g_energy_nseen = 0;
  pthread_mutex_unlock(&g_energy_mu);
}

int nr_pdcch_blind_persistence_count_for_test(uint16_t rnti, int *total)
{
  nr_pdcch_blind_phase2_lock();
  int n = 0;
  for (int i = 0; i < g_recent_count; i++)
    n += g_recent[i].rnti == rnti;
  if (total)
    *total = g_recent_count;
  nr_pdcch_blind_phase2_unlock();
  return n;
}
