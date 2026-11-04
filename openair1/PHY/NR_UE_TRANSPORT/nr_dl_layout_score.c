/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* E2: full-layout DM-RS scorer and constellation Qm classifier (see nr_dl_layout_score.h). */
#include "nr_dl_layout_score.h"
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "common/utils/nr/nr_common.h" /* get_beta_dmrs */
#include "nr_blind_stats.h"            /* NBS_SIGNIF_ALPHA */
#include "nr_pdsch_dmrs_scan.h"        /* nr_dmrs_scan_prb_score_gold */

#define KAPPA 1.62f           /* E|P|^2 / (E|P|)^2 of a product of two complex Gaussians: 16/pi^2 */
#define NCELLS 42.0f          /* 14 symbols x 3 CDM groups */
#define Z_ALPHA 3.0902f       /* Phi^-1(1 - NBS_SIGNIF_ALPHA) */
#define NR_QM_UNIT 8192.0f /* rms amplitude of the unit-mean-energy constellation in the eq scale (see the header) */
#define NR_QM_RMS_TOL 0.15 /* stated tolerance of the eq scale contract: the gain nuisance (+-8 %) plus the sampling fluctuation of the energy */
#define GBITS (12 * NR_DL_MAX_PRB)

/* ---- Gold sequence (TS 38.211 5.2.1), c(0..n-1), no allocation. x1 and x2 start at n = Nc = 1600: x1 from a constant, x2 by linearity in c_init. ---- */
static const uint32_t X1_NC = 0x5e485840u;
static const uint32_t X2_BASIS[31] = {
  0x70889900, 0x1199ab01, 0x53bbcf03, 0x57ff0707, 0x2ffe0e0e, 0x5ffc1c1c, 0x3ff83838, 0x7ff07070, 0x7fe0e0e1, 0x7fc1c1c2, 0x7f838384,
  0x7f070708, 0x7e0e0e11, 0x7c1c1c22, 0x78383844, 0x70707088, 0x60e0e111, 0x41c1c222, 0x03838444, 0x07070889, 0x0e0e1113, 0x1c1c2226,
  0x3838444c, 0x70708899, 0x60e11132, 0x41c22264, 0x038444c8, 0x07088990, 0x0e111320, 0x1c222640, 0x38444c80};

static void gold_bits(uint32_t cinit, int n, uint8_t *c)
{
  uint32_t a = X1_NC, b = 0;
  for (uint32_t v = cinit; v; v &= v - 1)
    b ^= X2_BASIS[__builtin_ctz(v)];
  for (int i = 0; i < n; i++) {
    c[i] = (uint8_t)((a ^ b) & 1u);
    a = (a >> 1) | ((((a >> 3) ^ a) & 1u) << 30);
    b = (b >> 1) | ((((b >> 3) ^ (b >> 2) ^ (b >> 1) ^ b) & 1u) << 30);
  }
}

static uint32_t cinit_of(int symbols_per_slot, int slot, int l, uint32_t nid, int nscid)
{
  const uint64_t t = (((uint64_t)symbols_per_slot * slot + l + 1) * (((uint64_t)nid << 1) + 1)) << 17;
  return (uint32_t)((t + ((uint64_t)nid << 1) + (uint64_t)nscid) % (1ULL << 31));
}

static inline int ngroups_of(int type) { return type == 1 ? 2 : 3; }
static inline int pil_per_prb(int type) { return type == 1 ? 6 : 4; } /* pilots per PRB and CDM group */
/* subcarrier offset in the PRB of pilot p (0..pil-1) of CDM group g, and its sequence index m (CRB 0 referenced) */
static inline int pilot_k(int type, int g, int p) { return type == 1 ? 2 * p + g : 6 * (p >> 1) + (p & 1) + 2 * g; }
static inline int pilot_m(int type, int prb, int p) { return type == 1 ? 6 * prb + p : 4 * prb + p; }

