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

#include "nr_hyp_sweep.h"
#include "nr_crc_evidence.h"
#include "common/utils/LOG/log.h"
#include <string.h>
#include <math.h>

static int refuse(nr_hyp_sweep_state_t *st, int error)
{
  if (st) { st->n_classes = 0; st->winner = -1; }
  LOG_E(PHY, "UL hypothesis search refused: error=%d (raw cap=%d, class cap=%d)\n",
        error, NR_HYP_SWEEP_MAX_RAW, NR_HYP_SWEEP_MAX_CLASSES);
  return error;
}
int nr_hyp_sweep_init(nr_hyp_sweep_state_t *st, const nr_hyp_t *raw, int n,
                     nr_hyp_constraint_fn constraint, void *cc, nr_hyp_equivalent_fn eq,
                     const void *const *samples, int ns, void *ec)
{
  if (!st) return refuse(st, NR_HYP_SWEEP_INVALID);
  memset(st, 0, sizeof(*st));
  st->winner = -1;
  memset(st->class_of_raw, -1, sizeof(st->class_of_raw));
  if (n > NR_HYP_SWEEP_MAX_RAW) return refuse(st, NR_HYP_SWEEP_RAW_OVERFLOW);
  if (n < 0 || (n && !raw) || ns < 0 || (ns && !samples))
    return refuse(st, NR_HYP_SWEEP_INVALID);
  for (int s = 0; s < ns; ++s)
    if (!samples[s]) return refuse(st, NR_HYP_SWEEP_INVALID);
  for (int i = 0; i < n; ++i) {
    if (raw[i].len <= 0 || raw[i].len > NR_HYP_BYTES)
      return refuse(st, NR_HYP_SWEEP_INVALID);
    if (constraint && !constraint(&raw[i], cc)) continue;
    int cls = -1;
    if (eq && ns) {
      for (int c = 0; c < st->n_classes; ++c) {
        bool same = true;
        for (int s = 0; s < ns && same; ++s)
          same = eq(&st->classes[c].hyp, &raw[i], samples[s], ec);
        if (same) { cls = c; break; }
      }
    }
    if (cls < 0) {
      if (st->n_classes == NR_HYP_SWEEP_MAX_CLASSES)
        return refuse(st, NR_HYP_SWEEP_CLASS_OVERFLOW);
      cls = st->n_classes++;
      st->order[cls] = cls;
      st->classes[cls].hyp = raw[i];
    }
    st->classes[cls].members++;
    st->class_of_raw[i] = cls;
  }
  return st->n_classes;
}
/* Allocate probes using an anytime Hoeffding confidence bound. A union bound
 * over both tails, all classes and n>=1 uses delta/[K*(n+1)*(n+2)].
 * Skipping a statistically inferior probe is NOT a CRC success or a committed
 * winner. It remains eligible if subsequent leader evidence deteriorates.
 * Untested classes retain an upper bound of one and are never discarded here. */
static double probe_radius(const nr_hyp_class_t *c)
{
  if (!c->trials) return 1.0;
  const double n=(double)c->trials;
  return sqrt(log(2.0*NR_HYP_SWEEP_MAX_CLASSES*(n+1.0)*(n+2.0)/1e-6)/(2.0*n));
}

