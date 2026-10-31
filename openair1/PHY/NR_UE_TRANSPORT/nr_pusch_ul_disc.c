/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_pusch_ul_disc.h"
#include "nr_pdcch_blind_monitor.h"
#include "PHY/NR_REFSIG/nr_refsig.h"
#include "PHY/gold.h"
#include "openair2/LAYER2/NR_MAC_COMMON/nr_mac_common.h"
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "common/utils/nr_cell.h"
#include "PHY/nr_phy_common/inc/nr_passive_delay_fast.h"

#define UL_DISC_TRACKS 128
#define UL_DISC_SCAN_PER_GRANT 64
#define UL_DISC_FAMILY (65536.0 * 2.0 * 2.0 * 32.0)
#define UL_DISC_ALPHA 1.0e-6
#define UL_DISC_EVIDENCE 4
#define UL_DISC_TM 3          /* arrival-window candidates accumulated per RNTI (nr_pusch_ul_disc_timing_evidence) */
#define UL_DISC_TM_TOL 4.0    /* samples: observations of one candidate agree within this (the closed-form delay is read to a fraction of it) */
#define UL_DISC_TM_TAU 600.0  /* slots: e-folding time of the accumulated evidence */
typedef struct {
  uint16_t rnti;
  bool used, timed;
  long last_slot;
  double correction;
  uint32_t scan_cursor;
  nr_pusch_ul_disc_result_t found;
  struct { int id, nscid, type, grants; bool used; double x, dof, ln_trials; int64_t last_slot; } ev[UL_DISC_EVIDENCE]; /* DM-RS identity evidence without a CRC */
  struct { bool used; double a1, x, dof, ln_trials; int grants; int64_t last_slot, obs_slot; } tm[UL_DISC_TM]; /* arrival-window evidence without a per-grant bound */
} ul_disc_track_t;
typedef ul_disc_track_t tracks_cell_t[UL_DISC_TRACKS];
NR_CELL_STATIC(tracks_cell_t, tracks);
#define tracks NR_CELL_REF(tracks)
NR_CELL_STATIC_INIT(pthread_mutex_t, track_lock, PTHREAD_MUTEX_INITIALIZER);
#define track_lock NR_CELL_REF(track_lock)
NR_CELL_STATIC(unsigned, evict_next);
#define evict_next NR_CELL_REF(evict_next)
NR_CELL_STATIC(uint32_t, current_epoch);
#define current_epoch NR_CELL_REF(current_epoch)
#define UL_DISC_CELL_N 16
typedef int cell_res_cell_t[UL_DISC_CELL_N];
NR_CELL_STATIC(cell_res_cell_t, cell_res); /* cell prior of the timing tracker; kept across epochs (timing is physical) */
#define cell_res NR_CELL_REF(cell_res)
NR_CELL_STATIC(int, cell_n);
#define cell_n NR_CELL_REF(cell_n)

static ul_disc_track_t *track(uint16_t rnti)
{
  for (int i = 0; i < UL_DISC_TRACKS; ++i)
    if (tracks[i].used && tracks[i].rnti == rnti) return &tracks[i];
  for (int i = 0; i < UL_DISC_TRACKS; ++i)
    if (!tracks[i].used) {
      tracks[i] = (ul_disc_track_t){.rnti=rnti, .used=true};
      return &tracks[i];
    }
  ul_disc_track_t *t = &tracks[evict_next++ % UL_DISC_TRACKS];
  *t = (ul_disc_track_t){.rnti=rnti, .used=true};
  return t;
}

nr_pusch_ul_disc_result_t nr_pusch_ul_disc_cached(uint16_t rnti)
{
  nr_pusch_ul_disc_result_t result = {.id=-1, .false_alarm_bound=1};
  pthread_mutex_lock(&track_lock);
  for (int i = 0; i < UL_DISC_TRACKS; ++i)
    if (tracks[i].used && tracks[i].rnti == rnti) {
      result = tracks[i].found;
      result.fresh = false;
      break;
    }
  pthread_mutex_unlock(&track_lock);
  return result;
}

void nr_pusch_ul_disc_reset(void)
{
  pthread_mutex_lock(&track_lock);
  memset(tracks, 0, sizeof(tracks));
  evict_next = 0;
  current_epoch = 0;
  cell_n = 0;
  pthread_mutex_unlock(&track_lock);
}

void nr_pusch_ul_disc_epoch(uint32_t dedicated_epoch)
{
  pthread_mutex_lock(&track_lock);
  if (current_epoch != dedicated_epoch) {
    memset(tracks, 0, sizeof(tracks));
    evict_next = 0;
    current_epoch = dedicated_epoch;
  }
  pthread_mutex_unlock(&track_lock);
}

/* cell-level prior for RNTIs never timed: the residual most other accepted residuals agree with (+-64), from the last 16 */
static bool cell_mode(int *out)
{
  int best = -1, bv = 0;
  for (int i = 0; i < cell_n && i < UL_DISC_CELL_N; i++) {
    int v = 0;
    for (int k = 0; k < cell_n && k < UL_DISC_CELL_N; k++) v += abs(cell_res[k] - cell_res[i]) <= 64;
    if (v > bv) { bv = v; best = i; }
  }
  if (best >= 0) *out = cell_res[best];
  return best >= 0;
}

int nr_pusch_ul_disc_timing_seed(uint16_t rnti, int nominal_advance, long abs_slot, bool *tracked)
{
  pthread_mutex_lock(&track_lock);
  ul_disc_track_t *t = track(rnti);
  /* a learned advance outlives the grant that measured it (older slots, 1 s of silence): a failed search decodes near the real timing */
  if (tracked) *tracked = t->timed && labs(abs_slot - t->last_slot) <= 2000;
  int correction = t->timed ? (int)lrint(t->correction) : 0;
  if (!t->timed && !cell_mode(&correction)) correction = 0;
  pthread_mutex_unlock(&track_lock);
  return nominal_advance + correction;
}

