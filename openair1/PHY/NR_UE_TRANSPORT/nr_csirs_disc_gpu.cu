/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_csirs_disc_fp_internal.h"
#include <cuda_runtime.h>
#include <cstdlib>
#include <cstring>
#include <algorithm>

/* Dedicated nonblocking stream and blocking host completion: CUDA's default
 * spin-wait must not consume the single discovery CPU while kernels execute. */
struct DiscoveryStream {
  cudaStream_t stream=nullptr;cudaEvent_t done=nullptr;
  bool init() {
    if(stream && done) return true;
    return cudaStreamCreateWithFlags(&stream,cudaStreamNonBlocking)==cudaSuccess
        && cudaEventCreateWithFlags(&done,cudaEventBlockingSync|cudaEventDisableTiming)==cudaSuccess;
  }
};
static DiscoveryStream discovery_stream;
static cudaStream_t disc_stream() {return discovery_stream.stream;}
static cudaError_t disc_wait() {
  cudaError_t rc=cudaEventRecord(discovery_stream.done,disc_stream());
  return rc==cudaSuccess?cudaEventSynchronize(discovery_stream.done):rc;
}
static cudaError_t disc_copy(void *dst,const void *src,size_t n,cudaMemcpyKind kind) {
  cudaError_t rc=cudaMemcpyAsync(dst,src,n,kind,disc_stream());
  return rc==cudaSuccess?disc_wait():rc;
}
static cudaError_t disc_zero(void *dst,int value,size_t n) {return cudaMemsetAsync(dst,value,n,disc_stream());}

/* Integer normalization/reduction is bit-identical to the CPU, including
 * quantization and clipping. One block-independent thread per (slot,RB,symbol). */
