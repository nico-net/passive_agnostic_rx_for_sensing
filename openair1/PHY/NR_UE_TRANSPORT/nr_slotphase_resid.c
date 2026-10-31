/* SLOTPHASE residual estimator: see nr_slotphase_resid.h. Pure functions. */
#include "nr_slotphase_resid.h"
#include "nr_re_pattern.h"
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define RHO_CAP 1.0e3 /* one wild grant cannot own a bin's variance */

static int cmp_f(const void *a, const void *b)
{
  const float x = *(const float *)a, y = *(const float *)b;
  return x < y ? -1 : x > y ? 1 : 0;
}
static int cmp_d(const void *a, const void *b)
{
  const double x = *(const double *)a, y = *(const double *)b;
  return x < y ? -1 : x > y ? 1 : 0;
}

/* Acklam's rational approximation of the standard normal quantile (relative error 1e-9) */
double nr_spr_norm_quantile(double p)
{
  static const double a[] = {-3.969683028665376e+01, 2.209460984245205e+02, -2.759285104469687e+02, 1.383577518672690e+02, -3.066479806614716e+01, 2.506628277459239e+00};
  static const double b[] = {-5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02, 6.680131188771972e+01, -1.328068155288572e+01};
  static const double c[] = {-7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00, -2.549732539343734e+00, 4.374664141464968e+00, 2.938163982698783e+00};
  static const double d[] = {7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00, 3.754408661907416e+00};
  if (!(p > 0.0)) return -INFINITY;
  if (!(p < 1.0)) return INFINITY;
  const double pl = 0.02425;
  double q, r;
  if (p < pl) {
    q = sqrt(-2.0 * log(p));
    return (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) / ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
  }
  if (p > 1.0 - pl) {
    q = sqrt(-2.0 * log(1.0 - p));
    return -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) / ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
  }
  q = p - 0.5;
  r = q * q;
  return (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q / (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1.0);
}

bool nr_spr_add(nr_spr_tab_t *t, const float e[][NR_SPR_N], const uint32_t n[][NR_SPR_N])
{
  float eb[NR_SPR_NSYM][NR_SPR_N], tmp[NR_SPR_NSYM * NR_SPR_KEYS];
  int nv = 0;
  for (int s = 0; s < NR_SPR_NSYM; s++)
    for (int k = 0; k < NR_SPR_N; k++) {
      const uint32_t need = k < NR_SPR_KEYS ? NR_SPR_MIN_RE : NR_SPR_MIN_RE_BKT;
      eb[s][k] = n[s][k] >= need && isfinite(e[s][k]) ? e[s][k] / (float)n[s][k] : -1.f;
      if (k < NR_SPR_KEYS && eb[s][k] >= 0.f) tmp[nv++] = eb[s][k];
    }
  if (nv < NR_SPR_MIN_BINS) return false;
  qsort(tmp, (size_t)nv, sizeof(float), cmp_f);
  const double base = nv & 1 ? tmp[nv / 2] : 0.5 * ((double)tmp[nv / 2 - 1] + tmp[nv / 2]);
  if (!(base > 0.0) || !isfinite(base)) return false;
  for (int s = 0; s < NR_SPR_NSYM; s++)
    for (int k = 0; k < NR_SPR_N; k++) {
      if (eb[s][k] < 0.f) continue;
      double rho = (double)eb[s][k] / base;
      if (rho > RHO_CAP) rho = RHO_CAP;
      nr_spr_acc_t *a = &t->a[s][k];
      a->n += 1.0;
      a->s += rho;
      a->q += rho * rho;
    }
  t->grants++;
  return true;
}

/* one-sided Welch z of f over r (excess = mean f - mean r); false when either side has too few grants */
static bool welch(const nr_spr_acc_t *f, const nr_spr_acc_t *r, double min_g, double *z, double *ex)
{
  if (f->n < min_g || r->n < min_g) return false;
  const double mf = f->s / f->n, mr = r->s / r->n;
  double vf = (f->q - f->s * f->s / f->n) / (f->n - 1.0), vr = (r->q - r->s * r->s / r->n) / (r->n - 1.0);
  const double fl = 1e-6 * (mf * mf + mr * mr) + 1e-12; /* identical grants must not give an infinite z */
  if (!(vf > fl)) vf = fl;
  if (!(vr > fl)) vr = fl;
  const double se = sqrt(vf / f->n + vr / r->n);
  *z = (mf - mr) / se;
  *ex = mf - mr;
  return true;
}
static nr_spr_acc_t sum2(const nr_spr_acc_t *a, const nr_spr_acc_t *b)
{
  nr_spr_acc_t r = {a->n + b->n, a->s + b->s, a->q + b->q};
  return r;
}