/* Per-RNTI accumulation of weak timing observations. One grant of a weak UE reads the apparent arrival (a1 = window advance minus the closed-form
 * delay) with an evidence ratio too low for the 1e-6 bound alone; observations that agree on a1 within UL_DISC_TM_TOL are summed non-coherently
 * (x = dof * ratio, the same statistic as nr_pusch_ul_disc_id_evidence) with an e-folding decay of UL_DISC_TM_TAU slots, so that a UE that is
 * heard repeatedly converges while noise (random a1 per grant) does not: the first observation of a candidate pays the union over its grant's delay
 * trials, each later one only the positions inside the candidate window, and the bound is taken on the summed candidate over the UL_DISC_TM candidates. `family` is the number of identity hypotheses the observations were drawn from. The same absolute slot counts once. */
bool nr_pusch_ul_disc_timing_evidence(uint16_t rnti, int a1, const nr_pusch_ul_disc_delay_t *d, int64_t abs_slot, double family,
                                      int *grants, int *adv, double *bound)
{
  if (grants) *grants = 0;
  if (bound) *bound = 1;
  if (!d || !d->valid || !isfinite(d->ratio)) return false;
  pthread_mutex_lock(&track_lock);
  ul_disc_track_t *t = track(rnti);
  int k = -1, free_k = -1, weak_k = -1;
  double weak_v = INFINITY;
  for (int i = 0; i < UL_DISC_TM; ++i) {
    if (!t->tm[i].used) { if (free_k < 0) free_k = i; continue; }
    const double dt = (double)(abs_slot - t->tm[i].last_slot);
    if (dt > 0 && t->tm[i].last_slot != INT64_MIN) { /* decay to now */
      const double f = exp(-dt / UL_DISC_TM_TAU);
      t->tm[i].x *= f; t->tm[i].dof *= f; t->tm[i].ln_trials *= f;
    }
    if (fabs((double)a1 - t->tm[i].a1) <= UL_DISC_TM_TOL) k = i;
    const double v = t->tm[i].x - t->tm[i].dof;
    if (v < weak_v) { weak_v = v; weak_k = i; }
    if (dt > 0) t->tm[i].last_slot = abs_slot;
  }
  if (k < 0) {
    k = free_k >= 0 ? free_k : weak_k;
    memset(&t->tm[k], 0, sizeof(t->tm[k]));
    t->tm[k] = (typeof(t->tm[k])){.used = true, .a1 = a1, .last_slot = abs_slot, .obs_slot = INT64_MIN};
  }
  typeof(t->tm[k]) *e = &t->tm[k];
  if (e->obs_slot != abs_slot) { /* the same slot seen twice is one observation */
    const double x = d->dof * d->ratio, w0 = e->x - e->dof > 0 ? e->x - e->dof : 0, w1 = x - d->dof > 0 ? x - d->dof : 0;
    e->a1 = (w0 + w1) > 0 ? (e->a1 * w0 + (double)a1 * w1) / (w0 + w1) : (double)a1; /* excess-power weighted mean of the candidate */
    e->x += x;
    e->dof += d->dof;
    /* the first observation picks the candidate among all the delay positions of its grant (union over its trials); a later grant only counts when its
     * maximum falls into that candidate's window, which costs the positions inside the window, not the grant's whole range */
    e->ln_trials += e->grants ? log(2.0 * UL_DISC_TM_TOL + 1.0) : log(d->trials > 1 ? d->trials : 1);
    e->grants++;
    e->obs_slot = abs_slot;
  }
  const double ln = nr_pusch_ul_disc_evidence_ln_bound(e->x, e->dof, e->ln_trials, e->grants > 1 ? family * UL_DISC_TM : family);
  const bool confirmed = ln <= log(UL_DISC_ALPHA);
  if (grants) *grants = e->grants;
  if (bound) *bound = exp(ln);
  if (adv) *adv = (int)lrint(e->a1);
  pthread_mutex_unlock(&track_lock);
  return confirmed;
}

static void timing_update_locked_in(uint16_t rnti, int nominal_advance, int measured_advance, long abs_slot)
{
  pthread_mutex_lock(&track_lock);
  ul_disc_track_t *t = track(rnti);
  const double correction = measured_advance - nominal_advance;
  cell_res[cell_n++ % UL_DISC_CELL_N] = (int)correction;
  if (!t->timed || labs(abs_slot - t->last_slot) > 2000) {
    t->correction = correction;
    t->timed = true;
  } else if (fabs(correction - t->correction) <= 256.0) {
    t->correction += 0.25 * (correction - t->correction);
  }
  if (abs_slot > t->last_slot || !t->timed) t->last_slot = abs_slot;
  pthread_mutex_unlock(&track_lock);
}

void nr_pusch_ul_disc_timing_update(uint16_t rnti, int nominal_advance,
                                    int measured_advance, double score, long abs_slot)
{
  if (score < 0.35 || abs(measured_advance - nominal_advance) > 2048) return;
  timing_update_locked_in(rnti, nominal_advance, measured_advance, abs_slot);
}

void nr_pusch_ul_disc_timing_update_ev(uint16_t rnti, int nominal_advance, int measured_advance, long abs_slot)
{
  if (abs(measured_advance - nominal_advance) > 2048) return;
  timing_update_locked_in(rnti, nominal_advance, measured_advance, abs_slot);
}

static void gold_for(uint32_t *seq, int words, int n_sym, int slot, int sym, int id, int nscid)
{
  const uint64_t a = ((uint64_t)n_sym * slot + sym + 1) * (((uint64_t)id << 1) + 1);
  uint32_t x2 = (uint32_t)(((a << 17) + ((uint64_t)id << 1) + nscid) % (1ULL << 31));
  uint32_t x1 = 0;
  seq[0] = gold_generic(&x1, &x2, 1);
  for (int i = 1; i < words; ++i) seq[i] = gold_generic(&x1, &x2, 0);
}

