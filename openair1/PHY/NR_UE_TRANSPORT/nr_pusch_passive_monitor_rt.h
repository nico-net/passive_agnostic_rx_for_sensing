/*
 * UL-slot hook: the half of the passive uplink path that does not run in a downlink slot.
 *
 * An UL grant is recovered from the DOWNLINK (the DCI that schedules it), but the PUSCH it names is
 * k2 slots later, in an UPLINK slot -- and nothing runs there today: the blind PDCCH monitor sits
 * inside nr-ue.c's `rx_slot_type == DOWNLINK || MIXED` guard, and a UE has no reason to process an
 * uplink slot at all. So the grant has to be PARKED when it is decoded and picked up again when its
 * slot arrives. That is the grant book below.
 *
 * The clock on both sides is nr_ue_diag_producer_absolute_slot, the producer's un-wrapped slot
 * counter. Deliberately not frame*slots_per_frame + slot: that wraps at 1024 frames, and this tree
 * has already measured that reconstruction producing a max_lag of 20491 slots on the DL queue.
 */

#ifndef NR_PUSCH_PASSIVE_MONITOR_RT_H
#define NR_PUSCH_PASSIVE_MONITOR_RT_H

#include <stdbool.h>
#include <stdint.h>

#include "PHY/defs_nr_UE.h"
#include "nr_pdcch_blind_monitor.h"
#include "nr_passive_ue_ctx.h"
#include "nr_pusch_passive_decode.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Park a recovered UL grant for the slot its PUSCH occupies (now + k2).
/// Called from the blind PDCCH tap when a DCI 0_1 is accepted.
void nr_pusch_grant_book_add(const nr_pdcch_blind_ul_result_t *g, long source_absolute_slot);

/// Process any PUSCH due in this slot. Called from nr-ue.c for every received slot (late/mixed slots included).
/// No-op unless [sensing] pdcch_blind_monitor_ul_pusch enables it.
void nr_pusch_passive_monitor_process(PHY_VARS_NR_UE *ue, const UE_nr_rxtx_proc_t *proc);

/// Periodic census: grants parked, matched to a slot, expired unclaimed.
void nr_pusch_grant_book_stats_dump(void);

/* U9 milestone output, called only after the decoder and U3/U4 feedback. */
void nr_pusch_first_crc_result(const nr_pdcch_blind_ul_result_t *grant,
                               const nr_ue_ul_ticket_t *ticket, bool ticket_valid,
                               const nr_pusch_passive_out_t *out, int64_t target_slot,
                               int slots_per_frame, uint64_t cpu_us);

#ifdef __cplusplus
}
#endif

#endif // NR_PUSCH_PASSIVE_MONITOR_RT_H