__global__ static void normalize(const int16_t *grid,const nr_csirs_disc_slot_meta_t *meta,
                                 size_t stride,int nt,int na,int nb,uint16_t symbols,
                                 const uint8_t *exclude,uint32_t *x,int tiles)
{
  int id=blockIdx.x*blockDim.x+threadIdx.x;
  if(id>=nt*nb*14) return;
  int l=id%14,rb=(id/14)%nb,t=id/(14*nb),nk=12*nb,nr=14*nk+168*tiles;
  if(!meta[t].valid) return;
  uint64_t power[12]={0},total=0;int count=0;
  for(int sc=0;sc<12;++sc) {
    int k=12*rb+sc;
    if((symbols&(1<<l)) || (exclude && exclude[l*nk+k])) continue;
    for(int a=0;a<na;++a) {
      const int16_t *v=grid+(size_t)t*stride+((size_t)a*14+l)*2*nk+2*k;
      power[sc]+=(int64_t)v[0]*v[0]+(int64_t)v[1]*v[1];
    }
    total+=power[sc];++count;
  }
  uint64_t low[5]={UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX};
  for(int sc=0;sc<12;++sc) {
    int k=12*rb+sc;
    if((symbols&(1<<l)) || (exclude && exclude[l*nk+k])) continue;
    uint64_t v=power[sc];
    for(int i=0;i<5;++i) {uint64_t lo=v<low[i]?v:low[i];v=v>low[i]?v:low[i];low[i]=lo;}
  }
  int available=count;count=(count+2)/3;total=0;
  for(int i=0;i<count;++i) total+=low[i];
  for(int sc=0;sc<12;++sc) {
    uint64_t reference=total;
    if(available>count && power[sc]<=low[count-1]) reference+=low[count]-power[sc];
    uint64_t v=reference?power[sc]*count*4096/reference:0;
    x[(size_t)t*nr+l*nk+12*rb+sc]=v>8192?8192:v;
  }
}
__global__ static void pool(uint32_t *x,int nt,int nb,int tiles)
{
  int id=blockIdx.x*blockDim.x+threadIdx.x;
  if(id>=nt*168*tiles) return;
  int sc=id%12,l=(id/12)%14,tile=(id/168)%tiles,t=id/(168*tiles),nk=12*nb,nr=14*nk+168*tiles;
  int first=tiles==1?0:tile*16,end=tiles==1?nb:min(nb,first+16);
  uint64_t sum=0;
  for(int rb=first;rb<end;++rb) sum+=x[(size_t)t*nr+l*nk+12*rb+sc];
  x[(size_t)t*nr+14*nk+tile*168+l*12+sc]=sum/(end-first);
}
/* Each RE thread folds all slots; no atomics, no floating-point reduction. */
__global__ static void fold_kernel(const uint32_t *x,const nr_csirs_disc_slot_meta_t *meta,
                                   int nt,int nr,int p,uint64_t *sum,uint64_t *sq)
{
  int j=blockIdx.x*blockDim.x+threadIdx.x;
  if(j>=nr) return;
  for(int t=0;t<nt;++t) if(meta[t].valid) {
    uint64_t v=x[(size_t)t*nr+j];size_t b=(size_t)(meta[t].abs_slot%p)*nr+j;
    sum[b]+=v;sq[b]+=v*v;
  }
}
struct Fold {uint32_t *x;nr_csirs_disc_slot_meta_t *meta;int nt,nr;};
static int fold_gpu(void *opaque,int p,uint64_t *sum,uint64_t *sq)
{
  Fold *f=(Fold *)opaque;uint64_t *ds=nullptr,*dq=nullptr;
  size_t bytes=(size_t)p*f->nr*sizeof(*sum);int ret=-1;
  if(cudaMalloc(&ds,bytes)!=cudaSuccess || cudaMalloc(&dq,bytes)!=cudaSuccess) goto done;
  if(disc_zero(ds,0,bytes)!=cudaSuccess || disc_zero(dq,0,bytes)!=cudaSuccess) goto done;
  fold_kernel<<<(f->nr+127)/128,128,0,disc_stream()>>>(f->x,f->meta,f->nt,f->nr,p,ds,dq);
  if(cudaGetLastError()!=cudaSuccess || disc_copy(sum,ds,bytes,cudaMemcpyDeviceToHost)!=cudaSuccess ||
      disc_copy(sq,dq,bytes,cudaMemcpyDeviceToHost)!=cudaSuccess) goto done;
  ret=0;
done: cudaFree(ds);cudaFree(dq);return ret;
}
static int footprints(const nr_csirs_disc_ring_t *r,const nr_csirs_disc_fp_cfg_t *cfg,nr_csirs_disc_fp_t *out,int max_out)
{
  if(!nr_disc_fp_valid(r) || !out || max_out<=0) return -1;
  if(max_out>NR_CSIRS_DISC_MAX_FP) max_out=NR_CSIRS_DISC_MAX_FP;
  const int tiles=(r->n_rb+15)/16;
  int16_t *grid=nullptr;uint8_t *excl=nullptr;Fold f={nullptr,nullptr,r->capacity,14*12*r->n_rb+168*tiles};int ret=-1;
  size_t gb=(size_t)r->capacity*r->slot_stride*sizeof(int16_t), xb=(size_t)f.nt*f.nr*sizeof(uint32_t);
  /* Explicit-copy baseline accepts ordinary malloc or unified U1 rings. */
  if(cudaMalloc(&grid,gb)!=cudaSuccess || cudaMalloc(&f.meta,r->capacity*sizeof(*f.meta))!=cudaSuccess ||
     cudaMalloc(&f.x,xb)!=cudaSuccess) goto done;
  if(disc_copy(grid,r->grid_base,gb,cudaMemcpyHostToDevice)!=cudaSuccess ||
     disc_copy(f.meta,r->meta,r->capacity*sizeof(*f.meta),cudaMemcpyHostToDevice)!=cudaSuccess ||
     disc_zero(f.x,0,xb)!=cudaSuccess) goto done;
  if(cfg && cfg->excl_re) {
    if(cudaMalloc(&excl,14*12*r->n_rb)!=cudaSuccess || disc_copy(excl,cfg->excl_re,14*12*r->n_rb,cudaMemcpyHostToDevice)!=cudaSuccess) goto done;
  }
  normalize<<<(r->capacity*r->n_rb*14+127)/128,128,0,disc_stream()>>>(grid,f.meta,r->slot_stride,r->capacity,r->n_ant,r->n_rb,
                                                cfg?cfg->excl_sym_mask_any:0,excl,f.x,tiles);
  if(cudaGetLastError()!=cudaSuccess) goto done;
  pool<<<(r->capacity*168*tiles+127)/128,128,0,disc_stream()>>>(f.x,r->capacity,r->n_rb,tiles);
  if(cudaGetLastError()!=cudaSuccess || disc_wait()!=cudaSuccess) goto done;
  ret=nr_disc_fp_cluster(r,cfg,out,max_out,fold_gpu,&f);
done: cudaFree(grid);cudaFree(f.meta);cudaFree(f.x);cudaFree(excl);return ret;
}
static int normalize_host(const nr_csirs_disc_ring_t *r,const nr_csirs_disc_fp_cfg_t *cfg,uint32_t *out)
{
  if(!nr_disc_fp_valid(r) || !out) return -1;
  const int tiles=1;
  int16_t *grid=nullptr;uint8_t *excl=nullptr;Fold f={nullptr,nullptr,r->capacity,14*12*r->n_rb+168};int ret=-1;
  size_t gb=(size_t)r->capacity*r->slot_stride*sizeof(int16_t), xb=(size_t)f.nt*f.nr*sizeof(uint32_t);
  /* Explicit-copy baseline accepts ordinary malloc or unified U1 rings. */
  if(cudaMalloc(&grid,gb)!=cudaSuccess || cudaMalloc(&f.meta,r->capacity*sizeof(*f.meta))!=cudaSuccess ||
     cudaMalloc(&f.x,xb)!=cudaSuccess) goto done;
  if(disc_copy(grid,r->grid_base,gb,cudaMemcpyHostToDevice)!=cudaSuccess ||
     disc_copy(f.meta,r->meta,r->capacity*sizeof(*f.meta),cudaMemcpyHostToDevice)!=cudaSuccess ||
     disc_zero(f.x,0,xb)!=cudaSuccess) goto done;
  if(cfg && cfg->excl_re) {
    if(cudaMalloc(&excl,14*12*r->n_rb)!=cudaSuccess || disc_copy(excl,cfg->excl_re,14*12*r->n_rb,cudaMemcpyHostToDevice)!=cudaSuccess) goto done;
  }
  normalize<<<(r->capacity*r->n_rb*14+127)/128,128,0,disc_stream()>>>(grid,f.meta,r->slot_stride,r->capacity,r->n_ant,r->n_rb,
                                                cfg?cfg->excl_sym_mask_any:0,excl,f.x,tiles);
  if(cudaGetLastError()!=cudaSuccess) goto done;
  pool<<<(r->capacity*168*tiles+127)/128,128,0,disc_stream()>>>(f.x,r->capacity,r->n_rb,tiles);
  if(cudaGetLastError()!=cudaSuccess || disc_wait()!=cudaSuccess) goto done;
  ret=disc_copy(out,f.x,xb,cudaMemcpyDeviceToHost)==cudaSuccess?0:-1;
done: cudaFree(grid);cudaFree(f.meta);cudaFree(f.x);cudaFree(excl);return ret;
}
static int lattice_host(int,int,uint64_t,const uint64_t *,const uint64_t *,const uint64_t *,const uint64_t *,const uint64_t *,uint16_t *);
static int fep_host(const nr_csirs_disc_fep_cfg_t *,const nr_csirs_disc_fep_slot_t *,int,nr_csirs_disc_ring_t *,int);
static int accumulate_host(const nr_csirs_disc_ring_t *,const nr_csirs_disc_fp_cfg_t *,uint64_t *,uint64_t *,uint64_t *,uint64_t *,uint64_t *,uint64_t *);
static int fft_host(int, int, const int16_t *, int16_t *);
static int prepare_host(int,int,int,int,size_t);
static int readback(const nr_csirs_disc_ring_t *r) {
  return r->device_valid && r->device_grid
      ? (disc_copy(r->grid_base,r->device_grid,(size_t)r->capacity*r->slot_stride*2,cudaMemcpyDeviceToHost)==cudaSuccess?0:-1):0;
}
static int upload_symbols(nr_csirs_disc_ring_t *r,uint16_t symbols)
{
  if(!r->device_grid) return -1;
  for(int a=0;a<r->n_ant;++a) for(int l=0;l<14;++l) if(symbols&(1u<<l)) {
    size_t at=((size_t)a*14+l)*24*r->n_rb;
    if(cudaMemcpyAsync(r->device_grid+at,r->grid_base+at,24*r->n_rb*sizeof(int16_t),cudaMemcpyHostToDevice,disc_stream())!=cudaSuccess) return -1;
  }
  if(disc_wait()!=cudaSuccess) return -1;
  r->device_valid=true;return 0;
}
static void *device_alloc(size_t n) {void *p=nullptr;return cudaMalloc(&p,n)==cudaSuccess?p:nullptr;}
static void device_free(void *p) {cudaFree(p);}
namespace resident {static int detect(const nr_csirs_disc_ring_t *,const nr_csirs_disc_fp_cfg_t *,nr_csirs_disc_fp_t *,int,bool);static void reset();}
extern "C" const nr_csirs_disc_gpu_ops_t *nr_csirs_disc_gpu_ops(void)
{
  if(!discovery_stream.init()) return nullptr;
  int devices=0;if(cudaGetDeviceCount(&devices)!=cudaSuccess || devices<1) return nullptr;
  static const nr_csirs_disc_gpu_ops_t ops={footprints,nullptr,normalize_host,fft_host,fep_host,accumulate_host,lattice_host,device_alloc,device_free,prepare_host,resident::detect,resident::reset,nullptr,nullptr,nullptr,readback,upload_symbols};return &ops;
}
/* Host staging is pinned by default. Managed memory is an explicit diagnostic
 * opt-in; production FFT, history and sequence workspace use cudaMalloc. */