static double score_impl(const nr_pusch_ul_disc_grid_t *g, uint16_t mask,
                         int type, int nscid, int id, int *pairs, bool phase_sensitive)
{
  if (pairs) *pairs = 0;
  if (!g || !mask || g->fft <= 0 || g->fft > 8192 || g->n_rx < 1 || g->n_rx > 4 ||
      g->n_rb_carrier < 1 || g->n_rb_carrier > 275 || g->rb_count < 1 ||
      g->rb_start < 0 || g->rb_start + g->rb_count > g->n_rb_carrier ||
      g->symbols_per_slot < 1 || g->symbols_per_slot > 14 ||
      (mask >> g->symbols_per_slot) || type < 0 || type > 1 || nscid < 0 || nscid > 1 ||
      id < 0 || id > 65535) return 0;
  for (int a = 0; a < g->n_rx; ++a) if (!g->rx[a]) return 0;
  const int nprb = type ? 4 : 6;
  const int ntot = nprb * (g->rb_start + g->rb_count);
  const int words = ((g->n_rb_carrier * 24) >> 5) + 1;
  uint32_t seq[208];
  c16_t pilot[1650];
  if (words > 208 || ntot > 1650) return 0;
  double nr = 0, ni = 0, den = 0;
  int count = 0;
  for (int sym = 0; sym < g->symbols_per_slot; ++sym) {
    if (!(mask & (1u << sym))) continue;
    gold_for(seq, words, g->symbols_per_slot, g->slot, sym, id, nscid);
    nr_pdsch_dmrs_rx(g->normal_cp ? NR_NORMAL : NR_EXTENDED, seq, pilot, 1000, 0,
                     (unsigned short)(g->rb_start + g->rb_count), type, 16384);
    const c16_t *p = pilot + nprb * g->rb_start;
    for (int a = 0; a < g->n_rx; ++a) {
      const c16_t *row = g->rx[a] + sym * g->fft;
      double prev_r = 0, prev_i = 0;
      for (int m = 0; m < nprb * g->rb_count; ++m) {
        const int off = type ? 6 * (m / 2) + m % 2 : 4 * (m / 2) + 2 * (m % 2);
        const int bin = (g->first_carrier + 12 * g->rb_start + off) % g->fft;
        const c16_t y = row[bin];
        const double hr = (double)p[m].r * y.r - (double)p[m].i * y.i;
        const double hi = (double)p[m].r * y.i + (double)p[m].i * y.r;
        den += hr * hr + hi * hi;
        if (m) {
          nr += hr * prev_r + hi * prev_i;
          ni += hi * prev_r - hr * prev_i;
          ++count;
        }
        prev_r = hr; prev_i = hi;
      }
    }
  }
  if (pairs) *pairs = count;
  return den > 0 ? (phase_sensitive ? nr : hypot(nr, ni)) / den : 0;
}

double nr_pusch_ul_disc_score(const nr_pusch_ul_disc_grid_t *g, uint16_t mask,
                              int type, int nscid, int id, int *pairs)
{
  return score_impl(g, mask, type, nscid, id, pairs, false);
}

double nr_pusch_ul_disc_delay_score(const nr_pusch_ul_disc_grid_t *g, uint16_t mask,
                                    int type, int nscid, int id, int *pairs)
{
  return score_impl(g, mask, type, nscid, id, pairs, true);
}

void nr_pusch_ul_disc_score_batch(const nr_pusch_ul_disc_grid_t *g,
                                  const nr_pusch_ul_disc_hyp_t *h, int count,
                                  nr_pusch_ul_disc_metric_t *m)
{
  if (!h || !m || count < 0) return;
  for (int i = 0; i < count; ++i)
    m[i].score = score_impl(g, h[i].mask, h[i].dmrs_type, h[i].nscid,
                            h[i].id, &m[i].pairs, false);
}

bool nr_pusch_ul_disc_accept(double score, int pairs, double *bound)
{
  /* Conservative complex random-walk tail and union over the search family.
   * This is a design bound for independent circular Gaussian noise, not a
   * guarantee for correlated interference or a wrong grant. */
  const double p = pairs >= 128 && isfinite(score) && score > 0
      ? fmin(1.0, 2.0 * UL_DISC_FAMILY * exp(-0.25 * pairs * score * score)) : 1.0;
  if (bound) *bound = p;
  return p <= UL_DISC_ALPHA;
}


/* ---- Closed-form delay (G1), see the header ---------------------------------------------------------------------------------------------- */
#define UL_CF_MAX_NSYM 4
#define UL_CF_MAX_STAGES 14

static int cf_pairs(const int *ks, int n, int delta, int *pi, int *pj)
{
  int cnt = 0, j = 0;
  for (int i = 0; i < n; ++i) {
    if (j < i) j = i;
    while (j < n && ks[j] < ks[i] + delta) ++j;
    if (j < n && ks[j] == ks[i] + delta) { pi[cnt] = i; pj[cnt] = j; ++cnt; }
  }
  return cnt;
}

/* sum over `nvec` pilot vectors of |sum_m h_m exp(+j 2 pi d k_m / fft)|^2; step[] = exp(+j 2 pi d (k_{m+1}-k_m) / fft) precomputed by the caller's cache */
static double cf_power(const double *h /* re,im interleaved */, const int *ks, int n, int nvec, double d, int fft)
{
  const double w = 2.0 * M_PI * d / fft;
  int dk_c[6] = {0}; double cr[6], ci[6]; int nc = 0;
  double tot = 0;
  for (int v = 0; v < nvec; ++v) {
    const double *x = h + (size_t)v * n * 2;
    double wr = cos(w * ks[0]), wi = sin(w * ks[0]), sr = 0, si = 0;
    for (int m = 0; m < n; ++m) {
      sr += x[2 * m] * wr - x[2 * m + 1] * wi;
      si += x[2 * m] * wi + x[2 * m + 1] * wr;
      if (m + 1 < n) {
        const int dk = ks[m + 1] - ks[m];
        int c = 0;
        while (c < nc && dk_c[c] != dk) ++c;
        if (c == nc) {
          if (nc < 6) { dk_c[nc] = dk; cr[nc] = cos(w * dk); ci[nc] = sin(w * dk); ++nc; }
          else { cr[5] = cos(w * dk); ci[5] = sin(w * dk); dk_c[5] = dk; c = 5; }
        }
        const double t = wr * cr[c] - wi * ci[c];
        wi = wr * ci[c] + wi * cr[c];
        wr = t;
      }
    }
    tot += sr * sr + si * si;
  }
  return tot;
}

