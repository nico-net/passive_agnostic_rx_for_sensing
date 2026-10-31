/* SPDX-License-Identifier: LicenseRef-CSSL-1.0
 * PHASE-TRACK (D1 + D2, fix1031/phase): the arithmetic of the PDSCH channel time interpolation and of the per-symbol common phase error (CPE) tracker, written ONCE and
 * instantiated for the CPU (double, nr_pdsch_phase_track.c) and for the GPU kernels (double and float, nr_pdsch_gpu_demod.cu). Every operation is one of + - * and the four
 * macros below, in a fixed order, so the CPU (built with -ffp-contract=off) and the GPU (built with --fmad=false) evaluate the same expression tree: the double instantiations agree
 * bit for bit (the demod self-check compares CPU and GPU exactly), the float one is the FP32-class twin.
 *
 * The includer defines, before including this file (and undefines them after):
 *   NRPT_T        scalar type (double | float)
 *   NRPT_S        name suffix (_d | _f)
 *   NRPT_FN       storage class of the functions (static inline | static __device__ __forceinline__)
 *   NRPT_FMA(a,b,c)  a*b + c with one rounding
 *   NRPT_DIV(a,b)    IEEE a / b
 *   NRPT_SQRT(a)     IEEE sqrt
 *   NRPT_RND(v)      round to the nearest integer, ties away from zero (lround), as an int
 * and has `struct nrd_c16 {int16_t r, i;}` in scope (nr_pdsch_gpu_demod.cuh; c16_t has the same layout).
 *
 * What it does (physics in nr_pdsch_phase_track.h):
 *   - nrpt_delta_phasor / nrpt_blend_phasor / nrpt_interp_re: linear interpolation of the channel between two DM-RS symbols, phase-aligned: the pair's common phase Delta (per RX branch and
 *     per block of NRPT_BLK subcarriers, from sum_l sum_k hb conj(ha)) is removed from hb, the two are blended, and the blend is turned by the chord-normalised phasor
 *     ((1-t) + t e^{j Delta}) / |.| -- no trigonometry, so the CPU and the GPU round alike.
 *   - nrpt_zf: zero-forcing solve of one RE (nl <= 4 layers, nb <= 4 branches).
 *   - nrpt_cpe_decide: the CPE phasor of one layer from the sums of the clipped 4th-power statistic (blind, unbiased for square QAM at any SNR since E[s^2] = 0 and the noise is circular).
 */
#define NRPT_CAT_(a, b) a##b
#define NRPT_CAT(a, b) NRPT_CAT_(a, b)
#define NRPT(name) NRPT_CAT(nrpt_##name, NRPT_S)

#ifndef NRPT_CONSTS_DEFINED
#define NRPT_CONSTS_DEFINED
#define NRPT_BLK 48          /* subcarriers per phase block of the interpolation (and per partial sum of the CPE statistic) */
#define NRPT_MAXCH 72        /* >= ceil(MAX_EXT / NRPT_BLK) for 275 PRB */
#define NRPT_CPE_BLK 12      /* REs per partial sum of the CPE statistic (GPUPERF: 12 instead of the interpolation's 48 = four times the chunks, so the GPU spreads a symbol over the device and each thread's sequential chain is 12 REs) */
#define NRPT_CPE_MAXCH 276   /* >= ceil(MAX_EXT / NRPT_CPE_BLK) for 275 PRB */
#ifndef NRPT_MIN_RE
#define NRPT_MIN_RE 48       /* fewest valid REs of a symbol the CPE statistic is formed from */
#endif
#define NRPT_COND_MIN 0.05   /* ZF: smallest pivot relative to the largest diagonal of H^H H (noise gain <= 20 per layer) */
#define NRPT_CLIP2 4.0       /* an RE whose |x|^2 exceeds NRPT_CLIP2 x the layer's mean power is left out (noise outlier of an ill-conditioned RE) */
#define NRPT_COH2 4.5        /* |S|^2 > NRPT_COH2 Q: S = N c e^{j4phi} against a noise std sqrt(Q/2) per component: |S| / std > 3 */
#define NRPT_SIN_CAP 0.479425538604203  /* sin(0.5 rad): the largest CPE the tracker applies */
#define NRPT_COS_CAP 0.877582561890373
#endif

typedef struct { NRPT_T r, i; } NRPT(cx);

