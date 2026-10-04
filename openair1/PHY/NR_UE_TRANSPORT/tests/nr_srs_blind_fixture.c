/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_srs_blind.h"
#include "PHY/nr_phy_common/inc/nr_phy_common.h"
#include "PHY/NR_REFSIG/ul_ref_seq_nr.h"
#include "common/utils/LOG/log.h"
#include "common/config/config_userapi.h"
configmodule_interface_t *uniqCfg;
void exit_function(const char *file,const char *function,int line,const char *message,int fatal)
{ abort(); }
void srs_test_init(void)
{
  logInit();
  load_dftslib();
  InitSinLUT();
  generate_ul_reference_signal_sequences(32767);
}
bool srs_test_generate(const nr_srs_grid_t *g, const nr_srs_candidate_t *c, int16_t *out)
{
  NR_DL_FRAME_PARMS fp = {.ofdm_symbol_size=g->fft_size, .first_carrier_offset=g->first_carrier_offset,
    .symbols_per_slot=g->symbols_per_slot, .slots_per_frame=g->slots_per_frame};
  nr_srs_info_t info = {.comb_size=c->comb==2?0:c->comb==4?1:2, .K_TC_overbar=c->comb_offset,
    .n_SRS_cs=c->cs, .n_ID_SRS=c->group, .l_offset=c->start_symbol, .N_symb_SRS=c->n_symbols,
    .R=1, .n_srs_ports=1, .resource_type=aperiodic};
  bool found=false;
  for(int b=0;b<4 && !found;b++) for(int v=0;v<64;v++) if(get_m_srs(v,b)==c->nb_rb) {
    info.C_SRS=v; info.B_SRS=b; info.b_hop=b; found=true; break;
  }
  if(!found) return false;
  c16_t *p=(c16_t *)out;
  return generate_srs_nr(&fp,&p,c->start_symbol*g->fft_size,c->start_rb,&info,8192,0,0,1);
}
#include "nr_srs_blind_internal.h"
bool srs_test_supported(const nr_srs_candidate_t *c)
{
  nr_srs_info_t info;
  return nr_srs_make_info(c,&info);
}
#include "nr_srs_fep.h"
#include "nr_sense_api.h"
#include "PHY/NR_REFSIG/nr_refsig.h"
#include "PHY/NR_REFSIG/dmrs_nr.h"
#include <math.h>
/* Independent PUSCH QPSK DM-RS generator, common delayed/noisy OFDM channel,
 * PUSCH's real RX rotation and the production SRS FEP. Four distinct antennas. */
void srs_test_realistic(const nr_srs_grid_t *g,const nr_srs_candidate_t *c,int delay,int srs_advance,int pusch_advance,
                        int16_t *srs,float *pusch,uint16_t *pk,double *pusch_delay)
{
  const int N=g->fft_size,width=c->nb_rb*12,np=width/2;
  c16_t ref[np],spectrum[N],raw[N],rx[N],shift[N],rot[28];
  uint32_t gold[64]={0},bits[2048];pseudo_random_sequence(2048,bits,123456);
  for(int i=0;i<2048;i++) gold[i/32]|=bits[i]<<(i%32);
  nr_pusch_dmrs_rx(NR_NORMAL,gold,ref,1000,0,c->nb_rb,0,pusch_dmrs_type1,1024);
  for(int i=0;i<28;i++) rot[i]=(c16_t){lrint(32767*cos(.13*i)),lrint(32767*sin(.13*i))};
  for(int k=0;k<N;k++) shift[k]=(c16_t){lrint(32767*cos(2*M_PI*k*9/N)),lrint(32767*sin(2*M_PI*k*9/N))};
  uint32_t rng=123;
  for(int a=0;a<g->n_ant;a++) {
    c16_t *grid=(c16_t *)srs+a*14*N;
    srs_test_generate(g,c,(int16_t *)grid);
    for(int source=0;source<2;source++) {
      memset(spectrum,0,sizeof(spectrum));
      if(source==0) memcpy(spectrum,grid+c->start_symbol*N,sizeof(spectrum));
      else for(int j=0;j<np;j++) spectrum[(g->first_carrier_offset+c->start_rb*12+2*j)%N]=(c16_t){ref[j].r,-ref[j].i};
      for(int k=0;k<N;k++) {
        // Inverse receiver rotation plus propagation delay; independent of SRS search.
        double phase=.13*(14+c->start_symbol)-2*M_PI*k*(delay+9+(source==0?srs_advance:pusch_advance))/N+.2*a;
        double r=spectrum[k].r,i=spectrum[k].i,gain=.5+.1*a;
        spectrum[k]=(c16_t){lrint(gain*(r*cos(phase)-i*sin(phase))),lrint(gain*(r*sin(phase)+i*cos(phase)))};
      }
      idft(get_idft(N),(int16_t *)spectrum,(int16_t *)raw,1);
      for(int k=0;k<N;k++) {
        rng=rng*1664525u+1013904223u;raw[k].r+=(int)(rng>>27)-16;
        rng=rng*1664525u+1013904223u;raw[k].i+=(int)(rng>>27)-16;
      }
      if(source==0) nr_srs_fep_symbol(g,2,shift,rot,1,c->start_symbol,0,15360,0,raw,grid+c->start_symbol*N);
      else {
        dft(get_dft(N),(int16_t *)raw,(int16_t *)rx,1);
        apply_nr_rotation_symbol_RX(14,2,shift,g->first_carrier_offset,rx,rot,g->n_rb,1,c->start_symbol);
        double dr=0,di=0,pr=0,pi=0;
        for(int j=0;j<np;j++) {
          int k=(g->first_carrier_offset+c->start_rb*12+2*j)%N;
          double power=(double)ref[j].r*ref[j].r+(double)ref[j].i*ref[j].i;
          double hr=((double)rx[k].r*ref[j].r-(double)rx[k].i*ref[j].i)/power;
          double hi=((double)rx[k].i*ref[j].r+(double)rx[k].r*ref[j].i)/power;
          pusch[2*(a*np+j)]=hr;pusch[2*(a*np+j)+1]=hi;pk[j]=nr_sense_crb_k(k,g->first_carrier_offset,N);
          if(j) {dr+=hr*pr+hi*pi;di+=hi*pr-hr*pi;}pr=hr;pi=hi;
        }
        if(a==0) *pusch_delay=-atan2(di,dr)*N/(4*M_PI)-pusch_advance;
      }
    }
  }
}
void srs_test_pusch_tp(const nr_srs_grid_t *g,const nr_srs_candidate_t *c,int16_t *iq)
{
  int np=c->nb_rb*6;
  c16_t dmrs[np];
  nr_pusch_lowpaprtype1_dmrs_rx(NR_NORMAL,dmrs_lowpaprtype1_ul_ref_sig[c->group][0][get_index_for_dmrs_lowpapr_seq(np)],
                               dmrs,1000,0,c->nb_rb,0,pusch_dmrs_type1);
  for(int a=0;a<g->n_ant;a++) for(int j=0;j<np;j++) {
    int bin=(g->first_carrier_offset+c->start_rb*12+2*j)%g->fft_size;
    ((c16_t *)iq)[a*14*g->fft_size+c->start_symbol*g->fft_size+bin]=(c16_t){dmrs[j].r/4,-dmrs[j].i/4};
  }
}
