/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_phase2.h
 * \brief Shared, cross-occasion state of the blind-PDCCH scan that more than one scan consumer can
 * reach (Task A7): the Phase-2 lock, the RNTI-persistence sightings ring and the adaptive energy
 * floor. Moved out of nr_pdcch_blind_monitor_rt.c (PHY_NR_UE) into the lean nr_pdcch_blind_monitor
 * library so the offline gtest can drive it from several threads under TSAN; the RT file links
 * against PHY_NR_UE's whole dependency graph and is not in the gtest's link closure.
 */
#ifndef NR_PDCCH_BLIND_PHASE2_H
#define NR_PDCCH_BLIND_PHASE2_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Phase 2 of one occasion (everything after the candidate decode join) runs under this lock. */
void nr_pdcch_blind_phase2_lock(void);
void nr_pdcch_blind_phase2_unlock(void);

/* Returns true once `rnti` has been sighted at least `min_k` times (including this one) within the
 * last `window_slots` slots; always records the sighting. CALLER HOLDS the Phase-2 lock. */
bool nr_pdcch_blind_rnti_persistence_check(uint16_t rnti, uint32_t abs_slot, uint32_t window_slots, int min_k);

/* Adaptive noise-floor energy estimator (frugal streaming median). update() feeds one candidate's
 * mean |LLR| and returns the floor after the update; both write the sample count to *nseen_out when
 * non-NULL. Internally locked: called from Phase 1, which runs concurrently across consumers. */
float nr_pdcch_blind_energy_floor_update(float x, uint64_t *nseen_out);
float nr_pdcch_blind_energy_floor_get(uint64_t *nseen_out);

/* ---- Test hooks (offline gtest only; harmless in production, never called there). ---- */
void nr_pdcch_blind_phase2_reset_for_test(void);
/* One synthetic Phase-2 accept, the same sequence as the DCI 1_1 accept path in
 * nr_pdcch_blind_monitor_rt.c: raw-accept counter, dci_thres EMA, mismatch gate, persistence gate.
 * Returns the gate verdict. */
bool nr_pdcch_blind_phase2_for_test(int *dci_thres, uint32_t mismatched_bits, uint16_t rnti, uint32_t abs_slot,
                                    uint32_t window_slots, int min_k);
uint64_t nr_pdcch_blind_phase2_accepts_for_test(void);
/* Sightings of `rnti` currently in the persistence ring; *total = ring occupancy. */
int nr_pdcch_blind_persistence_count_for_test(uint16_t rnti, int *total);

#ifdef __cplusplus
}
#endif
#endif
