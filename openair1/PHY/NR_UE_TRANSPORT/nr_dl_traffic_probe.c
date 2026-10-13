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
      if (!granted_rb(grants, n_grants, r)) { st->pdsch_like_ungranted++; pdsch_un = true; }
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
