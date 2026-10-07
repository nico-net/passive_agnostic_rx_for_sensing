/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_csirs_disc_fp_internal.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#ifndef NR_CSIRS_DISC_GPU_MODULE
static uint64_t fp_us(void) {struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000+t.tv_nsec/1000;}

#endif

/* Local period table avoids coupling U2 to the ring implementation's table. */
static const int periods[] = {4,5,8,10,16,20,32,40,64,80,160,320,640};
#define Q 4096.0
#define CAND_CAP 2048
#define TILES(r) (((r)->n_rb+15)/16)
#define POOL(r) (168*TILES(r))

typedef struct { nr_csirs_disc_fp_t fp; double significance; } candidate_t;

int nr_disc_fp_valid(const nr_csirs_disc_ring_t *r)
{
  return r && r->grid_base && r->meta && r->capacity > 0 && r->n_ant > 0 && r->n_ant <= 4 &&
         r->n_rb > 0 && r->n_rb <= NR_CSIRS_DISC_MAX_RB &&
         r->slot_stride >= (size_t)r->n_ant * 14 * 24 * r->n_rb;
}

/* Normalize within the SAME symbol and RB. Scheduling another symbol must not
 * turn a fully occupied data symbol into a pilot comb. */
static void normalize_local(const nr_csirs_disc_ring_t *r, const nr_csirs_disc_fp_cfg_t *cfg, uint32_t *x)
{
  const int nk = 12*r->n_rb, nr = 14*nk+POOL(r);
  for (int t=0; t<r->capacity; ++t) {
    if (!r->meta[t].valid) continue;
    for (int l=0; l<14; ++l) for (int rb=0; rb<r->n_rb; ++rb) {
      uint64_t power[12]={0}, total=0;
      int count=0;
      for (int sc=0; sc<12; ++sc) {
        int k=12*rb+sc;
        if (cfg && ((cfg->excl_sym_mask_any & (1<<l)) || (cfg->excl_re && cfg->excl_re[l*nk+k]))) continue;
        for (int a=0; a<r->n_ant; ++a) {
          const int16_t *v=nr_csirs_disc_ring_sym(r,t,a,l)+2*k;
          power[sc]+=(int64_t)v[0]*v[0]+(int64_t)v[1]*v[1];
        }
        total+=power[sc]; ++count;
      }
      /* Bounded lower-third reference excludes up to eight boosted pilot REs.
       * Fixed compare/select insertion avoids data-dependent sorting branches. */
      uint64_t low[5]={UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX};
      for(int sc=0;sc<12;++sc) {
        int k=12*rb+sc;
        if(cfg && ((cfg->excl_sym_mask_any&(1<<l)) || (cfg->excl_re && cfg->excl_re[l*nk+k]))) continue;
        uint64_t v=power[sc];
        for(int i=0;i<5;++i) {uint64_t lo=v<low[i]?v:low[i];v=v>low[i]?v:low[i];low[i]=lo;}
      }
      int available=count;
      count=(count+2)/3;total=0;
      for(int i=0;i<count;++i) total+=low[i];
      for (int sc=0; sc<12; ++sc) {
        uint64_t reference=total;
        if(available>count && power[sc]<=low[count-1]) reference+=low[count]-power[sc];
        uint64_t v=reference ? power[sc]*count*4096/reference : 0;
        x[(size_t)t*nr+l*nk+12*rb+sc]=(uint32_t)(v>8192 ? 8192 : v);
      }
    }
    for (int tile=0;tile<TILES(r);++tile) for (int l=0;l<14;++l) for (int sc=0;sc<12;++sc) {
      uint64_t sum=0;
      int end=(tile+1)*16<r->n_rb?(tile+1)*16:r->n_rb;
      for(int rb=tile*16;rb<end;++rb) sum+=x[(size_t)t*nr+l*nk+12*rb+sc];
      x[(size_t)t*nr+14*nk+tile*168+l*12+sc]=sum/(end-tile*16);
    }
  }
}

/* Preserve the public GPU batch primitive's original layout. */
void nr_disc_fp_normalize(const nr_csirs_disc_ring_t *r, const nr_csirs_disc_fp_cfg_t *cfg, uint32_t *out)
{
  int re=14*12*r->n_rb, stride=re+POOL(r);
  uint32_t *local=calloc((size_t)r->capacity*stride,sizeof(*local));
  if(!local) return;
  normalize_local(r,cfg,local);
  for(int t=0;t<r->capacity;++t) {
    uint32_t *dst=out+(size_t)t*(re+168), *src=local+(size_t)t*stride;
    memcpy(dst,src,re*sizeof(*src));
    for(int l=0;l<14;++l) for(int sc=0;sc<12;++sc) {
      uint64_t sum=0;
      for(int rb=0;rb<r->n_rb;++rb) sum+=src[l*12*r->n_rb+12*rb+sc];
      dst[re+l*12+sc]=sum/r->n_rb;
    }
  }
  free(local);
}

