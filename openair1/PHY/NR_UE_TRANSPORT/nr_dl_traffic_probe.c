/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* See nr_dl_traffic_probe.h. Pure functions plus the grant registry; the RT feed is nr_dl_traffic_probe_rt.c. */
#include "nr_dl_traffic_probe.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void nr_dltp_energy(const int16_t *rxF, int fft, int fco, int n_rb, float *e)
{
  for (int s = 0; s < NR_DLTP_NSYM; ++s) {
    const int16_t *sym = rxF + (size_t)s * fft * 2;
    for (int r = 0; r < n_rb; ++r) {
      float acc = 0.f;
      for (int k = 0; k < 12; ++k) {
        const int bin = (fco + 12 * r + k) % fft;
        const float i = sym[2 * bin], q = sym[2 * bin + 1];
        acc += i * i + q * q;
      }
      e[s * n_rb + r] = acc;
    }
  }
}

static int cmp_float(const void *a, const void *b)
{
  const float x = *(const float *)a, y = *(const float *)b;
  return x < y ? -1 : x > y;
}

float nr_dltp_floor(const float *e, int n, float *scratch)
{
  if (n < 1) return 0.f;
  memcpy(scratch, e, sizeof(float) * (size_t)n);
  qsort(scratch, (size_t)n, sizeof(float), cmp_float);
  return scratch[n / 10] / NR_DLTP_P10_RATIO;
}

static bool granted(const nr_dltp_grant_t *g, int n, int rb, int sym)
{
  for (int i = 0; i < n; ++i)
    if (rb >= g[i].rb0 && rb < g[i].rb0 + g[i].n_rb && sym >= g[i].s && sym < g[i].s + g[i].l) return true;
  return false;
}
static bool granted_rb(const nr_dltp_grant_t *g, int n, int rb)
{
  for (int i = 0; i < n; ++i)
    if (rb >= g[i].rb0 && rb < g[i].rb0 + g[i].n_rb) return true;
  return false;
}

void nr_dltp_slot(const float *e, const nr_dltp_geom_t *g, float floor_mean, bool tdd_known,
                  const nr_dltp_grant_t *grants, int n_grants, nr_dltp_stats_t *st)
{
  if (g->n_rb < 1 || g->n_rb > NR_DLTP_MAX_RB || floor_mean <= 0.f) return;
  const float thr = floor_mean * NR_DLTP_OCC_RATIO;
  const int rb0 = g->carrier_rb0 < 0 ? 0 : g->carrier_rb0, rb1 = g->carrier_rb1 > g->n_rb ? g->n_rb : g->carrier_rb1;
  bool ctrl_hit[3] = {false, false, false}, pdsch_un = false;
  st->slots++;
  st->slots_tdd_unknown += !tdd_known;
  st->grants_used += (uint32_t)n_grants;
  st->floor_mean = floor_mean;
  for (int r = rb0; r < rb1; ++r) {
    if (r >= g->ssb_rb0 && r < g->ssb_rb1) continue;
    for (int s = 0; s < 3; ++s) {
      if (g->css0_nsym > s && r >= g->css0_rb0 && r < g->css0_rb1) continue;
      if (e[s * g->n_rb + r] > thr && !granted(grants, n_grants, r, s)) { st->ctrl_rbs[s]++; ctrl_hit[s] = true; }
    }
    int run = 0, best = 0;
    for (int s = 3; s < NR_DLTP_NSYM; ++s) {
      const bool lit = e[s * g->n_rb + r] > thr;
      st->cells++;
      if (lit) {
        st->cells_lit++;
        if (granted(grants, n_grants, r, s)) st->cells_granted_lit++; else st->cells_ungranted_lit++;
        if (++run > best) best = run;
      } else run = 0;
    }
    if (best >= NR_DLTP_PDSCH_RUN) {
      st->pdsch_like++;
      if (!granted_rb(grants, n_grants, r)) {
        st->pdsch_like_ungranted++; pdsch_un = true;
        if (!(g->css0_rb1 > g->css0_rb0 && r >= g->css0_rb0 && r < g->css0_rb1)) st->pdsch_like_ungranted_nocss0++;
      }
    }
  }
  for (int s = 0; s < 3; ++s) st->ctrl_slots[s] += ctrl_hit[s];
  st->slots_ungranted_pdsch += pdsch_un;
}

