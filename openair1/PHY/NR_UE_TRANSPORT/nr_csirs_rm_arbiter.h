/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* CRC arbiter of the CSI-RS rate-matching patterns (per pattern, learned from the passive PDSCH decodes).
 *
 * A blind CSI-RS pattern (NZP confirmed by its sequence, ZP confirmed by its holes) is a hypothesis about the REs the gNB rate-matched around. The only
 * ground truth the passive receiver has is the transport-block CRC: decoding WITH a pattern passes where decoding WITHOUT it fails (the pattern is real),
 * or the reverse (it is not). Evidence comes only from grants whose two decodes differ, so a grant that passes (or fails) both ways teaches nothing,
 * exactly as the LBRM layer-term hypotheses n_L of nr_pdsch_passive_decode.c latch only on a CRC pass.
 *
 * A pattern is identified by its geometry and type (TS 38.211 Table 7.4.1.5.3-1 row, frequency domain, symbols, CDM, density, RB extent, scrambling ID) and by its LATTICE VARIANT
 * (fapi last_trs_slot, unused by rate-matching PDUs): 0 is the slot lattice the detector proposed; variant k > 0 is the extra phase the lattice would have with a period halved k times
 * (period 80 offset 4 -> variant 1 = slots 44 mod 80). A detector that settled on a too-coarse period is then corrected by the decodes, phase by phase.
 *   - A pattern that has no sequence evidence (NR_CSIRS_PDU_HOLE_ONLY: a ZP found only as persistent holes) needs PROOF: it enters rate matching only
 *     after NR_CSI_ARB_MIN_WINS decodes with it passed where decoding without it failed (verdict KEEP).
 *   - Every other pattern is in by default (its sequence or its ZP bank is the evidence) and leaves after NR_CSI_ARB_MIN_WINS decodes without it passed
 *     where decoding with it failed (verdict DROP).
 *   - Either way the evidence has to dominate the opposite evidence 2:1, and a proof-needing pattern whose added trials never win in
 *     NR_CSI_ARB_MAX_TRIALS tries is dropped (no more trials spent on it). The table is cleared on a configuration epoch change.
 */
#ifndef NR_CSIRS_RM_ARBITER_H
#define NR_CSIRS_RM_ARBITER_H
#include <stdbool.h>
#include <stdint.h>
#include "fapi_nr_ue_interface.h"
#ifdef __cplusplus
extern "C" {
#endif

/* fapi_nr_dl_config_csirs_pdu_rel15_t::measurement_bitmap bit 7 (rate-matching PDUs carry no measurement): the ZP pattern has no sequence evidence. */
#define NR_CSIRS_PDU_HOLE_ONLY 0x80
#define NR_CSI_ARB_MIN_WINS 2
#define NR_CSI_ARB_MAX_TRIALS 64

typedef enum { NR_CSI_ARB_UNDECIDED = 0, NR_CSI_ARB_KEEP, NR_CSI_ARB_DROP } nr_csi_arb_verdict_t;
typedef enum {
  NR_CSI_ARB_WITH_WINS,    /* decode with the pattern passed where decoding without it failed */
  NR_CSI_ARB_WITHOUT_WINS, /* decode without the pattern passed where decoding with it failed */
  NR_CSI_ARB_NO_EVIDENCE   /* the trial changed nothing (both failed): counts as a spent trial only */
} nr_csi_arb_outcome_t;

void nr_csi_arb_reset(void);
nr_csi_arb_verdict_t nr_csi_arb_verdict(const fapi_nr_dl_config_csirs_pdu_rel15_t *p);
/* true when the pattern belongs in the rate-matching list of a decode */
bool nr_csi_arb_applies(const fapi_nr_dl_config_csirs_pdu_rel15_t *p);
/* true when a decode WITH the (currently excluded) pattern is worth a trial: it needs proof and has not been dropped */
bool nr_csi_arb_trial_add(const fapi_nr_dl_config_csirs_pdu_rel15_t *p);
/* true when a decode WITHOUT the (currently applied) pattern is worth a trial: its verdict is not KEEP */
bool nr_csi_arb_trial_remove(const fapi_nr_dl_config_csirs_pdu_rel15_t *p);
/* records a trial outcome; logs a verdict change. Returns the new verdict. */
nr_csi_arb_verdict_t nr_csi_arb_note(const fapi_nr_dl_config_csirs_pdu_rel15_t *p, nr_csi_arb_outcome_t o);
/* counters of a pattern (tests, logs) */
void nr_csi_arb_counts(const fapi_nr_dl_config_csirs_pdu_rel15_t *p, uint32_t *with_wins, uint32_t *without_wins, uint32_t *trials);

#ifdef __cplusplus
}
#endif
#endif
