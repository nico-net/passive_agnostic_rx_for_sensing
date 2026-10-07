/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* T16 resident history, lattice and RB segmentation. Included after the shared
 * normalization/lattice kernels. Only final footprints cross back to the CPU. */
namespace resident {
constexpr int CAND_CAP=2048, GPU_CAND_CAP=65536;
struct candidate_t { nr_csirs_disc_fp_t fp; double significance; };
__device__ static int gcd_period(int a,int b)
{
  while(b) {int t=a%b;a=b;b=t;}return a;
}

__device__ static int rank_candidate(const void *a, const void *b)
{
  const candidate_t *x=(const candidate_t *)a,*y=(const candidate_t *)b;
  double xs=x->significance*(x->fp.strength-1), ys=y->significance*(y->fp.strength-1);
  if(xs!=ys) return xs<ys ? 1:-1;
  if(x->fp.period!=y->fp.period) return x->fp.period-y->fp.period;
  if(x->fp.offset!=y->fp.offset) return x->fp.offset-y->fp.offset;
  if(x->fp.sym_mask!=y->fp.sym_mask) return x->fp.sym_mask-y->fp.sym_mask;
  return x->fp.rb_start-y->fp.rb_start;
}
__device__ static void add_candidate(candidate_t *heap, int *count, candidate_t c)
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

__device__ static int rank_strength(const void *a, const void *b)
{
  const nr_csirs_disc_fp_t *x=(const nr_csirs_disc_fp_t *)a,*y=(const nr_csirs_disc_fp_t *)b;
  if(x->strength!=y->strength) return x->strength<y->strength ? 1:-1;
  if(x->period!=y->period) return x->period-y->period;
  if(x->offset!=y->offset) return x->offset-y->offset;
  return x->sym_mask-y->sym_mask;
}


template<class T, class Compare> __device__ void device_sort(T *a,int n,Compare cmp)
{
  // In-place heapsort, bounded workspace, same comparator as the CPU.
  for(int start=n/2-1;start>=-1;--start) {
    int end=n;
    if(start==-1) end=n-1;
    for(;end>0;--end) {
      int root=start>=0?start:0;
      if(start<0) {T t=a[0];a[0]=a[end];a[end]=t;}
      int limit=start>=0?n:end;
      while(root*2+1<limit) {
        int child=root*2+1;
        if(child+1<limit && cmp(a+child,a+child+1)<0) ++child;
        if(cmp(a+root,a+child)>=0) break;
        T t=a[root];a[root]=a[child];a[child]=t;root=child;
      }
      if(start>=0) break;
    }
  }
}
__device__ static int finish_candidates(candidate_t *cand,int nc,nr_csirs_disc_fp_t *out,int max_out)
{
  device_sort(cand,nc,rank_candidate);
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
    if(contained) {for(int k=i;k<n-1;++k) out[k]=out[k+1];--n;} else ++i;
  }
  device_sort(out,n,rank_strength);
  return n;
}

