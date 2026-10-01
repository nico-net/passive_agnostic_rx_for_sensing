/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_td_legal.h"
#include <string.h>
#include <stdlib.h>

void nr_td_mask_and(nr_td_mask_t *dst, const nr_td_mask_t *a, int n_hyp)
{
  const int nw = (n_hyp + 63) / 64;
  for (int i = 0; i < nw; i++)
    dst->w[i] &= a->w[i];
}

int nr_td_mask_count(const nr_td_mask_t *m, int n_hyp)
{
  int c = 0;
  const int nw = (n_hyp + 63) / 64;
  for (int i = 0; i < nw; i++) {
    uint64_t v = m->w[i];
    if (i == nw - 1 && (n_hyp & 63))
      v &= (1ull << (n_hyp & 63)) - 1;
    c += __builtin_popcountll(v);
  }
  return c;
}

/* Reject conditions copied from the real RX path (evidence: tree read 2026-10-01).
 * openair1/PHY/CODING/nrLDPC_coding/nrLDPC_coding_segment/nr_rate_matching.c, nr_rate_matching_ldpc_rx:
 *   :612  if (C == 0) { LOG_E("invalid parameter C"); return -1; }
 *   :618  uint32_t N = (BG == 1) ? (66 * Z) : (50 * Z);
 *   :620-:624  if (Tbslbrm == 0) Ncb = N; else { Nref = 3 * Tbslbrm / (2 * C); Ncb = min(N, Nref); }   (R_LBRM = 2/3)
 *   :627  ind = (index_k0[BG - 1][rvidx] * Ncb / N) * Z;       (rv-dependent k0; not itself a reject)
 *   :637  if (Foffset > Ncb) { LOG_E("invalid parameters (Foffset %d > Ncb %d)"); return -1; }   (LOG_E at :638)
 *   NOT a reject on RX: Foffset > E (the TX-side nr_rate_matching_ldpc has it at :539-:552 (Foffset > E at :539, Foffset > Ncb at :550); see the RX comment).
 * nrLDPC_coding_segment_decoder.c:178-192:
 *   nr_rate_matching_ldpc_rx(tbslbrm, BG, Z, d, harq_e, C, rv_index, clear, E, F, K - F - 2 * Z) == -1
 *     -> "Problem in rate_matching BG .. Z .. C .. rv_index .. E .. F .. K.. K-F-2*Z"
 *   Foffset is passed as uint32_t: a negative K - F - 2Z wraps to a huge value and is rejected too.
 * Per-CB E (nr_tbs_tools.c nr_get_E): E = Nl*Qm*floor(G/(Nl*Qm*C)), +Nl*Qm for the first-listed blocks
 *   r > C-((G/(Nl*Qm))%C)-1; E does not enter the RX reject predicate, so it is not evaluated here. */
bool nr_td_rm_feasible(const nr_td_rm_geom_t *g)
{
  if (g->G == 0 || g->tbs == 0 || g->C < 1 || g->C > 255) /* the real C parameter is uint8_t */
    return false;
  const uint32_t Foffset = (uint32_t)(g->K - g->F - 2 * g->Zc);
  return Foffset <= g->Ncb;
}

/* Computational signature: encodes the PHY properties that hypotheses must share to share the FEP,
 * channel estimate, equalization, and LLR work. mcs_table enters only through qm (the modulation
 * order the hypothesis implies for the grant's MCS table entry). */
uint64_t nr_td_signature(const nr_pdsch_cfg_hypothesis_t *h, int nl, int qm)
{
  uint64_t sig = h->tda_start
                 | ((uint64_t)h->tda_length << 4)
                 | ((uint64_t)h->k0 << 8)
                 | ((uint64_t)h->mapping_type << 14)
                 | ((uint64_t)h->dmrs_mask << 16)
                 | ((uint64_t)h->dmrs_max_len << 30)
                 | ((uint64_t)nl << 32)
                 | ((uint64_t)qm << 36);
  return sig;
}

/* Comparator for qsort of uint64_t values */
static int nr_td_sig_cmp(const void *a, const void *b)
{
  uint64_t va = *(const uint64_t *)a;
  uint64_t vb = *(const uint64_t *)b;
  return (va < vb) ? -1 : (va > vb) ? 1 : 0;
}

/* Count the distinct signatures in a catalog. */
int nr_td_count_signatures(const nr_pdsch_cfg_hypothesis_t *hyp, int n, int nl, const int *qm_per_hyp)
{
  if (n <= 0)
    return 0;

  /* Allocate array of signatures and compute them */
  uint64_t *sigs = malloc(n * sizeof(uint64_t));
  if (!sigs)
    return 0;

  for (int i = 0; i < n; i++)
    sigs[i] = nr_td_signature(&hyp[i], nl, qm_per_hyp[i]);

  /* Sort and count distinct values */
  qsort(sigs, n, sizeof(uint64_t), nr_td_sig_cmp);

  int count = 1;
  for (int i = 1; i < n; i++) {
    if (sigs[i] != sigs[i - 1])
      count++;
  }

  free(sigs);
  return count;
}
