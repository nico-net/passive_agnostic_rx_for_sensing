/* Offline analytic timing contract against the actual production estimator. */
#include "PHY/nr_phy_common/inc/nr_phy_common.h"
#include "PHY/nr_phy_common/inc/nr_passive_delay.h"
static int replay_delay_contract(void)
{
  enum {N=4096,ACTIVE=3276};
  c16_t ls[N] __attribute__((aligned(32))),time[N] __attribute__((aligned(32)));
  const int amplitudes[]={16,128,512,2048,8192};
  const int shifts[]={0,-8,13};
  unsigned failed=0;
  for(unsigned s=0;s<sizeof(shifts)/sizeof(*shifts);++s)
    for(unsigned a=0;a<sizeof(amplitudes)/sizeof(*amplitudes);++a) {
      memset(ls,0,sizeof(ls));
      for(int k=0;k<ACTIVE;++k) {
        const double phase=-2.0*M_PI*k*shifts[s]/N;
        ls[k].r=lrint(amplitudes[a]*cos(phase));
        ls[k].i=lrint(amplitudes[a]*sin(phase));
      }
      delay_t d={0};
      nr_passive_est_delay(N,ls,time,&d);
      const bool ok=abs(d.est_delay-shifts[s])<=1;
      failed+=!ok;
      printf("DELAY-CONTRACT amplitude=%d expected=%d measured=%d peak=%d %s\n",
             amplitudes[a],shifts[s],d.est_delay,d.delay_max_val,ok?"PASS":"FAIL");
    }
  printf("DELAY-CONTRACT failures=%u/15; synthetic only, no radio\n",failed);
  /* Evidence accumulation must compare original peak strengths even when the
   * two transforms used different headroom shifts. Reverse observation order
   * to expose accidental comparisons of independently normalized peaks. */
  for(int order=0;order<2;++order) {
    delay_t d={0};
    for(int observation=0;observation<2;++observation) {
      const bool strong=(observation==order);
      const int amplitude=strong?8192:128,expected=strong?-8:13;
      memset(ls,0,sizeof(ls));
      for(int k=0;k<ACTIVE;++k) {
        const double phase=-2.0*M_PI*k*expected/N;
        ls[k].r=lrint(amplitude*cos(phase)); ls[k].i=lrint(amplitude*sin(phase));
      }
      nr_passive_est_delay(N,ls,time,&d);
    }
    failed+=d.est_delay!=-8;
    printf("DELAY-ACCUMULATOR order=%d expected=-8 measured=%d %s\n",order,d.est_delay,d.est_delay==-8?"PASS":"FAIL");
  }
  return failed?2:0;
}