int nr_spr_estimate(const nr_spr_tab_t *fail, const nr_spr_tab_t *ra, const nr_spr_tab_t *rb, double alpha, nr_spr_result_t *out)
{
  memset(out, 0, sizeof(*out));
  if (!fail || !ra || !rb || !(alpha > 0.0 && alpha < 1.0)) return 0;
  double zz[NR_SPR_NSYM][NR_SPR_KEYS], xx[NR_SPR_NSYM][NR_SPR_KEYS];
  bool tst[NR_SPR_NSYM][NR_SPR_KEYS], sig[NR_SPR_NSYM][NR_SPR_KEYS];
  double exs[NR_SPR_NSYM * NR_SPR_KEYS];
  int nt = 0;
  memset(sig, 0, sizeof(sig));
  for (int s = 0; s < NR_SPR_NSYM; s++)
    for (int k = 0; k < NR_SPR_KEYS; k++) {
      const nr_spr_acc_t ref = sum2(&ra->a[s][k], &rb->a[s][k]);
      tst[s][k] = welch(&fail->a[s][k], &ref, NR_SPR_MIN_G, &zz[s][k], &xx[s][k]);
      if (tst[s][k]) exs[nt++] = xx[s][k];
      double zn, en;
      if (welch(&ra->a[s][k], &rb->a[s][k], 6.0, &zn, &en) && fabs(zn) > out->znull) out->znull = fabs(zn);
      if (welch(&rb->a[s][k], &ra->a[s][k], 6.0, &zn, &en) && fabs(zn) > out->znull) out->znull = fabs(zn);
    }
  for (int s = 0; s < NR_SPR_NSYM; s++) {
    double cov = 0.0;
    for (int k = 0; k < NR_SPR_KEYS; k++)
      if (fail->a[s][k].n > cov) cov = fail->a[s][k].n;
    if (cov > 0.0 && cov < 2.0 * NR_SPR_MIN_G) out->undersampled |= (uint16_t)(1u << s);
  }
  if (nt < 8) return 0;
  const double zb = nr_spr_norm_quantile(1.0 - alpha / (double)nt);
  out->zthr = zb > out->znull ? zb : out->znull;
  /* Otsu's two-class split of the excesses: foreign REs are the upper class; a bin must be in it as well as significant */
  {
    qsort(exs, (size_t)nt, sizeof(double), cmp_d);
    double tot = 0.0;
    for (int i = 0; i < nt; i++) tot += exs[i];
    double best = -1.0, cum = 0.0;
    for (int c = 1; c < nt; c++) {
      cum += exs[c - 1];
      const double m0 = cum / c, m1 = (tot - cum) / (nt - c), v = (double)c * (nt - c) * (m1 - m0) * (m1 - m0);
      if (v > best) {
        best = v;
        out->otsu = 0.5 * (exs[c - 1] + exs[c]);
      }
    }
  }
  uint16_t seed_sym = 0;
  for (int s = 0; s < NR_SPR_NSYM; s++)
    for (int k = 0; k < NR_SPR_KEYS; k++)
      if (tst[s][k] && zz[s][k] > out->zthr && xx[s][k] >= out->otsu && xx[s][k] > 0.0) {
        sig[s][k] = true;
        out->nseed++;
        seed_sym |= (uint16_t)(1u << s);
      }
  if (out->nseed == 0) return 0;
  /* hysteresis: inside the seed symbols the remaining bins only face the Bonferroni quantile of their own number */
  int nb2 = 0;
  for (int s = 0; s < NR_SPR_NSYM; s++)
    if (seed_sym >> s & 1)
      for (int k = 0; k < NR_SPR_KEYS; k++) nb2 += tst[s][k];
  out->zthr2 = nr_spr_norm_quantile(1.0 - alpha / (double)(nb2 > 0 ? nb2 : 1));
  if (out->zthr2 > out->zthr) out->zthr2 = out->zthr;
  for (int s = 0; s < NR_SPR_NSYM; s++)
    if (seed_sym >> s & 1)
      for (int k = 0; k < NR_SPR_KEYS; k++)
        if (tst[s][k] && !sig[s][k] && zz[s][k] > out->zthr2 && xx[s][k] >= out->otsu && xx[s][k] > 0.0) sig[s][k] = true;
  for (int s = 0; s < NR_SPR_NSYM; s++) {
    if (!(seed_sym >> s & 1)) continue;
    int ntest = 0, nsg = 0;
    for (int k = 0; k < NR_SPR_KEYS; k++) {
      ntest += tst[s][k];
      nsg += sig[s][k];
    }
    if (ntest >= 24 && nsg * 10 >= ntest * 9) {
      out->sym_full |= (uint16_t)(1u << s);
      continue;
    }
    for (int k = 0; k < NR_SPR_KEYS; k++)
      if (sig[s][k]) {
        out->nsig++;
        out->sig_sc[s][k / 12] |= (uint16_t)(1u << (k % 12));
      }
  }
  /* patterns: per symbol the sc mask of each CRB parity = both mod-4 classes of it; equal (even, odd) pairs share a symbol mask */
  struct { uint32_t sym; uint16_t sc; uint8_t dens; } pp[2 * NR_SPR_NSYM];
  int np = 0;
  for (int s = 0; s < NR_SPR_NSYM; s++) {
    const uint16_t *c = out->sig_sc[s];
    const uint16_t a0 = (uint16_t)(c[0] & c[2]), a1 = (uint16_t)(c[1] & c[3]);
    out->mod4_left += __builtin_popcount((unsigned)((c[0] | c[2]) & ~a0)) + __builtin_popcount((unsigned)((c[1] | c[3]) & ~a1));
    uint16_t sc[3] = {0, 0, 0}; /* dens 0, dens 1, dens 2 */
    if (a0 && a0 == a1) sc[2] = a0;
    else {
      sc[0] = a0;
      sc[1] = a1;
    }
    for (int d = 0; d < 3; d++) {
      if (!sc[d]) continue;
      int at = -1;
      for (int q = 0; q < np && at < 0; q++)
        if (pp[q].sc == sc[d] && pp[q].dens == d) at = q;
      if (at < 0) {
        at = np++;
        pp[at].sym = 0;
        pp[at].sc = sc[d];
        pp[at].dens = (uint8_t)d;
      }
      pp[at].sym |= 1u << s;
    }
  }
  if (np == 0) return 0;
  uint32_t psym = 0;
  for (int q = 0; q < np; q++) psym |= pp[q].sym;
  /* CRB range. A bucket (8 CRBs) of a pattern symbol is ON when its residual is in excess, OFF only on evidence: enough FAIL grants cover it and its mean is significantly below
   * half of the typical ON excess of that symbol; anything else is UNKNOWN (few grants cover it: it keeps the resource's extent). The range is the ON hull, extended outward over
   * UNKNOWN buckets up to the first OFF bucket or the band edge. */
  int lo = -1, hi = -1;
  int state[NR_SPR_BKT]; /* 0 unknown, 1 on, -1 off */
  memset(state, 0, sizeof(state));
  {
    const int nsy = __builtin_popcount(psym);
    const double zbk = nr_spr_norm_quantile(1.0 - alpha / (double)(nsy * NR_SPR_BKT));
    double on_ex[NR_SPR_NSYM];
    for (int s = 0; s < NR_SPR_NSYM; s++) {
      on_ex[s] = 0.0;
      if (!(psym >> s & 1)) continue;
      double xs[NR_SPR_BKT];
      int nx = 0;
      for (int b = 0; b < NR_SPR_BKT; b++) {
        const nr_spr_acc_t ref = sum2(&ra->a[s][NR_SPR_KEYS + b], &rb->a[s][NR_SPR_KEYS + b]);
        double z, ex;
        if (welch(&fail->a[s][NR_SPR_KEYS + b], &ref, NR_SPR_MIN_G, &z, &ex) && z > zbk && ex > 0.0) {
          state[b] = 1;
          xs[nx++] = ex;
        }
      }
      if (nx) {
        qsort(xs, (size_t)nx, sizeof(double), cmp_d);
        on_ex[s] = xs[nx / 2];
      }
    }
    for (int b = 0; b < NR_SPR_BKT; b++) {
      if (state[b] == 1) continue;
      bool off = false, contra = false;
      for (int s = 0; s < NR_SPR_NSYM; s++) {
        if (!(psym >> s & 1) || !(on_ex[s] > 0.0)) continue;
        const nr_spr_acc_t *f = &fail->a[s][NR_SPR_KEYS + b];
        const nr_spr_acc_t ref = sum2(&ra->a[s][NR_SPR_KEYS + b], &rb->a[s][NR_SPR_KEYS + b]);
        if (f->n < NR_SPR_MIN_G || ref.n < NR_SPR_MIN_G) continue;
        const double mf = f->s / f->n, mr = ref.s / ref.n;
        double vf = (f->q - f->s * f->s / f->n) / (f->n - 1.0), vr = (ref.q - ref.s * ref.s / ref.n) / (ref.n - 1.0);
        const double fl = 1e-6 * (mf * mf + mr * mr) + 1e-12;
        if (!(vf > fl)) vf = fl;
        if (!(vr > fl)) vr = fl;
        const double zoff = (mf - (mr + 0.5 * on_ex[s])) / sqrt(vf / f->n + vr / ref.n);
        if (zoff < -zbk) off = true;
        else contra = true;
      }
      if (off && !contra) state[b] = -1;
    }
    for (int b = 0; b < NR_SPR_BKT; b++)
      if (state[b] == 1) {
        if (lo < 0) lo = b;
        hi = b;
      }
  }
  if (lo < 0) {
    lo = 0;
    hi = NR_SPR_BKT - 1;
  } else {
    while (lo > 0 && state[lo - 1] == 0) lo--;
    while (hi < NR_SPR_BKT - 1 && state[hi + 1] == 0) hi++;
  }
  out->rb_lo = lo * 8;
  out->rb_hi = (hi + 1) * 8;
  for (int q = 0; q < np && out->npat < NR_SPR_MAXPAT; q++)
    nr_re_pattern_make(&out->pat[out->npat++], pp[q].sym, pp[q].sc, pp[q].dens, out->rb_lo, out->rb_hi - out->rb_lo);
  return out->npat;
}
