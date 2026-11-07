#include "nr_sense_quality.h"
#include <math.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>

/* ---- null distributions (all tests are at the one level NR_SENSE_PRB_ALPHA) ----
 * A PRB's statistics under H0 are Gamma(shape k, scale th) in units of the mean per-RE noise variance s2 of H:
 *   SNR test      T = sum |h|^2 over tot RE x n_ant           (noise only)           E[T] = tot n_ant s2
 *   residual test S = sse of the affine fit over the best symbol (affine channel+noise) E[S] = s2 n_ant tr(M C)
 * White noise (C = I): T ~ Gamma(n_ant tot, 1), S ~ Gamma(n_ant (cnt-2), 1) exactly (complex Gaussian: |.|^2 sums). Correlated noise (DM-RS chest, covariance s2 C, C of the 6 REs with
 * mean diagonal 1) and per-RE heterogeneous variances (data-aided H = Y/X: v_i = s2 / |X_i|^2) make the quadratic forms sums of lambda_i * Exp(1): matched to a Gamma by mean and variance
 * (Satterthwaite). Heterogeneity: k = N eta, th = 1/eta with eta = E[v]^2 / E[v^2] over the REs (N_eff = N eta); the chest operator: th = tr(Q^2)/tr(Q), k = n_ant tr(Q)^2/tr(Q^2), Q = C (T) or
 * M C (S), M = I - X(X'X)^-1 X' the affine residual projector over the 6 subcarrier positions. The Gamma tail at 1e-3 is an approximation of the exact one.
 * A noise reference estimated from the record (median of the OTHER PRBs' residuals) has its own dof nu: the ratio statistic is then F-like, R = nu G_a / G_nu, and its tail is the incomplete beta:
 * P(R > x) = I_{nu/(x+nu)}(nu, a). nu = 0 means an exact reference (producer figure in H scale). */
static double gq(double a, double x) /* upper regularised incomplete gamma Q(a,x) */
{
  if (x <= 0) return 1;
  int sg;
  const double lg = lgamma_r(a, &sg);
  if (x < a + 1) { /* series for P */
    double ap = a, del = 1.0 / a, sum = del;
    for (int n = 0; n < 500 && fabs(del) > fabs(sum) * 1e-14; n++) { ap += 1; del *= x / ap; sum += del; }
    return 1.0 - sum * exp(-x + a * log(x) - lg);
  }
  double b = x + 1 - a, c = 1e300, d = 1 / b, h = d; /* continued fraction for Q */
  for (int i = 1; i < 500; i++) {
    const double an = -i * (i - a);
    b += 2;
    d = an * d + b; if (fabs(d) < 1e-300) d = 1e-300;
    c = b + an / c; if (fabs(c) < 1e-300) c = 1e-300;
    d = 1 / d;
    const double del = d * c;
    h *= del;
    if (fabs(del - 1) < 1e-14) break;
  }
  return exp(-x + a * log(x) - lg) * h;
}
static double betacf(double a, double b, double x)
{
  const double qab = a + b, qap = a + 1, qam = a - 1;
  double c = 1, d = 1 - qab * x / qap;
  if (fabs(d) < 1e-300) d = 1e-300;
  d = 1 / d;
  double h = d;
  for (int m = 1; m <= 300; m++) {
    const int m2 = 2 * m;
    double aa = m * (b - m) * x / ((qam + m2) * (a + m2));
    d = 1 + aa * d; if (fabs(d) < 1e-300) d = 1e-300;
    c = 1 + aa / c; if (fabs(c) < 1e-300) c = 1e-300;
    d = 1 / d; h *= d * c;
    aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2));
    d = 1 + aa * d; if (fabs(d) < 1e-300) d = 1e-300;
    c = 1 + aa / c; if (fabs(c) < 1e-300) c = 1e-300;
    d = 1 / d;
    const double del = d * c;
    h *= del;
    if (fabs(del - 1) < 1e-14) break;
  }
  return h;
}
static double betai(double a, double b, double x) /* regularised incomplete beta I_x(a,b) */
{
  if (x <= 0) return 0;
  if (x >= 1) return 1;
  int s;
  const double lbt = lgamma_r(a + b, &s) - lgamma_r(a, &s) - lgamma_r(b, &s) + a * log(x) + b * log1p(-x);
  if (x < (a + 1) / (a + b + 2)) return exp(lbt) * betacf(a, b, x) / a;
  return 1 - exp(lbt) * betacf(b, a, 1 - x) / b;
}
static double ftail(double a, double nu, double x) { return nu > 0 ? betai(nu, a, nu / (x + nu)) : gq(a, x); }
/* x with tail(a, nu, x) = alpha. Cached per thread, keyed by the shape on a 1/16 grid and nu on an integer (< 32) or multiple-of-8 grid rounded DOWN (a smaller nu is the more
 * conservative); the threshold is rescaled by a / a_grid. The cache is hashed on both keys, so integer shapes (white noise) do not collide. */