/* The per-grant estimator of the receive chain (comb 2 / DM-RS configuration type 1 branch of nr_ul_channel_estimation.c) re-run on the same descrambled pilots:
 * per antenna one LS value per pilot pair repeated on 4 packed bins, n-point transform, peak/mean > PEAK_DETECT_THRESHOLD, best antenna by peak power; 0 when no
 * antenna clears the threshold (the chain then asks for no correction). Same decision as nr_passive_est_delay (float transform instead of the fixed-point IDFT).
 * h: [vec][pilot] re,im; vector a*nsym is antenna a's first DM-RS symbol. Returns false when not applicable. */
static bool cf_old_delay(const double *h, int n, int nsym, int nrx, int fft, int *est)
{
  const int pairs = n / 2;
  if (fft < 256 || 4 * pairs > fft) return false;
  float *re = malloc(2 * (size_t)fft * sizeof(float)), *im = re + fft;
  if (!re) return false;
  double best_pk = -1; int best_est = 0;
  for (int a = 0; a < nrx; ++a) {
    const double *x = h + (size_t)(a * nsym) * n * 2;
    memset(re, 0, 2 * (size_t)fft * sizeof(float));
    for (int q = 0; q < pairs; ++q) {
      const float vr = (float)(x[4 * q] + x[4 * q + 2]), vi = (float)(x[4 * q + 1] + x[4 * q + 3]);
      for (int k = 0; k < 4; ++k) { re[4 * q + k] = vr; im[4 * q + k] = vi; }
    }
    nr_pdf_fft(re, im, fft);
    double pk = -1, sum = 0; int tb = 0;
    for (int t = 0; t < fft; ++t) { const double p = (double)re[t] * re[t] + (double)im[t] * im[t]; sum += p; if (p > pk) { pk = p; tb = t; } }
    const double mean = sum / fft;
    const int e = (mean > 0 && pk / mean > NR_PDF_THRESHOLD) ? (tb > fft / 2 ? tb - fft : tb) : 0;
    if (pk >= best_pk) { best_pk = pk; best_est = e; }
  }
  free(re);
  *est = best_est;
  return true;
}

