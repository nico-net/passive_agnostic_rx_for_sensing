/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#ifndef NR_ISAC_H
#define NR_ISAC_H

#include <stdint.h>
#include "PHY/NR_UE_TRANSPORT/nr_rx_branch.h"

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

/* P14 Stage A (removal table 2.4, row 2): the AOA_ENABLE/AOA_UL_ENABLE environment override and
 * the process-wide globals it drove are gone. AoA is requested only by the `[sensing] aoa_enable`
 * / `aoa_ul_enable` configuration keys, whose parsed values live in PipelineConfig. The obsolete
 * environment keys are rejected loudly instead of silently ignored; returns how many were set. */
int nr_isac_obsolete_env_keys(void);

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

/* adaptive_RX_pipeline.md P10a: "no branch identity". NOT branch 0 -- branch 0 is a real physical
 * receive branch under P03's rx_branches/rx_branch_phys_map, so using 0 as the unset value would
 * make every legacy producer claim to be branch 0. The report omits the branch field for this
 * value, following the same omit-rather-than-fabricate convention as azimuth_std_deg. */
#define NR_ISAC_BRANCH_NONE 0xFFu

/* adaptive_RX_pipeline.md P10a: nr_isac_submit_cfr_multi() with an identity slot for the receive
 * branch the CFR was measured on. nr_isac_submit_cfr_multi() above is a one-line wrapper for this
 * function with NR_ISAC_BRANCH_NONE -- that wrapper, not an argument, is what makes every producer
 * that has not migrated structurally unchanged rather than assumed unchanged.
 *
 * P13 (adaptive_RX_pipeline.md Stage 4) made the identity LOAD-BEARING: this value now selects
 * which of the per-branch SensingEngine instances consumes the row. A branch_id naming a branch
 * that is not in `rx_branches` is DROPPED and counted, never folded into another branch's coherent
 * window; NR_ISAC_BRANCH_NONE goes to the lowest active branch, which with a single active branch
 * (the legacy and default configuration) is the only engine there is. */
void nr_isac_submit_cfr_multi_branch(uint32_t slot_idx,
                                     float slot_frac,
                                     int source,
                                     const nr_isac_carrier_t *carrier,
                                     const float *h,
                                     uint32_t nof_ant,
                                     uint32_t ant_stride_re,
                                     const uint32_t *k_abs,
                                     const uint32_t *l_sym,
                                     uint32_t nof_re,
                                     float noise_var,
                                     uint8_t branch_id);

uint32_t nr_isac_aoa_antennas(void);
uint32_t nr_isac_subslot_config(uint32_t *min_re, float *min_snr_db);

/* adaptive_RX_pipeline.md P03: [sensing] rx_branches / rx_branch_phys_map, parsed by
 * nr_isac_init() into a process-wide nr_rx_branch_set_t. Foundation only -- nothing in the RT
 * read loop consults this yet (P04/P05). Returns NULL if sensing is disabled or the branch
 * config failed to parse (nr_isac_init() logs LOG_E and disables sensing in that case, same as
 * every other fatal [sensing] parse failure in this file). */
const nr_rx_branch_set_t *nr_isac_rx_branches(void);

/* adaptive_RX_pipeline.md P06a: the same set, writable, for the ONE owner of branch lifecycle --
 * the nr-ue.c read loop (AcquisitionOwner). Every other reader uses the const accessor above.
 * The epochs it mutates are plain aligned uint32 counters read without a lock by the DL decode
 * consumers: they only ever increase, a torn read is not possible on an aligned 32-bit load, and
 * a consumer that reads one epoch late merely discards one more job than strictly necessary --
 * which is the safe direction. Returns NULL under exactly the same conditions as the const
 * accessor. */
nr_rx_branch_set_t *nr_isac_rx_branches_mutable(void);

/* Sets the live UE receive-antenna count nr_isac_init() checks rx_branches against (P03's "more
 * branches than antennas" reject). Must be called BEFORE nr_isac_init() to take effect; default
 * is 0 ("unknown"), under which the antenna-count check is skipped rather than fatally rejecting
 * a config it cannot evaluate. Not called from anywhere in this task (nr-uesoftmodem.c is out of
 * this task's file scope) -- see nr_isac.cc's nr_isac_init() comment and
 * docs/passive_branch_globals_audit.md for where get_nrUE_params()->nb_antennas_rx would supply
 * a live value once wired. */
void nr_isac_set_nb_antennas_rx(int nb_antennas_rx);

#ifdef __cplusplus
}
#endif
#endif
