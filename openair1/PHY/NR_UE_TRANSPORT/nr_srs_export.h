/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_SRS_EXPORT_H
#define NR_SRS_EXPORT_H
#include "nr_srs_blind.h"
typedef struct {
  uint64_t abs_slot, t_mono_ns, rx_ts;
  uint32_t epoch, identity_gen;
  int dedicated_epoch;
  int frame, slot, pci, n_ant, ta_est_samples, cfr_max_re;
  bool reconf, rx_ts_valid;
} nr_srs_capture_t;
#ifdef __cplusplus
extern "C" {
#endif
uint64_t nr_srs_export(const nr_srs_candidate_t *c, const nr_srs_capture_t *m,
                       int n_re, const float *iq, const uint16_t *k, const uint8_t *l);
#ifdef __cplusplus
}
#endif
#endif
