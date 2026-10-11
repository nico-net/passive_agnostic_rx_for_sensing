/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include <cuda_runtime.h>
#include "PHY/TOOLS/nr_gpu_budget_cuda.h"
#include <cstdio>
#include <cstddef>
#include <cstdlib>
#include <initializer_list>
#include <chrono>
#include <thread>
#include <algorithm>
#include "nr_dl_gpu_memory.h"
#include "nr_gpu_green.h"
#define NID_FN __device__
#include "nr_pdcch_nid_stage1.h"
#include "nr_pdcch_nid_chunks.h"

namespace {
constexpr int IDS=65536, CELLS=14*IDS, CHUNK=4, TIMERS=32768;
enum Kernel {RESET, SWEEP, STANDARDIZE, ACCUMULATE, PREPARE_INIT, PROBE, CONTROL, SNAPSHOTS, HULL, GENERATE, SCORE, FINISH, SUMMARY};
constexpr int STAT=6*CELLS, CAND_CAP=4096;
static_assert(offsetof(NidStage2,alive)-offsetof(NidStage2,nact)==4*sizeof(int), "stage-2 control transfer layout");
struct Candidate {int cell;float score;};
struct Compact {unsigned count;Candidate item[CAND_CAP];};
struct Summary {unsigned total;float peak;int cell;};
struct Device {
 NidStage2 stage2;
 int score_nf[NID_HYP_CAP], score_below[NID_HYP_CAP];
 NidView view;
 bool multisymbol;
 uint16_t *history_hits, *total_hits;
 float *history_z;
 double *total_z;
 Compact compact;
 int threshold[3][14];
 double lambda[3][14];
 float *values, *zsum, *multi;
 int *candidates, selected_count[CHUNK];
 uint16_t *hits;
 int *lit,*active;
 int explicit_duration;
 Summary *summary;
 float median[4*CHUNK], sigma[4*CHUNK];
 nr_pdcch_nid_result_t result;
};
struct HostTransfer {
 int slot[NR_PDCCH_NID_BATCH],sym[NR_PDCCH_NID_BATCH],control[5];
 nr_pdcch_nid_result_t result;
 Compact compact;
 double lambda[3][14];
 int threshold[3][14];
 uint16_t hits[6*CELLS];
 float zsum[6*CELLS];
 Summary summary[2*CELLS/256];
};
struct Context {
 Device host, *dev;
 cudaStream_t stream;
 NrGpuDiscoverySlice slice;
 cudaEvent_t event[3], done, timer[TIMERS][2];
 int kind[TIMERS], ntimer;
 const char *operation[TIMERS];
 nr_pdcch_nid_result_t timing;
 nr_pdcch_nid_iq_t *staging;
 HostTransfer *transfer;
 nr_pdcch_nid_config_t cfg;
 int rb, dur, window, batches, budget_ms;
 int batch_size, pending, first_new;
 bool incremental;
 uint64_t look;
 nr_pdcch_nid_result_t bank;
 uint64_t last_seen[NR_PDCCH_NID_MAX_RECORDS];
 void *alloc[28]; size_t bytes[28]; int nalloc;
};
__global__ void reset(Device *p, NidView v) {
 if(threadIdx.x || blockIdx.x) return;
 p->view=v; p->stage2.g_idsw=v;
 p->result={};
}
// Tile two snapshots across 128 IDs. Cooperative pilot loads are contiguous
// in DM-RS order; every warp then broadcasts shared REs. Power prefixes and
// square roots are no longer redundantly calculated for each ID/window.
__global__ void sweep(Device *p,int first,int count,int id_base=0) {
 __shared__ NidSigns signs[2][3*275];
 __shared__ float denominator[2][2][275];
 __shared__ uint32_t x1words[54];
 int lane=threadIdx.x, tile=threadIdx.y,k=2*blockIdx.y+tile;
 const NidView &v=p->view;
 int hi=(v.nsc-v.point_a_sc)/12,lo=v.point_a_sc>=0?0:(-v.point_a_sc+11)/12;
 bool css=v.css0_nrb>=6 && v.css0_crb>=lo && v.css0_crb+v.css0_nrb<=hi;
 if(!lane && !tile) {
  uint32_t x1=0x5e485840u;
  for(int w=0;w<54;w++) {x1words[w]=x1;x1=(x1>>1)^(x1>>4);x1^=(x1<<31)^(x1<<28);}
 }
 if(k<count) {
  const c16_t *re=v.grid(first+k);
  for(int m=3*lo+lane;m<3*hi;m+=128) {
   signs[tile][m]=nid_signs(re[v.point_a_sc+4*m+1]);
  }
 }
 __syncthreads();
 if(k<count && lane<2) {
  int begin=lane && css?v.css0_crb:lo,end=lane && css?v.css0_crb+v.css0_nrb:hi;
  nid_denominators(signs[tile],denominator[tile][lane],begin,end,
                   !lane && css?v.css0_crb:0,!lane && css?v.css0_crb+v.css0_nrb:0);
 }
 __syncthreads();
 int id=id_base+blockIdx.x*128+lane;if(id>=IDS || k>=count)return;
 int idx=v.index(first+k);
 uint64_t x=((uint64_t)v.sps*v.slot[idx]+v.sym[idx]+1)*((uint64_t)(id*2)+1);
 uint32_t seed=(uint32_t)(((x<<17)+id*2)&0x7fffffff);
 p->values[(2*k+1)*IDS+id]=nid_best_prepared(seed,lo,hi,0,signs[tile],denominator[tile][0],x1words);
 p->values[(2*k)*IDS+id]=css?nid_best_prepared(seed,v.css0_crb,v.css0_crb+v.css0_nrb,v.css0_crb,
                                           signs[tile],denominator[tile][1],x1words):0;
}
__device__ float secondary(const NidView &v,int k,int id,int d) {
 int idx=v.index(k),hi=(v.nsc-v.point_a_sc)/12,lo=v.point_a_sc>=0?0:(-v.point_a_sc+11)/12;
 uint64_t x=((uint64_t)v.sps*v.slot[idx]+v.sym[idx]+d+1)*((uint64_t)(id*2)+1);
 uint32_t seed=(uint32_t)(((x<<17)+id*2)&0x7fffffff);
 bool css=v.css0_nrb>=6 && v.css0_crb>=lo && v.css0_crb+v.css0_nrb<=hi;
 return nid_best_symbol(v,k,seed,lo,hi,css?v.css0_crb:0,css?v.css0_crb+v.css0_nrb:0,0,d);
}
__global__ void calibrate(Device *p,int first,int count,int base=0) {
 int j=base+blockIdx.x*blockDim.x+threadIdx.x,k=blockIdx.y,d=blockIdx.z+1;
 if(j>=NID_CAL || k>=count || d>=p->view.dur)return;
 p->values[(2*CHUNK+2*k+d-1)*IDS+j]=secondary(p->view,first+k,j*64,d);
}
__device__ void histogram_add(unsigned *h,int bin) {
 unsigned peers=__match_any_sync(__activemask(),bin);
 if((threadIdx.x&31)==__ffs(peers)-1)atomicAdd(h+bin,__popc(peers));
}
__global__ void standardize(Device *p,int base=0,int size=IDS) {
 __shared__ unsigned h[4096];
 int k=base+blockIdx.x,tid=threadIdx.x;
 for(int j=tid;j<4096;j+=blockDim.x)h[j]=0;
 __syncthreads();
 const float *v=p->values+k*IDS;
 for(int i=tid;i<size;i+=blockDim.x) {
  int b=(int)(v[i]*4095);histogram_add(h,b<0?0:b>=4096?4095:b);
 }
 __syncthreads();
 if(tid==0) {
  unsigned a=0;int b=0;while(b<4095 && a+h[b]<size/2)a+=h[b++];
  p->median[k]=(b+0.5f)/4095;
 }
 __syncthreads();
 for(int j=tid;j<4096;j+=blockDim.x)h[j]=0;
 __syncthreads();
 for(int i=tid;i<size;i+=blockDim.x) {
  int b=(int)(fabsf(v[i]-p->median[k])*4095);histogram_add(h,b>=4096?4095:b);
 }
 __syncthreads();
 if(tid==0) {
  unsigned a=0;int b=0;while(b<4095 && a+h[b]<size/2)a+=h[b++];
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
// Compact before secondary Gold generation, so sparse surviving IDs occupy
// full warps instead of forcing almost every warp through expensive branches.
__global__ void screen(Device *p,int count) {
 int id=blockIdx.x*blockDim.x+threadIdx.x,k=blockIdx.y;
 if(id>=IDS || k>=count)return;
 float z=(p->values[(2*k+1)*IDS+id]-p->median[2*k+1])/p->sigma[2*k+1];
 if(z>2.f) {
  int j=atomicAdd(p->selected_count+k,1);
  p->candidates[k*IDS+j]=id;
 }
}
__global__ void combine(Device *p,int first,int count,int base=0) {
 int j=base+blockIdx.x*blockDim.x+threadIdx.x,k=blockIdx.y;
 if(k>=count || j>=p->selected_count[k])return;
 int id=p->candidates[k*IDS+j];
 const float bound=sqrtf(2.f*logf((float)IDS))+2.f;
 float sum=(p->values[(2*k+1)*IDS+id]-p->median[2*k+1])/p->sigma[2*k+1];
 for(int d=1;d<p->view.dur && p->view.sym[p->view.index(first+k)]+d<p->view.sps;d++) {
  int cal=2*CHUNK+2*k+d-1;
  float value=id%64?secondary(p->view,first+k,id,d):p->values[cal*IDS+id/64];
  float z=(value-p->median[cal])/p->sigma[cal];
  if(z<=2.f)break;
  sum+=z;
  float combined=sum/sqrtf((float)(d+1));
  p->multi[(2*k+d-1)*IDS+id]=combined>bound?combined:0;
 }
}
__global__ void collect(Device *p,int first,int count) {
 int id=blockIdx.x*blockDim.x+threadIdx.x;
 if(id>=IDS)return;
 for(int k=0;k<count;k++) {
  // Scores are written by ID into a separate workspace, preserving CPU order.
  for(int d=1;d<p->view.dur;d++) {
   float value=p->multi[(2*k+d-1)*IDS+id];
   if(value<=0)continue;
   int sym=p->view.sym[p->view.index(first+k)],cell=(2*d+1)*CELLS+sym*IDS+id;
   p->hits[cell]++;p->zsum[cell]+=value;
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
// Each new batch is scored once. Subtract the evicted batch, add the new one.
// IQ and sufficient statistics share the same batch ring position.
__global__ void slide(Device *p,int ring,bool empty,bool evict) {
 int j=blockIdx.x*blockDim.x+threadIdx.x;if(j>=STAT)return;
 size_t h=(size_t)ring*STAT+j;
 unsigned old=evict?p->history_hits[h]:0;
 double zold=evict?p->history_z[h]:0;
 p->total_hits[j]=(empty?0:p->total_hits[j])-old+p->hits[j];
 p->total_z[j]=(empty?0:p->total_z[j])-zold+p->zsum[j];
 p->history_hits[h]=p->hits[j];p->history_z[h]=p->zsum[j];
}
// Explicit diagnostic only; production never copies the statistics to the CPU.
__global__ void diagnostic_stats(Device *p) {
 int j=blockIdx.x*blockDim.x+threadIdx.x;if(j>=STAT)return;
 p->hits[j]=p->total_hits[j];p->zsum[j]=(float)p->total_z[j];
}
__global__ void family_totals(Device *p) {
 __shared__ unsigned sums[256];
 int j=blockIdx.x*256+threadIdx.x;
 sums[threadIdx.x]=p->total_hits[j];__syncthreads();
 for(int n=128;n;n/=2){if(threadIdx.x<n)sums[threadIdx.x]+=sums[threadIdx.x+n];__syncthreads();}
 if(!threadIdx.x)p->summary[blockIdx.x].total=sums[0];
}
__global__ void thresholds(Device *p,double alpha) {
 int j=blockIdx.x*blockDim.x+threadIdx.x;
 if(j==0)p->compact.count=0;
 if(j>=42)return;
 int d=j/14,s=j%14;
 if(d>=p->view.dur || s+d>=p->view.sps){p->threshold[d][s]=0;p->lambda[d][s]=0;return;}
 unsigned sum=0;
 int base=((2*d+1)*CELLS+s*IDS)/256;
 for(int k=0;k<IDS/256;k++)sum+=p->summary[base+k].total;
 double lambda=(double)sum/IDS;
 int families=0;for(int n=0;n<p->view.dur;n++)families+=p->view.sps-n;
 p->threshold[d][s]=nid_threshold_alpha(lambda,families,alpha);
 p->lambda[d][s]=lambda;
}
__global__ void compact_list(Device *p) {
 int j=blockIdx.x*blockDim.x+threadIdx.x;
 if(j>=3*CELLS)return;
 int d=j/CELLS,s=j%CELLS/IDS;
 if(d>=p->view.dur || s+d>=p->view.sps)return;
 int cell=(2*d+1)*CELLS+j%CELLS;
 if(p->total_hits[cell]<p->threshold[d][s])return;
 unsigned n=atomicAdd(&p->compact.count,1);
 if(n<CAND_CAP)p->compact.item[n]={j,(float)p->total_z[cell]};
}
// Scan order is independent of CUDA atomic append order. Overflow fails closed.
void incremental_shortlist(HostTransfer *t) {
 auto &out=t->result;out={};
 if(t->compact.count>CAND_CAP){out.shortlist_overflow=1;return;}
 std::sort(t->compact.item,t->compact.item+t->compact.count,
           [](const Candidate &a,const Candidate &b){return a.cell<b.cell;});
 for(unsigned n=0;n<t->compact.count;n++) {
  const auto &c=t->compact.item[n];
  int d=c.cell/CELLS,s=c.cell%CELLS/IDS,id=c.cell%IDS;
  nid_list_insert(&out,id,s,d,c.score);
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
  if(!p->multisymbol)for(int j=0;j<list;j++)if(p->result.list_id[j]==nid)return;
  s.nid=nid;s.sym0=p->result.list_sym[list];
  int pa=s.g_idsw.point_a_sc;
  s.g_st2_clo=pa>=0?0:(-pa+11)/12;s.g_st2_chi=(s.g_idsw.nsc-pa)/12;
  p->explicit_duration=p->result.list_dur[list];
  s.g_st2_D=p->result.list_dur[list]>0?p->result.list_dur[list]:1;s.alive=1;
 }
}
__global__ void prepare_probe(Device *p,bool active) {
 auto &s=p->stage2;int k=blockIdx.x*blockDim.x+threadIdx.x;
 if(k>=s.g_idsw.n)return;
 p->lit[k]=0;
 if(!s.alive)return;
 int sym=s.g_idsw.sym[s.g_idsw.index(k)];
 if(sym+s.g_st2_D>s.g_idsw.sps)return;
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
  if(p->multisymbol && p->explicit_duration>0)return;
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
// One block per hypothesis; warps score snapshots. Control adds snapshot
// results in original order; warp reductions select maxima, never sum scores.
__global__ void score(Device *p,int base,int count,int snapshot=0,int snapshots=100000) {
 auto &s=p->stage2;int i=base+blockIdx.x,tid=threadIdx.x;
 if(!s.alive || i>=s.nh || i>=base+count || (snapshot && snapshot>=s.nact))return;
 // A warp owns one snapshot; cache each bundle once, shared across every AL
 // and the consistency vote. The largest legal geometry has <=275 bundles.
 __shared__ double excess[4][276],value[4];
 __shared__ int vote[4],al[4],nf,below;
 int lane=tid&31,warp=tid/32;
 const st2_hyp_t h=s.H[i];
 int D=s.g_st2_D,L=h.L?h.L:6,B=L/D,per=6/L,nb=6*h.ng*D/L;
 int cs=h.phase+6*h.g0;
 if(tid==0){if(!snapshot){s.H[i].score=0;memset(s.H[i].al_win,0,sizeof(s.H[i].al_win));p->score_nf[i]=p->score_below[i]=0;}nf=p->score_nf[i];below=p->score_below[i];}
 __syncthreads();
 const int stop=min(s.nact,snapshot+snapshots);
 for(int first=snapshot;first<stop;first+=4) {
  int k=first+warp;
  if(k<stop)for(int j=lane;j<nb;j+=32) {
   int f=h.L?((j%h.R)*(nb/h.R)+j/h.R+h.shift)%nb:j;
   excess[warp][j]=s.st2_exc(s.sn+k,cs+f*B,B);
  }
  __syncthreads();
  double best=-1;int key=0x7fffffff,full=0,partial=0;
  if(k<stop) {
   for(int ai=1;ai<5;ai++) {
    int aggregation=1<<ai;
    for(int cce=lane*aggregation;cce+aggregation<=h.ng*D;cce+=32*aggregation) {
     double v=nid_cached_candidate(excess[warp],cce*per,aggregation*per);
     int order=ai*512+cce;
     if(v>best || (v==best && order<key)){best=v;key=order;}
    }
   }
   double threshold=2.0/(3.0*L-1.0);
   for(int j0=lane*per;j0+per<=nb;j0+=32*per) {
    int hits=0;for(int dj=0;dj<per;dj++)hits+=excess[warp][j0+dj]>threshold;
    full+=hits==per;partial+=hits>0 && hits<per;
   }
  }
  for(int offset=16;offset;offset/=2) {
   double other=__shfl_down_sync(0xffffffff,best,offset);
   int order=__shfl_down_sync(0xffffffff,key,offset);
   if(other>best || (other==best && order<key)){best=other;key=order;}
   full+=__shfl_down_sync(0xffffffff,full,offset);
   partial+=__shfl_down_sync(0xffffffff,partial,offset);
  }
  if(!lane) {
   value[warp]=key==0x7fffffff?0:best;al[warp]=key==0x7fffffff?-1:key/512;
   vote[warp]=full+partial?(full<partial?0:1):-1;
  }
  __syncthreads();
  if(tid==0)for(int j=0;j<4 && first+j<stop;j++) {
   s.H[i].score+=value[j];if(al[j]>=0)s.H[i].al_win[al[j]]++;
   nf+=vote[j]>=0;below+=vote[j]==0;
  }
  __syncthreads();
 }
 if(tid==0) {p->score_nf[i]=nf;p->score_below[i]=below;
  if(snapshot+snapshots>=s.nact && (!nf || below>nf/2))s.H[i].score=0;
 }
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
 // Per-lane sorted tie lists, merged in stable score/index order. The old
 // control lane scanned up to 1M hypotheses here and exceeded 10 ms on GB10.
 int chosen[64],ntie=0;
 for(int i=tid;i<s.nh;i+=blockDim.x) {
  const auto &h=s.H[i];
  if(h.L!=win.L || h.R!=win.R || h.phase!=win.phase || fabs(h.score-win.score)>1e-9*(1+fabs(win.score)))continue;
  int at=0;while(at<ntie && s.H[chosen[at]].score>=h.score)at++;
  if(at>=64)continue;
  if(ntie<64)ntie++;
  for(int j=ntie-1;j>at;j--)chosen[j]=chosen[j-1];chosen[at]=i;
 }
 __shared__ int owner,ties;
 __shared__ st2_hyp_t handoff;
 if(tid==0){handoff=win;ties=0;}
 int used=0;
 for(int t=0;t<64;t++) {
  candidates[tid]=used<ntie?chosen[used]:-1;__syncthreads();
  for(int step=128;step;step/=2) {
   if(tid<step) {
    int a=candidates[tid],b=candidates[tid+step];
    if(b>=0 && (a<0 || s.H[b].score>s.H[a].score || (s.H[b].score==s.H[a].score && b<a)))candidates[tid]=b;
   }
   __syncthreads();
  }
  if(tid==0) {
   int i=candidates[0];owner=i<0?-1:i%(int)blockDim.x;
   if(i>=0) {
    ties++;const auto &h=s.H[i];
    if(!win.L && h.phase+6*h.g0<handoff.phase+6*handoff.g0) {
     handoff=h;handoff.ng=(s.g_st2_chi-(handoff.phase+6*handoff.g0))/6;
    }
   }
  }
  __syncthreads();if(owner<0)break;
  if(tid==owner)used++;
 }
 if(tid==0) {
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
 /* Duration 2/3 multi-symbol evidence (T19): own flag, default on (was ISAC_CSIRS_DISC). */
 const char *enable=getenv("ISAC_PDCCH_NID_MULTISYMBOL");
 p.multisymbol=!(enable && !strcmp(enable,"0"));
 int *slots=nullptr; c16_t *iq=nullptr;double *scratch=nullptr;st2_snap_t *sn=nullptr;
 int least_priority=0,greatest_priority=0;
 if(cudaDeviceGetStreamPriorityRange(&least_priority,&greatest_priority)!=cudaSuccess
    || (!nr_gpu_green_stream(&c->stream,least_priority,false)
        && cudaStreamCreateWithPriority(&c->stream,cudaStreamNonBlocking,least_priority)!=cudaSuccess))goto fail;
 if(!c->slice.init())goto fail;
 for(auto &e:c->event)if(cudaEventCreateWithFlags(&e,cudaEventDefault)!=cudaSuccess)goto fail;
 if(cudaEventCreateWithFlags(&c->done,cudaEventDisableTiming)!=cudaSuccess)goto fail;
 for(auto &pair:c->timer)for(auto &e:pair)if(cudaEventCreateWithFlags(&e,cudaEventDefault)!=cudaSuccess)goto fail;
 if(cudaHostAlloc(&c->staging,(size_t)3*NR_PDCCH_NID_BATCH*dur*sc*sizeof(c16_t),cudaHostAllocDefault)!=cudaSuccess)goto fail;
 memset(c->staging,0,(size_t)3*NR_PDCCH_NID_BATCH*dur*sc*sizeof(c16_t));
 if(cudaHostAlloc(&c->transfer,sizeof(HostTransfer),cudaHostAllocDefault)!=cudaSuccess)goto fail;
 memset(c->transfer,0,sizeof(HostTransfer));
 if(!allocate(c,&c->dev,1) || !allocate(c,&iq,(size_t)n*dur*sc) || !allocate(c,&slots,2*n)
    || !allocate(c,&p.candidates,CHUNK*IDS) || !allocate(c,&p.multi,2*CHUNK*IDS)
    || !allocate(c,&p.values,4*CHUNK*IDS) || !allocate(c,&p.zsum,6*CELLS) || !allocate(c,&p.hits,6*CELLS)
    || !allocate(c,&p.history_hits,(size_t)window*STAT) || !allocate(c,&p.history_z,(size_t)window*STAT)
    || !allocate(c,&p.total_hits,STAT) || !allocate(c,&p.total_z,STAT)
    || !allocate(c,&p.summary,STAT/256) || !allocate(c,&p.lit,n) || !allocate(c,&p.active,n)
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
 for(const void *f : { (const void*)diagnostic_stats,(const void*)slide,(const void*)family_totals,(const void*)thresholds,(const void*)compact_list,(const void*)reset,(const void*)sweep,(const void*)standardize,
                      (const void*)calibrate,(const void*)screen,(const void*)combine,(const void*)collect,(const void*)accumulate,(const void*)prepare_init,(const void*)prepare_probe,
                      (const void*)prepare_control,(const void*)prepare_snapshots,(const void*)hull,
                      (const void*)generate,(const void*)score,(const void*)finish,(const void*)summarize }) {
  cudaFuncAttributes a;if(cudaFuncGetAttributes(&a,f)!=cudaSuccess)goto fail;
 }
 reset<<<1,256,0,c->stream>>>(c->dev,p.view);
 slide<<<STAT/256,256,0,c->stream>>>(c->dev,0,true,false);
 diagnostic_stats<<<STAT/256,256,0,c->stream>>>(c->dev);
 family_totals<<<STAT/256,256,0,c->stream>>>(c->dev);
 thresholds<<<1,64,0,c->stream>>>(c->dev,0.01);
 compact_list<<<3*CELLS/256,256,0,c->stream>>>(c->dev);
 sweep<<<dim3(IDS/128,1),dim3(128,2),0,c->stream>>>(c->dev,0,0);
 standardize<<<2*CHUNK,256,0,c->stream>>>(c->dev);
 accumulate<<<IDS/256,256,0,c->stream>>>(c->dev,0,0);
 calibrate<<<dim3(NID_CAL/128,1,2),128,0,c->stream>>>(c->dev,0,0);
 screen<<<1,256,0,c->stream>>>(c->dev,0);
 combine<<<1,256,0,c->stream>>>(c->dev,0,0);
 collect<<<IDS/256,256,0,c->stream>>>(c->dev,0,0);
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
// Bound transfers and history clears as well as kernels. Metadata uses the
// same accounting, so no work is queued behind the arbiter's back.
static cudaError_t budget_copy(Context *c,void *dst,const void *src,size_t n,cudaMemcpyKind kind,cudaStream_t stream) {
 const size_t chunk=nr_gpu_budget_enabled()?1024*1024:n;
 for(size_t at=0;at<n;at+=chunk) {
  cudaError_t e=c->slice.run(stream,[&]() {
   return cudaMemcpyAsync((char *)dst+at,(const char *)src+at,std::min(chunk,n-at),kind,stream);
  });
  if(e!=cudaSuccess)return e;
 }
 return cudaSuccess;
}
static cudaError_t budget_zero(Context *c,void *dst,int value,size_t n,cudaStream_t stream) {
 const size_t chunk=nr_gpu_budget_enabled()?1024*1024:n;
 for(size_t at=0;at<n;at+=chunk) {
  cudaError_t e=c->slice.run(stream,[&]() {
   return cudaMemsetAsync((char *)dst+at,value,std::min(chunk,n-at),stream);
  });
  if(e!=cudaSuccess)return e;
 }
 return cudaSuccess;
}
int upload(void *ptr,const nr_pdcch_nid_batch_t *b) {
 Context *c=(Context*)ptr;
 if(b->cfg.sps<1 || b->cfg.sps>14 || b->cfg.nsc<72 || b->cfg.nsc>12*c->rb || b->cfg.dur!=c->dur
    || (b->cfg.nsc-b->cfg.point_a_sc)/12>275
    || (b->cfg.nsc-b->cfg.point_a_sc)/12-(b->cfg.point_a_sc<0?(-b->cfg.point_a_sc+11)/12:0)<6)return -1;
 int count=b->count?b->count:NR_PDCCH_NID_BATCH;
 if(count<1 || count>NR_PDCCH_NID_BATCH || (b->count && !c->host.multisymbol))return -1;
 if(c->pending && c->incremental)return -1; // never silently lose an unscored batch
 bool changed=memcmp(&b->cfg,&c->cfg,sizeof(c->cfg)) || count!=c->batch_size || c->incremental!=(b->count>0);
 if(changed) {c->batches=0;c->cfg=b->cfg;c->bank={};}
 c->batch_size=count;c->incremental=b->count>0;
 for(int k=0;k<count;k++)if(b->sym[k]<0 || b->sym[k]>=b->cfg.sps || b->slot[k]<0 || b->slot[k]>=160)return -1;
 int first=(c->batches%c->window)*count;
 auto stream=c->stream;NidView &v=c->host.view;
 // Only these small metadata arrays need a host copy; CUDA never stages
 // pageable pointers in the processing path (including result/control D2H).
 memcpy(c->transfer->slot,b->slot,sizeof(b->slot));
 memcpy(c->transfer->sym,b->sym,sizeof(b->sym));
 // Staging has max-RB stride; no repack or host-side pilot extraction.
 if(budget_copy(c,(void*)(v.re+(size_t)first*c->dur*v.stride),b->iq,
                     (size_t)count*c->dur*v.stride*sizeof(c16_t),cudaMemcpyHostToDevice,stream)!=cudaSuccess
    || budget_copy(c,(void*)(v.slot+first),c->transfer->slot,count*sizeof(int),cudaMemcpyHostToDevice,stream)!=cudaSuccess
    || budget_copy(c,(void*)(v.sym+first),c->transfer->sym,count*sizeof(int),cudaMemcpyHostToDevice,stream)!=cudaSuccess)return -1;
 c->batches++;c->pending=count;
 v.capacity=c->window*count;
 v.nsc=b->cfg.nsc;v.point_a_sc=b->cfg.point_a_sc;v.css0_crb=b->cfg.css0_crb;v.css0_nrb=b->cfg.css0_nrb;v.sps=b->cfg.sps;
 v.n=(c->batches<c->window?c->batches:c->window)*count;
 v.begin=c->batches<c->window?0:(c->batches%c->window)*count;
 c->first_new=v.n-count;
 return cudaStreamSynchronize(stream)==cudaSuccess?0:-1;
}
// Events are allocated/touched before RX. No CUDA allocation or chunk wait here.
bool timing_begin(Context *c,int kind,const char *operation) {
 if(c->ntimer==TIMERS)return false;
 c->kind[c->ntimer]=kind;c->operation[c->ntimer]=operation;
 return cudaEventRecord(c->timer[c->ntimer][0],c->stream)==cudaSuccess;
}
bool timing_end(Context *c) {
 return cudaGetLastError()==cudaSuccess && cudaEventRecord(c->timer[c->ntimer++][1],c->stream)==cudaSuccess;
}
// Slice completion already retired these events; never enqueue/wait another event.
bool collect_timing(Context *c) {
 for(int i=0;i<c->ntimer;i++) {
  float ms=0;if(cudaEventElapsedTime(&ms,c->timer[i][0],c->timer[i][1])!=cudaSuccess)return false;
  int k=c->kind[i];
  if(ms>5.f && ms>c->timing.kernel_peak_ms[k])
   fprintf(stderr,"PDCCH_NID_GPU kernel_overrun operation=%s elapsed_ms=%.6f\n",c->operation[i],ms);
  c->timing.kernel_calls[k]++;c->timing.kernel_total_ms[k]+=ms;
  c->timing.kernel_peak_ms[k]=fmaxf(c->timing.kernel_peak_ms[k],ms);
  c->timing.kernel_max_ms=fmaxf(c->timing.kernel_max_ms,ms);
 }
 c->ntimer=0;return true;
}
#define LAUNCH(kind, ...) do { \
 if(c->ntimer==TIMERS && !complete(c,start))return -1; \
 if(c->slice.run(stream,[&]() {if(!timing_begin(c,kind,#__VA_ARGS__))return cudaErrorUnknown; \
 __VA_ARGS__;return timing_end(c)?cudaSuccess:cudaErrorUnknown;},#__VA_ARGS__)!=cudaSuccess)return -1; \
 if(nr_gpu_budget_enabled() && !nr_gpu_allocations_allowed() && !collect_timing(c))return -1; \
} while(0)
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
  std::this_thread::yield();
 }
 return collect_timing(c);
}

int run_impl(void *ptr,nr_pdcch_nid_result_t *out,const nr_pdcch_nid_result_t *seed) {
 Context *c=(Context*)ptr;auto stream=c->stream;Device *p=c->dev;
 if(!c->batches || (!c->incremental && c->batches<c->window))return 1;
 if(c->incremental && !c->pending){*out=c->bank;return 0;}
 const auto start=std::chrono::steady_clock::now();c->timing={};c->ntimer=0;
 if(cudaEventRecord(c->event[0],stream)!=cudaSuccess
    || budget_zero(c,c->host.hits,0,6*CELLS*sizeof(uint16_t),stream)!=cudaSuccess
    || budget_zero(c,c->host.zsum,0,6*CELLS*sizeof(float),stream)!=cudaSuccess)return -1;
 LAUNCH(RESET,reset<<<1,256,0,stream>>>(p,c->host.view));
 if(!seed) {
  const int chunk=NR_NID_SWEEP_SNAPSHOTS;
  for(int first=c->incremental?c->first_new:0;first<c->host.view.n;first+=chunk) {
   int n=nr_nid_chunk_count(c->host.view.n,first,chunk);
   const int ids=IDS;
   for(int id=0;id<IDS;id+=ids)
    LAUNCH(SWEEP,sweep<<<dim3(ids/128,(n+1)/2),dim3(128,2),0,stream>>>(p,first,n,id));
   LAUNCH(STANDARDIZE,standardize<<<2*n,256,0,stream>>>(p));
   LAUNCH(ACCUMULATE,accumulate<<<IDS/256,256,0,stream>>>(p,first,n));
   if(c->host.multisymbol && c->dur>1) {
    for(int base=0;base<NID_CAL;base+=NR_NID_SECONDARY_IDS)
     LAUNCH(SWEEP,calibrate<<<dim3(NR_NID_SECONDARY_IDS/128,n,c->dur-1),128,0,stream>>>(p,first,n,base));
    LAUNCH(STANDARDIZE,standardize<<<2*n,256,0,stream>>>(p,2*CHUNK,NID_CAL));
    if(budget_zero(c,p->selected_count,0,sizeof(c->host.selected_count),stream)!=cudaSuccess
       || budget_zero(c,c->host.multi,0,2*n*IDS*sizeof(float),stream)!=cudaSuccess)return -1;
    LAUNCH(ACCUMULATE,screen<<<dim3(IDS/256,n),256,0,stream>>>(p,n));
    // Read the compact length, not all 65536 possible IDs. Bound even the
    // adversarial all-survivor case and skip empty tail launches.
    int *selected=c->transfer->control;
    if(budget_copy(c,selected,p->selected_count,n*sizeof(int),cudaMemcpyDeviceToHost,stream)!=cudaSuccess
       || !complete(c,start))return -1;
    int survivors=*std::max_element(selected,selected+n);
    for(int base=0;base<survivors;base+=NR_NID_SECONDARY_IDS)
     LAUNCH(SWEEP,combine<<<dim3(NR_NID_SECONDARY_IDS/128,n),128,0,stream>>>(p,first,n,base));
    LAUNCH(ACCUMULATE,collect<<<IDS/256,256,0,stream>>>(p,first,n));
   }
  }
  if(c->incremental) {
   LAUNCH(ACCUMULATE,slide<<<STAT/256,256,0,stream>>>(p,(c->batches-1)%c->window,c->batches==1,c->batches>c->window));
   LAUNCH(SUMMARY,family_totals<<<STAT/256,256,0,stream>>>(p));
   LAUNCH(SUMMARY,thresholds<<<1,64,0,stream>>>(p,nid_look_alpha(++c->look)));
   LAUNCH(SUMMARY,compact_list<<<3*CELLS/256,256,0,stream>>>(p));
   if(budget_copy(c,&c->transfer->compact,&p->compact,sizeof(Compact),cudaMemcpyDeviceToHost,stream)!=cudaSuccess
      || budget_copy(c,c->transfer->lambda,p->lambda,sizeof(p->lambda),cudaMemcpyDeviceToHost,stream)!=cudaSuccess
      || budget_copy(c,c->transfer->threshold,p->threshold,sizeof(p->threshold),cudaMemcpyDeviceToHost,stream)!=cudaSuccess
      || !complete(c,start))return -1;
   incremental_shortlist(c->transfer);
   memcpy(c->transfer->result.lambda,c->transfer->lambda,sizeof(p->lambda));
   memcpy(c->transfer->result.threshold,c->transfer->threshold,sizeof(p->threshold));
  } else {
   // Diagnostic fixed-window parity API only: full hashes/strength readback.
   LAUNCH(SUMMARY,summarize<<<2*CELLS/256,256,0,stream>>>(p));
   if(budget_copy(c,c->transfer->hits,c->host.hits,sizeof(c->transfer->hits),cudaMemcpyDeviceToHost,stream)!=cudaSuccess
      || budget_copy(c,c->transfer->summary,c->host.summary,sizeof(c->transfer->summary),cudaMemcpyDeviceToHost,stream)!=cudaSuccess
      || budget_copy(c,c->transfer->zsum,c->host.zsum,sizeof(c->transfer->zsum),cudaMemcpyDeviceToHost,stream)!=cudaSuccess
      || !complete(c,start))return -1;
   c->transfer->result={};
   if(c->host.multisymbol)nid_shortlist(c->transfer->hits,c->transfer->zsum,c->dur,&c->transfer->result);
   else shortlist(c->transfer);
  }
 } else {
  c->transfer->result={};c->transfer->result.list_n=seed->list_n;
  memcpy(c->transfer->result.list_id,seed->list_id,sizeof(seed->list_id));
  memcpy(c->transfer->result.list_sym,seed->list_sym,sizeof(seed->list_sym));
  memcpy(c->transfer->result.list_dur,seed->list_dur,sizeof(seed->list_dur));
 }
 int lists=c->transfer->result.list_n;
 if(budget_copy(c,&p->result,&c->transfer->result,sizeof(*out),cudaMemcpyHostToDevice,stream)!=cudaSuccess
    || cudaEventRecord(c->event[1],stream)!=cudaSuccess)return -1;
 for(int j=0;j<lists;j++) {
  bool known=false;
  if(c->incremental)for(int k=0;k<c->bank.count;k++) {
   const auto &r=c->bank.records[k];const auto &list=c->transfer->result;
   if(r.nid==list.list_id[j] && r.sym==list.list_sym[j] && r.dur==list.list_dur[j]) {
    known=true;c->last_seen[k]=c->look;
   }
  }
  if(known)continue;
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
   if(budget_copy(c,control,&p->stage2.nact,5*sizeof(int),cudaMemcpyDeviceToHost,stream)!=cudaSuccess
      || !complete(c,start))return -1;
   // Contiguous stage-2 metadata: nact, nid, sym0, nh, alive.
   if(!control[4])break;
   const int hypotheses=NR_NID_SCORE_HYPOTHESES;
   const int snapshots=NR_NID_SCORE_SNAPSHOTS;
   for(int base=0;base<control[3];base+=hypotheses)
    for(int snap=0;snap<std::max(1,control[0]);snap+=snapshots)
     LAUNCH(SCORE,score<<<nr_nid_chunk_count(control[3],base,hypotheses),128,0,stream>>>(p,base,hypotheses,snap,snapshots));
   LAUNCH(FINISH,finish<<<1,256,0,stream>>>(p));
   LAUNCH(HULL,hull<<<3,128,0,stream>>>(p));
   LAUNCH(GENERATE,generate<<<1,32,0,stream>>>(p));
  }
 }
 if(cudaEventRecord(c->event[2],stream)!=cudaSuccess
    || budget_copy(c,&c->transfer->result,&p->result,sizeof(*out),cudaMemcpyDeviceToHost,stream)!=cudaSuccess
    || !complete(c,start))return -1;
 *out=c->transfer->result;
 if(c->incremental) {
  for(int k=0;k<c->bank.count;) {
   if(c->look-c->last_seen[k]<(uint64_t)c->window){k++;continue;}
   int last=--c->bank.count;c->bank.records[k]=c->bank.records[last];c->last_seen[k]=c->last_seen[last];
  }
  for(int j=0;j<out->count;j++) {
   int k=0;for(;k<c->bank.count;k++)if(!memcmp(c->bank.records+k,out->records+j,sizeof(out->records[j])))break;
   if(k==c->bank.count && k<NR_PDCCH_NID_MAX_RECORDS)c->bank.count++;
   if(k<NR_PDCCH_NID_MAX_RECORDS){c->bank.records[k]=out->records[j];c->last_seen[k]=c->look;}
  }
  out->count=c->bank.count;memcpy(out->records,c->bank.records,sizeof(out->records));
  c->pending=0;
 }
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

extern "C" int nr_pdcch_nid_gpu_statistics_check(void *ctx,uint16_t *hits,float *zs) {
 auto *c=(Context*)ctx;
 if(!c || !c->incremental || c->pending || !hits || !zs)return -1;
 diagnostic_stats<<<STAT/256,256,0,c->stream>>>(c->dev);
 if(budget_copy(c,c->transfer->hits,c->host.hits,sizeof(c->transfer->hits),cudaMemcpyDeviceToHost,c->stream)!=cudaSuccess
    || budget_copy(c,c->transfer->zsum,c->host.zsum,sizeof(c->transfer->zsum),cudaMemcpyDeviceToHost,c->stream)!=cudaSuccess
    || cudaStreamSynchronize(c->stream)!=cudaSuccess)return -1;
 memcpy(hits,c->transfer->hits,sizeof(c->transfer->hits));memcpy(zs,c->transfer->zsum,sizeof(c->transfer->zsum));return 0;
}
