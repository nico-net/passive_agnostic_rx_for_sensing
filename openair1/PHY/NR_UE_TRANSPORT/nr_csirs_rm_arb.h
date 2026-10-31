/* CSI-RS / TRS PDSCH rate-matching set that works on any cell (fix1031/rm, audit D5).
 *
 * The rate-matching RE list of a PDSCH grant used to be ONE rule, "resources whose scramblingID equals the grant's DM-RS scrambling ID (plus the
 * co-block ones) are rate-matched, every other resource is erased", calibrated on the six TRS IDs of PCI 415. On a cell whose TRS carries another
 * scrambling ID than its PDSCH DM-RS the rule erases the cell's own TRS as data (stream shift, all-CB failures at good SNR).
 * Nothing about WHICH relation a cell uses is assumed here: a small family of candidate sets is built from what the receiver measured in the slot
 * (confirmed resources, TRS partners, discovery energy footprints), the TB CRC arbitrates, and the winner is cached per (RNTI, slot class, DCI
 * rate-matching-indicator / ZP-trigger value).
 *
 * Candidate kinds (a kind whose RE sets equal an earlier kind's is dropped):
 *   NR_RM_OWN        the former rule (scramblingID == DM-RS ID or inside the RB block of such a resource), the rest erased (always first)
 *   NR_RM_ALL_CONF   every confirmed resource of the slot rate-matched, nothing erased
 *   NR_RM_ALL_FOOT   NR_RM_ALL_CONF plus the discovery footprints (generic RE patterns, DM-RS symbols removed), fragments of one resource merged
 *   NR_RM_ALL_WIDE   NR_RM_ALL_FOOT with every resource merged from >= 2 fragments widened to the BWP (a TRS is one wideband resource)
 *   NR_RM_NONE       nothing rate-matched, nothing erased
 * Consolidation: entries of one slot with the same pattern (type, row, symbols, subcarriers, density, scramblingID) whose RB ranges overlap or are
 * separated by less than the narrower range are fragments of one resource whose detection has holes; they become their RB hull.
 *
 * Arbitration: the first decode of a grant uses the key's adopted kind (RNTI entry, else the cell entry), else NR_RM_OWN. A strong-DM-RS failure
 * tries the other kinds; each CRC pass is a vote; a kind is adopted when it has >= 2 passes (cell entry: 3) and strictly more than any other.
 * 3 consecutive failures of the adopted kind drop its votes (re-test). Slots with a single candidate are never arbitrated (cls == 0).
 * State is per cell, one mutex per cell. */
#ifndef NR_CSIRS_RM_ARB_H
#define NR_CSIRS_RM_ARB_H
#include <stdbool.h>
#include <stdint.h>
#include "nfapi/open-nFAPI/nfapi/public_inc/fapi_nr_ue_interface.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { NR_RM_OWN = 0, NR_RM_ALL_CONF = 1, NR_RM_ALL_FOOT = 2, NR_RM_ALL_WIDE = 3, NR_RM_NONE = 4, NR_RM_NKIND = 5 };
#define NR_RM_MAXL NFAPI_MAX_NUM_CSI_RATEMATCH
#define NR_RM_ADOPT_RNTI 2
#define NR_RM_ADOPT_CELL 3
#define NR_RM_RETEST_STREAK 3

typedef struct {
  fapi_nr_dl_config_csirs_pdu_rel15_t sel[NR_RM_MAXL]; ///< rate-matched around
  fapi_nr_dl_config_csirs_pdu_rel15_t er[NR_RM_MAXL];  ///< erased (LLR 0)
  int n_sel, n_er;
  uint8_t kind; ///< NR_RM_*
} nr_rm_cand_t;

typedef struct {
  int n;        ///< candidates (>= 1; c[0] is NR_RM_OWN)
  uint32_t cls; ///< slot class (hash of the slot's raw resource list); 0 when n == 1 (nothing to arbitrate)
  nr_rm_cand_t c[NR_RM_NKIND];
} nr_rm_cands_t;

