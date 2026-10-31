/* SLOTPHASE residual estimator: learns, from measurements, WHICH resource elements a flagged slot phase carries that are not our PDSCH (a multi-port NZP CSI-RS the gNB
 * rate-matches around, a ZP resource, ...) and turns them into generic RE-pattern rate-match entries (nr_re_pattern.h) that the SLOTPHASE variant arbitration then
 * CRC-tests. No cell constants, no fixed thresholds: every cut is a statistical test against the cell's own reference.
 *
 * Input per decoded grant (GPU equaliser output, k_tail RESID block, nr_pdsch_evm_acc.h nr_pdsch_resid_t): per OFDM symbol, the layer-0 decision-directed error sum
 * e |z/u - lattice point|^2 and RE count n of 48 position keys (subcarrier 0..11 + 12 * (CRB mod 4)) and of 36 buckets of 8 CRBs. A grant is first made scale-free: the mean
 * error of every key bin with >= NR_SPR_MIN_RE REs is divided by the median of its own key bins (the grant's EVM "elsewhere"; robust while fewer than half of its REs
 * are foreign), giving rho(sym, key). Each bin then accumulates, over grants, n, sum rho and sum rho^2 in one of three tables:
 *   FAIL - strong-DM-RS grants of the flagged phase whose CRC failed (decoded as always, no rate-match hypothesis),
 *   REF A/B - strong-DM-RS grants whose CRC passed anywhere in the cell (alternating, so that the two halves give the estimator's empirical null).
 * REs that carry something else than our PDSCH stand out as FAIL-over-REF excess of rho at exactly their (symbol, subcarrier, CRB parity) bins.
 *
 * nr_spr_estimate: per bin a one-sided Welch z (FAIL vs REF). Seeds: z above max(Bonferroni quantile over all bins and looks, largest |z| between the two REF halves)
 * and an excess in the upper class of Otsu's two-class split of all excesses; then hysteresis inside the seed symbols (Bonferroni over those bins only), i.e. the
 * subcarriers of a resource that a seed symbol shares. Symbols where nearly all bins are in excess are reported as sym_full (an end trim / tail loss, not a resource) and
 * kept out of the patterns. Per symbol, the significant sc sets per CRB mod 4 give: the sc mask of each parity (intersection of its two mod-4 classes), symbols with
 * the same (even, odd) masks form one pattern (every RB when both parities agree, else even / odd RBs); the CRB range is the hull of the significant 8-CRB buckets of the
 * pattern symbols, extended to the band edge over buckets no FAIL grant covered. Pure functions, no state, no locking. */
#ifndef NR_SLOTPHASE_RESID_H
#define NR_SLOTPHASE_RESID_H
#include <stdbool.h>
#include <stdint.h>
#include "nfapi/open-nFAPI/nfapi/public_inc/fapi_nr_ue_interface.h"
#ifdef __cplusplus
extern "C" {
#endif

#define NR_SPR_NSYM 14
#define NR_SPR_KEYS 48
#define NR_SPR_BKT 36
#define NR_SPR_N (NR_SPR_KEYS + NR_SPR_BKT)
#define NR_SPR_MAXPAT 6
#define NR_SPR_MIN_RE 4        /* REs a key bin needs in one grant to count (a bucket needs NR_SPR_MIN_RE_BKT) */
#define NR_SPR_MIN_RE_BKT 16
#define NR_SPR_MIN_BINS 12     /* valid key bins a grant needs for its median */
#define NR_SPR_MIN_G 8         /* grants a bin needs in a table to be tested */

typedef struct { double n, s, q; } nr_spr_acc_t; /* grants, sum rho, sum rho^2 of one bin */
typedef struct {
  nr_spr_acc_t a[NR_SPR_NSYM][NR_SPR_N];
  uint32_t grants;
} nr_spr_tab_t;

typedef struct {
  int nsig;            /* significant key bins (seeds + hysteresis) */
  int nseed;
  double zthr, zthr2;  /* seed and hysteresis z cuts actually used */
  double znull;        /* largest |z| between the two reference halves (0 when they were too small) */
  double otsu;         /* excess cut (Otsu), 0 when fewer than 4 bins were testable */
  uint16_t undersampled; /* symbols covered by fewer than 2 * NR_SPR_MIN_G FAIL grants (but by some): too little power to say they carry nothing */
  uint16_t sym_full;   /* symbols with nearly all testable bins in excess (tail loss), not turned into patterns */
  int mod4_left;       /* significant (sym, sc) cells that exist for one CRB mod-4 class of a parity only and are therefore not in a pattern */
  int npat;
  fapi_nr_dl_config_csirs_pdu_rel15_t pat[NR_SPR_MAXPAT]; /* RE-pattern PDUs (nr_re_pattern.h) */
  uint16_t sig_sc[NR_SPR_NSYM][4]; /* significant sc bits per symbol and CRB mod-4 class */
  int rb_lo, rb_hi;    /* CRB range of the patterns [rb_lo, rb_hi) */
} nr_spr_result_t;

/** Adds one grant (e / n: [14][NR_SPR_N]) to @p t. false = unusable (too few valid bins / zero baseline); the table is untouched then. */
bool nr_spr_add(nr_spr_tab_t *t, const float e[][NR_SPR_N], const uint32_t n[][NR_SPR_N]);
/** FAIL vs REF (halves @p ra, @p rb). @p alpha = family-wise level of this look. Returns the number of patterns (also out->npat); 0 = nothing foreign found. */
int nr_spr_estimate(const nr_spr_tab_t *fail, const nr_spr_tab_t *ra, const nr_spr_tab_t *rb, double alpha, nr_spr_result_t *out);
/** Standard normal quantile (Acklam), p in (0,1). */
double nr_spr_norm_quantile(double p);

#ifdef __cplusplus
}
#endif
#endif
