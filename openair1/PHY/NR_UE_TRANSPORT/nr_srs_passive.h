/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_SRS_PASSIVE_H
#define NR_SRS_PASSIVE_H
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
struct PHY_VARS_NR_UE_s;
/* Start/stop outside RX workers; capture is allocation-free, try-lock/drop. */
void nr_srs_passive_start(struct PHY_VARS_NR_UE_s *ue);
void nr_srs_passive_stop(void);
void nr_srs_passive_capture(struct PHY_VARS_NR_UE_s *ue, int frame, int slot, uint64_t abs_slot);
void nr_srs_passive_trigger(uint16_t rnti, uint64_t slot, unsigned request);
void nr_srs_passive_pusch(uint16_t rnti, uint64_t slot, int advance,
                         const float *cfr, const uint32_t *k, int n_re);
void nr_srs_passive_hint(int comb, int offset, int group, int cs);
bool nr_srs_passive_enabled(void);
#ifdef __cplusplus
}
#endif
#endif