static __thread struct { float a, nu; double x; } g_gc[512];
static __thread uint64_t g_gc_calls, g_gc_miss;
void nr_sense_quality_gcrit_stats(uint64_t *calls, uint64_t *misses) { *calls = g_gc_calls; *misses = g_gc_miss; }
static double gcrit(double a, double nu)
{
  float aq = floorf((float)a * 16.f + 0.5f) / 16.f;
  if (aq < 0.0625f) aq = 0.0625f;
  float nq = nu <= 0 || nu > 4000 ? 0.f : nu < 32 ? floorf((float)nu) : floorf((float)nu / 8.f) * 8.f;
  if (nu > 0 && nq < 1.f) nq = 1.f;
  const uint32_t hs = ((uint32_t)(aq * 16.f) * 2654435761u ^ ((uint32_t)nq + 1u) * 0x9E3779B1u) >> 23;
  g_gc_calls++;
  if (g_gc[hs].a != aq || g_gc[hs].nu != nq || g_gc[hs].x == 0) {
    g_gc_miss++;
    double lo = 0, hi = aq + 10;
    while (ftail(aq, nq, hi) > NR_SENSE_PRB_ALPHA) hi *= 2;
    for (int i = 0; i < 60; i++) {
      const double mid = 0.5 * (lo + hi);
      if (ftail(aq, nq, mid) > NR_SENSE_PRB_ALPHA) lo = mid; else hi = mid;
    }
    g_gc[hs].a = aq; g_gc[hs].nu = nq; g_gc[hs].x = hi;
  }
  return g_gc[hs].x * (a / aq);
}

/* ---- heterogeneous per-RE noise (data-aided H = Y/X; LS-window rows): saddlepoint tail ----
 * T/s2 = sum_i v_i G_i, G_i ~ Gamma(n_ant) (the antennas share X, so v_i is common to them), v_i iid from a known discrete law (mean 1): the QAM law w = 1/|X|^2 over the 38.211 5.1
 * constellation of order qm (kind 0), or, for window rows whose law the producer only summarises by eta = (sum v)^2 / (R sum v^2), a lognormal of that variance (kind 1; an assumption: heavy
 * right tail like the inverse of a Gram-matrix diagonal). Cumulant K(s) = N log E[(1 - s v)^-n_ant]; Lugannani-Rice tail with the saddlepoint K'(s) = t; the threshold t with tail alpha is
 * cached per thread by (kind, parameter, n_ant, N). A two-moment Gamma is far off here (26-45 alpha measured on 16..256QAM). */
typedef struct { int n; double v[256], pi[256]; } sdist_t;
static void sdist_qam(int qm, sdist_t *d)
{
  const int M = 1 << (qm / 2);
  double w[256]; int cnt[256], n = 0;
  for (int a = 0; a < M; a++)
    for (int b = 0; b < M; b++) {
      const double x = 2 * a - (M - 1), y = 2 * b - (M - 1), ww = 1.0 / (x * x + y * y);
      int j = 0;
      while (j < n && fabs(w[j] - ww) > 1e-12) j++;
      if (j == n) { w[n] = ww; cnt[n++] = 0; }
      cnt[j]++;
    }
  double m = 0;
  for (int j = 0; j < n; j++) m += w[j] * cnt[j] / ((double)M * M);
  d->n = n;
  for (int j = 0; j < n; j++) { d->v[j] = w[j] / m; d->pi[j] = cnt[j] / ((double)M * M); }
}
static void sdist_logn(double eta, sdist_t *d)
{
  const int n = 128;
  const double s2 = log(1.0 / (eta > 0.02 ? eta : 0.02)), sg = sqrt(s2); /* 1 + CV^2 = 1/eta */
  double m = 0;
  d->n = n;
  for (int j = 0; j < n; j++) { /* z = Phi^-1((j + 0.5) / n) by bisection on erfc */
    const double pr = (j + 0.5) / n;
    double lo = -8, hi = 8;
    for (int i = 0; i < 50; i++) { const double mid = 0.5 * (lo + hi); if (0.5 * erfc(-mid / M_SQRT2) < pr) lo = mid; else hi = mid; }
    d->v[j] = exp(sg * 0.5 * (lo + hi)); d->pi[j] = 1.0 / n; m += d->v[j] / n;
  }
  for (int j = 0; j < n; j++) d->v[j] /= m;
}
static double sp_tail(const sdist_t *d, int na, double N, double t, double *shat)
{
  double vmax = 0;
  for (int j = 0; j < d->n; j++) if (d->v[j] > vmax) vmax = d->v[j];
  double lo = 0, hi = (1.0 - 1e-12) / vmax;
  for (int i = 0; i < 80; i++) { /* K'(s) increasing */
    const double s = 0.5 * (lo + hi);
    double M0 = 0, M1 = 0;
    for (int j = 0; j < d->n; j++) { const double f = pow(1 - s * d->v[j], -na); M0 += d->pi[j] * f; M1 += d->pi[j] * na * d->v[j] * f / (1 - s * d->v[j]); }
    if (N * M1 / M0 < t) lo = s; else hi = s;
  }
  const double s = 0.5 * (lo + hi);
  double M0 = 0, M1 = 0, M2 = 0;
  for (int j = 0; j < d->n; j++) {
    const double f = pow(1 - s * d->v[j], -na), g = 1 - s * d->v[j];
    M0 += d->pi[j] * f; M1 += d->pi[j] * na * d->v[j] * f / g; M2 += d->pi[j] * na * (na + 1.0) * d->v[j] * d->v[j] * f / (g * g);
  }
  const double K = N * log(M0), K2 = N * (M2 / M0 - (M1 / M0) * (M1 / M0)), w2 = 2 * (s * t - K);
  if (shat) *shat = s;
  if (!(w2 > 1e-12) || !(K2 > 0)) return 0.5;
  const double w = sqrt(w2), u = s * sqrt(K2), phi = exp(-0.5 * w2) / sqrt(2 * M_PI);
  return 0.5 * erfc(w / M_SQRT2) + phi * (1.0 / u - 1.0 / w);
}
static __thread struct { int kind, par, na, N; double x; } g_sp[256];
static double sp_crit(int kind, int par, int na, int N)
{
  const uint32_t hs = ((uint32_t)(kind * 1000003 + par * 7919 + na * 104729 + N) * 2654435761u) >> 24;
  if (g_sp[hs].x > 0 && g_sp[hs].kind == kind && g_sp[hs].par == par && g_sp[hs].na == na && g_sp[hs].N == N) return g_sp[hs].x;
  sdist_t d;
  if (kind == 0) sdist_qam(par, &d); else sdist_logn(par / 255.0, &d);
  const double mean = (double)na * N;
  double lo = mean, hi = mean * 2 + 10;
  while (sp_tail(&d, na, N, hi, NULL) > NR_SENSE_PRB_ALPHA) hi *= 1.6;
  for (int i = 0; i < 50; i++) {
    const double mid = 0.5 * (lo + hi);
    if (sp_tail(&d, na, N, mid, NULL) > NR_SENSE_PRB_ALPHA) lo = mid; else hi = mid;
  }
  g_sp[hs].kind = kind; g_sp[hs].par = par; g_sp[hs].na = na; g_sp[hs].N = N; g_sp[hs].x = hi;
  return hi;
}