/* phase of hb relative to ha over one block: (cd, sd) = Cr + jCi normalised, Cr + jCi = sum hb conj(ha). (1, 0) for an empty correlation. */
NRPT_FN void NRPT(delta_phasor)(NRPT_T cr, NRPT_T ci, NRPT_T *cd, NRPT_T *sd)
{
  NRPT_T sc = (NRPT_T)1;
  if (sizeof(NRPT_T) == 4) { /* float: the sums reach 2^60 and more, the squares would overflow; scale by the larger component first */
    const NRPT_T ar = cr < 0 ? -cr : cr, ai = ci < 0 ? -ci : ci;
    sc = ar > ai ? ar : ai;
    if (sc > (NRPT_T)0) { cr = NRPT_DIV(cr, sc); ci = NRPT_DIV(ci, sc); }
  }
  const NRPT_T mag = NRPT_SQRT(NRPT_FMA(cr, cr, ci * ci));
  if (mag > (NRPT_T)0) {
    *cd = NRPT_DIV(cr, mag);
    *sd = NRPT_DIV(ci, mag);
  } else {
    *cd = (NRPT_T)1;
    *sd = (NRPT_T)0;
  }
}

/* the unit phasor u of the blend weight t in (0, 1) between two estimates whose relative phase is (cd, sd): u = ((1 - t) + t e^{j Delta}) / |.|. |.| >= |cos(Delta / 2)| > 0 unless Delta = pi. */
NRPT_FN void NRPT(blend_phasor)(NRPT_T t, NRPT_T cd, NRPT_T sd, NRPT_T *ur, NRPT_T *ui)
{
  const NRPT_T w = (NRPT_T)1 - t;
  const NRPT_T pr = NRPT_FMA(t, cd, w), pi_ = t * sd;
  const NRPT_T mag = NRPT_SQRT(NRPT_FMA(pr, pr, pi_ * pi_));
  if (mag > (NRPT_T)0) {
    *ur = NRPT_DIV(pr, mag);
    *ui = NRPT_DIV(pi_, mag);
  } else { /* Delta = pi: no direction to turn to */
    *ur = (NRPT_T)1;
    *ui = (NRPT_T)0;
  }
}

/* GPUPERF: hb conj(e^{j Delta}), the part of the interpolation that does not depend on the symbol (hoisted out of the symbol loop of the GPU kernel; same operations as before) */
NRPT_FN void NRPT(interp_rot)(struct nrd_c16 hb, NRPT_T cd, NRPT_T sd, NRPT_T *br, NRPT_T *bi)
{
  *br = NRPT_FMA((NRPT_T)hb.r, cd, (NRPT_T)hb.i * sd);
  *bi = NRPT_FMA((NRPT_T)hb.i, cd, -((NRPT_T)hb.r * sd));
}

/* one RE: h(t) = ((1 - t) ha + t hb e^{-j Delta}) u, rounded to int16 (saturated: the blend of two int16 values turned by a unit phasor cannot leave +-32768 * sqrt 2, the clamp is for that corner);
 * (br, bi) = interp_rot(hb) */
NRPT_FN struct nrd_c16 NRPT(interp_blend)(struct nrd_c16 ha, NRPT_T br, NRPT_T bi, NRPT_T t, NRPT_T ur, NRPT_T ui)
{
  const NRPT_T w = (NRPT_T)1 - t;
  const NRPT_T mr = NRPT_FMA(w, (NRPT_T)ha.r, t * br);
  const NRPT_T mi = NRPT_FMA(w, (NRPT_T)ha.i, t * bi);
  const NRPT_T or_ = NRPT_FMA(mr, ur, -(mi * ui));
  const NRPT_T oi = NRPT_FMA(mr, ui, mi * ur);
  long long vr = (long long)NRPT_RND(or_), vi = (long long)NRPT_RND(oi);
  vr = vr > 32767 ? 32767 : vr < -32768 ? -32768 : vr;
  vi = vi > 32767 ? 32767 : vi < -32768 ? -32768 : vi;
  struct nrd_c16 o;
  o.r = (int16_t)vr;
  o.i = (int16_t)vi;
  return o;
}

NRPT_FN struct nrd_c16 NRPT(interp_re)(struct nrd_c16 ha, struct nrd_c16 hb, NRPT_T t, NRPT_T cd, NRPT_T sd, NRPT_T ur, NRPT_T ui)
{
  NRPT_T br, bi;
  NRPT(interp_rot)(hb, cd, sd, &br, &bi);
  return NRPT(interp_blend)(ha, br, bi, t, ur, ui);
}

