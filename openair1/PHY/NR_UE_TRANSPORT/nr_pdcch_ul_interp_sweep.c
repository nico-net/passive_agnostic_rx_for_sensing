/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this
 * file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.openairinterface.org/?page_id=698
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

#include "nr_pdcch_ul_interp_sweep.h"
#include "common/utils/LOG/log.h"
#include <string.h>
int nr_pdcch_ul_interp_sweep_generate(nr_hyp_t *out, int cap)
{
  static const uint8_t tda[][4] = {
    {0,14,0,1},{0,14,0,2},{0,14,0,3},{0,14,0,4},
    {0,7,0,1},{0,7,0,2},{2,12,0,1},{2,12,0,2},{0,4,1,1},{0,4,1,2}
  };
  if (!out || cap<=0) return NR_HYP_SWEEP_INVALID;
  int n=0;
  for (int t=0;t<10;++t)
    for (int type=0;type<2;++type)
      for (int pos=0;pos<4;++pos)
        for (int max=1;max<=2;++max)
          for (int tp=0;tp<2;++tp)
            for (int mcs=0;mcs<3;++mcs) {
              if (n==cap || n==NR_HYP_SWEEP_MAX_RAW) {
                LOG_E(PHY,"UL interpretation search refused: raw cap exceeded\n");
                return NR_HYP_SWEEP_RAW_OVERFLOW;
              }
              nr_pdcch_ul_interp_hyp_t h={tda[t][0],tda[t][1],tda[t][2],tda[t][3],
                                         type,pos,max,tp,mcs};
              out[n]=(nr_hyp_t){.len=sizeof(h)};
              memcpy(out[n++].bytes,&h,sizeof(h));
            }
  return n;
}
bool nr_pdcch_ul_interp_sweep_apply(const nr_hyp_t *hyp, int idx, nr_pdcch_blind_ul_opts_t *o)
{
  if (!hyp || !o || hyp->len!=sizeof(nr_pdcch_ul_interp_hyp_t) ||
      idx<0 || idx>=16 || o->tda_count<=idx) return false;
  nr_pdcch_ul_interp_hyp_t h;
  memcpy(&h,hyp->bytes,sizeof(h));
  o->tda_start[idx]=h.tda_start; o->tda_length[idx]=h.tda_length;
  o->tda_mapping[idx]=h.tda_mapping; o->tda_k2[idx]=h.tda_k2;
  o->dmrs_config_type=h.dmrs_config_type; o->dmrs_add_pos=h.dmrs_add_pos;
  o->dmrs_max_length=h.dmrs_max_length; o->transform_precoding=h.transform_precoding;
  o->mcs_table=h.mcs_table;
  return true;
}
