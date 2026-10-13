#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>
static __attribute__((optimize("O3"))) void tinterp_row(const int16_t *restrict ha, const int16_t *restrict hb, int16_t *restrict hm, uint32_t n, float t)
{
  for (uint32_t k = 0; k < n; k++) {
    float v = (float)ha[k] + t * ((float)hb[k] - (float)ha[k]);
    v = fminf(fmaxf(v + copysignf(0.5f, v), -32768.f), 32767.f); /* branch-free: vectorises */
    hm[k] = (int16_t)v;
  }
}
int main(){
  enum{N=4096,S=14,R=4}; int16_t *e=malloc(sizeof(int16_t)*2*N*S*R); for(int i=0;i<2*N*S*R;i++) e[i]=(rand()%60000)-30000;
  struct timespec a,b; clock_gettime(CLOCK_MONOTONIC,&a); long bad=0;
  for(int it=0;it<1000;it++) for(int r=0;r<R;r++) for(int m=3;m<14;m++){ if(m==11) continue; int16_t*row=e+(size_t)r*S*2*N; tinterp_row(row+2*N*2,row+2*N*11,row+2*N*m,2*2604,(m-2)/9.f);}  
  clock_gettime(CLOCK_MONOTONIC,&b); printf("per decode (4 rx, 10 sym, 217 PRB): %.1f us\n",((b.tv_sec-a.tv_sec)*1e9+(b.tv_nsec-a.tv_nsec))/1000/1000.0);
  /* exactness vs lround(double) */
  for(int k=0;k<2*N;k++){ int16_t ha=e[k],hb=e[k+2*N]; for(int m=0;m<14;m++){ double t=(m-2)/9.0; long x=lround(ha+t*((double)hb-ha)); if(x>32767)x=32767; if(x<-32768)x=-32768; int16_t o; tinterp_row(&ha,&hb,&o,1,(float)((m-2)/9.f)); if(o!=x) bad++; }}
  printf("mismatch vs double lround: %ld of %d\n",bad,2*N*14); return 0; }
