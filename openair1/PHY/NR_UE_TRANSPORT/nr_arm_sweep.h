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

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_arm_sweep.h
 * \brief Minimal per-RNTI Wilson-interval arm-selection + latch, factored out of the DCI 1_1
 * interleaved VRB-to-PRB bundle-size sweep (full-running-agnosticity Task 11) so a THIRD sweep of
 * the same shape -- PRB-bundling/PRG, Task 12 -- does not re-derive it a third time.
 *
 * A passive receiver cannot see several RRC-configured fields (vrb-ToPRB-Interleaver,
 * PRB-bundling-type/bundleSize, ...) that change how it must demodulate a grant, and the only
 * ground truth available is whether the resulting TB CRC passes. Each such field becomes a tiny
 * per-RNTI multi-armed-bandit: try an arm, feed back the CRC outcome, and once one arm's Wilson
 * confidence interval clears every other arm's, latch it -- from then on this costs one field read,
 * not a repeated trial.
 *
 * Deliberately NOT nr_hyp_sweep (nr_pdcch_ul_discovery.c): that state is ~1.16 MB per instance
 * (measured from its own two users), because it explores a much larger joint DCI-field space. A
 * per-RNTI bundle-size/PRG hypothesis is 2-3 fixed small arms, so RNTI_DEC_MAX (16) contexts of this
 * cost nothing worth heap-allocating -- exactly the reasoning nr_pdsch_passive_decode.c's original
 * VRB-L comment already gave; this header just stops it being repeated per sweep.
 *
 * Header-only (static inline, no PHY/UE dependencies) so it can be unit-tested standalone -- see
 * tests/nr_arm_sweep_test.cc -- without dragging in nr_pdsch_passive_decode.c's own dependencies.
 */

#ifndef NR_ARM_SWEEP_H
#define NR_ARM_SWEEP_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Largest arm count any current sweep needs (VRB-L: 2, PRG: 3). Bump if a future sweep needs more
/// -- the arrays are tiny (2 x uint32_t x N), so headroom costs nothing.
#define NR_ARM_SWEEP_MAX 3
/// Successes an arm must reach before it may latch. Same value the VRB-L/PT-RS sweeps already used.
#define NR_ARM_SWEEP_LATCH_MIN_OK 8

typedef struct {
  uint32_t tr[NR_ARM_SWEEP_MAX]; ///< trials per arm
  uint32_t ok[NR_ARM_SWEEP_MAX]; ///< successes (TB CRC pass) per arm
  int latched;                   ///< -1 = not yet decided, else the latched arm index
} nr_arm_sweep_t;

/// Wilson score interval for `ok` successes out of `n` trials at z=1.96 (~95%). n == 0 -> [0, 1],
/// i.e. an untried arm is maximally uncertain in both directions.
static inline void nr_arm_sweep_wilson(uint32_t ok, uint32_t n, double *lo, double *hi)
{
  if (n == 0) {
    *lo = 0.0;
    *hi = 1.0;
    return;
  }
  const double z = 1.96, nn = (double)n, p = (double)ok / nn, d = 1.0 + z * z / nn;
  const double c = p + z * z / (2.0 * nn), q = z * sqrt(p * (1.0 - p) / nn + z * z / (4.0 * nn * nn));
  *lo = (c - q) / d;
  *hi = (c + q) / d;
  if (*lo < 0.0)
    *lo = 0.0;
  if (*hi > 1.0)
    *hi = 1.0;
}

/// Which arm to try next (or the latched arm once settled). `n_arms` <= NR_ARM_SWEEP_MAX.
static inline int nr_arm_sweep_pick(const nr_arm_sweep_t *s, int n_arms)
{
  if (s->latched >= 0)
    return s->latched;
  int lead = -1;
  double lead_p = -1.0;
  for (int a = 0; a < n_arms; a++)
    if (s->tr[a] > 0 && (double)s->ok[a] / (double)s->tr[a] > lead_p) {
      lead_p = (double)s->ok[a] / (double)s->tr[a];
      lead = a;
    }
  if (lead >= 0 && s->ok[lead] >= NR_ARM_SWEEP_LATCH_MIN_OK) {
    double llo, lhi;
    nr_arm_sweep_wilson(s->ok[lead], s->tr[lead], &llo, &lhi);
    for (int a = 0; a < n_arms; a++) {
      if (a == lead)
        continue;
      double lo, hi;
      nr_arm_sweep_wilson(s->ok[a], s->tr[a], &lo, &hi);
      if (hi >= llo)
        return a; /* still contending: spend a trial ruling it out */
    }
    return lead;
  }
  int arg = 0;
  double best = -1.0;
  for (int a = 0; a < n_arms; a++) {
    double lo, hi;
    nr_arm_sweep_wilson(s->ok[a], s->tr[a], &lo, &hi);
    if (hi > best + 1e-12 || (fabs(hi - best) <= 1e-12 && s->tr[a] < s->tr[arg])) {
      best = hi;
      arg = a;
    }
  }
  return arg;
}

/// Feed one trial's outcome back. Returns the newly-latched arm (>= 0) the call that settles it,
/// -1 while still unsettled, or the already-latched arm (a no-op) if called again after latching.
static inline int nr_arm_sweep_feed(nr_arm_sweep_t *s, int n_arms, int arm, bool tb_ok)
{
  if (arm < 0 || arm >= n_arms || s->latched >= 0)
    return s->latched;
  s->tr[arm]++;
  if (tb_ok)
    s->ok[arm]++;
  if (s->ok[arm] < NR_ARM_SWEEP_LATCH_MIN_OK)
    return -1;
  double lo, hi;
  nr_arm_sweep_wilson(s->ok[arm], s->tr[arm], &lo, &hi);
  for (int a = 0; a < n_arms; a++) {
    if (a == arm)
      continue;
    double lo2, hi2;
    nr_arm_sweep_wilson(s->ok[a], s->tr[a], &lo2, &hi2);
    if (hi2 >= lo)
      return -1;
  }
  s->latched = arm;
  return arm;
}

