#ifndef NR_PASSIVE_DELAY_H
#define NR_PASSIVE_DELAY_H
#include "PHY/nr_phy_common/inc/nr_phy_common.h"
#include <limits.h>

/* Timing-only block scaling, not gain control of the equalizer/channel estimate.
 * The unitary fixed-point IDFT can amplify a coherent input by sqrt(N). A strong
 * DMRS channel therefore overflows even though every input fits in int16_t.
 * Preserve the original LS samples; the temporary copy reserves 6 dB headroom.
 * Returned peak power is restored to input-domain units (/N), so independently
 * scaled antennas and repeated observations remain comparable. */
static inline void nr_passive_est_delay(int n, const c16_t *ls, c16_t *time, delay_t *delay)
{
  int root=1;
  while (root*root<n) ++root;
  int peak_l1=0;
  for(int k=0;k<n;++k) {
    int p=abs((int)ls[k].r)+abs((int)ls[k].i);
    if(p>peak_l1) peak_l1=p;
  }
  int shift=0;
  const int limit=16383/root;
  while((peak_l1>>shift)>limit) ++shift;
  c16_t scaled[n] __attribute__((aligned(32)));
  const int divisor=1<<shift;
  for(int k=0;k<n;++k) {
    scaled[k].r=ls[k].r/divisor;
    scaled[k].i=ls[k].i/divisor;
  }
  delay_t candidate={0};
  nr_est_delay(n,scaled,time,&candidate);
  const uint64_t restored=((uint64_t)candidate.delay_max_val<<(2*shift))/(unsigned)n;
  candidate.delay_max_val=restored>INT_MAX?INT_MAX:(int)restored;
  if(candidate.delay_max_val>=delay->delay_max_val) *delay=candidate;
}
#endif
