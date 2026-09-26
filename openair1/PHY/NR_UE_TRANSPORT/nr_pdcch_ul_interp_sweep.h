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

#ifndef NR_PDCCH_UL_INTERP_SWEEP_H
#define NR_PDCCH_UL_INTERP_SWEEP_H
#include "nr_hyp_sweep.h"
#include "nr_pdcch_blind_monitor.h"
typedef struct {
  uint8_t tda_start, tda_length, tda_mapping, tda_k2;
  uint8_t dmrs_config_type, dmrs_add_pos, dmrs_max_length, transform_precoding, mcs_table;
} nr_pdcch_ul_interp_hyp_t;
/* TS 38.214 Table 6.1.2.1-1, normal CP: mapping type A (0) S = 0, L 4..14; type B (1) S 0..13,
 * L 1..14, S+L <= 14. Any other mapping_type is not legal. */
bool nr_pusch_tda_legal(int mapping_type, int S, int L);
/* Initial candidate catalogue from the design; not exhaustive NR configuration recovery. Every TDA row
 * (both mapping types) satisfies nr_pusch_tda_legal() -- enforced by the unit test.
 * Unsupported receiver modes remain unresolved hypotheses, never scored as CRC failures. */
int nr_pdcch_ul_interp_sweep_generate(nr_hyp_t *, int);
/* Apply only to the actually observed TDA index, never silently to entry zero. */
bool nr_pdcch_ul_interp_sweep_apply(const nr_hyp_t *, int tda_index, nr_pdcch_blind_ul_opts_t *);
#endif