static bool use_managed(void) {const char *e=getenv("ISAC_CSIRS_DISC_MANAGED");return e && atoi(e)==1;}
extern "C" void *nr_csirs_disc_gpu_alloc_unified(size_t bytes)
{
  void *ptr=nullptr;
  if(!bytes) return nullptr;
  if(use_managed()) return cudaMallocManaged(&ptr,bytes)==cudaSuccess?ptr:nullptr;
  return cudaHostAlloc(&ptr,bytes,cudaHostAllocMapped|cudaHostAllocPortable)==cudaSuccess?ptr:nullptr;
}
extern "C" void nr_csirs_disc_gpu_free_unified(void *ptr)
{
  if(!ptr) return;
  cudaPointerAttributes a{};
  if(cudaPointerGetAttributes(&a,ptr)==cudaSuccess && a.type==cudaMemoryTypeHost) cudaFreeHost(ptr);
  else cudaFree(ptr);
}

/* T8: only the worker calls this entry. Buffers and plans are retained between
 * batches; all FFT staging is unified memory on the DGX. OAI's fixed-point
 * butterfly rounding is not reproduced by floating-point cuFFT. */
#include <cufft.h>
#include <mutex>
__global__ static void fft_unpack(const short2 *in, cufftComplex *out, size_t n)
{
  size_t i = (size_t)blockIdx.x*blockDim.x+threadIdx.x;
  if (i<n) out[i] = make_float2(in[i].x, in[i].y);
}
__global__ static void fft_pack(const cufftComplex *in, short2 *out, size_t n, float scale)
{
  size_t i = (size_t)blockIdx.x*blockDim.x+threadIdx.x;
  if (i<n) out[i] = make_short2((short)fminf(32767.f,fmaxf(-32768.f,rintf(in[i].x*scale))),
                               (short)fminf(32767.f,fmaxf(-32768.f,rintf(in[i].y*scale))));
}
static int fft_host(int fft, int transforms, const int16_t *in, int16_t *out)
{
  static std::mutex mutex;
  std::lock_guard<std::mutex> guard(mutex);
  static cufftHandle plan;
  static int last_fft, last_transforms;
  static cufftComplex *work;
  static size_t capacity;
  if (fft<=0 || transforms<=0 || !in || !out) return -1;
  const size_t n=(size_t)fft*transforms;
  if (last_fft!=fft || last_transforms!=transforms) {
    if (plan) cufftDestroy(plan);
    plan=0;last_fft=last_transforms=0;
    if (cufftPlan1d(&plan,fft,CUFFT_C2C,transforms)!=CUFFT_SUCCESS) return -1;
    cufftSetStream(plan,disc_stream());
    last_fft=fft;last_transforms=transforms;
  }
  if (capacity<n) {
    cudaFree(work);work=nullptr;capacity=0;
    if (cudaMalloc(&work,n*sizeof(*work))!=cudaSuccess) return -1;
    capacity=n;
  }
  fft_unpack<<<(n+255)/256,256,0,disc_stream()>>>((const short2 *)in,work,n);
  if (cudaGetLastError()!=cudaSuccess || cufftExecC2C(plan,work,work,CUFFT_FORWARD)!=CUFFT_SUCCESS) {
    disc_wait(); // Quiesce queued work before the caller's CPU fallback.
    return -1;
  }
  fft_pack<<<(n+255)/256,256,0,disc_stream()>>>(work,(short2 *)out,n,1.f/sqrtf((float)fft));
  const cudaError_t launch = cudaGetLastError();
  const cudaError_t complete = disc_wait();
  return launch==cudaSuccess && complete==cudaSuccess ? 0 : -1;
}

