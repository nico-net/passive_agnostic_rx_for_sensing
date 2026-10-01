/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_TD_FIELDBOOK_H
#define NR_TD_FIELDBOOK_H
#include <stdbool.h>
#include <stdint.h>
#include "nr_td_order.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { NR_TD_F_TDRA = 0, NR_TD_F_DMRS_ADD_POS, NR_TD_F_DMRS_MAX_LEN, NR_TD_F_COUNT } nr_td_field_t;
/* MCS table is deliberately NOT a field (UE-capability specific; never promoted). TDRA value packs S,L,mapping,k0. */
#define NR_TD_FB_MAX_RNTI 16
#define NR_TD_FB_MAX_CAND 4
/* One candidate value of a field with its own distinct-RNTI support set (order-independent promotion). */
typedef struct {
  int32_t value; /* -1 = free row */
  uint16_t support[NR_TD_FB_MAX_RNTI];
  int n_support;
  uint32_t born; /* creation tick, for oldest-first tie-break on eviction */
} nr_td_field_cand_t;
/* Field states: UNSEEN -> CANDIDATE -> PROMOTED -> SUSPECT -> PROMOTED | (withdrawn: back to CANDIDATE, counted). */
typedef enum { NR_TD_FS_UNSEEN = 0, NR_TD_FS_CANDIDATE, NR_TD_FS_PROMOTED, NR_TD_FS_SUSPECT } nr_td_field_state_t;
typedef struct {
  nr_td_field_state_t state;
  int32_t hint_value; /* value of the last PROMOTED/SUSPECT field at an epoch bump; ordering only, -1 = none */
  int32_t value; /* promoted value (PROMOTED/SUSPECT only), -1 = none */
  nr_td_field_cand_t cand[NR_TD_FB_MAX_CAND]; /* per-value support, incl. the promoted value's own row */
  uint32_t tick;
  uint16_t contra[NR_TD_FB_MAX_RNTI];
  int n_contra; /* distinct RNTIs contradicting `value` in the current epoch (cleared on epoch bump) */
  uint32_t epoch; /* config_epoch when last confirmed */
  uint64_t last_confirmed_slot;
} nr_td_field_entry_t;
typedef struct {
  nr_td_field_entry_t f[NR_TD_F_COUNT];
  uint32_t epoch;
  int promote_rntis; /* default 2 */
  int withdraw_rntis; /* default 2 */
  uint32_t generation; /* bumped on every field state change: promote, suspect, reconfirm, withdraw, epoch bump */
  uint32_t n_withdrawn;
} nr_td_fieldbook_t;
void nr_td_fieldbook_init(nr_td_fieldbook_t *fb, int promote_rntis, int withdraw_rntis);
int32_t nr_td_pack_tdra(int S, int L, int mapping, int k0);
/* A context for `rnti` converged on hypothesis `h` (call once per CONVERGED).
 * pruned_fields: bit f set = field f was pruned in that context (not independent): it neither supports nor contradicts f. */
void nr_td_fieldbook_converged(nr_td_fieldbook_t *fb, uint16_t rnti, const nr_pdsch_cfg_hypothesis_t *h, uint64_t slot,
                               uint32_t pruned_fields);
/* `rnti` produced contradiction evidence for field `f` (M-failures rule evaluated by the caller). */
void nr_td_fieldbook_contradict(nr_td_fieldbook_t *fb, uint16_t rnti, nr_td_field_t f);
/* Epoch bump: every PROMOTED/SUSPECT field becomes CANDIDATE with hint_value = value, value = -1; all support and
 * contradiction sets are cleared (old-epoch evidence never maintains pruning). */
void nr_td_fieldbook_bump_epoch(nr_td_fieldbook_t *fb);
nr_td_field_state_t nr_td_fieldbook_state(const nr_td_fieldbook_t *fb, nr_td_field_t f);
/* True only for PROMOTED (and sets *value). CALLER CONTRACT on SUSPECT (operator 2026-10-01): on SUSPECT, new contexts do
 * not prune on the field; existing UNSETTLED contexts immediately clear_dormant(FIELD_BASE + f) (only that field's cause);
 * converged contexts keep their winner and set an untrusted_fields bit. A caller that re-evaluates prunes() whenever
 * generation changes gets exactly this. The field book itself never touches sweep states. */
bool nr_td_fieldbook_prunes(const nr_td_fieldbook_t *fb, nr_td_field_t f, int32_t *value);
bool nr_td_fieldbook_hyp_matches(nr_td_field_t f, int32_t value, const nr_pdsch_cfg_hypothesis_t *h);
uint32_t nr_td_fieldbook_generation(const nr_td_fieldbook_t *fb);
/* simulator/test hook: PROMOTED, no supporters */
void nr_td_fieldbook_force_promote(nr_td_fieldbook_t *fb, nr_td_field_t f, int32_t value);
/* Copy each field's value (PROMOTED/SUSPECT) else its hint_value into side info (f_*, f_conf = 1.0). Ordering only. */
void nr_td_fieldbook_fill_side_info(const nr_td_fieldbook_t *fb, nr_td_side_info_t *si);
#ifdef __cplusplus
}
#endif
#endif
