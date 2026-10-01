/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_TD_LEGAL_H
#define NR_TD_LEGAL_H
#include <stdbool.h>
#include <stdint.h>
#include "nr_pdsch_config_sweep.h"
#ifdef __cplusplus
extern "C" {
#endif
#define NR_TD_WORDS ((NR_PDSCH_SWEEP_MAX_HYP + 63) / 64)
typedef struct {
  uint64_t w[NR_TD_WORDS];
} nr_td_mask_t;
static inline void nr_td_mask_set(nr_td_mask_t *m, int i)
{
  m->w[i >> 6] |= 1ull << (i & 63);
}
static inline bool nr_td_mask_get(const nr_td_mask_t *m, int i)
{
  return (m->w[i >> 6] >> (i & 63)) & 1;
}
/* dst &= a over the first n_hyp bits. */
void nr_td_mask_and(nr_td_mask_t *dst, const nr_td_mask_t *a, int n_hyp);
/* Number of set bits among the first n_hyp. */
int nr_td_mask_count(const nr_td_mask_t *m, int n_hyp);

/* Geometry of one code-block layout. tbs and G are in bits; Ncb is the circular buffer length
 * min(N, 3*Tbslbrm/(2C)) (N = 66Z for BG1, 50Z for BG2). */
typedef struct {
  uint32_t G;
  uint32_t tbs;
  int qm, nl, C, K, F, Zc, BG;
  uint32_t Ncb;
  int rv;
} nr_td_rm_geom_t;
/* Rate-matching feasibility: the same predicates that make the RX rate recovery reject a geometry
 * (nr_rate_matching_ldpc_rx: C == 0, Foffset > Ncb with Foffset = K-F-2Z as uint32), evaluated before decoding,
 * plus G > 0 && tbs > 0. "Foffset > E" is a TX-only check (nr_rate_matching_ldpc) and is deliberately NOT applied:
 * the RX side tolerates it (every loop is bounded by k < E). Mathematically guaranteed exclusion only. */
bool nr_td_rm_feasible(const nr_td_rm_geom_t *g);

/* Computational signature: encodes the PHY properties that hypotheses must share to share the FEP,
 * channel estimate, equalization, and LLR work. Hypotheses with equal signature differ only in TBS,
 * rate-matching, and LDPC decoding (MCS-table dependent). qm is the modulation order the hypothesis
 * implies for the grant's MCS table entry; mcs_table enters only through qm. */
uint64_t nr_td_signature(const nr_pdsch_cfg_hypothesis_t *h, int nl, int qm);

/* Count the distinct signatures in a catalog. qm_per_hyp provides the modulation order for each
 * hypothesis (may vary per hypothesis). Returns the number of distinct signature values (0 for an
 * empty catalog), or -1 on error (n < 0, NULL hyp/qm_per_hyp with n > 0, or allocation failure). */
int nr_td_count_signatures(const nr_pdsch_cfg_hypothesis_t *hyp, int n, int nl, const int *qm_per_hyp);
#ifdef __cplusplus
}
#endif
#endif