/* type 2 counterpart of nr_dmrs_scan_prb_score_gold (which is type 1): pairs (m, m+2) = (j = 0, 1) of the same k', 6 subcarriers apart */
static float coh_type2(const int16_t *const *rx, int nant, const uint8_t *c, const uint16_t *crb, int n_crb, int g)
{
  double Sr = 0, Si = 0, den = 0;
  for (int q = 0; q < n_crb; q++)
    for (int kp = 0; kp < 2; kp++) {
      const int m = 4 * crb[q] + kp, k1 = 12 * crb[q] + 2 * g + kp, k2 = k1 + 6;
      double r = 0, im_ = 0;
      for (int a = 0; a < nant; a++) {
        const double ar = rx[a][2 * k2], ai = rx[a][2 * k2 + 1], br = rx[a][2 * k1], bi = rx[a][2 * k1 + 1];
        r += ar * br + ai * bi;
        im_ += ai * br - ar * bi;
      }
      const int a1 = c[2 * m], b1 = c[2 * m + 1], a2 = c[2 * m + 4], b2 = c[2 * m + 5];
#define SG(x) ((x) ? -1 : 1)
      const int re = SG(a1 ^ a2) + SG(b1 ^ b2), im = SG(a2 ^ b1) - SG(b2 ^ a1);
#undef SG
      Sr += r * re - im_ * im;
      Si += r * im + im_ * re;
      den += sqrt(r * r + im_ * im_);
    }
  return den > 0 ? (float)(sqrt(Sr * Sr + Si * Si) / 2.0 / den) : 0.f;
}

static inline float e_re(const c16_t *p, int k) { return (float)p[k].r * p[k].r + (float)p[k].i * p[k].i; }

/* Wilson-Hilferty alpha-quantile of sum|n|^2 over M complex noise samples of variance nv */
static float noise_sum_quantile(float nv, int M)
{
  const float k = 2.0f * (float)M, t = 2.0f / (9.0f * k);
  const float w = 1.0f - t + Z_ALPHA * sqrtf(t);
  return 0.5f * nv * k * w * w * w;
}

static int valid(const nr_dl_hyp_t *h, const nr_dl_obs_t *o)
{
  if (!h || !o || !h->crb || !o->rxF || h->n_crb < 1 || h->n_crb > NR_DL_MAX_PRB) return 0;
  if (o->n_ant < 1 || o->n_ant > NR_DL_MAX_ANT || o->symbols < 1 || o->symbols > 16 || o->n_sc < 12) return 0;
  if (h->dmrs_type != 1 && h->dmrs_type != 2) return 0;
  const int ng = ngroups_of(h->dmrs_type);
  if (!h->cdm_groups_mask || (h->cdm_groups_mask >> ng) || h->nscid > 1 || h->nid > 65535) return 0;
  if (!h->dmrs_sym_mask || (h->dmrs_sym_mask >> o->symbols) || __builtin_popcount(h->dmrs_sym_mask) > NR_DL_MAX_DMRS_SYM) return 0;
  if (h->data_sym_mask >> o->symbols) return 0;
  int mx = 0;
  for (int q = 0; q < h->n_crb; q++) if (h->crb[q] > mx) mx = h->crb[q];
  return mx < NR_DL_MAX_PRB && 12 * (mx + 1) <= o->n_sc;
}

int nr_dl_cdm_consistent(const nr_dl_hyp_t *h, const nr_dl_score_t *s)
{
  if (!h || !s || (h->dmrs_type != 1 && h->dmrs_type != 2)) return 0;
  const int ng = ngroups_of(h->dmrs_type);
  const unsigned all = (1u << ng) - 1, empty = ((1u << (h->cdm_no_data < 8 ? h->cdm_no_data : 8)) - 1) & all & ~(unsigned)h->cdm_groups_mask;
  const unsigned other = all & ~(unsigned)h->cdm_groups_mask;
  const float beta = get_beta_dmrs(h->cdm_no_data, h->dmrs_type == 2);
  const float mid = 0.5f / (beta * beta); /* between ~0 (empty) and ~1/beta^2 (data) */
  if (empty) return s->comb_ratio < mid;
  if (other) return s->comb_ratio >= mid;
  return 1;
}

