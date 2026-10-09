/* SPDX-License-Identifier: LicenseRef-CSSL-1.0
 * Stage-2 arithmetic ported from the frozen idsweep_offline.c (2026-09-30).
 * Fixed workspace, no allocation in processing. NID_FN is __device__ for CUDA.
 */
#pragma once
#include "nr_pdcch_nid_gpu.h"
#include <math.h>
#include <string.h>
#ifndef NID_FN
#define NID_FN
#endif
#define IDSW_NSYM 14
#define NID_HYP_CAP (1 << 20)
typedef nr_pdcch_nid_iq_t c16_t;
typedef struct {
 int phase, g0, ng, L, R, shift;
 double score;
 int al_win[5];
} st2_hyp_t;
typedef struct {
 double *re, *im, *p, *rr, *ri, *rp;
 int k;
} st2_snap_t;
struct NidView {
 int nsc, point_a_sc, css0_crb, css0_nrb, sps, n, dur;
 int begin, capacity, stride;
 const int *slot, *sym;
 const c16_t *re;
 /* NidView::index: maps a relative index k (0..n-1) of the view to its slot in the circular capture buffer (begin + k) mod capacity.
  *   Used by grid() and by the stage-2 build loops.
  */
 NID_FN int index(int k) const { return (begin+k)%capacity; }
 /* NidView::grid: returns the sample block of relative index k and duration offset d: the capture's FFT grid at that slot and symbol, stride apart. Callers: st2_build() and the stage-1 code. */
 NID_FN const c16_t *grid(int k, int d=0) const {
   return re + ((size_t)index(k)*dur+d)*stride;
 }
};
/* nid_gold: advances the length-31 Gold sequence generator (TS 38.211 5.2.1) by one 32-bit word; with reset set it first initialises x1 and x2 from the seed in x2 and runs 50 warm-up steps.
 *   Returns x1 XOR x2. Same arithmetic as the frozen offline tool. Callers: st2_build() and the stage-1 scrambling code.
 */
NID_FN inline uint32_t nid_gold(uint32_t *x1, uint32_t *x2, int reset) {
 if (reset) {
  *x1=1+(1U<<31);
  *x2=*x2 ^ ((*x2 ^ (*x2>>1) ^ (*x2>>2) ^ (*x2>>3))<<31);
  for(int i=1;i<50;i++) nid_gold(x1,x2,0);
 }
 *x1=(*x1>>1)^(*x1>>4); *x1=*x1^(*x1<<31)^(*x1<<28);
 *x2=(*x2>>1)^(*x2>>2)^(*x2>>3)^(*x2>>4);
 *x2=*x2^(*x2<<31)^(*x2<<30)^(*x2<<29)^(*x2<<28);
 return *x1^*x2;
}
/* idsw_pilot: returns the DM-RS-like pilot value (xr, xi) at position m of a Gold-derived byte array (the QPSK pattern of the scrambling sequence, index swapped as in the offline tool).
 *   Pure function. Caller: st2_build().
 */