__global__ void segment(int rb_count,int p,uint64_t nt,const uint64_t *counts,
    const uint64_t *sum,const uint64_t *sum80,const uint64_t *sum64,
    const uint64_t *tot,const uint64_t *totq,const uint16_t *masks,
    uint16_t excluded,float min_strength,candidate_t *cand,unsigned *count)
{
  int id=blockIdx.x*blockDim.x+threadIdx.x;if(id>=p*14) return;
  int o=id/14,l=id%14,tiles=(rb_count+15)/16,nk=12*rb_count,nr=14*nk+168*tiles;
  uint64_t on=counts[o],off=nt-on;
  if(on<3 || off<3 || (excluded&(1<<l))) return;
  bool any=false;for(int k=0;k<tiles*6;++k) any|=masks[(size_t)id*tiles*6+k]!=0;
  if(!any) return;
  double ion=1.0/on/4096.,ioff=1.0/off/4096.;
  double dr[NR_CSIRS_DISC_MAX_RB*12],vr[NR_CSIRS_DISC_MAX_RB*12],br[NR_CSIRS_DISC_MAX_RB*12];
  int basis=80%p==0?80:64%p==0?64:640;
  const uint64_t *values=basis==80?sum80:basis==64?sum64:sum;
  for(int k=0;k<nk;++k) {
    int j=l*nk+k;uint64_t v=0;
    for(int residue=o;residue<basis;residue+=p) v+=values[(size_t)residue*nr+j];
    double a=v*ion,b=(tot[j]-v)*ioff,mean=tot[j]/(nt*4096.);
    dr[k]=a-b;br[k]=b;vr[k]=fmax(0.,totq[j]/(nt*4096.*4096.)-mean*mean)*(1.0/on+1.0/off);
  }
  for(int tile=0;tile<tiles;++tile) for(int component_mask=-2;component_mask<4;++component_mask) {
    uint16_t mask=masks[((size_t)id*tiles+tile)*6+component_mask+2];if(!mask) continue;
        /* Noise-penalized contiguous segmentation, separately by RB parity.
         * A half-density extent is the minimal observable span (first..last
         * occupied RB); an unoccupied endpoint cannot be inferred from IQ. */
        double d[NR_CSIRS_DISC_MAX_RB]={0}, v[NR_CSIRS_DISC_MAX_RB]={0}, base[NR_CSIRS_DISC_MAX_RB]={0};
        int ns=__popc(mask);
        double peak=0;
        for(int rb=0;rb<rb_count;++rb) {
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
          for(int rb=first;rb<rb_count;rb+=step) {
            int lo=rb-4*step, hi=rb+4*step;
            double support=0,variance=0;
            for(int k=lo;k<=hi;k+=step) if(k>=0 && k<rb_count) {support+=d[k];variance+=v[k];}
            used[rb]=support<2*sqrt(variance);
          }
          for(int component=0;component<8;++component) {
          double run=0,best=0;int start=first,bs=0,be=-1;
          for(int rb=first;rb<rb_count;rb+=step) {
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
          unsigned at=atomicAdd(count,1u);
          if(at<GPU_CAND_CAP) cand[at]=c;
          }
        }

  }
}
__global__ void collect(candidate_t *raw,unsigned *count,candidate_t *groups,int *sizes,int group,bool clear)
{
  if(clear) sizes[group]=0;
  int n=sizes[group];
  for(unsigned i=0;i<min(*count,(unsigned)GPU_CAND_CAP);++i) add_candidate(groups+group*CAND_CAP,&n,raw[i]);
  sizes[group]=n;
}
__global__ void finish(candidate_t *groups,int *sizes,candidate_t *all,nr_csirs_disc_fp_t *out,int *nout,int maxout)
{
  int n=0;for(int g=0;g<4;++g) for(int i=0;i<sizes[g];++i) add_candidate(all,&n,groups[g*CAND_CAP+i]);
  *nout=finish_candidates(all,n,out,maxout);
}
struct State {
  Buffer<uint64_t> sum,sq,s80,s64,total,totalq,ps,pq,dc;
  Buffer<uint32_t> x;
  Buffer<nr_csirs_disc_slot_meta_t> meta;
  Buffer<int16_t> grid;
  Buffer<uint8_t> exclude;
  Buffer<uint16_t> masks;
  Buffer<candidate_t> raw,groups,all;
  Buffer<unsigned> count;
  Buffer<int> sizes,nout;
  Buffer<nr_csirs_disc_fp_t> out;
  uint64_t counts[640]={},nt=0,written=0;
  const int16_t *identity=nullptr;
  int rb=0,ant=0,capacity=0,round=0;
  uint16_t symbols=0;uint8_t host_exclude[14*12*NR_CSIRS_DISC_MAX_RB]={};
} state;
static bool prepare(int rb,int ant,int capacity)
{
  auto &s=state;int pooled=168*((rb+15)/16),nr=14*12*rb+pooled;
  return s.sum.reserve((size_t)640*nr) && s.sq.reserve((size_t)640*pooled)
    && s.s80.reserve((size_t)80*nr) && s.s64.reserve((size_t)64*nr)
    && s.total.reserve(nr) && s.totalq.reserve(nr) && s.ps.reserve((size_t)640*pooled)
    && s.pq.reserve((size_t)640*pooled) && s.dc.reserve(640) && s.x.reserve((size_t)100*nr)
    && s.meta.reserve(100) && s.grid.reserve((size_t)100*ant*14*24*rb)
    && s.exclude.reserve(14*12*rb) && s.masks.reserve((size_t)640*14*((rb+15)/16)*6)
    && s.raw.reserve(GPU_CAND_CAP) && s.groups.reserve(4*CAND_CAP) && s.all.reserve(CAND_CAP)
    && s.count.reserve(1) && s.sizes.reserve(4) && s.nout.reserve(1) && s.out.reserve(128);
}
static void reset() {state.identity=nullptr;}
static int detect(const nr_csirs_disc_ring_t *r,const nr_csirs_disc_fp_cfg_t *cfg,nr_csirs_disc_fp_t *out,int maxout,bool scheduled)
{
  if(!nr_disc_fp_valid(r) || maxout<1) return -1;
  auto &s=state;int rb=r->n_rb,tiles=(rb+15)/16,pooled=tiles*168,nr=14*12*rb+pooled;
  uint16_t symbols=cfg?cfg->excl_sym_mask_any:0;
  bool changed=s.identity!=r->grid_base || s.rb!=rb || s.ant!=r->n_ant || s.capacity!=r->capacity || r->written<s.written || symbols!=s.symbols;
  for(int j=0;j<14*12*rb && !changed;++j) changed=s.host_exclude[j]!=(cfg && cfg->excl_re?cfg->excl_re[j]:0);
  if(changed) {
    if(!prepare(rb,r->n_ant,r->capacity)) return -1;
    s.rb=rb;s.ant=r->n_ant;s.capacity=r->capacity;s.symbols=symbols;s.nt=0;s.round=0;
    s.written=r->written>(uint64_t)r->capacity?r->written-r->capacity:0;
    memset(s.counts,0,sizeof(s.counts));
    if(cfg && cfg->excl_re) memcpy(s.host_exclude,cfg->excl_re,14*12*rb);else memset(s.host_exclude,0,14*12*rb);
    if(disc_copy(s.exclude.p,s.host_exclude,14*12*rb,cudaMemcpyHostToDevice)!=cudaSuccess) return -1;
    if(disc_zero(s.sum.p,0,640ull*nr*8)!=cudaSuccess || disc_zero(s.sq.p,0,640ull*pooled*8)!=cudaSuccess
      || disc_zero(s.s80.p,0,80ull*nr*8)!=cudaSuccess || disc_zero(s.s64.p,0,64ull*nr*8)!=cudaSuccess
      || disc_zero(s.total.p,0,nr*8)!=cudaSuccess || disc_zero(s.totalq.p,0,nr*8)!=cudaSuccess
      || disc_zero(s.sizes.p,0,4*sizeof(int))!=cudaSuccess) return -1;
    s.identity=r->grid_base;
  }
  if(r->written-s.written>(uint64_t)r->capacity) return -1;
  uint64_t fresh=r->written-s.written;
  while(s.written<r->written) {
    int first=s.written%r->capacity,n=std::min<uint64_t>(100,std::min<uint64_t>(r->capacity-first,r->written-s.written));
    const int16_t *grid=r->device_valid && r->device_grid?r->device_grid+(size_t)first*r->slot_stride:s.grid.p;
    if(!(r->device_valid && r->device_grid) && disc_copy(s.grid.p,r->grid_base+(size_t)first*r->slot_stride,n*r->slot_stride*2,cudaMemcpyHostToDevice)!=cudaSuccess) return -1;
    if(disc_copy(s.meta.p,r->meta+first,n*sizeof(*r->meta),cudaMemcpyHostToDevice)!=cudaSuccess || disc_zero(s.x.p,0,(size_t)n*nr*4)!=cudaSuccess) return -1;
    normalize<<<(n*rb*14+127)/128,128,0,disc_stream()>>>(grid,s.meta.p,r->slot_stride,n,r->n_ant,rb,symbols,s.exclude.p,s.x.p,tiles);
    pool<<<(n*pooled+127)/128,128,0,disc_stream()>>>(s.x.p,n,rb,tiles);
    accumulate_kernel<<<(nr+127)/128,128,0,disc_stream()>>>(s.x.p,s.meta.p,n,nr,pooled,s.sum.p,s.sq.p,s.s80.p,s.s64.p,s.total.p,s.totalq.p);
    if(cudaGetLastError()!=cudaSuccess || disc_wait()!=cudaSuccess) {reset();return -1;}
    for(int i=0;i<n;++i) if(r->meta[first+i].valid) {++s.counts[r->meta[first+i].abs_slot%640];++s.nt;}
    s.written+=n;
  }
  static const int periods[]={4,5,8,10,16,20,32,40,64,80,160,320,640};
  int group=scheduled && fresh<(uint64_t)r->capacity?s.round++%4:-1;
  for(int g=0;g<4;++g) if(group<0 || group==g) {
    bool clear=true;
    for(int pi=g;pi<13;pi+=4) {
      int p=periods[pi];uint64_t counts[640]={};for(int i=0;i<640;++i) counts[i%p]+=s.counts[i];
      if(disc_copy(s.dc.p,counts,p*8,cudaMemcpyHostToDevice)!=cudaSuccess
         || disc_zero(s.masks.p,0,(size_t)p*14*tiles*6*2)!=cudaSuccess || disc_zero(s.count.p,0,4)!=cudaSuccess) return -1;
      lattice_pool<<<(p*pooled+127)/128,128,0,disc_stream()>>>(p,nr,pooled,s.sum.p,s.sq.p,s.ps.p,s.pq.p);
      lattice_screen<<<(p*14+63)/64,64,0,disc_stream()>>>(p,tiles,s.nt,s.dc.p,s.ps.p,s.pq.p,s.total.p+nr-pooled,s.totalq.p+nr-pooled,s.masks.p);
      segment<<<(p*14+31)/32,32,0,disc_stream()>>>(rb,p,s.nt,s.dc.p,s.sum.p,s.s80.p,s.s64.p,s.total.p,s.totalq.p,s.masks.p,symbols,
        cfg && cfg->min_strength>0?cfg->min_strength:2.f,s.raw.p,s.count.p);
      collect<<<1,1,0,disc_stream()>>>(s.raw.p,s.count.p,s.groups.p,s.sizes.p,g,clear);clear=false;
      unsigned count=0;if(disc_copy(&count,s.count.p,4,cudaMemcpyDeviceToHost)!=cudaSuccess || count>GPU_CAND_CAP) return -1;
    }
  }
  finish<<<1,1,0,disc_stream()>>>(s.groups.p,s.sizes.p,s.all.p,s.out.p,s.nout.p,std::min(maxout,128));
  int n=0;if(cudaGetLastError()!=cudaSuccess || disc_copy(&n,s.nout.p,4,cudaMemcpyDeviceToHost)!=cudaSuccess
     || disc_copy(out,s.out.p,n*sizeof(*out),cudaMemcpyDeviceToHost)!=cudaSuccess) return -1;
  return n;
}
} // namespace resident