typedef struct { const nr_csirs_disc_ring_t *r; const uint32_t *x; } fold_ctx_t;
static int fold_cpu(void *opaque, int p, uint64_t *sum, uint64_t *sq)
{
  const fold_ctx_t *c=opaque;
  int nr=14*12*c->r->n_rb+POOL(c->r);
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
  double xs=x->significance*(x->fp.strength-1), ys=y->significance*(y->fp.strength-1);
  if(xs!=ys) return xs<ys ? 1:-1;
  if(x->fp.period!=y->fp.period) return x->fp.period-y->fp.period;
  if(x->fp.offset!=y->fp.offset) return x->fp.offset-y->fp.offset;
  if(x->fp.sym_mask!=y->fp.sym_mask) return x->fp.sym_mask-y->fp.sym_mask;
  return x->fp.rb_start-y->fp.rb_start;
}
static void add_candidate(candidate_t *heap, int *count, candidate_t c)
{
  int at;
  if(*count<CAND_CAP) {
    at=(*count)++;heap[at]=c;
    while(at>0) {int parent=(at-1)/2;if(rank_candidate(heap+at,heap+parent)<=0) break;
      candidate_t tmp=heap[at];heap[at]=heap[parent];heap[parent]=tmp;at=parent;}
  } else if(rank_candidate(&c,heap)<0) {
    heap[0]=c;at=0;
    while(2*at+1<*count) {int child=2*at+1;
      if(child+1<*count && rank_candidate(heap+child+1,heap+child)>0) ++child;
      if(rank_candidate(heap+child,heap+at)<=0) break;
      candidate_t tmp=heap[at];heap[at]=heap[child];heap[child]=tmp;at=child;}
  }
}

static int rank_strength(const void *a, const void *b)
{
  const nr_csirs_disc_fp_t *x=a,*y=b;
  if(x->strength!=y->strength) return x->strength<y->strength ? 1:-1;
  if(x->period!=y->period) return x->period-y->period;
  if(x->offset!=y->offset) return x->offset-y->offset;
  return x->sym_mask-y->sym_mask;
}

/* Every legal period divides 640. Sufficient statistics preserve T2's Q12
 * samples and pooled comb variance without retaining IQ. One serialized worker. */
typedef struct {
  uint64_t *sum, *sq, *total, *totalq;
  uint64_t *sum80, *sum64;
  candidate_t *groups[4];
  int group_count[4];
  uint32_t round;
  uint64_t counts[640], nt, written;
  const int16_t *grid;
  int nr, n_rb, n_ant, capacity;
  size_t stride;
  uint16_t symbols;
  uint8_t *exclude;
  const nr_csirs_disc_gpu_ops_t *ops;
} history_t;
#ifndef NR_CSIRS_DISC_GPU_MODULE
static history_t history;
uint64_t csirs_disc_gpu_detect_batches, csirs_disc_cpu_detect_batches;
uint64_t csirs_disc_gpu_lattice_calls, csirs_disc_cpu_lattice_calls;
static bool history_unified;
static void *history_alloc(size_t count)
{
  size_t bytes=count*sizeof(uint64_t);
  void *p=history_unified?nr_csirs_disc_alloc_unified(bytes):malloc(bytes);
  if(p) memset(p,0,bytes);
  return p;
}
static void history_free(void *p)
{
  if(history_unified) {if(p) nr_csirs_disc_free_unified(p);}
  else free(p);
}

#endif

/* Two short-period bases avoid rereading 640 RE vectors for every short
 * lattice. Only pooled tiles need per-phase squares; extent uses the global
 * per-RE variance, which conservatively includes between-phase variation. */
static void history_reduce(const history_t *h,int p,int o,int first,int end,uint64_t *sum)
{
  int basis=80%p==0?80:64%p==0?64:640;
  const uint64_t *values=basis==80?h->sum80:basis==64?h->sum64:h->sum;
  memset(sum+first,0,(end-first)*sizeof(*sum));
  for(int residue=o;residue<basis;residue+=p) {
    const uint64_t *v=values+(size_t)residue*h->nr;
    for(int j=first;j<end;++j) sum[j]+=v[j];
  }
}