/* T9: preserve OAI's Q14 LUT/128-sample recurrence, including its quantization
 * drift. One CTA per (slot,symbol), shared ramp across all RX branches. */
__constant__ short sin_lut[129];
__device__ static short sat16(int64_t x) { return (short)max(-32768LL,min(32767LL,(long long)x)); }
__device__ static short2 lut_phase(double phase)
{
  int index=phase<0?512-((int)(-phase*512+0.5)%512):(int)(phase*512+0.5)%512;
  if(index<128) return make_short2(sin_lut[128-index],sin_lut[index]);
  if(index<256) return make_short2(-sin_lut[index-128],sin_lut[256-index]);
  if(index<384) return make_short2(-sin_lut[384-index],-sin_lut[index-256]);
  return make_short2(sin_lut[index-384],-sin_lut[512-index]);
}
__device__ static short2 multiply(short2 x,short2 y,int shift,bool saturate)
{
  // SIMD madd wraps the 32-bit sum; negate y.i in int16 as in OAI.
  int re=(int)((unsigned)(x.x*y.x)+(unsigned)(x.y*(short)-y.y));
  int im=(int)((unsigned)(x.x*y.y)+(unsigned)(x.y*y.x));
  re>>=shift;im>>=shift;
  return make_short2(saturate?sat16(re):(short)re,saturate?sat16(im):(short)im);
}
__global__ static void fep_unpack(nr_csirs_disc_fep_cfg_t cfg,const nr_csirs_disc_fep_slot_t *slots,
                                  cufftComplex *work)
{
  int t=blockIdx.x/14,l=blockIdx.x%14,lane=threadIdx.x;
  const auto &s=slots[t];
  __shared__ short2 ramp[128],step;
  if(lane==0 && s.compensate) {
    double inc=-s.fo/(cfg.samples_per_ms*1000);
    double phase=s.offset[l]*inc;phase-=(int)phase;
    for(int i=0;i<128;++i) {ramp[i]=lut_phase(phase);phase=__dadd_rn(phase,inc);}
    step=lut_phase(128*inc);
  }
  __syncthreads();
  short2 rot=s.compensate?ramp[lane]:make_short2(16384,0);
  for(int k=lane;k<cfg.fft;k+=128) {
    for(int a=0;a<cfg.n_ant;++a) {
      short2 v=((const short2 *)s.samples)[a*cfg.stride+s.offset[l]-s.offset[0]+k];
      if(s.compensate) v=multiply(v,rot,14,false); // mult_complex_vectors wraps
      work[((size_t)(t*cfg.n_ant+a)*14+l)*cfg.fft+k]=make_float2(v.x,v.y);
    }
    if(s.compensate) rot=multiply(rot,step,14,true); // rotate_cpx_vector saturates
  }
}
__device__ static short2 symbol_rotate(short2 x,short2 a,int mode)
{
  if(mode==2) { // vqdmulh followed by vqrdmlsh/vqrdmlah
    int re=sat16((int64_t)a.x*x.x>>15),im=sat16((int64_t)a.x*x.y>>15);
    return make_short2(sat16(((int64_t)re*32768-(int64_t)a.y*x.y+16384)>>15),
                       sat16(((int64_t)im*32768+(int64_t)a.y*x.x+16384)>>15));
  }
  if(mode==1) {
    int re=(int)((unsigned)(a.x*x.x)-(unsigned)(a.y*x.y));
    int im=(int)((unsigned)(a.x*x.y)+(unsigned)(a.y*x.x));
    return make_short2(sat16(((int64_t)re+16384)>>15),sat16(((int64_t)im+16384)>>15));
  }
  return multiply(x,a,15,true);
}
__global__ static void fep_extract(nr_csirs_disc_fep_cfg_t cfg,const nr_csirs_disc_fep_slot_t *slots,
                                   const cufftComplex *work,short2 *grid,int n,int capacity,int first)
{
  size_t id=(size_t)blockIdx.x*blockDim.x+threadIdx.x;
  int nk=12*cfg.n_rb;
  size_t stride=(size_t)cfg.n_ant*14*nk;
  if(id>=(size_t)n*stride) return;
  int t=id/stride,k=id%nk,l=id/nk%14,a=id/(14*nk)%cfg.n_ant;
  int bin=(cfg.first_carrier+k)%cfg.fft;
  cufftComplex v=work[((size_t)(t*cfg.n_ant+a)*14+l)*cfg.fft+bin];
  float scale=rsqrtf((float)cfg.fft);
  short2 x=make_short2(sat16((int)rintf(v.x*scale)),sat16((int)rintf(v.y*scale)));
  short2 rot=((const short2 *)slots[t].rotation)[l];rot.y=(short)-rot.y;
  // Match the two OAI ranges, including the odd-RB extra six bins and SIMD tail.
  int length=(cfg.n_rb+(cfg.n_rb&1))*6;
  int high=cfg.first_carrier-((cfg.n_rb&1)?6:0);
  int pos=bin<length?bin:bin>=high && bin<high+length?bin-high:-1;
  if(pos>=0) {
    x=symbol_rotate(x,rot,pos<(length&~3)?cfg.rotation_mode:0);
    if(pos<(length&~3)) {
      short2 shift=((const short2 *)cfg.timeshift)[bin];
      // oai_mm_cpx_mult conjugates the INPUT, including -32768 wrap.
      int re=(int)((unsigned)(x.x*shift.x)+(unsigned)((short)-x.y*shift.y));
      int im=(int)((unsigned)(x.x*shift.y)+(unsigned)(x.y*shift.x));
      x=make_short2(sat16(re>>15),sat16(im>>15));
    }
  }
  grid[(size_t)((first+t)%capacity)*stride+id%stride]=x;
}