float nr_dl_score_rank(const nr_dl_hyp_t *h, const nr_dl_score_t *s)
{
  if (!h || !s || s->coh == -FLT_MAX || s->n_signif == 0) return -INFINITY; /* invalid input, or not one cell significant: never rank on noise */
  return nr_dl_cdm_consistent(h, s) ? s->coh + s->occ_f1 : -INFINITY;
}

void nr_dl_layout_score(const nr_dl_hyp_t *h, const nr_dl_obs_t *o, nr_dl_score_t *s)
{
  memset(s, 0, sizeof(*s));
  if (!valid(h, o)) { s->coh = -FLT_MAX; return; }
  const int type = h->dmrs_type, ng = ngroups_of(type), pil = pil_per_prb(type), nant = o->n_ant, nprb_all = o->n_sc / 12;
  int mx = 0;
  for (int q = 0; q < h->n_crb; q++) if (h->crb[q] > mx) mx = h->crb[q];
  const int nbits = 12 * (mx + 1);
  uint8_t gb[NR_DL_MAX_DMRS_SYM][GBITS];
  int sym_l[NR_DL_MAX_DMRS_SYM], nsym = 0;
  float eprb[NR_DL_MAX_PRB + 1];
  memset(eprb, 0, sizeof(eprb));
  const int nprb_occ = nprb_all < NR_DL_MAX_PRB ? nprb_all : NR_DL_MAX_PRB;
  const int ngm = __builtin_popcount(h->cdm_groups_mask);
  float cv_all[NR_DL_MAX_DMRS_SYM * 3];
  int ncv = 0;
  for (int l = 0; l < o->symbols; l++) {
    if (!((h->dmrs_sym_mask >> l) & 1)) continue;
    uint8_t *c = gb[nsym];
    sym_l[nsym++] = l;
    gold_bits(cinit_of(o->symbols, o->slot, l, h->nid, h->nscid), nbits, c);
    for (int g = 0; g < ng; g++) {
      if (!((h->cdm_groups_mask >> g) & 1)) continue;
      const int16_t *rx[NR_DL_MAX_ANT];
      for (int a = 0; a < nant; a++) rx[a] = (const int16_t *)o->rxF[a * o->symbols + l] + (type == 1 ? 2 * g : 0);
      /* type 1: group g is group 0 shifted one subcarrier (pilot_re of the reused scorer has no wrap since 12 (max CRB + 1) <= n_sc) */
      const float cv = type == 1 ? nr_dmrs_scan_prb_score_gold(rx, nant, o->n_sc, 0, c, h->crb, h->n_crb)
                                 : coh_type2(rx, nant, c, h->crb, h->n_crb, g);
      cv_all[ncv++] = cv;
      for (int p = 0; p < nprb_occ; p++)
        for (int i = 0; i < pil; i++) {
          const int k = 12 * p + pilot_k(type, g, i);
          for (int a = 0; a < nant; a++) eprb[p] += e_re(o->rxF[a * o->symbols + l], k);
        }
    }
  }
  /* Significance threshold. c weights every PRB by its pilot amplitude |P|, so unequal PRBs (a strong narrow allocation inside a claimed superset, fading)
   * carry fewer independent pairs than n_crb: n_eff = (sum w)^2 / sum w^2 over the claimed PRBs, w = pilot energy above the noise floor (|P| ~ w). */
  double sw = 0, sw2 = 0;
  {
    const double floor_e = o->noise_var > 0 ? (double)o->noise_var * nant * nsym * ngm * pil : 0.0;
    for (int q = 0; q < h->n_crb; q++) {
      const double w = fmax((double)eprb[h->crb[q]] - floor_e, 0.0);
      sw += w; sw2 += w * w;
    }
  }
  const double neff = sw2 > 0 ? fmin(sw * sw / sw2, (double)h->n_crb) : 1.0;
  const float thr = sqrtf(KAPPA * logf(NCELLS / (float)NBS_SIGNIF_ALPHA) / (float)((type == 1 ? 4 : 2) * neff));
  float coh = 0;
  for (int i = 0; i < ncv; i++) { coh += cv_all[i] - thr; s->n_signif += cv_all[i] > thr; }
  s->n_cells = ncv;
  s->coh = coh;

  /* occupancy F1 vs h->crb */
  if (o->noise_var > 0) {
    const float T = noise_sum_quantile(o->noise_var, nant * nsym * ngm * pil);
    uint8_t in[NR_DL_MAX_PRB];
    memset(in, 0, sizeof(in));
    for (int q = 0; q < h->n_crb; q++) in[h->crb[q]] = 1;
    int tp = 0, occ = 0;
    for (int p = 0; p < nprb_occ; p++)
      if (eprb[p] > T) { occ++; tp += in[p]; }
    s->occ_f1 = occ + h->n_crb > 0 ? 2.0f * (float)tp / (float)(occ + h->n_crb) : 0.f;
  }

  /* comb ratio: other CDM groups' REs vs pilot REs, noise-corrected */
  {
    const unsigned allg = (1u << ng) - 1, mask = h->cdm_groups_mask;
    const unsigned decl = ((1u << (h->cdm_no_data < 8 ? h->cdm_no_data : 8)) - 1) & allg & ~mask;
    const unsigned oth = decl ? decl : (allg & ~mask);
    double ep = 0, eo = 0;
    long np = 0, no = 0;
    for (int d = 0; d < nsym; d++) {
      const int l = sym_l[d];
      for (int q = 0; q < h->n_crb; q++)
        for (int g = 0; g < ng; g++) {
          const int isp = (mask >> g) & 1, iso = (oth >> g) & 1;
          if (!isp && !iso) continue;
          for (int i = 0; i < pil; i++) {
            const int k = 12 * h->crb[q] + pilot_k(type, g, i);
            for (int a = 0; a < nant; a++) {
              const float e = e_re(o->rxF[a * o->symbols + l], k);
              if (isp) { ep += e; np++; } else { eo += e; no++; }
            }
          }
        }
    }
    if (np > 0 && no > 0) {
      const double p = ep / (double)np - o->noise_var, q = eo / (double)no - o->noise_var;
      s->comb_ratio = p > 0 ? (float)(q > 0 ? q / p : 0.0) : 0.f;
    }
  }

  /* Qm from equalised data REs */
  if (h->data_sym_mask && o->noise_var > 0) {
    int dsym[16], nd = 0;
    for (int l = 0; l < o->symbols; l++)
      if (((h->data_sym_mask >> l) & 1) && !((h->dmrs_sym_mask >> l) & 1)) dsym[nd++] = l;
    const long total = (long)nd * h->n_crb * 12;
    if (nd == 0 || total < 64) return;
    long st = total / NR_DL_QM_MAX_RE;
    if (st < 1) st = 1;
    if (st % 12 == 0) st++; /* avoid aliasing with the 12 subcarriers of a PRB */
    const int g0 = __builtin_ctz(h->cdm_groups_mask);
    const float beta = get_beta_dmrs(h->cdm_no_data, type == 2);
    float xr[NR_DL_QM_MAX_RE + 1], xi[NR_DL_QM_MAX_RE + 1];
    int ne = 0;
    double nvsum = 0;
    for (long i = 0; i < total && ne < NR_DL_QM_MAX_RE; i += st) {
      const int d = dsym[i / ((long)h->n_crb * 12)], q = (int)((i / 12) % h->n_crb), k = (int)(i % 12), prb = h->crb[q];
      int ms = 0;
      for (int j = 1; j < nsym; j++) if (abs(sym_l[j] - d) < abs(sym_l[ms] - d)) ms = j;
      const int lm = sym_l[ms];
      float hr[NR_DL_MAX_ANT], hi[NR_DL_MAX_ANT], S = 0;
      for (int a = 0; a < nant; a++) {
        float sr = 0, si = 0;
        for (int p = 0; p < pil; p++) {
          const int m = pilot_m(type, prb, p);
          const float rr = (1 - 2 * gb[ms][2 * m]) * 0.70710678f, ri = (1 - 2 * gb[ms][2 * m + 1]) * 0.70710678f;
          const c16_t y = o->rxF[a * o->symbols + lm][12 * prb + pilot_k(type, g0, p)];
          sr += y.r * rr + y.i * ri; /* y conj(r) */
          si += y.i * rr - y.r * ri;
        }
        hr[a] = sr / (pil * beta);
        hi[a] = si / (pil * beta);
        S += hr[a] * hr[a] + hi[a] * hi[a];
      }
      if (!(S > 0)) continue;
      float er = 0, ei = 0;
      for (int a = 0; a < nant; a++) {
        const c16_t y = o->rxF[a * o->symbols + d][12 * prb + k];
        er += hr[a] * y.r + hi[a] * y.i; /* conj(H) y */
        ei += hr[a] * y.i - hi[a] * y.r;
      }
      xr[ne] = er / S; xi[ne] = ei / S;
      nvsum += (double)o->noise_var / S * (1.0 + 1.0 / pil);
      ne++;
    }
    if (ne >= 64) {
      double e = 0;
      for (int i = 0; i < ne; i++) e += (double)xr[i] * xr[i] + (double)xi[i] * xi[i];
      if (!(e > 0) || !isfinite(e)) return; /* nothing to classify (all-zero or non-finite equalised data): undecided */
      const float sc = NR_QM_UNIT; /* x is the equalised symbol in units of the unit-mean-energy constellation (H carries the gain, beta removed) */
      c16_t eq[NR_DL_QM_MAX_RE];
      for (int i = 0; i < ne; i++) {
        /* a deep-fade RE (small S) gives a huge x: clamp instead of wrapping the int16 (the goodness-of-fit then rejects the grant) */
        const float fr = fminf(fmaxf(xr[i] * sc, -32767.f), 32767.f), fi = fminf(fmaxf(xi[i] * sc, -32767.f), 32767.f);
        eq[i].r = (int16_t)lrintf(fr);
        eq[i].i = (int16_t)lrintf(fi);
      }
      s->qm = nr_qm_classify(eq, ne, (float)(nvsum / ne), &s->qm_llr);
    }
  }
}

