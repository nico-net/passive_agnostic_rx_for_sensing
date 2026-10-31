/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_PUSCH_UL_DISC_H
#define NR_PUSCH_UL_DISC_H
#include <stdbool.h>
#include <stdint.h>
#include "PHY/TOOLS/tools_defs.h"

/* CPU reference contract: one grant, contiguous PRBs, up to four RX. The grids are
 * [antenna][symbol][FFT bin]. A GPU port can replace the batched scorer without
 * changing the hypothesis/state interface. All searches run on the UL consumer. */
typedef struct {
  int fft, n_rb_carrier, first_carrier, rb_start, rb_count;
  int symbols_per_slot, slot, start_symbol, num_symbols, normal_cp;
  const c16_t *rx[4];
  int n_rx;
} nr_pusch_ul_disc_grid_t;

typedef struct {
  int id, nscid, dmrs_type, mapping_type, add_pos, front_load;
  int max_length; /* 2 when two-symbol front load proves it; 0 means 1 or 2 */
  uint16_t mask;
  double coherence, false_alarm_bound;
  bool accepted, fresh;
} nr_pusch_ul_disc_result_t;

typedef struct {
  uint16_t mask, id;
  uint8_t dmrs_type, nscid;
} nr_pusch_ul_disc_hyp_t;
typedef struct { double score; int pairs; } nr_pusch_ul_disc_metric_t;

void nr_pusch_ul_disc_score_batch(const nr_pusch_ul_disc_grid_t *grid,
                                  const nr_pusch_ul_disc_hyp_t *hypotheses, int count,
                                  nr_pusch_ul_disc_metric_t *metrics);

/* Coherent adjacent-pilot correlation across all RX, on the exact granted REs.
 * Returns 0 for invalid geometry. The null tail uses a conservative union bound
 * over 2^16 IDs, two nSCIDs, two types, and 32 symbol layouts. */
double nr_pusch_ul_disc_score(const nr_pusch_ul_disc_grid_t *grid, uint16_t mask,
                              int dmrs_type, int nscid, int id, int *pairs);
double nr_pusch_ul_disc_delay_score(const nr_pusch_ul_disc_grid_t *grid, uint16_t mask,
                                    int dmrs_type, int nscid, int id, int *pairs);
bool nr_pusch_ul_disc_accept(double score, int pairs, double *false_alarm_bound);

/* Priors are evaluated first. Background ID sweep tests 64 consecutive IDs per
 * eligible grant, covering the whole 16-bit range after at most 1024 grants.
 * A result is published only when the familywise noise false-alarm bound <=1e-6
 * and at least 128 adjacent pilot pairs were observed. */
nr_pusch_ul_disc_result_t nr_pusch_ul_disc_search(const nr_pusch_ul_disc_grid_t *grid,
                                                  uint16_t rnti, uint8_t type_a_pos,
                                                  const int *prior_ids, int npriors,
                                                  int preferred_type, int preferred_nscid,
                                                  uint8_t antenna_ports_field);
nr_pusch_ul_disc_result_t nr_pusch_ul_disc_cached(uint16_t rnti);
/* the same search, but the blind scan covers ids [id_lo, id_lo + id_n) (ISAC_UL_BGDISC worker); stops at the first accepted id */
nr_pusch_ul_disc_result_t nr_pusch_ul_disc_search_sweep(const nr_pusch_ul_disc_grid_t *grid, uint16_t rnti, uint8_t type_a_pos, const int *prior_ids,
                                                        int npriors, int preferred_type, int preferred_nscid, uint8_t antenna_ports_field,
                                                        int id_lo, int id_n);
/* ids already latched for any RNTI (priors for the others); returns the count */
int nr_pusch_ul_disc_known_ids(int *ids, int max);

/* Applied advance = N_TA_offset + a signed arrival correction. Tracker stores
 * that signed correction, never calls it a decoded MAC timing-advance command. */
int nr_pusch_ul_disc_timing_seed(uint16_t rnti, int nominal_advance, long abs_slot, bool *tracked);
void nr_pusch_ul_disc_timing_update(uint16_t rnti, int nominal_advance,
                                    int measured_advance, double score, long abs_slot);

/* ---- Closed-form UL timing (G1) --------------------------------------------------------------------------------------------------------
 * The grid is the FFT of ONE window position (one FEP). For a hypothesis (id, nscid, type) the descrambled pilots h_m = conj(p_m) y_m of one DM-RS symbol carry
 * the common arrival delay as a phase slope exp(-j 2 pi k_m d / fft) (k = signed bin; the per-antenna gain and the per-symbol phase cancel in pair products
 * h_{m'} conj(h_m)). The estimate is a ladder of pair statistics with doubling baselines (comb 2: 2, 4, 8 .. bins): the shortest baseline fixes d within
 * +-fft/(2 baseline), each longer one refines it, the alias ambiguity of a stage is resolved by the delay-domain power sum_a |sum_m h_m exp(+j 2 pi d k_m / fft)|^2
 * among the +-2 alias candidates. No further FEP is needed; the caller applies it as  advance -= delay  (one corrective FEP) and may verify there.
 * `ratio` is that power over its noise-only expectation, summed over RX and DM-RS symbols (mean 1 for noise or a wrong id), `dof` the degrees of freedom
 * of the wrong-hypothesis null (one per DM-RS symbol: with a signal present all RX carry the same wrong sequence and do not average; noise-only input has more and a lighter
 * tail): ratio*dof is bounded by Gamma(dof) for a wrong hypothesis, which gives a false-alarm bound without any CRC. Valid for |d| < fft/(2 min baseline). */
