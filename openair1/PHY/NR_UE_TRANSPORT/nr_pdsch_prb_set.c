#include "nr_pdsch_prb_set.h"
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

bool nr_prb_list_normalise(const uint16_t *prb, int n, int bwp_size, uint32_t *bitmap, int bitmap_words, int *first,
                           int *last)
{
  if (prb == NULL || n <= 0 || n > NR_PRB_SET_MAX || bitmap_words <= 0 || bitmap_words > 16)
    return false;
  uint32_t bm[16] = {0};
  int lo = NR_PRB_SET_MAX, hi = -1;
  for (int i = 0; i < n; i++) {
    const int r = prb[i];
    if (r >= bwp_size || r >= 32 * bitmap_words || ((bm[r / 32] >> (r % 32)) & 1u))
      return false; /* outside the BWP, or listed twice */
    bm[r / 32] |= 1u << (r % 32);
    if (r < lo) lo = r;
    if (r > hi) hi = r;
  }
  memcpy(bitmap, bm, (size_t)bitmap_words * sizeof(*bitmap));
  *first = lo;
  *last = hi;
  return true;
}

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

int nr_fdra_rbg_size(int mode, int bwp_size)
{
  return mode == NR_FDRA_TYPE1 ? 0 : nr_rbg_size(bwp_size, mode == NR_FDRA_TYPE0_CFG2 || mode == NR_FDRA_DYN_CFG2);
}

int nr_fdra_bits(int mode, int n_rbg, int riv_bits)
{
  if (mode == NR_FDRA_TYPE1)
    return riv_bits;
  if (mode == NR_FDRA_TYPE0_CFG1 || mode == NR_FDRA_TYPE0_CFG2)
    return n_rbg;
  return 1 + (n_rbg > riv_bits ? n_rbg : riv_bits);
}

int nr_fdra_prbs(uint32_t field, int mode, int n_rbg, int riv_bits, int bwp_start, int bwp_size, uint16_t *prb,
                 int max, int *type0)
{
  uint32_t v = field;
  int t0 = mode == NR_FDRA_TYPE0_CFG1 || mode == NR_FDRA_TYPE0_CFG2;
  if (mode == NR_FDRA_DYN_CFG1 || mode == NR_FDRA_DYN_CFG2)
    t0 = !nr_fdra_dynamic_split(field, n_rbg, riv_bits, &v, &v);
  if (type0)
    *type0 = t0;
  if (t0)
    return v ? nr_ra_type0_prbs(v, bwp_start, bwp_size, nr_fdra_rbg_size(mode, bwp_size), prb, max) : 0;
  /* RIV, TS 38.214 5.1.2.2.2 (same decode as NRRIV2BW / NRRIV2PRBOFFSET) */
  const int N = bwp_size;
  if (N <= 0 || v >= (uint32_t)N * (uint32_t)(N + 1) / 2u)
    return 0;
  int L = (int)v / N + 1, S = (int)v % N;
  if (S + L > N) {
    L = N + 1 - (int)v / N;
    S = N - 1 - (int)v % N;
  }
  if (L < 1 || S < 0 || S + L > N)
    return 0;
  int n = 0;
  for (int i = 0; i < L && n < max; i++)
    prb[n++] = (uint16_t)(S + i);
  return n;
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

int nr_prb_gather_index(const nr_prb_seg_t *seg, int nseg, int re_per_prb, int *out, int max)
{
  int n = 0;
  for (int s = 0; s < nseg; s++)
    for (int re = seg[s].prb_start * re_per_prb; re < (seg[s].prb_start + seg[s].n_prb) * re_per_prb; re++) {
      if (n == max)
        return -1;
      out[n++] = re;
    }
  return n;
}