/* ---- constellation likelihood ---- */
#define Z2_ALPHA 3.2905 /* Phi^-1(1 - NBS_SIGNIF_ALPHA / 2): two-sided energy check at alpha */
#define QM_GOF_MAX 512 /* axis samples (I and Q pooled) used by the goodness-of-fit */
#define QM_GOF_SCALES 9
#define QM_GOF_SPAN 0.08f /* the equaliser gain is only known to a few percent: the scale is a nuisance parameter searched within +-8 % of NR_QM_UNIT */

/* log density of one axis (I or Q) of the unit-mean-energy square M-QAM (levels a (2j - M + 1)) in Gaussian noise of variance s2 */
static inline float axis_logp(float x, int M, float a, float s2)
{
  float dmin = FLT_MAX, d[16];
  for (int j = 0; j < M; j++) {
    const float df = x - a * (float)(2 * j - M + 1);
    d[j] = df * df / (2.0f * s2);
    if (d[j] < dmin) dmin = d[j];
  }
  float sum = 0;
  for (int j = 0; j < M; j++) sum += expf(dmin - d[j]);
  return -dmin + logf(sum) - logf((float)M) - 0.5f * logf(6.2831853f * s2);
}

/* CDF of one axis of the model */
static inline float axis_cdf(float x, int M, float a, float sg)
{
  float f = 0;
  for (int j = 0; j < M; j++) f += 0.5f * erfcf(-(x - a * (float)(2 * j - M + 1)) / (sg * 1.4142136f));
  return f / (float)M;
}

