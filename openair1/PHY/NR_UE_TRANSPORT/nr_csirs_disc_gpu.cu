/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_csirs_disc_fp_internal.h"
#include <cuda_runtime.h>
#include <cstdlib>
#include <cstring>

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
  if(cudaMemset(ds,0,bytes)!=cudaSuccess || cudaMemset(dq,0,bytes)!=cudaSuccess) goto done;
  fold_kernel<<<(f->nr+127)/128,128>>>(f->x,f->meta,f->nt,f->nr,p,ds,dq);
  if(cudaGetLastError()!=cudaSuccess || cudaMemcpy(sum,ds,bytes,cudaMemcpyDeviceToHost)!=cudaSuccess ||
      cudaMemcpy(sq,dq,bytes,cudaMemcpyDeviceToHost)!=cudaSuccess) goto done;
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
  if(cudaMemcpy(grid,r->grid_base,gb,cudaMemcpyHostToDevice)!=cudaSuccess ||
     cudaMemcpy(f.meta,r->meta,r->capacity*sizeof(*f.meta),cudaMemcpyHostToDevice)!=cudaSuccess ||
     cudaMemset(f.x,0,xb)!=cudaSuccess) goto done;
  if(cfg && cfg->excl_re) {
    if(cudaMalloc(&excl,14*12*r->n_rb)!=cudaSuccess || cudaMemcpy(excl,cfg->excl_re,14*12*r->n_rb,cudaMemcpyHostToDevice)!=cudaSuccess) goto done;
  }
  normalize<<<(r->capacity*r->n_rb*14+127)/128,128>>>(grid,f.meta,r->slot_stride,r->capacity,r->n_ant,r->n_rb,
                                                cfg?cfg->excl_sym_mask_any:0,excl,f.x,tiles);
  if(cudaGetLastError()!=cudaSuccess) goto done;
  pool<<<(r->capacity*168*tiles+127)/128,128>>>(f.x,r->capacity,r->n_rb,tiles);
  if(cudaGetLastError()!=cudaSuccess || cudaDeviceSynchronize()!=cudaSuccess) goto done;
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
  if(cudaMemcpy(grid,r->grid_base,gb,cudaMemcpyHostToDevice)!=cudaSuccess ||
     cudaMemcpy(f.meta,r->meta,r->capacity*sizeof(*f.meta),cudaMemcpyHostToDevice)!=cudaSuccess ||
     cudaMemset(f.x,0,xb)!=cudaSuccess) goto done;
  if(cfg && cfg->excl_re) {
    if(cudaMalloc(&excl,14*12*r->n_rb)!=cudaSuccess || cudaMemcpy(excl,cfg->excl_re,14*12*r->n_rb,cudaMemcpyHostToDevice)!=cudaSuccess) goto done;
  }
  normalize<<<(r->capacity*r->n_rb*14+127)/128,128>>>(grid,f.meta,r->slot_stride,r->capacity,r->n_ant,r->n_rb,
                                                cfg?cfg->excl_sym_mask_any:0,excl,f.x,tiles);
  if(cudaGetLastError()!=cudaSuccess) goto done;
  pool<<<(r->capacity*168*tiles+127)/128,128>>>(f.x,r->capacity,r->n_rb,tiles);
  if(cudaGetLastError()!=cudaSuccess || cudaDeviceSynchronize()!=cudaSuccess) goto done;
  ret=cudaMemcpy(out,f.x,xb,cudaMemcpyDeviceToHost)==cudaSuccess?0:-1;
done: cudaFree(grid);cudaFree(f.meta);cudaFree(f.x);cudaFree(excl);return ret;
}
static int fft_host(int, int, const int16_t *, int16_t *);
extern "C" const nr_csirs_disc_gpu_ops_t *nr_csirs_disc_gpu_ops(void)
{
  int devices=0;if(cudaGetDeviceCount(&devices)!=cudaSuccess || devices<1) return nullptr;
  static const nr_csirs_disc_gpu_ops_t ops={footprints,nullptr,normalize_host,fft_host};return &ops;
}
extern "C" void *nr_csirs_disc_gpu_alloc_unified(size_t bytes)
{
  void *ptr=nullptr;return bytes && cudaMallocManaged(&ptr,bytes)==cudaSuccess?ptr:nullptr;
}
extern "C" void nr_csirs_disc_gpu_free_unified(void *ptr) { if(ptr) cudaFree(ptr); }

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
    last_fft=fft;last_transforms=transforms;
  }
  if (capacity<n) {
    cudaFree(work);work=nullptr;capacity=0;
    if (cudaMallocManaged(&work,n*sizeof(*work))!=cudaSuccess) return -1;
    capacity=n;
  }
  fft_unpack<<<(n+255)/256,256>>>((const short2 *)in,work,n);
  if (cudaGetLastError()!=cudaSuccess || cufftExecC2C(plan,work,work,CUFFT_FORWARD)!=CUFFT_SUCCESS) {
    cudaDeviceSynchronize(); // Quiesce queued work before the caller's CPU fallback.
    return -1;
  }
  fft_pack<<<(n+255)/256,256>>>(work,(short2 *)out,n,1.f/sqrtf((float)fft));
  const cudaError_t launch = cudaGetLastError();
  const cudaError_t complete = cudaDeviceSynchronize();
  return launch==cudaSuccess && complete==cudaSuccess ? 0 : -1;
}