/* Noise covariance of the DM-RS decoder estimate (nr_dl_channel_estimation.c NFAPI_NR_DMRS_TYPE{1,2}_linear_interp, 38.211 7.4.1.1.2: pilots on every second subcarrier of the port's comb
 * (type 1) or on subcarrier pairs (type 2)). The LS value of a pilot PAIR (mean of 2, variance s2_p/2) stands for a block of 4 (type 1) or 6 (type 2) subcarriers (3 / 2 blocks per PRB). The
 * chest then runs 16-tap filters over the half-block sequence k = 2c (c = 0..): the middle filter is 0.125 on 16 outputs (UL filters, 4096/2048/8192 over 16384), written at an offset that
 * advances 4 outputs every second step (the same for both types), so the estimate is a unit-gain staircase of 4-subcarrier steps. The exported H is its comb-2 samples (6 per PRB).
 * The delay de-rotation is a diagonal unitary congruence D C D^H with D = diag(exp(j phi_k)): tr C and tr C^2 are unchanged, and tr(MC) changes by terms in cos(phi_i - phi_j) that are
 * second order for the phase ramp inside a PRB: phi_i - phi_j = 2 pi delay (i-j) 2 / 4096 over the 6 comb REs, i.e. 0.15 rad per 10 samples of residual delay across the whole PRB;
 * ignored (< 1 % of tr(MC) below ~20 samples). Edge PRBs of an allocation (first/last filter weights 0.25/0.5) use the interior operator: their noise is larger than modelled. */
