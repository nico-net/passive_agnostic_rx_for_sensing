/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_srs_blind_internal.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
/* All access is on the SRS consumer. Runtime producers post bounded evidence
 * events through a try-lock queue; this state never shares a receiver mutex. */
static srs_ue_evidence_t *evidence(nr_srs_blind_t *s,uint16_t rnti)
{
  srs_ue_evidence_t *old=&s->ues[0];
  for(int i=0;i<NR_SRS_MAX_TRACKS;i++) {
    srs_ue_evidence_t *e=&s->ues[i];
    if(e->rnti==rnti) return e;
    if(!e->rnti || (old->rnti && e->last<old->last)) old=e;
  }
  *old=(srs_ue_evidence_t){.rnti=rnti};return old;
}
void nr_srs_note_trigger(nr_srs_blind_t *s,uint16_t rnti,uint64_t slot,unsigned request)
{
  if(!s || !rnti || !request) return;
  srs_ue_evidence_t *e=evidence(s,rnti);e->trigger=slot;e->last=slot;e->trigger_valid=true;
}
void nr_srs_note_pusch(nr_srs_blind_t *s,uint16_t rnti,uint64_t slot,int delay,
                     const float *cfr,const uint16_t *k,int n_re)
{
  if(!s || !rnti) return;
  srs_ue_evidence_t *e=evidence(s,rnti);e->last=slot;e->pusch_slot=slot;e->delay=delay;e->pusch_valid=true;e->n=0;
  if(!cfr || !k || n_re<=0) return;
  for(int i=0;i<n_re && e->n<64;i++) {
    if(!isfinite(cfr[2*i]) || !isfinite(cfr[2*i+1])) continue;
    bool duplicate=false;for(int j=0;j<e->n;j++) if(e->k[j]==k[i]) duplicate=true;
    if(duplicate) continue;
    e->k[e->n]=k[i];e->h[2*e->n]=cfr[2*i];e->h[2*e->n+1]=cfr[2*i+1];e->n++;
  }
}
static double similarity(const srs_ue_evidence_t *e,const nr_srs_candidate_t *c,const float *cfr,const uint16_t *k,int n)
{
  if(!cfr || !k) return 0;
  double re=0,im=0,ep=0,es=0;int count=0;
  for(int j=0;j<e->n;j++) for(int i=0;i<n;i++) if(k[i]==e->k[j]) {
    double pr=e->h[2*j],pi=e->h[2*j+1],sr=cfr[2*i],si=cfr[2*i+1];
    if(c->delay_ambiguity_samples>0) {
      int aliases=lrint((e->delay-c->delay_samples)/c->delay_ambiguity_samples);
      int relative=(int)k[i]-c->start_rb*12-c->comb_offset;
      if(aliases && (relative<0 || relative%c->comb)) break;
      int ncs=c->comb==2?8:c->comb==4?12:6;
      double phase=2*M_PI*aliases*relative/(c->comb*ncs);
      double r=sr;sr=r*cos(phase)-si*sin(phase);si=r*sin(phase)+si*cos(phase);
    }
    re+=pr*sr+pi*si;im+=pr*si-pi*sr;ep+=pr*pr+pi*pi;es+=sr*sr+si*si;count++;break;
  }
  return count>=4 && ep>0 && es>0?(re*re+im*im)/(ep*es):0;
}
void nr_srs_attribute(nr_srs_blind_t *s,nr_srs_candidate_t *c,uint64_t slot,
                     const float *cfr,const uint16_t *k,int n_re)
{
  c->rnti=0;c->attribution="anonymous";c->delay_resolved=false;
  if(!s) return;
  uint16_t triggered=0,matched=0;int nt=0,nm=0;
  for(int i=0;i<NR_SRS_MAX_TRACKS;i++) {
    const srs_ue_evidence_t *e=&s->ues[i];if(!e->rnti) continue;
    double delta=e->delay-c->delay_samples;
    if(c->delay_ambiguity_samples>0) delta=remainder(delta,c->delay_ambiguity_samples);
    bool timing=e->pusch_valid && slot>=e->pusch_slot && slot-e->pusch_slot<=256 && fabs(delta)<=2;
    if(timing && e->trigger_valid && slot>=e->trigger && slot-e->trigger<=32) {triggered=e->rnti;nt++;}
    if(timing && similarity(e,c,cfr,k,n_re)>=0.9) {matched=e->rnti;nm++;}
  }
  /* Conflicting evidence is a reason to remain anonymous, not to pick a winner. */
  if(nt==1 && nm<=1 && (!nm || matched==triggered)) {c->rnti=triggered;c->attribution="dci_request";}
  else if(nt==0 && nm==1) {c->rnti=matched;c->attribution="pusch_delay_cfr";}
  c->delay_resolved=false;
  if(c->rnti && c->delay_ambiguity_samples>0) {
    const srs_ue_evidence_t *e=evidence(s,c->rnti);
    int aliases=lrint((e->delay-c->delay_samples)/c->delay_ambiguity_samples);
    int ncs=c->comb==2?8:c->comb==4?12:6;
    c->cs=((c->cs-aliases)%ncs+ncs)%ncs;
    c->residual_delay-=aliases*c->delay_ambiguity_samples;
    c->delay_samples+=lrint(aliases*c->delay_ambiguity_samples);
    c->delay_resolved=true;
  }
}
