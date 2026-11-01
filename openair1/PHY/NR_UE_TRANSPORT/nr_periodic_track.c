/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_periodic_track.h"
#include <string.h>
/* TS 38.331 periodicities expressed in slots, per resource type. SRS: SRS-PeriodicityAndOffset 1..2560; CSI-RS/TRS:
 * CSI-ResourcePeriodicityAndOffset 4..640; PUCCH: SR/CSI report periods; CG/SPS: ConfiguredGrantConfig / SPS-Config periods. */
static const unsigned srs_p[]={1,2,4,5,8,10,16,20,32,40,64,80,160,320,640,1280,2560};
static const unsigned csirs_p[]={4,5,8,10,16,20,32,40,64,80,160,320,640};
static const unsigned pucch_p[]={1,2,4,5,8,10,16,20,32,40,64,80,160,320,640};
static const unsigned cg_p[]={1,2,4,5,8,10,16,20,32,40,64,80,128,160,320,640};
unsigned nr_ptrack_period_set(nr_ptrack_type_t type,const unsigned **p)
{
  switch(type) {
  case NR_PTRACK_SRS: *p=srs_p;return sizeof(srs_p)/sizeof(*srs_p);
  case NR_PTRACK_CSIRS: *p=csirs_p;return sizeof(csirs_p)/sizeof(*csirs_p);
  case NR_PTRACK_PUCCH: *p=pucch_p;return sizeof(pucch_p)/sizeof(*pucch_p);
  case NR_PTRACK_CG: *p=cg_p;return sizeof(cg_p)/sizeof(*cg_p);
  default: *p=NULL;return 0;
  }
}
bool nr_ptrack_estimate(nr_ptrack_type_t type,const uint64_t *hits,unsigned n,unsigned *period,unsigned *offset)
{
  const unsigned *set;unsigned np=nr_ptrack_period_set(type,&set);
  if(!hits || n<NR_PTRACK_MIN_HITS || n>NR_PTRACK_HITS) return false;
  for(unsigned pi=np;pi-->0;) {
    const unsigned P=set[pi];
    unsigned best=0,best_res=0;
    for(unsigned i=0;i<n;i++) {
      const unsigned r=hits[i]%P;unsigned cnt=0;
      for(unsigned j=0;j<n;j++) cnt+=hits[j]%P==r;
      if(cnt>=best) {best=cnt;best_res=r;} /* ties go to the later detection */
    }
    if(best<NR_PTRACK_MIN_HITS || 5*best<4*n) continue;
    /* occasion rate over the longest recent run of matching detections: a pattern that became denser
     * (e.g. 20 -> 10) must not be held back by the sparse history before it */
    uint64_t m[NR_PTRACK_HITS];unsigned k=0;
    for(unsigned i=0;i<n;i++) if(hits[i]%P==best_res) m[k++]=hits[i];
    bool dense=false;
    const unsigned need=NR_PTRACK_MIN_HITS>(n+1)/2?NR_PTRACK_MIN_HITS:(n+1)/2; /* and at least half of all detections: stray detections must not fake a finer grid */
    for(unsigned i=0;i+need<=k && !dense;i++) {
      const uint64_t occasions=(m[k-1]-m[i])/P+1;
      dense=4*(uint64_t)(k-i)>=3*occasions;
    }
    if(!dense) continue;
    *period=P;*offset=best_res;
    return true;
  }
  return false;
}
void nr_ptrack_init(nr_ptrack_t *t){memset(t,0,sizeof(*t));}
bool nr_ptrack_due(const nr_ptrack_t *t,uint64_t slot)
{
  return t->confirmed && t->n && slot>t->hits[t->n-1] && slot-t->hits[t->n-1]<=NR_PTRACK_EXPIRY && slot%t->period==t->offset;
}
uint64_t nr_ptrack_next(const nr_ptrack_t *t,uint64_t slot)
{
  if(!t->confirmed) return 0;
  uint64_t s=slot+1;
  return s+((t->offset+t->period-s%t->period)%t->period);
}
static void demote(nr_ptrack_t *t,nr_ptrack_counters_t *c)
{
  if(t->confirmed && c) c->demoted++;
  nr_ptrack_init(t);
}
void nr_ptrack_hit(nr_ptrack_t *t,nr_ptrack_type_t type,uint64_t slot,nr_ptrack_counters_t *c)
{
  if(t->n && slot==t->hits[t->n-1]) return;
  if(t->n && (slot<t->hits[t->n-1] || slot-t->hits[t->n-1]>NR_PTRACK_EXPIRY)) demote(t,c);
  const bool due=nr_ptrack_due(t,slot);
  if(due && c) {c->predicted++;c->hit++;}
  else if(t->confirmed && c) c->audit_hit++;
  if(due) {t->miss_run=0;t->outcomes=(t->outcomes<<1)|1u;if(t->n_outcomes<32) t->n_outcomes++;}
  if(t->n==NR_PTRACK_HITS) {memmove(t->hits,t->hits+1,(NR_PTRACK_HITS-1)*sizeof(*t->hits));t->n--;}
  t->hits[t->n++]=slot;
  unsigned p,o;
  if(nr_ptrack_estimate(type,t->hits,t->n,&p,&o)) {
    if(!t->confirmed && c) c->confirmed++;
    t->confirmed=true;t->period=p;t->offset=o;
  }
  /* A detection that fits no pattern keeps the learned one: a stray detection cannot break a periodic resource,
   * and a real change shows up as misses of the predicted occasions (nr_ptrack_miss). */
}
void nr_ptrack_miss(nr_ptrack_t *t,nr_ptrack_type_t type,uint64_t slot,nr_ptrack_counters_t *c)
{
  (void)type;
  if(!nr_ptrack_due(t,slot)) return;
  if(c) {c->predicted++;c->miss++;}
  t->miss_run++;t->outcomes<<=1;if(t->n_outcomes<32) t->n_outcomes++;
  /* demote on four misses in a row, or when a full 16-occasion window kept at most a quarter of its occasions */
  const uint32_t win=t->outcomes&0xffffu;
  if(t->miss_run>=4 || (t->n_outcomes>=16 && __builtin_popcount(win)<=4)) demote(t,c);
}