bool nr_pusch_ul_disc_delay_closed(const nr_pusch_ul_disc_grid_t *g, uint16_t mask, int type, int nscid, int id, nr_pusch_ul_disc_delay_t *out)
{
  if (out) memset(out, 0, sizeof(*out));
  if (!out || !g || !mask || g->fft < 64 || g->fft > 8192 || g->n_rx < 1 || g->n_rx > 4 ||
      g->n_rb_carrier < 1 || g->n_rb_carrier > 275 || g->rb_count < 1 ||
      g->rb_start < 0 || g->rb_start + g->rb_count > g->n_rb_carrier ||
      g->symbols_per_slot < 1 || g->symbols_per_slot > 14 ||
      (mask >> g->symbols_per_slot) || type < 0 || type > 1 || nscid < 0 || nscid > 1 ||
      id < 0 || id > 65535) return false;
  for (int a = 0; a < g->n_rx; ++a) if (!g->rx[a]) return false;
  int syms[UL_CF_MAX_NSYM], nsym = 0;
  for (int sym = 0; sym < g->symbols_per_slot && nsym < UL_CF_MAX_NSYM; ++sym)
    if (mask & (1u << sym)) syms[nsym++] = sym;
  const int nprb = type ? 4 : 6, n = nprb * g->rb_count, ntot = nprb * (g->rb_start + g->rb_count);
  const int words = ((g->n_rb_carrier * 24) >> 5) + 1;
  uint32_t seq[208];
  c16_t pilot[1650];
  if (words > 208 || ntot > 1650 || n < 8) return false;
  const int nvec = g->n_rx * nsym;
  double *h = malloc(((size_t)nvec * n * 2) * sizeof(double) + (size_t)n * 3 * sizeof(int));
  if (!h) return false;
  int *ks = (int *)(h + (size_t)nvec * n * 2), *pi = ks + n, *pj = pi + n;
  for (int m = 0; m < n; ++m) {
    const int off = type ? 6 * (m / 2) + m % 2 : 4 * (m / 2) + 2 * (m % 2);
    const int bin = (g->first_carrier + 12 * g->rb_start + off) % g->fft;
    ks[m] = bin >= g->fft / 2 ? bin - g->fft : bin;
  }
  double etot = 0;
  bool ok = true;
  for (int s = 0; s < nsym && ok; ++s) {
    gold_for(seq, words, g->symbols_per_slot, g->slot, syms[s], id, nscid);
    nr_pdsch_dmrs_rx(g->normal_cp ? NR_NORMAL : NR_EXTENDED, seq, pilot, 1000, 0, (unsigned short)(g->rb_start + g->rb_count), type, 16384);
    const c16_t *p = pilot + nprb * g->rb_start;
    for (int a = 0; a < g->n_rx; ++a) {
      const c16_t *row = g->rx[a] + (size_t)syms[s] * g->fft;
      double *x = h + (size_t)(a * nsym + s) * n * 2;
      for (int m = 0; m < n; ++m) {
        const int off = type ? 6 * (m / 2) + m % 2 : 4 * (m / 2) + 2 * (m % 2);
        const c16_t y = row[(g->first_carrier + 12 * g->rb_start + off) % g->fft];
        const double pm = hypot((double)p[m].r, (double)p[m].i);
        if (pm <= 0) { ok = false; break; }
        /* nr_pdsch_dmrs_rx returns the RECEIVER-side (conjugated) sequence: the descrambled pilot is p * y, as in score_impl */
        x[2 * m] = ((double)p[m].r * y.r - (double)p[m].i * y.i) / pm;
        x[2 * m + 1] = ((double)p[m].r * y.i + (double)p[m].i * y.r) / pm;
        etot += x[2 * m] * x[2 * m] + x[2 * m + 1] * x[2 * m + 1];
      }
    }
  }
  if (!ok || etot <= 0) { free(h); return false; }
  const int fft = g->fft;
  double d = 0;
  int delta = type ? 1 : 2, stages = 0, delta0 = delta;
  while (stages < UL_CF_MAX_STAGES) {
    const int cnt = cf_pairs(ks, n, delta, pi, pj);
    if (cnt < (stages == 0 ? 4 : (n / 4 > 8 ? n / 4 : 8))) break;
    double zr = 0, zi = 0;
    for (int v = 0; v < nvec; ++v) {
      const double *x = h + (size_t)v * n * 2;
      for (int q = 0; q < cnt; ++q) {
        const double ar = x[2 * pj[q]], ai = x[2 * pj[q] + 1], br = x[2 * pi[q]], bi = x[2 * pi[q] + 1];
        zr += ar * br + ai * bi;           /* a conj(b) */
        zi += ai * br - ar * bi;
      }
    }
    const double th = 2.0 * M_PI * d * delta / fft; /* undo the delay already removed */
    const double rr = zr * cos(th) - zi * sin(th), ri = zr * sin(th) + zi * cos(th);
    const double step = -atan2(ri, rr) * fft / (2.0 * M_PI * delta);
    /* phase unwrapping: psi lies in (-pi, pi], so `step` is already the alias nearest to the previous estimate (the doubling ladder keeps the previous stage's
     * error below a quarter of this stage's alias spacing at the SNRs where the result is accepted) */
    d += step;
    ++stages;
    delta = (type && delta == 1) ? 6 : delta * 2;
  }
  if (stages == 0) { free(h); return false; }
  const double pw = cf_power(h, ks, n, nvec, d, fft);
  out->delay = d;
  out->ratio = pw / etot;
  out->dof = nsym; /* NOT nvec: with a real signal present and a wrong hypothesis (wrong id, or data read as DM-RS) the RX see the same pseudo-random sequence, so they do not average: one independent draw per DM-RS symbol (measured: the Gamma(nvec) null accepted a wrong id in 6-10 % of strong-signal trials at its 1e-6 bound). Noise-only input is lighter-tailed, so this null is conservative for it too. */
  out->pilots = n;
  out->stages = stages;
  out->trials = fmax(1.0, (double)(ks[n - 1] - ks[0]) / delta0);
  /* Second candidate: the answer of the receive chain's own per-grant estimator on the same pilots (comb 2 only). Both candidates are scored by the SAME
   * delay-domain power ratio (mean 1 for noise, so the comparison needs no threshold) and the better-verified one is kept, so the result is never worse than
   * either estimator alone. The choice is a maximum over two candidates: the union bound of the acceptance test counts both (trials x 2). */
  int eo = 0;
  if (type == 0 && cf_old_delay(h, n, nsym, g->n_rx, fft, &eo)) {
    const double ro = cf_power(h, ks, n, nvec, (double)eo, fft) / etot;
    out->old_delay = eo; out->old_ratio = ro;
    out->trials *= 2.0;
    if (ro > out->ratio) { out->delay = (double)eo; out->ratio = ro; out->used_old = true; }
  }
  out->valid = true;
  free(h);
  return true;
}

double nr_pusch_ul_disc_evidence_ln_bound(double sum_x, double sum_dof, double ln_trials_total, double family)
{
  if (!(sum_dof > 0) || !(sum_x > sum_dof)) return 0;
  const double ln = ln_trials_total + log(family > 1 ? family : 1) + sum_dof * (1.0 + log(sum_x / sum_dof)) - sum_x;
  return ln > 0 ? 0 : ln;
}

bool nr_pusch_ul_disc_delay_accept(const nr_pusch_ul_disc_delay_t *d, double family, double *bound)
{
  double ln = 0;
  if (d && d->valid && isfinite(d->ratio)) ln = nr_pusch_ul_disc_evidence_ln_bound(d->dof * d->ratio, d->dof, log(d->trials > 1 ? d->trials : 1), family);
  if (bound) *bound = exp(ln);
  return ln <= log(UL_DISC_ALPHA);
}

bool nr_pusch_ul_disc_id_evidence(uint16_t rnti, int id, int nscid, int type, const nr_pusch_ul_disc_delay_t *d, int64_t abs_slot, double family,
                                  int *grants, double *bound)
{
  if (grants) *grants = 0;
  if (bound) *bound = 1;
  if (!d || !d->valid || !isfinite(d->ratio) || id < 0 || id > 65535) return false;
  pthread_mutex_lock(&track_lock);
  ul_disc_track_t *t = track(rnti);
  int k = -1, free_k = -1, weak_k = 0;
  for (int i = 0; i < UL_DISC_EVIDENCE; ++i) {
    if (!t->ev[i].used) { if (free_k < 0) free_k = i; continue; }
    if (t->ev[i].id == id && t->ev[i].nscid == nscid && t->ev[i].type == type) k = i;
    if (t->ev[i].x - t->ev[i].dof < t->ev[weak_k].x - t->ev[weak_k].dof) weak_k = i;
  }
  if (k < 0) {
    k = free_k >= 0 ? free_k : weak_k;
    memset(&t->ev[k], 0, sizeof(t->ev[k]));
    t->ev[k].used = true; t->ev[k].id = id; t->ev[k].nscid = nscid; t->ev[k].type = type; t->ev[k].last_slot = INT64_MIN;
  }
  if (t->ev[k].last_slot != abs_slot) { /* the same slot seen twice is one observation */
    t->ev[k].x += d->dof * d->ratio;
    t->ev[k].dof += d->dof;
    t->ev[k].ln_trials += log(d->trials > 1 ? d->trials : 1);
    t->ev[k].grants++;
    t->ev[k].last_slot = abs_slot;
  }
  const double ln = nr_pusch_ul_disc_evidence_ln_bound(t->ev[k].x, t->ev[k].dof, t->ev[k].ln_trials, family);
  const bool confirmed = ln <= log(UL_DISC_ALPHA);
  if (grants) *grants = t->ev[k].grants;
  if (bound) *bound = exp(ln);
  if (confirmed && (!t->found.accepted || exp(ln) < t->found.false_alarm_bound)) {
    t->found = (nr_pusch_ul_disc_result_t){.id = id, .nscid = nscid, .dmrs_type = type, .mapping_type = -1, .add_pos = -1, .front_load = 0, .max_length = 0,
                                           .mask = 0, .coherence = d->ratio, .false_alarm_bound = exp(ln), .accepted = true, .fresh = true};
  }
  pthread_mutex_unlock(&track_lock);
  return confirmed;
}

