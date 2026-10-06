/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_csirs_disc_fp_internal.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Local period table avoids coupling U2 to the ring implementation's table. */
static const int periods[] = {4,5,8,10,16,20,32,40,64,80,160,320,640};
#define Q 4096.0
#define CAND_CAP 512

typedef struct { nr_csirs_disc_fp_t fp; double significance; } candidate_t;

int nr_disc_fp_valid(const nr_csirs_disc_ring_t *r)
{
  return r && r->grid_base && r->meta && r->capacity > 0 && r->n_ant > 0 && r->n_ant <= 4 &&
         r->n_rb > 0 && r->n_rb <= NR_CSIRS_DISC_MAX_RB &&
         r->slot_stride >= (size_t)r->n_ant * 14 * 24 * r->n_rb;
}

/* A same-slot/RB reference removes random scheduled bandwidth and slot power.
 * Pool all symbols: a same-symbol-only denominator erases dense (e.g. row 9)
 * pilots. The subsequent on/off test uses each RE's own history, removing
 * stationary frequency selectivity and permanent DM-RS combs. Excluded REs
 * participate in neither numerator nor reference. Cap impulsive outliers. */
void nr_disc_fp_normalize(const nr_csirs_disc_ring_t *r, const nr_csirs_disc_fp_cfg_t *cfg, uint32_t *x)
{
  const int nk = 12*r->n_rb, nr = 14*nk+168;
  for (int t=0; t<r->capacity; ++t) {
    if (!r->meta[t].valid) continue;
    for (int rb=0; rb<r->n_rb; ++rb) {
      uint64_t power[168], total=0;
      int count=0;
      for (int l=0; l<14; ++l) for (int s=0; s<12; ++s) {
        int k=12*rb+s, j=l*12+s;
        power[j]=0;
        if (cfg && ((cfg->excl_sym_mask_any & (1<<l)) || (cfg->excl_re && cfg->excl_re[l*nk+k]))) continue;
        for (int a=0; a<r->n_ant; ++a) {
          const int16_t *v=nr_csirs_disc_ring_sym(r,t,a,l)+2*k;
          power[j]+=(int64_t)v[0]*v[0]+(int64_t)v[1]*v[1];
        }
        total+=power[j]; ++count;
      }
      for (int l=0; l<14; ++l) for (int s=0; s<12; ++s) {
        uint64_t v=total ? power[l*12+s]*count*4096/total : 0;
        x[(size_t)t*nr+l*nk+12*rb+s]=(uint32_t)(v>8192 ? 8192 : v);
      }
    }
    for (int l=0;l<14;++l) for (int sc=0;sc<12;++sc) {
      uint64_t sum=0;
      for(int rb=0;rb<r->n_rb;++rb) sum+=x[(size_t)t*nr+l*nk+12*rb+sc];
      x[(size_t)t*nr+14*nk+l*12+sc]=sum/r->n_rb;
    }
  }
}

typedef struct { const nr_csirs_disc_ring_t *r; const uint32_t *x; } fold_ctx_t;
static int fold_cpu(void *opaque, int p, uint64_t *sum, uint64_t *sq)
{
  const fold_ctx_t *c=opaque;
  int nr=14*12*c->r->n_rb+168;
  for (int t=0; t<c->r->capacity; ++t) if(c->r->meta[t].valid) {
    size_t b=(size_t)(c->r->meta[t].abs_slot%p)*nr;
    const uint32_t *v=c->x+(size_t)t*nr;
    for (int j=0;j<nr;++j) { sum[b+j]+=v[j]; sq[b+j]+=(uint64_t)v[j]*v[j]; }
  }
  return 0;
}

static int gcd_period(int a,int b)
{
  while(b) {int t=a%b;a=b;b=t;}return a;
}

static int rank_candidate(const void *a, const void *b)
{
  const candidate_t *x=a,*y=b;
  if(x->significance!=y->significance) return x->significance<y->significance ? 1:-1;
  if(x->fp.period!=y->fp.period) return x->fp.period-y->fp.period;
  if(x->fp.offset!=y->fp.offset) return x->fp.offset-y->fp.offset;
  if(x->fp.sym_mask!=y->fp.sym_mask) return x->fp.sym_mask-y->fp.sym_mask;
  return x->fp.rb_start-y->fp.rb_start;
}
static int rank_strength(const void *a, const void *b)
{
  const nr_csirs_disc_fp_t *x=a,*y=b;
  if(x->strength!=y->strength) return x->strength<y->strength ? 1:-1;
  if(x->period!=y->period) return x->period-y->period;
  if(x->offset!=y->offset) return x->offset-y->offset;
  return x->sym_mask-y->sym_mask;
}