template<class T> struct Buffer {
  T *p=nullptr;size_t capacity=0;
  ~Buffer() = default; /* static objects: no CUDA call after runtime teardown at exit() (T26, see nr_csirs_disc_gpu_sweep.cu) */
  bool reserve(size_t n) {
    if(n<=capacity) return true;
    cudaFree(p);p=nullptr;capacity=0;
    if(cudaMalloc(&p,n*sizeof(T))!=cudaSuccess) return false;
    capacity=n;return true;
  }
};
struct FepWorkspace {
  Buffer<cufftComplex> work;
  Buffer<nr_csirs_disc_fep_slot_t> desc;
  Buffer<short2> shift,td;
  cufftHandle plan=0,single_plan=0;int last_fft=0,last_ant=0;bool lut_ready=false;
};
static FepWorkspace fep_workspace;
static int fep_prepare(int fft,int ant,size_t stride)
{
  auto &f=fep_workspace;
  if(!f.lut_ready) {
    short host[129];for(int i=0;i<=128;++i) host[i]=(short)round(sin(3.14159265358979323846*i/256)*16384);
    if(cudaMemcpyToSymbol(sin_lut,host,sizeof(host))!=cudaSuccess) return -1;
    f.lut_ready=true;
  }
  if(f.last_fft!=fft || f.last_ant!=ant) {
    if(f.plan) cufftDestroy(f.plan);if(f.single_plan) cufftDestroy(f.single_plan);
    f.plan=f.single_plan=0;f.last_fft=f.last_ant=0;
    if(cufftPlan1d(&f.plan,fft,CUFFT_C2C,NR_CSIRS_DISC_DEFAULT_BATCH*ant*14)!=CUFFT_SUCCESS) return -1;
    if(cufftPlan1d(&f.single_plan,fft,CUFFT_C2C,ant*14)!=CUFFT_SUCCESS) return -1;
    cufftSetStream(f.plan,disc_stream());cufftSetStream(f.single_plan,disc_stream());
    f.last_fft=fft;f.last_ant=ant;
  }
  return f.work.reserve((size_t)NR_CSIRS_DISC_DEFAULT_BATCH*ant*14*fft)
      && f.desc.reserve(NR_CSIRS_DISC_DEFAULT_BATCH) && f.shift.reserve(fft)
      && f.td.reserve((size_t)NR_CSIRS_DISC_DEFAULT_BATCH*ant*stride)?0:-1;
}
static int fep_host(const nr_csirs_disc_fep_cfg_t *c,const nr_csirs_disc_fep_slot_t *slots,
                    int n,nr_csirs_disc_ring_t *ring,int first)
{
  static std::mutex mutex;std::lock_guard<std::mutex> guard(mutex);
  auto &work=fep_workspace.work;auto &desc=fep_workspace.desc;auto &shift=fep_workspace.shift;
  auto &plan=fep_workspace.plan;
  if(!c || !slots || !ring || n<1 || n>NR_CSIRS_DISC_DEFAULT_BATCH || c->fft<128 || c->fft%128
      || first<0 || first>=ring->capacity || ring->n_ant!=c->n_ant || ring->n_rb!=c->n_rb) return -1;
  if(fep_prepare(c->fft,c->n_ant,c->stride)<0) return -1;
  size_t count=(size_t)(n==1?1:NR_CSIRS_DISC_DEFAULT_BATCH)*c->n_ant*14*c->fft;
  nr_csirs_disc_fep_slot_t staged[NR_CSIRS_DISC_DEFAULT_BATCH];
  for(int i=0;i<n;++i) {
    staged[i]=slots[i];staged[i].samples=(int16_t *)(fep_workspace.td.p+(size_t)i*c->n_ant*c->stride);
    if(disc_copy((void *)staged[i].samples,slots[i].samples,c->n_ant*c->stride*sizeof(short2),cudaMemcpyHostToDevice)!=cudaSuccess) return -1;
  }
  if(disc_copy(desc.p,staged,n*sizeof(*slots),cudaMemcpyHostToDevice)!=cudaSuccess ||
     disc_copy(shift.p,c->timeshift,c->fft*sizeof(short2),cudaMemcpyHostToDevice)!=cudaSuccess) return -1;
  size_t active=(size_t)n*c->n_ant*14*c->fft;
  if(active<count && disc_zero(work.p+active,0,(count-active)*sizeof(cufftComplex))!=cudaSuccess) return -1;
  nr_csirs_disc_fep_cfg_t cfg=*c;cfg.timeshift=(int16_t *)shift.p;
  fep_unpack<<<n*14,128,0,disc_stream()>>>(cfg,desc.p,work.p);
  if(cudaGetLastError()!=cudaSuccess || cufftExecC2C(n==1?fep_workspace.single_plan:plan,work.p,work.p,CUFFT_FORWARD)!=CUFFT_SUCCESS) {
    disc_wait();return -1;
  }
  size_t out=(size_t)n*c->n_ant*14*12*c->n_rb;
  fep_extract<<<(out+255)/256,256,0,disc_stream()>>>(cfg,desc.p,work.p,(short2 *)(ring->device_grid?ring->device_grid:ring->grid_base),n,ring->capacity,first);
  ring->device_valid=ring->device_grid!=nullptr;
  if(ring->device_valid && getenv("ISAC_CSIRS_DISC_DUMP"))
    for(int i=0;i<n;++i) {size_t at=(size_t)((first+i)%ring->capacity)*ring->slot_stride;
      if(disc_copy(ring->grid_base+at,ring->device_grid+at,ring->slot_stride*2,cudaMemcpyDeviceToHost)!=cudaSuccess) return -1;
    }
  cudaError_t launch=cudaGetLastError(),complete=disc_wait();
  return launch==cudaSuccess && complete==cudaSuccess?0:-1;
}

