#include <assert.h>
#include <stdio.h>
#include "openair1/PHY/NR_UE_TRANSPORT/nr_passive_uci_probe.h"
int main(void)
{
  int16_t in[72],out[72];
  for(int i=0;i<72;++i) in[i]=i;
  const int removed[2][6]={{24,25,32,33,40,41},{0,1,8,9,16,17}};
  for(int csi=0;csi<2;++csi) {
    assert(nr_passive_uci_probe_demux(in,72,out,1,0,4,2,12,2,3,csi)==66);
    int wr=0;
    for(int i=0;i<72;++i) {
      bool skip=false;
      for(int j=0;j<6;++j) if(i==removed[csi][j]) skip=true;
      if(!skip) assert(out[wr++]==i);
    }
    assert(wr==66);
  }
  assert(nr_passive_uci_probe_demux(in,72,out,1,0,4,2,12,2,0,false)==72);
  assert(!memcmp(in,out,sizeof(in)));
  out[0]=-99;
  assert(nr_passive_uci_probe_demux(in,71,out,1,0,4,2,12,2,3,false)==0);
  assert(out[0]==-99);
  assert(nr_passive_uci_probe_demux(in,72,in,1,0,4,2,12,2,3,false)==66);
  assert(in[24]==26 && in[30]==34 && in[36]==42);
  puts("UCI single-component probe: exact ACK/CSI positions, no-UCI, invalid input and in-place PASS");
}
