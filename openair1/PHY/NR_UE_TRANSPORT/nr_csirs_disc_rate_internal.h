/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* ISAC_CSIRS_DISC_RATELIMIT=1: discovery rate state machine (pure logic, no globals, unit-tested).
 * SEARCH: the existing full-rate ingest duty (acquisition burst, then 1 window in `div`).
 * STABLE: the confirmed set has been unchanged for `stable_cycles` search cycles and for 2x the time it took to build
 *   it (and >= 4 occasions of its slowest resource). Ingest drops to G consecutive windows per `div_s`; the windows
 *   still run the unchanged pipeline, so they are both the cheap re-check of the confirmed set and the slow
 *   background search for new/changed resources.
 * Rates are derived, not tuned per cell:
 *   G     = ceil(4 * maxP / win)          windows that contain >= 4 occasions of the slowest confirmed resource
 *   m     = ceil(ln(miss) / ln(1 - pdet)) checks needed to push the false-miss probability below `miss`
 *   cycle = notice / m                    check spacing so that a change is noticed within `notice` at that bound
 *   div_s = max(ceil(cycle / win), G * div) never above the SEARCH duty.
 * Any evidence of change returns to SEARCH and re-arms the acquisition burst. */
#ifndef NR_CSIRS_DISC_RATE_INTERNAL_H
#define NR_CSIRS_DISC_RATE_INTERNAL_H
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
  uint32_t win, div;        /* SEARCH ingest: `win` contiguous slots every `div` windows */
  uint64_t notice_slots;    /* tolerated latency to notice a change while STABLE */
  double miss, pdet;        /* false-miss bound, per-check detection probability assumed (conservative) */
  int stable_cycles;
} nr_csirs_rl_cfg_t;

typedef struct {
  uint64_t slot;            /* latest absolute slot */
  int n_conf;               /* confirmed discovered resources now */
  int lost;                 /* resources that were confirmed and are unmatched now (level, not edge; debounced by the state machine) */
  bool new_pattern;         /* detector footprint outside the learned baseline and at least as strong as the weakest confirmed */
  uint32_t max_period;      /* slowest confirmed period, slots */
  uint64_t crc_ok, crc_dec; /* cumulative PDSCH CRC counters (0/0 when unavailable) */
  bool epoch;               /* configuration epoch / reconfiguration since last step */
} nr_csirs_rl_obs_t;

typedef struct {
  bool stable, rearm;       /* rearm: caller restarts the acquisition burst (one-shot) */
  int n_conf;
  uint64_t first_conf, last_change;
  uint64_t crc_ok0, crc_dec0, crc_ok_b, crc_dec_b; /* baseline block, current block start */
  int lost_streak, crc_streak; /* consecutive observations/blocks in violation (statistical debounce) */
  uint64_t need;            /* slots the set must stay unchanged before STABLE (diagnostic) */
  double p0;
  uint32_t G, div_s;
  const char *reason;
} nr_csirs_rl_t;

static inline uint32_t nr_csirs_rl_checks(const nr_csirs_rl_cfg_t *c)
{
  double m = ceil(log(c->miss) / log(1.0 - c->pdet));
  return m < 1 ? 1 : (uint32_t)m;
}

static inline void nr_csirs_rl_to_search(nr_csirs_rl_t *s, const nr_csirs_rl_obs_t *o, const char *why, bool rearm)
{
  s->stable = false; s->rearm = rearm; s->last_change = o->slot; s->reason = why;
  s->crc_ok0 = o->crc_ok; s->crc_dec0 = o->crc_dec; s->p0 = 0;
}

/* Returns true when the state changed (caller logs / applies the duty). */
static inline bool nr_csirs_rl_step(nr_csirs_rl_t *s, const nr_csirs_rl_cfg_t *c, const nr_csirs_rl_obs_t *o)
{
  const bool was = s->stable;
  if (o->epoch) {
    *s = (nr_csirs_rl_t){0};
    nr_csirs_rl_to_search(s, o, "epoch", was);
    return was;
  }
  if (!s->n_conf && o->n_conf) s->first_conf = o->slot;
  if (s->stable) {
    /* a loss counts only when seen in 2 consecutive observations (a resource being re-confirmed or widened is transiently unmatched) */
    s->lost_streak = o->lost > 0 ? s->lost_streak + 1 : 0;
    const char *why = s->lost_streak >= 2 ? "confirmed-lost" : o->n_conf != s->n_conf ? "confirmed-changed"
                    : o->new_pattern ? "new-pattern" : NULL;
    /* CRC drop: disjoint blocks of >= 200 decodes vs the baseline rate; two-proportion z with +1/+1 pseudo-counts (valid at p0 near 0 or 1),
     * 3 sigma one-sided, and 2 consecutive violating blocks (a single look at 3 sigma false-alarms repeatedly over a long STABLE) */
    if (!why && s->crc_dec0 >= 200 && o->crc_dec - s->crc_dec_b >= 200) {
      const double n1 = (double)(o->crc_dec - s->crc_dec_b), n0 = (double)s->crc_dec0;
      const double k1 = (double)(o->crc_ok - s->crc_ok_b), k0 = s->p0 * n0;
      const double q1 = (k1 + 1) / (n1 + 2), q0 = (k0 + 1) / (n0 + 2), pp = (k0 + k1 + 2) / (n0 + n1 + 4);
      const double sd = sqrt(pp * (1 - pp) * (1.0 / n1 + 1.0 / n0));
      s->crc_streak = (q0 - q1) / sd > 3.0 ? s->crc_streak + 1 : 0;
      s->crc_ok_b = o->crc_ok; s->crc_dec_b = o->crc_dec;
      if (s->crc_streak >= 2) why = "crc-drop";
    }
    s->n_conf = o->n_conf;
    if (why) { nr_csirs_rl_to_search(s, o, why, true); return true; }
    return false;
  }
  if (o->n_conf != s->n_conf) { s->last_change = o->slot; s->crc_ok0 = o->crc_ok; s->crc_dec0 = o->crc_dec; }
  s->n_conf = o->n_conf;
  s->lost_streak = 0; s->crc_streak = 0;
  if (!o->n_conf || o->lost) { s->last_change = o->slot; return false; }
  uint64_t need = (uint64_t)c->stable_cycles * c->div * c->win;
  const uint64_t spread = 2 * (s->last_change - s->first_conf), occ = 4ull * o->max_period;
  if (spread > need) need = spread;
  if (occ > need) need = occ;
  s->need = need;
  if (o->slot - s->last_change < need) return false;
  uint32_t G = (uint32_t)((4ull * o->max_period + c->win - 1) / c->win);
  if (G < 1) G = 1;
  uint32_t div_s = (uint32_t)((c->notice_slots / nr_csirs_rl_checks(c) + c->win - 1) / c->win);
  if (div_s < G * c->div) div_s = G * c->div;
  s->G = G; s->div_s = div_s; s->stable = true; s->rearm = false; s->reason = "stable";
  s->crc_ok_b = o->crc_ok; s->crc_dec_b = o->crc_dec;
  s->crc_dec0 = o->crc_dec - s->crc_dec0; /* decodes seen since the last change */
  s->p0 = s->crc_dec0 ? (double)(o->crc_ok - s->crc_ok0) / (double)s->crc_dec0 : 0;
  return true;
}
#endif
