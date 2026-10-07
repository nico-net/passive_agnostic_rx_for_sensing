/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#pragma once
#include "nr_pdcch_nid_algorithm.h"

// Prefix arithmetic follows the CPU oracle exactly (the old GPU sliding sum
// did not). --fmad=false, precise div/sqrt, no fast-math preserve decisions.
NID_FN inline float nid_best_symbol(const NidView &v,int k,uint32_t seed,int lo,int hi,int exlo,int exhi,int m0,int d=0) {
 // Keep the last six PREFIX values, not a sliding sum: subtraction and
 // rounding are identical to the CPU's full prefix arrays, with no large spills.
 float ar[6]={},ai[6]={},ap[6]={},sr=0,si=0,sp=0,b=0;
 const c16_t *re=v.grid(k,d);
 // Sequence words are consumed monotonically: no per-thread 54-word local array.
 uint32_t x1=0,x2=seed,word=0;int current=-1;
 for(int c=lo;c<hi;c++) {
  float r=0,i=0,p=0;
  for(int q=0;q<3;q++) {
   int m=3*(c-m0)+q;
   while(current<m/16) {word=nid_gold(&x1,&x2,current<0);current++;}
   // Same conjugated QPSK mapping as idsw_pilot, without a local-memory lookup.
   unsigned bits=(word>>(2*(m%16)))&3;
   float xr=(bits&1)?-1.f:1.f,xi=(bits&2)?1.f:-1.f;
   c16_t y=re[v.point_a_sc+12*c+1+4*q];
   r+=y.r*xr-y.i*xi; i+=y.r*xi+y.i*xr;
   p+=(float)y.r*y.r+(float)y.i*y.i;
  }
  int j=c-lo;
#ifdef __CUDACC__
  #pragma unroll
#endif
  for(int t=0;t<5;t++) {ar[t]=ar[t+1];ai[t]=ai[t+1];ap[t]=ap[t+1];}
  ar[5]=sr;ai[5]=si;ap[5]=sp;
  sr+=r;si+=i;sp+=p;
  if(j<5)continue;
  int first=c-5;if(exhi>exlo && first<exhi && first+6>exlo)continue;
  float wr=sr-ar[0],wi=si-ai[0],wp=sp-ap[0];
  float value=wp>0?sqrtf((wr*wr+wi*wi)/(wp*36.f)):0;
  if(value>b)b=value;
 }
 return b;
}