static int cmpf(const void *p, const void *q) { const float a = *(const float *)p, b = *(const float *)q; return (a > b) - (a < b); }

/* Goodness of fit of the winner: two-sided Kolmogorov-Smirnov test, at NBS_SIGNIF_ALPHA, of the 2n axis samples (I and Q pooled, first QM_GOF_MAX) against
 * the CDF of the winning constellation in noise noise_var: sqrt(N) D <= sqrt(ln(2 / alpha) / 2) (Kolmogorov, asymptotic). The likelihood ranking alone
 * is blind to mis-specification: if the real dispersion exceeds noise_var (noise_var too low, H-estimation error, an unmodelled layer) every model is
 * penalised and the densest wins; the KS test sees the cluster structure that the dense lattice cannot reproduce. The equaliser gain is known only to
 * a few percent, so D is minimised over QM_GOF_SCALES scales within +-QM_GOF_SPAN of NR_QM_UNIT (a nuisance parameter; the pass level is
 * not corrected for that, the tests bound the resulting false-accept rate). */
static int qm_gof_ok(const c16_t *eq, int n, float u, int M, float a, float s2)
{
  float x[QM_GOF_MAX];
  int N = 0;
  for (int i = 0; i < n && N + 2 <= QM_GOF_MAX; i++) { x[N++] = (float)eq[i].r; x[N++] = (float)eq[i].i; }
  if (N < 8) return 0;
  qsort(x, (size_t)N, sizeof(float), cmpf);
  const float sg = sqrtf(s2);
  /* energy of the model per axis sample: x = level + noise, E x^2 and Var x^2 (levels a (2j - M + 1), equiprobable); sample mean must agree within 3.29 sigma (two-sided alpha) */
  double Ea2 = 0, Ea4 = 0, Sx2 = 0;
  for (int j = 0; j < M; j++) { const double l2 = (double)a * (2 * j - M + 1) * a * (2 * j - M + 1); Ea2 += l2 / M; Ea4 += l2 * l2 / M; }
  const double mx2 = Ea2 + s2, vx2 = Ea4 + 6.0 * Ea2 * s2 + 3.0 * (double)s2 * s2 - mx2 * mx2;
  for (int i = 0; i < N; i++) Sx2 += (double)x[i] * x[i];
  Sx2 /= N;
  const double zlim = sqrt(vx2 / N) * Z2_ALPHA;
  const double lim = sqrt(0.5 * log(2.0 / NBS_SIGNIF_ALPHA)) / sqrt((double)N);
  for (int k = 0; k < QM_GOF_SCALES; k++) {
    const float uk = u * (1.0f + QM_GOF_SPAN * (2.0f * (float)k / (float)(QM_GOF_SCALES - 1) - 1.0f));
    if (fabs(Sx2 * (double)uk * uk - mx2) > zlim) continue; /* energy not explained by this scale and noise */
    double D = 0;
    for (int i = 0; i < N; i++) {
      const double F = axis_cdf(x[i] * uk, M, a, sg);
      const double d1 = fabs((double)(i + 1) / N - F), d0 = fabs(F - (double)i / N);
      if (d1 > D) D = d1;
      if (d0 > D) D = d0;
      if (D > lim) break;
    }
    if (D <= lim) return 1;
  }
  return 0;
}