__global__ static void accumulate_kernel(const uint32_t *x,const nr_csirs_disc_slot_meta_t *meta,
                                         int nt,int nr,int pooled,uint64_t *sum,uint64_t *sq,
                                         uint64_t *s80,uint64_t *s64,uint64_t *total,uint64_t *totalq)
{
  int j=blockIdx.x*blockDim.x+threadIdx.x;if(j>=nr) return;
  uint64_t st=total[j],qt=totalq[j];
  for(int t=0;t<nt;++t) if(meta[t].valid) {
    int residue=meta[t].abs_slot%640;uint64_t v=x[(size_t)t*nr+j],q=v*v;
    sum[(size_t)residue*nr+j]+=v;s80[(size_t)(residue%80)*nr+j]+=v;s64[(size_t)(residue%64)*nr+j]+=v;
    st+=v;qt+=q;
    if(j>=nr-pooled) sq[(size_t)residue*pooled+j-(nr-pooled)]+=q;
  }
  total[j]=st;totalq[j]=qt;
}
static int accumulate_host(const nr_csirs_disc_ring_t *r,const nr_csirs_disc_fp_cfg_t *cfg,
                            uint64_t *sum,uint64_t *sq,uint64_t *s80,uint64_t *s64,uint64_t *total,uint64_t *totalq)
{
  static std::mutex mutex;std::lock_guard<std::mutex> guard(mutex);
  static Buffer<int16_t> grid;static Buffer<nr_csirs_disc_slot_meta_t> meta;
  static Buffer<uint32_t> x;static Buffer<uint8_t> excl;
  if(!nr_disc_fp_valid(r)) return -1;
  int tiles=(r->n_rb+15)/16,nr=14*12*r->n_rb+168*tiles;
  size_t gb=(size_t)r->capacity*r->slot_stride,xb=(size_t)r->capacity*nr;
  if(!grid.reserve(gb) || !meta.reserve(r->capacity) || !x.reserve(xb) || !excl.reserve(14*12*r->n_rb)) return -1;
  // Reuse managed input directly, explicit-copy ordinary CPU fixture rings.
  cudaPointerAttributes attr={};
  bool managed=cudaPointerGetAttributes(&attr,r->grid_base)==cudaSuccess && (attr.type==cudaMemoryTypeManaged || attr.type==cudaMemoryTypeHost);
  cudaGetLastError();
  const int16_t *input=managed?r->grid_base:grid.p;
  if((!managed && disc_copy(grid.p,r->grid_base,gb*sizeof(int16_t),cudaMemcpyHostToDevice)!=cudaSuccess) ||
     disc_copy(meta.p,r->meta,r->capacity*sizeof(*r->meta),cudaMemcpyHostToDevice)!=cudaSuccess ||
     disc_zero(x.p,0,xb*sizeof(uint32_t))!=cudaSuccess) return -1;
  if(cfg && cfg->excl_re && disc_copy(excl.p,cfg->excl_re,14*12*r->n_rb,cudaMemcpyHostToDevice)!=cudaSuccess) return -1;
  normalize<<<(r->capacity*r->n_rb*14+127)/128,128,0,disc_stream()>>>(input,meta.p,r->slot_stride,r->capacity,r->n_ant,r->n_rb,
      cfg?cfg->excl_sym_mask_any:0,cfg && cfg->excl_re?excl.p:nullptr,x.p,tiles);
  pool<<<(r->capacity*168*tiles+127)/128,128,0,disc_stream()>>>(x.p,r->capacity,r->n_rb,tiles);
  accumulate_kernel<<<(nr+127)/128,128,0,disc_stream()>>>(x.p,meta.p,r->capacity,nr,168*tiles,sum,sq,s80,s64,total,totalq);
  cudaError_t launch=cudaGetLastError(),complete=disc_wait();
  return launch==cudaSuccess && complete==cudaSuccess?0:-1;
}