typedef struct { double kT, thT, kR, thR, mR; } dm_t; /* per antenna: shape and scale of T and S, and E[S]/s2 (mR) */
static dm_t g_dm[2];
static pthread_once_t g_dm_once = PTHREAD_ONCE_INIT;
static void dm_build(int type, dm_t *o)
{
  enum { NP = 6, NS = NP * 12, NB = NP * 3 };
  const int blk = type == 1 ? 4 : 6;
  double A[NS][NB];
  memset(A, 0, sizeof A);
  const double fp0[16] = {.25, .25, .25, .25, .25, .25, .25, .25}, fp12[16] = {.25, .25, .25, .25, .125, .125, .125, .125, .125, .125, .125, .125},
               fmid[16] = {.125, .125, .125, .125, .125, .125, .125, .125, .125, .125, .125, .125, .125, .125, .125, .125},
               flast[16] = {.25, .25, .25, .25, .5, .5, .5, .5};
  const int cnt = NS >> 1;
  int ptr = 0;
  for (int c = 0; c < cnt; c++) {
    const double *f = c == 0 ? fp0 : (c == 1 || c == 2) ? fp12 : c == cnt - 1 ? flast : fmid;
    for (int t = 0; t < 16 && ptr + t < NS; t++) A[ptr + t][(2 * c) / blk] += f[t];
    if (c > 2 && c != cnt - 1 && c % 2 == 0) ptr += 4;
  }
  double C[6][6], M[6][6], MC[6][6];
  for (int i = 0; i < 6; i++)
    for (int j = 0; j < 6; j++) {
      double s = 0;
      const int xi = 24 + 2 * i, xj = 24 + 2 * j; /* PRB 2 (interior) comb REs */
      for (int b = 0; b < NB; b++) s += A[xi][b] * A[xj][b];
      C[i][j] = 0.5 * s; /* LS pair noise s2_p/2 */
    }
  double trC = 0;
  for (int i = 0; i < 6; i++) trC += C[i][i];
  for (int i = 0; i < 6; i++)
    for (int j = 0; j < 6; j++) C[i][j] *= 6.0 / trC; /* mean diagonal 1: s2 is the per-RE variance of H */
  double sxx = 0;
  for (int i = 0; i < 6; i++) sxx += (i - 2.5) * (i - 2.5);
  for (int i = 0; i < 6; i++)
    for (int j = 0; j < 6; j++) M[i][j] = (i == j ? 1.0 : 0.0) - 1.0 / 6 - (i - 2.5) * (j - 2.5) / sxx;
  double t1 = 6, t2 = 0, u1 = 0, u2 = 0;
  for (int i = 0; i < 6; i++)
    for (int j = 0; j < 6; j++) {
      t2 += C[i][j] * C[j][i];
      double s = 0;
      for (int m = 0; m < 6; m++) s += M[i][m] * C[m][j];
      MC[i][j] = s;
    }
  for (int i = 0; i < 6; i++) {
    u1 += MC[i][i];
    for (int j = 0; j < 6; j++) u2 += MC[i][j] * MC[j][i];
  }
  o->thT = t2 / t1, o->kT = t1 * t1 / t2, o->thR = u2 / u1, o->kR = u1 * u1 / u2, o->mR = u1;
}
static void dm_init(void) { dm_build(1, &g_dm[0]); dm_build(2, &g_dm[1]); }
void nr_sense_quality_dmrs_model(int type, double out[5])
{
  pthread_once(&g_dm_once, dm_init);
  const dm_t *d = &g_dm[type == 2];
  out[0] = d->kT, out[1] = d->thT, out[2] = d->kR, out[3] = d->thR, out[4] = d->mR;
}
/* eta = E[w]^2 / E[w^2], w = 1/|X|^2 over the 38.211 5.1 constellation of order qm (Gray 2^(qm/2)-PAM per axis; the scale cancels) */
static double g_eta[5];
static pthread_once_t g_eta_once = PTHREAD_ONCE_INIT;
static void eta_init(void)
{
  double *tab = g_eta;
  {
    for (int q = 1; q <= 4; q++) {
      const int M = 1 << q; /* PAM levels per axis */
      double s1 = 0, s2 = 0;
      for (int a = 0; a < M; a++)
        for (int b = 0; b < M; b++) {
          const double x = 2 * a - (M - 1), y = 2 * b - (M - 1), w = 1.0 / (x * x + y * y);
          s1 += w, s2 += w * w;
        }
      s1 /= (double)M * M, s2 /= (double)M * M;
      tab[q] = s1 * s1 / s2;
    }
  }
}
double nr_sense_quality_qam_eta(int qm)
{
  pthread_once(&g_eta_once, eta_init);
  const int q = qm / 2;
  return q >= 1 && q <= 4 ? g_eta[q] : g_eta[4]; /* unknown order: the most conservative (256QAM) */
}

#define QMAXANT 8
#define QSYM 16
#define QKSPAN (NR_SENSE_MAX_PRB * 12)

static float pick(float *v, int n, int num, int den)
{
  for (int i = 1; i < n; i++) { /* insertion sort: n <= 275 */
    float x = v[i];
    int j = i - 1;
    while (j >= 0 && v[j] > x) {
      v[j + 1] = v[j];
      j--;
    }
    v[j + 1] = x;
  }
  return v[(n - 1) * num / den];
}


/* Sparse sources: the noise variance of H from the frame itself. The producers' own noise figures (the CSI-RS estimator's) are not in H scale, so for a sparse source the noise is derived
 * here, white and common to the record: the second difference of the estimates of three equally spaced PRBs (same subcarrier, same symbol, spacing 1 PRB, else 2) removes any channel
 * that is linear in frequency; for white noise E|h0 - 2 h1 + h2|^2 = 6 sigma^2. A channel with curvature across 2-4 PRBs (large delay spread) leaves a residue in it: the estimate is then
 * biased HIGH (fewer PRBs valid, never more). Returns -1 when the frame has no such triple (the producer's figure, if any, then stands). */
static double sparse_noise_from_frame(int n_ant, const float *iq, int ant_stride, int n_re, const uint16_t *k_abs, const uint8_t *l_sym, int pmin, int sa)
{
  int16_t pe[NR_SENSE_MAX_PRB];
  for (int p = 0; p < NR_SENSE_MAX_PRB; p++)
    pe[p] = -1;
  for (int i = 0; i < n_re; i++) {
    const int p = k_abs[i] / 12 - pmin;
    if (p < NR_SENSE_MAX_PRB && l_sym[i] == sa && pe[p] < 0)
      pe[p] = (int16_t)i;
  }
  for (int d = 1; d <= 2; d++) {
    double acc = 0;
    long n = 0;
    for (int p = 0; p + 2 * d < NR_SENSE_MAX_PRB; p++) {
      const int i0 = pe[p], i1 = pe[p + d], i2 = pe[p + 2 * d];
      if (i0 < 0 || i1 < 0 || i2 < 0 || k_abs[i0] % 12 != k_abs[i1] % 12 || k_abs[i1] % 12 != k_abs[i2] % 12)
        continue;
      for (int a = 0; a < n_ant; a++) {
        const float *h = iq + 2 * (size_t)a * ant_stride;
        const double r = h[2 * i0] - 2.0 * h[2 * i1] + h[2 * i2], m = h[2 * i0 + 1] - 2.0 * h[2 * i1 + 1] + h[2 * i2 + 1];
        acc += r * r + m * m;
        n++;
      }
    }
    if (n > 0)
      return acc / (6.0 * (double)n);
  }
  return -1;
}