int nr_dltp_verdict(const nr_dltp_stats_t *st, char *buf, size_t n)
{
  /* Null (noise only): P(>= 3 consecutive false cells of 11 | 1e-4 each) ~ 1e-11 per RB-slot, so even 8 ungranted
   * PDSCH-like RB-slots over a run are ~1e-7 x the bound. 8, not 1: leakage from a granted neighbour RB and a wrong
   * grant extent register as a few isolated hits. The ABSENT floor of 100 probed slots (about 1 s at one probe in 16
   * slots) is sampling depth, not a tuning. */
  const int v = st->pdsch_like_ungranted >= 8 ? 1 : (st->slots >= 100 ? -1 : 0);
  if (buf && n)
    snprintf(buf, n,
             "TRAFFIC slots=%u (tdd_unknown=%u) grants=%u floor=%.0f data_cells=%llu lit=%llu granted_lit=%llu "
             "ungranted_lit=%llu pdsch_like_rbslots=%llu ungranted=%llu (slots %u) ctrl_outside_css0 rbs=%llu/%llu/%llu "
             "slots=%u/%u/%u -> UE-specific traffic %s",
             st->slots, st->slots_tdd_unknown, st->grants_used, (double)st->floor_mean,
             (unsigned long long)st->cells, (unsigned long long)st->cells_lit, (unsigned long long)st->cells_granted_lit,
             (unsigned long long)st->cells_ungranted_lit, (unsigned long long)st->pdsch_like,
             (unsigned long long)st->pdsch_like_ungranted, st->slots_ungranted_pdsch,
             (unsigned long long)st->ctrl_rbs[0], (unsigned long long)st->ctrl_rbs[1], (unsigned long long)st->ctrl_rbs[2],
             st->ctrl_slots[0], st->ctrl_slots[1], st->ctrl_slots[2],
             v > 0 ? "PRESENT (unexplained PDSCH-like occupancy; co-channel cells count too)"
                   : v < 0 ? "ABSENT (every lit PDSCH-like RB is a decoded grant)" : "UNDECIDED");
  return v;
}

static pthread_mutex_t g_gmu = PTHREAD_MUTEX_INITIALIZER;
static nr_dltp_grant_t g_grants[NR_DLTP_GRANTS];
static unsigned g_gn;

void nr_dltp_note_grant(uint16_t frame, uint8_t slot, int rb0, int n_rb, int s, int l)
{
  if (rb0 < 0 || n_rb < 1 || s < 0 || l < 1 || s + l > NR_DLTP_NSYM) return;
  pthread_mutex_lock(&g_gmu);
  g_grants[g_gn++ % NR_DLTP_GRANTS] = (nr_dltp_grant_t){frame, slot, (uint16_t)rb0, (uint16_t)n_rb, (uint8_t)s, (uint8_t)l};
  pthread_mutex_unlock(&g_gmu);
}

int nr_dltp_grants_for(uint16_t frame, uint8_t slot, nr_dltp_grant_t *out, int cap)
{
  int n = 0;
  pthread_mutex_lock(&g_gmu);
  const unsigned used = g_gn < NR_DLTP_GRANTS ? g_gn : NR_DLTP_GRANTS;
  for (unsigned i = 0; i < used && n < cap; ++i)
    if (g_grants[i].frame == frame && g_grants[i].slot == slot) out[n++] = g_grants[i];
  pthread_mutex_unlock(&g_gmu);
  return n;
}