/* ---- EVIDENCE-TRIGGERED variant: arm 0 is the INCUMBENT (today's decode path) --------------------
 * For a sweep whose non-zero arms are much more expensive than arm 0 and change a path that works on
 * every cell seen so far (PRG: arms 1/2 take the segmented chest, ~100x the cost, and skip the
 * per-grant CFO/SFO/chest-cache/PT-RS/GPU steps), exploring from the first grant is wrong: arm 0 is
 * used EXCLUSIVELY until it has shown a sustained CRC deficit while the link is demonstrably healthy
 * (other decodes passing), and only then are the other arms explored.
 *
 * Trigger: NR_ARM_SWEEP_INCUMBENT_MIN_TRIALS consecutive arm-0 trials fed with link_ok, of which at most
 * NR_ARM_SWEEP_INCUMBENT_POOR_RATE passed. A window that is NOT poor is discarded and a new one starts,
 * so old evidence and link outages (link_ok false -> not counted) never add up to a trigger.
 *   N = 32, rate 0.25: a healthy wideband decode (true rate >= 0.5) reads <= 8/32 with probability
 *   P(Bin(32, 0.5) <= 8) = 3.5e-3 per window, while a precoder-switching (PRG) mismatch collapses wide
 *   grants to ~0 % (OTA 2026-09-12: 0/10000 full-band) and trips in the first window.
 * Explore: Wilson-upper-bound pick over all arms, ties broken TOWARDS ARM 0 (not towards the fewest
 * trials). Latch: the usual separation rule (nr_arm_sweep_feed), or, once n_arms x
 * NR_ARM_SWEEP_EXPLORE_MAX_TRIALS trials have been spent exploring with no arm separated, arm 0 -- a tie
 * is not evidence for the costlier path, and without this a cell where all arms decode alike never
 * latches and keeps paying. The budget counts exploration trials in TOTAL, not per arm: the plain
 * pick can keep re-trying one contender, so a per-arm floor might never be reached. */
#define NR_ARM_SWEEP_INCUMBENT_MIN_TRIALS 32
#define NR_ARM_SWEEP_INCUMBENT_POOR_RATE 0.25
#define NR_ARM_SWEEP_EXPLORE_MAX_TRIALS 128

typedef struct {
  nr_arm_sweep_t s;   ///< every trial, all arms (s.latched is the decision)
  uint32_t win_tr;    ///< arm-0 trials with the link healthy in the current trigger window
  uint32_t win_ok;    ///< ... of which passed
  bool explore;       ///< arm 0 showed a sustained deficit on a healthy link: arms 1.. are now tried
  uint32_t explore_tr; ///< trials fed while exploring (bounded by n_arms x NR_ARM_SWEEP_EXPLORE_MAX_TRIALS)
} nr_arm_sweep_gated_t;

static inline void nr_arm_sweep_gated_init(nr_arm_sweep_gated_t *g)
{
  *g = (nr_arm_sweep_gated_t){0};
  g->s.latched = -1;
}

static inline int nr_arm_sweep_gated_pick(const nr_arm_sweep_gated_t *g, int n_arms)
{
  if (!g->explore)
    return 0;
  if (g->s.latched >= 0)
    return g->s.latched;
  const int p = nr_arm_sweep_pick(&g->s, n_arms);
  /* nr_arm_sweep_pick breaks upper-bound ties by the fewest trials; here a tie goes to arm 0. */
  double lo0, hi0, lo, hi;
  nr_arm_sweep_wilson(g->s.ok[0], g->s.tr[0], &lo0, &hi0);
  nr_arm_sweep_wilson(g->s.ok[p], g->s.tr[p], &lo, &hi);
  return (p != 0 && fabs(hi - hi0) <= 1e-12) ? 0 : p;
}

/// Feed one outcome. link_ok: some OTHER decode passed recently (caller's measure). Returns the latched
/// arm (>= 0) once decided, else -1.
static inline int nr_arm_sweep_gated_feed(nr_arm_sweep_gated_t *g, int n_arms, int arm, bool tb_ok, bool link_ok)
{
  if (arm < 0 || arm >= n_arms || g->s.latched >= 0)
    return g->s.latched;
  if (!g->explore) {
    if (arm != 0)
      return -1;
    g->s.tr[0]++; /* real arm-0 evidence, kept for the Wilson comparison once exploring */
    if (tb_ok)
      g->s.ok[0]++;
    if (!link_ok)
      return -1; /* a dead link says nothing about the incumbent */
    g->win_tr++;
    g->win_ok += tb_ok;
    if (g->win_tr >= NR_ARM_SWEEP_INCUMBENT_MIN_TRIALS) {
      if ((double)g->win_ok <= NR_ARM_SWEEP_INCUMBENT_POOR_RATE * (double)g->win_tr)
        g->explore = true;
      g->win_tr = g->win_ok = 0;
    }
    return -1;
  }
  const int l = nr_arm_sweep_feed(&g->s, n_arms, arm, tb_ok);
  if (l >= 0)
    return l;
  if (++g->explore_tr < (uint32_t)n_arms * NR_ARM_SWEEP_EXPLORE_MAX_TRIALS)
    return -1;
  g->s.latched = 0; /* budget spent, nothing separated: a tie stays on the incumbent */
  return 0;
}

#ifdef __cplusplus
}
#endif

#endif // NR_ARM_SWEEP_H