__global__ static void lattice_pool(int p,int nr,int pooled,const uint64_t *sum,const uint64_t *sq,
                                    uint64_t *ps,uint64_t *pq)
{
  int id=blockIdx.x*blockDim.x+threadIdx.x;if(id>=p*pooled) return;
  int o=id/pooled,j=id%pooled;uint64_t s=0,q=0;
  for(int residue=o;residue<640;residue+=p) {s+=sum[(size_t)residue*nr+nr-pooled+j];q+=sq[(size_t)residue*pooled+j];}
  ps[id]=s;pq[id]=q;
}
/* Device implementation of the CPU's tiled significance and phase-background
 * screen. Keep tile/component order: a failed first tile still marks tried. */
__global__ static void lattice_screen(int p,int tiles,uint64_t nt,const uint64_t *counts,
                                      const uint64_t *ps,const uint64_t *pq,const uint64_t *total,
                                      const uint64_t *totalq,uint16_t *masks)
{
  int id=blockIdx.x*blockDim.x+threadIdx.x;if(id>=p*14) return;
  int o=id/14,l=id%14,pooled=168*tiles;
  uint64_t on=counts[o],off=nt-on;if(on<3 || off<3) return;
  double ion=1.0/on/4096.,ioff=1.0/off/4096.;
  // 4096 bits, private to this (period,offset,symbol).
  uint32_t tried[128]={0};
  for(int tile=0;tile<tiles;++tile) {
    uint16_t observed=0,strong=0;
    for(int sc=0;sc<12;++sc) {
      int j=tile*168+l*12+sc;size_t at=(size_t)o*pooled+j;
      double a=ps[at]*ion,b=(total[j]-ps[at])*ioff;
      double va=fmax(0.,pq[at]*ion/4096.-a*a),vb=fmax(0.,(totalq[j]-pq[at])*ioff/4096.-b*b),diff=a-b;
      if(diff>0.025 && diff*diff>16*(vb/(on-1)+vb/(off-1))+1e-12) observed|=1<<sc;
      if(diff>0.025 && diff*diff>36*(fmax(va,vb)/(on-1)+vb/(off-1))+1e-12) strong|=1<<sc;
    }
    for(int component=-2;component<4;++component) {
      if(component==-1) {
        bool partial=false;for(int k=0;k<4;++k) partial|=(strong&(0x111u<<k))==strong;
        if(__popc(strong)<2 || partial) continue;
      }
      uint16_t mask=component==-2?strong:component==-1?
        (strong|((strong&0x555)<<1)|((strong&0xaaa)>>1)):(0x111u<<component);
      if(!mask || mask==0xfff || (component!=-1 && (mask&observed)!=mask) || (tried[mask/32]&(1u<<(mask%32)))) continue;
      if(__popc(mask)==1 && !(mask&strong)) continue;
      bool comb=mask==0x111 || mask==0x222 || mask==0x444 || mask==0x888;
      bool pairs=(mask&0x555)==((mask>>1)&0x555);
      if(__popc(mask)!=1 && !comb && !pairs) continue;
      tried[mask/32]|=1u<<(mask%32);
      double on_mean=0,off_mean=0,on_error=0;
      for(int sc=0;sc<12;++sc) if(mask&(1<<sc)) {
        int j=tile*168+l*12+sc;size_t at=(size_t)o*pooled+j;
        on_mean+=ps[at]*ion;off_mean+=(total[j]-ps[at])*ioff;
        double mean=ps[at]*ion;on_error+=fmax(0.,pq[at]*ion/4096.-mean*mean)/on;
      }
      int below=0,alternatives=0;
      for(int other=0;other<p;++other) if(other!=o && counts[other]) {
        double mean=0,error=0,scale=1.0/(counts[other]*4096.);
        for(int sc=0;sc<12;++sc) if(mask&(1<<sc)) {
          size_t at=(size_t)other*pooled+tile*168+l*12+sc;
          double v=ps[at]*scale,qv=pq[at]*scale/4096.;mean+=v;error+=fmax(0.,qv-v*v)/counts[other];
        }
        double delta=on_mean-mean;
        below+=mean<(on_mean+off_mean)*0.5 && delta>0 && delta*delta>9*(on_error+error);++alternatives;
      }
      if(2*below>alternatives) masks[((size_t)id*tiles+tile)*6+component+2]=mask;
    }
  }
}
static int lattice_host(int rb,int p,uint64_t nt,const uint64_t *counts,const uint64_t *sum,
                         const uint64_t *sq,const uint64_t *total,const uint64_t *totalq,uint16_t *masks)
{
  static std::mutex mutex;std::lock_guard<std::mutex> guard(mutex);
  static Buffer<uint64_t> ps,pq,dc;
  static Buffer<uint16_t> dm;
  int tiles=(rb+15)/16,pooled=168*tiles,nr=14*12*rb+pooled;
  size_t count=(size_t)p*14*tiles*6;
  if(!ps.reserve((size_t)p*pooled) || !pq.reserve((size_t)p*pooled) || !dc.reserve(p) || !dm.reserve(count)) return -1;
  if(disc_copy(dc.p,counts,p*sizeof(uint64_t),cudaMemcpyHostToDevice)!=cudaSuccess ||
     disc_zero(dm.p,0,count*sizeof(uint16_t))!=cudaSuccess) return -1;
  lattice_pool<<<(p*pooled+127)/128,128,0,disc_stream()>>>(p,nr,pooled,sum,sq,ps.p,pq.p);
  lattice_screen<<<(p*14+63)/64,64,0,disc_stream()>>>(p,tiles,nt,dc.p,ps.p,pq.p,total+nr-pooled,totalq+nr-pooled,dm.p);
  if(cudaGetLastError()!=cudaSuccess || disc_copy(masks,dm.p,count*sizeof(uint16_t),cudaMemcpyDeviceToHost)!=cudaSuccess) {
    disc_wait();return -1;
  }
  return 0;
}

