/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_srs_blind.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "PHY/nr_phy_common/inc/nr_phy_common.h"
#include "PHY/NR_REFSIG/ul_ref_seq_nr.h"
#include "nr_srs_blind_internal.h"
#include "PHY/INIT/nr_phy_init.h"
nr_srs_blind_t *nr_srs_blind_create(const nr_srs_grid_t *g)
{
  if (!nr_srs_valid_grid(g,g?14*g->fft_size:0)) return NULL;
  nr_srs_blind_t *s=calloc(1,sizeof(*s));
  if(s) {
    s->grid=*g;
    s->reference=calloc(4*g->fft_size,sizeof(c16_t));
    s->gnb=calloc(1,sizeof(*s->gnb));
    s->fp=s->gnb?&s->gnb->frame_parms:NULL;
    if(!s->reference || !s->fp) { nr_srs_blind_destroy(s); return NULL; }
    s->fp->ofdm_symbol_size=g->fft_size;
    s->fp->first_carrier_offset=g->first_carrier_offset;
    s->fp->symbols_per_slot=g->symbols_per_slot;
    s->fp->slots_per_frame=g->slots_per_frame;
    s->fp->nb_antennas_rx=g->n_ant;
    init_delay_table(g->fft_size,MAX_DELAY_COMP,NR_MAX_OFDM_SYMBOL_SIZE,s->fp->delay_table);
  }
  return s;
}
void nr_srs_blind_destroy(nr_srs_blind_t *s) { if(s) {free(s->reference);free(s->gnb);free(s);} }
bool nr_srs_valid_grid(const nr_srs_grid_t *g, int stride)
{
  return g && g->fft_size>=128 && g->fft_size<=4096 && g->n_rb>0 && g->n_rb<=275
      && g->n_rb*12<=g->fft_size && g->first_carrier_offset>=0 && g->first_carrier_offset<g->fft_size
      && g->n_ant>=1 && g->n_ant<=8 && g->symbols_per_slot==14 && stride>=14*g->fft_size;
}
int nr_srs_comb_detect(const nr_srs_grid_t *g,const int16_t *iq,int stride,uint16_t mask,nr_srs_candidate_t out[NR_SRS_MAX_CANDIDATES])
{
  if (!iq || !out || !nr_srs_valid_grid(g,stride)) return 0;
  unsigned char combs[6][275]={0}, offsets[6][275]={0};
  for (int l=8;l<14;l++) {
    if (!(mask & (1u<<l))) continue;
    for (int rb=0;rb<g->n_rb;rb++) {
      double energy[12]={0}, total=0;
      for (int k=0;k<12;k++) {
        int bin=(g->first_carrier_offset+rb*12+k)%g->fft_size;
        for(int a=0;a<g->n_ant;a++) {
          const int16_t *v=iq+2*(a*stride+l*g->fft_size+bin);
          energy[k]+=(double)v[0]*v[0]+(double)v[1]*v[1];
        }
        total+=energy[k];
      }
      if (total<16.0*12*g->n_ant) continue;
      for (int comb=8;comb>=2 && !combs[l-8][rb];comb/=2) for (int off=0;off<comb;off++) {
        double on=0;
        for(int k=0;k<12;k++) if((rb*12+k)%comb==off) on+=energy[k];
        if (on>0.85*total) { combs[l-8][rb]=comb; offsets[l-8][rb]=off; break; }
      }
    }
  }
  int count=0;
  for (int l=0;l<6;l++) for (int rb=0;rb<g->n_rb;) {
    const int comb=combs[l][rb], off=offsets[l][rb];
    if (!comb) { rb++; continue; }
    int end=rb+1;
    while(end<g->n_rb && combs[l][end]==comb && offsets[l][end]==off) end++;
    if(end-rb<4) {rb=end;continue;}
    int ns=1;
    for (int n=1;n<4 && l+n<6;n++) {
      bool same=true;
      for(int r=rb;r<end;r++) if(combs[l+n][r]!=comb || offsets[l+n][r]!=off) same=false;
      if(!same) break;
      ns++;
    }
    if(ns==3) ns=2;
    if(count==NR_SRS_MAX_CANDIDATES) return count;
    out[count++]=(nr_srs_candidate_t){.comb=comb,.comb_offset=(off-rb*12%comb+comb)%comb,
      .start_symbol=l+8,.n_symbols=ns,.start_rb=rb,.nb_rb=end-rb,.group=-1,.cs=-1,.attribution="anonymous"};
    for(int n=0;n<ns;n++) for(int r=rb;r<end;r++) combs[l+n][r]=0;
    rb=end;
  }
  return count;
}
/* Use BWP start for the observed absolute RB start: the upstream generator's
 * k_0_p is uint8_t, so placing a wide-band start in n_shift would wrap it. */
bool nr_srs_make_info(const nr_srs_candidate_t *c, nr_srs_info_t *info)
{
  *info=(nr_srs_info_t){.comb_size=c->comb==2?0:c->comb==4?1:2,.K_TC_overbar=c->comb_offset,
    .n_SRS_cs=c->cs,.n_ID_SRS=c->group,.l_offset=c->start_symbol,.N_symb_SRS=c->n_symbols,
    .R=1,.n_srs_ports=1,.resource_type=aperiodic};
  for(int b=0;b<4;b++) for(int v=0;v<64;v++) if(get_m_srs(v,b)==c->nb_rb) {
    info->C_SRS=v;info->B_SRS=b;info->b_hop=b;return true;
  }
  return false;
}
bool nr_srs_valid_candidate(const nr_srs_grid_t *g, const nr_srs_candidate_t *c)
{
  return c && (c->comb==2 || c->comb==4 || c->comb==8) && c->comb_offset>=0 && c->comb_offset<c->comb
    && c->start_rb>=0 && c->nb_rb>=4 && c->start_rb+c->nb_rb<=g->n_rb
    && (c->n_symbols==1 || c->n_symbols==2 || c->n_symbols==4)
    && c->start_symbol>=8 && c->start_symbol+c->n_symbols<=14;
}
/* Correlate after removing a constant phase slope (unknown UL arrival time).
 * CS/delay aliases are equivalent on a comb. Choose the representative with
 * the smallest residual delay; this is a convention, not a recovered UE TA. */