int nr_disc_fp_cluster(const nr_csirs_disc_ring_t *r, const nr_csirs_disc_fp_cfg_t *cfg,
                       nr_csirs_disc_fp_t *out, int max_out, nr_disc_fold_fn fold, void *ctx)
{
  int nk=12*r->n_rb, nr=14*nk+168, nc=0, nt=0;
  for(int t=0;t<r->capacity;++t) nt+=!!r->meta[t].valid;
  if(nt<12) return 0;
  candidate_t *cand=calloc(CAND_CAP,sizeof(*cand));
  uint64_t *tot=calloc(nr,sizeof(*tot)), *totq=calloc(nr,sizeof(*totq));
  if(!cand || !tot || !totq) { free(cand); free(tot); free(totq); return -1; }
  float min_strength=cfg && cfg->min_strength>0 ? cfg->min_strength:2.0f;
  for(unsigned pi=0;pi<sizeof(periods)/sizeof(*periods);++pi) {
    int p=periods[pi], counts[640]={0};
    for(int t=0;t<r->capacity;++t) if(r->meta[t].valid) ++counts[r->meta[t].abs_slot%p];
    int usable=0;
    for(int o=0;o<p;++o) usable+=counts[o]>=3 && nt-counts[o]>=3;
    if(!usable) continue;
    uint64_t *sum=calloc((size_t)p*nr,sizeof(*sum)), *sq=calloc((size_t)p*nr,sizeof(*sq));
    if(!sum || !sq || fold(ctx,p,sum,sq)<0) {
      free(sum); free(sq); free(cand); free(tot); free(totq); return -1;
    }
    memset(tot,0,nr*sizeof(*tot)); memset(totq,0,nr*sizeof(*totq));
    for(int o=0;o<p;++o) for(int j=0;j<nr;++j) {tot[j]+=sum[(size_t)o*nr+j];totq[j]+=sq[(size_t)o*nr+j];}
    for(int o=0;o<p;++o) {
      int on=counts[o], off=nt-on;
      if(on<3 || off<3) continue;
      const uint64_t *s=sum+(size_t)o*nr, *q=sq+(size_t)o*nr;
      for(int l=0;l<14;++l) {
        if(cfg && (cfg->excl_sym_mask_any&(1<<l))) continue;
        /* Detect the comb from pooled RB evidence before locating edges. */
        uint16_t mask=0;
        for(int sc=0;sc<12;++sc) {
          int j=14*nk+l*12+sc;
          double a=(double)s[j]/on/Q,b=(double)(tot[j]-s[j])/off/Q;
          double va=fmax(0,(double)q[j]/on/Q/Q-a*a);
          double vb=fmax(0,(double)(totq[j]-q[j])/off/Q/Q-b*b);
          double error=sqrt(fmax(va,vb)/(on-1)+vb/(off-1)+1e-12);
          /* Conservative small-sample guard; at three occasions sample
           * variance alone severely understates scheduling uncertainty. */
          double threshold=6.0*sqrt(1.0+20.0/(on-2));
          if(a-b>0.025 && a-b>threshold*error) mask|=1<<sc;
        }
        if(!mask) continue;
        /* Noise-penalized contiguous segmentation, separately by RB parity.
         * A half-density extent is the minimal observable span (first..last
         * occupied RB); an unoccupied endpoint cannot be inferred from IQ. */
        double d[NR_CSIRS_DISC_MAX_RB]={0}, v[NR_CSIRS_DISC_MAX_RB]={0}, base[NR_CSIRS_DISC_MAX_RB]={0};
        int ns=__builtin_popcount(mask);
        double peak=0;
        for(int rb=0;rb<r->n_rb;++rb) {
          for(int sc=0;sc<12;++sc) if(mask&(1<<sc)) {
            int j=l*nk+12*rb+sc;
            double a=(double)s[j]/on/Q, b=(double)(tot[j]-s[j])/off/Q;
            d[rb]+=a-b; base[rb]+=b;
            v[rb]+=fmax(0,(double)q[j]/on/Q/Q-a*a)/on+
                   fmax(0,(double)(totq[j]-q[j])/off/Q/Q-b*b)/off;
          }
          d[rb]/=ns; base[rb]/=ns; v[rb]/=ns*ns;
          if(d[rb]>peak) peak=d[rb];
        }
        for(int density=0;density<3;++density) {
          int step=density<2 ? 2:1, first=density<2 ? density:0;
          double run=0,best=0;int start=first,bs=0,be=-1;
          for(int rb=first;rb<r->n_rb;rb+=step) {
            double e=d[rb]-fmax(0.25*peak,3.0*sqrt(v[rb]));
            if(run<=0) {run=e;start=rb;} else run+=e;
            if(run>best) {best=run;bs=start;be=rb;}
          }
          if(be<bs || (be-bs)/step+1<4) continue;
          double signal=0,var=0,bg=0,other=0; int nb=0,no=0;
          for(int rb=bs;rb<=be;++rb) {
            if(density<2 && rb%2!=density) {other+=d[rb];++no;continue;}
            signal+=d[rb];var+=v[rb];bg+=base[rb];++nb;
          }
          /* Reject parity duplicates of full-density patterns and vice versa. */
          if(density<2 && no && other/no>0.35*signal/nb) continue;
          if(density==2) {
            double even=0,odd=0;int ne=0,no2=0;
            for(int rb=bs;rb<=be;++rb) if(rb%2) {odd+=d[rb];++no2;} else {even+=d[rb];++ne;}
            if(ne && no2 && fmin(even/ne,odd/no2)<0.35*fmax(even/ne,odd/no2)) continue;
          }
          double strength=1+signal/fmax(bg,1e-9), z=signal/sqrt(var+1e-9);
          if(strength<min_strength || z<8) continue;
          candidate_t c={.fp={.sym_mask=1<<l,.sc_mask=mask,.density=density,
            .rb_start=bs,.n_rb=be-bs+1,.period=p,.offset=o,.strength=strength},.significance=z};
          if(density==2 && (mask==0x111 || mask==0x222 || mask==0x444 || mask==0x888)) c.fp.density=3;
          if(nc<CAND_CAP) cand[nc++]=c;
          else {int worst=0;for(int i=1;i<nc;++i) if(cand[i].significance<cand[worst].significance) worst=i;
            if(z>cand[worst].significance) cand[worst]=c;}
        }
      }
    }
    free(sum);free(sq);
  }
  qsort(cand,nc,sizeof(*cand),rank_candidate);
  int n=0;
  for(int i=0;i<nc;++i) {
    nr_csirs_disc_fp_t c=cand[i].fp;bool duplicate=false;
    for(int j=0;j<n;++j) {
      nr_csirs_disc_fp_t *a=out+j;
      int lo=a->rb_start>c.rb_start?a->rb_start:c.rb_start;
      int hi=a->rb_start+a->n_rb<c.rb_start+c.n_rb?a->rb_start+a->n_rb:c.rb_start+c.n_rb;
      /* Harmonics explain the same REs less well. Keep strongest evidence,
       * not the biggest power ratio (which favours sparsely sampled aliases). */
      int g=gcd_period(a->period,c.period);
      if((a->sym_mask&c.sym_mask) && (a->sc_mask&c.sc_mask) && hi>lo &&
         a->offset%g==c.offset%g &&
         c.strength-1 <= 1.5*(a->strength-1)*g/a->period) {
        duplicate=true;break;
      }
      if(a->sc_mask==c.sc_mask && a->density==c.density && a->rb_start==c.rb_start &&
         a->n_rb==c.n_rb && a->period==c.period && a->offset==c.offset) {
        a->sym_mask|=c.sym_mask;a->strength=fminf(a->strength,c.strength);duplicate=true;break;
      }
    }
    if(!duplicate && n<max_out) out[n++]=c;
  }
  qsort(out,n,sizeof(*out),rank_strength);
  free(cand);free(tot);free(totq);return n;
}

