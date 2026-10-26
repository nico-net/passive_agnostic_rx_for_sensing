/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Passive UL receive-branch selection from what the device measures (replaces the fixed "branch 0 only" policy).
 *
 * MRC (y * conj(h), the combiner's weight is the branch's own channel estimate) already weights every branch by its own SNR, so a weak
 * but live branch adds gain and is never excluded. A branch is excluded only when its DM-RS shows no detectable signal against its own
 * measured noise: a dead or disconnected input, whose "channel" estimate is the smoothed noise floor and which would add noise
 * through its own estimate. The test is statistical, from the pilot count, not a power ratio:
 *   sig2   mean |H_smoothed|^2 per RE of the branch, resid2 mean |LS - H_smoothed|^2 (its noise, per RE),
 *   sigma2 = resid2 / (1 - g), signal power h2 = sig2 - g * sigma2   (nr_ulsch_dmrs_snr_db: smoother noise gain g)
 *   with no signal sig2 is the mean of n_eff = cnt * g independent exponential variables of mean g * sigma2 (cnt = LS-vs-smoothed
 *   differences used for resid2; one independent noise sample per 1/g of them), so sd(sig2) = g * sigma2 / sqrt(n_eff).
 *   The branch is live when h2 exceeds z_alpha standard deviations: h2 > z * sigma2 * sqrt(g / cnt)  (one-sided, alpha = 0.1 %,
 *   Bonferroni over the branches).
 * A branch without a noise estimate (cnt 0) is kept: no information is not evidence of a dead branch. If no branch passes, all are kept.
 * A table of the per-branch live fraction (nr_ul_branch_tab_t) serves the GPU pass, whose branch set is fixed before its own channel
 * estimate exists: all branches when the whole table is live, a single branch when only one is, otherwise the CPU path decides per grant.
 * Header-only and pure so the unit test links it directly. */
#ifndef NR_UL_BRANCH_SELECT_H
#define NR_UL_BRANCH_SELECT_H
#include <stdint.h>
#include <math.h>

#define NR_UL_BRANCH_MAX 8
#define NR_UL_BRANCH_SMOOTH_G 0.125   /* = NR_ULSCH_SMOOTH_G (nr_ulsch_passive_branch.h): type-1 DM-RS frequency smoother noise gain */
#define NR_UL_BRANCH_ALPHA 1e-3       /* false-exclusion probability of a live branch (per grant, all branches together) */
#define NR_UL_BRANCH_TAB_MIN 8u       /* grants measured before the table is trusted */
#define NR_UL_BRANCH_TAB_SHIFT 3      /* exponential smoothing 1/8 */

/* one-sided normal quantile for small tail probabilities (Abramowitz-Stegun 26.2.23, |err| < 5e-4) */
static inline double nr_ul_branch_zq(double p)
{
  const double t = sqrt(-2.0 * log(p));
  return t - (2.515517 + 0.802853 * t + 0.010328 * t * t) / (1.0 + 1.432788 * t + 0.189269 * t * t + 0.001308 * t * t * t);
}

/* is the branch's DM-RS detectable against its own noise? n = branches sharing the alpha budget */
static inline int nr_ul_branch_live(double sig2, double resid2, uint32_t cnt, int n)
{
  if (!(resid2 > 0.0) || cnt == 0) return 1; /* no noise estimate: no evidence of a dead branch */
  const double g = NR_UL_BRANCH_SMOOTH_G;
  const double sigma2 = resid2 / (1.0 - g);
  const double h2 = sig2 - g * sigma2;
  const double z = nr_ul_branch_zq(NR_UL_BRANCH_ALPHA / (double)(n > 0 ? n : 1));
  return h2 > z * sigma2 * sqrt(g / (double)cnt);
}

/* bit mask of the branches to combine. sig2[a] = mean |H|^2 (max over layers), resid2[a] = mean noise per RE, cnt[a] = samples of resid2. */
static inline uint32_t nr_ul_branch_select(const double *sig2, const double *resid2, const uint32_t *cnt, int n)
{
  if (n <= 0 || n > NR_UL_BRANCH_MAX || !sig2 || !resid2 || !cnt) return 0;
  uint32_t m = 0;
  for (int a = 0; a < n; a++)
    if (nr_ul_branch_live(sig2[a], resid2[a], cnt[a], n)) m |= 1u << a;
  return m ? m : (1u << n) - 1u;
}

typedef struct {
  uint32_t n_seen;
  uint32_t live[NR_UL_BRANCH_MAX]; /* smoothed live fraction, 16.16 fixed point */
} nr_ul_branch_tab_t;

static inline void nr_ul_branch_tab_note(nr_ul_branch_tab_t *t, uint32_t mask, int n)
{
  if (!t || n <= 0 || n > NR_UL_BRANCH_MAX) return;
  for (int a = 0; a < n; a++) {
    const int64_t x = (mask >> a) & 1u ? 65536 : 0;
    if (!t->n_seen) t->live[a] = (uint32_t)x;
    else t->live[a] = (uint32_t)((int64_t)t->live[a] + ((x - (int64_t)t->live[a]) >> NR_UL_BRANCH_TAB_SHIFT));
  }
  t->n_seen++;
}

/* the branch set for a fixed-set consumer: -1 = all branches, >= 0 = that single branch, -2 = not decided (too few grants, or a mixed
 * set): the per-grant CPU path decides. A branch counts as live when it was live in the majority of the recent grants. */
static inline int nr_ul_branch_tab_keep(const nr_ul_branch_tab_t *t, int n)
{
  if (!t || n <= 1 || n > NR_UL_BRANCH_MAX) return -1;
  if (t->n_seen < NR_UL_BRANCH_TAB_MIN) return -2;
  uint32_t m = 0;
  for (int a = 0; a < n; a++) if (t->live[a] > 32768u) m |= 1u << a;
  const uint32_t all = (1u << n) - 1u;
  if (m == all || !m) return -1;
  if (!(m & (m - 1u))) return __builtin_ctz(m);
  return -2;
}

#ifdef __cplusplus
extern "C" {
#endif
/* nr_ulsch_demodulation.c: the table fed by every passive grant */
int nr_ulsch_passive_branch_keep(int nb_rx_ant);
void nr_ulsch_passive_branch_note(uint32_t mask, int nb_rx_ant);
#ifdef __cplusplus
}
#endif
#endif
