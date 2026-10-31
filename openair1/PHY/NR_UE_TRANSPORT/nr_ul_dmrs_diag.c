/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* UL_DMRS_DIAG core, see nr_ul_dmrs_diag.h. */
#include "nr_ul_dmrs_diag.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define NT NR_UL_DMRS_DIAG_NTAU
#define NC NR_UL_DMRS_DIAG_NCOARSE
#define TAU0 (NT / 2)

/* prefix sums over the carrier PRBs: P[t * (nrb + 1) + rb], one S2 row */
typedef struct { double *C; double *S2; } stat_t;

typedef struct {
  const nr_ul_dmrs_diag_in_t *in;
  int nrb;
  float cs[NT][6], sn[NT][6];
  stat_t cur[2];       /* [control] scratch for the hypothesis in flight */
  c16_t *pil[2];
  c16_t *tmpF[NR_UL_DMRS_DIAG_MAX_ANT];
} ctx_t;

/* A comb of 2 cannot tell delays N/2 apart, and equal figures must give the smallest shift: scan the delays from zero outward,
 * a later one has to win by a margin. */
#define TIE 0.02 /* ~0.09 dB: below it two figures are the same measurement (the statistic saturates at nre), keep the nearer offset */
static int tau_order(int i) { return TAU0 + ((i & 1) ? (i + 1) / 2 : -(i / 2)); }

static float to_db(double r) { return r > 0.0 ? (float)(10.0 * log10(r)) : -99.f; }

static void stat_alloc(stat_t *s, int nrb)
{
  s->C = (double *)calloc((size_t)NT * (nrb + 1), sizeof(double));
  s->S2 = (double *)calloc((size_t)(nrb + 1), sizeof(double));
}
static void stat_free(stat_t *s) { free(s->C); free(s->S2); s->C = s->S2 = NULL; }
static bool stat_ok(const stat_t *s) { return s->C && s->S2; }

/* Per-PRB C(tau) and S2 of the antenna frequency vectors F[ant][fft] against the pilots, then the prefix sums. */
static void stat_compute(const ctx_t *x, c16_t *const *F, const c16_t *pil, stat_t *s)
{
  const nr_ul_dmrs_diag_in_t *in = x->in;
  const int nrb = x->nrb, nre = in->nre, N = in->fft;
  double cacc = 0, sacc = 0;
  double cpre[NT];
  for (int t = 0; t < NT; t++) {
    cpre[t] = 0;
    s->C[(size_t)t * (nrb + 1)] = 0;
  }
  s->S2[0] = 0;
  for (int rb = 0; rb < nrb; rb++) {
    float c[NT] = {0}, s2 = 0.f;
    for (int a = 0; a < in->nant; a++) {
      float ar[NT] = {0}, ai[NT] = {0};
      for (int j = 0; j < nre; j++) {
        const c16_t y = F[a][(in->first_carrier + 12 * rb + in->re_off[j]) % N];
        const c16_t p = pil[nre * rb + j];
        const float zr = (float)y.r * p.r - (float)y.i * p.i, zi = (float)y.r * p.i + (float)y.i * p.r;
        s2 += zr * zr + zi * zi;
        for (int t = 0; t < NT; t++) {
          ar[t] += zr * x->cs[t][j] + zi * x->sn[t][j];
          ai[t] += zi * x->cs[t][j] - zr * x->sn[t][j];
        }
      }
      for (int t = 0; t < NT; t++) c[t] += ar[t] * ar[t] + ai[t] * ai[t];
    }
    sacc += s2;
    s->S2[rb + 1] = sacc;
    for (int t = 0; t < NT; t++) {
      cpre[t] += c[t];
      s->C[(size_t)t * (nrb + 1) + rb + 1] = cpre[t];
    }
  }
  (void)cacc;
}

/* R = sum C / sum S2 over the PRB range [r0, r1), maximised over tau. -1 when empty. */
static double range_best(const ctx_t *x, const stat_t *s, int r0, int r1, int *t_best)
{
  const int w = x->nrb + 1;
  const double s2 = s->S2[r1] - s->S2[r0];
  if (r1 <= r0 || !(s2 > 0.0)) return -1.0;
  double best = -1.0;
  int tb = TAU0;
  for (int i = 0; i < NT; i++) {
    const int t = tau_order(i);
    const double c = s->C[(size_t)t * w + r1] - s->C[(size_t)t * w + r0];
    if (c / s2 > best * (1.0 + TIE)) { best = c / s2; tb = t; }
  }
  if (t_best) *t_best = tb;
  return best;
}

