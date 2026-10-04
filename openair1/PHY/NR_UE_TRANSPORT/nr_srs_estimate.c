/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_srs_blind_internal.h"
#include "PHY/nr_phy_common/inc/nr_phy_common.h"
#include "PHY/NR_TRANSPORT/nr_transport_proto.h"
#include "PHY/NR_ESTIMATION/nr_ul_estimation.h"
#include <math.h>
#include <string.h>

int nr_srs_estimate(nr_srs_blind_t *s, const int16_t *iq, int stride, nr_srs_candidate_t *c,
                    float *out, uint16_t *k_abs, uint8_t *l_sym)
{
  if (!s || !iq || !out || !k_abs || !l_sym || !nr_srs_valid_grid(&s->grid,stride)
      || !nr_srs_valid_candidate(&s->grid,c) || c->group<0 || c->group>=30
      || c->cs<0 || c->cs>=(c->comb==2?8:c->comb==4?12:6)) return 0;
  nr_srs_info_t info;
  if(!nr_srs_make_info(c,&info)) return 0;
  const int N=s->grid.fft_size, ns=c->n_symbols, nant=s->grid.n_ant, width=c->nb_rb*12;
  const int size=N*ns, nre=width*ns;
  nfapi_nr_srs_pdu_t pdu={.bwp_start=c->start_rb,.bwp_size=c->nb_rb,
    .num_symbols=ns==4?2:ns==2?1:0,.time_start_position=c->start_symbol,
    .comb_size=info.comb_size,.comb_offset=c->comb_offset,.cyclic_shift=c->cs,
    .config_index=info.C_SRS,.bandwidth_index=info.B_SRS,.sequence_id=c->group};
  pdu.srs_parameters_v4.num_ul_spatial_streams_ports=nant;
  memset(s->reference,0,4*N*sizeof(c16_t));
  if(!generate_srs_nr(s->fp,&s->reference,0,c->start_rb,&info,8192,0,0,1)) return 0;
  c16_t *rx[8];
  for(int a=0;a<nant;a++) rx[a]=(c16_t *)iq+a*stride;
  c16_t received[nant][size] __attribute__((aligned(32)));
  c16_t noise[nant][size] __attribute__((aligned(32)));
  memset(received,0,sizeof(received));memset(noise,0,sizeof(noise));
  /* Slot zero is deliberate: the adapter exposes just the captured slot, not an RU ring. */
  if(nr_get_srs_signal(s->gnb,rx,0,&pdu,&info,received,noise)<0) return 0;
  double noise_sum=0;unsigned noise_n=0;
  for(int a=0;a<nant;a++) {
    c16_t ls[size] __attribute__((aligned(32)));
    c16_t interpolated[size] __attribute__((aligned(32)));
    c16_t time[NR_SRS_IDFT_OVERSAMP_FACTOR*N] __attribute__((aligned(32)));
    c16_t shifted[NR_SRS_IDFT_OVERSAMP_FACTOR*N] __attribute__((aligned(32)));
    delay_t delay={0};
    nr_srs_ls_channel_estimation(a,0,N,s->grid.first_carrier_offset,ns,&pdu,&info,s->reference,received[a],ls,&delay);
    if(c->comb!=8) {
      uint32_t power=0;
      memset(interpolated,0,sizeof(interpolated));
      nr_srs_channel_interpolation(0,N,s->grid.first_carrier_offset,ns,&pdu,&info,ls,
        delay.est_delay,noise[a],interpolated,time,shifted,&power,s->fp->delay_table);
    }
    for(int l=0;l<ns;l++) for(int k=0;k<width;k++) {
      const int abs=c->start_rb*12+k, bin=(s->grid.first_carrier_offset+abs)%N;
      double re,im;
      if(c->comb==8) {
        /* Upstream has only comb-2/4 filters. Linear interpolation of its LS
         * pilots is bounded at both edges and preserves the measured phase. */
        int j=(k-c->comb_offset)/8;
        if(k<c->comb_offset) j=0;
        int np=width/8;
        if(j>=np) j=np-1;
        int j1=j+1<np?j+1:j;
        int b0=(s->grid.first_carrier_offset+c->start_rb*12+c->comb_offset+8*j)%N;
        int b1=(s->grid.first_carrier_offset+c->start_rb*12+c->comb_offset+8*j1)%N;
        double t=fmax(0,fmin(1,(k-c->comb_offset-8*j)/8.0));
        re=(1-t)*ls[l*N+b0].r+t*ls[l*N+b1].r;
        im=(1-t)*ls[l*N+b0].i+t*ls[l*N+b1].i;
      } else if(k<c->comb_offset) {
        /* The gNB filters start at the first pilot. Extend that estimate to
         * the leading allocation edge so every exported RE has a CFR. */
        int first=(s->grid.first_carrier_offset+c->start_rb*12+c->comb_offset)%N;
        re=ls[l*N+first].r;im=ls[l*N+first].i;
      } else {
        int centered=s->grid.first_carrier_offset-N/2+abs;
        re=interpolated[l*N+centered].r;im=interpolated[l*N+centered].i;
      }
      int i=l*width+k;
      out[2*(a*nre+i)]=re/8192.0;out[2*(a*nre+i)+1]=im/8192.0;
      if(a==0) {k_abs[i]=abs;l_sym[i]=c->start_symbol+l;}
      if((k-c->comb_offset+c->comb)%c->comb) {
        c16_t v=rx[a][(c->start_symbol+l)*N+bin];
        noise_sum+=(double)v.r*v.r+(double)v.i*v.i;noise_n++;
      }
    }
  }
  c->noise_var=noise_n?noise_sum/(noise_n*8192.0*8192.0):0;
  return nre;
}
