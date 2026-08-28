#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
/* Sizes taken from nr_pdsch_passive_decode.c at 273 PRB / 4 rx / Nl=1:
   rxdataF_comp, dl_ch_mag, dl_ch_magb, dl_ch_magr = 14*4*3280*4 B each; chest = 4*57344*4;
   plus the UL FEP scratch = 4*57344*4. */
int main(void){
  const size_t s3 = (size_t)14*4*3280*4, sc = (size_t)4*57344*4, sl = 300000;
  const int N = 2000;
  double t0=now();
  for(int i=0;i<N;i++){
    void *a=aligned_alloc(32,s3),*b=aligned_alloc(32,s3),*c=aligned_alloc(32,s3),
         *d=aligned_alloc(32,s3),*e=aligned_alloc(32,sc),*f=aligned_alloc(32,sl);
    memset(a,0,s3);memset(b,0,s3);memset(c,0,s3);memset(d,0,s3);memset(e,0,sc);memset(f,0,sl);
    /* touch so nothing is optimised away */
    ((char*)a)[0]=1;((char*)f)[0]=1;
    free(a);free(b);free(c);free(d);free(e);free(f);
  }
  double t1=now();
  printf("DL-decode-shaped alloc+clear+free: %.1f us/iter  (%.2f MB/iter)\n",
         (t1-t0)/N*1e6,(4.0*s3+sc+sl)/1048576.0);
  /* the persistent-buffer alternative: allocate once, clear each iteration */
  void *A=aligned_alloc(32,s3),*B=aligned_alloc(32,s3),*C=aligned_alloc(32,s3),
       *D=aligned_alloc(32,s3),*E=aligned_alloc(32,sc),*F=aligned_alloc(32,sl);
  memset(A,0,s3);memset(B,0,s3);memset(C,0,s3);memset(D,0,s3);memset(E,0,sc);memset(F,0,sl);
  t0=now();
  for(int i=0;i<N;i++){
    memset(A,0,s3);memset(B,0,s3);memset(C,0,s3);memset(D,0,s3);memset(E,0,sc);memset(F,0,sl);
    ((char*)A)[0]=1;((char*)F)[0]=1;
  }
  t1=now();
  printf("same buffers reused, clear only:   %.1f us/iter\n",(t1-t0)/N*1e6);
  /* UL FEP scratch alone: 4 * samples_per_slot_wCP * sizeof(c16_t) */
  t0=now();
  for(int i=0;i<N;i++){void*p=aligned_alloc(32,sc);memset(p,0,sc);((char*)p)[0]=1;free(p);}
  t1=now();
  printf("UL FEP scratch alloc+clear+free:   %.1f us/iter (%.2f MB)\n",(t1-t0)/N*1e6,sc/1048576.0);
  return 0;
}