/* nr_cdisc_step: one evaluation of the CORESET_DISC state (see header). recover is set when the caller should re-arm the sweep. */
void nr_cdisc_step(nr_cdisc_t *c, const nr_cdisc_in_t *in, nr_cdisc_out_t *out)
{
  const nr_cdisc_state_t old = c->state;
  out->recover = 0; out->traffic = 0; out->reacquire = 0; out->rearm = 0;
  if (in->bank_count == 0 && in->sync_poor) {
    /* unusable symbols are not a miss: no grace progress, no recovery budget */
    if (!c->was_poor) { c->was_poor = 1; c->poor_since_s = in->now_s; c->last_reacquire_s = -1e9; }
    c->state = NR_CDISC_SYNC_POOR; out->reason = "sync_poor_timing_unstable";
    if (in->now_s - c->last_reacquire_s >= in->miss_s) { c->last_reacquire_s = in->now_s; c->reacquires++; out->reacquire = 1; }
    out->changed = c->state != old;
    return;
  }
  if (c->was_poor) { /* sync is back: exclude the poor time from the grace and re-arm the sweep over fresh symbols */
    const double lost = in->now_s - c->poor_since_s;
    c->armed_s += lost; c->last_recover_s += lost; c->was_poor = 0; out->rearm = 1;
  }
  if (in->bank_count > 0) {
    c->state = NR_CDISC_LOCKED; c->recoveries = 0; out->reason = "dedicated_coreset_banked";
  } else if (!in->carrier_ok) {
    c->state = NR_CDISC_SEARCHING; c->armed = 0; out->reason = "carrier_pending";
  } else {
    if (!c->armed) { c->armed = 1; c->armed_s = in->now_s; c->last_recover_s = in->now_s; }
    const double since = in->now_s - c->armed_s;
    const double rate = in->window_probed ? (double)in->window_rbslots / (double)in->window_probed : 0.0;
    out->traffic = in->window_rbslots >= NR_CDISC_RBSLOTS_PRESENT && rate >= NR_CDISC_RATE_PRESENT ? 1
                 : in->window_probed >= NR_CDISC_PROBED_ABSENT && in->window_rbslots < 8 ? -1 : 0;
    if (out->traffic < 0) { c->state = NR_CDISC_NO_TRAFFIC; out->reason = "probe_absent_no_unexplained_pdsch"; }
    else if (out->traffic == 0) { c->state = NR_CDISC_SEARCHING; out->reason = "traffic_undecided"; }
    else if (since < in->miss_s) { c->state = NR_CDISC_SEARCHING; out->reason = "traffic_present_within_grace"; }
    else {
      c->state = NR_CDISC_MISSED; out->reason = "traffic_present_no_dedicated_coreset";
      /* linear back-off: the k-th recovery waits (k+1) x miss_s after the previous one */
      if (c->recoveries < in->max_recover && in->now_s - c->last_recover_s >= (c->recoveries + 1) * in->miss_s) {
        c->recoveries++; c->last_recover_s = in->now_s; out->recover = 1;
      }
    }
  }
  out->changed = c->state != old;
}

