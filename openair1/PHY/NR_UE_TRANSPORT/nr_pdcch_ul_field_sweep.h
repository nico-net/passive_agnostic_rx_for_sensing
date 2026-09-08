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

#ifndef NR_PDCCH_UL_FIELD_SWEEP_H
#define NR_PDCCH_UL_FIELD_SWEEP_H
#include "nr_hyp_sweep.h"
#include "nr_pdcch_blind_monitor.h"
typedef struct {
  int carrier_indicator_bits;
  int ul_sul_bits;
  int bwp_indicator_bits;
  int freq_hopping_bits;
  int harq_pid_bits;
  int dai1_bits;
  int dai2_bits;
  int sri_bits;
  int precoding_info_bits;
  int antenna_ports_bits;
  int srs_request_bits;
  int csi_request_bits;
  int cbg_bits;
  int ptrs_dmrs_bits;
  int beta_offset_bits;
  int dmrs_seq_init_bits;
} nr_pdcch_ul_field_widths_t;
/* Bounds apply AFTER the length constraint, before equivalence collapsing. On overflow
 * the partial output MUST NOT be consumed. Fixed BWP/TDA width are prerequisites. */
int nr_pdcch_ul_field_sweep_generate(const nr_pdcch_blind_ul_opts_t *, uint16_t, nr_hyp_t *, int);
void nr_pdcch_ul_field_sweep_apply(const nr_hyp_t *, nr_pdcch_blind_ul_opts_t *);
#endif
