/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#include "PHY/NR_UE_ISAC/nr_isac.h"

int AOA_ENABLE = 0;
int AOA_UL_ENABLE = 0;

void nr_isac_init(void) {}
void nr_isac_start(void) {}
void nr_isac_stop(void) {}
int nr_isac_enabled(void) { return 0; }
int nr_isac_source(void) { return NR_ISAC_SRC_CSI_RS; }
int nr_isac_source_enabled(int source) { (void)source; return 0; }

void nr_isac_submit_cfr(uint32_t s, int t, const nr_isac_carrier_t *c, const float *h,
                        const uint32_t *k, const uint32_t *l, uint32_t n, float v)
{ (void)s; (void)t; (void)c; (void)h; (void)k; (void)l; (void)n; (void)v; }
void nr_isac_submit_cfr_at(uint32_t s, float f, int t, const nr_isac_carrier_t *c,
                           const float *h, const uint32_t *k, const uint32_t *l,
                           uint32_t n, float v)
{ (void)s; (void)f; (void)t; (void)c; (void)h; (void)k; (void)l; (void)n; (void)v; }
void nr_isac_submit_cfr_multi(uint32_t s, float f, int t, const nr_isac_carrier_t *c,
                              const float *h, uint32_t a, uint32_t z,
                              const uint32_t *k, const uint32_t *l, uint32_t n, float v)
{ (void)s; (void)f; (void)t; (void)c; (void)h; (void)a; (void)z; (void)k; (void)l; (void)n; (void)v; }
uint32_t nr_isac_aoa_antennas(void) { return 0; }
uint32_t nr_isac_subslot_config(uint32_t *min_re, float *min_snr_db)
{ if (min_re) *min_re = 0; if (min_snr_db) *min_snr_db = 0.0f; return 0; }
const nr_rx_branch_set_t *nr_isac_rx_branches(void) { return NULL; }
nr_rx_branch_set_t *nr_isac_rx_branches_mutable(void) { return NULL; }
void nr_isac_set_nb_antennas_rx(int nb_antennas_rx) { (void)nb_antennas_rx; }