void nr_sense_quality_compute(int n_ant, const float *iq, int ant_stride, int n_re, const uint16_t *k_abs,
                              const uint8_t *l_sym, float noise_var, nr_sense_quality_t *q)
{
  nr_sense_quality_compute_ex(n_ant, iq, ant_stride, n_re, k_abs, l_sym, noise_var, 0, q);
}

void nr_sense_quality_compute_ex(int n_ant, const float *iq, int ant_stride, int n_re, const uint16_t *k_abs,
                                 const uint8_t *l_sym, float noise_var, unsigned flags, nr_sense_quality_t *q)
{
  const bool sparse = (flags & NR_SENSE_Q_SPARSE) != 0, dmi = (flags & NR_SENSE_Q_DMRS_INTERP) != 0 && !sparse, window = (flags & NR_SENSE_Q_WINDOW) != 0 && !sparse;
  const dm_t *dmm = NULL;
  if (dmi) { pthread_once(&g_dm_once, dm_init); dmm = &g_dm[(flags & NR_SENSE_Q_DMRS_T2) ? 1 : 0]; }
  /* heterogeneity of the per-RE noise variance (eta): window LS rows carry their own (producer, 8 bit); data-aided H = Y/X from the constellation order (unknown order: 256QAM) */
  double eta = 1;
  if (window) { const unsigned h8 = (flags >> 16) & 255u; eta = h8 ? h8 / 255.0 : 1; }
  else if (flags & NR_SENSE_Q_DATA_AIDED) eta = nr_sense_quality_qam_eta(2 * (int)((flags >> 8) & 15u));
  const bool het = !dmi && eta < 0.999;
  const int hkind = window ? 1 : 0, hpar = window ? (int)((flags >> 16) & 255u) : (((flags >> 8) & 15u) ? 2 * (int)((flags >> 8) & 15u) : 8);
  memset(q, 0, sizeof(*q));
  q->snr_db_p50 = q->snr_db_p10 = q->resid_db = q->sym_consist = q->recon_conf = NAN;
  if (n_ant < 1 || n_re < 1 || !iq || !k_abs || !l_sym)
    return;
  if (n_ant > QMAXANT)
    n_ant = QMAXANT;
  int pmin = 0xffff, kmin = 0xffff;
  for (int i = 0; i < n_re; i++) {
    if (k_abs[i] / 12 < pmin)
      pmin = k_abs[i] / 12;
    if (k_abs[i] < kmin)
      kmin = k_abs[i];
  }
  q->first_prb = (int16_t)pmin;

  uint16_t cnt[NR_SENSE_MAX_PRB][QSYM] = {{0}};
  uint16_t tot[NR_SENSE_MAX_PRB] = {0};
  uint32_t symcnt[QSYM] = {0};
  for (int i = 0; i < n_re; i++) {
    int p = k_abs[i] / 12 - pmin;
    if (p >= NR_SENSE_MAX_PRB)
      continue;
    tot[p]++;
    if (l_sym[i] < QSYM) {
      cnt[p][l_sym[i]]++;
      symcnt[l_sym[i]]++;
    }
  }
  uint8_t best[NR_SENSE_MAX_PRB];
  for (int p = 0; p < NR_SENSE_MAX_PRB; p++) {
    int b = 0;
    for (int s = 1; s < QSYM; s++)
      if (cnt[p][s] > cnt[p][b])
        b = s;
    best[p] = (uint8_t)b;
    if (tot[p])
      q->allocated_prbs++;
  }
  q->estimated_usable_prbs = q->allocated_prbs; /* shortcut: usable = allocated until producers report rate-matched PRBs */

  /* UL record expressed in the DL frame (nr_sense_ul_to_dl): H carries the transmitter's timing advance as a phase ramp of up to ~pi per subcarrier, which a per-PRB
   * constant+slope fit in the complex domain cannot follow (residual ~1 whatever the SNR). The ramp is one common delay: its per-subcarrier phase step is the argument of the
   * correlation of the record's adjacent REs (at the record's own RE spacing) (same symbol, summed over antennas and REs: independent of the antenna phases, unbiased by noise). Inside a PRB the fit sees
   * H derotated by that step; powers and the SNR are untouched, and a PRB without signal still fails the SNR test. */
  float tw_r[12], tw_i[12];
  for (int x = 0; x < 12; x++)
    tw_r[x] = 1.f, tw_i[x] = 0.f;
  if (flags & NR_SENSE_Q_UL_DELAY) {
    /* the RE spacing d of the record is the commonest positive step between consecutive REs of a symbol (1 for dense records, the comb for pilot-only ones; DM-RS type 2 pairs give 1).
     * The step per subcarrier is arg(corr_d)/d: an aliased arg only moves the derotation by a multiple of 2pi/d, which is a multiple of 2pi at every RE that is present. */
    int hist[5] = {0}, d = 1;
    for (int i = 0; i + 1 < n_re; i++) {
      const int df = (int)k_abs[i + 1] - (int)k_abs[i];
      if (l_sym[i + 1] == l_sym[i] && df >= 1 && df <= 4)
        hist[df]++;
    }
    for (int j = 2; j <= 4; j++)
      if (hist[j] > hist[d])
        d = j;
    double cr = 0, ci = 0;
    for (int a = 0; a < n_ant; a++) {
      const float *h = iq + 2 * (size_t)a * ant_stride;
      for (int i = 0; i + 1 < n_re; i++) {
        if ((int)k_abs[i + 1] - (int)k_abs[i] != d || l_sym[i + 1] != l_sym[i])
          continue;
        cr += (double)h[2 * i + 2] * h[2 * i] + (double)h[2 * i + 3] * h[2 * i + 1];
        ci += (double)h[2 * i + 3] * h[2 * i] - (double)h[2 * i + 2] * h[2 * i + 1];
      }
    }
    q->delay_step = 2;
    if (cr != 0 || ci != 0) {
      q->delay_step = 1;
      const double phi = atan2(ci, cr) / d;
      for (int x = 0; x < 12; x++)
        tw_r[x] = (float)cos(phi * (x - 5.5)), tw_i[x] = (float)-sin(phi * (x - 5.5));
    }
  }
  double pwr[NR_SENSE_MAX_PRB] = {0}, sse[NR_SENSE_MAX_PRB] = {0}, sig[NR_SENSE_MAX_PRB] = {0};
  for (int a = 0; a < n_ant; a++) {
    double n[NR_SENSE_MAX_PRB] = {0}, sx[NR_SENSE_MAX_PRB] = {0}, sxx[NR_SENSE_MAX_PRB] = {0};
    double shr[NR_SENSE_MAX_PRB] = {0}, shi[NR_SENSE_MAX_PRB] = {0}, sxhr[NR_SENSE_MAX_PRB] = {0};
    double sxhi[NR_SENSE_MAX_PRB] = {0}, shh[NR_SENSE_MAX_PRB] = {0};
    const float *h = iq + 2 * (size_t)a * ant_stride;
    for (int i = 0; i < n_re; i++) {
      int p = k_abs[i] / 12 - pmin;
      if (p >= NR_SENSE_MAX_PRB)
        continue;
      double hr = h[2 * i], hi = h[2 * i + 1], e = hr * hr + hi * hi;
      pwr[p] += e;
      if (l_sym[i] != best[p])
        continue;
      const int kx = k_abs[i] % 12;
      const double dr = hr * tw_r[kx] - hi * tw_i[kx];
      hi = hr * tw_i[kx] + hi * tw_r[kx];
      hr = dr;
      double x = (k_abs[i] % 12) - 5.5;
      n[p]++, sx[p] += x, sxx[p] += x * x, shr[p] += hr, shi[p] += hi;
      sxhr[p] += x * hr, sxhi[p] += x * hi, shh[p] += e;
    }
    for (int p = 0; p < NR_SENSE_MAX_PRB; p++) {
      if (n[p] < 3)
        continue;
      double mx = sx[p] / n[p], sxxc = sxx[p] - sx[p] * mx;
      double cr = sxhr[p] - mx * shr[p], ci = sxhi[p] - mx * shi[p];
      double r = shh[p] - (shr[p] * shr[p] + shi[p] * shi[p]) / n[p] - (sxxc > 1e-9 ? (cr * cr + ci * ci) / sxxc : 0);
      sse[p] += r > 0 ? r : 0;
      sig[p] += shh[p];
    }
  }

  if (sparse) {
    int sa0 = 0;
    for (int sy = 1; sy < QSYM; sy++)
      if (symcnt[sy] > symcnt[sa0])
        sa0 = sy;
    const double nf = sparse_noise_from_frame(n_ant, iq, ant_stride, n_re, k_abs, l_sym, pmin, sa0);
    if (nf >= 0) /* a noiseless frame (0) gets a tiny positive variance: the 30 dB ceiling floor below then applies */
      noise_var = nf > 1e-30 ? (float)nf : 1e-30f;
  }
  /* PRBs whose 6 REs of the fit symbol are the DM-RS comb use the interpolation model; any other PRB (edge, partial) the white one */
#define DMP(p) (dmi && tot[p] == 6 && cnt[p][best[p]] == 6)
  /* Noise reference s2 (mean per-RE variance of H) and its own dof nu, per PRB:
   *  - the producer's figure when it is in H scale (not for the DM-RS chest: its nvar is 3.3x / 1.75x the H noise): exact, nu = 0;
   *  - otherwise a leave-one-out reference from the record itself: the OTHER candidate PRBs' fit residuals per unit of their null mean (mean of 1-2 others, else their lower median
   *    corrected from median to mean of the Gamma), with dof nu = 2 n_eff k / pi (median of n_eff Gamma(k): variance (pi/2)/(n_eff k) relative; n_eff halved for the chest, whose
   *    outputs of adjacent PRBs share pilots). A PRB never sets its own reference; with a single candidate PRB there is none: the PRB is charged to NOSIG (counted, not valid).
   *    Bad PRBs among the others raise the reference: the SNR test gets stricter (fewer valid), the residual test more lenient (a PRB with an elevated residual is excused by a high reference; bounded by the median's robustness: it takes half the pool to move it). */
  const bool ext_ref = sparse || ((window || !dmi) && noise_var > 0);
  float sref[NR_SENSE_MAX_PRB], nur[NR_SENSE_MAX_PRB];
  bool have_ref[NR_SENSE_MAX_PRB];
  memset(have_ref, 0, sizeof have_ref);
  if (!ext_ref && !window) {
    float ev[NR_SENSE_MAX_PRB];
    int ix[NR_SENSE_MAX_PRB], rk[NR_SENSE_MAX_PRB], ne = 0;
    double sum = 0;
    for (int p = 0; p < NR_SENSE_MAX_PRB; p++) {
      if (tot[p] < 3 || cnt[p][best[p]] < 3 || !(sig[p] > 0) || !isfinite(pwr[p]) || !isfinite(sse[p])) continue;
      const double m = DMP(p) ? dmm->mR : (double)(cnt[p][best[p]] - 2);
      ev[ne] = (float)(sse[p] / (m * n_ant));
      ix[ne++] = p;
      sum += ev[ne - 1];
    }
    if (ne >= 2) {
      float srt[NR_SENSE_MAX_PRB];
      int ord[NR_SENSE_MAX_PRB]; /* ord[j] = candidate index of the j-th smallest */
      for (int i = 0; i < ne; i++) {
        int j = i - 1;
        while (j >= 0 && srt[j] > ev[i]) { srt[j + 1] = srt[j]; ord[j + 1] = ord[j]; j--; }
        srt[j + 1] = ev[i]; ord[j + 1] = i;
      }
      for (int j = 0; j < ne; j++) rk[ord[j]] = j;
      for (int i = 0; i < ne; i++) {
        const int p = ix[i], no = ne - 1;
        const double ke = DMP(p) ? n_ant * dmm->kR : n_ant * (cnt[p][best[p]] - 2) * eta;
        double ref, nu;
        if (no == 1) { ref = sum - ev[i]; nu = dmi ? ke / 2 : ke; }
        else if (no == 2) { ref = (sum - ev[i]) / 2; nu = dmi ? ke : 2 * ke; }
        else {
          const int mi = (no - 1) / 2;
          ref = srt[mi + (mi >= rk[i])] / ((ke - 1.0 / 3) / ke);
          nu = 2.0 * (dmi ? no / 2.0 : (double)no) * ke / M_PI;
        }
        sref[p] = (float)ref, nur[p] = (float)nu, have_ref[p] = true;
      }
    }
  }
  float snr[NR_SENSE_MAX_PRB], res[NR_SENSE_MAX_PRB];
  int nv = 0;
  for (int p = 0; p < NR_SENSE_MAX_PRB; p++) {
    if (sparse) { /* one RE per PRB is enough: there is no local fit, so the producer's noise estimate is required and the residual test is not applied */
      if (tot[p] < 1 || !isfinite(pwr[p]) || !(pwr[p] > 0) || !(noise_var > 0)) {
        if (tot[p]) q->inval[NR_SENSE_INV_NOSIG]++;
        continue;
      }
    } else if (window) { /* window-LS rows: one estimate per LS window, no per-symbol affine fit (the window ladder already proved flatness within noise), SNR test only */
      if (tot[p] < 1) continue;
      if (!isfinite(pwr[p]) || !(pwr[p] > 0) || !(noise_var > 0)) {
        q->inval[NR_SENSE_INV_NOSIG]++;
        continue;
      }
    } else {
      if (tot[p] < 3 || cnt[p][best[p]] < 3) {
        if (tot[p]) q->inval[NR_SENSE_INV_FEW_RE]++;
        continue;
      }
      if (!(sig[p] > 0) || !isfinite(pwr[p]) || !isfinite(sse[p]) || (!ext_ref && !have_ref[p])) { /* no independent reference: counted, never valid */
        q->inval[NR_SENSE_INV_NOSIG]++;
        continue;
      }
    }
    const double mean_pwr = pwr[p] / ((double)tot[p] * n_ant);
    double npw = ext_ref ? noise_var : sref[p], nu = ext_ref ? 0 : nur[p];
    /* floor: no receiver sees an effective SNR above ~30 dB (NR_SENSE_RECON_IMPL_LOSS: transmitter EVM, phase noise, ADC), so a noise estimate
     * below that fraction of the PRB power (a noiseless simulation gives exactly 0) is raised to it: high SNR, never "invalid" */
    if (npw < NR_SENSE_RECON_IMPL_LOSS * mean_pwr) {
      npw = NR_SENSE_RECON_IMPL_LOSS * mean_pwr;
      nu = 0; /* a floor, not an estimate */
    }
    if (!(npw > 0)) {
      q->inval[NR_SENSE_INV_NOSIG]++;
      continue;
    }
    /* channel SNR (spec 4.2a): E[mean|H|^2] = S + N, so subtract the noise; no signal left -> -100 dB (invalid). Reported only: validity is the two tail tests below. */
    double sp = mean_pwr - npw;
    float s = sp > 0 ? (float)(10.0 * log10(sp / npw)) : -100.0f;
    float r = sparse || window ? 0.f : (float)(sse[p] / sig[p]);
    const bool dm = DMP(p);
    const double kT = dm ? n_ant * dmm->kT : (double)n_ant * tot[p] * eta, thT = dm ? dmm->thT : 1.0 / eta;
    /* heterogeneous per-RE variance: saddlepoint threshold (units of the mean noise), inflated by the F factor of an estimated reference */
    const double infl = het && nu > 0 ? gcrit(kT, nu) / gcrit(kT, 0) : 1.0;
    const bool snr_ok = het ? pwr[p] / npw > sp_crit(hkind, hpar, n_ant, tot[p]) * infl : pwr[p] / (npw * thT) > gcrit(kT, nu);
    bool res_ok = true;
    if (!sparse && !window) {
      const int cn = cnt[p][best[p]];
      const double kR = dm ? n_ant * dmm->kR : (double)n_ant * (cn - 2) * eta, thR = dm ? dmm->thR : 1.0 / eta;
      res_ok = het ? sse[p] / npw <= sp_crit(hkind, hpar, n_ant, cn) * (cn - 2.0) / cn * infl : sse[p] / (npw * thR) <= gcrit(kR, nu);
    }
    if (snr_ok && res_ok) {
      q->valid_mask[p >> 3] |= (uint8_t)(1u << (p & 7));
      snr[nv] = s;
      res[nv++] = r;
    } else
      q->inval[!snr_ok ? NR_SENSE_INV_SNR : NR_SENSE_INV_RESID]++;
  }
  q->valid_cfr_prbs = (uint16_t)nv;
  if (nv) {
    q->snr_db_p10 = pick(snr, nv, 1, 10);
    q->snr_db_p50 = pick(snr, nv, 1, 2);
    float m = pick(res, nv, 1, 2);
    q->resid_db = sparse || window ? NAN : 10.0f * log10f(m > 1e-12f ? m : 1e-12f);
  }

  /* symbol consistency: two most populated symbols, over shared k, per-antenna complex correlation */
  int sa = -1, sb = -1;
  for (int s = 0; s < QSYM; s++) {
    if (!symcnt[s])
      continue;
    if (sa < 0 || symcnt[s] > symcnt[sa])
      sb = sa, sa = s;
    else if (sb < 0 || symcnt[s] > symcnt[sb])
      sb = s;
  }
  if (sb < 0)
    return;
  int16_t idx[QKSPAN];
  for (int i = 0; i < QKSPAN; i++)
    idx[i] = -1;
  for (int i = 0; i < n_re; i++)
    if (l_sym[i] == sa && k_abs[i] - kmin < QKSPAN)
      idx[k_abs[i] - kmin] = (int16_t)i;
  double cr = 0, ci = 0, pa = 0, pb = 0;
  for (int a = 0; a < n_ant; a++) {
    const float *h = iq + 2 * (size_t)a * ant_stride;
    for (int j = 0; j < n_re; j++) {
      if (l_sym[j] != sb || k_abs[j] - kmin >= QKSPAN || idx[k_abs[j] - kmin] < 0)
        continue;
      int i = idx[k_abs[j] - kmin];
      double ar = h[2 * i], ai = h[2 * i + 1], br = h[2 * j], bi = h[2 * j + 1];
      cr += ar * br + ai * bi, ci += ar * bi - ai * br;
      pa += ar * ar + ai * ai, pb += br * br + bi * bi;
    }
  }
  if (pa > 0 && pb > 0)
    q->sym_consist = (float)(sqrt(cr * cr + ci * ci) / sqrt(pa * pb));
  /* sparse source with two or more symbols: a failed symbol consistency invalidates the record's PRBs (nothing else checks them) */
  if (sparse && q->sym_consist == q->sym_consist && q->sym_consist < NR_SENSE_SPARSE_SYM_MIN) {
    memset(q->valid_mask, 0, sizeof q->valid_mask);
    q->inval[NR_SENSE_INV_SYM] = q->valid_cfr_prbs;
    q->valid_cfr_prbs = 0;
    q->snr_db_p10 = q->snr_db_p50 = NAN;
  }
}

