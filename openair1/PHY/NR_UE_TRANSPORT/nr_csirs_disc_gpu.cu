/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_csirs_disc_fp_internal.h"
#include <cuda_runtime.h>
#include <cstdlib>
#include <cstring>

/* Integer normalization/reduction is bit-identical to the CPU, including
 * quantization and clipping. One block-independent thread per (slot,RB). */
__global__ static void normalize(const int16_t *grid,const nr_csirs_disc_slot_meta_t *meta,
                                 size_t stride,int nt,int na,int nb,uint16_t symbols,
                                 const uint8_t *exclude,uint32_t *x)
{
  int id=blockIdx.x*blockDim.x+threadIdx.x;
  if(id>=nt*nb) return;
  int t=id/nb,rb=id%nb,nk=12*nb,nr=14*nk+168;
  if(!meta[t].valid) return;
  uint64_t power[168],total=0;int count=0;
  for(int l=0;l<14;++l) for(int s=0;s<12;++s) {
    int k=12*rb+s,j=l*12+s;power[j]=0;
    if((symbols&(1<<l)) || (exclude && exclude[l*nk+k])) continue;
    for(int a=0;a<na;++a) {
      const int16_t *v=grid+(size_t)t*stride+((size_t)a*14+l)*2*nk+2*k;
      power[j]+=(int64_t)v[0]*v[0]+(int64_t)v[1]*v[1];
    }
    total+=power[j];++count;
  }
  for(int l=0;l<14;++l) for(int s=0;s<12;++s) {
    uint64_t v=total?power[l*12+s]*count*4096/total:0;
    x[(size_t)t*nr+l*nk+12*rb+s]=v>8192?8192:v;
  }
}
__global__ static void pool(uint32_t *x,int nt,int nb)
{
  int id=blockIdx.x*blockDim.x+threadIdx.x;
  if(id>=nt*168) return;
  int t=id/168,l=id%168/12,sc=id%12,nk=12*nb,nr=14*nk+168;
  uint64_t sum=0;
  for(int rb=0;rb<nb;++rb) sum+=x[(size_t)t*nr+l*nk+12*rb+sc];
  x[(size_t)t*nr+14*nk+l*12+sc]=sum/nb;
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
  int16_t *grid=nullptr;uint8_t *excl=nullptr;Fold f={nullptr,nullptr,r->capacity,14*12*r->n_rb+168};int ret=-1;
  size_t gb=(size_t)r->capacity*r->slot_stride*sizeof(int16_t), xb=(size_t)f.nt*f.nr*sizeof(uint32_t);
  /* Explicit-copy baseline accepts ordinary malloc or unified U1 rings. */
  if(cudaMalloc(&grid,gb)!=cudaSuccess || cudaMalloc(&f.meta,r->capacity*sizeof(*f.meta))!=cudaSuccess ||
     cudaMalloc(&f.x,xb)!=cudaSuccess) goto done;
  if(cudaMemcpy(grid,r->grid_base,gb,cudaMemcpyHostToDevice)!=cudaSuccess ||
     cudaMemcpy(f.meta,r->meta,r->capacity*sizeof(*f.meta),cudaMemcpyHostToDevice)!=cudaSuccess ||
     cudaMemset(f.x,0,xb)!=cudaSuccess) goto done;
  if(cfg && cfg->excl_re) {
    if(cudaMalloc(&excl,f.nr-168)!=cudaSuccess || cudaMemcpy(excl,cfg->excl_re,f.nr-168,cudaMemcpyHostToDevice)!=cudaSuccess) goto done;
  }
  normalize<<<(r->capacity*r->n_rb+127)/128,128>>>(grid,f.meta,r->slot_stride,r->capacity,r->n_ant,r->n_rb,
                                                cfg?cfg->excl_sym_mask_any:0,excl,f.x);
  if(cudaGetLastError()!=cudaSuccess) goto done;
  pool<<<(r->capacity*168+127)/128,128>>>(f.x,r->capacity,r->n_rb);
  if(cudaGetLastError()!=cudaSuccess || cudaDeviceSynchronize()!=cudaSuccess) goto done;
  ret=nr_disc_fp_cluster(r,cfg,out,max_out,fold_gpu,&f);
done: cudaFree(grid);cudaFree(f.meta);cudaFree(f.x);cudaFree(excl);return ret;
}
extern "C" const nr_csirs_disc_gpu_ops_t *nr_csirs_disc_gpu_ops(void)
{
  int devices=0;if(cudaGetDeviceCount(&devices)!=cudaSuccess || devices<1) return nullptr;
  static const nr_csirs_disc_gpu_ops_t ops={footprints,nullptr};return &ops;
}
extern "C" void *nr_csirs_disc_alloc_unified(size_t bytes)
{
  void *ptr=nullptr;return bytes && cudaMallocManaged(&ptr,bytes)==cudaSuccess?ptr:nullptr;
}
extern "C" void nr_csirs_disc_free_unified(void *ptr) { if(ptr) cudaFree(ptr); }
