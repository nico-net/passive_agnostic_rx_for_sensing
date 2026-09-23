/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#include "PHY/NR_UE_ISAC/nr_isac.h"


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
void nr_isac_submit_cfr_multi_session(uint32_t s, float f, int t, const nr_isac_carrier_t *c,
                                      const float *h, uint32_t a, uint32_t z,
                                      const uint32_t *k, const uint32_t *l, uint32_t n, float v,
                                      uint64_t id)
{ (void)s; (void)f; (void)t; (void)c; (void)h; (void)a; (void)z; (void)k; (void)l; (void)n; (void)v; (void)id; }
uint32_t nr_isac_aoa_antennas(void) { return 0; }
/* P7: the multi-RX nr_isac.h no longer declares nr_isac_aoa_antennas() -- the engine owns the
 * channel count via nr_isac_rx_channels() instead. Receiver call sites were repointed there. */
uint32_t nr_isac_rx_channels(void) { return 0; }
uint32_t nr_isac_subslot_config(uint32_t *min_re, float *min_snr_db)
{ if (min_re) *min_re = 0; if (min_snr_db) *min_snr_db = 0.0f; return 0; }
void nr_isac_flow_note(uint16_t rnti, int uplink) { (void)rnti; (void)uplink; }
int nr_isac_flow_admit(uint16_t rnti) { (void)rnti; return 0; }
void nr_isac_request_discard(void) {}
int nr_isac_drained(void) { return 1; }
/* Removed (Task 2, P7 follow-up): nr_isac_submit_cfr_multi_branch, nr_isac_submit_plan,
 * nr_isac_rx_branches[_mutable], nr_isac_set_nb_antennas_rx. Their types (nr_isac_submit_plan_t,
 * nr_rx_branch_set_t, NR_ISAC_BRANCH_NONE) are not declared anywhere in the multi-RX nr_isac.h and
 * grep confirms zero callers in receiver code (whole-tree, excluding this file) -- a stale "branch
 * set" API from before the merge that broke the ENABLE_ISAC_SENSING=OFF compile. Re-add if a real
 * caller + header declaration reappears. */
