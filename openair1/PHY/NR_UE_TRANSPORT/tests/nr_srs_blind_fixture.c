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
