/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Standard-conformant rules for UE re-attachment, RRC processing delay, BWP switch delay and RRC_INACTIVE/IDLE linking.
 * Pure C (no OAI dependency, unit-tested in tests/nr_passive_reconf_rules_test.cc). Clause numbers are written from the specification text
 * as remembered, not re-read against the TS in this session: [RECALLED] until checked. The consumers are nr_passive_ue_ctx.c (incarnations,
 * links, RRC windows), nr_pdcch_blind_monitor_rt.c (DCI-triggered BWP switch) and nr_pdsch_passive_queue.c / nr_pusch_passive_queue.c (Msg4 / Msg3). */
#ifndef NR_PASSIVE_RECONF_RULES_H
#define NR_PASSIVE_RECONF_RULES_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* ---- identity of a UE carried in the 48-bit UL CCCH SDU (Msg3), TS 38.331 UL-CCCH-Message (RRCSetupRequest / RRCResumeRequest) ---- */
typedef enum { NR_RECONF_ID_NONE = 0, NR_RECONF_ID_SETUP_RANDOM, NR_RECONF_ID_SETUP_STMSI, NR_RECONF_ID_RESUME } nr_reconf_id_kind_t;
typedef struct {
  nr_reconf_id_kind_t kind;
  uint64_t value; /* 39-bit InitialUE-Identity, or the 24-bit ShortI-RNTI-Value */
} nr_reconf_id_t;
bool nr_reconf_ccch_identity(const uint8_t sdu[6], nr_reconf_id_t *out);
/* A identity that can recur across attachments of one UE: S-TMSI part 1 (stable) and the resume identity (I-RNTI). A random value never recurs. */
bool nr_reconf_id_linkable(const nr_reconf_id_t *id);
bool nr_reconf_id_equal(const nr_reconf_id_t *a, const nr_reconf_id_t *b);
/* TS 38.321 5.1.5: contention resolution succeeds when the UE Contention Resolution Identity (6 octets of the DL-SCH MAC CE) equals the first 48 bits
 * of the CCCH SDU the UE sent in Msg3. */
bool nr_reconf_cr_matches(const uint8_t msg3_ccch[6], const uint8_t cr_identity[6]);
/* The 48-bit UL CCCH SDU of a Msg3 MAC PDU (R/R/LCID 52 + 6 octets, or LCID 0 CCCH1 + 8 octets; TS 38.321 6.1.2, Table 6.2.1-2). */
bool nr_reconf_ulsch_ccch(const uint8_t *pdu, uint32_t len, uint8_t out[6]);
/* The same 48 bits packed into an integer (event payload). */
uint64_t nr_reconf_pack48(const uint8_t b[6]);
void nr_reconf_unpack48(uint64_t v, uint8_t b[6]);

/* ---- timing ---- */
typedef enum { NR_RECONF_RRC_SETUP = 0, NR_RECONF_RRC_RESUME, NR_RECONF_RRC_REESTABLISH, NR_RECONF_RRC_RECONF } nr_reconf_rrc_kind_t;
/* TS 38.331 12 (RRC procedure delay): the new dedicated configuration is applied within 10 ms (RRCSetup, RRCResume, RRCReestablishment) or
 * 16 ms (RRCReconfiguration); with_bwp_switch adds the BWP switch delay of 38.133 8.6.2. Result in slots of numerology mu (rounded up). */
int nr_reconf_rrc_delay_slots(nr_reconf_rrc_kind_t kind, int mu, bool with_bwp_switch);
/* TS 38.133 8.6.2 + TS 38.213 12: BWP switch delay in slots, FR1. type 1 or 2 is a UE capability (bwp-SwitchingDelay, TS 38.306) and invisible
 * passively: [min, max] = [type 1, type 2]. */
int nr_reconf_bwp_switch_delay_slots(int mu, int type);
/* TS 38.321 5.3.2.2 / 38.214 5.1.3.1: a DL assignment whose NDI did not toggle is a retransmission of the SAME transport block. Both transmissions
 * decoded with a passing CRC but with different TBS and an untoggled NDI: the HARQ process was reset (a new UE on a reused RNTI). */
bool nr_reconf_hq_discontinuity(bool prev_valid, uint8_t prev_ndi, int prev_tbs_bits, uint8_t ndi, int tbs_bits);

/* ---- time constants shared with the UeContext (nanoseconds) ---- */
#define NR_RECONF_REUSE_NS 10000000000ull   /* an RNTI sighted again within this window may be a new UE; beyond it the context is IDLE/INACTIVE anyway */
#define NR_RECONF_REATTACH_GAP_NS 200000000ull /* silence after which a DCI size change is read as a new attachment, not a reconfiguration */
#define NR_RECONF_INACTIVE_NS 10000000000ull /* silence after which a UE is IDLE/INACTIVE (TS 38.331 5.3.8 release, 38.321 DRX, no passive way to tell) */
#define NR_RECONF_GONE_NS 60000000000ull
#define NR_RECONF_HQ_VIOLATIONS 2           /* distinct HARQ processes showing the discontinuity ... */
#define NR_RECONF_HQ_WINDOW_NS 500000000ull /* ... within this time */
#define NR_RECONF_RRC_MIN_SDU 40            /* SRB1 DL SDU bytes from which a ciphered message is read as RRCReconfiguration-sized [INFERRED] */

/* ---- RRC processing windows per RNTI (lock-free; written by the PDSCH consumer, read by the PDCCH real-time thread) ----
 * HOLD  : message received, delay not elapsed - the dedicated configuration may change in any slot of the window; the UE context only re-verifies at its end.
 * ARMED : delay elapsed (for NR_RECONF_ARM_SLOTS_MS) - a change, if any, has happened: re-lock quickly. */
#define NR_RECONF_ARM_MS 100
#define NR_RECONF_ARM_N_SUSPECT 16
typedef enum { NR_RECONF_PHASE_NONE = 0, NR_RECONF_PHASE_HOLD, NR_RECONF_PHASE_ARMED } nr_reconf_phase_t;
void nr_reconf_rrc_expect(uint16_t rnti, int64_t rx_slot, nr_reconf_rrc_kind_t kind, int mu, bool with_bwp_switch);
nr_reconf_phase_t nr_reconf_rrc_phase(uint16_t rnti, int64_t abs_slot, int mu);
/* Misses-before-SUSPECT for one RNTI at this slot: unchanged outside windows and in HOLD, at most NR_RECONF_ARM_N_SUSPECT when ARMED. */
uint32_t nr_reconf_rrc_n_suspect(uint16_t rnti, int64_t abs_slot, int mu, uint32_t base);
int64_t nr_reconf_rrc_effective_slot(uint16_t rnti);

/* ---- DCI-triggered BWP switches (TS 38.212 7.3.1.1.2 indicator, switch applied after the delay above) ---- */
void nr_reconf_bwp_switch_dci(uint16_t rnti, int to_entry, int64_t dci_slot, int mu);
/* Next switch whose maximum delay has elapsed at abs_slot; false when none. The caller raises the epoch / re-locks. */
bool nr_reconf_bwp_due(int64_t abs_slot, uint16_t *rnti, int *entry);
/* The slot window [first, last] in which grants on the new BWP are expected for rnti (false: no pending switch). */
bool nr_reconf_bwp_expected_window(uint16_t rnti, int64_t *first, int64_t *last);
void nr_reconf_reset(void); /* tests and receiver lifetime */

#ifdef __cplusplus
}
#endif
#endif
