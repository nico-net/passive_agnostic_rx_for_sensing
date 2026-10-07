/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_pdcch_nid_algorithm.h"
#include "nr_pdcch_nid_reference.h"
#include "nr_pdcch_nid_stage1.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <dlfcn.h>

static bool same(const nr_pdcch_nid_result_t &a,const nr_pdcch_nid_result_t &b,bool stage1) {
 if(a.count!=b.count || memcmp(a.records,b.records,a.count*sizeof(a.records[0])))return false;
 if(memcmp(a.al_win,b.al_win,a.count*sizeof(a.al_win[0])))return false;
 if(stage1 && (memcmp(a.hit_hash,b.hit_hash,sizeof(a.hit_hash)) || memcmp(a.top_id,b.top_id,sizeof(a.top_id)) || memcmp(a.top_sym,b.top_sym,sizeof(a.top_sym))
    || a.list_n!=b.list_n || memcmp(a.list_id,b.list_id,a.list_n*sizeof(int))
    || memcmp(a.list_sym,b.list_sym,a.list_n*sizeof(int))
    || memcmp(a.list_dur,b.list_dur,a.list_n*sizeof(int))
    || memcmp(a.lambda,b.lambda,sizeof(a.lambda)) || memcmp(a.threshold,b.threshold,sizeof(a.threshold))))return false;
 return true;
}
static void dump(const char *name,const nr_pdcch_nid_result_t &r) {
 fprintf(stderr,"%s: count=%d top=%d/%d %d/%d\n",name,r.count,r.top_id[0],r.top_sym[0],r.top_id[1],r.top_sym[1]);
 for(int i=0;i<r.count;i++) {const auto &a=r.records[i];fprintf(stderr,"%d %d %d %d %d %d %d %d %d\n",a.nid,a.sym,a.dur,a.grid_rb,a.groups,a.L,a.R,a.shift,a.ties);}
}
int main(int argc,char **argv) {
 if(argc>2 && !strcmp(argv[1],"--frozen"))return nid_reference_original_main(argc-1,argv+1);
 int fixture=argc>1?atoi(argv[1]):7;
 bool device=argc>2 && !strcmp(argv[2],"--device");
 bool stage1=fixture==0 || fixture>=7;
 nr_pdcch_nid_batch_t b={};nid_reference_make(fixture,&b);
 nr_pdcch_nid_result_t want={},got={};
 nid_reference_run(stage1,&want);
 if(device) {
  setenv("ISAC_CSIRS_DISC","1",1);
  void *lib=dlopen("libpdcch_nid_gpu.so",RTLD_NOW|RTLD_LOCAL);
  if(!lib) {fprintf(stderr,"%s\n",dlerror());return 2;}
  auto get=(const nr_pdcch_nid_ops_t*(*)())dlsym(lib,"nr_pdcch_nid_gpu_ops");
  if(!get)return 2;
  const auto *ops=get();if(!ops || ops->abi!=NR_PDCCH_NID_ABI)return 2;
  nr_pdcch_nid_iq_t *staging=nullptr;
  void *ctx=ops->create(b.cfg.nsc/12,b.cfg.dur,1,&staging);if(!ctx)return 2;
  size_t bytes=(size_t)336*b.cfg.dur*b.cfg.nsc*sizeof(*staging);
  memcpy(staging,b.iq,bytes);b.iq=staging;
  auto stats=(void(*)(uint64_t*,uint64_t*))dlsym(lib,"nr_dl_gpu_memory_stats");
  uint64_t before,denied,after,denied_after;if(!stats)return 2;stats(&before,&denied);
  auto stage2=(int(*)(void*,const nr_pdcch_nid_result_t*,nr_pdcch_nid_result_t*))dlsym(lib,"nr_pdcch_nid_gpu_stage2_check");
  if(!stage1 && !stage2)return 2;
  auto run=[&](nr_pdcch_nid_result_t *r){return stage1?ops->run(ctx,r):stage2(ctx,&want,r);};
  if(ops->upload(ctx,&b) || run(&got))return 2;
  // Repeat through device-window wrap and assert no late allocation attempts.
  nr_pdcch_nid_result_t repeated={};
  if(ops->upload(ctx,&b) || run(&repeated) || !same(got,repeated,stage1))return 3;
  stats(&after,&denied_after);if(before!=after || denied!=denied_after)return 4;
  ops->destroy(ctx);dlclose(lib);
 } else {
  got=want;got.count=0;
  NidStage2 s={};int n=336,rb=b.cfg.nsc/12;
  std::vector<double> scratch((size_t)(4*n+1)*6*(rb+1)+2*n);
  std::vector<st2_snap_t> snaps(4*n+1);
  std::vector<st2_hyp_t> hyps(NID_HYP_CAP);
  for(int j=0;j<4*n+1;j++) {
   double *p=scratch.data()+(size_t)j*6*(rb+1);
   snaps[j]={p,p+rb+1,p+2*(rb+1),p+3*(rb+1),p+4*(rb+1),p+5*(rb+1),-1};
  }
  s.g_idsw={b.cfg.nsc,b.cfg.point_a_sc,b.cfg.css0_crb,b.cfg.css0_nrb,14,n,b.cfg.dur,0,n,b.cfg.nsc,b.slot,b.sym,(c16_t*)b.iq};
  s.sn=snaps.data();for(int d=0;d<3;d++)s.dec[d]=s.sn+(d+1)*n;s.tmp=snaps[4*n];
  s.H=hyps.data();s.vr=scratch.data()+(size_t)(4*n+1)*6*(rb+1);s.vd=s.vr+n;
  for(int j=0;j<want.list_n;j++) {
   s.prepare(want.list_id[j],want.list_sym[j],want.list_dur[j]);
   for(int it=0;it<6 && s.alive;it++) {
    for(int i=0;i<s.nh;i++)s.score(i);
    s.finish(&got);
   }
  }
 }
 // Parity must not pass vacuously if a planted reference scene goes empty.
 if(fixture>=1 && fixture<=6 && !got.count)return 7;
 if((fixture==1 || fixture==3) && got.records[0].L!=0)return 8;
 if((fixture==2 || fixture==4) && got.records[0].L!=6)return 9;
 if((fixture==3 || fixture==4) && got.records[0].dur!=2)return 10;
 if(fixture==4 && got.records[0].R!=2)return 11;
 // The frozen AL1-only reference can rank an interleaved geometry on this host;
 // fixture 5 deliberately requires parity, not a stronger classification claim.
 if(fixture==6) {
  int a=0,b=0;for(int j=0;j<got.count;j++){a+=got.records[j].nid==12345;b+=got.records[j].nid==777;}
  if(got.count!=3 || a!=2 || b!=1)return 12;
 }
 if(!same(want,got,stage1)) {dump("CPU reference",want);dump("ported",got);return 1;}
 if(fixture>=7 && fixture<=9 && (got.count==0 || got.records[0].nid!=12345 || got.records[0].dur!=fixture-6 || got.records[0].sym!=3)) {dump("duration failure",got);return 5;}
 if(fixture>=10) {
  int D=fixture==21?1:fixture<=11 || fixture==20?2:3,sym=fixture==10?0:fixture==11?1:fixture==19?11:fixture==20?12:3;
  if(fixture==16 || fixture==17) {
   if(got.count || got.list_n)return 13;
  } else {
   bool found=false,other=fixture!=15,listed=false;
   for(int i=0;i<got.list_n;i++)listed|=got.list_id[i]==12345 && got.list_sym[i]==sym && got.list_dur[i]==D;
   for(int i=0;i<got.count;i++) {
    const auto &r=got.records[i];
    if(r.nid==777 && r.sym==8 && r.dur==1)other=true;
    if(r.nid!=12345 || r.sym!=sym || r.dur!=D)continue;
    found=true;
    if(r.L && (r.L!=6 && r.L!=(D==3?3:2)))return 14;
    if((fixture==12 || fixture==13) && (r.L!=6 || r.R!=(fixture==12?2:3) || r.grid_rb!=12 || r.groups!=4 || (fixture==13 && r.shift!=1))) {dump("mapping failure",got);return 15;}
    if(fixture==18 && (r.L!=3 || r.R!=2 || r.grid_rb!=12 || r.groups!=4))return 21;
    if((fixture==14 || fixture==15) && r.L!=0) {dump("NI failure",got);return 16;}
   }
   if(!found || !other || !listed) {dump("missing plant",got);return 17;}
  }
 }
 if(stage1) {
  for(int i=0;i<got.list_n;i++)for(int j=0;j<i;j++)
   if(got.list_id[i]==got.list_id[j] && got.list_sym[i]<got.list_sym[j]+got.list_dur[j]
      && got.list_sym[j]<got.list_sym[i]+got.list_dur[i])return 20;
  double budget=0;int families=0;
  for(int d=0;d<b.cfg.dur;d++)families+=14-d;
  for(int d=0;d<b.cfg.dur;d++)for(int s=0;s<14-d;s++) {
   double lambda=want.lambda[d][s];int k=want.threshold[d][s];
   double tail=NID_IDS*nid_poisson_tail(lambda,k);budget+=tail;
   if(tail>=.01/families || (k>1 && NID_IDS*nid_poisson_tail(lambda,k-1)<.01/families))return 18;
  }
  if(budget>=.01)return 19;
  printf("FALSE_ALARM_MODEL expected_false_cells<=%.9g budget=0.01\n",budget);
 }
 if(fixture==0 && (got.top_id[0]!=2 || got.top_sym[0]!=0 || got.top_id[1]!=12345 || got.top_sym[1]!=3))return 6;
 if(device)printf("GPU_STAGE_MS stage1=%.3f stage2=%.3f\n",got.stage1_ms,got.stage2_ms);
 printf("PASS fixture=%d %s == CPU reference (%d records)\n",fixture,device?"GPU":"CPU port",got.count);
 return 0;
}
