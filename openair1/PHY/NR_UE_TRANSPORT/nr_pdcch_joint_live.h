#ifndef NR_PDCCH_JOINT_LIVE_H
#define NR_PDCCH_JOINT_LIVE_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* UNKNOWN-RNTI FALLBACK for the live PDCCH candidate worker (2026-09-24, OPT-IN, NOT validated on air).
 * ISAC_PDCCH_JOINT=1 enables it; default off.
 *
 * For a cell whose PDCCH scrambling depends on the C-RNTI (pdcch-DMRS-ScramblingID configured, n_RNTI = C-RNTI) a
 * candidate cannot be descrambled without the RNTI, so the ordinary polar+CRC decode fails on a real DCI. This
 * solves (DCI payload, C-RNTI) jointly from the LLRs (nr_pdcch_joint_solve.h) after a cheap scale-free pre-screen,
 * and applies the SAME admission tests nr_pdcch_blind_decode_raw_11() applies (plausible RNTI range; DL indicator).
 * `llr` is what the caller already descrambled with n_RNTI = pre_descrambled_rnti (the worker's configured or
 * alternate scrambling RNTI); nid = the CORESET's pdcch-DMRS-ScramblingID. */
typedef struct {
  uint64_t payload;
  uint16_t rnti;
  uint16_t mismatched_bits;
  const char *reject_reason;
} nr_pdcch_joint_live_result_t;

bool nr_pdcch_joint_live_enabled(void);
bool nr_pdcch_joint_live_decode_11(const int16_t *llr, uint8_t aggregation_level, uint16_t dci_length, uint16_t nid,
                                   int pre_descrambled_rnti, uint16_t rnti_min, uint16_t rnti_max,
                                   nr_pdcch_joint_live_result_t *out);
void nr_pdcch_joint_live_counts(unsigned long long *attempts, unsigned long long *prescreen_pass,
                                unsigned long long *accepted);

#ifdef __cplusplus
}
#endif
#endif
