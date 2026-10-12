/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_PUSCH_SOLVER_H
#define NR_PUSCH_SOLVER_H

#include "nr_pdcch_blind_monitor.h"
#include "nr_passive_ue_ctx.h"

/* A reading is one physical interpretation of a CRC-accepted DCI 0_1.
 * layouts is the set of U3 layout indices which produce that same reading. */
typedef struct {
  nr_pdcch_blind_ul_result_t grant;
  uint64_t layouts;
} nr_pusch_solver_reading_t;

typedef struct {
  nr_pdcch_blind_ul_result_t grant;
  uint64_t layouts;
  uint32_t dci_hash;
  uint64_t dci_slot;
  uint8_t type_a_pos;
  bool probe;
  bool confirmation;
  uint32_t reserved_cpu_us;
} nr_pusch_solver_var_t;

typedef struct {
  uint64_t grants, probes, probe_ok, candidates, solved, fulls, full_ok, drops;
  uint64_t probe_cpu_us;
} nr_pusch_solver_stats_t;

bool nr_pusch_solver_enabled(void); /* ISAC_UL_PUSCH_SOLVER=1, default off */
void nr_pusch_solver_reset(void);
void nr_pusch_solver_set_mu(int mu); /* ISAC_SLOT_SCALE=1 (default off): refill period 20 ms at any numerology */
/* Signal-time token bucket; at most max_out variants from one grant. Only a
 * DL-locked ticket may enter. The caller supplies readings from U3, with the
 * original grant as fallback, and PCI/DL-derived scrambling-ID priors. */
int nr_pusch_solver_plan(const nr_ue_ul_ticket_t *ticket,
                         const nr_pusch_solver_reading_t *readings, int n_readings,
                         uint64_t source_slot, uint64_t target_slot, int type_a_pos,
                         const uint16_t *data_ids, int n_data_ids,
                         nr_pusch_solver_var_t *out, int max_out);
void nr_pusch_solver_feedback(const nr_ue_ul_ticket_t *ticket,
                              const nr_pusch_solver_var_t *variant, bool crc_ok,
                              bool credit, uint64_t cpu_us);
void nr_pusch_solver_stats(nr_pusch_solver_stats_t *out);
bool nr_pusch_solver_is_solved(const nr_ue_ul_ticket_t *ticket);

#endif
