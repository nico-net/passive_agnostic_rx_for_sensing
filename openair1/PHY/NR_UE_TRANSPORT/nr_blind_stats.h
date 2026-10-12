/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* BLINDFIX (2026-10-12): scale-invariant, false-alarm-bounded replacements for cell-fitted thresholds
 * (docs/superpowers/reports/csd/BLINDFIX-20261012.md). Header-only, pure functions; every consumer is behind its own
 * default-off env flag. */
#ifndef NR_BLIND_STATS_H
#define NR_BLIND_STATS_H
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

/* Exact P(X >= k) for X ~ Binomial(n, p). */
static inline double nbs_binom_sf(uint64_t k, uint64_t n, double p)
{
  if (k == 0) return 1.0;
  if (k > n || p <= 0.0) return 0.0;
  if (p >= 1.0) return 1.0;
  const double lp = log(p), lq = log1p(-p);
  double s = 0.0;
  for (uint64_t i = k; i <= n; i++) {
    const double t = lgamma((double)n + 1) - lgamma((double)i + 1) - lgamma((double)(n - i) + 1) + (double)i * lp + (double)(n - i) * lq;
    const double e = exp(t);
    s += e;
    if (i > k + 8 && e < s * 1e-17) break; /* tail terms are decreasing past the mode */
  }
  return s > 1.0 ? 1.0 : s;
}

/* A wrong hypothesis passes a CRC-24 by chance with p = 2^-24 (TS 38.212 5.1; the TB CRC of an LDPC decode). */
#define NBS_CRC_FALSE_PASS (1.0 / 16777216.0)

/* Do `ok` passes in `n` trials beat chance (a wrong hypothesis) at level alpha? */
static inline bool nbs_beats_chance(uint64_t ok, uint64_t n, double alpha)
{
  return ok > 0 && nbs_binom_sf(ok, n, NBS_CRC_FALSE_PASS) < alpha;
}

/* Is best (ok_b/n_b) better than runner-up (ok_s/n_s) at level alpha? Conditional exact test: given the k = ok_b + ok_s
 * passes, under "equally good" each lands on best with probability n_b / (n_b + n_s). */
static inline bool nbs_beats_runner(uint64_t ok_b, uint64_t n_b, uint64_t ok_s, uint64_t n_s, double alpha)
{
  const uint64_t k = ok_b + ok_s;
  if (ok_b == 0 || n_b == 0) return false;
  if (n_s == 0) return true; /* no runner-up evidence at all */
  return nbs_binom_sf(ok_b, k, (double)n_b / (double)(n_b + n_s)) < alpha;
}

/* F8: the sweep decision. alpha is split over n_active hypotheses (Bonferroni). */
static inline bool nbs_sweep_signif_enabled(void)
{
  const char *e = getenv("ISAC_SWEEP_SIGNIF");
  return e && atoi(e) != 0;
}
static inline bool nbs_sweep_winner(uint64_t ok_b, uint64_t n_b, uint64_t ok_s, uint64_t n_s, int n_active)
{
  const double a = 1e-3 / (double)(n_active > 1 ? n_active : 1);
  return nbs_beats_chance(ok_b, n_b, a) && nbs_beats_runner(ok_b, n_b, ok_s, n_s, a);
}

/* F3: the QPSK amplitude-consistency score s = (sum|x|)^2 / (n sum x^2) (nr_pdcch_scan_score.h) of n pure-noise soft
 * bits has mean 2/pi and, by the delta method, variance (8/pi - 24/pi^2)/n. A candidate whose score is below
 * mean + z sigma is indistinguishable from an empty CCE at false-alarm probability Q(z): scale-invariant, needs no
 * population statistics (the running-median gate rejected 100 % of a dense CORESET's candidates). */
static inline double nbs_cfar_score_min(int n_bits, double z)
{
  const double m = 2.0 / M_PI, v = 8.0 / M_PI - 24.0 / (M_PI * M_PI);
  return m + z * sqrt(v / (double)(n_bits > 1 ? n_bits : 1));
}

/* F1: number of slots in a 640-slot window where two periodic resources (period, up to two phases) both occur. */
static inline int nbs_joint_slots(unsigned pa, const uint16_t *fa, int na, unsigned pc, const uint16_t *fc, int nc)
{
  int j = 0;
  if (!pa || !pc) return 0;
  for (unsigned s = 0; s < 640; s++) {
    bool a = false, c = false;
    for (int i = 0; i < na; i++) a |= (s % pa) == fa[i];
    for (int i = 0; i < nc; i++) c |= (s % pc) == fc[i];
    j += a && c;
  }
  return j;
}

/* F7: evidence for the MCS table of a cell: full-TB CRC passes by table (0 = qam64, 1 = qam256, 2 = lowSE), counted only
 * on grants whose MCS reads differently in at least two tables. The latch needs the leader to beat the runner-up with the
 * sign test (equal chance per pass) at alpha 1e-3. */
typedef struct { uint32_t pass[3]; } nbs_table_ev_t;
static inline void nbs_table_ev_feed(nbs_table_ev_t *e, int table, bool discriminating)
{
  if (discriminating && table >= 0 && table < 3) e->pass[table]++;
}
static inline int nbs_table_ev_best(const nbs_table_ev_t *e)
{
  int b = 0;
  for (int t = 1; t < 3; t++) if (e->pass[t] > e->pass[b]) b = t;
  uint32_t s = 0;
  for (int t = 0; t < 3; t++) if (t != b && e->pass[t] > s) s = e->pass[t];
  return nbs_beats_runner(e->pass[b], 1, s, 1, 1e-3 / 2) ? b : -1;
}
#endif