/* zero-forcing solve of one RE: x = (H^H H)^-1 H^H y by Gauss-Jordan without pivoting (H^H H is Hermitian positive definite). yr/yi [a], hr/hi [l][a] are the int16 values (as int).
 * 0 when the RE is ill-conditioned (a pivot below NRPT_COND_MIN of the largest diagonal): the CPE statistic does not take it.
 * GPUPERF: H^H H and H^H y are formed in integers (|sum| <= 2^33, exact), converted once, and only the upper triangle is formed (the lower one is its exact conjugate). In double this is bit for bit
 * what the former chain of fused multiply-adds on the int16 values gave (every product and partial sum is an integer below 2^53, so each fma was exact), at a tenth of the FP64 instructions. */
NRPT_FN int NRPT(zf)(int nl, int nb, const int *yr, const int *yi, const int hr[4][4], const int hi[4][4], NRPT_T *xr, NRPT_T *xi)
{
  NRPT_T gr[4][5], gi[4][5];
  NRPT_T gmax = (NRPT_T)0;
  for (int l = 0; l < nl; l++) {
    for (int k = l; k < nl; k++) {
      long long sr = 0, si = 0;
      for (int a = 0; a < nb; a++) { /* conj(h_l) h_k */
        sr += (long long)hr[l][a] * hr[k][a] + (long long)hi[l][a] * hi[k][a];
        si += (long long)hr[l][a] * hi[k][a] - (long long)hi[l][a] * hr[k][a];
      }
      gr[l][k] = (NRPT_T)sr;
      gi[l][k] = (NRPT_T)si;
      gr[k][l] = (NRPT_T)sr;
      gi[k][l] = (NRPT_T)(-si);
    }
    long long br = 0, bi = 0;
    for (int a = 0; a < nb; a++) { /* conj(h_l) y */
      br += (long long)hr[l][a] * yr[a] + (long long)hi[l][a] * yi[a];
      bi += (long long)hr[l][a] * yi[a] - (long long)hi[l][a] * yr[a];
    }
    gr[l][nl] = (NRPT_T)br;
    gi[l][nl] = (NRPT_T)bi;
    gmax = gr[l][l] > gmax ? gr[l][l] : gmax;
  }
  if (!(gmax > (NRPT_T)0))
    return 0;
  const NRPT_T pmin = (NRPT_T)NRPT_COND_MIN * gmax;
  for (int p = 0; p < nl; p++) {
    const NRPT_T d = gr[p][p]; /* real up to rounding: the Schur complement of a Hermitian positive definite matrix */
    if (!(d > pmin))
      return 0;
    const NRPT_T id = NRPT_DIV((NRPT_T)1, d);
    for (int c = p; c <= nl; c++) {
      gr[p][c] = gr[p][c] * id;
      gi[p][c] = gi[p][c] * id;
    }
    for (int r = 0; r < nl; r++) {
      if (r == p)
        continue;
      const NRPT_T fr = gr[r][p], fi = gi[r][p];
      for (int c = p; c <= nl; c++) {
        gr[r][c] = NRPT_FMA(-fr, gr[p][c], NRPT_FMA(fi, gi[p][c], gr[r][c]));
        gi[r][c] = NRPT_FMA(-fr, gi[p][c], NRPT_FMA(-fi, gr[p][c], gi[r][c]));
      }
    }
  }
  for (int l = 0; l < nl; l++) {
    xr[l] = gr[l][nl];
    xi[l] = gi[l][nl];
  }
  return 1;
}

/* pass 1 of the statistic for one RE of one layer: the power |x|^2 */
NRPT_FN NRPT_T NRPT(pow2)(NRPT_T xr, NRPT_T xi) { return NRPT_FMA(xr, xr, xi * xi); }

/* pass 2 for one RE of one layer, given g = 1 / mean power of the layer: t = (x^2 g)^2 into (*sr, *si) and |t|^2 into *q; false (nothing added) for an outlier */
NRPT_FN int NRPT(term4)(NRPT_T xr, NRPT_T xi, NRPT_T g, NRPT_T *tr, NRPT_T *ti, NRPT_T *tq)
{
  const NRPT_T p = NRPT(pow2)(xr, xi);
  if (p * g > (NRPT_T)NRPT_CLIP2)
    return 0;
  const NRPT_T u = NRPT_FMA(xr, xr, -(xi * xi)) * g, v = ((NRPT_T)2 * xr * xi) * g; /* x^2 / mean power */
  *tr = NRPT_FMA(u, u, -(v * v));
  *ti = (NRPT_T)2 * u * v;
  *tq = NRPT_FMA(*tr, *tr, *ti * *ti);
  return 1;
}