int nr_hyp_sweep_next(nr_hyp_sweep_state_t *st, const void *cand,
                     nr_hyp_plausible_fn plausible, void *ctx, nr_hyp_t *out)
{
  if (!st || !out || st->n_classes <= 0) return -1;
  if (st->winner >= 0) {
    const nr_hyp_t *h = &st->classes[st->winner].hyp;
    if (plausible && !plausible(h, cand, ctx)) return -1;
    *out = *h;
    return st->winner;
  }
  double leader_lower=0.0;
  for (int c=0; c<st->n_classes; ++c) {
    const nr_hyp_class_t *h=&st->classes[c];
    if (!h->trials || !h->passes) continue;
    const double lo=(double)h->passes/h->trials-probe_radius(h);
    if (lo>leader_lower) leader_lower=lo;
  }
  /* Reshuffle at most once per call, only at entry. Doing this inside the loop below
   * (checked on every candidate) let a wraparound mid-pass reshuffle the very order this
   * call is iterating -- positions already visited before the wrap could reappear after
   * it while a class that was never visited got silently skipped, so a call could return
   * -1 even though a plausible, unpruned class existed but was simply never reached this
   * pass. A single call always advances the cursor by exactly n_classes steps when it
   * doesn't return early, i.e. exactly one full lap of a FIXED order array -- reshuffling
   * only between calls preserves the "every class visited exactly once per call" guarantee
   * regardless of where in the cycle this call starts. */
  if (!st->cursor) nr_crc_shuffle(st->order, st->n_classes, &st->random_state);
  for (int t = 0; t < st->n_classes; ++t) {
    const int c = st->order[st->cursor];
    st->cursor = (st->cursor + 1) % st->n_classes;
    const nr_hyp_class_t *h=&st->classes[c];
    if (h->trials && (double)h->passes/h->trials+probe_radius(h)<leader_lower)
      continue;
    if (!plausible || plausible(&st->classes[c].hyp, cand, ctx)) {
      *out = st->classes[c].hyp;
      return c;
    }
    st->classes[c].skipped++;
  }
  return -1;
}
static double rate(const nr_hyp_class_t *c)
{
  return c->trials ? (double)c->passes / c->trials : 0.0;
}
/* A hypothesis that cannot even INTERPRET the observed payloads is disproved by that alone: the
 * true field layout must yield a valid grant for every real DCI, so a class that fails extraction
 * on every candidate cannot be the right one. Such a class is never selected, so it never accrues
 * trials, and the "every class must reach MIN_TRIALS" gate below would wait for it forever.
 *
 * Measured live 2026-09-09: 101 classes, 8000 trials accumulated, and the slowest class still at
 * 0/300 -- the search could not have converged in any run length.
 *
 * Retiring these does NOT weaken the oracle. It removes candidates that failed a NECESSARY
 * condition, and only after MIN_TRIALS separate opportunities to interpret something, which is the
 * same evidence threshold every other decision here uses. A class with even one successful
 * extraction is never retired. */
static bool eliminated(const nr_hyp_class_t *c)
{
  return c->trials == 0 && c->skipped >= NR_HYP_SWEEP_MIN_TRIALS;
}
int nr_hyp_sweep_feed(nr_hyp_sweep_state_t *st, int idx, bool ok)
{
  if (!st || idx < 0 || idx >= st->n_classes) return -1;
  if (st->classes[idx].trials == UINT64_MAX) return -1;
  st->classes[idx].trials++;
  st->classes[idx].passes += ok;
  /* Selection does not end measurement. Baseline validation consumes the
   * selected class's subsequent CRC outcomes, not a frozen exploration score. */
  if (st->winner >= 0) return st->winner;
  /* A clearly separated, high-quality candidate need not wait for 300
   * trials of every incorrect class. Keep the legacy marginal-link gate below. */
  if ((st->classes[idx].trials % 16) == 0) {
    int leader=0;
    for(int c=1;c<st->n_classes;c++)
      if(rate(&st->classes[c])>rate(&st->classes[leader])) leader=c;
    double lo,hi;
    nr_crc_interval(st->classes[leader].passes,st->classes[leader].trials,
                    NR_HYP_SWEEP_MAX_CLASSES,&lo,&hi);
    bool separated=st->classes[leader].trials>=64 && lo>=0.60;
    for(int c=0;c<st->n_classes && separated;c++) {
      if(c==leader || eliminated(&st->classes[c])) continue;
      double other_lo,other_hi;
      nr_crc_interval(st->classes[c].passes,st->classes[c].trials,
                      NR_HYP_SWEEP_MAX_CLASSES,&other_lo,&other_hi);
      if(other_hi>=lo) separated=false;
    }
    if(separated) { st->winner=leader; return leader; }
  }
  int best = -1;
  for (int c = 0; c < st->n_classes; ++c) {
    if (eliminated(&st->classes[c])) continue;
    if (st->classes[c].trials < NR_HYP_SWEEP_MIN_TRIALS) return -1;
    if (best < 0 || rate(&st->classes[c]) > rate(&st->classes[best])) best = c;
  }
  if (best < 0) return -1; // every class retired: nothing interpreted anything, stay unresolved
  double second = 0.0;
  for (int c = 0; c < st->n_classes; ++c)
    if (c != best && !eliminated(&st->classes[c]) && rate(&st->classes[c]) > second)
      second = rate(&st->classes[c]);
  if (rate(&st->classes[best]) >= NR_HYP_SWEEP_MIN_RATE &&
      rate(&st->classes[best]) >= NR_HYP_SWEEP_WIN_RATIO * second)
    st->winner = best;
  return st->winner;
}
int nr_hyp_sweep_winner(const nr_hyp_sweep_state_t *st)
{
  return st && st->n_classes > 0 ? st->winner : -1;
}