void nr_sense_quality_apply_recon(nr_sense_quality_t *q, const uint8_t recon_ok_abs[(NR_SENSE_MAX_PRB + 7) / 8])
{
  int n = 0;
  const int before = q->valid_cfr_prbs;
  for (int p = 0; p < NR_SENSE_MAX_PRB; p++) {
    const int a = q->first_prb + p;
    const bool ok = a >= 0 && a < NR_SENSE_MAX_PRB && (recon_ok_abs[a >> 3] >> (a & 7) & 1);
    if (q->valid_mask[p >> 3] >> (p & 7) & 1) {
      if (ok)
        n++;
      else
        q->valid_mask[p >> 3] &= (uint8_t)~(1u << (p & 7));
    }
  }
  q->valid_cfr_prbs = (uint16_t)n;
  q->inval[NR_SENSE_INV_RECON] = (uint16_t)(before - n);
}

int nr_sense_quality_absent(nr_sense_quality_t *q, int nb_rb, bool truncated, bool partial_tb)
{
  int miss = nb_rb - (int)q->allocated_prbs, tot = 0;
  if (miss > 0) {
    q->inval[truncated ? NR_SENSE_INV_TRUNC : partial_tb ? NR_SENSE_INV_PARTIAL : NR_SENSE_INV_ABSENT] = (uint16_t)miss;
    tot = miss;
  }
  for (int i = NR_SENSE_INV_FEW_RE; i < NR_SENSE_INV_N; i++)
    tot += q->inval[i];
  return tot;
}

const char *nr_sense_inv_name(int r)
{
  static const char *const n[NR_SENSE_INV_N] = {"trunc", "da_partial", "absent", "few_re", "nosig", "snr", "resid", "sym", "recon"};
  return r >= 0 && r < NR_SENSE_INV_N ? n[r] : "?";
}