/* ---- sync quality (ACQMISS-2) ---- */
#include <stdatomic.h>
static int cmp_u(const void *a, const void *b) { unsigned x = *(const unsigned *)a, y = *(const unsigned *)b; return x < y ? -1 : x > y; }
int nr_syncq_classify(const unsigned *mag, int n, int prev_poor, int *med, int *p90)
{
  if (n < NR_SYNCQ_MIN) return -1;
  unsigned s[NR_SYNCQ_WIN];
  if (n > NR_SYNCQ_WIN) n = NR_SYNCQ_WIN;
  memcpy(s, mag, (size_t)n * sizeof(unsigned));
  qsort(s, (size_t)n, sizeof(unsigned), cmp_u);
  const int m = (int)s[n / 2], p = (int)s[(int)(n * 0.9)];
  if (med) *med = m;
  if (p90) *p90 = p;
  if (m > NR_SYNCQ_MED_POOR || p > NR_SYNCQ_P90_POOR) return 2;
  if (prev_poor && (m > NR_SYNCQ_MED_GOOD || p > NR_SYNCQ_P90_GOOD)) return 2;
  return m > NR_SYNCQ_MED_DEGRADED ? 1 : 0;
}
static _Atomic unsigned g_sq_ring[NR_SYNCQ_WIN];
static _Atomic unsigned g_sq_count, g_sq_poor, g_sq_level, g_sq_reacq, g_sq_clamp;
static _Atomic int g_sq_med, g_sq_p90;
int nr_syncq_enabled(void)
{
  static int e = -1;
  if (e < 0) { const char *v = getenv("ISAC_CORESET_DISC_SYNCQ"); e = v ? atoi(v) : 0; if (e < 0 || e > 2) e = 0; }
  return e != 0;
}
void nr_syncq_note(int step)
{
  if (!nr_syncq_enabled()) return;
  const unsigned k = atomic_load_explicit(&g_sq_count, memory_order_relaxed);
  atomic_store_explicit(&g_sq_ring[k % NR_SYNCQ_WIN], (unsigned)(step < 0 ? -step : step), memory_order_relaxed);
  atomic_store_explicit(&g_sq_count, k + 1, memory_order_release);
  unsigned w[NR_SYNCQ_WIN];
  const int n = k + 1 < NR_SYNCQ_WIN ? (int)(k + 1) : NR_SYNCQ_WIN;
  for (int i = 0; i < n; ++i) w[i] = atomic_load_explicit(&g_sq_ring[i], memory_order_relaxed);
  int med = 0, p90 = 0;
  const int r = nr_syncq_classify(w, n, (int)atomic_load_explicit(&g_sq_poor, memory_order_relaxed), &med, &p90);
  atomic_store_explicit(&g_sq_med, med, memory_order_relaxed);
  atomic_store_explicit(&g_sq_p90, p90, memory_order_relaxed);
  atomic_store_explicit(&g_sq_poor, r == 2, memory_order_relaxed);
  atomic_store_explicit(&g_sq_level, r < 0 ? 0 : r, memory_order_relaxed);
}
int nr_syncq_status(int *med, int *p90)
{
  if (med) *med = atomic_load_explicit(&g_sq_med, memory_order_relaxed);
  if (p90) *p90 = atomic_load_explicit(&g_sq_p90, memory_order_relaxed);
  if (atomic_load_explicit(&g_sq_count, memory_order_acquire) < NR_SYNCQ_MIN) return -1;
  return (int)atomic_load_explicit(&g_sq_level, memory_order_relaxed);
}
/* ISAC_CORESET_DISC_SYNCQ=1: report + re-acquire + re-arm. =2: additionally pause the nID capture while POOR. MEASURED (replay, injected wander): pausing
 * HURTS where the sweep still works with poor sync (TIM tile3 banked 1 CORESET with wander and 0 when paused), so 2 is not the recommended value. */
int nr_syncq_gate_closed(void)
{
  static int lvl = -1;
  if (lvl < 0) { const char *v = getenv("ISAC_CORESET_DISC_SYNCQ"); lvl = v ? atoi(v) : 0; }
  return lvl == 2 && atomic_load_explicit(&g_sq_poor, memory_order_relaxed);
}
void nr_syncq_request_reacquire(void) { atomic_store_explicit(&g_sq_reacq, 1, memory_order_relaxed); atomic_store_explicit(&g_sq_clamp, 1, memory_order_relaxed); }
int nr_syncq_take_reacquire(void) { return nr_syncq_enabled() && atomic_exchange_explicit(&g_sq_reacq, 0, memory_order_relaxed); }
int nr_syncq_clamp(void) { return nr_syncq_enabled() && atomic_load_explicit(&g_sq_clamp, memory_order_relaxed) ? NR_SYNCQ_CLAMP : 0; }
void nr_syncq_clamp_release(void) { atomic_store_explicit(&g_sq_clamp, 0, memory_order_relaxed); }
