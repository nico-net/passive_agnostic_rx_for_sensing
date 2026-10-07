/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include <cuda_runtime.h>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <chrono>
#include "nr_dl_gpu_memory.h"
#define NID_FN __device__
#include "nr_pdcch_nid_algorithm.h"

namespace {
constexpr int IDS=65536, CELLS=14*IDS, CHUNK=16;
struct Device {
 NidStage2 stage2;
 NidView view;
 float *values, *zsum;
 uint16_t *hits;
 float median[2*CHUNK], sigma[2*CHUNK];
 nr_pdcch_nid_result_t result;
};
struct HostTransfer {
 int slot[NR_PDCCH_NID_BATCH],sym[NR_PDCCH_NID_BATCH],control[2];
 nr_pdcch_nid_result_t result;
};
struct Context {
 Device host, *dev;
 cudaStream_t stream;
 cudaEvent_t event[3];
 nr_pdcch_nid_iq_t *staging;
 HostTransfer *transfer;
 nr_pdcch_nid_config_t cfg;
 int rb, dur, window, batches, budget_ms;
 void *alloc[16]; size_t bytes[16]; int nalloc;
};
__global__ void noop() {}
__global__ void reset(Device *p, NidView v) {
 if(threadIdx.x || blockIdx.x) return;
 p->view=v; p->stage2.g_idsw=v;
 p->result={};
}
// Prefix arithmetic follows the CPU oracle exactly (the old GPU sliding sum
// did not). --fmad=false, precise div/sqrt, no fast-math preserve decisions.
__device__ float best(const NidView &v,int k,const uint32_t *g,int lo,int hi,int exlo,int exhi,int m0) {
 // Keep the last six PREFIX values, not a sliding sum: subtraction and
 // rounding are identical to the CPU's full prefix arrays, with no large spills.
 float ar[6],ai[6],ap[6],sr=0,si=0,sp=0,b=0;
 const c16_t *re=v.grid(k);
 for(int c=lo;c<hi;c++) {
  float r=0,i=0,p=0;
  for(int q=0;q<3;q++) {
   float xr,xi; idsw_pilot(g,3*(c-m0)+q,&xr,&xi);
   c16_t y=re[v.point_a_sc+12*c+1+4*q];
   r+=y.r*xr-y.i*xi; i+=y.r*xi+y.i*xr;
   p+=(float)y.r*y.r+(float)y.i*y.i;
  }
  int j=c-lo,save=j%6;
  ar[save]=sr;ai[save]=si;ap[save]=sp;
  sr+=r;si+=i;sp+=p;
  if(j<5)continue;
  int first=c-5;if(exhi>exlo && first<exhi && first+6>exlo)continue;
  int old=(j+1)%6;
  float wr=sr-ar[old],wi=si-ai[old],wp=sp-ap[old];
  float value=wp>0?sqrtf((wr*wr+wi*wi)/(wp*36.f)):0;
  if(value>b)b=value;
 }
 return b;
}
__global__ void sweep(Device *p,int first,int count) {
 int id=blockIdx.x*blockDim.x+threadIdx.x,k=blockIdx.y;
 if(id>=IDS || k>=count) return;
 const NidView &v=p->view; int idx=v.index(first+k);
 uint64_t x=((uint64_t)v.sps*v.slot[idx]+v.sym[idx]+1)*((uint64_t)(id*2)+1);
 uint32_t x1=0,x2=(uint32_t)(((x<<17)+id*2)&0x7fffffff),g[54];
 int hi=(v.nsc-v.point_a_sc)/12,lo=v.point_a_sc>=0?0:(-v.point_a_sc+11)/12;
 int words=(6*hi)/32+2;
 for(int w=0;w<words;w++)g[w]=nid_gold(&x1,&x2,w==0);
 bool css=v.css0_nrb>=6 && v.css0_crb>=lo && v.css0_crb+v.css0_nrb<=hi;
 p->values[(2*k+1)*IDS+id]=best(v,first+k,g,lo,hi,css?v.css0_crb:0,css?v.css0_crb+v.css0_nrb:0,0);
 p->values[(2*k)*IDS+id]=css?best(v,first+k,g,v.css0_crb,v.css0_crb+v.css0_nrb,0,0,v.css0_crb):0;
}
__global__ void standardize(Device *p) {
 __shared__ unsigned h[4096];
 int k=blockIdx.x,tid=threadIdx.x;
 for(int j=tid;j<4096;j+=blockDim.x)h[j]=0;
 __syncthreads();
 const float *v=p->values+k*IDS;
 for(int i=tid;i<IDS;i+=blockDim.x) {
  int b=(int)(v[i]*4095);atomicAdd(h+(b<0?0:b>=4096?4095:b),1);
 }
 __syncthreads();
 if(tid==0) {
  unsigned a=0;int b=0;while(b<4095 && a+h[b]<IDS/2)a+=h[b++];
  p->median[k]=(b+0.5f)/4095;
 }
 __syncthreads();
 for(int j=tid;j<4096;j+=blockDim.x)h[j]=0;
 __syncthreads();
 for(int i=tid;i<IDS;i+=blockDim.x) {
  int b=(int)(fabsf(v[i]-p->median[k])*4095);atomicAdd(h+(b>=4096?4095:b),1);
 }
 __syncthreads();
 if(tid==0) {
  unsigned a=0;int b=0;while(b<4095 && a+h[b]<IDS/2)a+=h[b++];
  p->sigma[k]=fmaxf(1.4826f*((b+0.5f)/4095),1e-6f);
 }
}
__global__ void accumulate(Device *p,int first,int count) {
 int id=blockIdx.x*blockDim.x+threadIdx.x;
 if(id>=IDS) return;
 float zb=sqrtf(2.f*logf((float)IDS))+2.f;
 for(int k=0;k<count;k++) {
  int sym=p->view.sym[p->view.index(first+k)];
  int lo=p->view.point_a_sc>=0?0:(-p->view.point_a_sc+11)/12;
  int hi=(p->view.nsc-p->view.point_a_sc)/12;
  bool css=p->view.css0_nrb>=6 && p->view.css0_crb>=lo && p->view.css0_crb+p->view.css0_nrb<=hi;
  for(int r=css?0:1;r<2;r++) {
   float z=(p->values[(2*k+r)*IDS+id]-p->median[2*k+r])/p->sigma[2*k+r];
   int j=r*CELLS+sym*IDS+id;
   p->zsum[j]+=z; if(z>zb && p->hits[j]<65535)p->hits[j]++;
  }
 }
}
__global__ void shortlist(Device *p) {
 if(threadIdx.x || blockIdx.x) return;
 for(int r=0;r<2;r++) {
  double total=0;float bv=-1;int bi=0;uint64_t hash=1469598103934665603ULL;
  for(int j=0;j<CELLS;j++) {
   total+=p->hits[r*CELLS+j];
   hash=(hash^p->hits[r*CELLS+j])*1099511628211ULL;
   float v=p->hits[r*CELLS+j]+p->zsum[r*CELLS+j]*1e-4f;
   if(v>bv) {bv=v;bi=j;}
  }
  p->result.hit_hash[r]=hash;
  p->result.top_id[r]=bi%IDS;p->result.top_sym[r]=bi/IDS;
  double lam=total/CELLS;int threshold=1;
  while(threshold<1000) {
   double term=exp(-lam),cdf=0;
   for(int i=0;i<threshold;i++){cdf+=term;term*=lam/(i+1);}
   if(CELLS*fmax(1.0-cdf,0.0)<0.01)break;
   threshold++;
  }
  if(r==0)continue;
  // Same descending hits / ascending cell order as idsw_list.
  int selected[40],n=0;
  for(int j=0;j<CELLS;j++) {
   unsigned h=p->hits[CELLS+j];if(h<(unsigned)threshold)continue;
   int at=0;while(at<n && p->hits[CELLS+selected[at]]>=h)at++;
   if(at>=40)continue;
   if(n<40)n++;
   for(int t=n-1;t>at;t--)selected[t]=selected[t-1];selected[at]=j;
  }
  p->result.list_n=n;
  for(int t=0;t<n;t++){p->result.list_id[t]=selected[t]%IDS;p->result.list_sym[t]=selected[t]/IDS;}
 }
}
__global__ void prepare(Device *p,int list) {
 if(threadIdx.x || blockIdx.x)return;
 p->stage2.alive=0;
 if(list>=p->result.list_n)return;
 int nid=p->result.list_id[list];
 for(int j=0;j<list;j++)if(p->result.list_id[j]==nid)return;
 p->stage2.prepare(nid,p->result.list_sym[list]);
}
__global__ void score(Device *p,int base,int count) {
 int i=base+blockIdx.x*blockDim.x+threadIdx.x;
 if(p->stage2.alive && i<p->stage2.nh && i<base+count)p->stage2.score(i);
}
__global__ void finish(Device *p) {
 if(!threadIdx.x && !blockIdx.x)p->stage2.finish(&p->result);
}
void destroy(void *ptr) {
 Context *c=(Context*)ptr;if(!c)return;
 nr_dl_gpu_sealed=false; // destroy is called only after producer and worker stop
 if(c->stream)cudaStreamSynchronize(c->stream);
 for(int i=0;i<c->nalloc;i++)cudaFree(c->alloc[i]);
 if(c->staging)cudaFreeHost(c->staging);
 if(c->transfer)cudaFreeHost(c->transfer);
 for(auto e:c->event)if(e)cudaEventDestroy(e);
 if(c->stream)cudaStreamDestroy(c->stream);
 delete c;
}
template<typename T> bool allocate(Context *c,T **p,size_t n) {
 if(cudaMalloc(p,n*sizeof(T))!=cudaSuccess)return false;
 c->alloc[c->nalloc]=*p;c->bytes[c->nalloc++]=n*sizeof(T);return true;
}
void *create(int rb,int dur,int window,nr_pdcch_nid_iq_t **staging) {
 if(nr_dl_gpu_sealed)return nullptr; // one pre-RX context per module lifetime
 if(rb<6 || rb>275 || dur<1 || dur>3 || window<1 || window>NR_PDCCH_NID_MAX_WINDOW)return nullptr;
 Context *c=new Context{};c->rb=rb;c->dur=dur;c->window=window;
 c->budget_ms=5000;
 if(const char *value=getenv("ISAC_PDCCH_NID_GPU_BUDGET_MS")) {
  char *end;long n=strtol(value,&end,10);if(*value && !*end && n>=100 && n<=60000)c->budget_ms=(int)n;
 }
 int n=window*NR_PDCCH_NID_BATCH,sc=12*rb;
 Device &p=c->host;
 int *slots=nullptr; c16_t *iq=nullptr;double *scratch=nullptr;st2_snap_t *sn=nullptr;
 int least_priority=0,greatest_priority=0;
 if(cudaDeviceGetStreamPriorityRange(&least_priority,&greatest_priority)!=cudaSuccess
    || cudaStreamCreateWithPriority(&c->stream,cudaStreamNonBlocking,least_priority)!=cudaSuccess)goto fail;
 for(auto &e:c->event)if(cudaEventCreateWithFlags(&e,cudaEventDefault)!=cudaSuccess)goto fail;
 if(cudaHostAlloc(&c->staging,(size_t)3*NR_PDCCH_NID_BATCH*dur*sc*sizeof(c16_t),cudaHostAllocDefault)!=cudaSuccess)goto fail;
 memset(c->staging,0,(size_t)3*NR_PDCCH_NID_BATCH*dur*sc*sizeof(c16_t));
 if(cudaHostAlloc(&c->transfer,sizeof(HostTransfer),cudaHostAllocDefault)!=cudaSuccess)goto fail;
 memset(c->transfer,0,sizeof(HostTransfer));
 if(!allocate(c,&c->dev,1) || !allocate(c,&iq,(size_t)n*dur*sc) || !allocate(c,&slots,2*n)
    || !allocate(c,&p.values,2*CHUNK*IDS) || !allocate(c,&p.zsum,2*CELLS) || !allocate(c,&p.hits,2*CELLS)
    || !allocate(c,&p.stage2.H,NID_HYP_CAP) || !allocate(c,&sn,4*n+1)
    || !allocate(c,&scratch,(size_t)(4*n+1)*6*276+2*n))goto fail;
 // Commit/touch every device workspace before RX, not at the first pass.
 for(int i=0;i<c->nalloc;i++)if(cudaMemsetAsync(c->alloc[i],0,c->bytes[i],c->stream)!=cudaSuccess)goto fail;
 p.view={sc,0,0,0,14,0,dur,0,n,sc,slots,slots+n,iq};
 p.stage2.sn=sn;for(int j=0;j<3;j++)p.stage2.dec[j]=sn+(j+1)*n;
 // Pointer descriptors initialized once on host, then resident on device.
 {
  auto desc=new st2_snap_t[4*n+1]{};
  for(int j=0;j<4*n+1;j++) {
   double *b=scratch+(size_t)j*6*276;
   desc[j]={b,b+276,b+2*276,b+3*276,b+4*276,b+5*276,-1};
  }
  p.stage2.tmp=desc[4*n];
  auto err=cudaMemcpyAsync(sn,desc,(4*n+1)*sizeof(*desc),cudaMemcpyHostToDevice,c->stream);
  if(err==cudaSuccess)err=cudaStreamSynchronize(c->stream);
  delete[] desc;if(err!=cudaSuccess)goto fail;
 }
 p.stage2.vr=scratch+(size_t)(4*n+1)*6*276;p.stage2.vd=p.stage2.vr+n;
 if(cudaMemcpyAsync(c->dev,&p,sizeof(p),cudaMemcpyHostToDevice,c->stream)!=cudaSuccess)goto fail;
 // Eager-load every kernel before RX (CUDA_MODULE_LOADING=LAZY included).
 for(const void *f : { (const void*)noop,(const void*)reset,(const void*)sweep,(const void*)standardize,
                      (const void*)accumulate,(const void*)shortlist,(const void*)prepare,(const void*)score,(const void*)finish }) {
  cudaFuncAttributes a;if(cudaFuncGetAttributes(&a,f)!=cudaSuccess)goto fail;
 }
 // Execute all entry kernels as well as loading their code before sealing.
 cudaMemsetAsync(p.values,0,2*CHUNK*IDS*sizeof(float),c->stream);
 cudaMemsetAsync(p.hits,0,2*CELLS*sizeof(uint16_t),c->stream);
 cudaMemsetAsync(p.zsum,0,2*CELLS*sizeof(float),c->stream);
 reset<<<1,1,0,c->stream>>>(c->dev,p.view);
 sweep<<<dim3(IDS/128,CHUNK),128,0,c->stream>>>(c->dev,0,0);
 standardize<<<2*CHUNK,256,0,c->stream>>>(c->dev);
 accumulate<<<IDS/256,256,0,c->stream>>>(c->dev,0,0);
 shortlist<<<1,1,0,c->stream>>>(c->dev);
 prepare<<<1,1,0,c->stream>>>(c->dev,0);
 score<<<8,128,0,c->stream>>>(c->dev,0,0);
 finish<<<1,1,0,c->stream>>>(c->dev);
 noop<<<1,1,0,c->stream>>>();
 for(auto event:c->event)if(cudaEventRecord(event,c->stream)!=cudaSuccess)goto fail;
 if(cudaMemcpyAsync(iq,c->staging,(size_t)NR_PDCCH_NID_BATCH*dur*sc*sizeof(c16_t),
                    cudaMemcpyHostToDevice,c->stream)!=cudaSuccess)goto fail;
 if(cudaMemcpyAsync(&c->transfer->result,&c->dev->result,sizeof(c->transfer->result),
                    cudaMemcpyDeviceToHost,c->stream)!=cudaSuccess)goto fail;
 if(cudaStreamSynchronize(c->stream)!=cudaSuccess)goto fail;
 nr_dl_gpu_sealed=true;
 *staging=c->staging;return c;
fail: destroy(c);return nullptr;
}
int upload(void *ptr,const nr_pdcch_nid_batch_t *b) {
 Context *c=(Context*)ptr;
 if(b->cfg.nsc<72 || b->cfg.nsc>12*c->rb || b->cfg.dur!=c->dur
    || (b->cfg.nsc-b->cfg.point_a_sc)/12>275
    || (b->cfg.nsc-b->cfg.point_a_sc)/12-(b->cfg.point_a_sc<0?(-b->cfg.point_a_sc+11)/12:0)<6)return -1;
 if(memcmp(&b->cfg,&c->cfg,sizeof(c->cfg))) {c->batches=0;c->cfg=b->cfg;}
 int first=(c->batches%c->window)*NR_PDCCH_NID_BATCH;
 auto stream=c->stream;NidView &v=c->host.view;
 // Only these small metadata arrays need a host copy; CUDA never stages
 // pageable pointers in the processing path (including result/control D2H).
 memcpy(c->transfer->slot,b->slot,sizeof(b->slot));
 memcpy(c->transfer->sym,b->sym,sizeof(b->sym));
 // Staging has max-RB stride; no repack or host-side pilot extraction.
 if(cudaMemcpyAsync((void*)(v.re+(size_t)first*c->dur*v.stride),b->iq,
                     (size_t)NR_PDCCH_NID_BATCH*c->dur*v.stride*sizeof(c16_t),cudaMemcpyHostToDevice,stream)!=cudaSuccess
    || cudaMemcpyAsync((void*)(v.slot+first),c->transfer->slot,sizeof(b->slot),cudaMemcpyHostToDevice,stream)!=cudaSuccess
    || cudaMemcpyAsync((void*)(v.sym+first),c->transfer->sym,sizeof(b->sym),cudaMemcpyHostToDevice,stream)!=cudaSuccess)return -1;
 c->batches++;
 v.nsc=b->cfg.nsc;v.point_a_sc=b->cfg.point_a_sc;v.css0_crb=b->cfg.css0_crb;v.css0_nrb=b->cfg.css0_nrb;v.sps=b->cfg.sps;
 v.n=(c->batches<c->window?c->batches:c->window)*NR_PDCCH_NID_BATCH;
 v.begin=c->batches<c->window?0:(c->batches%c->window)*NR_PDCCH_NID_BATCH;
 return cudaStreamSynchronize(stream)==cudaSuccess?0:-1;
}
int run_impl(void *ptr,nr_pdcch_nid_result_t *out,const nr_pdcch_nid_result_t *seed) {
 Context *c=(Context*)ptr;auto stream=c->stream;Device *p=c->dev;
 if(c->batches<c->window)return 1;
 const auto start=std::chrono::steady_clock::now();
 auto expired=[&]() {return std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now()-start).count()>=c->budget_ms;};
 if(cudaMemsetAsync(c->host.hits,0,2*CELLS*sizeof(uint16_t),stream)!=cudaSuccess
    || cudaMemsetAsync(c->host.zsum,0,2*CELLS*sizeof(float),stream)!=cudaSuccess)return -1;
 reset<<<1,1,0,stream>>>(p,c->host.view);cudaEventRecord(c->event[0],stream);
 if(!seed) {
 for(int first=0;first<c->host.view.n;first+=CHUNK) {
  int n=c->host.view.n-first;if(n>CHUNK)n=CHUNK;
  sweep<<<dim3(IDS/128,n),128,0,stream>>>(p,first,n);
  standardize<<<2*n,256,0,stream>>>(p);
  accumulate<<<IDS/256,256,0,stream>>>(p,first,n);
  // Yield between bounded chunks, never monopolize the device with an entire window.
  if(cudaStreamSynchronize(stream)!=cudaSuccess)return -1;
  if(expired())return 2;
 }
 shortlist<<<1,1,0,stream>>>(p);
 } else {
  // Test-only stage-2 entry: seed ONLY the shortlist, never expected records.
  c->transfer->result={};c->transfer->result.list_n=seed->list_n;
  memcpy(c->transfer->result.list_id,seed->list_id,sizeof(seed->list_id));
  memcpy(c->transfer->result.list_sym,seed->list_sym,sizeof(seed->list_sym));
  if(cudaMemcpyAsync(&p->result,&c->transfer->result,sizeof(*out),cudaMemcpyHostToDevice,stream)!=cudaSuccess)return -1;
 }
 cudaEventRecord(c->event[1],stream);
 if(cudaMemcpyAsync(&c->transfer->result,&p->result,sizeof(*out),cudaMemcpyDeviceToHost,stream)!=cudaSuccess
    || cudaStreamSynchronize(stream)!=cudaSuccess)return -1;
 *out=c->transfer->result;
 for(int j=0;j<out->list_n;j++) {
  prepare<<<1,1,0,stream>>>(p,j);
  for(int it=0;it<6;it++) {
   // Read control only; hypothesis/snapshot arrays stay on device.
   int *control=c->transfer->control;
   if(cudaMemcpyAsync(control,&p->stage2.nh,2*sizeof(int),cudaMemcpyDeviceToHost,stream)!=cudaSuccess
      || cudaStreamSynchronize(stream)!=cudaSuccess)return -1;
   if(expired())return 2;
   if(!control[1])break;
   for(int base=0;base<control[0];base+=1024) {
    score<<<8,128,0,stream>>>(p,base,1024);
    if(cudaStreamSynchronize(stream)!=cudaSuccess)return -1;
    if(expired())return 2;
   }
   finish<<<1,1,0,stream>>>(p);
  }
 }
 cudaEventRecord(c->event[2],stream);
 if(cudaMemcpyAsync(&c->transfer->result,&p->result,sizeof(*out),cudaMemcpyDeviceToHost,stream)!=cudaSuccess
    || cudaStreamSynchronize(stream)!=cudaSuccess)return -1;
 *out=c->transfer->result;
 cudaEventElapsedTime(&out->stage1_ms,c->event[0],c->event[1]);
 cudaEventElapsedTime(&out->stage2_ms,c->event[1],c->event[2]);
 return 0;
}
int run(void *ptr,nr_pdcch_nid_result_t *out) {return run_impl(ptr,out,nullptr);}
const nr_pdcch_nid_ops_t ops={NR_PDCCH_NID_ABI,create,upload,run,destroy};
}
extern "C" const nr_pdcch_nid_ops_t *nr_pdcch_nid_gpu_ops(void) {return &ops;}

// Synthetic parity harness only; the receiver's ABI always calls the full pipeline.
extern "C" int nr_pdcch_nid_gpu_stage2_check(void *ctx,const nr_pdcch_nid_result_t *seed,nr_pdcch_nid_result_t *out) {
 if(!seed || seed->list_n<1 || seed->list_n>40)return -1;
 for(int i=0;i<seed->list_n;i++)if(seed->list_id[i]<0 || seed->list_id[i]>65535 || seed->list_sym[i]<0 || seed->list_sym[i]>13)return -1;
 return run_impl(ctx,out,seed);
}