#include "nr_csirs_disc_gpu_cluster.cuh"
static int prepare_host(int rb,int ant,int capacity,int fft,size_t stride)
{
  if(!resident::prepare(rb,ant,capacity) || fep_prepare(fft,ant,stride)<0) return -1;
  auto &f=fep_workspace;
  // Exercise plans and kernel local storage before RX, including the large
  // per-symbol segmentation frame. Subsequent calls only clear retained state.
  nr_csirs_disc_fep_slot_t descriptors[NR_CSIRS_DISC_DEFAULT_BATCH]={};
  for(int i=0;i<NR_CSIRS_DISC_DEFAULT_BATCH;++i) {
    descriptors[i].samples=(int16_t *)(f.td.p+(size_t)i*ant*stride);
    for(int l=0;l<14;++l) descriptors[i].offset[l]=l*fft;
  }
  nr_csirs_disc_fep_cfg_t cfg={};cfg.fft=fft;cfg.n_ant=ant;cfg.n_rb=rb;cfg.stride=stride;
  cfg.timeshift=(int16_t *)f.shift.p;
  if(disc_zero(f.td.p,0,f.td.capacity*sizeof(short2))!=cudaSuccess
     || disc_zero(f.shift.p,0,fft*sizeof(short2))!=cudaSuccess
     || disc_copy(f.desc.p,descriptors,sizeof(descriptors),cudaMemcpyHostToDevice)!=cudaSuccess) return -1;
  fep_unpack<<<NR_CSIRS_DISC_DEFAULT_BATCH*14,128,0,disc_stream()>>>(cfg,f.desc.p,f.work.p);
  if(cufftExecC2C(f.plan,f.work.p,f.work.p,CUFFT_FORWARD)!=CUFFT_SUCCESS
     || cufftExecC2C(f.single_plan,f.work.p,f.work.p,CUFFT_FORWARD)!=CUFFT_SUCCESS) return -1;
  size_t n=(size_t)NR_CSIRS_DISC_DEFAULT_BATCH*ant*14*12*rb;
  fep_extract<<<(n+255)/256,256,0,disc_stream()>>>(cfg,f.desc.p,f.work.p,(short2 *)resident::state.grid.p,100,100,0);
  int16_t dummy=0;nr_csirs_disc_slot_meta_t meta={};nr_csirs_disc_fp_t out[128];
  nr_csirs_disc_ring_t ring={};ring.n_ant=ant;ring.n_rb=rb;ring.capacity=1;ring.slots_per_frame=20;
  ring.slot_stride=(size_t)ant*14*24*rb;ring.grid_base=&dummy;ring.meta=&meta;
  if(resident::detect(&ring,nullptr,out,128,false)<0) return -1;
  auto &h=resident::state;int tiles=(rb+15)/16,nr=14*12*rb+168*tiles;
  if(disc_zero(h.meta.p,0,sizeof(meta))!=cudaSuccess || disc_zero(h.x.p,0,nr*4)!=cudaSuccess) return -1;
  normalize<<<(rb*14+127)/128,128,0,disc_stream()>>>(h.grid.p,h.meta.p,ring.slot_stride,1,ant,rb,0,nullptr,h.x.p,tiles);
  pool<<<(168*tiles+127)/128,128,0,disc_stream()>>>(h.x.p,1,rb,tiles);
  accumulate_kernel<<<(nr+127)/128,128,0,disc_stream()>>>(h.x.p,h.meta.p,1,nr,168*tiles,h.sum.p,h.sq.p,h.s80.p,h.s64.p,h.total.p,h.totalq.p);
  resident::reset();
  return cudaGetLastError()==cudaSuccess && disc_wait()==cudaSuccess?0:-1;
}
