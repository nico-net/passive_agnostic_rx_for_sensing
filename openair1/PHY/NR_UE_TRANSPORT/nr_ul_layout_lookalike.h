/* BWP look-alike pairs of the UL layout lock (ISAC_UL_LOOKALIKE=1): two (N, start) hypotheses that cover the same RBs except
 * one edge RB (e.g. 216@1 vs 217@0). Votes whose probe does not touch that edge RB cannot tell them apart. Pure helpers. */
#ifndef NR_UL_LAYOUT_LOOKALIKE_H
#define NR_UL_LAYOUT_LOOKALIKE_H
#include <stdbool.h>
#include <stdint.h>

#define NR_UL_LOOKALIKE_MIN_VOTES 6   /* discriminating votes the winner needs (= LOCK_MIN of the lock) */
#define NR_UL_LOOKALIKE_RATIO 2u      /* ... and at least this multiple of the other's */

static inline bool nr_ul_layout_lookalike(uint16_t n_a, uint16_t s_a, uint16_t n_b, uint16_t s_b)
{
  const int dn = (int)n_a - (int)n_b, ds = (int)s_a - (int)s_b;
  return (dn == 1 || dn == -1) && ds >= -1 && ds <= 1 && ((int)s_a + n_a == (int)s_b + n_b || s_a == s_b);
}

static inline int nr_ul_lookalike_ov(int s0, int e0, int s1, int e1)
{
  const int lo = s0 > s1 ? s0 : s1, hi = e0 < e1 ? e0 : e1;
  return hi > lo ? hi - lo : 0;
}

/* the probe [probe_start, probe_start+probe_len) touches an RB inside exactly one of the two hypotheses */
static inline bool nr_ul_layout_discriminates(uint16_t n_a, uint16_t s_a, uint16_t n_b, uint16_t s_b, uint16_t probe_start,
                                              uint16_t probe_len)
{
  const int ps = probe_start, pe = probe_start + probe_len;
  const int is = s_a > s_b ? s_a : s_b, ie = (s_a + n_a) < (s_b + n_b) ? (s_a + n_a) : (s_b + n_b);
  const int oa = nr_ul_lookalike_ov(ps, pe, s_a, s_a + n_a), ob = nr_ul_lookalike_ov(ps, pe, s_b, s_b + n_b);
  const int oi = ie > is ? nr_ul_lookalike_ov(ps, pe, is, ie) : 0;
  return oa != oi || ob != oi;
}

/* 0 or 1 = the hypothesis to lock, -1 = no lock. Enough discriminating votes decide; otherwise the one carrying the
 * prior (DL BWP / SIB1 UL BWP) wins; no prior or both -> no lock. */
static inline int nr_ul_layout_resolve_lookalike(const uint32_t disc_votes[2], const uint8_t prior[2])
{
  const uint32_t a = disc_votes[0], b = disc_votes[1];
  if (a >= NR_UL_LOOKALIKE_MIN_VOTES && a >= NR_UL_LOOKALIKE_RATIO * b) return 0;
  if (b >= NR_UL_LOOKALIKE_MIN_VOTES && b >= NR_UL_LOOKALIKE_RATIO * a) return 1;
  if (prior[0] && !prior[1]) return 0;
  if (prior[1] && !prior[0]) return 1;
  return -1;
}
#endif