/* the same for "BWP [b0, b1) without [c0, c1)" */
static double rest_best(const ctx_t *x, const stat_t *s, int b0, int b1, int c0, int c1)
{
  const int w = x->nrb + 1;
  const double s2 = (s->S2[b1] - s->S2[b0]) - (s->S2[c1] - s->S2[c0]);
  if (!(s2 > 0.0)) return -1.0;
  double best = -1.0;
  for (int t = 0; t < NT; t++) {
    const double c = (s->C[(size_t)t * w + b1] - s->C[(size_t)t * w + b0]) - (s->C[(size_t)t * w + c1] - s->C[(size_t)t * w + c0]);
    if (c / s2 > best) best = c / s2;
  }
  return best;
}

/* best window of n PRBs anywhere in [b0, b1) */
static double window_best(const ctx_t *x, const stat_t *s, int b0, int b1, int n, int o_nom, int *o_best, int *t_best)
{
  /* ranked by the excess coherent energy X = C - S2 (zero for noise, additive over PRBs, so a window holding only part of the
   * signal scores lower than the full one even where the ratio R saturates); the returned figure is R of the winner */
  const int w = x->nrb + 1;
  double bestx = -1e300, bestr = -1.0;
  for (int oi = 0; oi < 2 * x->nrb + 2; oi++) {
    const int o = o_nom + ((oi & 1) ? (oi + 1) / 2 : -(oi / 2));
    if (o < b0 || o + n > b1) continue;
    const double s2 = s->S2[o + n] - s->S2[o];
    if (!(s2 > 0.0)) continue;
    for (int i = 0; i < NT; i++) {
      const int t = tau_order(i);
      const double c = s->C[(size_t)t * w + o + n] - s->C[(size_t)t * w + o];
      const double xx = c - s2;
      if (xx > bestx + TIE * (bestx > 0 ? bestx : 0.0) + 1e-9) { bestx = xx; bestr = c / s2; if (o_best) *o_best = o; if (t_best) *t_best = t; }
    }
  }
  return bestr;
}

static float snr_of(double r, int nre)
{
  if (!(r > 1.0)) return -99.f;
  if (r >= nre) return 99.f;
  return to_db((r - 1.0) / ((double)nre - r));
}