static int finish_candidates(candidate_t *cand,int nc,nr_csirs_disc_fp_t *out,int max_out)
{
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
         (((a->sc_mask & c.sc_mask)==c.sc_mask && (a->period% c.period==0 || c.period%a->period==0))
          || c.strength-1 <= 1.5*(a->strength-1)*g/a->period)) {
        duplicate=true;break;
      }
      if(a->sc_mask==c.sc_mask && a->density==c.density && a->rb_start==c.rb_start &&
         a->n_rb==c.n_rb && a->period==c.period && a->offset==c.offset) {
        a->sym_mask|=c.sym_mask;a->strength=fminf(a->strength,c.strength);duplicate=true;break;
      }
    }
    if(!duplicate && n<max_out) out[n++]=c;
  }
  /* Coalescing symbols may reveal a complete pattern after one of its partial
   * masks was already selected. Canonicalize again so the same RE support is
   * not emitted both as the complete footprint and a contained fragment. */
  for(int i=0;i<n;) {
    bool contained=false;
    for(int j=0;j<n;++j) if(i!=j) {
      const nr_csirs_disc_fp_t *a=out+j,*b=out+i;
      contained |= a->period==b->period && a->offset==b->offset && a->density==b->density
        && a->rb_start==b->rb_start && a->n_rb==b->n_rb
        && (a->sym_mask & b->sym_mask)==b->sym_mask && (a->sc_mask & b->sc_mask)==b->sc_mask
        && (a->sym_mask!=b->sym_mask || a->sc_mask!=b->sc_mask);
    }
    if(contained) {memmove(out+i,out+i+1,(n-i-1)*sizeof(*out));--n;} else ++i;
  }
  qsort(out,n,sizeof(*out),rank_strength);
  free(cand);return n;
}
static int cluster(const nr_csirs_disc_ring_t *r, const nr_csirs_disc_fp_cfg_t *cfg,
                   nr_csirs_disc_fp_t *out, int max_out, nr_disc_fold_fn fold, void *ctx, const history_t *h,
                   int first_period, int period_step, candidate_t **raw, int *nraw)
{
  int nk=12*r->n_rb, nr=14*nk+POOL(r), nc=0;
  uint64_t nt=h ? h->nt:0;
  if(!h) for(int t=0;t<r->capacity;++t) nt+=!!r->meta[t].valid;
  if(nt<12) return 0;
  candidate_t *cand=calloc(CAND_CAP,sizeof(*cand));
  uint64_t *tot=calloc(nr,sizeof(*tot)), *totq=calloc(nr,sizeof(*totq));
  if(!cand || !tot || !totq) { free(cand); free(tot); free(totq); return -1; }
  float min_strength=cfg && cfg->min_strength>0 ? cfg->min_strength:2.0f;
  for(unsigned pi=first_period;pi<sizeof(periods)/sizeof(*periods);pi+=period_step) {
    int p=periods[pi];
    uint64_t counts[640]={0};
    if(h) for(int t=0;t<640;++t) counts[t%p]+=h->counts[t];
    else for(int t=0;t<r->capacity;++t) if(r->meta[t].valid) ++counts[r->meta[t].abs_slot%p];
    int usable=0;
    for(int o=0;o<p;++o) usable+=counts[o]>=3 && nt-counts[o]>=3;
    if(!usable) continue;
    uint64_t *sum=calloc((size_t)(h?1:p)*nr,sizeof(*sum)), *sq=calloc((size_t)(h?1:p)*nr,sizeof(*sq));
    if(!sum || !sq || (!h && fold(ctx,p,sum,sq)<0)) {
      free(sum); free(sq); free(cand); free(tot); free(totq); return -1;
    }
    memset(tot,0,nr*sizeof(*tot)); memset(totq,0,nr*sizeof(*totq));
    if(h) {memcpy(tot,h->total,nr*sizeof(*tot));memcpy(totq,h->totalq,nr*sizeof(*totq));}
    else for(int o=0;o<p;++o) for(int j=0;j<nr;++j) {tot[j]+=sum[(size_t)o*nr+j];totq[j]+=sq[(size_t)o*nr+j];}
    const int pooled=POOL(r);
    uint64_t *pool_sum=NULL,*pool_sq=NULL;
    uint16_t *masks=NULL;
#ifndef NR_CSIRS_DISC_GPU_MODULE
    if(h && h->ops && h->ops->lattice) {
      masks=calloc((size_t)p*14*TILES(r)*6,sizeof(*masks));
      if(masks && h->ops->lattice(r->n_rb,p,nt,counts,h->sum,h->sq,h->total,h->totalq,masks)<0) {
        free(masks);masks=NULL;
      }
      if(masks) __atomic_fetch_add(&csirs_disc_gpu_lattice_calls,1,__ATOMIC_RELAXED);
    }
    if(h && !masks) __atomic_fetch_add(&csirs_disc_cpu_lattice_calls,1,__ATOMIC_RELAXED);
#endif
    if(h && !masks) {
      pool_sum=calloc((size_t)p*pooled,sizeof(*pool_sum));
      pool_sq=calloc((size_t)p*pooled,sizeof(*pool_sq));
      if(!pool_sum || !pool_sq) {free(pool_sum);free(pool_sq);free(sum);free(sq);free(cand);free(tot);free(totq);return -1;}
      for(int residue=0;residue<640;++residue) {
        const uint64_t *s=h->sum+(size_t)residue*nr+14*nk,*q=h->sq+(size_t)residue*pooled;
        uint64_t *dst=pool_sum+(size_t)(residue%p)*pooled,*dstq=pool_sq+(size_t)(residue%p)*pooled;
        for(int j=0;j<pooled;++j) {dst[j]+=s[j];dstq[j]+=q[j];}
      }
    }
    for(int o=0;o<p;++o) {
      uint64_t on=counts[o], off=nt-on;
      if(on<3 || off<3) continue;
      const double ion=1.0/on/Q, ioff=1.0/off/Q;
      const double pooled_error=16.0;
      if(h && !masks) {
        memcpy(sum+14*nk,pool_sum+(size_t)o*pooled,pooled*sizeof(*sum));
        memcpy(sq+14*nk,pool_sq+(size_t)o*pooled,pooled*sizeof(*sq));
      }
      const uint64_t *s=sum+(h?0:(size_t)o*nr), *q=sq+(h?0:(size_t)o*nr);
      for(int l=0;l<14;++l) {
        if(cfg && (cfg->excl_sym_mask_any&(1<<l))) continue;
        /* Local seeds avoid diluting a narrow resource into the whole carrier.
         * Each distinct seed is segmented independently over the RB axis. */
        bool tried[4096]={0};
        bool reduced=false;
        double dr[NR_CSIRS_DISC_MAX_RB*12],vr[NR_CSIRS_DISC_MAX_RB*12],br[NR_CSIRS_DISC_MAX_RB*12];
        for(int tile=0;tile<TILES(r);++tile) {
        uint16_t mask=0,strong=0;
        if(!masks) for(int sc=0;sc<12;++sc) {
          int j=14*nk+tile*168+l*12+sc;
          double a=s[j]*ion,b=(tot[j]-s[j])*ioff;
          double va=fmax(0,q[j]*ion/Q-a*a);
          double vb=fmax(0,(totq[j]-q[j])*ioff/Q-b*b);
          /* Test the null (off-lattice) variance; on-lattice fading is signal,
           * not evidence against a repeatable pilot. Squared form avoids sqrt. */
          double diff=a-b;
          if(diff>0.025 && diff*diff>pooled_error*(vb/(on-1)+vb/(off-1))+1e-12) mask|=1<<sc;
          if(diff>0.025 && diff*diff>36*(fmax(va,vb)/(on-1)+vb/(off-1))+1e-12) strong|=1<<sc;
        }
        const uint16_t observed=mask;
        for(int component_mask=-2;component_mask<4;++component_mask) {
        if(masks) {
          mask=masks[(((size_t)o*14+l)*TILES(r)+tile)*6+component_mask+2];
          if(!mask) continue;
        } else {
        if(component_mask==-1) {
          bool partial_comb=false;
          for(int k=0;k<4;++k) partial_comb |= (strong & (0x111u<<k))==strong;
          if(__builtin_popcount(strong)<2 || partial_comb) continue;
        }
        mask=component_mask==-2?strong:component_mask==-1?
          (strong | ((strong&0x555)<<1) | ((strong&0xaaa)>>1)):(0x111u<<component_mask);
        if(!mask || mask==0xfff || (component_mask!=-1 && (mask&observed)!=mask) || tried[mask]) continue;
        if(__builtin_popcount(mask)==1 && !(mask&strong)) continue;
        bool comb3=mask==0x111 || mask==0x222 || mask==0x444 || mask==0x888;
        bool pairs=(mask & 0x555)==((mask>>1)&0x555);
        if(__builtin_popcount(mask)!=1 && !comb3 && !pairs) continue;
        tried[mask]=true;
        /* A peak must exceed a majority of OTHER phase means. Comparing only
         * against their mean mistakes a pilot-induced trough for pilots on all
         * remaining phases, particularly with dense CDM rows. */
        double on_mean=0,off_mean=0,on_error=0;
        for(int sc=0;sc<12;++sc) if(mask&(1<<sc)) {
          int j=14*nk+tile*168+l*12+sc;
          on_mean+=s[j]*ion;off_mean+=(tot[j]-s[j])*ioff;
          double mean=s[j]*ion;on_error+=fmax(0,q[j]*ion/Q-mean*mean)/on;
        }
        int below=0,alternatives=0;
        for(int other=0;other<p;++other) if(other!=o && counts[other]) {
          double mean=0,error=0,scale=1.0/(counts[other]*Q);
          for(int sc=0;sc<12;++sc) if(mask&(1<<sc)) {
            int j=tile*168+l*12+sc;
            double v=(h?pool_sum[(size_t)other*pooled+j]:sum[(size_t)other*nr+14*nk+j])*scale;
            double qv=(h?pool_sq[(size_t)other*pooled+j]:sq[(size_t)other*nr+14*nk+j])*scale/Q;
            mean+=v;error+=fmax(0,qv-v*v)/counts[other];
          }
          double delta=on_mean-mean;
          below+=mean<(on_mean+off_mean)*0.5 && delta>0 && delta*delta>9*(on_error+error);
          ++alternatives;
        }
        if(2*below<=alternatives) continue;
        }
        if(!reduced) {
          if(h) history_reduce(h,p,o,l*nk,(l+1)*nk,sum);
          for(int rb=0;rb<r->n_rb;++rb) {
            double diff[12];
            for(int sc=0;sc<12;++sc) {
              int k=12*rb+sc,j=l*nk+k;
              double a=s[j]*ion,b=(tot[j]-s[j])*ioff;
              diff[sc]=a-b;br[k]=b;
              double mean=tot[j]/(nt*Q);
              vr[k]=fmax(0,totq[j]/(nt*Q*Q)-mean*mean)*(1.0/on+1.0/off);
            }
            for(int sc=0;sc<12;++sc) dr[12*rb+sc]=diff[sc];
          }
          reduced=true;
        }
        /* Noise-penalized contiguous segmentation, separately by RB parity.
         * A half-density extent is the minimal observable span (first..last
         * occupied RB); an unoccupied endpoint cannot be inferred from IQ. */
        double d[NR_CSIRS_DISC_MAX_RB]={0}, v[NR_CSIRS_DISC_MAX_RB]={0}, base[NR_CSIRS_DISC_MAX_RB]={0};
        int ns=__builtin_popcount(mask);
        double peak=0;
        for(int rb=0;rb<r->n_rb;++rb) {
          for(int sc=0;sc<12;++sc) if(mask&(1<<sc)) {
            int k=12*rb+sc;d[rb]+=dr[k];base[rb]+=br[k];v[rb]+=vr[k];
          }
          d[rb]/=ns; base[rb]/=ns; v[rb]/=ns*ns;
          if(d[rb]>peak) peak=d[rb];
        }
        for(int density=0;density<3;++density) {
          int step=density<2 ? 2:1, first=density<2 ? density:0;
          bool used[NR_CSIRS_DISC_MAX_RB]={0};
          /* Long silent gaps separate colocated combs from different subbands.
           * Use pooled eight-RB support so a channel notch is not a boundary. */
          for(int rb=first;rb<r->n_rb;rb+=step) {
            int lo=rb-4*step, hi=rb+4*step;
            double support=0,variance=0;
            for(int k=lo;k<=hi;k+=step) if(k>=0 && k<r->n_rb) {support+=d[k];variance+=v[k];}
            used[rb]=support<2*sqrt(variance);
          }
          for(int component=0;component<8;++component) {
          double run=0,best=0;int start=first,bs=0,be=-1;
          for(int rb=first;rb<r->n_rb;rb+=step) {
            double e=used[rb]?-1e9:d[rb]-fmax(0.25*peak,3.0*sqrt(v[rb]));
            if(run<=0) {run=e;start=rb;} else run+=e;
            if(run>best) {best=run;bs=start;be=rb;}
          }
          if(be<bs || (be-bs)/step+1<4) break;
          for(int rb=bs;rb<=be;++rb) used[rb]=true;
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
          add_candidate(cand,&nc,c);
          }
        }
        }
      }
    }
    }
    free(masks);free(pool_sum);free(pool_sq);free(sum);free(sq);
  }
  free(tot);free(totq);
  if(raw) {*raw=cand;*nraw=nc;return nc;}
  return finish_candidates(cand,nc,out,max_out);
}

