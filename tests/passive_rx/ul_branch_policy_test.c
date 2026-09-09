#include <assert.h>
#include <stdio.h>
#include "openair1/PHY/NR_TRANSPORT/nr_ulsch_passive_branch.h"

int main(void)
{
  assert(nr_ulsch_passive_branch_selection(false,4,"0","2")==-1);
  assert(nr_ulsch_passive_branch_selection(true,1,"0","2")==-1);
  assert(nr_ulsch_passive_branch_selection(true,4,NULL,NULL)==0);
  assert(nr_ulsch_passive_branch_selection(true,4,NULL,"0")==0);
  assert(nr_ulsch_passive_branch_selection(true,4,NULL,"2")==-1);
  assert(nr_ulsch_passive_branch_selection(true,2,NULL,"2")==-1);
  assert(nr_ulsch_passive_branch_selection(true,4,"0","2")==0);
  assert(nr_ulsch_passive_branch_selection(true,4,"3","2")==3);
  assert(nr_ulsch_passive_branch_selection(true,4,"-1","0")==-1);
  assert(nr_ulsch_passive_branch_selection(true,4,"4","2")==-1);
  puts("UL branch policy: 10 assertions PASS (policy only, not RF performance)");
}
