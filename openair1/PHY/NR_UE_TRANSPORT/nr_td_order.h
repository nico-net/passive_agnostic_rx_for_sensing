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
/* ---- BC12a: SIB1 common-TDRA census (log + metrics only, never read by the search) ----------------------------------
 * At the first convergence of a context (rnti, cfg, tda) the winner's (k0, S, L, mapping) is compared with the SIB1 common
 * TDRA row selected by the DCI's TDA index, and with the default table A row (tda_index + 1, TS 38.214 Table 5.1.2.1.1-2,
 * MIB dmrs-TypeA-Position). It does not feed ordering, pruning or acceptance. */
typedef enum { NR_TD_CENSUS_NONE = 0, NR_TD_CENSUS_MATCH = 1, NR_TD_CENSUS_MISMATCH = 2 } nr_td_census_t;
enum { NR_TD_FMT_UNK = 0, NR_TD_FMT_10 = 1, NR_TD_FMT_11 = 2 };
/* NONE: no SIB1 list (n == 0). tda_index >= n is a MISMATCH (the DCI addresses a row the broadcast list does not have). */
nr_td_census_t nr_td_census_sib1(const nr_td_tdra_t *list, int n, int tda_index, const nr_pdsch_cfg_hypothesis_t *h);
/* NONE: dmrs_typeA_pos not 2/3 (unknown). An invalid row (tda_index + 1 > 16) is a MISMATCH. */
nr_td_census_t nr_td_census_deftab(int dmrs_typeA_pos, int tda_index, const nr_pdsch_cfg_hypothesis_t *h);
/* Last decoded SIB1 pdsch-TimeDomainAllocationList (k0 included); set(NULL, 0) clears it. Returns/stores <= 16 rows. */
void nr_td_sib1_store_set(const nr_td_tdra_t *list, int n);
int nr_td_sib1_store_get(nr_td_tdra_t out[NR_TD_MAX_SIB1_TDRA]);
/* Counters [format 0=unknown/1=1_0/2=1_1 (NR_TD_FMT_*)][outcome]; dci_format arg is 10 or 11, anything else is "unknown". */
void nr_td_census_count(int dci_format, nr_td_census_t sib1);
void nr_td_census_count_deftab(int dci_format, nr_td_census_t deftab);
void nr_td_census_get(uint64_t sib1[3][3]);
void nr_td_census_get_deftab(uint64_t deftab[3][3]);
void nr_td_census_reset(void);
/* Non-negative ordering score, higher = try earlier. 0 whenever all weights are 0. */
float nr_td_ordering_score(const nr_pdsch_cfg_hypothesis_t *h, const nr_td_side_info_t *si);
#ifdef __cplusplus
}
#endif
#endif
