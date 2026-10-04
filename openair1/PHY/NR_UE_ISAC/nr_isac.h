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

/* Receiver-facing adapter onto nr_sense_api (CFR v1, records schema 2).
 * init reads [sensing]: enable, sources, cfr_path, records_path, endpoint,
 * cfr_ring_frames, cfr_max_re; rx_id/rx_array are session metadata only.
 * All legacy algorithm keys are accepted and ignored with one warning.
 * start opens the exporter; stop drains and closes it after producers stop.
 * Disabled calls are no-ops. Submit calls copy once and never block, allocate or do I/O;
 * a full ring drops the frame and counts it. Every call creates an independent frame.
 * h is antenna-major interleaved float IQ; ant_stride_re counts complex RE.
 * k_abs/l_sym are absolute subcarrier / OFDM symbol indices for each RE.
 * slot_frac is the offset within the slot, noise_var is linear per-RE noise power.
 * subslot_config returns zero: retired algorithm grouping/gates do not apply.
 */
#ifndef NR_ISAC_H
#define NR_ISAC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
typedef enum nr_isac_source_e {
  NR_ISAC_SRC_CSI_RS = 0,
  NR_ISAC_SRC_PDSCH_DMRS = 1,
  NR_ISAC_SRC_PDSCH_DATA = 2,
  NR_ISAC_SRC_PDSCH_DMRS_BLIND = 3,
  NR_ISAC_SRC_PUSCH_DMRS = 4,
  NR_ISAC_SRC_PUSCH_DATA = 5,
  NR_ISAC_SRC_SSB    = 6,
  NR_ISAC_SRC_COUNT  = 7
} nr_isac_source_t;
#define NR_ISAC_NUM_ILLUM 2
#define NR_ISAC_ILLUM_DL  0
#define NR_ISAC_ILLUM_UL  1
static inline unsigned nr_isac_source_illum(nr_isac_source_t s)
{
  return (s == NR_ISAC_SRC_PUSCH_DMRS || s == NR_ISAC_SRC_PUSCH_DATA) ? NR_ISAC_ILLUM_UL
                                                                      : NR_ISAC_ILLUM_DL;
}
typedef struct nr_isac_carrier_s {
  uint32_t nof_prb;
  uint32_t scs_hz;
  uint64_t dl_center_hz;
  uint16_t pci;
  uint16_t slots_per_frame;
} nr_isac_carrier_t;

void nr_isac_init(void);
void nr_isac_start(void);
void nr_isac_stop(void);
int nr_isac_enabled(void);
int nr_isac_source(void);

int nr_isac_source_enabled(int source);
/* PRACH has a record only and therefore no CFR source enum. */
int nr_isac_prach_enabled(void);

void nr_isac_submit_cfr(uint32_t                 slot_idx,
                        int                      source,
                        const nr_isac_carrier_t* carrier,
                        const float*             h,
                        const uint32_t*          k_abs,
                        const uint32_t*          l_sym,
                        uint32_t                 nof_re,
                        float                    noise_var);

void nr_isac_submit_cfr_at(uint32_t                 slot_idx,
                           float                    slot_frac,
                           int                      source,
                           const nr_isac_carrier_t* carrier,
                           const float*             h,
                           const uint32_t*          k_abs,
                           const uint32_t*          l_sym,
                           uint32_t                 nof_re,
                           float                    noise_var);

void nr_isac_submit_cfr_multi(uint32_t                 slot_idx,
                              float                    slot_frac,
                              int                      source,
                              const nr_isac_carrier_t* carrier,
                              const float*             h,
                              uint32_t                 nof_ant,
                              uint32_t                 ant_stride_re,
                              const uint32_t*          k_abs,
                              const uint32_t*          l_sym,
                              uint32_t                 nof_re,
                              float                    noise_var);

uint32_t nr_isac_subslot_config(uint32_t* min_re, float* min_snr_db);

#ifdef __cplusplus
}
#endif

#endif // NR_ISAC_H
