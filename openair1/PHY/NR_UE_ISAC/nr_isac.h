/* SPDX-License-Identifier: OAI-Public-License-1.1 */
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
  NR_ISAC_SRC_SSB = 6,
  NR_ISAC_SRC_COUNT = 7
} nr_isac_source_t;

#define NR_ISAC_NUM_ILLUM 2
#define NR_ISAC_ILLUM_DL 0
#define NR_ISAC_ILLUM_UL 1

static inline unsigned nr_isac_source_illum(nr_isac_source_t source)
{
  return (source == NR_ISAC_SRC_PUSCH_DMRS || source == NR_ISAC_SRC_PUSCH_DATA)
             ? NR_ISAC_ILLUM_UL
             : NR_ISAC_ILLUM_DL;
}

typedef struct nr_isac_carrier_s {
  uint32_t nof_prb;
  uint32_t scs_hz;
  uint64_t dl_center_hz;
  uint16_t pci;
  uint16_t slots_per_frame;
} nr_isac_carrier_t;

/* Process-wide policy values requested by the deployment. The UL flag is always subordinate. */
extern int AOA_ENABLE;
extern int AOA_UL_ENABLE;

void nr_isac_init(void);
void nr_isac_start(void);
void nr_isac_stop(void);
int nr_isac_enabled(void);
int nr_isac_source(void);
int nr_isac_source_enabled(int source);

void nr_isac_submit_cfr(uint32_t slot_idx,
                        int source,
                        const nr_isac_carrier_t *carrier,
                        const float *h,
                        const uint32_t *k_abs,
                        const uint32_t *l_sym,
                        uint32_t nof_re,
                        float noise_var);

void nr_isac_submit_cfr_at(uint32_t slot_idx,
                           float slot_frac,
                           int source,
                           const nr_isac_carrier_t *carrier,
                           const float *h,
                           const uint32_t *k_abs,
                           const uint32_t *l_sym,
                           uint32_t nof_re,
                           float noise_var);

void nr_isac_submit_cfr_multi(uint32_t slot_idx,
                              float slot_frac,
                              int source,
                              const nr_isac_carrier_t *carrier,
                              const float *h,
                              uint32_t nof_ant,
                              uint32_t ant_stride_re,
                              const uint32_t *k_abs,
                              const uint32_t *l_sym,
                              uint32_t nof_re,
                              float noise_var);
/* k_abs is always the logical CRB/Point-A carrier-grid coordinate in
 * [0, carrier->nof_prb*12). It is never an FFT-buffer index and excludes first_carrier_offset. */

uint32_t nr_isac_aoa_antennas(void);
uint32_t nr_isac_subslot_config(uint32_t *min_re, float *min_snr_db);

#ifdef __cplusplus
}
#endif
#endif
