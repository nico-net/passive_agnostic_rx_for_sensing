/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_TD_ORDER_H
#define NR_TD_ORDER_H
#include <stdbool.h>
#include <stdint.h>
#include "nr_pdsch_config_sweep.h"
#ifdef __cplusplus
extern "C" {
#endif
#define NR_TD_MAX_SIB1_TDRA 16
typedef struct {
  uint8_t S, L, mapping, k0; /* mapping: 0 = type A, 1 = type B */
} nr_td_tdra_t;
/* Side information used ONLY to order hypotheses (search priority). It never removes a hypothesis and never
 * affects acceptance, which stays with the TB CRC. */
typedef struct nr_td_side_info_s {
  /* SIB1 common TDRA list (0 entries = SIB1 absent or ignored) */
  int n_sib1;
  nr_td_tdra_t sib1[NR_TD_MAX_SIB1_TDRA];
  int dmrs_typeA_pos; /* MIB dmrs-TypeA-Position 2 or 3; 0 = unknown */
  int obs_dmrs_mask; /* measured DM-RS symbol mask, -1 = none */
  int obs_qm; /* measured modulation order for obs_mcs, -1 = none */
  int obs_mcs;
  int obs_last_symbol; /* last symbol with PDSCH energy, -1 = none (optional observable) */
  /* promoted cell fields, -1 = not promoted; confidence 0..1 */
  int f_S, f_L, f_mapping, f_k0, f_dmrs_add_pos, f_dmrs_max_len;
  float f_conf;
  float w_sib1, w_default, w_obs, w_field; /* ISAC_TD_W_*; all 0 = neutral (today's order) */
  float w_probe; /* P1 probe-ordering bonus applied by the engine to hypotheses with probe_pass > 0; not used by nr_td_ordering_score() */
} nr_td_side_info_t;

/* TS 38.214 Table 5.1.2.1.1-2, normal CP, rows 1..16; false for an invalid row or dmrs_typeA_pos not in {2,3} */
bool nr_td_default_table_a(int row, int dmrs_typeA_pos, nr_td_tdra_t *out);
/* Non-negative ordering score, higher = try earlier. 0 whenever all weights are 0. */
float nr_td_ordering_score(const nr_pdsch_cfg_hypothesis_t *h, const nr_td_side_info_t *si);
#ifdef __cplusplus
}
#endif
#endif
