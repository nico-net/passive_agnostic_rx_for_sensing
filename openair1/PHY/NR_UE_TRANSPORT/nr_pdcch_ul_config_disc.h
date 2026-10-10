/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_PDCCH_UL_CONFIG_DISC_H
#define NR_PDCCH_UL_CONFIG_DISC_H
#include "nr_pdcch_blind_monitor.h"
#include <stdint.h>
#include <stdbool.h>

/* U3: provisional, per-RNTI no-RRC UL BWP/layout hypotheses. A returned grant
 * carries a producer-time ticket in hyp_generation. Only a full nonzero PUSCH
 * TB CRC passed to feedback can validate it. The TDD mask is a PRIOR: zero
 * period means unknown and never rejects a grant. */
typedef struct {
  uint16_t dl_bwp_start, dl_bwp_size, carrier_rb;
  uint8_t tdd_period;
  uint64_t ul_slot_mask;
  uint32_t source_abs_slot;
  uint64_t bank_key;
} nr_pdcch_ul_config_prior_t;
typedef struct {
  uint64_t proposed, dropped_busy, dropped_capacity, bank_note_dropped_busy, feedback_ok, feedback_bad;
  uint64_t proposal_calls, proposal_ns, proposal_max_ns;
  uint32_t live_hypotheses, locked_hypotheses;
} nr_pdcch_ul_config_stats_t;

bool nr_pdcch_ul_config_disc_enabled(void); /* ISAC_UL_CONFIG_DISC=1 */
void nr_pdcch_ul_config_disc_reset(void);
bool nr_pdcch_ul_config_disc_grant(uint16_t rnti, uint16_t length, nr_dci_bits_t payload,
                                   const nr_pdcch_blind_ul_opts_t *base,
                                   const nr_pdcch_ul_config_prior_t *prior,
                                   nr_pdcch_blind_ul_result_t *out);
/* Return true if this was a U3 ticket, including stale/rejected ones. */
bool nr_pdcch_ul_config_disc_feedback(const nr_pdcch_blind_ul_result_t *grant, bool tb_crc_ok);
void nr_pdcch_ul_config_disc_stats(nr_pdcch_ul_config_stats_t *out);
/* A TB-validated geometry may seed the existing full width/interpreter sweep. */
bool nr_pdcch_ul_config_disc_locked(uint16_t rnti, uint16_t length,
                                    const nr_pdcch_ul_config_prior_t *prior,
                                    nr_pdcch_blind_ul_opts_t *out);
/* Worker-side snapshot of all equal-length interpretations belonging to this
 * producer ticket. A stale/reused ticket returns zero; no state is advanced. */
int nr_pdcch_ul_config_disc_options(const nr_pdcch_blind_ul_result_t *grant,
                                    nr_pdcch_blind_ul_opts_t *out, int capacity,
                                    int *selected);
/* Accepted DL DCI occupancy supplies a weak 20-slot TDD prior without SIB1. */
void nr_pdcch_ul_config_disc_note_dl_slot(uint32_t abs_slot);

/* U2 evidence and replay milestones. Key includes the bank geometry so a
 * RNTI reused on another CORESET cannot borrow length or traffic evidence. */
bool nr_pdcch_ul_bank_enabled(void); /* ISAC_UL_DCI_BANK=1 */
void nr_pdcch_ul_bank_note(uint64_t bank_key, uint16_t rnti, uint16_t length,
                           uint32_t abs_slot, bool length_lock);
#endif