/* Noise robustness. The unit-energy scale of the equalised symbols is known only to a few percent, so the symbols cannot tell noise_var from a slightly
 * different one at moderate SNR (a 64QAM blob at 15 dB is also a 256QAM blob in 3 dB less noise), and ranking the constellations at the stated noise_var
 * would call it 256QAM with a margin. The noise level is therefore a nuisance parameter on the grid QM_NOISE_MULT x noise_var (the true noise may exceed
 * the stated one up to 9 dB, of which the top level only serves as an edge detector: a winner whose best level is the top one is refused: H-estimation error, Doppler, an unmodelled layer all add dispersion, none removes it) and every constellation is ranked by
 * its profile likelihood L_q = max over the grid. A wrong constellation Q beats the true P at the grid point of the true noise by t with probability
 * <= exp(-t) (Markov on the likelihood ratio); Q may use any of the QM_NOISE_N levels, and there are 3 alternatives, so the margin must be
 * ln(3 QM_NOISE_N QM_SCALES / alpha) nats and a wrong certain answer has probability <= alpha. (The unit-energy scale is a further nuisance parameter
 * of each model, fitted on the same samples: QM_GOF_SCALES is the number of distinct scales it can take within its few-percent uncertainty.) The winner must also pass the KS goodness of fit at its noise level. */