int nr_disc_fp_cluster(const nr_csirs_disc_ring_t *r, const nr_csirs_disc_fp_cfg_t *cfg,
                       nr_csirs_disc_fp_t *out, int max_out, nr_disc_fold_fn fold, void *ctx)
{
  return cluster(r,cfg,out,max_out,fold,ctx,NULL,0,1,NULL,NULL);
}

int nr_csirs_disc_footprints_cpu(const nr_csirs_disc_ring_t *r, const nr_csirs_disc_fp_cfg_t *cfg,
                                nr_csirs_disc_fp_t *out, int max_out)
{
  if(!nr_disc_fp_valid(r) || !out || max_out<=0) return -1;
  if(max_out>NR_CSIRS_DISC_MAX_FP) max_out=NR_CSIRS_DISC_MAX_FP;
  size_t n=(size_t)r->capacity*(14*12*r->n_rb+POOL(r));
  uint32_t *x=calloc(n,sizeof(*x));
  if(!x) return -1;
  normalize_local(r,cfg,x);
  fold_ctx_t c={r,x};
  int ret=nr_disc_fp_cluster(r,cfg,out,max_out,fold_cpu,&c);
  free(x);return ret;
}

#ifndef NR_CSIRS_DISC_GPU_MODULE
void nr_csirs_disc_detect_reset(void)
{
  const nr_csirs_disc_gpu_ops_t *gpu=nr_csirs_disc_gpu_load();
  if(gpu && gpu->reset) gpu->reset();
  for(int i=0;i<4;++i) free(history.groups[i]);
  history_free(history.sum80);history_free(history.sum64);
  history_free(history.sum);history_free(history.sq);history_free(history.total);history_free(history.totalq);free(history.exclude);
  history_unified=false;
  memset(&history,0,sizeof(history));
}