static double score_sequence(nr_srs_blind_t *s,const int16_t *iq,int stride,nr_srs_candidate_t *c)
{
  nr_srs_info_t info;
  if (!nr_srs_make_info(c,&info)) return 0;
  memset(s->reference,0,4*s->grid.fft_size*sizeof(c16_t));
  if(!generate_srs_nr(s->fp,&s->reference,0,c->start_rb,&info,8192,0,0,1)) return 0;
  const int n=c->nb_rb*12/c->comb, N=s->grid.fft_size;
  double best=0;int delay=0;
  for(int a=0;a<s->grid.n_ant;a++) {
    double zr[1632],zi[1632],energy=0,dr=0,di=0;
    for(int k=0;k<n;k++) {
      int bin=(s->grid.first_carrier_offset+c->start_rb*12+c->comb_offset+k*c->comb)%N;
      const int16_t *v=iq+2*(a*stride+c->start_symbol*N+bin);
      c16_t r=s->reference[bin];
      zr[k]=(double)v[0]*r.r+(double)v[1]*r.i;
      zi[k]=(double)v[1]*r.r-(double)v[0]*r.i;
      energy+=zr[k]*zr[k]+zi[k]*zi[k];
      if(k) {dr+=zr[k]*zr[k-1]+zi[k]*zi[k-1];di+=zi[k]*zr[k-1]-zr[k]*zi[k-1];}
    }
    double slope=atan2(di,dr), cr=0,ci=0;
    for(int k=0;k<n;k++) {
      double r=cos(slope*k),i=sin(slope*k);
      cr+=zr[k]*r+zi[k]*i;ci+=zi[k]*r-zr[k]*i;
    }
    double score=energy>0?(cr*cr+ci*ci)/(energy*n):0;
    if(score>best) {best=score;delay=lrint(-slope*N/(2*M_PI*c->comb));}
  }
  c->delay_samples=delay;
  return best;
}
bool nr_srs_group_cs_search(nr_srs_blind_t *s,const int16_t *iq,int stride,nr_srs_candidate_t *c,const nr_srs_candidate_t *hint)
{
  if(!s || !iq || !nr_srs_valid_grid(&s->grid,stride) || !nr_srs_valid_candidate(&s->grid,c)) return false;
  nr_srs_candidate_t best=*c;
  double best_rank=-1;
  const int ncs=c->comb==2?8:c->comb==4?12:6;
  for(int trial=-1;trial<30*ncs;trial++) {
    nr_srs_candidate_t test=*c;
    if(trial<0) {
      if(!hint || hint->comb!=c->comb || hint->comb_offset!=c->comb_offset || hint->nb_rb!=c->nb_rb) continue;
      test.group=hint->group;test.cs=hint->cs;
    } else {test.group=trial/ncs;test.cs=trial%ncs;}
    if(test.group<0 || test.group>=30 || test.cs<0 || test.cs>=ncs) continue;
    test.score=score_sequence(s,iq,stride,&test);
    double rank=test.score-1e-5*abs(test.delay_samples);
    if(rank>best_rank) {best_rank=rank;best=test;}
  }
  if(best.score<0.65) return false;
  *c=best;
  return true;
}
static unsigned srs_gcd(unsigned a, unsigned b)
{
  while (b) { unsigned rem=a%b; a=b; b=rem; }
  return a;
}
static bool same_resource(const nr_srs_candidate_t *a, const nr_srs_candidate_t *b)
{
  return a->comb==b->comb && a->comb_offset==b->comb_offset && a->start_rb==b->start_rb
      && a->nb_rb==b->nb_rb && a->start_symbol==b->start_symbol && a->n_symbols==b->n_symbols
      && a->group==b->group && a->cs==b->cs;
}
void nr_srs_observe(nr_srs_blind_t *s,nr_srs_candidate_t *c,uint64_t slot)
{
  if (!s || !c) return;
  srs_track_t *t=NULL, *oldest=&s->tracks[0];
  for (int i=0;i<NR_SRS_MAX_TRACKS;i++) {
    srs_track_t *p=&s->tracks[i];
    if (p->id && same_resource(&p->key,c)) { t=p; break; }
    if (!p->id || (oldest->id && p->last<oldest->last)) oldest=p;
  }
  if (!t) {
    t=oldest;
    *t=(srs_track_t){.key=*c,.first=slot,.last=slot,.id=++s->next_id,.count=1};
  } else if (slot<t->last || slot-t->last>10240) {
    *t=(srs_track_t){.key=*c,.first=slot,.last=slot,.id=++s->next_id,.count=1};
  } else if (slot>t->last) {
    unsigned gap=slot-t->last;
    t->gcd=t->gcd?srs_gcd(t->gcd,gap):gap;
    t->last=slot; t->count++;
  }
  c->track_id=t->id; c->period=0; c->slot_offset=0;
  static const unsigned periods[]={1,2,4,5,8,10,16,20,32,40,64,80,160,320,640,1280,2560};
  if (t->count>=3) for(unsigned i=0;i<sizeof(periods)/sizeof(*periods);i++)
    if(t->gcd==periods[i]) { c->period=t->gcd; c->slot_offset=t->first%t->gcd; break; }
}