#define QM_NOISE_N 7 /* sqrt(2) spacing over 1..8: the true noise is within a factor 2^(1/4) = 0.75 dB of a grid point whatever it is */
static const float QM_NOISE_MULT[QM_NOISE_N] = {1.0f, 1.4142136f, 2.0f, 2.8284271f, 4.0f, 5.6568542f, 8.0f};

int nr_qm_classify(const c16_t *eq, int n, float noise_var, float *llr)
{
  if (llr) *llr = 0;
  if (!eq || n < 1 || !(noise_var > 0) || !isfinite(noise_var)) return 0;
  double et = 0;
  for (int i = 0; i < n; i++) et += (double)eq[i].r * eq[i].r + (double)eq[i].i * eq[i].i;
  if (!(et > 0) || !isfinite(et)) return 0;
  { /* API contract: rms of eq within NR_QM_RMS_TOL of NR_QM_UNIT x sqrt(1 + noise) (the noise may exceed the stated one up to the top grid level) */
    const double r = sqrt(et / n) / NR_QM_UNIT;
    if (r < 1.0 - NR_QM_RMS_TOL || r > (1.0 + NR_QM_RMS_TOL) * sqrt(1.0 + 8.0 * (double)noise_var)) return 0;
  }
  double L[4];
  int bm[4], bk[4]; /* noise level and scale of each constellation's profile maximum */
  for (int qi = 0; qi < 4; qi++) {
    const int M = 2 << qi; /* levels per axis: 2, 4, 8, 16 */
    const float a = sqrtf(3.0f / (2.0f * ((float)M * M - 1.0f)));
    L[qi] = -INFINITY;
    bm[qi] = bk[qi] = 0;
    for (int mi = 0; mi < QM_NOISE_N; mi++) {
      const float s2 = 0.5f * noise_var * QM_NOISE_MULT[mi];
      for (int k = 0; k < QM_GOF_SCALES; k++) {
        const float u = (1.0f + QM_GOF_SPAN * (2.0f * (float)k / (float)(QM_GOF_SCALES - 1) - 1.0f)) / NR_QM_UNIT;
        double ll = 0;
        for (int i = 0; i < n; i++) ll += (double)axis_logp(eq[i].r * u, M, a, s2) + (double)axis_logp(eq[i].i * u, M, a, s2);
        if (ll > L[qi]) { L[qi] = ll; bm[qi] = mi; bk[qi] = k; }
      }
    }
  }
  int b = 0;
  for (int qi = 1; qi < 4; qi++) if (L[qi] > L[b]) b = qi;
  double m2 = -INFINITY;
  for (int qi = 0; qi < 4; qi++) if (qi != b && L[qi] > m2) m2 = L[qi];
  const double margin = L[b] - m2;
  if (llr) *llr = (float)margin;
  if (!(margin >= log(3.0 * QM_NOISE_N * QM_GOF_SCALES / NBS_SIGNIF_ALPHA))) return 0;
  if (bm[b] == QM_NOISE_N - 1) return 0; /* the winner wants the largest noise level of the grid: the real dispersion is beyond what the grid (and the bound) covers */
  const int M = 2 << b;
  const float a = sqrtf(3.0f / (2.0f * ((float)M * M - 1.0f)));
  return qm_gof_ok(eq, n, 1.0f / NR_QM_UNIT, M, a, 0.5f * noise_var * QM_NOISE_MULT[bm[b]]) ? 2 * (b + 1) : 0;
}
