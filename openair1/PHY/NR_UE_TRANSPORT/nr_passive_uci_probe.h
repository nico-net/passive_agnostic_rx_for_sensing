/* OFFLINE ONLY inverse of a single rate-matched UCI component, rank one,
 * no PTRS. ACK starts after the first DMRS; CSI starts at the first non-DMRS
 * symbol. Combined ACK/CSI reservation layouts are not covered here. */
#ifndef NR_PASSIVE_UCI_PROBE_H
#define NR_PASSIVE_UCI_PROBE_H
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
static inline uint32_t nr_passive_uci_probe_demux(const int16_t *input, uint32_t bits,
    int16_t *output, unsigned rb, unsigned start, unsigned length, uint16_t dmrs,
    unsigned dmrs_re, unsigned qm, unsigned uci_re, bool csi)
{
  if(!input || !output || !rb || rb>275 || !length || start+length>14 ||
     !qm || qm>8 || (qm&1) || dmrs_re>12 || !dmrs) return 0;
  if(dmrs&~(((1u<<length)-1)<<start)) return 0;
  unsigned after=__builtin_ctz(dmrs)+1;
  while(after<start+length && (dmrs&(1u<<after))) ++after;
  if(csi) after=start;
  uint32_t expected=0,available=0;
  for(unsigned sym=start;sym<start+length;++sym) {
    unsigned re=rb*(12u-((dmrs&(1u<<sym))?dmrs_re:0u));
    expected+=re*qm;
    if(sym>=after && !(dmrs&(1u<<sym))) available+=re;
  }
  if(bits!=expected || uci_re>available) return 0;
  unsigned remaining=uci_re;
  uint32_t rd=0,wr=0;
  for(unsigned sym=start;sym<start+length;++sym) {
    bool has_dmrs=dmrs&(1u<<sym);
    unsigned re=rb*(12u-(has_dmrs?dmrs_re:0u));
    unsigned choose=(sym>=after && !has_dmrs)?(remaining<re?remaining:re):0;
    unsigned stride=choose?re/choose:1,next=0,removed=0;
    for(unsigned k=0;k<re;++k) {
      if(removed<choose && k==next) {++removed; next+=stride;}
      else {memmove(output+wr,input+rd,qm*sizeof(*output)); wr+=qm;}
      rd+=qm;
    }
    remaining-=removed;
  }
  return remaining?0:wr;
}
#endif