static uint16_t observed_symbols(const nr_pusch_ul_disc_grid_t *g, int id, int nscid, int type)
{
  uint16_t mask = 0;
  nr_pusch_ul_disc_hyp_t h[14];
  nr_pusch_ul_disc_metric_t m[14];
  int n = 0;
  for (int sym = g->start_symbol; sym < g->start_symbol + g->num_symbols; ++sym)
    h[n++] = (nr_pusch_ul_disc_hyp_t){.mask=1u << sym, .id=id,
                                      .dmrs_type=type, .nscid=nscid};
  if (!n) return 0;
  nr_pusch_ul_disc_score_batch(g, h, n, m);
  for (int i = 0; i < n; ++i)
    if (nr_pusch_ul_disc_accept(m[i].score, m[i].pairs, NULL)) mask |= h[i].mask;
  return mask;
}

static void consider(const nr_pusch_ul_disc_grid_t *g, nr_pusch_ul_disc_result_t *best,
                     uint16_t observed, int id, int nscid, int type,
                     int mapping, int add, int len, int front_load, int type_a_pos)
{
  if (len != front_load) return;
  const int mask = nr_pdcch_blind_ul_dmrs_mask(g->num_symbols, g->start_symbol,
                                               mapping, add, len, type_a_pos);
  if (mask <= 0 || (uint16_t)mask != observed) return;
  int pairs = 0;
  const double score = nr_pusch_ul_disc_score(g, mask, type, nscid, id, &pairs);
  double p = 1;
  if (!nr_pusch_ul_disc_accept(score, pairs, &p)) return;
  if (!best->accepted || p < best->false_alarm_bound) {
    *best = (nr_pusch_ul_disc_result_t){.id=id, .nscid=nscid, .dmrs_type=type,
      .mapping_type=mapping, .add_pos=add, .front_load=len,
      .max_length=len == 2 ? 2 : 0, .mask=(uint16_t)mask,
      .coherence=score, .false_alarm_bound=p, .accepted=true, .fresh=true};
  } else if (best->id == id && best->nscid == nscid && best->dmrs_type == type &&
             best->mask == (uint16_t)mask && fabs(p - best->false_alarm_bound) < 1e-12) {
    /* Multiple RRC layouts can produce the same observed symbol mask. The
     * waveform then proves the mask, not the distinct ASN.1 configuration. */
    if (best->mapping_type != mapping) best->mapping_type = -1;
    if (best->add_pos != add) best->add_pos = -1;
  }
}

/* background discovery worker: the scan below covers [lo, lo+n) in one call instead of 64 ids per grant; the per-RNTI cursor is left alone. */
static __thread int t_sweep_lo, t_sweep_n;

int nr_pusch_ul_disc_known_ids(int *ids, int max)
{
  int n = 0;
  pthread_mutex_lock(&track_lock);
  for (int i = 0; i < UL_DISC_TRACKS && n < max; ++i)
    if (tracks[i].used && tracks[i].found.accepted) {
      bool seen = false;
      for (int k = 0; k < n; ++k) seen |= ids[k] == tracks[i].found.id;
      if (!seen) ids[n++] = tracks[i].found.id;
    }
  pthread_mutex_unlock(&track_lock);
  return n;
}

