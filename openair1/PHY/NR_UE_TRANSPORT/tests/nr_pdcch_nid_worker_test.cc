/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_pdcch_nid_reference.h"
#include <vector>
#include <thread>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
extern "C" {
#include "common/config/config_userapi.h"
#include "common/utils/LOG/log.h"
configmodule_interface_t *uniqCfg=nullptr;
void exit_function(const char *,const char *,int,const char *,int) {abort();}
}
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);return 1;}} while(0)
static void wait_ms(int n) {std::this_thread::sleep_for(std::chrono::milliseconds(n));}
int main(int argc,char **argv) {
 bool device=argc>1 && !strcmp(argv[1],"--device");
 bool incremental=argc>1 && !strcmp(argv[1],"--incremental");
 int batch_size=incremental?64:336;
 int dur=incremental?3:device?1:(argc>1?atoi(argv[1]):1);
 logInit();
 unsetenv("ISAC_CSIRS_DISC");
 if(incremental)setenv("ISAC_CSIRS_DISC","1",1);
 unsetenv("ISAC_PDCCH_NID_GPU");
 nr_pdcch_nid_record_t records[16],sentinel[16];memset(records,0xa5,sizeof(records));memcpy(sentinel,records,sizeof(records));
 uint64_t generation=0;
 CHECK(nr_pdcch_nid_gpu_start(36)==0 && !nr_pdcch_nid_gpu_running());
 CHECK(nr_pdcch_nid_gpu_records(records,16,&generation)==-1 && !memcmp(records,sentinel,sizeof(records)) && generation==0);
 nr_pdcch_nid_batch_t b={};nr_pdcch_nid_result_t expected={};
 if(device) {setenv("ISAC_CSIRS_DISC","1",1);setenv("ISAC_PDCCH_NID_GPU_BATCH","336",1);nid_reference_make(7,&b);nid_reference_run(1,&expected);}
 else b.cfg={432,0,0,6,2,14,dur};
 setenv("ISAC_PDCCH_NID_GPU","1",1);char duration[8];snprintf(duration,sizeof(duration),"%d",dur);setenv("ISAC_COREMAP_IDSWEEP_DUR",duration,1);
 setenv("ISAC_PDCCH_NID_GPU_WINDOW",device?"1":"2",1);setenv("ISAC_PDCCH_NID_GPU_EVERY","2",1);
 CHECK(nr_pdcch_nid_gpu_start(36)==0 && nr_pdcch_nid_gpu_running());
 void *lib=dlopen("libpdcch_nid_gpu.so",RTLD_NOW|RTLD_LOCAL);CHECK(lib);
 auto uploaded=(int(*)())dlsym(lib,"nid_fake_uploaded");
 auto errors=(int(*)())dlsym(lib,"nid_fake_errors");
 auto pause=(void(*)(int))dlsym(lib,"nid_fake_pause");
 auto runs=(int(*)())dlsym(lib,"nid_fake_runs");
 if(!device)CHECK(uploaded && errors && pause && runs);
 std::vector<nr_pdcch_nid_iq_t> fft(512*dur);
 auto push_batch=[&](bool retry) {
  for(int k=0;k<batch_size;k++) {
   for(int d=0;d<dur;d++)for(int i=0;i<432;i++)fft[d*512+(499+i)%512]=device?b.iq[(size_t)k*432+i]:nr_pdcch_nid_iq_t{(int16_t)(k+d+i),(int16_t)(-k-d-i)};
   int rc;
   do {
    rc=nr_pdcch_nid_gpu_push(fft.data(),512,499,device?b.slot[k]:k/14,device?b.sym[k]:k%(incremental?14:15-dur),&b.cfg);
    if(rc && retry)wait_ms(1);
   }while(rc && retry);
   if(rc)return false;
  }
  return true;
 };
 int total=device?3:4;
 for(int pass=1;pass<=total;pass++) {
  CHECK(push_batch(true));
  bool due=incremental || device || pass==2 || pass==4;
  if(due) {
   int n=-1;
   for(int attempt=0;attempt<30000 && n<0;attempt++) {wait_ms(10);n=nr_pdcch_nid_gpu_records(records,16,&generation);}
   CHECK(n>=0);
   if(device)CHECK(n==expected.count && !memcmp(records,expected.records,n*sizeof(records[0])));
   else CHECK(n==1 && records[0].nid==12000+pass);
  } else if(!device) {
   for(int t=0;t<1000 && uploaded()<pass;t++)wait_ms(1);
   CHECK(uploaded()==pass);CHECK(nr_pdcch_nid_gpu_records(records,16,&generation)==-1);
  }
 }
 if(!device) {
  CHECK(errors()==0 && runs()==(incremental?total:2));
  // Hold worker ownership, fill remaining staging entries, verify bounded drop.
  pause(1);CHECK(push_batch(true));CHECK(push_batch(true));CHECK(push_batch(true));
  CHECK(!push_batch(false));pause(0);
  for(int t=0;t<2000 && uploaded()<7;t++)wait_ms(1);
  CHECK(uploaded()==7 && errors()==0);
  b.cfg.pci=3;CHECK(push_batch(true));
  for(int t=0;t<2000 && uploaded()<8;t++)wait_ms(1);
  CHECK(uploaded()==8);
  if(incremental) {
   int n=-1;for(int t=0;t<2000;t++){wait_ms(1);n=nr_pdcch_nid_gpu_records(records,16,&generation);if(n==1)break;}
   CHECK(n==1 && records[0].nid==12008);
  } else CHECK(nr_pdcch_nid_gpu_records(records,16,&generation)==0);
  CHECK(push_batch(true));
  int n=-1;
  for(int t=0;t<2000 && n<0;t++){wait_ms(1);n=nr_pdcch_nid_gpu_records(records,16,&generation);}
  CHECK(n==1 && records[0].nid==12009);
 }
 nr_pdcch_nid_gpu_stop();CHECK(!nr_pdcch_nid_gpu_running());dlclose(lib);
 puts(device?"PASS synthetic GPU push -> worker -> records":"PASS host transport: off identity, wrap, cadence, single SCHED_OTHER core, overflow, shutdown");return 0;
}
