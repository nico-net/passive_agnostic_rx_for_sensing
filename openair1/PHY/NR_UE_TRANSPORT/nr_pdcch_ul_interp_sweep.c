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
bool nr_pusch_tda_legal(int mapping_type, int S, int L)
{
  if (S < 0 || L < 1 || S + L > 14)
    return false;
  if (mapping_type == 0)
    return S == 0 && L >= 4;
  return mapping_type == 1;
}
int nr_pdcch_ul_interp_sweep_generate(nr_hyp_t *out, int cap)
{
  /* {S, L, mapping (0 = A, 1 = B), k2}. Mapping type B rows are enumerated like type A: the extraction
   * derives the type-B DM-RS mask (first DM-RS on the first PUSCH symbol) from the row's own mapping.
   * S=2 L=12 used to be listed as type A, which TS 38.214 Table 6.1.2.1-1 forbids (type A has S = 0):
   * it is legal only as type B, so it is listed as type B. The rows stay a curated list: the full legal
   * set (11 type A + 105 type B) x k2 {1..4} x the 96 field combinations below is 44,544 raw hypotheses
   * against NR_HYP_SWEEP_MAX_RAW = 8192, and without a UL DM-RS oracle nothing prunes it. */
  static const uint8_t tda[][4] = {
    {0,14,0,1},{0,14,0,2},{0,14,0,3},{0,14,0,4},
    {0,7,0,1},{0,7,0,2},{2,12,1,1},{2,12,1,2},{0,4,1,1},{0,4,1,2}
  };
  /* TS 38.214 6.1.4.1: which MCS table opts.mcs_table names depends on transform precoding --
   * nr_get_Qm_ul()/nr_get_code_rate_ul() (nr_mac_common.c) index 0=Table 5.1.3.1-1 (qam64), 1=Table
   * 5.1.3.1-2 (qam256), 2=Table 5.1.3.1-3 (qam64LowSE), 3=Table 6.1.4.1-1 (the TP-enabled default),
   * 4=Table 6.1.4.1-2 (TP-enabled qam64LowSE) -- get_pusch_mcs_table()'s own `2 + (is_tp<<1)` /
   * `0 + is_tp*3` arithmetic. qam256 (index 1) is not combined with transform precoding (spec:
   * mcs-Table is not applicable when transformPrecoder is enabled), so TP-enabled has two table
   * choices, not three. opts.mcs_table is the FINAL index used downstream (blind_ul_finish(),
   * fill_pusch_pdu()) -- generating it correctly here means no separate resolution step exists to
   * forget, matching this field's own header comment ("3..5 = TP variants"). */
  static const uint8_t mcs_no_tp[3] = {0, 1, 2};
  static const uint8_t mcs_tp[2]    = {3, 4};
  if (!out || cap<=0) return NR_HYP_SWEEP_INVALID;
  int n=0;
  for (int t=0;t<10;++t)
    for (int type=0;type<2;++type)
      for (int pos=0;pos<4;++pos)
        for (int max=1;max<=2;++max)
          for (int tp=0;tp<2;++tp) {
            const uint8_t *mcs_list = tp ? mcs_tp : mcs_no_tp;
            const int      n_mcs    = tp ? 2 : 3;
            for (int mi=0; mi<n_mcs; ++mi) {
              if (n==cap || n==NR_HYP_SWEEP_MAX_RAW) {
                LOG_E(PHY,"UL interpretation search refused: raw cap exceeded\n");
                return NR_HYP_SWEEP_RAW_OVERFLOW;
              }
              nr_pdcch_ul_interp_hyp_t h={tda[t][0],tda[t][1],tda[t][2],tda[t][3],
                                         type,pos,max,tp,mcs_list[mi]};
              out[n]=(nr_hyp_t){.len=sizeof(h)};
              memcpy(out[n++].bytes,&h,sizeof(h));
            }
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
