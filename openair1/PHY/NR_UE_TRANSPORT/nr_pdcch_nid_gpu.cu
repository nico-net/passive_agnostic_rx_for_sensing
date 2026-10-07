/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include <cuda_runtime.h>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <chrono>
#include <thread>
#include "nr_dl_gpu_memory.h"
#define NID_FN __device__
#include "nr_pdcch_nid_stage1.h"

namespace {
constexpr int IDS=65536, CELLS=14*IDS, CHUNK=4, TIMERS=32768;
enum Kernel {RESET, SWEEP, STANDARDIZE, ACCUMULATE, PREPARE_INIT, PROBE, CONTROL, SNAPSHOTS, HULL, GENERATE, SCORE, FINISH, SUMMARY};
struct Summary {unsigned total;float peak;int cell;};
struct Device {
 NidStage2 stage2;
 NidView view;
 float *values, *zsum;
 uint16_t *hits;
 int *lit,*active;
 Summary *summary;
 float median[2*CHUNK], sigma[2*CHUNK];
 nr_pdcch_nid_result_t result;
};
struct HostTransfer {
 int slot[NR_PDCCH_NID_BATCH],sym[NR_PDCCH_NID_BATCH],control[2];
 nr_pdcch_nid_result_t result;
 uint16_t hits[2*CELLS];
 Summary summary[2*CELLS/256];
};
struct Context {
 Device host, *dev;
 cudaStream_t stream;
 cudaEvent_t event[3], done, timer[TIMERS][2];
 int kind[TIMERS], ntimer;
 nr_pdcch_nid_result_t timing;
 nr_pdcch_nid_iq_t *staging;
 HostTransfer *transfer;
 nr_pdcch_nid_config_t cfg;
 int rb, dur, window, batches, budget_ms;
 void *alloc[16]; size_t bytes[16]; int nalloc;
};
__global__ void reset(Device *p, NidView v) {
 if(threadIdx.x || blockIdx.x) return;
 p->view=v; p->stage2.g_idsw=v;
 p->result={};
}
__global__ void sweep(Device *p,int first,int count) {
 int id=blockIdx.x*blockDim.x+threadIdx.x,k=blockIdx.y;
 if(id>=IDS || k>=count) return;
 const NidView &v=p->view; int idx=v.index(first+k);
 uint64_t x=((uint64_t)v.sps*v.slot[idx]+v.sym[idx]+1)*((uint64_t)(id*2)+1);
 uint32_t seed=(uint32_t)(((x<<17)+id*2)&0x7fffffff);
 int hi=(v.nsc-v.point_a_sc)/12,lo=v.point_a_sc>=0?0:(-v.point_a_sc+11)/12;
 bool css=v.css0_nrb>=6 && v.css0_crb>=lo && v.css0_crb+v.css0_nrb<=hi;
 p->values[(2*k+1)*IDS+id]=nid_best_symbol(v,first+k,seed,lo,hi,css?v.css0_crb:0,css?v.css0_crb+v.css0_nrb:0,0);
 p->values[(2*k)*IDS+id]=css?nid_best_symbol(v,first+k,seed,v.css0_crb,v.css0_crb+v.css0_nrb,0,0,v.css0_crb):0;
}
__device__ void histogram_add(unsigned *h,int bin) {
 unsigned peers=__match_any_sync(__activemask(),bin);
 if((threadIdx.x&31)==__ffs(peers)-1)atomicAdd(h+bin,__popc(peers));
}
__global__ void standardize(Device *p) {
 __shared__ unsigned h[4096];
 int k=blockIdx.x,tid=threadIdx.x;
 for(int j=tid;j<4096;j+=blockDim.x)h[j]=0;
 __syncthreads();
 const float *v=p->values+k*IDS;
 for(int i=tid;i<IDS;i+=blockDim.x) {
  int b=(int)(v[i]*4095);histogram_add(h,b<0?0:b>=4096?4095:b);
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
  int b=(int)(fabsf(v[i]-p->median[k])*4095);histogram_add(h,b>=4096?4095:b);
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
// Integer totals and stable (value, lowest cell) argmax per 256 cells.
__global__ void summarize(Device *p) {
 __shared__ Summary v[256];
 int tid=threadIdx.x,j=blockIdx.x*256+tid;
 float value=p->hits[j]+p->zsum[j]*1e-4f;
 v[tid]={p->hits[j],value>-1?value:-1,value>-1?j%CELLS:0};
 __syncthreads();
 for(int step=128;step;step/=2) {
  if(tid<step) {
   auto &a=v[tid];const auto &b=v[tid+step];a.total+=b.total;
   if(b.peak>a.peak || (b.peak==a.peak && b.cell<a.cell)){a.peak=b.peak;a.cell=b.cell;}
  }
  __syncthreads();
 }
 if(tid==0)p->summary[blockIdx.x]=v[0];
}
void shortlist(HostTransfer *p) {
 for(int r=0;r<2;r++) {
  double total=0;float bv=-1;int bi=0;uint64_t hash=1469598103934665603ULL;
  for(int j=0;j<CELLS/256;j++) {
   const auto &v=p->summary[r*(CELLS/256)+j];total+=v.total;
   if(v.peak>bv || (v.peak==bv && v.cell<bi)){bv=v.peak;bi=v.cell;}
  }
  for(int j=0;j<CELLS;j++)hash=(hash^p->hits[r*CELLS+j])*1099511628211ULL;
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
// Parallel work is separate from NidStage2's scalar, reference-order arithmetic.
// Control lanes only scan bounded metadata; each snapshot keeps its own sums.
__global__ void prepare_init(Device *p,int list) {
 auto &s=p->stage2;
 if(threadIdx.x==0) {
  s.alive=0;s.nact=0;s.nh=0;
  if(list>=p->result.list_n)return;
  int nid=p->result.list_id[list];
  for(int j=0;j<list;j++)if(p->result.list_id[j]==nid)return;
  s.nid=nid;s.sym0=p->result.list_sym[list];
  int pa=s.g_idsw.point_a_sc;
  s.g_st2_clo=pa>=0?0:(-pa+11)/12;s.g_st2_chi=(s.g_idsw.nsc-pa)/12;
  s.g_st2_D=1;s.alive=1;
 }
}
__global__ void prepare_probe(Device *p,bool active) {
 auto &s=p->stage2;int k=blockIdx.x*blockDim.x+threadIdx.x;
 if(k>=s.g_idsw.n)return;
 p->lit[k]=0;
 if(!s.alive)return;
 int sym=s.g_idsw.sym[s.g_idsw.index(k)];
 if(active?sym!=s.sym0:(sym<s.sym0 || sym>s.sym0+2))return;
 s.st2_build(k,s.nid,s.sn+k);p->lit[k]=s.st2_lit(s.sn+k);
}
__global__ void prepare_control(Device *p,bool active) {
 auto &s=p->stage2;
 if(threadIdx.x || !s.alive)return;
 if(active) {
  for(int k=0;k<s.g_idsw.n;k++)if(p->lit[k])p->active[s.nact++]=k;
  s.alive=s.nact>0;
 } else {
  int nsym[14]={},nlit[14]={};
  for(int k=0;k<s.g_idsw.n;k++) {
   int sym=s.g_idsw.sym[s.g_idsw.index(k)];nsym[sym]++;nlit[sym]+=p->lit[k];
  }
  double r0=nsym[s.sym0]?(double)nlit[s.sym0]/nsym[s.sym0]:0;
  int d=1;
  while(d<3 && s.sym0+d<14 && nsym[s.sym0+d] && (double)nlit[s.sym0+d]/nsym[s.sym0+d]>=0.5*r0)d++;
  s.g_st2_D=d;s.alive=d<=s.g_idsw.dur;
 }
}
__global__ void prepare_snapshots(Device *p) {
 auto &s=p->stage2;int k=blockIdx.x*blockDim.x+threadIdx.x,d=blockIdx.y;
 if(!s.alive || k>=s.nact)return;
 int ids[4]={s.nid,(s.nid+7919)&65535,(s.nid+30011)&65535,(s.nid^0x5a5a)&65535};
 s.st2_build(p->active[k],ids[d],d?s.dec[d-1]+k:s.sn+k);
}
__global__ void hull(Device *p) {
 auto &s=p->stage2;int c=blockIdx.x*blockDim.x+threadIdx.x;
 if(c>=276)return;
 unsigned n=0;
 if(s.alive && c>=s.g_st2_clo && c+6<=s.g_st2_chi) {
  double t=1.0-pow(1e-4,1.0/(18.0*s.g_st2_D-1.0));
  for(int k=0;k<s.nact;k++) {
   const auto &a=s.sn[k];
   double r=a.re[c+6]-a.re[c],i=a.im[c+6]-a.im[c],v=a.p[c+6]-a.p[c];
   n+=v>0 && (r*r+i*i)/(v*36.0*s.g_st2_D)>t;
  }
 }
 s.lit_hist[c]=n;
}
__global__ void generate(Device *p) {
 auto &s=p->stage2;
 __shared__ int qlo,qhi,counts[6],offset[6];
 if(threadIdx.x==0) {
  unsigned n=0;for(int c=0;c<s.g_st2_chi;c++)n+=s.lit_hist[c];
  s.alive=s.alive && n>0;s.nh=0;qlo=qhi=0;
  unsigned lo=(unsigned)(0.02*(n?n-1:0)),hi=(unsigned)(0.98*(n?n-1:0)),acc=0;
  for(int c=0;c<s.g_st2_chi;c++) {
   if(acc<=lo && acc+s.lit_hist[c]>lo)qlo=c;
   if(acc<=hi && acc+s.lit_hist[c]>hi)qhi=c+5;
   acc+=s.lit_hist[c];
  }
 }
 __syncthreads();
 if(!s.alive)return;
 int ph=threadIdx.x;
 if(ph<6)counts[ph]=s.st2_gen(nullptr,NID_HYP_CAP,qlo,qhi,ph,ph+1);
 __syncthreads();
 if(ph==0)for(int j=0;j<6;j++) {offset[j]=s.nh;s.nh+=min(counts[j],NID_HYP_CAP-s.nh);}
 __syncthreads();
 if(ph<6)s.st2_gen(s.H+offset[ph],min(counts[ph],NID_HYP_CAP-offset[ph]),qlo,qhi,ph,ph+1);
}
// One block per hypothesis, lanes score independent snapshots. The control
// lane adds results in original snapshot order (no floating-point tree sum).
__global__ void score(Device *p,int base,int count) {
 auto &s=p->stage2;int i=base+blockIdx.x,tid=threadIdx.x;
 if(!s.alive || i>=s.nh || i>=base+count)return;
 __shared__ double value[128];
 __shared__ int vote[128],al[128],nf,below;
 if(tid==0){s.H[i].score=0;memset(s.H[i].al_win,0,sizeof(s.H[i].al_win));nf=below=0;}
 __syncthreads();
 for(int first=0;first<s.nact;first+=128) {
  int k=first+tid;
  if(k<s.nact) {
   st2_hyp_t h=s.H[i];s.st2_score(&h,s.sn+k,1);value[tid]=h.score;
   al[tid]=-1;for(int j=1;j<5;j++)if(h.al_win[j])al[tid]=j;
   vote[tid]=s.consistency_vote(&h,k);
  }
  __syncthreads();
  if(tid==0)for(int j=0;j<128 && first+j<s.nact;j++) {
   s.H[i].score+=value[j];if(al[j]>=0)s.H[i].al_win[al[j]]++;
   nf+=vote[j]>=0;below+=vote[j]==0;
  }
  __syncthreads();
 }
 if(tid==0 && (!nf || below>nf/2))s.H[i].score=0;
}
__global__ void finish(Device *p) {
 auto &s=p->stage2;int tid=threadIdx.x;
 if(!s.alive)return;
 __shared__ int candidates[256],removed[256];
 __shared__ st2_hyp_t win;
 int best=0;
 for(int i=tid;i<s.nh;i+=blockDim.x)
  if(s.H[i].score>s.H[best].score || (s.H[i].score==s.H[best].score && i<best))best=i;
 candidates[tid]=best;__syncthreads();
 for(int step=128;step;step/=2) {
  if(tid<step) {
   int a=candidates[tid],b=candidates[tid+step];
   if(s.H[b].score>s.H[a].score || (s.H[b].score==s.H[a].score && b<a))candidates[tid]=b;
  }
  __syncthreads();
 }
 if(tid==0) {win=s.H[candidates[0]];if(win.score<=0)s.alive=0;}
 __syncthreads();if(!s.alive)return;
 for(int k=tid;k<s.nact;k+=blockDim.x)s.st2_per_snap(&win,s.sn+k,1,s.vr+k);
 __syncthreads();
 for(int d=0;d<3;d++) {
  for(int k=tid;k<s.nact;k+=blockDim.x)s.st2_per_snap(&win,s.dec[d]+k,1,s.vd+k);
  __syncthreads();
  // Keep paired-t summation in snapshot order, exactly as the CPU reference.
  if(tid==0 && s.st2_paired_t(s.vr,s.vd,s.nact)<5.0)s.alive=0;
  __syncthreads();if(!s.alive)return;
 }
 if(tid==0) {
  int ties=0,chosen[64];st2_hyp_t handoff=win;
  for(int i=0;i<s.nh;i++) {
   const auto &h=s.H[i];
   if(h.L!=win.L || h.R!=win.R || h.phase!=win.phase || fabs(h.score-win.score)>1e-9*(1+fabs(win.score)))continue;
   int at=0;while(at<ties && s.H[chosen[at]].score>=h.score)at++;
   if(at>=64)continue;
   if(ties<64)ties++;
   for(int j=ties-1;j>at;j--)chosen[j]=chosen[j-1];chosen[at]=i;
  }
  for(int t=0;t<ties;t++) {
   const auto &h=s.H[chosen[t]];
   if(!win.L && h.phase+6*h.g0<handoff.phase+6*handoff.g0) {
    handoff=h;handoff.ng=(s.g_st2_chi-(handoff.phase+6*handoff.g0))/6;
   }
  }
  auto &out=p->result;
  if(out.count<NR_PDCCH_NID_MAX_RECORDS && s.g_idsw.point_a_sc%12==0) {
   memcpy(out.al_win[out.count],win.al_win,sizeof(win.al_win));
   out.records[out.count++]={s.nid,s.sym0,s.g_st2_D,handoff.phase+6*handoff.g0+s.g_idsw.point_a_sc/12,
                            handoff.ng,handoff.L,handoff.R,handoff.shift,ties};
  }
 }
 // Removal mutates one snapshot only, so snapshots are independent.
 int n=0;for(int k=tid;k<s.nact;k+=blockDim.x)n+=s.st2_remove(s.sn+k,&win);
 removed[tid]=n;__syncthreads();
 for(int step=128;step;step/=2) {if(tid<step)removed[tid]+=removed[tid+step];__syncthreads();}
 if(tid==0)s.alive=removed[0]>0;
}
void destroy(void *ptr) {
 Context *c=(Context*)ptr;if(!c)return;
 nr_dl_gpu_sealed=false; // destroy is called only after producer and worker stop
 if(c->stream)cudaStreamSynchronize(c->stream);
 for(int i=0;i<c->nalloc;i++)cudaFree(c->alloc[i]);
 if(c->staging)cudaFreeHost(c->staging);
 if(c->transfer)cudaFreeHost(c->transfer);
 for(auto e:c->event)if(e)cudaEventDestroy(e);
 if(c->done)cudaEventDestroy(c->done);
 for(auto &pair:c->timer)for(auto e:pair)if(e)cudaEventDestroy(e);
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
 if(cudaEventCreateWithFlags(&c->done,cudaEventDisableTiming)!=cudaSuccess)goto fail;
 for(auto &pair:c->timer)for(auto &e:pair)if(cudaEventCreateWithFlags(&e,cudaEventDefault)!=cudaSuccess)goto fail;
 if(cudaHostAlloc(&c->staging,(size_t)3*NR_PDCCH_NID_BATCH*dur*sc*sizeof(c16_t),cudaHostAllocDefault)!=cudaSuccess)goto fail;
 memset(c->staging,0,(size_t)3*NR_PDCCH_NID_BATCH*dur*sc*sizeof(c16_t));
 if(cudaHostAlloc(&c->transfer,sizeof(HostTransfer),cudaHostAllocDefault)!=cudaSuccess)goto fail;
 memset(c->transfer,0,sizeof(HostTransfer));
 if(!allocate(c,&c->dev,1) || !allocate(c,&iq,(size_t)n*dur*sc) || !allocate(c,&slots,2*n)
    || !allocate(c,&p.values,2*CHUNK*IDS) || !allocate(c,&p.zsum,2*CELLS) || !allocate(c,&p.hits,2*CELLS)
    || !allocate(c,&p.summary,2*CELLS/256) || !allocate(c,&p.lit,n) || !allocate(c,&p.active,n)
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
 for(const void *f : { (const void*)reset,(const void*)sweep,(const void*)standardize,
                      (const void*)accumulate,(const void*)prepare_init,(const void*)prepare_probe,
                      (const void*)prepare_control,(const void*)prepare_snapshots,(const void*)hull,
                      (const void*)generate,(const void*)score,(const void*)finish,(const void*)summarize }) {
  cudaFuncAttributes a;if(cudaFuncGetAttributes(&a,f)!=cudaSuccess)goto fail;
 }
 reset<<<1,256,0,c->stream>>>(c->dev,p.view);
 sweep<<<dim3(IDS/128,1),128,0,c->stream>>>(c->dev,0,0);
 standardize<<<2*CHUNK,256,0,c->stream>>>(c->dev);
 accumulate<<<IDS/256,256,0,c->stream>>>(c->dev,0,0);
 prepare_init<<<1,256,0,c->stream>>>(c->dev,0);
 prepare_probe<<<1,128,0,c->stream>>>(c->dev,false);
 prepare_control<<<1,256,0,c->stream>>>(c->dev,false);
 prepare_snapshots<<<dim3(1,4),128,0,c->stream>>>(c->dev);
 hull<<<3,128,0,c->stream>>>(c->dev);
 generate<<<1,32,0,c->stream>>>(c->dev);
 score<<<1,128,0,c->stream>>>(c->dev,0,0);
 finish<<<1,256,0,c->stream>>>(c->dev);
 summarize<<<2*CELLS/256,256,0,c->stream>>>(c->dev);
 // Touch timing-event backing before the receiver seals device allocations.
 for(auto &pair:c->timer)for(auto event:pair)if(cudaEventRecord(event,c->stream)!=cudaSuccess)goto fail;
 for(auto event:c->event)if(cudaEventRecord(event,c->stream)!=cudaSuccess)goto fail;
 if(cudaEventRecord(c->done,c->stream)!=cudaSuccess)goto fail;
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
// Events are allocated/touched before RX. No CUDA allocation or chunk wait here.
bool timing_begin(Context *c,int kind) {
 if(c->ntimer==TIMERS)return false;
 c->kind[c->ntimer]=kind;
 return cudaEventRecord(c->timer[c->ntimer][0],c->stream)==cudaSuccess;
}
bool timing_end(Context *c) {
 return cudaGetLastError()==cudaSuccess && cudaEventRecord(c->timer[c->ntimer++][1],c->stream)==cudaSuccess;
}
#define LAUNCH(kind, ...) do {if(!timing_begin(c,kind))return -1; __VA_ARGS__; if(!timing_end(c))return -1;} while(0)
bool complete(Context *c,const std::chrono::steady_clock::time_point &start) {
 if(cudaEventRecord(c->done,c->stream)!=cudaSuccess)return false;
 for(;;) {
  cudaError_t e=cudaEventQuery(c->done);
  if(e==cudaSuccess)break;
  if(e!=cudaErrorNotReady)return false;
  // Coarse in-pass observation only. Drain already queued work and publish a
  // complete pass even when shared-device contention exceeds the soft budget.
  if(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count()>=c->budget_ms)
   c->timing.budget_exceeded=1;
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
 }
 for(int i=0;i<c->ntimer;i++) {
  float ms=0;if(cudaEventElapsedTime(&ms,c->timer[i][0],c->timer[i][1])!=cudaSuccess)return false;
  int k=c->kind[i];c->timing.kernel_calls[k]++;c->timing.kernel_total_ms[k]+=ms;
  c->timing.kernel_peak_ms[k]=fmaxf(c->timing.kernel_peak_ms[k],ms);
  c->timing.kernel_max_ms=fmaxf(c->timing.kernel_max_ms,ms);
 }
 c->ntimer=0;return true;
}
int run_impl(void *ptr,nr_pdcch_nid_result_t *out,const nr_pdcch_nid_result_t *seed) {
 Context *c=(Context*)ptr;auto stream=c->stream;Device *p=c->dev;
 if(c->batches<c->window)return 1;
 const auto start=std::chrono::steady_clock::now();c->timing={};c->ntimer=0;
 if(cudaEventRecord(c->event[0],stream)!=cudaSuccess
    || cudaMemsetAsync(c->host.hits,0,2*CELLS*sizeof(uint16_t),stream)!=cudaSuccess
    || cudaMemsetAsync(c->host.zsum,0,2*CELLS*sizeof(float),stream)!=cudaSuccess)return -1;
 LAUNCH(RESET,reset<<<1,256,0,stream>>>(p,c->host.view));
 if(!seed) {
  const int chunk=CHUNK/c->dur;
  for(int first=0;first<c->host.view.n;first+=chunk) {
   int n=c->host.view.n-first;if(n>chunk)n=chunk;
   LAUNCH(SWEEP,sweep<<<dim3(IDS/128,n),128,0,stream>>>(p,first,n));
   LAUNCH(STANDARDIZE,standardize<<<2*n,256,0,stream>>>(p));
   LAUNCH(ACCUMULATE,accumulate<<<IDS/256,256,0,stream>>>(p,first,n));
  }
  // The FNV diagnostic is order dependent; keep it and exact stable selection
  // on the worker CPU. Bounded 3.6 MiB D2H, independent of window/duration.
  LAUNCH(SUMMARY,summarize<<<2*CELLS/256,256,0,stream>>>(p));
  if(cudaMemcpyAsync(c->transfer->hits,c->host.hits,sizeof(c->transfer->hits),cudaMemcpyDeviceToHost,stream)!=cudaSuccess
     || cudaMemcpyAsync(c->transfer->summary,c->host.summary,sizeof(c->transfer->summary),cudaMemcpyDeviceToHost,stream)!=cudaSuccess
     || !complete(c,start))return -1;
  c->transfer->result={};shortlist(c->transfer);
 } else {
  c->transfer->result={};c->transfer->result.list_n=seed->list_n;
  memcpy(c->transfer->result.list_id,seed->list_id,sizeof(seed->list_id));
  memcpy(c->transfer->result.list_sym,seed->list_sym,sizeof(seed->list_sym));
 }
 int lists=c->transfer->result.list_n;
 if(cudaMemcpyAsync(&p->result,&c->transfer->result,sizeof(*out),cudaMemcpyHostToDevice,stream)!=cudaSuccess
    || cudaEventRecord(c->event[1],stream)!=cudaSuccess)return -1;
 for(int j=0;j<lists;j++) {
  const int blocks=(c->host.view.n+127)/128;
  LAUNCH(PREPARE_INIT,prepare_init<<<1,256,0,stream>>>(p,j));
  LAUNCH(PROBE,prepare_probe<<<blocks,128,0,stream>>>(p,false));
  LAUNCH(CONTROL,prepare_control<<<1,256,0,stream>>>(p,false));
  LAUNCH(PROBE,prepare_probe<<<blocks,128,0,stream>>>(p,true));
  LAUNCH(CONTROL,prepare_control<<<1,256,0,stream>>>(p,true));
  LAUNCH(SNAPSHOTS,prepare_snapshots<<<dim3(blocks,4),128,0,stream>>>(p));
  LAUNCH(HULL,hull<<<3,128,0,stream>>>(p));
  LAUNCH(GENERATE,generate<<<1,32,0,stream>>>(p));
  for(int it=0;it<6;it++) {
   int *control=c->transfer->control;
   if(cudaMemcpyAsync(control,&p->stage2.nh,2*sizeof(int),cudaMemcpyDeviceToHost,stream)!=cudaSuccess
      || !complete(c,start))return -1;
   if(!control[1])break;
   for(int base=0;base<control[0];base+=128)
    LAUNCH(SCORE,score<<<128,128,0,stream>>>(p,base,128));
   LAUNCH(FINISH,finish<<<1,256,0,stream>>>(p));
   LAUNCH(HULL,hull<<<3,128,0,stream>>>(p));
   LAUNCH(GENERATE,generate<<<1,32,0,stream>>>(p));
  }
 }
 if(cudaEventRecord(c->event[2],stream)!=cudaSuccess
    || cudaMemcpyAsync(&c->transfer->result,&p->result,sizeof(*out),cudaMemcpyDeviceToHost,stream)!=cudaSuccess
    || !complete(c,start))return -1;
 *out=c->transfer->result;
 if(cudaEventElapsedTime(&out->stage1_ms,c->event[0],c->event[1])!=cudaSuccess
    || cudaEventElapsedTime(&out->stage2_ms,c->event[1],c->event[2])!=cudaSuccess)return -1;
 out->kernel_max_ms=c->timing.kernel_max_ms;
 out->budget_exceeded=c->timing.budget_exceeded;
 memcpy(out->kernel_calls,c->timing.kernel_calls,sizeof(out->kernel_calls));
 memcpy(out->kernel_total_ms,c->timing.kernel_total_ms,sizeof(out->kernel_total_ms));
 memcpy(out->kernel_peak_ms,c->timing.kernel_peak_ms,sizeof(out->kernel_peak_ms));
 return 0;
}
#undef LAUNCH
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