typedef struct {
  double delay;   /* apparent delay of the signal in the window, samples (+ = late) */
  double ratio;   /* delay-domain power at `delay` / noise-only expectation */
  double trials;  /* independent delay positions the ladder could have picked (union bound) */
  int dof, pilots, stages;
  bool valid;
  bool used_old;     /* the receive chain's own per-grant estimate verified better than the ladder (comb 2 only) */
  int old_delay;     /* that estimate (0 = no peak) and its ratio, valid when trials were doubled */
  double old_ratio;
} nr_pusch_ul_disc_delay_t;
bool nr_pusch_ul_disc_delay_closed(const nr_pusch_ul_disc_grid_t *grid, uint16_t mask, int dmrs_type, int nscid, int id,
                                   nr_pusch_ul_disc_delay_t *out);
/* Chernoff bound on P(noise reaches this evidence): sum_i x_i over `grants` independent observations (x_i = dof_i * ratio_i), union over `trials` delay positions
 * per grant and `family` hypotheses. Returns ln(bound) <= 0. */
double nr_pusch_ul_disc_evidence_ln_bound(double sum_x, double sum_dof, double ln_trials_total, double family);
/* single observation: the bound <= 1e-6 */
bool nr_pusch_ul_disc_delay_accept(const nr_pusch_ul_disc_delay_t *d, double family, double *false_alarm_bound);
/* DM-RS identity evidence without a CRC: accumulates the observation of (rnti, id, nscid, type) (a second look at the same absolute slot is ignored); true when the
 * summed evidence of the hypothesis reaches the 1e-6 bound over `family` hypotheses. The state is bounded (4 candidates per RNTI) and is dropped with the track. */
bool nr_pusch_ul_disc_id_evidence(uint16_t rnti, int id, int nscid, int dmrs_type, const nr_pusch_ul_disc_delay_t *d, int64_t abs_slot, double family,
                                  int *grants, double *false_alarm_bound);
/* a timing measurement already accepted by an evidence bound trains the trackers (no score threshold) */
void nr_pusch_ul_disc_timing_update_ev(uint16_t rnti, int nominal_advance, int measured_advance, long abs_slot);

/* ---- G1 timing resolver (the worker branch of ul_bg_run, with the FEP injected so that a unit test can drive it with synthetic time-domain bursts) ----
 * `fep(ctx, adv)` fills the caller's grid (grid->rx) with the FFT of the first DM-RS symbol at advance `adv` and returns false when the window is outside the
 * snapshot. The resolver reads the closed-form delay of the best DM-RS id of `prior`, accepts it by its false-alarm bound, verifies it at the corrected window and
 * resolves the comb-2 alias (delay known only modulo fft/2) by comparing the alias windows with the same delay-domain ratio. */
typedef struct {
  const nr_pusch_ul_disc_grid_t *grid;
  uint16_t mask;
  int type, nscid;
  const int *prior;
  int nprior;
  int cached_id;   /* -1: none; the id the hypothesis cache already holds (family 1 for it) */
  int seed, ta_offset, fft;
  bool tracked;
  bool (*fep)(void *ctx, int adv);
  /* cyclic-prefix statistic of the grant's symbols for the window at advance `adv`: m = Re sum x[n] conj(x[n+N]) over the CP samples of every grant symbol and RX,
   * e2 = sum |x[n]|^2 |x[n+N]|^2 (null: m / sqrt(e2/2) ~ N(0,1)). May be NULL or return false (no snapshot): then no alias is ever preferred over the default. */
  bool (*cp_stat)(void *ctx, int adv, double *m, double *e2);
  void *ctx;
} nr_pusch_ul_disc_timing_in_t;
/* index (0 = the default, candidate 0) of the alias candidate to use. Evidence per candidate: zcp[] = CP z-scores (NAN = not available; may be NULL) and/or dd[] = the
 * closed-form result read at each candidate window (valid=false or NULL: not available); n_rx = receive antennas of the grid. A candidate replaces the default only when
 * its combined normal score beats the alpha bound over the n-1 alternatives. */
int nr_pusch_ul_disc_alias_choose(const double *zcp, const nr_pusch_ul_disc_delay_t *dd, int n_rx, int n);
typedef struct {
  bool locked;      /* ta / idx / v are an accepted measurement */
  int ta;           /* advance to decode with */
  int idx;          /* index in prior[] of the id that carried it */
  nr_pusch_ul_disc_delay_t v;
  bool alias_switched;
  bool undec;       /* first window: best id not decisive alone (evidence for the id may still accumulate) */
  int undec_idx;
  nr_pusch_ul_disc_delay_t undec_d;
  int feps, vfail;
} nr_pusch_ul_disc_timing_out_t;
void nr_pusch_ul_disc_timing_resolve(const nr_pusch_ul_disc_timing_in_t *in, nr_pusch_ul_disc_timing_out_t *out);
void nr_pusch_ul_disc_reset(void);
bool nr_pusch_ul_disc_ta_track_on(void); /* ISAC_UL_TA_TRACK */
void nr_pusch_ul_disc_epoch(uint32_t dedicated_epoch);
#endif
