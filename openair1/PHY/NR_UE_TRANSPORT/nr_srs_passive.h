/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_SRS_PASSIVE_H
#define NR_SRS_PASSIVE_H
#include <stdint.h>
#include <stdbool.h>
#include "nr_periodic_track.h"
#ifdef __cplusplus
extern "C" {
#endif
struct PHY_VARS_NR_UE_s;
/* Per-cell (mpci CELLFEAT): every call acts on the state of the calling thread's cell (nr_cell_idx()); the cell's worker is created bound to it.
 * nr_srs_passive_cell_prealloc(cell) runs once per admitted cell before the RX fence (also when SRS is off: the producers read the active flag). */
void nr_srs_passive_cell_prealloc(int cell);
/* Start/stop outside RX workers; capture is allocation-free, try-lock/drop. */
void nr_srs_passive_cell_pause(int cell, bool pause); /* retired cell: producers see enabled()==false; state kept, no allocation */
void nr_srs_passive_start(struct PHY_VARS_NR_UE_s *ue);
void nr_srs_passive_stop(void);
void nr_srs_passive_capture(struct PHY_VARS_NR_UE_s *ue, int frame, int slot, uint64_t abs_slot);
void nr_srs_passive_trigger(uint16_t rnti, uint64_t slot, unsigned request);
/* Arrival estimate = ta_applied_samples - final residual delay; positive means
 * arrival advanced relative to the DL frame. This is attribution evidence,
 * not the applied FFT-window field or a commanded UE TA. */
void nr_srs_passive_pusch(uint16_t rnti, uint64_t slot, int ta_est_samples,
                         const float *cfr, const uint32_t *k, int n_re);
void nr_srs_passive_hint(int comb, int offset, int group, int cs);
void nr_srs_passive_grant(uint64_t slot,uint16_t mask,int start_rb,int nb_rb);
void nr_srs_passive_metrics(uint64_t *us,uint64_t *dropped,uint64_t *pregate,uint64_t *backoff,
                            uint64_t *budget,uint64_t *resets);
bool nr_srs_passive_enabled(void);
/* Periodic-resource tracker counters of the bound cell (SRS): predicted occasions, hits, misses, demotions, confirmations, audit hits, audits run. */
void nr_srs_passive_gate_metrics(uint64_t *unexplained, uint64_t *pusch);
/* Diagnostic (is SRS detection SNR dependent): histogram of the time-domain comb gate statistic (sigma units above the Gamma null, nr_srs_td_comb_gate; the gate opens above 9) of every
 * evaluated eligible slot, bins [0,3) [3,6) [6,9] (skipped) (9,12] (12,18] (18,30] >30 (opened); det[] = of the opened ones, the slots in which the scan exported at least one SRS record. */
#define NR_SRS_GATE_BINS 7
void nr_srs_passive_gate_hist(uint64_t bin[NR_SRS_GATE_BINS], uint64_t det[NR_SRS_GATE_BINS]);
static inline int nr_srs_gate_bin(double s) { return s < 3 ? 0 : s < 6 ? 1 : s <= 9 ? 2 : s <= 12 ? 3 : s <= 18 ? 4 : s <= 30 ? 5 : 6; }
void nr_srs_passive_periodic_metrics(nr_ptrack_counters_t *c, uint64_t *audits);
#ifdef __cplusplus
}
#endif
#endif
