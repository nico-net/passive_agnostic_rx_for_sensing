#ifndef NR_PDCCH_USS_PRUNE_H
#define NR_PDCCH_USS_PRUNE_H

#include <stdbool.h>
#include <stdint.h>

/* USS hash pruning of the dedicated blind PDCCH scan (TS 38.213 10.1). Per cell: the hash parameters shared by every UE of a CORESET
 * geometry (N_CCE, p mod 3, M_L per AL) are FITTED from CRC-accepted DCIs; a known RNTI is then decoded only at its own hashed positions.
 * Falsifiable: 1 occasion in 16 decodes every position anyway (audit); a DCI an RNTI shows outside its hashed set demotes that RNTI back
 * to full search, and a run of such misses with no in-set audit hit resets the geometry's fit. Pruning never decides a CRC. */

#define NR_USS_PRUNE_MAX_POS 8 /* M_L is fitted up to 8 */

typedef struct {
  uint64_t pruned;         /* (RNTI, candidate) polar tasks skipped by the hash */
  uint64_t audit_tasks;    /* tasks the hash would have skipped but the audit occasion decoded */
  uint64_t audit_miss;     /* CRC-accepted DCI outside its RNTI's hashed set (hypothesis falsified for that RNTI) */
  uint64_t rnti_demotions; /* RNTIs returned to full search */
  uint64_t rnti_repromotions;
  uint64_t geom_resets;    /* fit discarded after a run of misses with no in-set hit */
  uint64_t fits;           /* successful refits (hypotheses > 0) */
} nr_uss_prune_stats_t;

/* TS 38.213 10.1: Y_p,-1 = n_RNTI, Y_p,n = (A_p * Y_p,n-1) mod 65537 (D); A_p = 39827 / 39829 / 39839 for p mod 3 = 0 / 1 / 2. Slot n = 0..slot. */
uint32_t nr_uss_hash_Y(uint16_t rnti, int slot, uint32_t A);
/* Candidate m of AL L (n_CI = 0): L * ((Y + floor(m * N_CCE / (L * M_L))) mod floor(N_CCE / L)). Writes the distinct CCEs of m = 0..M-1, returns their count. */
int nr_uss_hash_positions(uint16_t rnti, int slot, int L, int N, uint32_t A, int M, uint16_t *cce_out);
bool nr_uss_hash_in(uint16_t rnti, int slot, int cce, int L, int N, uint32_t A, int M);

/* Admission of one (RNTI, candidate) task of CORESET geometry `geom`. true = decode it. false = the hash proves it cannot carry this RNTI's DCI.
 * `audit` (an audit occasion) admits and counts it. Always true while the geometry has no fit, or for a demoted RNTI. */
bool nr_uss_prune_admit(uint64_t geom, uint16_t rnti, int slot, int cce, int L, bool audit);
/* admit() without counters (fit-dependent consumers that only need the answer). */
bool nr_uss_prune_allows(uint64_t geom, uint16_t rnti, int slot, int cce, int L);
/* Deterministic audit occasion: 1 absolute slot in 16, a pure function of the slot. */
bool nr_uss_prune_audit_slot(uint32_t abs_slot);
/* Record one CRC-accepted known-RNTI DCI (slot = slot in frame). force (UL DCI sharing the USS): refit now; if no hypothesis would survive the
 * sample is taken back and false is returned. A DL sample that no hypothesis would survive demotes its RNTI and is not fitted until the
 * geometry's misses accumulate (then the fit is reset and re-learned). */
bool nr_uss_prune_note(uint64_t geom, uint16_t rnti, int slot, int cce, int L, int ncce_max, bool force);
/* Cell counters (this cell). */
void nr_uss_prune_stats(nr_uss_prune_stats_t *out);
/* Number of fitted hypotheses of a geometry (0 = none; tests). */
int nr_uss_prune_hypotheses(uint64_t geom);
/* Forget everything (tests, cell restart). */
void nr_uss_prune_reset(void);

#endif
