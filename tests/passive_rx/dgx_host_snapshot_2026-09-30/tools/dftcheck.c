#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "PHY/TOOLS/tools_defs.h"
#include "common/config/config_userapi.h"
#include "common/utils/LOG/log.h"
configmodule_interface_t *uniqCfg = NULL;
void exit_function(const char *file, const char *function, const int line, const char *s, const int assert) { exit(1); }
static void ref(const int16_t *x, double *yr, double *yi, int n, int inv){
  for (int k=0;k<n;k++){double sr=0,si=0; for(int t=0;t<n;t++){double a=(inv?2:-2)*M_PI*(double)k*t/n; double c=cos(a),s=sin(a); sr+=x[2*t]*c-x[2*t+1]*s; si+=x[2*t]*s+x[2*t+1]*c;} yr[k]=sr; yi[k]=si;}
}
int main(int argc,char**argv){
  char *av[]={"x","-O","cmdlineonly::dbgl0"}; uniqCfg=load_configmodule(3,av,CONFIG_ENABLECMDLINEONLY); logInit();
  load_dftslib();
  int sizes[]={128,256,512,1024,1536,2048,3072,4096};
  for(int si=0;si<8;si++){int n=sizes[si];
    for(int inv=0;inv<2;inv++){
    int16_t *x=aligned_alloc(64,4*n+64),*y=aligned_alloc(64,4*n+64); double *rr=malloc(8*n),*ri=malloc(8*n);
    srand(1); for(int i=0;i<2*n;i++) x[i]=(rand()%2001)-1000;
    if(!inv) dft(get_dft(n),x,y,1); else idft(get_idft(n),x,y,1);
    ref(x,rr,ri,n,inv);
    /* best real scale by LS */
    double num=0,den=0; for(int k=0;k<n;k++){num+=y[2*k]*rr[k]+y[2*k+1]*ri[k]; den+=rr[k]*rr[k]+ri[k]*ri[k];}
    double g=num/den, e=0,p=0; int bad=0;
    for(int k=0;k<n;k++){double er=y[2*k]-g*rr[k], ei=y[2*k+1]-g*ri[k]; double pk=g*g*(rr[k]*rr[k]+ri[k]*ri[k]); e+=er*er+ei*ei; p+=pk; if(er*er+ei*ei>0.01*pk+4) bad++;}
    printf("%s n=%5d scale=%.5f (1/sqrt(n)=%.5f) SQNR=%.1f dB bins_off=%d/%d\n",inv?"IDFT":"DFT ",n,g,1/sqrt(n),10*log10(p/e),bad,n);
  }}
  return 0;
}