nr_pusch_ul_disc_result_t nr_pusch_ul_disc_search(const nr_pusch_ul_disc_grid_t *g,
                                                  uint16_t rnti, uint8_t type_a_pos,
                                                  const int *prior_ids, int npriors,
                                                  int preferred_type, int preferred_nscid,
                                                  uint8_t antenna_ports_field)
{
  nr_pusch_ul_disc_result_t best = {.id=-1, .false_alarm_bound=1};
  if (!g || g->symbols_per_slot < 1 || g->symbols_per_slot > 14 ||
      g->num_symbols <= 0 || g->num_symbols > 14 || g->start_symbol < 0 ||
      g->start_symbol + g->num_symbols > g->symbols_per_slot) return best;
  pthread_mutex_lock(&track_lock);
  ul_disc_track_t *t = track(rnti);
  const nr_pusch_ul_disc_result_t cached = t->found;
  const uint32_t cursor = t_sweep_n ? (uint32_t)t_sweep_lo : t->scan_cursor;
  const int scan_total = t_sweep_n ? t_sweep_n : UL_DISC_SCAN_PER_GRANT;
  if (!t_sweep_n)
    t->scan_cursor = cursor + UL_DISC_SCAN_PER_GRANT;
  pthread_mutex_unlock(&track_lock);
  int front_load[2] = {0,0};
  for (int type = 0; type < 2; ++type) {
    uint8_t cdm = 0;
    uint16_t ports = 0;
    const long one = 1;
    if (decode_dci_antenna_ports_val(1, type ? &one : NULL,
                                     NR_PUSCH_Config__transformPrecoder_disabled,
                                     antenna_ports_field, &cdm, &ports, &front_load[type]) != 0)
      front_load[type] = 0;
  }

  /* DL-known physical IDs first, full legal symbol-layout family. */
  for (int i = -1; i < npriors && i < 16; ++i) {
    if (i == -1 && !cached.accepted) continue;
    const int id = i == -1 ? cached.id : prior_ids[i];
    if (id < 0 || id > 65535) continue;
    for (int ns = 0; ns < 2; ++ns)
      for (int type = 0; type < 2; ++type) {
        const int first_ns = i == -1 ? cached.nscid : preferred_nscid;
        const int first_type = i == -1 ? cached.dmrs_type : preferred_type;
        const int nscid = ns == 0 ? first_ns : 1-first_ns;
        const int dmrs_type = type == 0 ? first_type : 1-first_type;
        if (!front_load[dmrs_type]) continue;
        const uint16_t observed = observed_symbols(g, id, nscid, dmrs_type);
        if (!observed) continue;
        for (int mapping = 0; mapping < 2; ++mapping)
          for (int add = 0; add < 4; ++add)
            for (int len = 1; len <= 2; ++len)
              consider(g, &best, observed, id, nscid, dmrs_type, mapping, add, len,
                       front_load[dmrs_type], type_a_pos);
        if (best.accepted) goto publish;
      }
  }
  if (!best.accepted) {
    /* A first DM-RS symbol is enough to reject almost every wrong ID, and it
     * avoids a 16-bit Cartesian product with all additional-position masks.
     * Type A begins at l0=2/3; type B begins at the allocation start. Once a
     * physical ID crosses the familywise gate, rank its complete layouts. */
    const int firsts[3] = {g->start_symbol, 2, 3};
    nr_pusch_ul_disc_hyp_t h[UL_DISC_SCAN_PER_GRANT * 12];
    nr_pusch_ul_disc_metric_t m[UL_DISC_SCAN_PER_GRANT * 12];
    for (int base = 0; base < scan_total && !best.accepted; base += UL_DISC_SCAN_PER_GRANT) {
    int n = 0;
    for (int k = base; k < base + UL_DISC_SCAN_PER_GRANT && k < scan_total; ++k)
      for (int ns = 0; ns < 2; ++ns)
        for (int type = 0; type < 2; ++type)
          for (int s = 0; s < 3; ++s) {
            if (!front_load[type]) continue;
            const int sym = firsts[s];
            if (sym < g->start_symbol || sym >= g->start_symbol + g->num_symbols ||
                (s && sym == firsts[0]) || (s == 2 && sym == firsts[1])) continue;
            h[n++] = (nr_pusch_ul_disc_hyp_t){.mask=1u << sym,
              .id=(cursor+k)&65535, .dmrs_type=type, .nscid=ns};
          }
    nr_pusch_ul_disc_score_batch(g, h, n, m);
    for (int i = 0; i < n; ++i) {
      if (!nr_pusch_ul_disc_accept(m[i].score, m[i].pairs, NULL)) continue;
      const int id=h[i].id, ns=h[i].nscid, type=h[i].dmrs_type;
      const uint16_t observed = observed_symbols(g, id, ns, type);
      for (int mapping = 0; mapping < 2; ++mapping)
        for (int add = 0; add < 4; ++add)
          for (int len = 1; len <= 2; ++len)
            consider(g, &best, observed, id, ns, type, mapping, add, len,
                     front_load[type], type_a_pos);
    }
    }
  }
publish:
  if (best.accepted) {
    if (cached.accepted && best.id == cached.id && best.nscid == cached.nscid &&
        best.dmrs_type == cached.dmrs_type) best.fresh = false;
    pthread_mutex_lock(&track_lock);
    t = track(rnti);
    if (!t->found.accepted || best.id == t->found.id ||
        best.false_alarm_bound < t->found.false_alarm_bound) t->found = best;
    pthread_mutex_unlock(&track_lock);
  }
  return best;
}

nr_pusch_ul_disc_result_t nr_pusch_ul_disc_search_sweep(const nr_pusch_ul_disc_grid_t *g, uint16_t rnti, uint8_t type_a_pos, const int *prior_ids,
                                                        int npriors, int preferred_type, int preferred_nscid, uint8_t antenna_ports_field,
                                                        int id_lo, int id_n)
{
  t_sweep_lo = id_lo;
  t_sweep_n = id_n > 0 ? id_n : 0;
  const nr_pusch_ul_disc_result_t r = nr_pusch_ul_disc_search(g, rnti, type_a_pos, prior_ids, npriors, preferred_type, preferred_nscid, antenna_ports_field);
  t_sweep_n = 0;
  return r;
}

/* Evidence that window k reads the pilots with a larger coherence than the default window 0, as a standard-normal-under-the-null score. R = ratio = mean over the
 * n_rx x dof independent vectors of |sum_m h_m e^{j..}|^2 / sum_m |h_m|^2; for n pilots of coherence kappa (signal share of the pilot energy) a vector has mean
 * n kappa + (1 - kappa) and variance (1 - kappa)(2 n kappa + (1 - kappa)) (complex Gaussian noise on a coherent sum), so kappa = (R - 1) / (n - 1) and the
 * difference of two windows is scored by the sum of their variances over the number of vectors (the windows overlap, which only makes the true variance of the
 * difference smaller: the score is conservative). A window with ISI (the wrong alias) has less coherence than the one at the symbol boundary. */
static double pilot_alias_score(const nr_pusch_ul_disc_delay_t *d0, const nr_pusch_ul_disc_delay_t *dk, int n_rx)
{
  const double n = (double)d0->pilots, nvec = (double)n_rx * (d0->dof > 0 ? d0->dof : 1);
  if (!(n > 1) || !(dk->pilots > 1)) return NAN;
  double v[2];
  const nr_pusch_ul_disc_delay_t *w[2] = {d0, dk};
  for (int i = 0; i < 2; ++i) {
    double kap = (w[i]->ratio - 1.0) / (w[i]->pilots - 1.0);
    kap = kap < 0 ? 0 : (kap > 1 ? 1 : kap);
    v[i] = ((1.0 - kap) * (2.0 * w[i]->pilots * kap + (1.0 - kap)) + 1.0) / nvec; /* +1: the variance of a noise-only vector, so that it never vanishes */
  }
  return (dk->ratio - d0->ratio) / sqrt(v[0] + v[1]);
}

