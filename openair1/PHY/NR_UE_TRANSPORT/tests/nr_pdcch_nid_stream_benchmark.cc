/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_pdcch_nid_stage1.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <random>
#include <thread>

// Device-only synthetic arrival stream: 30 kHz SCS, 3/5 DL slots (assumption,
// not OTA evidence), rotating 0..13, antenna 0/already combined four-RX input.
int main(int argc,char **argv) {
 int rb=argc>1?atoi(argv[1]):217,dur=argc>2?atoi(argv[2]):3;
 double snr=argc>3?atof(argv[3]):5;
 int batch=argc>4?atoi(argv[4]):64,limit=argc>5?atoi(argv[5]):80;
 if(rb<60 || rb>275 || dur<1 || dur>3 || batch<1 || batch>336 || limit<1)return 2;
 setvbuf(stdout,nullptr,_IOLBF,0);setenv("ISAC_PDCCH_NID_GPU_INCREMENTAL","1",1);setenv("ISAC_PDCCH_NID_MULTISYMBOL","1",1);
 void *lib=dlopen("libpdcch_nid_gpu.so",RTLD_NOW|RTLD_LOCAL);
 if(!lib){fprintf(stderr,"dlopen: %s\n",dlerror());return 2;}
 auto get=(const nr_pdcch_nid_ops_t*(*)())dlsym(lib,"nr_pdcch_nid_gpu_ops");
 auto stats=(void(*)(uint64_t*,uint64_t*))dlsym(lib,"nr_dl_gpu_memory_stats");
 if(!get || !stats || get()->abi!=NR_PDCCH_NID_ABI)return 2;
 nr_pdcch_nid_iq_t *iq=nullptr;auto *ops=get();void *ctx=ops->create(rb,dur,10,&iq);
 if(!ctx){fprintf(stderr,"create failed rb=%d dur=%d\n",rb,dur);return 2;}
 nr_pdcch_nid_batch_t b={};b.cfg={12*rb,0,0,6,415,14,dur};b.iq=iq;b.count=batch;
 uint64_t alloc0,deny0,alloc1,deny1;stats(&alloc0,&deny0);
 std::mt19937 rng(2207);std::normal_distribution<float> noise(0,400);
 float amplitude=400*powf(10,snr/20);
 int abs_slot=0,captured=0;double first=-1,max_kernel=0,total_sweep=0;
 using Clock=std::chrono::steady_clock;auto start=Clock::now();
 bool timing_ok=true;
 for(int pass=0;pass<limit;pass++) {
  for(int k=0;k<batch;k++,captured++,abs_slot++) {
   while(abs_slot%5>=3)abs_slot++;
   b.slot[k]=abs_slot%20;b.sym[k]=captured%14;
   // Traffic on 2/3 of independently indexed DL occasions; two AL4 candidates.
   int occasion=abs_slot/5,crb=12+12*(occasion%2);
   for(int d=0;d<dur;d++) {
    int sym=b.sym[k]+d;auto *grid=iq+((size_t)k*dur+d)*12*rb;
    for(int sc=0;sc<12*rb;sc++)grid[sc]={(int16_t)noise(rng),(int16_t)noise(rng)};
    if(sym>=14){memset(grid,0,12*rb*sizeof(*grid));continue;}
    if(sym<3 || sym>=3+dur || occasion%3==2)continue;
    uint32_t x1=0,x2=(uint32_t)((((uint64_t)(14*b.slot[k]+sym+1)*(2*12345+1))<<17)+2*12345)&0x7fffffff,gold[54];
    for(int w=0;w<54;w++)gold[w]=nid_gold(&x1,&x2,w==0);
    for(int c=crb;c<crb+24/dur;c++)for(int q=0;q<3;q++) {
     float xr,xi;idsw_pilot(gold,3*c+q,&xr,&xi);
     grid[12*c+1+4*q].r+=(int16_t)(amplitude*xr);
     grid[12*c+1+4*q].i-=(int16_t)(amplitude*xi);
    }
   }
  }
  double air_ms=abs_slot*0.5;
  std::this_thread::sleep_until(start+std::chrono::microseconds((int64_t)(air_ms*1000)));
  auto before=Clock::now();nr_pdcch_nid_result_t r={};
  int rc=ops->upload(ctx,&b);if(!rc)rc=ops->run(ctx,&r);
  double batch_ms=std::chrono::duration<double,std::milli>(Clock::now()-before).count();
  double elapsed=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
  if(rc){fprintf(stderr,"stream failed pass=%d rc=%d\n",pass,rc);ops->destroy(ctx);return 3;}
  bool planted=false;for(int j=0;j<r.count;j++)planted|=r.records[j].nid==12345 && r.records[j].sym==3 && r.records[j].dur==dur;
  if(planted && first<0)first=elapsed;
  max_kernel=std::max(max_kernel,(double)r.kernel_max_ms);total_sweep+=r.kernel_total_ms[1];
  double tmac=(65536.0*3*rb*batch)/(r.kernel_total_ms[1]*1e9);
  printf("PDCCH_NID_STREAM rb=%d dur=%d snr_db=%.1f batch=%d pass=%d air_ms=%.3f wall_ms=%.3f batch_ms=%.3f stage1_ms=%.3f stage2_ms=%.3f max_kernel_ms=%.3f effective_tmac_s=%.4f records=%d first_record_ms=%.3f lambda=%.7g k=%d\n",
    rb,dur,snr,batch,pass,air_ms,elapsed,batch_ms,r.stage1_ms,r.stage2_ms,r.kernel_max_ms,tmac,r.count,first,r.lambda[dur-1][3],r.threshold[dur-1][3]);
  static const char *names[]={"reset","sweep","standardize","accumulate","prepare_init","probe","control",
                              "snapshots","hull","generate","score","finish","summary"};
  for(int j=0;j<NR_PDCCH_NID_KERNEL_KINDS;j++)
   printf("PDCCH_NID_KERNEL pass=%d kernel=%s calls=%u total_ms=%.6f peak_ms=%.6f\n",
          pass,names[j],r.kernel_calls[j],r.kernel_total_ms[j],r.kernel_peak_ms[j]);
  timing_ok &= r.kernel_max_ms<5 && r.kernel_total_ms[1]<=7.0*batch/64;
 }
 stats(&alloc1,&deny1);ops->destroy(ctx);dlclose(lib);
 double effective=(65536.0*3*rb*batch*limit)/(total_sweep*1e9);
 bool pass=first>=0 && first<=3000 && timing_ok && effective>=0.5;
 printf("PDCCH_NID_STREAM_RESULT %s first_record_ms=%.3f max_kernel_ms=%.3f effective_tmac_s=%.4f late_alloc=%llu denied=%llu\n",pass?"PASS":"FAIL",first,max_kernel,effective,(unsigned long long)(alloc1-alloc0),(unsigned long long)(deny1-deny0));
 if(alloc0!=alloc1 || deny0!=deny1)return 5;
 return pass?0:6;
}
