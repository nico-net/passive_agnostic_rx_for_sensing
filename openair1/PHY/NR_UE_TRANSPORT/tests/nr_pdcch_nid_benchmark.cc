/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_pdcch_nid_reference.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <vector>

int main(int argc,char **argv) {
 const int rb=argc>1?atoi(argv[1]):217,dur=argc>2?atoi(argv[2]):1,repeats=argc>3?atoi(argv[3]):3;
 if((rb!=217 && rb!=273) || dur<1 || dur>3 || repeats<1)return 2;
 void *lib=dlopen("libpdcch_nid_gpu.so",RTLD_NOW|RTLD_LOCAL);
 if(!lib){fprintf(stderr,"%s\n",dlerror());return 2;}
 auto get=(const nr_pdcch_nid_ops_t*(*)())dlsym(lib,"nr_pdcch_nid_gpu_ops");
 auto stats=(void(*)(uint64_t*,uint64_t*))dlsym(lib,"nr_dl_gpu_memory_stats");
 if(!get || !stats || get()->abi!=NR_PDCCH_NID_ABI)return 2;
 const auto *ops=get();nr_pdcch_nid_iq_t *staging=nullptr;
 void *ctx=ops->create(rb,dur,10,&staging);if(!ctx)return 2;
 nr_pdcch_nid_batch_t src={},b={};nid_reference_make(6+dur,&src);
 b.cfg=src.cfg;b.cfg.nsc=12*rb;b.iq=staging;
 memcpy(b.slot,src.slot,sizeof(b.slot));memcpy(b.sym,src.sym,sizeof(b.sym));
 uint32_t rng=0x12345678;
 for(int k=0;k<NR_PDCCH_NID_BATCH;k++)for(int d=0;d<dur;d++)for(int sc=0;sc<b.cfg.nsc;sc++) {
  rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;
  staging[((size_t)k*dur+d)*b.cfg.nsc+sc]=sc<src.cfg.nsc?src.iq[((size_t)k*dur+d)*src.cfg.nsc+sc]
    :nr_pdcch_nid_iq_t{(int16_t)((rng&1)?100:-100),(int16_t)((rng&2)?100:-100)};
 }
 for(int j=0;j<10;j++)if(ops->upload(ctx,&b))return 3;
 uint64_t before,denied,after,denied_after;stats(&before,&denied);
 static const char *names[]={"reset","sweep","standardize","accumulate","prepare_init","probe","control",
                             "snapshots","hull","generate","score","finish","summary"};
 nr_pdcch_nid_result_t previous={};
 bool target=true;
 for(int pass=0;pass<repeats;pass++) {
  auto start=std::chrono::steady_clock::now();nr_pdcch_nid_result_t r={};
  int rc=ops->run(ctx,&r);
  double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
  if(rc){fprintf(stderr,"run failed rc=%d\n",rc);return 3;}
  bool planted=false;for(int j=0;j<r.count;j++)planted|=r.records[j].nid==12345;
  if(!planted)return 4;
  if(pass && (r.count!=previous.count || memcmp(r.records,previous.records,r.count*sizeof(r.records[0]))
      || memcmp(r.hit_hash,previous.hit_hash,sizeof(r.hit_hash))))return 4;
  previous=r;
  printf("PDCCH_NID_BENCH rb=%d window=10 dur=%d pass=%d wall_ms=%.3f stage1_ms=%.3f stage2_ms=%.3f kernels_max_ms=%.3f records=%d budget_exceeded=%d\n",
         rb,dur,pass,ms,r.stage1_ms,r.stage2_ms,r.kernel_max_ms,r.count,r.budget_exceeded);
  for(int k=0;k<NR_PDCCH_NID_KERNEL_KINDS;k++)
   printf("  %-14s calls=%u total_ms=%.3f max_ms=%.3f\n",names[k],r.kernel_calls[k],r.kernel_total_ms[k],r.kernel_peak_ms[k]);
  target&=r.kernel_max_ms<10 && (rb!=217 || dur!=1 || ms<=150);
 }
 stats(&after,&denied_after);ops->destroy(ctx);dlclose(lib);
 if(before!=after || denied!=denied_after)return 5;
 puts(target?"PERFORMANCE TARGET PASS":"PERFORMANCE TARGET FAIL (retain timing report)");
 return target?0:6;
}