NID_FN void idsw_pilot(const uint32_t *gold, int m, float *xr, float *xi)
{
  const float tab[4][2] = {{1, -1}, {1, 1}, {-1, -1}, {-1, 1}};
  const int swap2bits[4] = {0, 2, 1, 3};
  const uint8_t b = ((const uint8_t *)gold)[m / 4];
  const int idx = swap2bits[(b >> ((m * 2) & 7)) & 3];
  *xr = tab[idx][0];
  *xi = tab[idx][1];
}
struct NidStage2 {
 NidView g_idsw;
 int g_st2_clo, g_st2_chi, g_st2_D;
 st2_snap_t *sn, *dec[3];
 st2_snap_t tmp;
 st2_hyp_t *H;
 double *vr, *vd;
 int nact, nid, sym0, nh, alive;
 unsigned lit_hist[276];
/* idsw_cinit: c_init of the DM-RS sequence of one symbol l in slot `slot` for identity nid: (sps * slot + l + 1) * (2 nid + 1) * 2^17 + 2 nid, modulo 2^31.
 *   Uses the slots-per-subframe value of the view. Caller: st2_build().
 */
NID_FN uint32_t idsw_cinit(int nid, int slot, int l)
{
  uint64_t x = ((uint64_t)g_idsw.sps * slot + l + 1) * ((uint64_t)(nid << 1) + 1);
  x <<= 17;
  x += (uint64_t)(nid << 1);
  return (uint32_t)(x % (1U << 31));
}
/* st2_exc: excess correlation of the window [b, b+B) of a snapshot: the normalised energy of the summed pilot correlation over the window, minus its noise expectation, scaled to 1 for a perfect match and 0 for noise.
 *   Returns 0 outside the valid CRB range or when the window has no energy. Callers: st2_cand(), st2_remove() and consistency_vote() (same arithmetic).
 */
NID_FN double st2_exc(const st2_snap_t *s, int b, int B)
{
  if (b < g_st2_clo || b + B > g_st2_chi)
    return 0.0;
  const double re = s->re[b + B] - s->re[b], im = s->im[b + B] - s->im[b], p = s->p[b + B] - s->p[b];
  if (p <= 0.0)
    return 0.0;
  const double n = 3.0 * B * g_st2_D, rho2 = (re * re + im * im) / (p * 2.0 * n);
  return (rho2 - 1.0 / n) / (1.0 - 1.0 / n);
}
/* st2_cand: mean excess correlation of one candidate (CCE index cce, aggregation level al) under hypothesis h on snapshot s.
 *   Maps the CCEs to their REG bundles (time-first REG numbering of TS 38.211 7.3.2.2, with the hypothesis' interleaver R and shift when L is set), and averages st2_exc() over those bundles. Caller: st2_score(), st2_per_snap(), st2_remove().
 */
NID_FN double st2_cand(const st2_snap_t *s, const st2_hyp_t *h, int cce, int al)
{
  /* TS 38.211 7.3.2.2: REGs are numbered time-first (REG n = RB n/D, symbol n%D), a REG bundle is L consecutive
   * REGs, so a bundle spans L/D RBs on ALL D symbols; N_REG = 6*groups*D. D = 1 is the original geometry. */
  const int D = g_st2_D, b = h->L ? h->L : 6, nb = 6 * h->ng * D / b, cs = h->phase + 6 * h->g0;
  double sum = 0.0;
  int cnt = 0;
  for (int j = cce * 6 / b; j < (cce + al) * 6 / b; j++) {
    const int f = h->L ? ((j % h->R) * (nb / h->R) + j / h->R + h->shift) % nb : j;
    sum += st2_exc(s, cs + f * b / D, b / D);
    cnt++;
  }
  return cnt ? sum / cnt : 0.0;
}
/* st2_score: scores hypothesis h over nact snapshots: for each snapshot takes the best candidate of every aggregation level (1 to 16 CCEs) and sums the best values; the winning level of each snapshot is counted in al_win.
 *   Writes h->score and h->al_win. Caller: NidStage2::score().
 */
NID_FN void st2_score(st2_hyp_t *h, const st2_snap_t *sn, int nact)
{
  const int als[5] = {1, 2, 4, 8, 16};
  h->score = 0.0;
  memset(h->al_win, 0, sizeof(h->al_win));
  for (int k = 0; k < nact; k++) {
    double best = -1.0;
    int bal = -1;
    for (int ai = 1; ai < 5; ai++)
      for (int cce = 0; cce + als[ai] <= h->ng * g_st2_D; cce += als[ai]) {
        const double v = st2_cand(&sn[k], h, cce, als[ai]);
        if (v > best) {
          best = v;
          bal = ai;
        }
      }
    if (bal >= 0) {
      h->score += best;
      h->al_win[bal]++;
    }
  }
}
/* st2_prefix_body: rebuilds the prefix sums (re, im, p) of a snapshot from its per-CRB terms (rr, ri, rp). Same body as st2_prefix(); used by st2_build(). */
NID_FN void st2_prefix_body(st2_snap_t *s)
{
  s->re[0] = s->im[0] = s->p[0] = 0.0;
  for (int c = 0; c < g_st2_chi; c++) {
    s->re[c + 1] = s->re[c] + s->rr[c];
    s->im[c + 1] = s->im[c] + s->ri[c];
    s->p[c + 1] = s->p[c] + s->rp[c];
  }
}
/* st2_build: builds one snapshot for capture slot k and identity nid: the pilot-derotated correlation of every CRB, summed over the D symbols of the window, then the prefix sums.
 *   The DM-RS sequence of each symbol uses its own c_init (idsw_cinit). Callers: NidStage2::prepare() (candidate and decoy identities).
 */
NID_FN void st2_build(int k, int nid, st2_snap_t *s)
{
  const int pa = g_idsw.point_a_sc, words = (2 * 3 * g_st2_chi) / 32 + 2;
  const int D = g_st2_D <= g_idsw.dur ? g_st2_D : g_idsw.dur; /* never read past the symbols the capture holds */
  s->re[0] = s->im[0] = s->p[0] = 0.0;
  for (int c = 0; c < g_st2_chi; c++)
    s->rr[c] = s->ri[c] = s->rp[c] = 0.0; /* every caller allocates through st2_alloc() */
  /* per CRB: the pilot-derotated correlation SUMMED over the D symbols of the window (coherent within a REG
   * bundle, which spans all D symbols), each symbol with its own DM-RS sequence (c_init depends on the symbol) */
  for (int d = 0; d < D; d++) {
    uint32_t gold[54], x1 = 0, x2 = idsw_cinit(nid, g_idsw.slot[g_idsw.index(k)], g_idsw.sym[g_idsw.index(k)] + d);
    gold[0] = nid_gold(&x1, &x2, 1);
    for (int w = 1; w < words; w++)
      gold[w] = nid_gold(&x1, &x2, 0);
    const c16_t *re = g_idsw.grid(k, d);
    for (int c = 0; c < g_st2_chi; c++) {
      const bool masked = c >= g_idsw.css0_crb && c < g_idsw.css0_crb + g_idsw.css0_nrb;
      if (c < g_st2_clo || masked)
        continue;
      double a = 0, b = 0, p = 0;
      for (int q = 0; q < 3; q++) {
        float xr, xi;
        idsw_pilot(gold, 3 * c + q, &xr, &xi);
        const c16_t y = re[pa + 12 * c + 1 + 4 * q];
        a += y.r * xr - y.i * xi;
        b += y.r * xi + y.i * xr;
        p += (double)y.r * y.r + (double)y.i * y.i;
      }
      s->rr[c] += a;
      s->ri[c] += b;
      s->rp[c] += p;
    }
  }
  st2_prefix_body(s);
  s->k = k;
}
/* st2_prefix: rebuilds the prefix sums of a snapshot from its per-CRB terms (rr, ri, rp). Called after a removal in st2_remove(). */
NID_FN void st2_prefix(st2_snap_t *s)
{
  s->re[0] = s->im[0] = s->p[0] = 0.0;
  for (int c = 0; c < g_st2_chi; c++) {
    s->re[c + 1] = s->re[c] + s->rr[c];
    s->im[c + 1] = s->im[c] + s->ri[c];
    s->p[c + 1] = s->p[c] + s->rp[c];
  }
}
/* st2_per_snap: best candidate value of each snapshot under hypothesis h (aggregation levels 2 to 16), written to out[0..nact-1]. Used for the paired test in NidStage2::finish(). */
NID_FN void st2_per_snap(const st2_hyp_t *h, const st2_snap_t *sn, int nact, double *out)
{
  const int als[4] = {2, 4, 8, 16};
  for (int k = 0; k < nact; k++) {
    double best = 0.0;
    for (int ai = 0; ai < 4; ai++)
      for (int cce = 0; cce + als[ai] <= h->ng * g_st2_D; cce += als[ai]) {
        const double v = st2_cand(&sn[k], h, cce, als[ai]);
        if (v > best) best = v;
      }
    out[k] = best;
  }
}
/* st2_paired_t: paired t-statistic of the per-snapshot differences a[i] - b[i] over n snapshots (zero when the variance is zero).
 *   Pure function. Caller: NidStage2::finish() (real vs decoy identities).
 */
NID_FN double st2_paired_t(const double *a, const double *b, int n)
{
  double m = 0, v = 0;
  for (int i = 0; i < n; i++) m += a[i] - b[i];
  m /= n;
  for (int i = 0; i < n; i++) v += (a[i] - b[i] - m) * (a[i] - b[i] - m);
  v = n > 1 ? v / (n - 1) : 0;
  return v > 0 ? m / sqrt(v / n) : 0.0;
}
/* st2_remove: removes the energy of the lit candidates of hypothesis h from snapshot s, so that a second CORESET of the same identity is not claimed twice; returns the number of removed candidates.
 *   Only aggregation levels 2 to 16 are removed, and a candidate is removed only when every one of its bundles is above 2 sigma. Caller: NidStage2::finish().
 */
NID_FN int st2_remove(st2_snap_t *s, const st2_hyp_t *h)
{
  const int als[5] = {1, 2, 4, 8, 16};
  const int D = g_st2_D, b = h->L ? h->L : 6, nb = 6 * h->ng * D / b, cs = h->phase + 6 * h->g0;
  const double n = 3.0 * b, t = 1.0 - pow(1e-3, 1.0 / (n - 1.0)); /* REs per bundle = 3 per REG whatever D */
  int removed = 0;
  /* AL >= 2 only: those candidates are the hypothesis's evidence. An AL1 candidate (one 6-RB bundle)
   * fits every mapping with 6-RB bundles, so letting it claim energy wiped out a second, interleaved
   * CORESET of the same nID (selftest3, CORESET C). */
  for (int ai = 4; ai >= 1; ai--)
    for (int cce = 0; cce + als[ai] <= h->ng * D; cce += als[ai]) {
      /* candidate-level test: under noise each bundle's excess has sd ~ 1/(n-1) (Beta(1,n-1)), so the
       * mean over m bundles has sd 1/((n-1) sqrt m); lit if the candidate mean exceeds 3.09 sd (1e-3) */
      const int m = als[ai] * 6 / b;
      const double thr = 3.09 / ((n - 1.0) * sqrt((double)m));
      bool all = st2_cand(s, h, cce, als[ai]) > thr;
      /* AND every bundle above 2 sd (1/(n-1) each): a half-lit candidate -- one real bundle next to an
       * empty one -- must not claim the real bundle (it swallowed a second CORESET in selftest3) */
      for (int j = cce * 6 / b; j < (cce + als[ai]) * 6 / b && all; j++) {
        const int f = h->L ? ((j % h->R) * (nb / h->R) + j / h->R + h->shift) % nb : j;
        all = st2_exc(s, cs + f * b / D, b / D) > 2.0 / (n - 1.0);
      }
      (void)t;
      if (!all) continue;
      for (int j = cce * 6 / b; j < (cce + als[ai]) * 6 / b; j++) {
        const int f = h->L ? ((j % h->R) * (nb / h->R) + j / h->R + h->shift) % nb : j;
        for (int c = cs + f * b / D; c < cs + f * b / D + b / D; c++) s->rr[c] = s->ri[c] = s->rp[c] = 0.0;
      }
      st2_prefix(s);
      removed++;
    }
  return removed;
}
/* st2_gen: generates hypotheses (phase, group start, group count, bundle length L, interleaver R, shift) for the CRB window [qlo, qhi) of one phase range, into H (up to cap).
 *   Also emits the non-interleaved geometry for every phase. Returns the number generated. Caller: hull_gen().
 */
NID_FN size_t st2_gen(st2_hyp_t *H, size_t cap, int qlo, int qhi, int phlo=0, int phhi=6)
{
  size_t nh = 0;
  for (int ph = phlo; ph < phhi; ph++) {
    for (int cs = ph; cs + 6 <= g_st2_chi && nh < cap; cs += 6) {
      if (cs < g_st2_clo) continue;
      if (H) H[nh] = (st2_hyp_t){ph, (cs - ph) / 6, (g_st2_chi - cs) / 6, 0, 0, 0, 0.0, {0}};
      nh++;
    }
    const int gl = (int)floor((qlo - ph) / 6.0), gh = (int)floor((qhi - ph) / 6.0);
    /* a lit 6-CRB window can start up to 5 CRBs before the CORESET and end up to 5 after it, so the
     * window->group conversion is uncertain by one group at each edge */
    for (int g0 = gl - 2; g0 <= gl + 1; g0++)
      for (int ge = gh - 1; ge <= gh + 2; ge++) {
        int a0 = g0, a1 = ge;
        while (ph + 6 * a0 < g_st2_clo) a0++;
        while (ph + 6 * (a1 + 1) > g_st2_chi) a1--;
        const int ng = a1 - a0 + 1;
        if (ng < 2 || (a0 != g0 && g0 != gl) || (a1 != ge && ge != gh)) continue;
        /* L is a multiple of the duration (38.211 7.3.2.2): {2,6} for D = 1, 2; {3,6} for D = 3 */
        const int Ls[2] = {g_st2_D == 3 ? 3 : 2, 6};
        const int Rs[3] = {2, 3, 6};
        for (int li = 0; li < 2; li++)
          for (int ri = 0; ri < 3; ri++) {
            const int nb = 6 * ng * g_st2_D / Ls[li];
            if (nb % Rs[ri]) continue;
            for (int sh = 0; sh < nb && nh < cap; sh++) {
              if (H) H[nh] = (st2_hyp_t){ph, a0, ng, Ls[li], Rs[ri], sh, 0.0, {0}};
              nh++;
            }
          }
      }
  }
  return nh;
}

