#include "nr_pdsch_prb_set.h"
#include <stdbool.h>
#include <stddef.h>

int nr_rbg_size(int bwp_size, int rbg_config2)
{
  static const int lim[4] = {36, 72, 144, 275}, p1[4] = {2, 4, 8, 16}, p2[4] = {4, 8, 16, 16};
  for (int i = 0; i < 4; i++)
    if (bwp_size >= 1 && bwp_size <= lim[i])
      return rbg_config2 ? p2[i] : p1[i];
  return 0;
}

int nr_rbg_count(int bwp_start, int bwp_size, int P)
{
  if (P <= 0 || bwp_size <= 0 || bwp_start < 0)
    return 0;
  return (bwp_size + bwp_start % P + P - 1) / P;
}

int nr_ra_type0_prbs(uint32_t bitmap, int bwp_start, int bwp_size, int P, uint16_t *prb, int max)
{
  const int n_rbg = nr_rbg_count(bwp_start, bwp_size, P);
  if (n_rbg <= 0 || n_rbg > 32 || prb == NULL)
    return 0;
  const int off = bwp_start % P;
  int n = 0;
  for (int g = 0; g < n_rbg; g++) {
    if (!((bitmap >> (n_rbg - 1 - g)) & 1u))
      continue;
    const int first = g == 0 ? 0 : g * P - off;
    const int last = g == n_rbg - 1 ? bwp_size - 1 : (g + 1) * P - off - 1;
    for (int r = first; r <= last && n < max; r++)
      prb[n++] = (uint16_t)r;
  }
  return n;
}

int nr_fdra_dynamic_split(uint32_t field, int n_rbg, int riv_bits, uint32_t *type0_bitmap, uint32_t *riv)
{
  const int w = n_rbg > riv_bits ? n_rbg : riv_bits;
  if ((field >> w) & 1u) {
    if (riv)
      *riv = field & ((1u << riv_bits) - 1u);
    return 1;
  }
  if (type0_bitmap)
    *type0_bitmap = field & ((1u << n_rbg) - 1u);
  return 0;
}

int nr_vrb_to_prb_interleaved(int bwp_start, int bwp_size, int L, int vrb_start, int n_vrb, uint16_t *prb)
{
  if ((L != 2 && L != 4) || bwp_size <= 0 || bwp_start < 0 || vrb_start < 0 || n_vrb <= 0
      || vrb_start + n_vrb > bwp_size || prb == NULL)
    return 0;
  const int off = bwp_start % L;
  const int nb = (bwp_size + off + L - 1) / L, C = nb / 2;
  for (int i = 0; i < n_vrb; i++) {
    const int v = vrb_start + i;
    const int j = (v + off) / L;                            /* VRB bundle */
    const int f = (j == nb - 1) ? j : (j % 2) * C + j / 2;  /* PRB bundle: f(j) = rC + c, j = cR + r, R = 2 */
    const int vfirst = j == 0 ? 0 : j * L - off;
    const int pfirst = f == 0 ? 0 : f * L - off;
    prb[i] = (uint16_t)(pfirst + (v - vfirst));             /* bundles 0 and N-1 map to themselves, so sizes agree */
  }
  return n_vrb;
}

int nr_prb_segments(const uint16_t *prb, int n, int bwp_start, int prg, nr_prb_seg_t *seg, int max)
{
  if (prb == NULL || seg == NULL || n <= 0)
    return 0;
  int ns = 0;
  for (int i = 0; i < n; i++) {
    const bool cont = ns > 0 && prb[i] == prb[i - 1] + 1 && !(prg > 0 && (bwp_start + prb[i]) % prg == 0);
    if (cont) {
      seg[ns - 1].n_prb++;
      continue;
    }
    if (ns == max)
      return -1;
    seg[ns++] = (nr_prb_seg_t){prb[i], 1, (uint16_t)i};
  }
  return ns;
}
