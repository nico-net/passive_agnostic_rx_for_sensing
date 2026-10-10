/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_pdcch_nid_stage1.h"
#include "nr_pdcch_nid_reference.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <vector>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);return 1;}} while(0)
int main(int argc,char **argv) {
 // Check bit-jump arithmetic against the independent 50-step recurrence.
 uint32_t random=2207;
 for(int k=0;k<65536;k++) {
  random^=random<<13;random^=random>>17;random^=random<<5;
  uint32_t a=0,b=random&0x7fffffff,c=0,d=0;
  CHECK(nid_gold(&a,&b,1)==nid_gold_start(&c,&d,random&0x7fffffff));
  for(int w=0;w<54;w++)CHECK(nid_gold(&a,&b,0)==nid_gold(&c,&d,0));
 }
 double spent=0;
 for(uint64_t look=1;look<=10000;look++) {
  double alpha=nid_look_alpha(look);spent+=alpha;
  for(double lam:{0.,0.001,0.1,1.,10.}) {
   int k=nid_threshold_alpha(lam,39,alpha);
   CHECK(39.*65536*nid_poisson_tail(lam,k)<alpha);
   CHECK(k==1 || 39.*65536*nid_poisson_tail(lam,k-1)>=alpha);
  }
 }
 CHECK(spent<0.01 && fabs(spent-0.01*(1-1./10001))<1e-14);
 puts("PASS Gold jump and repeated-look false-alarm budget");
 if(argc<2 || strcmp(argv[1],"--device"))return 0;
 int dur=argc>2?atoi(argv[2]):3;
 setenv("ISAC_PDCCH_NID_GPU_INCREMENTAL","1",1);setenv("ISAC_PDCCH_NID_MULTISYMBOL","1",1);
 void *lib=dlopen("libpdcch_nid_gpu.so",RTLD_NOW|RTLD_LOCAL);CHECK(lib);
 auto get=(const nr_pdcch_nid_ops_t*(*)())dlsym(lib,"nr_pdcch_nid_gpu_ops");
 auto read=(int(*)(void*,uint16_t*,float*))dlsym(lib,"nr_pdcch_nid_gpu_statistics_check");
 auto memory=(void(*)(uint64_t*,uint64_t*))dlsym(lib,"nr_dl_gpu_memory_stats");
 CHECK(get && read && memory && get()->abi==NR_PDCCH_NID_ABI);
 nr_pdcch_nid_batch_t src={},b={};nid_reference_make(6+dur,&src);
 auto *ops=get();nr_pdcch_nid_iq_t *staging=nullptr;void *ctx=ops->create(36,dur,2,&staging);CHECK(ctx);
 const int N=6*NID_CELLS,n=17;
 std::vector<uint16_t> h[2]={std::vector<uint16_t>(N),std::vector<uint16_t>(N)},got(N);
 std::vector<float> z[2]={std::vector<float>(N),std::vector<float>(N)},zs(N);
 uint64_t a0,d0,a1,d1;memory(&a0,&d0);
 b.cfg=src.cfg;b.count=n;b.iq=staging;
 for(int pass=0;pass<6;pass++) {
  // Different successive batches, partial size, multiple wraps, then config reset.
  if(pass==4){b.cfg.pci++;for(int r=0;r<2;r++){std::fill(h[r].begin(),h[r].end(),0);std::fill(z[r].begin(),z[r].end(),0);}}
  for(int k=0;k<n;k++) {
   int at=(pass*n+k)%336;b.slot[k]=src.slot[at];b.sym[k]=src.sym[at];
   memcpy(staging+(size_t)k*dur*432,src.iq+(size_t)at*dur*432,dur*432*sizeof(*staging));
   if(pass>=2)memset(staging+(size_t)k*dur*432,0,dur*432*sizeof(*staging));
  }
  nid_reference_batch_stats(&b,h[pass%2].data(),z[pass%2].data());
  nr_pdcch_nid_result_t r={},repeat={};CHECK(!ops->upload(ctx,&b) && !ops->run(ctx,&r));
  CHECK(!read(ctx,got.data(),zs.data()));
  for(int j=0;j<N;j++) {
   if(got[j]!=h[0][j]+h[1][j] || zs[j]!=(float)((double)z[0][j]+z[1][j])) {
    fprintf(stderr,"stats mismatch pass=%d cell=%d hits=%u/%u z=%g/%g\n",pass,j,got[j],h[0][j]+h[1][j],zs[j],(float)((double)z[0][j]+z[1][j]));return 1;
   }
  }
  int families=0;for(int d=0;d<dur;d++)families+=14-d;
  for(int d=0;d<dur;d++)for(int sym=0;sym<14-d;sym++) {
   unsigned sum=0;for(int id=0;id<65536;id++)sum+=got[(2*d+1)*NID_CELLS+sym*65536+id];
   double lambda=(double)sum/65536;
   CHECK(r.lambda[d][sym]==lambda);
   CHECK(r.threshold[d][sym]==nid_threshold_alpha(lambda,families,nid_look_alpha(pass+1)));
  }
  CHECK(!ops->run(ctx,&repeat) && repeat.count==r.count && !memcmp(repeat.records,r.records,r.count*sizeof(r.records[0])));
  CHECK(!read(ctx,got.data(),zs.data())); // diagnostic did not mutate resident totals
  for(int j=0;j<N;j++)CHECK(got[j]==h[0][j]+h[1][j]);
 }
 memory(&a1,&d1);CHECK(a0==a1 && d0==d1);ops->destroy(ctx);dlclose(lib);
 puts("PASS incremental GPU == independent CPU batch sums, expiry, wrap, config reset, idempotence, no late allocation");return 0;
}