int nr_csirs_disc_footprints_cpu(const nr_csirs_disc_ring_t *r, const nr_csirs_disc_fp_cfg_t *cfg,
                                nr_csirs_disc_fp_t *out, int max_out)
{
  if(!nr_disc_fp_valid(r) || !out || max_out<=0) return -1;
  if(max_out>NR_CSIRS_DISC_MAX_FP) max_out=NR_CSIRS_DISC_MAX_FP;
  size_t n=(size_t)r->capacity*(14*12*r->n_rb+168);
  uint32_t *x=calloc(n,sizeof(*x));
  if(!x) return -1;
  nr_disc_fp_normalize(r,cfg,x);
  fold_ctx_t c={r,x};
  int ret=nr_disc_fp_cluster(r,cfg,out,max_out,fold_cpu,&c);
  free(x);return ret;
}

/* Stable U5 entry (see nr_csirs_disc.h). Single-ring for now; T2b adds the accumulated history. */
int nr_csirs_disc_detect(const nr_csirs_disc_ring_t *r, const nr_csirs_disc_fp_cfg_t *cfg, nr_csirs_disc_fp_t *out,
                         int max_out)
{
  const nr_csirs_disc_gpu_ops_t *g = nr_csirs_disc_gpu_load();
  if (g && g->footprints) {
    const int n = g->footprints(r, cfg, out, max_out);
    if (n >= 0)
      return n;
  }
  return nr_csirs_disc_footprints_cpu(r, cfg, out, max_out);
}

void nr_csirs_disc_detect_reset(void) {}