int nr_pusch_ul_disc_alias_choose(const double *zcp, const nr_pusch_ul_disc_delay_t *dd, int n_rx, int n)
{
  int best = 0;
  double ub = -INFINITY;
  for (int k = 1; k < n; ++k) {
    double sum = 0;
    int cnt = 0;
    if (zcp && isfinite(zcp[0]) && isfinite(zcp[k])) { sum += (zcp[k] - zcp[0]) / M_SQRT2; ++cnt; } /* independent unit-variance CP scores: the difference has variance 2 */
    if (dd && dd[0].valid && dd[k].valid) {
      const double u = pilot_alias_score(&dd[0], &dd[k], n_rx);
      if (isfinite(u)) { sum += u; ++cnt; }
    }
    if (!cnt) continue;
    const double u = sum / sqrt((double)cnt); /* Stouffer: the two kinds of evidence are independent (CP samples vs the FFT of the window's pilots, to first order) */
    if (u > ub) { ub = u; best = k; }
  }
  /* a switch away from the default needs the normal tail bound exp(-Q^2/2) over the n - 1 alternatives below the alpha of every other decision of this file */
  const double q = sqrt(2.0 * log((double)(n > 1 ? n - 1 : 1) / UL_DISC_ALPHA));
  return (best > 0 && ub > q) ? best : 0;
}

void nr_pusch_ul_disc_timing_resolve(const nr_pusch_ul_disc_timing_in_t *in, nr_pusch_ul_disc_timing_out_t *out)
{
  memset(out, 0, sizeof(*out));
  const int hw = in->fft / 2; /* the comb-2 alias period (config type 1) */
  const int nwin = in->tracked ? 1 : 3;
  const int win[3] = {0, hw, -hw};
  for (int w = 0; w < nwin && !out->locked; ++w) {
    const int a0 = in->seed + win[w];
    if (a0 - in->ta_offset < -2048 || a0 - in->ta_offset > 2048) continue;
    if (!in->fep(in->ctx, a0)) continue;
    ++out->feps;
    int bi = -1;
    nr_pusch_ul_disc_delay_t best = {0};
    for (int i = 0; i < in->nprior; ++i) {
      nr_pusch_ul_disc_delay_t d;
      if (nr_pusch_ul_disc_delay_closed(in->grid, in->mask, in->type, in->nscid, in->prior[i], &d) && (bi < 0 || d.ratio > best.ratio)) { best = d; bi = i; }
    }
    if (bi < 0) continue;
    const double family = (in->cached_id >= 0 && in->prior[bi] == in->cached_id) ? 1.0 : (double)in->nprior;
    if (!nr_pusch_ul_disc_delay_accept(&best, family, NULL)) {
      if (w == 0) { out->undec = true; out->undec_idx = bi; out->undec_d = best; out->undec_a0 = a0; }
      continue;
    }
    const int a1 = a0 - (int)lrint(best.delay);
    int ta = a1;
    nr_pusch_ul_disc_delay_t v = best;
    bool vok = true;
    if (in->type == 0 && hw > 0) {
      /* Comb-2 pilots read the delay only modulo hw: the window that centres the arrival is a1 or one of its aliases a1 +- hw. The default is the candidate nearest
       * to the seed (the cell/RNTI prior and the answer of the former estimators, which also return the alias nearest to where they looked). It is replaced only
       * by evidence that does not depend on the alias-blind pilot comb: the window of the true symbol boundary has no inter-symbol interference, so its pilots are
       * the most coherent, and only there the cyclic prefix correlates with its copy (nr_pusch_ul_disc_alias_choose). Every candidate window is read once. */
      int cand[3] = {a1, a1 + hw, a1 - hw};
      for (int k = 1; k < 3; ++k)
        if (abs(cand[k] - in->seed) < abs(cand[0] - in->seed)) { const int t = cand[0]; cand[0] = cand[k]; cand[k] = t; }
      nr_pusch_ul_disc_delay_t dd[3];
      bool dv[3] = {false, false, false};
      double z[3] = {NAN, NAN, NAN};
      for (int k = 0; k < 3; ++k) {
        memset(&dd[k], 0, sizeof(dd[k]));
        if (cand[k] - in->ta_offset < -2048 || cand[k] - in->ta_offset > 2048) continue;
        if (in->cp_stat) {
          double m, e2;
          if (in->cp_stat(in->ctx, cand[k], &m, &e2) && e2 > 0) z[k] = m / sqrt(0.5 * e2);
        }
        if (!in->fep(in->ctx, cand[k])) continue;
        ++out->feps;
        dv[k] = nr_pusch_ul_disc_delay_closed(in->grid, in->mask, in->type, in->nscid, in->prior[bi], &dd[k]);
      }
      const int pick = nr_pusch_ul_disc_alias_choose(z, dd, in->grid->n_rx, 3);
      if (pick != 0) out->alias_switched = true;
      ta = cand[pick];
      v = dd[pick];
      vok = dv[pick] && nr_pusch_ul_disc_delay_accept(&v, family * (pick ? 3.0 : 1.0), NULL) && fabs(v.delay) <= 4.0;
    } else if (a1 != a0) {
      vok = in->fep(in->ctx, a1);
      if (vok) {
        ++out->feps;
        vok = nr_pusch_ul_disc_delay_closed(in->grid, in->mask, in->type, in->nscid, in->prior[bi], &v) && nr_pusch_ul_disc_delay_accept(&v, family, NULL) &&
              fabs(v.delay) <= 4.0;
      }
    }
    if (!vok) { ++out->vfail; continue; }
    out->locked = true;
    out->ta = ta;
    out->idx = bi;
    out->v = v;
  }
}