/* The CPE phasor e^{j phi} of one layer from S = sum t (the clipped 4th powers), Q = sum |t|^2 and the RE count n. S ~ n E[s^4] e^{j 4 phi} with E[s^4] < 0 for every square QAM and QPSK
 * (-1, -0.68, -0.62, -0.60), so 4 phi = arg(-S). Applied only when the estimate is coherent (|S| > 3 std) and significant (|phi| > 2 sigma_phi, sigma_phi = std / (4 |S|)), and capped at 0.5 rad.
 * Returns 1 with (*cr, *ci) = e^{j phi}, 0 (phasor 1, 0) when nothing is applied. */
NRPT_FN int NRPT(cpe_decide)(NRPT_T sr, NRPT_T si, NRPT_T q, uint32_t n, NRPT_T *cr, NRPT_T *ci)
{
  *cr = (NRPT_T)1;
  *ci = (NRPT_T)0;
  const NRPT_T m2 = NRPT_FMA(sr, sr, si * si);
  if (n < NRPT_MIN_RE || !(m2 > (NRPT_T)NRPT_COH2 * q) || !(m2 > (NRPT_T)0))
    return 0;
  const NRPT_T im = NRPT_DIV((NRPT_T)1, NRPT_SQRT(m2));
  const NRPT_T a = -sr * im, b = -si * im; /* e^{j 4 phi} */
  const NRPT_T h1 = (NRPT_T)0.5 * ((NRPT_T)1 + a), h2 = (NRPT_T)0.5 * ((NRPT_T)1 - a);
  const NRPT_T r2 = NRPT_SQRT(h1 > (NRPT_T)0 ? h1 : (NRPT_T)0);              /* e^{j 2 phi}, real part >= 0 */
  const NRPT_T i2 = b < (NRPT_T)0 ? -NRPT_SQRT(h2 > (NRPT_T)0 ? h2 : (NRPT_T)0) : NRPT_SQRT(h2 > (NRPT_T)0 ? h2 : (NRPT_T)0);
  const NRPT_T k2 = (NRPT_T)0.5 * ((NRPT_T)1 - r2);                          /* sin(phi) = +-sqrt((1 - cos 2phi) / 2), |phi| <= pi / 4 */
  const NRPT_T k2s = NRPT_SQRT(k2 > (NRPT_T)0 ? k2 : (NRPT_T)0);
  const NRPT_T i1 = i2 < (NRPT_T)0 ? -k2s : k2s;
  /* sigma_phi^2 = (Q / 2) / (16 |S|^2) = Q / (32 |S|^2). Below 1 sigma the estimate is noise: nothing applied. Above it the sine is shrunk by sigma^2 / sin (the Wiener-like weight
   * 1 - sigma^2 / phi^2: zero at 1 sigma, the full estimate well above it), so a CPE of the size of the estimator's own noise costs less than it would applied whole. */
  const NRPT_T k32 = (NRPT_T)32 * m2;
  if (!(i1 * i1 * k32 > q))
    return 0;
  NRPT_T sn = i1 - NRPT_DIV(q, k32 * i1);
  if (sn > (NRPT_T)NRPT_SIN_CAP || sn < -(NRPT_T)NRPT_SIN_CAP) {
    *cr = (NRPT_T)NRPT_COS_CAP;
    *ci = sn < (NRPT_T)0 ? -(NRPT_T)NRPT_SIN_CAP : (NRPT_T)NRPT_SIN_CAP;
    return 1;
  }
  const NRPT_T c2 = (NRPT_T)1 - sn * sn;
  *cr = NRPT_SQRT(c2 > (NRPT_T)0 ? c2 : (NRPT_T)0);
  *ci = sn;
  return 1;
}

/* rotate one int16 value by the unit phasor (c, s): the rounding of the MRC alignment (lround, then int16) */
NRPT_FN struct nrd_c16 NRPT(rot_re)(struct nrd_c16 v, NRPT_T c, NRPT_T s)
{
  const NRPT_T hr2 = NRPT_FMA((NRPT_T)v.r, c, -((NRPT_T)v.i * s));
  const NRPT_T hi2 = NRPT_FMA((NRPT_T)v.r, s, (NRPT_T)v.i * c);
  struct nrd_c16 o;
  o.r = (int16_t)NRPT_RND(hr2);
  o.i = (int16_t)NRPT_RND(hi2);
  return o;
}
