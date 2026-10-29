/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* T16 resident history, lattice and RB segmentation. Included after the shared
 * normalization/lattice kernels. Only final footprints cross back to the CPU. */
namespace resident {
constexpr int CAND_CAP=2048, GPU_CAND_CAP=65536;
struct candidate_t { nr_csirs_disc_fp_t fp; double significance; };
/* gcd_period: device helper: greatest common divisor of two periods (see the CPU gcd_period() in nr_csirs_disc_fp.c). */
__host__ __device__ static int gcd_period(int a,int b)
{
  while(b) {int t=a%b;a=b;b=t;}return a;
}

/* rank_candidate: device comparator of footprint candidates (same order as the CPU rank_candidate()): significance x (strength - 1), then period, offset, symbol mask and RB start. */
__host__ __device__ static int rank_candidate(const void *a, const void *b)
{
  const candidate_t *x=(const candidate_t *)a,*y=(const candidate_t *)b;
  double xs=x->significance*(x->fp.strength-1), ys=y->significance*(y->fp.strength-1);
  if(xs!=ys) return xs<ys ? 1:-1;
  if(x->fp.period!=y->fp.period) return x->fp.period-y->fp.period;
  if(x->fp.offset!=y->fp.offset) return x->fp.offset-y->fp.offset;
  if(x->fp.sym_mask!=y->fp.sym_mask) return x->fp.sym_mask-y->fp.sym_mask;
  return x->fp.rb_start-y->fp.rb_start;
}
/* add_candidate: device bounded max-heap insert of a candidate (CAND_CAP entries), same policy as the CPU add_candidate(). */
__host__ __device__ static void add_candidate(candidate_t *heap, int *count, candidate_t c)
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

/* rank_strength: device comparator of finished footprints (strength, period, offset, symbol mask); same order as the CPU rank_strength(). */
__host__ __device__ static int rank_strength(const void *a, const void *b)
{
  const nr_csirs_disc_fp_t *x=(const nr_csirs_disc_fp_t *)a,*y=(const nr_csirs_disc_fp_t *)b;
  if(x->strength!=y->strength) return x->strength<y->strength ? 1:-1;
  if(x->period!=y->period) return x->period-y->period;
  if(x->offset!=y->offset) return x->offset-y->offset;
  return x->sym_mask-y->sym_mask;
}


template<class T, class Compare> __host__ __device__ void device_sort(T *a,int n,Compare cmp)
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
/* finish_candidates: device version of finish_candidates(): drops duplicates and harmonics, merges symbol masks, removes contained fragments, and orders the output by strength.
 *   Uses device_sort() for the ordering. Returns the number of footprints written (at most max_out). Caller: finish().
 */
__host__ __device__ static int finish_candidates(candidate_t *cand,int nc,nr_csirs_disc_fp_t *out,int max_out)
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

/* segment: GPU kernel: the segmentation step of the CPU cluster() for one (period, offset, symbol) per thread, over the masks produced by lattice_screen().
 *   Builds the per-RB evidence from the history, segments each parity and density, and appends each significant candidate to cand[] through an atomic counter (capped at GPU_CAND_CAP). Launched by detect().
 */
/* T6b: the body of segment() for one id, working in caller-provided per-thread arrays: dr/vr/br (12*rb doubles each) and d/v/base/used (rb each). The local-memory kernel
 * (segment, 86 KB frame per thread on the stack: GB10) and the scratch kernel (segment_scratch: the same arrays in a global scratch sized by the launched threads) share it,
 * so the arithmetic is identical. */
__device__ __forceinline__ void segment_body(int id,int rb_count,int p,uint64_t nt,const uint64_t *counts,
    const uint64_t *sum,const uint64_t *sum80,const uint64_t *sum64,
    const uint64_t *tot,const uint64_t *totq,const uint16_t *masks,
    uint16_t excluded,float min_strength,candidate_t *cand,unsigned *count,
    double *dr,double *vr,double *br,double *d,double *v,double *base,bool *used)
{
  int o=id/14,l=id%14,tiles=(rb_count+15)/16,nk=12*rb_count,nr=14*nk+168*tiles;
  uint64_t on=counts[o],off=nt-on;
  if(on<3 || off<3 || (excluded&(1<<l))) return;
  bool any=false;for(int k=0;k<tiles*6;++k) any|=masks[(size_t)id*tiles*6+k]!=0;
  if(!any) return;
  double ion=1.0/on/4096.,ioff=1.0/off/4096.;
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
        for(int rb=0;rb<rb_count;++rb) {d[rb]=0;v[rb]=0;base[rb]=0;}
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
          for(int rb=0;rb<rb_count;++rb) used[rb]=false;
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
__global__ void segment(int rb_count,int p,uint64_t nt,const uint64_t *counts,
    const uint64_t *sum,const uint64_t *sum80,const uint64_t *sum64,
    const uint64_t *tot,const uint64_t *totq,const uint16_t *masks,
    uint16_t excluded,float min_strength,candidate_t *cand,unsigned *count,int bx0=0)
{
  int id=(blockIdx.x+bx0)*blockDim.x+threadIdx.x;if(id>=p*14) return;
  double dr[NR_CSIRS_DISC_MAX_RB*12],vr[NR_CSIRS_DISC_MAX_RB*12],br[NR_CSIRS_DISC_MAX_RB*12];
  double d[NR_CSIRS_DISC_MAX_RB],v[NR_CSIRS_DISC_MAX_RB],base[NR_CSIRS_DISC_MAX_RB];
  bool used[NR_CSIRS_DISC_MAX_RB];
  segment_body(id,rb_count,p,nt,counts,sum,sum80,sum64,tot,totq,masks,excluded,min_strength,cand,count,dr,vr,br,d,v,base,used);
}
/* segment_scratch: T6b. The same job with the per-thread arrays in global memory (scr: scratch_doubles(rb) doubles per launched thread slot) so the kernel's local frame is
 * small and the driver does not reserve frame x SMs x max threads of local memory (4.2 GB on a 34-SM card). The launch is grid-stride: ids [bx0*32, bx_end*32) are walked by
 * the launched threads, thread slot = blockIdx.x*32+threadIdx.x. Candidate order differs (atomic arrival, as before); the candidates and their values do not. */
__global__ void segment_scratch(int rb_count,int p,uint64_t nt,const uint64_t *counts,
    const uint64_t *sum,const uint64_t *sum80,const uint64_t *sum64,
    const uint64_t *tot,const uint64_t *totq,const uint16_t *masks,
    uint16_t excluded,float min_strength,candidate_t *cand,unsigned *count,int bx0,int bx_end,double *scr,size_t slot_doubles)
{
  const int slot=blockIdx.x*blockDim.x+threadIdx.x;
  double *dr=scr+(size_t)slot*slot_doubles,*vr=dr+12*rb_count,*br=vr+12*rb_count,*d=br+12*rb_count,*v=d+rb_count,*base=v+rb_count;
  bool *used=(bool *)(base+rb_count);
  const int last=min(bx_end*(int)blockDim.x,p*14);
  for(int id=bx0*blockDim.x+slot;id<last;id+=gridDim.x*blockDim.x)
    segment_body(id,rb_count,p,nt,counts,sum,sum80,sum64,tot,totq,masks,excluded,min_strength,cand,count,dr,vr,br,d,v,base,used);
}
static size_t segment_scratch_doubles(int rb) {return (size_t)39*rb+((size_t)rb+7)/8+1;}
/* collect: GPU kernel (single thread): merges the raw candidates of one scheduling group into that group's heap (capped at CAND_CAP), clearing it first when requested. Launched by detect(). */
__global__ void collect(candidate_t *raw,unsigned *count,candidate_t *groups,int *sizes,int group,bool clear)
{
  if(clear) sizes[group]=0;
  int n=sizes[group];
  for(unsigned i=0;i<min(*count,(unsigned)GPU_CAND_CAP);++i) add_candidate(groups+group*CAND_CAP,&n,raw[i]);
  sizes[group]=n;
}
/* finish_host: merges the four scheduling groups and runs finish_candidates() (the same code as on the CPU reference, compiled for the host). It used to be a <<<1,1>>> kernel:
 *   one GPU thread ran this job for 6-56 ms [MEASURED gsV2N], a stall of the real-time graphs' body launches (nr_gpu_chunk.h); on a CPU core it takes well under a millisecond. */
static int finish_host(candidate_t *all,const candidate_t *groups,const int *sizes,nr_csirs_disc_fp_t *out,int maxout)
{
  int n=0;for(int g=0;g<4;++g) for(int i=0;i<sizes[g];++i) add_candidate(all,&n,groups[g*CAND_CAP+i]);
  return finish_candidates(all,n,out,maxout);
}
struct State {
  Buffer<uint64_t> sum,sq,s80,s64,total,totalq,ps,pq,dc;
  Buffer<uint32_t> x;
  Buffer<nr_csirs_disc_slot_meta_t> meta;
  Buffer<int16_t> grid;
  Buffer<uint8_t> exclude;
  Buffer<uint16_t> masks;
  Buffer<candidate_t> raw,groups;
  Buffer<unsigned> count;
  Buffer<double> scr; /* T6b: segment_scratch arrays (g_segment_blocks*32 thread slots) */
  Buffer<int> sizes;
  uint64_t counts[640]={},nt=0,written=0;
  const int16_t *identity=nullptr;
  int rb=0,ant=0,capacity=0,round=0;
  uint16_t symbols=0;uint8_t host_exclude[14*12*NR_CSIRS_DISC_MAX_RB]={};
  candidate_t all[CAND_CAP],groups_h[4*CAND_CAP]; /* host scratch of finish_host() / detect(): per cell (they were function statics shared by every caller) */
};
/* state: the resident detector of the calling cell (G4: one per cell; created on first use, in the cell's prepare before the RX fence). */
static State &state()
{
  CellWs &w=ws();
  if(!w.res) {
    std::lock_guard<std::mutex> g(g_ws_mu);
    if(!w.res) w.res=new State();
  }
  return *w.res;
}
/* prepare: reserves the resident detector buffers (history sums, pooled sums, totals, grid, metadata, masks, candidate heaps) for a carrier of rb RBs, ant antennas and capacity slots.
 *   Returns false when any allocation fails. Called by detect() on a configuration change and by prepare_host() in nr_csirs_disc_gpu.cu.
 */
static bool prepare(int rb,int ant,int capacity)
{
  NrMsScope ms_(NR_MS_CSIRS,NR_MS_PER_CELL); // GPUMEM: the resident detector state of the preparing cell
  auto &s=state();int pooled=168*((rb+15)/16),nr=14*12*rb+pooled;
  return s.sum.reserve((size_t)640*nr) && s.sq.reserve((size_t)640*pooled)
    && s.s80.reserve((size_t)80*nr) && s.s64.reserve((size_t)64*nr)
    && s.total.reserve(nr) && s.totalq.reserve(nr) && s.ps.reserve((size_t)640*pooled)
    && s.pq.reserve((size_t)640*pooled) && s.dc.reserve(640) && s.x.reserve((size_t)100*nr)
    && s.meta.reserve(100) && s.grid.reserve((size_t)100*ant*14*24*rb)
    && s.exclude.reserve(14*12*rb) && s.masks.reserve((size_t)640*14*((rb+15)/16)*6)
    && s.raw.reserve(GPU_CAND_CAP) && s.groups.reserve(4*CAND_CAP)
    && s.count.reserve(1) && s.sizes.reserve(4)
    && (!g_segment_scratch || s.scr.reserve((size_t)g_segment_blocks*32*segment_scratch_doubles(rb)));
}
/* reset: forgets the resident history (clears the cached identity of the ring grid) so that the next detect() rebuilds it.
 *   Called by nr_csirs_disc_gpu.cu (resident reset after the warm-up) and by the detector reset path.
 */
static void reset() {state().identity=nullptr;}
/* detect: resident GPU footprint detector: folds the new slots of the ring into the device history (normalise, pool, accumulate), then runs lattice_pool/lattice_screen/segment per period group and finishes the footprints on the device.
 *   Rebuilds the history on a change of grid, geometry, symbol mask or exclusion mask. Only the final footprints are copied back to the host. Returns the count, or -1 (with the state reset) on any CUDA error.
 *   Caller: the GPU footprint entry in nr_csirs_disc_gpu.cu (footprints() / prepare_host()).
 */
static int detect(const nr_csirs_disc_ring_t *r,const nr_csirs_disc_fp_cfg_t *cfg,nr_csirs_disc_fp_t *out,int maxout,bool scheduled)
{
  if(!nr_disc_fp_valid(r) || maxout<1) return -1;
  auto &s=state();CellWs &cw=ws();int rb=r->n_rb,tiles=(rb+15)/16,pooled=tiles*168,nr=14*12*rb+pooled;
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
    NrGpuChunker &c_norm=cw.c_norm,&c_pool=cw.c_pool,&c_acc=cw.c_acc;
    if(disc_chunks(c_norm,(n*rb*14+127)/128,[&](long f,long k){normalize<<<k,128,0,disc_stream()>>>(grid,s.meta.p,r->slot_stride,n,r->n_ant,rb,symbols,s.exclude.p,s.x.p,tiles,1,(int)f);})!=cudaSuccess
       || disc_chunks(c_pool,(n*pooled+127)/128,[&](long f,long k){pool<<<k,128,0,disc_stream()>>>(s.x.p,n,rb,tiles,(int)f);})!=cudaSuccess
       || disc_chunks(c_acc,(nr+127)/128,[&](long f,long k){accumulate_kernel<<<k,128,0,disc_stream()>>>(s.x.p,s.meta.p,n,nr,pooled,s.sum.p,s.sq.p,s.s80.p,s.s64.p,s.total.p,s.totalq.p,(int)f);})!=cudaSuccess
       || disc_wait()!=cudaSuccess) {reset();return -1;}
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
      NrGpuChunker &c_lp=cw.c_lp,&c_ls=cw.c_ls,&c_seg=cw.c_seg;
      if(disc_chunks(c_lp,(p*pooled+127)/128,[&](long f,long k){lattice_pool<<<k,128,0,disc_stream()>>>(p,nr,pooled,s.sum.p,s.sq.p,s.ps.p,s.pq.p,(int)f);})!=cudaSuccess
         || disc_chunks(c_ls,(p*14+63)/64,[&](long f,long k){lattice_screen<<<k,64,0,disc_stream()>>>(p,tiles,s.nt,s.dc.p,s.ps.p,s.pq.p,s.total.p+nr-pooled,s.totalq.p+nr-pooled,s.masks.p,(int)f);})!=cudaSuccess
         || disc_chunks(c_seg,(p*14+31)/32,[&](long f,long k){
              const float min_strength=cfg && cfg->min_strength>0?cfg->min_strength:2.f;
              if(g_segment_scratch) /* T6b: small local frame, per-thread arrays in the global scratch (s.scr) */
                segment_scratch<<<(unsigned)std::min<long>(k,g_segment_blocks),32,0,disc_stream()>>>(rb,p,s.nt,s.dc.p,s.sum.p,s.s80.p,s.s64.p,s.total.p,s.totalq.p,s.masks.p,symbols,
                  min_strength,s.raw.p,s.count.p,(int)f,(int)(f+k),s.scr.p,segment_scratch_doubles(rb));
              else
                segment<<<k,32,0,disc_stream()>>>(rb,p,s.nt,s.dc.p,s.sum.p,s.s80.p,s.s64.p,s.total.p,s.totalq.p,s.masks.p,symbols,min_strength,s.raw.p,s.count.p,(int)f);})!=cudaSuccess) return -1;
      unsigned count=0;
      { /* G4: the one-thread collect kernel and the copy that drains it hold the discovery ticket */
        NrGpuDiscTicket ticket;
        collect<<<1,1,0,disc_stream()>>>(s.raw.p,s.count.p,s.groups.p,s.sizes.p,g,clear);clear=false;
        if(disc_copy(&count,s.count.p,4,cudaMemcpyDeviceToHost)!=cudaSuccess || count>GPU_CAND_CAP) return -1;
      }
    }
  }
  candidate_t *groups_h=s.groups_h;
  int sizes_h[4];
  if(disc_copy(sizes_h,s.sizes.p,sizeof(sizes_h),cudaMemcpyDeviceToHost)!=cudaSuccess) return -1;
  for(int g=0;g<4;++g) {
    if(sizes_h[g]<0 || sizes_h[g]>CAND_CAP) return -1;
    if(sizes_h[g] && disc_copy(groups_h+g*CAND_CAP,s.groups.p+g*CAND_CAP,sizes_h[g]*sizeof(candidate_t),cudaMemcpyDeviceToHost)!=cudaSuccess) return -1;
  }
  return finish_host(s.all,groups_h,sizes_h,out,std::min(maxout,128));
}
} // namespace resident
