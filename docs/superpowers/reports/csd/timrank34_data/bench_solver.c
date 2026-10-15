#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "gj.h"
#include "../../../../../openair1/PHY/NR_UE_TRANSPORT/nr_mmse_exact.h" /* gcc -O2 -mcpu=native bench_solver.c -lm */
static double now(){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+1e-9*t.tv_nsec;}
int main(){
  enum{N=4096}; static int16_t hr[N][4][4],hi[N][4][4],yr[N][4],yi[N][4];
  srand(1);
  for(int n=0;n<N;n++){for(int l=0;l<4;l++)for(int a=0;a<4;a++){hr[n][l][a]=rand()%41-20;hi[n][l][a]=rand()%41-20;} for(int a=0;a<4;a++){yr[n][a]=rand()%81-40;yi[n][a]=rand()%81-40;}}
  for(int nl=3;nl<=4;nl++){
    double xr[4],xi[4],ai[4],r,acc=0,maxd=0; double t0=now();
    for(int it=0;it<50;it++) for(int n=0;n<N;n++){gj_re(nl,4,hr[n],hi[n],yr[n],yi[n],1.0/32,nl==4?5.0:0.0,xr,xi,ai,&r);acc+=xr[0]+ai[nl-1];}
    double t1=now();
    for(int it=0;it<50;it++) for(int n=0;n<N;n++){nr_mmse_exact_re(nl,4,hr[n],hi[n],yr[n],yi[n],1.0/32,nl==4?5.0:0.0,xr,xi,ai,&r);acc+=xr[0]+ai[nl-1];}
    double t2=now();
    for(int n=0;n<N;n++){double a1[4],b1[4],c1[4],a2[4],b2[4],c2[4],r1,r2;
      int o1=gj_re(nl,4,hr[n],hi[n],yr[n],yi[n],1.0/32,2.0,a1,b1,c1,&r1),o2=nr_mmse_exact_re(nl,4,hr[n],hi[n],yr[n],yi[n],1.0/32,2.0,a2,b2,c2,&r2);
      if(o1&&o2) for(int l=0;l<nl;l++){double d=fabs(a1[l]-a2[l])+fabs(b1[l]-b2[l])+fabs(c1[l]-c2[l])/fabs(c1[l]); if(d>maxd)maxd=d;} if(o1&&o2&&fabs(r1-r2)>1e-6*(1+r1)) maxd=1e9;}
    printf("nl=%d GJ %.3f us/RE  Cholesky %.3f us/RE  max diff %.2e (%g)\n",nl,(t1-t0)/(50.0*N)*1e6,(t2-t1)/(50.0*N)*1e6,maxd,acc);
  }
}
