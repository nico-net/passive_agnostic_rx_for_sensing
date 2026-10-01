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
#ifdef __cplusplus
}
#endif
#endif