/* Four scheduling groups, executed serially on the single discovery worker. */
enum { FP_GROUPS=4 };
typedef struct {
  nr_csirs_disc_ring_t ring;
  const nr_csirs_disc_fp_cfg_t *cfg;
  uint32_t *x;
  int worker, period_step, error;
  candidate_t *candidates;
  int count;
} fp_job_t;
static void *normalize_job(void *opaque)
{
  fp_job_t *j=opaque;normalize_local(&j->ring,j->cfg,j->x);return NULL;
}
static void normalize_parallel(const nr_csirs_disc_ring_t *r,const nr_csirs_disc_fp_cfg_t *cfg,uint32_t *x)
{
  fp_job_t jobs[FP_GROUPS];
  int nr=14*12*r->n_rb+POOL(r);
  for(int i=FP_GROUPS-1;i>=0;--i) {
    int first=r->capacity*i/FP_GROUPS,end=r->capacity*(i+1)/FP_GROUPS;
    jobs[i]=(fp_job_t){.ring=*r,.cfg=cfg,.x=x+(size_t)first*nr};
    jobs[i].ring.grid_base+=(size_t)first*r->slot_stride;jobs[i].ring.meta+=first;jobs[i].ring.capacity=end-first;
    normalize_job(&jobs[i]);
  }
}
static void *fold_job(void *opaque)
{
  fp_job_t *job=opaque;history_t *h=&history;const nr_csirs_disc_ring_t *r=&job->ring;
  int nr=h->nr,pooled=POOL(r),first=nr*job->worker/FP_GROUPS,end=nr*(job->worker+1)/FP_GROUPS;
  for(int t=0;t<r->capacity;++t) if(r->meta[t].valid) {
    int residue=r->meta[t].abs_slot%640;
    uint64_t *s=h->sum+(size_t)residue*nr,*q=h->sq+(size_t)residue*pooled;
    uint64_t *s80=h->sum80+(size_t)(residue%80)*nr,*s64=h->sum64+(size_t)(residue%64)*nr;
    const uint32_t *v=job->x+(size_t)t*nr;
    for(int j=first;j<end;++j) {
      uint64_t square=(uint64_t)v[j]*v[j];
      s[j]+=v[j];s80[j]+=v[j];s64[j]+=v[j];h->total[j]+=v[j];h->totalq[j]+=square;
      if(j>=nr-pooled) q[j-(nr-pooled)]+=square;
    }
  }
  return NULL;
}
static void fold_parallel(const nr_csirs_disc_ring_t *r,uint32_t *x)
{
  fp_job_t jobs[FP_GROUPS];
  for(int i=FP_GROUPS-1;i>=0;--i) {
    jobs[i]=(fp_job_t){.ring=*r,.x=x,.worker=i};
    fold_job(&jobs[i]);
  }
  for(int t=0;t<r->capacity;++t) if(r->meta[t].valid) {++history.counts[r->meta[t].abs_slot%640];++history.nt;}
}

