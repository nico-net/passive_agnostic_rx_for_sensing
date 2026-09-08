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

#include "nr_pdcch_ul_field_sweep.h"
#include "common/utils/LOG/log.h"
#include <string.h>
_Static_assert(sizeof(nr_pdcch_ul_field_widths_t) <= NR_HYP_BYTES, "hypothesis storage");
static const int axes[16][7] = {
  {0,3},{0,1},{0,1,2},{0,1},{4,5},{1,2},{0,2},{0,1,2},{0,1,2,3,4},
  {2,3,4,5},{2,3},{0,1,2,3,4,5,6},{0,1,2},{0,1,2},{0,2},{0,1}
};
static const int sizes[16] = {2,2,3,2,2,2,2,3,5,4,2,7,3,3,2,2};
void nr_pdcch_ul_field_sweep_apply(const nr_hyp_t *h, nr_pdcch_blind_ul_opts_t *o)
{
  if (!h || !o || h->len != sizeof(nr_pdcch_ul_field_widths_t)) return;
  nr_pdcch_ul_field_widths_t w;
  memcpy(&w, h->bytes, sizeof(w));
  o->carrier_indicator_bits = w.carrier_indicator_bits;
  o->ul_sul_bits = w.ul_sul_bits;
  o->bwp_indicator_bits = w.bwp_indicator_bits;
  o->freq_hopping_bits = w.freq_hopping_bits;
  o->harq_pid_bits = w.harq_pid_bits;
  o->dai1_bits = w.dai1_bits;
  o->dai2_bits = w.dai2_bits;
  o->sri_bits = w.sri_bits;
  o->precoding_info_bits = w.precoding_info_bits;
  o->antenna_ports_bits = w.antenna_ports_bits;
  o->srs_request_bits = w.srs_request_bits;
  o->csi_request_bits = w.csi_request_bits;
  o->cbg_bits = w.cbg_bits;
  o->ptrs_dmrs_bits = w.ptrs_dmrs_bits;
  o->beta_offset_bits = w.beta_offset_bits;
  o->dmrs_seq_init_bits = w.dmrs_seq_init_bits;
}
static nr_hyp_t pack(const int *v)
{
  nr_pdcch_ul_field_widths_t w = {
    .carrier_indicator_bits = v[0],
    .ul_sul_bits = v[1],
    .bwp_indicator_bits = v[2],
    .freq_hopping_bits = v[3],
    .harq_pid_bits = v[4],
    .dai1_bits = v[5],
    .dai2_bits = v[6],
    .sri_bits = v[7],
    .precoding_info_bits = v[8],
    .antenna_ports_bits = v[9],
    .srs_request_bits = v[10],
    .csi_request_bits = v[11],
    .cbg_bits = v[12],
    .ptrs_dmrs_bits = v[13],
    .beta_offset_bits = v[14],
    .dmrs_seq_init_bits = v[15],
  };
  nr_hyp_t h = {.len = sizeof(w)};
  memcpy(h.bytes, &w, sizeof(w));
  return h;
}
typedef struct {
  const nr_pdcch_blind_ul_opts_t *fixed;
  int target, capacity, count;
  nr_hyp_t *out;
  int minimum[17], maximum[17], v[16];
} generation_t;
static bool enumerate(generation_t *g, int axis, int remaining)
{
  if (remaining < g->minimum[axis] || remaining > g->maximum[axis]) return true;
  if (axis == 16) {
    if (g->count == g->capacity) return false;
    nr_hyp_t h = pack(g->v);
    nr_pdcch_blind_ul_opts_t o = *g->fixed;
    nr_pdcch_ul_field_sweep_apply(&h,&o);
    if (nr_pdcch_blind_dci01_size(&o) == g->target) g->out[g->count++] = h;
    return true;
  }
  for (int a=0; a<sizes[axis]; ++a) {
    g->v[axis]=axes[axis][a];
    if (!enumerate(g,axis+1,remaining-g->v[axis])) return false;
  }
  return true;
}
int nr_pdcch_ul_field_sweep_generate(const nr_pdcch_blind_ul_opts_t *fixed,
                                    uint16_t len, nr_hyp_t *out, int cap)
{
  if (!fixed || !out || cap<=0 || fixed->bwp_size<1 || fixed->bwp_size>275 ||
      fixed->tda_count<0 || fixed->tda_count>16 || len<1 || len>63)
    return NR_HYP_SWEEP_INVALID;
  generation_t g = {.fixed=fixed,.target=len,.capacity=cap<NR_HYP_SWEEP_MAX_RAW?cap:NR_HYP_SWEEP_MAX_RAW,.out=out};
  for (int i=15;i>=0;--i) {
    g.minimum[i]=g.minimum[i+1]+axes[i][0];
    g.maximum[i]=g.maximum[i+1]+axes[i][sizes[i]-1];
    g.v[i]=axes[i][0];
  }
  nr_hyp_t h=pack(g.v);
  nr_pdcch_blind_ul_opts_t base=*fixed;
  nr_pdcch_ul_field_sweep_apply(&h,&base);
  const int fixed_bits=nr_pdcch_blind_dci01_size(&base)-g.minimum[0];
  if (!enumerate(&g,0,len-fixed_bits)) {
    LOG_E(PHY,"UL width search refused: admissible set exceeds raw cap %d at DCI length %u\n",g.capacity,len);
    return NR_HYP_SWEEP_RAW_OVERFLOW;
  }
  return g.count;
}