bool nr_ul_dmrs_diag_run(const nr_ul_dmrs_diag_in_t *in, nr_ul_dmrs_diag_out_t *out)
{
  if (!out) return false;
  memset(out, 0, sizeof(*out));
  out->e_claim_db = out->e_unclaim_db = out->e_floor_db = out->snr_claim_db = out->pw_exc_db = NAN;
  out->e_t_db = out->fl_t_db = out->e_rb_db = out->fl_rb_db = out->e_sym_db = out->fl_sym_db = out->e_slot_db = out->fl_slot_db = NAN;
  if (!in || !in->fep || in->fft < 128 || in->nant < 1 || in->nant > NR_UL_DMRS_DIAG_MAX_ANT || in->nrb_carrier < 1
      || in->nrb_carrier > NR_UL_DMRS_DIAG_MAX_RB || in->symbols_per_slot < 1 || in->symbols_per_slot > NR_UL_DMRS_DIAG_MAX_SYM
      || (in->nre != 4 && in->nre != 6) || in->claim_n < 1 || in->claim_start < 0 || in->claim_start + in->claim_n > in->nrb_carrier
      || in->dmrs_sym < 0 || in->dmrs_sym >= in->symbols_per_slot)
    return false;
  const int N = in->fft, nre = in->nre, nrb = in->nrb_carrier, nant = in->nant, sps = in->symbols_per_slot;
  const int b0 = in->bwp_size > 0 && in->bwp_start >= 0 && in->bwp_start < nrb ? in->bwp_start : 0;
  int b1 = in->bwp_size > 0 ? b0 + in->bwp_size : nrb;
  if (b1 > nrb) b1 = nrb;
  const int c0 = in->claim_start, c1 = in->claim_start + in->claim_n;
  const bool corr = in->pilot != NULL;

  ctx_t x;
  memset(&x, 0, sizeof(x));
  x.in = in;
  x.nrb = nrb;
  for (int t = 0; t < NT; t++)
    for (int j = 0; j < 6 && j < nre; j++) {
      const double ph = 2.0 * M_PI * (double)in->re_off[j] * (double)(t - TAU0) / 128.0;
      x.cs[t][j] = (float)cos(ph);
      x.sn[t][j] = (float)sin(ph);
    }
  c16_t *Fsym = NULL;           /* [sps][nant][N] */
  bool *avail = NULL;
  stat_t Ct[NC][2];             /* per coarse step, [control] */
  memset(Ct, 0, sizeof(Ct));
  bool ct_ok[NC] = {false};
  bool ret = false;
  const size_t fbytes = (size_t)sps * nant * N * sizeof(c16_t);
  if (posix_memalign((void **)&Fsym, 32, fbytes) != 0) Fsym = NULL;
  avail = (bool *)calloc((size_t)sps, sizeof(bool));
  for (int a = 0; a < nant; a++)
    if (posix_memalign((void **)&x.tmpF[a], 32, (size_t)N * sizeof(c16_t)) != 0) x.tmpF[a] = NULL;
  for (int k = 0; k < 2; k++) {
    x.pil[k] = (c16_t *)calloc((size_t)nre * nrb, sizeof(c16_t));
    stat_alloc(&x.cur[k], nrb);
  }
  bool mem = Fsym && avail && x.pil[0] && x.pil[1] && stat_ok(&x.cur[0]) && stat_ok(&x.cur[1]);
  for (int a = 0; a < nant; a++) mem = mem && x.tmpF[a];
  for (int c = 0; c < NC && mem; c++)
    for (int k = 0; k < 2; k++) {
      stat_alloc(&Ct[c][k], nrb);
      mem = mem && stat_ok(&Ct[c][k]);
    }
  if (!mem) goto done;
  memset(Fsym, 0, fbytes);

  /* ---- stage A: every symbol of the nominal slot at the decode's own timing (symbol search, nominal figures, powers) ---- */
  float e_sym_best = -1e30f, fl_sym_best = -1e30f, e_sym[NR_UL_DMRS_DIAG_MAX_SYM];
  for (int s = 0; s < NR_UL_DMRS_DIAG_MAX_SYM; s++) e_sym[s] = -1e30f;
  int sym_best = in->dmrs_sym;
  for (int s = 0; s < sps; s++) {
    bool ok = true;
    for (int a = 0; a < nant && ok; a++) ok = in->fep(in->u, a, 0, s, 0, Fsym + ((size_t)s * nant + a) * N);
    avail[s] = ok;
  }
  if (!avail[in->dmrs_sym]) goto done;
  out->valid = true;
  c16_t *Fp[NR_UL_DMRS_DIAG_MAX_ANT];
  {
    /* sequence-independent power figures over the grant's data symbols */
    double pc = 0, pu = 0, nc = 0, nu = 0;
    for (int s = in->grant_first_sym; s < in->grant_first_sym + in->grant_n_sym && s < sps; s++) {
      if (s < 0 || !avail[s] || ((in->dmrs_mask >> s) & 1u)) continue;
      for (int a = 0; a < nant; a++) {
        const c16_t *f = Fsym + ((size_t)s * nant + a) * N;
        for (int rb = b0; rb < b1; rb++) {
          double e = 0;
          for (int r = 0; r < 12; r++) {
            const c16_t y = f[(in->first_carrier + 12 * rb + r) % N];
            e += (double)y.r * y.r + (double)y.i * y.i;
          }
          if (rb >= c0 && rb < c1) { pc += e; nc += 12; } else { pu += e; nu += 12; }
        }
      }
    }
    if (nc > 0 && nu > 0 && pu > 0 && pc > 0) out->pw_exc_db = to_db((pc / nc) / (pu / nu));
  }
  if (corr) {
    for (int s = 0; s < sps; s++) {
      if (!avail[s]) continue;
      for (int a = 0; a < nant; a++) Fp[a] = Fsym + ((size_t)s * nant + a) * N;
      double r[2];
      for (int k = 0; k < 2; k++) {
        r[k] = -1.0;
        if (!in->pilot(in->u, 0, s, k == 1, x.pil[k])) continue;
        stat_compute(&x, Fp, x.pil[k], &x.cur[k]);
        r[k] = range_best(&x, &x.cur[k], c0, c1, NULL);
        if (s == in->dmrs_sym) {
          /* nominal figures; this symbol's stats also seed the coarse step 0 below */
          stat_t tmp = Ct[NC / 2][k];
          Ct[NC / 2][k] = x.cur[k];
          x.cur[k] = tmp;
        }
      }
      if (r[0] > 0) e_sym[s] = to_db(r[0]);
      if (r[1] > 0 && to_db(r[1]) > fl_sym_best) fl_sym_best = to_db(r[1]);
    }
    ct_ok[NC / 2] = true;
    e_sym_best = e_sym[in->dmrs_sym];
    for (int s = 0; s < sps; s++)
      if (e_sym[s] > e_sym_best + 0.1f) { e_sym_best = e_sym[s]; sym_best = s; }
    out->best_sym_off = sym_best - in->dmrs_sym;
    out->e_sym_db = e_sym_best > -1e29f ? e_sym_best : NAN;
    out->fl_sym_db = fl_sym_best > -1e29f ? fl_sym_best : NAN;
    const double rc = range_best(&x, &Ct[NC / 2][0], c0, c1, NULL), rf = range_best(&x, &Ct[NC / 2][1], c0, c1, NULL);
    out->e_claim_db = to_db(rc);
    out->e_floor_db = to_db(rf);
    out->snr_claim_db = snr_of(rc, nre);
    const double ru = rest_best(&x, &Ct[NC / 2][0], b0, b1, c0 > b0 ? (c0 < b1 ? c0 : b1) : b0, c1 < b1 ? (c1 > b0 ? c1 : b0) : b1);
    out->e_unclaim_db = ru > 0 ? to_db(ru) : NAN;

    /* ---- stage B: the window start (coarse steps of N/4, the fine delay grid joins them) ---- */
    double t_best = -1, t_fl = -1, rb_best = -1, rb_fl = -1;
    int t_best_off = 0, rb_off = 0, rb_toff = 0;
    for (int ci = 0; ci < NC; ci++) {
      const int c = NC / 2 + ((ci & 1) ? (ci + 1) / 2 : -(ci / 2));
      const int adv = (c - NC / 2) * (N / 4);
      if (c != NC / 2) {
        bool ok = true;
        for (int a = 0; a < nant && ok; a++) ok = in->fep(in->u, a, 0, in->dmrs_sym, adv, x.tmpF[a]);
        if (!ok) continue;
        for (int k = 0; k < 2; k++) {
          if (!in->pilot(in->u, 0, in->dmrs_sym, k == 1, x.pil[k])) { ok = false; break; }
          stat_compute(&x, x.tmpF, x.pil[k], &Ct[c][k]);
        }
        if (!ok) continue;
        ct_ok[c] = true;
      }
      for (int k = 0; k < 2; k++) {
        int tb = TAU0, ob = c0, tw = TAU0;
        const double rt = range_best(&x, &Ct[c][k], c0, c1, &tb);
        double rw = -1;
        if (c1 - c0 <= b1 - b0) rw = window_best(&x, &Ct[c][k], b0, b1, c1 - c0, c0, &ob, &tw);
        if (k == 0) {
          const int to = adv + (tb - TAU0) * (N / 128), ro = adv + (tw - TAU0) * (N / 128);
          /* a comb of 2 repeats every N/2 samples: equal figures keep the offset nearest to zero */
          if (rt > t_best * (1.0 + TIE) || (rt >= t_best * (1.0 - TIE) && abs(to) < abs(t_best_off))) { t_best = rt > t_best ? rt : t_best; t_best_off = to; }
          if (rw > rb_best * (1.0 + TIE) || (rw >= rb_best * (1.0 - TIE) && abs(ro) + 4 * abs(ob - c0) < abs(rb_toff) + 4 * abs(rb_off))) {
            rb_best = rw > rb_best ? rw : rb_best; rb_off = ob - c0; rb_toff = ro;
          }
        } else {
          if (rt > t_fl) t_fl = rt;
          if (rw > rb_fl) rb_fl = rw;
        }
      }
    }
    out->best_t_off = t_best_off;
    out->e_t_db = t_best > 0 ? to_db(t_best) : NAN;
    out->fl_t_db = t_fl > 0 ? to_db(t_fl) : NAN;
    out->best_rb_off = rb_off;
    out->best_rb_t_off = rb_toff;
    out->e_rb_db = rb_best > 0 ? to_db(rb_best) : NAN;
    out->fl_rb_db = rb_fl > 0 ? to_db(rb_fl) : NAN;

    /* ---- stage C: the slot (-1, 0, +1) at the decode's timing ---- */
    float e_slot = to_db(rc), fl_slot = to_db(rf);
    int slot_best = 0;
    for (int sd = -1; sd <= 1; sd += 2) {
      bool ok = true;
      for (int a = 0; a < nant && ok; a++) ok = in->fep(in->u, a, sd, in->dmrs_sym, 0, x.tmpF[a]);
      if (!ok) continue;
      double r[2] = {-1, -1};
      for (int k = 0; k < 2; k++) {
        if (!in->pilot(in->u, sd, in->dmrs_sym, k == 1, x.pil[k])) continue;
        stat_compute(&x, x.tmpF, x.pil[k], &x.cur[k]);
        r[k] = range_best(&x, &x.cur[k], c0, c1, NULL);
      }
      if (r[0] > 0 && to_db(r[0]) > e_slot + 0.1f) { e_slot = to_db(r[0]); slot_best = sd; }
      if (r[1] > 0 && to_db(r[1]) > fl_slot) fl_slot = to_db(r[1]);
    }
    out->best_slot_off = slot_best;
    out->e_slot_db = e_slot;
    out->fl_slot_db = fl_slot;
    out->have_corr = true;
  }
  ret = true;
done:
  free(Fsym);
  free(avail);
  for (int a = 0; a < nant; a++) free(x.tmpF[a]);
  for (int k = 0; k < 2; k++) {
    free(x.pil[k]);
    stat_free(&x.cur[k]);
  }
  for (int c = 0; c < NC; c++)
    for (int k = 0; k < 2; k++) stat_free(&Ct[c][k]);
  (void)ct_ok;
  return ret;
}
