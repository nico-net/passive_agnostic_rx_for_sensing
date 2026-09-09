#include <assert.h>
#include <stdio.h>
#include "openair1/PHY/NR_TRANSPORT/nr_ulsch_passive_timing.h"
int main(void)
{
  const int p[]={90000,10,30,50000};
  assert(nr_ulsch_passive_timing_branch(4,-1,p)==0);
  assert(nr_ulsch_passive_timing_branch(4,3,p)==3);
  assert(nr_ulsch_passive_timing_branch(0,-1,p)==-1);
  assert(nr_ulsch_passive_timing_branch(4,-1,NULL)==-1);
  const int swapped[]={10,50000,90000,30};
  assert(nr_ulsch_passive_timing_branch(4,-1,swapped)==2);
  const int tie[]={100,100};
  assert(nr_ulsch_passive_timing_branch(2,-1,tie)==0);
  puts("UL timing selection: 6 assertions PASS (policy only, not timing-estimator accuracy)");
}