static void *cluster_job(void *opaque)
{
  fp_job_t *j=opaque;
  j->error=cluster(&j->ring,j->cfg,NULL,0,NULL,NULL,&history,j->worker,j->period_step,&j->candidates,&j->count)<0;
  return NULL;
}
static int cluster_parallel(const nr_csirs_disc_ring_t *r,const nr_csirs_disc_fp_cfg_t *cfg,
                            nr_csirs_disc_fp_t *out,int max_out,int group)
{
  fp_job_t jobs[FP_GROUPS];
  for(int i=FP_GROUPS-1;i>=0;--i) {
    jobs[i]=(fp_job_t){.ring=*r,.cfg=cfg,.worker=group<0?i:group+FP_GROUPS*i,.period_step=group<0?FP_GROUPS:FP_GROUPS*FP_GROUPS};
    cluster_job(&jobs[i]);
  }
  candidate_t *updated=group<0?NULL:calloc(CAND_CAP,sizeof(*updated));int updated_count=0;bool error=false;
  for(int i=0;i<FP_GROUPS;++i) {
    error |= jobs[i].error;
    if(group<0) {
      free(history.groups[i]);history.groups[i]=jobs[i].candidates;history.group_count[i]=jobs[i].count;
    } else {
      if(updated) for(int k=0;k<jobs[i].count;++k) add_candidate(updated,&updated_count,jobs[i].candidates[k]);
      free(jobs[i].candidates);
    }
  }
  if(error) {free(updated);return -1;}
  if(group>=0) {
    if(!updated) return -1;
    free(history.groups[group]);history.groups[group]=updated;history.group_count[group]=updated_count;
  }
  candidate_t *all=calloc(CAND_CAP,sizeof(*all));int count=0;
  if(!all) return -1;
  for(int i=0;i<FP_GROUPS;++i) for(int k=0;k<history.group_count[i];++k)
    add_candidate(all,&count,history.groups[i][k]);
  return finish_candidates(all,count,out,max_out);
}