 /* st2_lit: true when any 6-CRB window of snapshot s carries energy above the lit threshold (1e-4 false-alarm rate). With histogram set it also counts the lit windows per CRB in lit_hist.
  *   Caller: prepare() (lit symbol selection) and hull_gen().
  */
 NID_FN bool st2_lit(const st2_snap_t *s, bool histogram=false) {
  const double t=1.0-pow(1e-4,1.0/(18.0*g_st2_D-1.0));
  bool any=false;
  for(int c=g_st2_clo;c+6<=g_st2_chi;c++) {
   double r=s->re[c+6]-s->re[c], i=s->im[c+6]-s->im[c], p=s->p[c+6]-s->p[c];
   if(p>0 && (r*r+i*i)/(p*36.0*g_st2_D)>t) {
    any=true; if(histogram) lit_hist[c]++;
   }
  }
  return any;
 }
 /* hull_gen: generates the hypothesis set from the histogram of lit CRBs over all snapshots: the central 96 % of the lit CRB range is the window the hypotheses are generated for.
  *   Returns the number of hypotheses (0 when nothing is lit). Caller: NidStage2::prepare() and NidStage2::finish() (after removals).
  */
 NID_FN int hull_gen() {
  memset(lit_hist,0,sizeof(lit_hist));
  for(int k=0;k<nact;k++) st2_lit(sn+k,true);
  unsigned n=0; for(int c=0;c<g_st2_chi;c++) n+=lit_hist[c];
  if(!n) return 0;
  unsigned lo=(unsigned)(0.02*(n-1)), hi=(unsigned)(0.98*(n-1)), acc=0;
  int qlo=0,qhi=0;
  for(int c=0;c<g_st2_chi;c++) {
   if(acc<=lo && acc+lit_hist[c]>lo) qlo=c;
   if(acc<=hi && acc+lit_hist[c]>hi) qhi=c+5;
   acc+=lit_hist[c];
  }
  return (int)st2_gen(H,NID_HYP_CAP,qlo,qhi);
 }
 /* consistency_vote: per-snapshot vote for hypothesis h: 1 when more of its REG-bundle groups are lit than not, 0 for the opposite, -1 when no group is lit. Does not change state.
  *   Caller: NidStage2::consistent().
  */
 NID_FN int consistency_vote(const st2_hyp_t *h,int k) const {
  const int D=g_st2_D,L=h->L?h->L:6,R=h->L?h->R:1,b=L/D;
  const int nb=6*h->ng*D/L,cs=h->phase+6*h->g0,per=6/L;
  const double thr=2.0/(3.0*L-1.0);

   int full=0,partial=0;
   for(int j0=0;j0+per<=nb;j0+=per) {
    int hit=0;
    for(int dj=0;dj<per;dj++) {
     int j=j0+dj,f=h->L?((j%R)*(nb/R)+j/R+h->shift)%nb:j;
     // Identical excess arithmetic; this method does not change state.
     const st2_snap_t *s=sn+k; int x=cs+f*b;
     double r=s->re[x+b]-s->re[x],i=s->im[x+b]-s->im[x],p=s->p[x+b]-s->p[x];
     double n=3.0*b*D, e=p>0?((r*r+i*i)/(p*2.0*n)-1.0/n)/(1.0-1.0/n):0;
     hit+=e>thr;
    }
    if(hit==per) full++; else if(hit) partial++;
   }
   return full+partial ? (full<partial?0:1) : -1;
 }
 /* consistent: true when a majority of the snapshots that vote do not vote against hypothesis h (at least half of the voting snapshots are not below). Caller: NidStage2::score(). */
 NID_FN bool consistent(const st2_hyp_t *h) const {
  int nf=0,below=0;
  for(int k=0;k<nact;k++) {
   int v=consistency_vote(h,k);nf+=v>=0;below+=v==0;
  }
  // sorted frac[nf/2] >= .5, without malloc or sort.
  return nf && below<=nf/2;
 }
 /* prepare: loads one stage-2 identity: builds the lit snapshots of the symbol group at sym (choosing the window duration D from the lit ratio of the next symbols when duration is 0), the decoy snapshots, and the hypothesis set.
  *   Sets alive when at least one hypothesis exists. Entry point of the stage-2 search for one identity nid. Callers: the CPU reference and the CUDA stage-2 kernel (nr_pdcch_nid_gpu.cu).
  */
 NID_FN void prepare(int id,int sym,int duration=0) {
  nid=id; sym0=sym; nact=0; alive=0;
  const int pa=g_idsw.point_a_sc;
  g_st2_clo=pa>=0?0:(-pa+11)/12; g_st2_chi=(g_idsw.nsc-pa)/12; g_st2_D=1;
  int dur=duration;
  if (!dur) {
  int nsym[14]={0},nlit[14]={0};
  for(int k=0;k<g_idsw.n;k++) {
   int s=g_idsw.sym[g_idsw.index(k)];
   if(s<sym0 || s>sym0+2) continue;
   st2_build(k,nid,&tmp);nsym[s]++;nlit[s]+=st2_lit(&tmp);
  }
  double r0=nsym[sym0]?(double)nlit[sym0]/nsym[sym0]:0;
  dur=1;
  while(dur<3 && sym0+dur<14 && nsym[sym0+dur] && (double)nlit[sym0+dur]/nsym[sym0+dur]>=0.5*r0) dur++;
  }
  if(dur<1 || dur>g_idsw.dur || sym0+dur>14) return;
  g_st2_D=dur;
  for(int k=0;k<g_idsw.n;k++) {
   if(g_idsw.sym[g_idsw.index(k)]!=sym0) continue;
   st2_build(k,nid,&tmp);
   if(!st2_lit(&tmp)) continue;
   st2_build(k,nid,sn+nact);
   int ids[3]={(nid+7919)&65535,(nid+30011)&65535,(nid^0x5a5a)&65535};
   for(int d=0;d<3;d++) st2_build(k,ids[d],dec[d]+nact);
   nact++;
  }
  if(nact) {nh=hull_gen();alive=nh>0;}
 }
 /* score: scores hypothesis i (the score is zeroed when it is not consistent). Caller: the stage-2 search loop (CPU reference and nr_pdcch_nid_gpu.cu). */
 NID_FN void score(int i) {
  st2_score(H+i,sn,nact);
  if(!consistent(H+i)) H[i].score=0;
 }
 /* finish: decides the identity: picks the best-scoring hypothesis, checks it against the decoy identities with a paired t-test (threshold 5), and records the result (up to NR_PDCCH_NID_MAX_RECORDS) with its tie count.
  *   Then removes its energy from the snapshots and regenerates the hypotheses for the next CORESET. Returns true when a record was produced. Caller: the stage-2 search loop.
  */
 NID_FN bool finish(nr_pdcch_nid_result_t *out) {
  if(!alive) return false;
  int best=0;
  for(int i=1;i<nh;i++) if(H[i].score>H[best].score) best=i;
  const st2_hyp_t win=H[best];
  if(win.score<=0) {alive=0;return false;}
  st2_per_snap(&win,sn,nact,vr);
  for(int d=0;d<3;d++) {
   st2_per_snap(&win,dec[d],nact,vd);
   if(st2_paired_t(vr,vd,nact)<5.0) {alive=0;return false;}
  }
  // glibc qsort is stable on this POD array. Retain its score/index tie order
  // when collecting the first 64 members of the 1e-9 equivalence class.
  int ties=0, chosen[64]; st2_hyp_t handoff=win;
  for(int i=0;i<nh;i++) {
   const st2_hyp_t &h=H[i];
   if(h.L!=win.L || h.R!=win.R || h.phase!=win.phase || fabs(h.score-win.score)>1e-9*(1+fabs(win.score))) continue;
   int at=0;while(at<ties && H[chosen[at]].score>=h.score)at++;
   if(at>=64)continue;
   if(ties<64)ties++;
   for(int j=ties-1;j>at;j--)chosen[j]=chosen[j-1];
   chosen[at]=i;
  }
  for(int t=0;t<ties;t++) {
   const st2_hyp_t &h=H[chosen[t]];
   if(!win.L && h.phase+6*h.g0<handoff.phase+6*handoff.g0) {
    handoff=h;handoff.ng=(g_st2_chi-(handoff.phase+6*handoff.g0))/6;
   }
  }
  if(out->count<NR_PDCCH_NID_MAX_RECORDS && g_idsw.point_a_sc%12==0) {
   memcpy(out->al_win[out->count],win.al_win,sizeof(win.al_win));
   nr_pdcch_nid_record_t &r=out->records[out->count++];
   r={nid,sym0,g_st2_D,handoff.phase+6*handoff.g0+g_idsw.point_a_sc/12,handoff.ng,handoff.L,handoff.R,handoff.shift,ties};
  }
  int removed=0;for(int k=0;k<nact;k++) removed+=st2_remove(sn+k,&win);
  if(removed) {nh=hull_gen();alive=nh>0;} else alive=0;
  return true;
 }
};