/* RE-set helpers shared with nr_csirs_blind_rt.c */
bool nr_rm_same_geometry(const fapi_nr_dl_config_csirs_pdu_rel15_t *a, const fapi_nr_dl_config_csirs_pdu_rel15_t *b);
bool nr_rm_dmrs_clash(const fapi_nr_dl_config_csirs_pdu_rel15_t *c, uint16_t dmrs_mask);
bool nr_rm_rb_overlap_half(const fapi_nr_dl_config_csirs_pdu_rel15_t *a, const fapi_nr_dl_config_csirs_pdu_rel15_t *b);

/** The former rule: entries of @p all with scramblingID == @p pop or inside the RB block of a confirmed NZP resource of @p pop go to sel, the others
 *  (NZP/REPAT only) to er; DM-RS-clashing entries and duplicates dropped. Either output list may be NULL. */
void nr_rm_select_own(const fapi_nr_dl_config_csirs_pdu_rel15_t *conf, int n_conf, const fapi_nr_dl_config_csirs_pdu_rel15_t *all, int n_all,
                      uint16_t pop, uint16_t dmrs_mask, fapi_nr_dl_config_csirs_pdu_rel15_t *sel, int *nsel, int max_sel,
                      fapi_nr_dl_config_csirs_pdu_rel15_t *er, int *ner, int max_er);

/** Merges fragments of one resource in place (see above); returns the new count. @p merged (optional, size n) gets the number of fragments
 *  each surviving entry was merged from. */
int nr_rm_consolidate(fapi_nr_dl_config_csirs_pdu_rel15_t *p, int n, uint8_t *merged);

/** Builds the candidate family for one grant. @p conf = confirmed resources (any slot, for the RB blocks), @p all = the slot's resources
 *  (confirmed occurrences, TRS partners, block members), @p foot = the slot's discovery footprints as RE patterns (may be NULL). The BWP bounds the
 *  widened kind. */
void nr_rm_build(const fapi_nr_dl_config_csirs_pdu_rel15_t *conf, int n_conf, const fapi_nr_dl_config_csirs_pdu_rel15_t *all, int n_all,
                 const fapi_nr_dl_config_csirs_pdu_rel15_t *foot, int n_foot, uint16_t pop, uint16_t dmrs_mask, uint16_t bwp_start,
                 uint16_t bwp_size, nr_rm_cands_t *cs);

/** Index into cs->c of the candidate the first decode of (rnti, key) uses. Touches the entry. */
int nr_rm_arb_first(uint16_t rnti, uint8_t rm_ind, uint8_t zp_trig, const nr_rm_cands_t *cs);
/** The next untried candidate (bit k of @p tried = cs->c[k]), cell-voted kinds first; -1 = none. */
int nr_rm_arb_next(uint16_t rnti, uint8_t rm_ind, uint8_t zp_trig, const nr_rm_cands_t *cs, unsigned tried);
/** CRC outcome of candidate @p k. @p from: index of the candidate whose first decode of this grant failed (-1 = k is the first decode); a pass
 *  after such a failure is a vote against @p from. @p strong: a failure counts toward the re-test streak only for strong-DM-RS grants. */
void nr_rm_arb_note(uint16_t rnti, uint8_t rm_ind, uint8_t zp_trig, const nr_rm_cands_t *cs, int k, int from, bool crc_ok, bool strong);
/** Adopted kind for (rnti, key) (-1 = none), tests / logs. rnti 0 = the cell entry. */
int nr_rm_arb_adopted(uint16_t rnti, uint32_t cls, uint8_t rm_ind, uint8_t zp_trig);
/** Counters since start: trials of alternates, passes of alternates, adoptions. */
void nr_rm_arb_stats(uint64_t *tries, uint64_t *passes, uint64_t *adopts);
void nr_rm_arb_reset(void);

#ifdef __cplusplus
}
#endif
#endif