/* Fold only new writes, including invalid entries in the write cursor but not
 * the statistics. The caller owns a stable snapshot. Missing overwritten IQ
 * cannot be reconstructed: return -1 before mutating history on an overrun. */
static int detect(const nr_csirs_disc_ring_t *r, const nr_csirs_disc_fp_cfg_t *cfg,
                         nr_csirs_disc_fp_t *out, int max_out, bool scheduled, bool force_cpu)
{
  if(!nr_disc_fp_valid(r) || !out || max_out<=0) return -1;
  uint64_t begin=fp_us(), norm=0;
  history_t *h=&history;
  const nr_csirs_disc_gpu_ops_t *ops=force_cpu?NULL:nr_csirs_disc_gpu_load();
  if(r->device_valid && (!ops || !ops->detect)) return -1;
  if(ops && ops->detect) {
    int rc=ops->detect(r,cfg,out,max_out,scheduled);
    if(rc>=0) {__atomic_fetch_add(&csirs_disc_gpu_detect_batches,1,__ATOMIC_RELAXED);__atomic_fetch_add(&csirs_disc_gpu_lattice_calls,1,__ATOMIC_RELAXED);}
    return rc; /* resident IQ cannot be silently scored through a stale host mirror */
  }
  bool gpu=ops && ops->accumulate && nr_csirs_disc_alloc_unified && nr_csirs_disc_free_unified;
  int nr=14*12*r->n_rb+POOL(r);
  uint16_t symbols=cfg?cfg->excl_sym_mask_any:0;
  bool changed=h->grid && (h->grid!=r->grid_base || h->n_rb!=r->n_rb || h->n_ant!=r->n_ant ||
    history_unified!=gpu || h->capacity!=r->capacity || h->stride!=r->slot_stride || h->symbols!=symbols || r->written<h->written);
  if(h->grid && !changed) for(int j=0;j<nr-POOL(r);++j)
    if(h->exclude[j]!=(cfg && cfg->excl_re?cfg->excl_re[j]:0)) {changed=true;break;}
  if(changed) nr_csirs_disc_detect_reset();
  if(h->grid && r->written-h->written>(uint64_t)r->capacity) return -1;
  if(!h->grid) {
    /* Bootstrap the available snapshot after reset; older IQ is unavailable. */
    h->written=r->written>(uint64_t)r->capacity?r->written-r->capacity:0;
    history_unified=gpu;
    h->sum=history_alloc((size_t)640*nr);h->sq=history_alloc((size_t)640*POOL(r));
    h->sum80=history_alloc((size_t)80*nr);h->sum64=history_alloc((size_t)64*nr);
    h->total=history_alloc(nr);h->totalq=history_alloc(nr);h->exclude=calloc(nr-POOL(r),1);
    if(!h->sum80 || !h->sum64 || !h->sum || !h->sq || !h->total || !h->totalq || !h->exclude) {nr_csirs_disc_detect_reset();return -1;}
    h->grid=r->grid_base;h->nr=nr;h->n_rb=r->n_rb;h->n_ant=r->n_ant;
    h->capacity=r->capacity;h->stride=r->slot_stride;h->symbols=symbols;h->ops=gpu?ops:NULL;
    if(cfg && cfg->excl_re) memcpy(h->exclude,cfg->excl_re,nr-POOL(r));
  }
  /* 8192^2 per sample; reject rather than silently overflow the exact totals.
   * At 2000 slots/s this limit is over four years of uninterrupted lock. */
  if(r->written-h->written>UINT64_MAX/(8192ULL*8192)-h->nt) return -1;
  uint64_t new_slots=r->written-h->written;
  while(h->written<r->written) {
    int start=h->written%r->capacity;
    uint64_t pending=r->written-h->written;
    int count=pending<(uint64_t)(r->capacity-start)?(int)pending:r->capacity-start;
    /* Bound temporary storage even for large offline rings. */
    if(count>100) count=100;
    nr_csirs_disc_ring_t batch=*r;
    batch.grid_base+=start*r->slot_stride;batch.meta+=start;batch.capacity=count;
    uint64_t nbegin=fp_us();
    if(gpu) {
      if(ops->accumulate(&batch,cfg,h->sum,h->sq,h->sum80,h->sum64,h->total,h->totalq)<0) {
        /* A device error may have partially updated statistics. Discard them;
         * CPU rebuilds ONLY the available snapshot, never double-folds a slot. */
        nr_csirs_disc_detect_reset();
        return detect(r,cfg,out,max_out,scheduled,true);
      }
      for(int t=0;t<count;++t) if(batch.meta[t].valid) {++h->counts[batch.meta[t].abs_slot%640];++h->nt;}
      norm+=fp_us()-nbegin;
    } else {
      uint32_t *x=calloc((size_t)count*nr,sizeof(*x));
      if(!x) return -1;
      normalize_parallel(&batch,cfg,x);
      norm+=fp_us()-nbegin;
      fold_parallel(&batch,x);free(x);
    }
    h->written+=count;
  }
  if(gpu) __atomic_fetch_add(&csirs_disc_gpu_detect_batches,1,__ATOMIC_RELAXED);
  else {
    __atomic_fetch_add(&csirs_disc_cpu_detect_batches,1,__ATOMIC_RELAXED);
    static bool warned;
    if(!warned) {fprintf(stderr,"SENSING: CSIRS_DISC CPU detector fallback (not real-time)\n");warned=true;}
  }
  if(max_out>128) max_out=128;
  uint64_t folded=fp_us();
  int group=scheduled && new_slots<(uint64_t)r->capacity?h->round++%FP_GROUPS:-1;
  int result=cluster_parallel(r,cfg,out,max_out,group);
  if(getenv("ISAC_CSIRS_DISC_DIAG")) fprintf(stderr,"DETECT normalize=%llu fold=%llu cluster=%llu us\n",
    (unsigned long long)norm,(unsigned long long)(folded-begin-norm),(unsigned long long)(fp_us()-folded));
  return result;
}

int nr_csirs_disc_detect(const nr_csirs_disc_ring_t *r,const nr_csirs_disc_fp_cfg_t *cfg,
                          nr_csirs_disc_fp_t *out,int max_out)
{
  return detect(r,cfg,out,max_out>NR_CSIRS_DISC_MAX_FP?NR_CSIRS_DISC_MAX_FP:max_out,false,false);
}

int nr_csirs_disc_detect_candidates(const nr_csirs_disc_ring_t *r,const nr_csirs_disc_fp_cfg_t *cfg,
                                     nr_csirs_disc_fp_t *out,int max_out)
{
  return detect(r,cfg,out,max_out,true,false);
}

#endif
