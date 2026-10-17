/* Pure helpers of nr_ul_nid_bg.h (no dependencies; unit-tested by nr_ul_nid_rule_test.cc). */
#include "nr_ul_nid_rule.h"

/* ---------------------------------------------------------------- pure helpers */
int nr_ul_nid_riv_bits(int n)
{
  const long v = (long)n * (n + 1) / 2;
  int b = 0;
  while ((1L << b) < v) b++;
  return b;
}
bool nr_ul_nid_riv_decode(uint32_t riv, int n, int *s, int *l)
{
  if (n < 1) return false;
  const int q = (int)(riv / (uint32_t)n), r = (int)(riv % (uint32_t)n);
  if (q <= n / 2 && r + q + 1 <= n) { /* (L-1) <= floor(N/2): RIV = N (L-1) + S */
    *s = r;
    *l = q + 1;
    return true;
  }
  const int L = n - q + 1, S = n - 1 - r; /* RIV = N (N-L+1) + (N-1-S) */
  if (L - 1 > n / 2 && L >= 1 && S >= 0 && S + L <= n) {
    *s = S;
    *l = L;
    return true;
  }
  return false;
}
int nr_ul_nid_twins(uint32_t riv, int n, int span, uint16_t *on, uint16_t *os, uint16_t *ol, int cap)
{
  int k = 0;
  const int bits = nr_ul_nid_riv_bits(n);
  for (int d = 0; d <= span && k < cap; d++)
    for (int sgn = 0; sgn < (d ? 2 : 1) && k < cap; sgn++) {
      const int m = sgn ? n + d : n - d;
      int s, l;
      if (m < 1 || nr_ul_nid_riv_bits(m) != bits || !nr_ul_nid_riv_decode(riv, m, &s, &l)) continue;
      on[k] = (uint16_t)m;
      os[k] = (uint16_t)s;
      ol[k] = (uint16_t)l;
      k++;
    }
  return k;
}
bool nr_ul_nid_confirm_rule(nr_ul_nid_pend_t *p, bool strong, int64_t abs_slot)
{
  if (strong) return true;
  for (int i = 0; i < p->n; i++)
    if (p->slot[i] != abs_slot) return true; /* a second, different slot */
  if (p->n < 2) p->slot[p->n++] = abs_slot;
  return false;
}

