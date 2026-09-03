/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this
 * file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.openairinterface.org/?page_id=698
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

#include "isac_sync.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdlib>

extern "C" {
#include "common/utils/LOG/log.h"
}

namespace nr_isac {

namespace {

constexpr double SPEED_OF_LIGHT = 299792458.0;

// LAST-RESORT fallback bin for the LOS search, nothing more. The walking tracker seeds row 0 from
// the earliest arrival it can actually find, and this value is used only when no window anywhere
// clears the fade gate (see the comment at the row-0 seed below). It is NOT a calibration and NOT a
// measurement of where the direct path is: on the current rig the gNB sits 4.13 m away and the LOS
// locks near bin 2. Deployments that want a better fallback set [sensing] nominal_los_range_m.
//
// Do not read a physical meaning into 98.0. It is twice a 49.0 that dated from a 20 MHz OTA cell,
// doubled in 2026-07-23 only so nominal_los_bin() kept landing on the same integer after
// range_res_m dropped an erroneous monostatic factor of 2.
constexpr double NOMINAL_LOS_RANGE_M = 98.0;

// Half-width (in bins) of the window around the nominal bin searched for the CPI's FIRST locked row
// only (before the walking tracker has anything to walk from) -- see cpi_sto_tracker's class comment
// in isac_sync.h for why this phase now walks rather than using this fixed window for every row.
constexpr uint32_t SEARCH_HALFWIN_BINS = 4;

// Half-width (in bins) of the window around the PREVIOUS locked row's own peak searched for every
// subsequent row (SYNC_NOISE_HANDOVER.md's root-cause fix). Deliberately much narrower than Phase
// 3's SFO_TRACK_HALFWIN_BINS below: this phase only needs to absorb the PER-STEP drift between
// consecutive rows (typically well under 1 bin at real TCXO-scale SFO), not survive SFO's much
// larger multi-hundred-bin excursion over a whole CPI, which remains Phase 3's job.
constexpr uint32_t WALK_HALFWIN_BINS = 2;

// Fade gate for the walking tracker: a row's in-window local maximum is trusted only if its power
// exceeds this many linear multiples of the average power OUTSIDE the search window (the same
// out-of-window-energy statistic Phase 3's ISI gating already uses, see track_row() below). Below
// this, the "maximum" found is more likely thermal noise/sidelobe than a real signal, and the
// flywheel projects forward instead of trusting it (see cpi_sto_tracker's class comment).
//
// VALUE EMPIRICALLY JUSTIFIED (2026-07-23) after two competing hypotheses about the flywheel's high
// engagement rate were BOTH measured and DISPROVED -- record them so neither is retried blindly:
//   1. "WALK_HALFWIN_BINS=+-2 is too narrow for real drift" (refactorDSP.md Task 1's premise, and
//      the fix proposed in SYNC_NOISE_HANDOVER.md). Instrumenting the walk on live data with a real
//      target: ~99% of bin steps between consecutive locked rows are 0 or 1 bin, and only 0-4 rows
//      per 128-row CPI ever reach the +-2 edge. The window is NOT the binding constraint; widening
//      it (statically or dynamically) would change nothing.
//   2. "This gate is too strict -- most failures are within 3 dB of passing, so they're real taps
//      being discarded." Measured the null distribution instead of assuming: pushing pure white
//      noise through this exact Hann-window + IFFT + (+-2-bin local max)/(out-of-window mean)
//      pipeline gives p50=1.78, p75=2.62, p90=3.69, p95=4.47, p99=6.23 (4000 trials). So rows in
//      [2,4) -- the "marginal" ones -- sit exactly where PURE NOISE lands most often; they are
//      genuinely ambiguous, not obviously-real. False-pass rate vs gate: 2.0 -> 42%, 3.0 -> 18%,
//      4.0 -> 7.3%. Lowering the gate trades a modest gain in locked rows for a large rise in
//      false locks, and a false lock is worse than a flywheel step because it drags the walk's
//      position onto noise. tests/isac_sync_test.cc's fade case independently rejects 2.0.
// 4.0 is therefore kept. A ~20%/CPI flywheel rate on this harness is the gate working as intended,
// not a defect -- look elsewhere (Phase 2/3's use of the locked rows) for sync-ON's cost.
constexpr double FADE_MIN_SNR_LINEAR = 4.0; // ~6 dB; 7.3% false-pass on pure noise (measured above)

// Minimum out-of-window sample count before the fade-gate ratio is trusted; below this (a very short
// compact CIR) the noise-floor estimate itself is too noisy to gate on, so the row is accepted
// unconditionally (matches the pre-walking design's behaviour for short CIRs).
constexpr uint32_t FADE_MIN_FLOOR_SAMPLES = 4;

// Shortest compacted CIR (in samples) trusted for a windowed peak search + parabolic fit. Below this
// a row is skipped rather than risking a peak search with no real margin around the nominal bin.
constexpr uint32_t MIN_CIR_LEN = 8;

// Minimum valid rows required to attempt a CPI-wide line fit / STO-vs-SFO classification.
constexpr uint32_t MIN_VALID_ROWS = 8;

// Total drift over the CPI's time span, in range bins, below which the per-row sub-bin delay is
// classified "constant" (fine STO -- safe to null). At/above this a real SFO is leaking through and
// must NOT be corrected here (Phase 3's job) -- ties to the acceptance criterion's "fraction of one
// range bin" residual tolerance.
constexpr double DRIFT_BIN_THRESHOLD = 0.25;

// Phase 2 (CFO): minimum valid LOS-tap rows to attempt the unwrap + line fit. Lower than Phase 1's
// MIN_VALID_ROWS since a phase-vs-time line has only 2 unknowns (slope, intercept) and each row
// contributes one scalar (vs. Phase 1 needing search-window margin per row).
constexpr uint32_t CFO_MIN_VALID_ROWS = 4;

// Alpha-beta tracker gains for the slow cross-CPI CFO estimate (diagnostic; not fed into this
// phase's own correction). dt is 1 CPI tick, not elapsed seconds -- see cpi_cfo_tracker::process().
// Starting defaults; tune in Phase 6a's sweep against a known injected CFO/drift rate.
constexpr double CFO_ALPHA = 0.3;
constexpr double CFO_BETA  = 0.05;

// Cross-CPI EMA gain for Phase 3's sfo_ppm smoothing (see cpi_sfo_tracker's "CROSS-CPI SMOOTHING"
// class comment in isac_sync.h for why, unlike CFO, this directly affects correction quality, not
// just diagnostics). Single-pole (no rate term, unlike CFO_ALPHA/CFO_BETA): SFO's physical origin is
// a near-constant clock-rate error, without CFO's comparatively fast/legitimate short-term wander,
// so a rate term wasn't judged necessary. Same order of magnitude as CFO_ALPHA (a starting default,
// not stress-tuned against a multi-CPI synthetic sweep -- flagged as follow-up if live data shows it
// needs retuning).
constexpr double SFO_EMA_ALPHA = 0.3;

// Phase 3 (SFO): half-width (in bins) of the per-row tracking window, re-centered on the PREVIOUS
// accepted row's own peak rather than a fixed nominal bin (see cpi_sfo_tracker's class comment).
// Wider than Phase 1's SEARCH_HALFWIN_BINS since it must additionally tolerate row-to-row peak
// wobble/noise on top of genuine per-step drift, with no "known stable location" prior beyond the
// very first row.
constexpr uint32_t SFO_TRACK_HALFWIN_BINS = 10;

// Minimum {time, delay} pairs (post-ISI-exclusion) to attempt the SFO line fit.
constexpr uint32_t SFO_MIN_VALID_ROWS = 8;

// Minimum R^2 of the delay-vs-time line fit before its slope is TRUSTED ENOUGH TO APPLY. A real
// sample-clock error makes delay advance linearly with elapsed time, so a genuine SFO fits its line
// almost perfectly (R^2 ~ 0.99+); a fit driven by per-row peak-search noise does not.
//
// ADDED 2026-07-23, and it is the fix for a measured, significant regression: a per-phase ablation
// (12 live runs, sync_sto/cfo/sfo/los gated independently) found Phase 3 ALONE dropped target
// detection to 49.9% vs 100.0% with all sync off -- worse than every phase enabled together (77.8%).
// Phases 1 and 2 were harmless (100.0% each). Instrumenting this fit then showed why: on this
// harness R^2 runs 0.04-0.65 (mean ~0.28) with RMS residuals of 1.2-3.6 range bins, i.e. there is no
// real linear clock drift here at all -- yet the correction was applied unconditionally whenever
// n_fit >= SFO_MIN_VALID_ROWS, multiplying a noise-derived slope by each row's elapsed time and
// injecting a time-proportional delay error into every row. That smears the range axis and destroys
// the moving target's coherent gain, which is exactly the symptom SYNC_NOISE_HANDOVER.md recorded as
// "sfo_ppm swinging wildly and flipping sign CPI to CPI".
//
// 0.90 sits well clear of both regimes: above every noise fit observed (max 0.65) and far below a
// genuine SFO's ~0.99 (tests/isac_sync_test.cc's injected-SFO sweep still passes, confirming a real
// clock error is not gated out). The estimate is still COMPUTED and REPORTED when the gate rejects
// it -- only the correction is withheld -- so the diagnostics/JSON keep working.
// LOS seed selection (see the MEASURED LOS SEED block). A qualifying window is accepted as the
// direct path if its peak is within this LINEAR power ratio of the strongest window found; among
// those, the EARLIEST wins. 0.25 = -6 dB.
//
// Tightening this to -3 dB was TRIED and REVERTED 2026-09-01. The motivation was sound (39 % of
// per-CPI seeds fall below the 3-4 m ground truth, i.e. weak early peaks are being accepted), but
// it was changed in the same build as LOS_LOCK_QUANTILE, so neither could be attributed, and the
// only capture that survived long enough to sample it gave n=8. At -6 dB the per-CPI median is
// 1.248 bins = 3.81 m, inside ground truth, which is what the converged estimate consumes. Re-try
// the tightening on its own, against >=200 seeds, before believing it either way.
//
// Why earliest-not-strongest, measured 2026-09-01: the sweep used to keep the strongest peak
// anywhere in the CIR. With only comb-1 sources enabled that is the direct path and it seeded a
// steady bin 1-4 (3-12 m). Enabling the combed sources (pdsch_dmrs_blind is comb-2, csi_rs
// comb-12) injects range-domain replicas -- a comb-c row repeats every nof_range/c bins -- and the
// replica is an exact copy, so it ties or beats the original. The seed then locked onto
// bin 1636-1641 against nof_range/2 = 1638, i.e. ~5 km, on 84 of 419 CPIs. range_doppler::process
// already de-aliases per row, but this sweep runs on the RAW CIR and does not.
//
// Strength cannot separate a replica from its original, because they have the same power. Arrival
// order can, and does so without depending on the comb layout: the direct path is the SHORTEST
// path, so nothing can arrive before it and every replica arrives after. The margin exists only so
// ordinary noise on a marginally-stronger later window cannot outrank a clean early one; the gate
// inside estimate_row() (FADE_MIN_SNR_LINEAR) still decides what qualifies at all.
constexpr double LOS_SEED_EARLIEST_FRAC = 0.25;

// How many rows the LOS seed sweep polls before deciding. Each contributes ONE candidate (its own
// earliest qualifying arrival) and the seed is their MEDIAN, so a single bad row cannot set the
// seed for the whole CPI.
//
// Why a median and not the first row that locks, measured 2026-09-01: taking the first locking row
// left 2 seeds of 109 at bin 197 and 270 against a population otherwise entirely in bins 0-3. One
// row landing on a fade, a collision or a strong second path is enough, and there was no second
// opinion to overrule it. The median needs more than half the polled rows to agree before it moves,
// which is exactly the robustness the single-row version lacked -- and it costs one extra sweep
// over rows that were already being read.
constexpr uint32_t LOS_SEED_POLL_ROWS = 8;

// Accepted per-CPI seeds kept for the static-LOS estimate, and how many are needed before it is
// trusted enough to reject a disagreeing CPI and be reported as CONVERGED.
//
// 32, raised from 8 on 2026-09-01. Four runs of the SAME binary, differing only in how long they
// survived before an RFSTALL ended them:
//     n=308 CPIs -> 5.91 m      n=40 -> 5.05 m      n=15 -> 1.53 m      n=14 -> 38.47 m
// The two long runs agree to within a bin; the two short ones are noise, and at 8 CPIs both were
// still labelled CONVERGED. A premature lock is worse than no lock, because the tolerance gate then
// starts REJECTING the good seeds that disagree with the bad value it settled on.
//
// The per-CPI seed distribution is what sets this: only ~16 % of individual seeds land inside the
// 3-4 m ground-truth window, 39 % fall below and 45 % above, so a median needs a few tens of
// samples before it is stable. Do not lower it to make a short run report a number sooner -- a run
// that has not converged should say so.
constexpr size_t   LOS_HIST_MAX       = 64;
constexpr uint32_t LOS_LOCK_MIN_CPIS  = 32;
// The converged LOS is the MODE of the accepted per-CPI seeds, not a quantile of them.
//
// Measured against a known 3-4 m ground truth (bins 0.98-1.31), 230 per-CPI seeds distribute:
//     min 0.000  p10 0.500  p20 0.561  p25 0.608  p50 1.248  p75 1.911  p90 22.9  max 1636.6
//     39 % below the true value, 16 % inside it, 45 % above
// so the noise is TWO-SIDED and heavy-tailed on both ends. The low mass (bins 0.0-0.6) is the
// earliest-arrival rule admitting noise peaks IN FRONT of the direct path; the high tail is comb
// replicas at nof_range/2 and beyond.
//
// Every order statistic fails on that shape, and both were tried on live data: the 20th percentile
// converged to 1.71 m (dragged by the low tail) and the median wandered between 1.5 m and 5.9 m
// across runs depending on which side happened to dominate its window. Neither is a defect of the
// estimator so much as of the question -- a quantile asks "what value splits this sample", when the
// physical quantity is ONE CONSTANT that every CPI is trying to measure.
//
// The mode asks the right question. A static LOS reproduces the SAME sub-bin position every CPI, so
// it accumulates; noise peaks land wherever they land and do not. A 1636-bin replica occupies its
// own histogram cell with a count of one and can never win, no matter how many of them there are,
// which is the property neither a mean nor a quantile has.
//
// Cell width is a compromise: narrow enough to resolve the sub-bin position that is the whole point
// of carrying frac_bin, wide enough that honest jitter on a real path lands in ONE cell rather than
// splitting across two and losing to a coincidence. A quarter bin is 0.76 m at 273 PRB / 30 kHz,
// comfortably finer than the 3-4 m the answer needs to distinguish.
constexpr double LOS_MODE_CELL_BINS = 0.25;
/// Support window for the cross-CPI LOS cluster search: the range mainlobe half-width, so the
/// selection cannot hinge on a histogram cell boundary.
constexpr double LOS_CLUSTER_HALFWIN_BINS = 0.5;
/// A cluster counts as real at this fraction of the modal support. See the derivation at the
/// call site -- measured against surveyed geometry, not tuned for a target number.
constexpr double LOS_CLUSTER_MIN_FRAC = 0.5;
// A CPI seed further than this from the converged value is rejected as a replica or a fade rather
// than believed. In BINS, and generous on purpose: it only has to exclude the failure actually
// observed (nof_range/2, i.e. 1638 bins away) without fighting honest sub-bin noise.
constexpr double   LOS_LOCK_TOL_BINS  = 8.0;

constexpr double SFO_MIN_R_SQUARED = 0.90;

// SECOND acceptance path for the fit above, added 2026-09-01 after the r2 gate was measured
// rejecting a 21-sigma slope on real OTA data. R^2 is fraction-of-variance-explained, NOT a
// significance test: it collapses when the PER-ROW delay noise is large even though the slope
// itself is pinned down by sheer row count. Measured on this X410/gNB pair, same run, same slope:
//   128-slot CPI:   n=88   r2=0.013  ->  t= 1.1   (genuinely unresolvable, reject)
//   1024-slot CPI:  n=726  r2=0.379  ->  t=21.0   (slope known to 5%, r2 rejected it anyway)
// Both fits agree on the slope (-0.194 / -0.158 ppm), which is an ordinary free-running OCXO
// offset. The drift signal grows with the CPI's time span while per-row scatter stays flat
// (resid 4.22 -> 3.64 bins), so r2 only clears 0.90 at a CPI ~5x longer again -- impractical.
// t = sqrt(r2*(n-2)/(1-r2)) is the standard slope t-statistic. At t=10 the slope is determined to
// ~10%, so applying it removes ~90% of the real ramp rather than injecting noise.
// This is an OR, not a replacement: everything that passed the r2 gate still passes, so no
// previously-accepted fit changes behaviour and the injected-SFO tests are unaffected.
constexpr double SFO_MIN_SLOPE_T = 10.0;

// Minimum same-comb rows observed before the ISI baseline is trusted enough to flag anomalies;
// below this every row in that comb group is accepted (building up the baseline).
constexpr uint32_t SFO_ISI_MIN_GROUP_ROWS = 3;

// Size of the sliding window of recent accepted same-comb rows the ISI baseline is computed over
// (see comb_stats_t's comment for why this replaced a whole-CPI cumulative mean). Large enough to
// average out row-to-row noise, small enough to track legitimate slow drift in the metric as the
// tracking window walks across the compact CIR over a long CPI.
constexpr uint32_t SFO_ISI_LOCAL_WINDOW = 20;

// A row is excluded as ISI-contaminated if its leakage_anomaly (see leakage_model in isac_sync.h --
// out-of-window CIR energy normalized by peak power AND by this exact (M, frac)'s modelled clean-
// signal expectation) exceeds its comb group's local mean by more than this many standard
// deviations. RESOLVED (2026-07-22, gap-analysis doc sec. 12.3 follow-up): SFO_ISI_SIGMA was
// previously 30.0 to blindly tolerate raw out-of-window energy's 3-4-order-of-magnitude swing, which
// a per-(M,frac) calibration sweep found was NOT just a function of frac as first framed (M matters
// at least as much -- see leakage_model's comment). Now that track_row() reports a normalized
// anomaly that should sit near 1.0 for a clean row regardless of M or frac, this threshold is
// tightened back towards the task's original intent. Re-measured empirically (this module's own
// established practice, not guessed) by bisecting against tests/isac_sync_test.cc's clean-signal SFO
// sweep: sigma=10.0 is the lowest value with zero false exclusions on that sweep; sigma<=8.0
// produces false exclusions at higher injected SFO (0.5-1 ppm), where the anchor-tracking walk's own
// step-to-step search noise legitimately grows. 12.0 is used for a small margin above that measured
// floor -- still ~2.5x more sensitive to genuine outliers than the pre-fix value of 30.0.
constexpr double SFO_ISI_SIGMA = 12.0;

// This build's actual RF sample rate (USRP B210, per CLAUDE.md's live-testbed config) -- used only
// to additionally express the fitted (dimensionless) ppm figure as an absolute Hz clock error for
// human-readable logging; the ppm value itself does not depend on this constant.
constexpr double SAMPLE_RATE_HZ = 23.04e6;

// Nominal LOS bin, shared between Phase 1 (fixed-window search) and Phase 3 (seeds its first row's
// tracking window from the same location). See NOMINAL_LOS_RANGE_M above for why this is the same
// integer regardless of a row's native comb.
uint32_t nominal_los_bin(uint32_t nof_subc, double scs_hz, double nominal_range_m)
{
  if (nof_subc == 0 || scs_hz <= 0.0) {
    return 0;
  }
  const double bin_to_delay_s = 1.0 / ((double)nof_subc * scs_hz);
  // Differential bistatic range per bin (no monostatic /2) — matches range_doppler.cc's
  // rvm.range_res_m; see the 2026-07-23 calibration-fix comment there.
  const double range_res_m    = SPEED_OF_LIGHT * bin_to_delay_s;
  return (uint32_t)std::lround(nominal_range_m / range_res_m);
}

// Phase 4: number of successful (detection-found) CPIs averaged to establish the LOS baseline.
// "First several CPIs" per the task text; not tied to any particular CPI duration.
constexpr uint32_t LOS_BASELINE_INIT_CPIS = 5;

// Phase 4: a detection must be within this many bins of the current (candidate) baseline on BOTH
// axes to be considered "the" LOS detection this CPI. Wide enough for residual jitter/sub-bin
// quantization, narrow enough not to grab an unrelated target sharing the LOS's general vicinity.
constexpr uint32_t LOS_MATCH_MAX_RANGE_BINS   = 5;
constexpr uint32_t LOS_MATCH_MAX_DOPPLER_BINS = 5;

// Phase 4: leaky-integrator gains for the closed-loop bias. A PURE integrator risks unbounded
// windup if the baseline match degrades for several consecutive CPIs (e.g. a real target
// transiting the LOS's own range/Doppler cell); the leak term keeps the bias state bounded while
// still accumulating a persistent correction over many CPIs. Starting defaults; tune in Phase 6a
// against the self-test harness's known-injected residual (this is the PLL/integrator bandwidth
// choice the Constraints section asks to be stated explicitly, not picked silently).
constexpr double LOS_BIAS_KI   = 0.3;
constexpr double LOS_BIAS_LEAK = 0.02;

// RESOLVED (2026-07-22, follow-up to gap-analysis doc sec. 12.2): the power-based 3-point parabolic
// estimator formerly used here had ~0.09-0.11 bin residual bias even after Hann-windowing (measured
// at the doc's own {0.092, 0.275, 0.459}-bin test offsets: {0.049, 0.171, 0.406}, i.e. errors up to
// ~0.11 bin). Replaced with a complex-domain (Jacobsen/Candan-form) ratio estimator, which uses the
// raw complex CIR samples straddling the peak instead of their squared magnitude -- the squaring is
// what distorts the power-based fit's symmetry assumption under a non-rectangular (here, Hann)
// mainlobe. HANN_ESTIMATOR_SCALE below is an empirically measured window-correction constant (this
// module's own established practice per sec. 12.2/12.3 -- measure, don't guess a literature figure):
// a standalone calibration harness reused isac_fft.cc UNMODIFIED to build the exact M-point
// Hann-windowed compact CIR row_cir_builder::build() produces, for a pure tone swept across true
// sub-bin offsets in [-0.5, 0.5), and least-squares-fit the scale mapping the raw complex-ratio
// estimate to the true offset. Result (M = compact CIR length, i.e. row_cir_builder's occupied-
// subcarrier count): the fitted scale is mildly M-dependent (0.416 at M=8 rising to 0.495 at M=128,
// asymptoting toward 0.5) but a SINGLE scale fitted at a representative mid-size M=32 generalises
// well: post-correction max bias 0.0001 (M=32) to 0.065 (M=8) to 0.016 (M=128) bins, all well below
// the previous estimator's 0.09-0.11 bin bias at every tested M -- so one global constant is used
// rather than a per-M table (this module doesn't need per-M precision beyond that). At the doc's own
// three test offsets the corrected estimator's error is <0.0002 bin (was up to 0.11). Shared by both
// Phase 1 (STO, estimate_row) and Phase 3 (SFO, track_row) since both search a Hann-windowed compact
// CIR peak the same way -- see subbin_delta() below.
constexpr double HANN_ESTIMATOR_SCALE = 0.478762;

// M-DEPENDENT form of the scale above, 2026-09-01. The comment above records the fitted scale as
// mildly M-dependent -- 0.416 at M=8, 0.478762 at M=32, 0.495 at M=128, asymptoting to 0.5 -- and
// settled on the single M=32 value because one global constant was accurate enough when every row
// had a similar M.
//
// That premise no longer holds. With sources = csi_rs + pdsch_dmrs_blind + pdsch_data fused into
// one grid, M is the row's occupied-subcarrier count and now spans a comb-12 CSI-RS row to a comb-1
// data row in the SAME CPI, so rows sit at both ends of the M range the single scale was chosen to
// average over.
//
// The three measured anchors are fit to three decimals by 0.5 - 0.672/M:
//     M=8   -> 0.4160  (measured 0.416)
//     M=32  -> 0.4790  (measured 0.478762)
//     M=128 -> 0.4947  (measured 0.495)
// which is also the right SHAPE -- the correction is a finite-length window effect and must vanish
// as 1/M with the asymptote at 0.5. So this is the measured calibration expressed exactly, not a
// new guess: at M=32 it reproduces the old constant to 4 decimals, so any row near that M is
// unchanged.
constexpr double HANN_SCALE_ASYMPTOTE = 0.5;
constexpr double HANN_SCALE_COEFF     = 0.672;

inline double hann_estimator_scale(size_t m)
{
  if (m < 4) {
    return HANN_ESTIMATOR_SCALE; // below the calibrated range; keep the old behaviour
  }
  return HANN_SCALE_ASYMPTOTE - HANN_SCALE_COEFF / (double)m;
}

// Complex-domain (Jacobsen/Candan-form) sub-bin peak estimator: unlike the discarded power-based
// parabolic fit, this uses the raw complex CIR samples straddling the peak (not |.|^2), then divides
// by HANN_ESTIMATOR_SCALE to correct for this module's specific Hann-windowed compact-CIR pipeline
// (see that constant's comment for how it was measured). `cir` must have valid entries at
// peak-1/peak/peak+1 (both call sites already guarantee this via their window-search bounds).
double subbin_delta(const std::vector<icf_t>& cir, uint32_t peak)
{
  const std::complex<double> xm1(cir[peak - 1].real(), cir[peak - 1].imag());
  const std::complex<double> x0(cir[peak].real(), cir[peak].imag());
  const std::complex<double> xp1(cir[peak + 1].real(), cir[peak + 1].imag());
  const std::complex<double> den = 2.0 * x0 - xm1 - xp1;
  if (std::abs(den) == 0.0) {
    return 0.0;
  }
  const double raw   = std::real((xm1 - xp1) / den);
  const double delta = raw / hann_estimator_scale(cir.size()); // scale from THIS row's compact-CIR length

  /* BIN 0 IS NOT A PROPAGATION PATH. cir[0] is the DC term of Y/X -- the mean of the channel
   * estimate -- and it is large. For a peak at bin 1 it becomes the parabola's LEFT sample, which
   * drags the fit toward it until delta hits the -0.5 clamp, placing the path BEFORE the first
   * valid bin. That is not a physical answer: nothing arrives earlier than zero delay.
   *
   * Measured 2026-09-01 with the downlink LOS separated from the uplink one: 54 of 64 CPIs agreed
   * on bin 0.071 (0.22 m) against a 3-4 m ground truth -- a sharp, repeatable consensus on an
   * artifact, which is exactly what a systematic pull looks like as opposed to noise. The mode
   * estimator above cannot help, because every CPI is making the same mistake.
   *
   * One-sided clamp at peak == 1: the right neighbour is a real sample so a LATER sub-bin estimate
   * is still trusted; only the DC-driven earlier half is refused. */
  const double lo = (peak <= 1) ? 0.0 : -0.5;
  return std::clamp(delta, lo, 0.5);
}

// Grid resolution for cpi_sfo_tracker::leakage_model's per-M calibration sweep (see that class's
// comment in isac_sync.h). |frac| in [0, 0.5]; the leakage shape is symmetric in frac's sign (a Hann
// window's sidelobe envelope depends only on distance from the nearest bin center).
constexpr int LEAKAGE_GRID_POINTS = 21;

// Builds one M's expected-leakage curve by literally running a synthetic single-tone through this
// pipeline's exact Hann-window + IFFT (own local fft_plan + Hann window, not row_cir_builder's
// private cache -- this only runs once per distinct M, amortized cost is irrelevant), at 21 |frac|
// points spanning [0, 0.5], measuring the SAME statistic track_row() does (mean power outside
// [peak-halfwin, peak+halfwin], divided by the peak bin's own power). `halfwin` must match
// SFO_TRACK_HALFWIN_BINS -- passed explicitly rather than hardcoded so this stays correct if that
// constant is ever retuned.
void build_leakage_curve(uint32_t M, uint32_t halfwin, std::vector<double>& frac_grid, std::vector<double>& ratio)
{
  frac_grid.assign(LEAKAGE_GRID_POINTS, 0.0);
  ratio.assign(LEAKAGE_GRID_POINTS, 0.0);
  if (M < 2 * halfwin + 3) {
    // Shouldn't happen: track_row already gates m >= 2*SFO_TRACK_HALFWIN_BINS+3 before ever calling
    // into the leakage model. Fall back to a flat curve (no normalization effect) defensively.
    std::fill(ratio.begin(), ratio.end(), 1.0);
    return;
  }

  std::vector<float> win(M);
  const uint32_t     denom = (M > 1) ? (M - 1) : 1;
  for (uint32_t i = 0; i < M; i++) {
    win[i] = 0.5f * (1.0f - std::cos(2.0f * (float)M_PI * (float)i / (float)denom));
  }

  fft_plan     plan(M, true /* inverse, matches row_cir_builder's IFFT */);
  const uint32_t peak_bin = M / 2;
  std::vector<icf_t> freq(M), cir(M);

  for (int g = 0; g < LEAKAGE_GRID_POINTS; g++) {
    const double frac = 0.5 * (double)g / (double)(LEAKAGE_GRID_POINTS - 1);
    frac_grid[g]       = frac;
    const double pos   = (double)peak_bin + frac;

    // Synthetic tone: in[n] = exp(-j*2*pi*n*pos/M), the CFR-domain model this pipeline uses
    // throughout (matches range_doppler.h's selftest_tone / the physical model doc comment) -- IFFTs
    // (via this plan's inverse=true convention, out[k]=sum_n in[n]*exp(+j2*pi*k*n/M)) to a peak at
    // delay-domain bin `pos`.
    for (uint32_t n = 0; n < M; n++) {
      const double ph = -2.0 * M_PI * (double)n * pos / (double)M;
      freq[n]          = icf_t((float)std::cos(ph), (float)std::sin(ph)) * win[n];
    }
    plan.run(freq.data(), cir.data());

    // True peak search near the expected location (matches track_row's local max search).
    uint32_t peak     = peak_bin;
    double   peak_pow = std::norm(cir[peak_bin]);
    for (uint32_t b = peak_bin - 1; b <= peak_bin + 1; b++) {
      const double p = std::norm(cir[b]);
      if (p > peak_pow) {
        peak_pow = p;
        peak     = b;
      }
    }

    const uint32_t lo = (peak > halfwin) ? peak - halfwin : 0;
    const uint32_t hi = std::min(peak + halfwin, M - 1);
    double         out_sum = 0.0;
    uint32_t       out_cnt = 0;
    for (uint32_t b = 0; b < M; b++) {
      if (b >= lo && b <= hi) {
        continue;
      }
      out_sum += std::norm(cir[b]);
      out_cnt++;
    }
    const double out_per_bin = (out_cnt > 0) ? out_sum / (double)out_cnt : 0.0;
    ratio[g]                 = (peak_pow > 0.0) ? out_per_bin / peak_pow : 0.0;
  }
}

} // namespace

fft_plan* row_cir_builder::plan_for(uint32_t m)
{
  for (auto& kv : plan_cache_) {
    if (kv.first == m) {
      return kv.second.get();
    }
  }
  plan_cache_.emplace_back(m, std::unique_ptr<fft_plan>(new fft_plan(m, true /* inverse, matches
                                                                        range_doppler's range IFFT */)));
  return plan_cache_.back().second.get();
}

const std::vector<float>& row_cir_builder::hann_for(uint32_t m)
{
  for (auto& kv : hann_cache_) {
    if (kv.first == m) {
      return kv.second;
    }
  }
  std::vector<float> w(m);
  const uint32_t     denom = (m > 1) ? (m - 1) : 1;
  for (uint32_t i = 0; i < m; i++) {
    w[i] = 0.5f * (1.0f - std::cos(2.0f * (float)M_PI * (float)i / (float)denom));
  }
  hann_cache_.emplace_back(m, std::move(w));
  return hann_cache_.back().second;
}

bool row_cir_builder::build(const icf_t* row, const uint8_t* mask, uint32_t nof_subc, uint32_t comb,
                            std::vector<icf_t>& cir)
{
  if (comb == 0) {
    return false;
  }

  int first = -1, last = -1;
  for (uint32_t c = 0; c < nof_subc; c++) {
    if (mask[c]) {
      if (first < 0) {
        first = (int)c;
      }
      last = (int)c;
    }
  }
  if (first < 0) {
    return false;
  }

  const uint32_t m = (uint32_t)((last - first) / (int)comb) + 1;
  if (m < MIN_CIR_LEN) {
    return false;
  }

  // Gather the row's real samples at their native comb stride. If a nominal position isn't actually
  // occupied (a rare same-slot multi-source merge where two combs overlap inconsistently), bail on
  // this row rather than IFFT a signal that isn't truly uniformly sampled.
  compact_.resize(m);
  for (uint32_t i = 0; i < m; i++) {
    const uint32_t c = (uint32_t)first + i * comb;
    if (c >= nof_subc || !mask[c]) {
      return false;
    }
    compact_[i] = row[c];
  }

  // Hann-window before the IFFT (same formula range_doppler.cc already uses for its own range/
  // Doppler axes). Discovered empirically in Phase 6a testing, not assumed: an un-windowed compact
  // CIR is a raw Dirichlet-kernel (rectangular-spectrum) mainlobe, and 3-point parabolic
  // interpolation of that shape has severe bias (measured up to ~0.2 bin error approaching a 0.5-bin
  // true offset) -- windowing shapes the mainlobe much closer to parabolic, which is exactly why
  // range_doppler's own main DSP path already windows both its axes. This does not shift the peak's
  // location (a window is a real, symmetric taper -- it broadens the mainlobe, it does not move it),
  // only the accuracy of sub-bin extraction from it. See docs/NR_UE_ISAC_sync_gap_analysis.md
  // section 12 for the measured before/after bias figures.
  const std::vector<float>& win = hann_for(m);
  for (uint32_t i = 0; i < m; i++) {
    compact_[i] *= win[i];
  }

  cir.resize(m);
  plan_for(m)->run(compact_.data(), cir.data());
  return true;
}

bool cpi_sto_tracker::estimate_row(const icf_t*         row,
                                   const uint8_t*       mask,
                                   uint32_t             nof_subc,
                                   uint32_t             comb,
                                   int                  center_bin,
                                   uint32_t             halfwin,
                                   los_row_estimate_t&  out,
                                   bool&                out_faded,
                                   double*              out_snr_lin)
{
  out_faded = false;
  if (out_snr_lin != nullptr) {
    *out_snr_lin = -1.0;
  }
  if (!cir_builder_.build(row, mask, nof_subc, comb, cir_)) {
    return false;
  }
  const uint32_t m = (uint32_t)cir_.size();

  if (m < 2 * halfwin + 3) {
    return false; // too short to window-search around the center bin with parabola margin
  }
  const int lo_i = std::max(1, center_bin - (int)halfwin);
  const int hi_i = std::min((int)m - 2, center_bin + (int)halfwin);
  if (hi_i <= lo_i) {
    return false; // center has walked outside this row's (shorter) CIR span
  }
  const uint32_t lo = (uint32_t)lo_i;
  const uint32_t hi = (uint32_t)hi_i;

  uint32_t peak     = lo;
  double   peak_pow = std::norm(cir_[lo]);
  for (uint32_t b = lo + 1; b <= hi; b++) {
    const double p = std::norm(cir_[b]);
    if (p > peak_pow) {
      peak_pow = p;
      peak     = b;
    }
  }

  // Fade gate: compare the in-window peak against the average power OUTSIDE the search window
  // (same statistic Phase 3's track_row() computes for its ISI gating below). A genuine LOS tap
  // sits far above its own sidelobe/noise floor; a faded/dropped-out row does not, and the
  // "maximum" found above is then more likely noise than signal -- see FADE_MIN_SNR_LINEAR's comment.
  double   out_energy_sum = 0.0;
  uint32_t out_count      = 0;
  for (uint32_t b = 0; b < m; b++) {
    if (b >= lo && b <= hi) {
      continue;
    }
    out_energy_sum += std::norm(cir_[b]);
    out_count++;
  }
  if (out_count >= FADE_MIN_FLOOR_SAMPLES) {
    const double floor_pow = out_energy_sum / (double)out_count;
    if (out_snr_lin != nullptr && floor_pow > 0.0) {
      *out_snr_lin = peak_pow / floor_pow;
    }
    if (floor_pow > 0.0 && peak_pow < FADE_MIN_SNR_LINEAR * floor_pow) {
      /* Flag it and FALL THROUGH. This used to `return true` here, leaving out.peak_bin /
       * frac_bin / peak_val at their defaults while still reporting success -- so any caller that
       * did not also test the fade flag silently consumed (bin 0, +0.000) as if it were a
       * measurement. The LOS seed did exactly that, and bin 0 is the earliest arrival there is, so
       * a faded row beat every real path under the earliest-arrival rule.
       *
       * The estimate is now always populated and `out_faded` is purely ADVISORY: the walker still
       * uses it to decide when to flywheel, while the seed -- which is trying to FIND the path
       * rather than track it -- can use the peak position on its own merits. A gate tuned for
       * "should I trust this step of the walk" was never the right test for "where is the path",
       * and applying it there discarded the rows the seed most needed. */
      out_faded = true;
    }
  }

  // Sub-bin estimate: complex-domain Jacobsen/Candan-form ratio, scale-corrected for this pipeline's
  // Hann-windowed compact CIR (see HANN_ESTIMATOR_SCALE's comment -- replaces the former power-based
  // parabolic fit, which had ~0.1 bin residual bias even after windowing).
  const double delta = subbin_delta(cir_, peak);

  out.peak_bin = peak;
  out.frac_bin = delta;
  out.peak_val = cir_[peak]; // pre-correction complex sample; Phase 2 reads its phase for CFO/CPE
  return true;
}

sto_fit_result_t cpi_sto_tracker::process(icf_t*                   h_cpi,
                                          const uint8_t*           occ_all,
                                          uint32_t                 cpi_rows,
                                          uint32_t                 nof_subc,
                                          const uint32_t*          row_comb,
                                       const uint8_t*           row_illum,
                                          const double*            row_time_slots,
                                          const nr_isac_carrier_t& carrier,
                                          double                   sfo_ppm_hint,
                                          bool                     apply_corr,
                                          double                   nominal_los_range_m)
{
  sto_fit_result_t fit;
  rows_.assign(cpi_rows, los_row_estimate_t());

  // Walking-tracker state resets every CPI -- each CPI re-acquires from the nominal bin (see class
  // comment in isac_sync.h for why this does NOT persist cross-CPI, unlike cpi_sfo_tracker's EMA).
  for (unsigned q = 0; q < NR_ISAC_NUM_ILLUM; q++) {
    current_center_bin_[q] = -1;
  }
  for (unsigned q = 0; q < NR_ISAC_NUM_ILLUM; q++) { anchor_bin_cpi_start_[q] = -1; }
  for (unsigned q = 0; q < NR_ISAC_NUM_ILLUM; q++) {
    last_locked_bin_[q] = -1;
  }
  for (unsigned q = 0; q < NR_ISAC_NUM_ILLUM; q++) { is_flywheeling_[q] = false; }

  if (cpi_rows == 0 || nof_subc == 0 || carrier.scs_hz == 0) {
    return fit;
  }

  const uint32_t nominal_bin  = nominal_los_bin(nof_subc, carrier.scs_hz, nominal_los_range_m);
  const double   bin_to_delay_s = 1.0 / ((double)nof_subc * (double)carrier.scs_hz);

  const double slots_per_sf = std::max(1.0, (double)carrier.scs_hz / 15000.0);
  const double slot_dur_s   = 1e-3 / slots_per_sf;

  /* ---- MEASURED LOS SEED ------------------------------------------------------------------
   * The walker used to seed row 0 at nominal_los_bin(nominal_los_range_m), a CONFIG CONSTANT whose
   * default is 98.0 m -- a TX/RX separation carried over from an old B210 bench setup. If the real
   * direct path is not there, every row's in-window "peak" is noise, correctly fails the
   * FADE_MIN_SNR_LINEAR gate, and the whole CPI flywheels: measured 2026-08-31 as n_valid=0 with
   * ~100 flywheel rows on EVERY CPI, which also starves Phase 2 (CFO) and Phase 3 (SFO) of the
   * locked rows they fit. Widening the pool from 64 to 512 slots cut submission drops 78 % -> 26 %
   * and did NOT move it, which is what ruled out row starvation as the cause.
   *
   * So measure it: sweep the search window across the CIR on the first row that locks and keep the
   * center with the strongest in-window peak. Reuses estimate_row() unchanged -- same fade gate,
   * same comb handling -- so the seed is found by exactly the test every later row must pass.
   * nominal_los_range_m survives only as the fallback when no window anywhere clears the gate. */
  /* One seed PER ILLUMINATOR. The gNB lights the downlink rows and a UE the uplink ones, from
   * different positions, so their direct paths are at different delays and a shared seed sends one
   * walker hunting the other transmitter's path. */
  uint32_t seed_bin[NR_ISAC_NUM_ILLUM];
  for (unsigned q = 0; q < NR_ISAC_NUM_ILLUM; q++) {
    seed_bin[q] = nominal_bin;
  }
  for (unsigned il = 0; il < NR_ISAC_NUM_ILLUM; il++) {
  {
    double         best_snr = -1.0;
    int            best_bin = -1;
    const uint32_t step     = 2 * SEARCH_HALFWIN_BINS + 1;

    /* One candidate per polled row, carried as a SUB-BIN position (peak_bin + frac_bin). The
     * fractional part is the parabolic offset estimate_row() already computes for every row and
     * that this seed used to discard by keeping only the integer bin -- which quantised the
     * reported LOS range to one bin, 3.05 m at 273 PRB / 30 kHz. */
    double   pos_c_dbg = -1.0; // this CPI's own estimate, before the static-LOS layer
    double   cand[LOS_SEED_POLL_ROWS];
    uint32_t cand_pk[LOS_SEED_POLL_ROWS];
    double   cand_fr[LOS_SEED_POLL_ROWS];
    double   cand_snr[LOS_SEED_POLL_ROWS];
    uint32_t n_cand = 0;

    for (uint32_t r0 = 0; r0 < cpi_rows && n_cand < LOS_SEED_POLL_ROWS; r0++) {
      const unsigned r0_il = (row_illum != nullptr && row_illum[r0] < NR_ISAC_NUM_ILLUM)
                                 ? (unsigned)row_illum[r0] : (unsigned)NR_ISAC_ILLUM_DL;
      if (r0_il != il) {
        continue; // this row belongs to the other transmitter
      }
      const icf_t*   row0 = &h_cpi[(size_t)r0 * nof_subc];
      const uint8_t* m0   = &occ_all[(size_t)r0 * nof_subc];

      /* Two passes over the SAME row, because "is this the direct path or a replica of it" cannot
       * be answered until the strongest peak is known. Pass 1 finds it; pass 2 takes the earliest
       * peak within LOS_SEED_EARLIEST_FRAC of it. */
      double peak_snr = -1.0;
      for (uint32_t c = SEARCH_HALFWIN_BINS + 1; c + SEARCH_HALFWIN_BINS + 2 < nof_subc; c += step) {
        los_row_estimate_t e;
        bool               f   = false;
        double             snr = -1.0;
        if (estimate_row(row0, m0, nof_subc, row_comb[r0], (int)c, SEARCH_HALFWIN_BINS, e, f, &snr)
            && snr > peak_snr) {
          peak_snr = snr;
        }
      }
      if (peak_snr <= 0.0) {
        continue; // nothing on this row cleared the fade gate; poll the next one
      }

      const double accept = peak_snr * LOS_SEED_EARLIEST_FRAC;
      for (uint32_t c = SEARCH_HALFWIN_BINS + 1; c + SEARCH_HALFWIN_BINS + 2 < nof_subc; c += step) {
        los_row_estimate_t e;
        bool               f   = false;
        double             snr = -1.0;
        /* No fade test here, deliberately. estimate_row() now always populates the estimate and
         * treats the fade flag as advisory, so a "faded" row still carries a real measured peak --
         * and the seed's job is to FIND the direct path, not to decide whether one step of a walk
         * is trustworthy. Gating the seed on the walker's fade threshold threw away most downlink
         * rows at ~7 dB and left the seed working from a handful of scattered survivors.
         *
         * What still qualifies a candidate is the RELATIVE test below (within
         * LOS_SEED_EARLIEST_FRAC of this row's own strongest peak), which needs no absolute SNR
         * threshold, plus the cross-row MAD reject and the cross-CPI mode. */
        if (estimate_row(row0, m0, nof_subc, row_comb[r0], (int)c, SEARCH_HALFWIN_BINS, e, f, &snr)
            && snr >= accept) {
          cand_snr[n_cand] = snr;
          cand[n_cand]     = (double)e.peak_bin + e.frac_bin; // earliest arrival == direct path
          /* Decomposed because the sum alone cannot say which half is wrong: a bin-0 pick and a
           * negative fraction produce the same number, and they need different fixes. */
          cand_pk[n_cand]  = e.peak_bin;
          cand_fr[n_cand]  = e.frac_bin;
          n_cand++;
          break;
        }
      }
    }

    if (n_cand > 0) {
      /* MEDIAN over the polled rows, then a MAD-based reject of the rows that disagree with it,
       * then the mean of the survivors. Median alone is robust but quantises to one candidate's
       * own value; averaging the agreeing rows recovers sub-bin precision from independent
       * measurements of the same static path. MAD (scaled to a sigma by 1.4826) is used rather
       * than a standard deviation because the outliers this rejects would inflate an SD enough to
       * keep themselves inside the gate. */
      double srt[LOS_SEED_POLL_ROWS];
      std::copy(cand, cand + n_cand, srt);
      std::sort(srt, srt + n_cand);
      const double med = (n_cand % 2) ? srt[n_cand / 2]
                                      : 0.5 * (srt[n_cand / 2 - 1] + srt[n_cand / 2]);

      double dev[LOS_SEED_POLL_ROWS];
      for (uint32_t i = 0; i < n_cand; i++) {
        dev[i] = std::fabs(cand[i] - med);
      }
      std::sort(dev, dev + n_cand);
      const double mad   = (n_cand % 2) ? dev[n_cand / 2] : 0.5 * (dev[n_cand / 2 - 1] + dev[n_cand / 2]);
      /* Floor the tolerance at half a bin: with several rows agreeing exactly, MAD is 0 and a bare
       * 3*MAD would reject every row whose sub-bin estimate differs by any amount at all. */
      const double tol = std::max(0.5, 3.0 * 1.4826 * mad);

      double   sum = 0.0, sum_snr = 0.0;
      uint32_t n_keep = 0;
      for (uint32_t i = 0; i < n_cand; i++) {
        if (std::fabs(cand[i] - med) <= tol) {
          sum += cand[i];
          sum_snr += cand_snr[i];
          n_keep++;
        }
      }
      const double pos = (n_keep > 0) ? (sum / (double)n_keep) : med;
      best_snr         = (n_keep > 0) ? (sum_snr / (double)n_keep) : -1.0;
      n_seed_rejected_[il] = n_cand - n_keep;

      /* The walker tracks an INTEGER bin, so it gets the rounded position; Phase 4 consumes the
       * sub-bin one. Clamped because a negative bin would index out of the CIR. */
      const double pos_c = (pos < 0.0) ? 0.0 : pos;
      pos_c_dbg          = pos_c;

      /* Fold this CPI's estimate into the STATIC-LOS estimate (see isac_sync.h). Once locked, a
       * seed that disagrees by more than LOS_LOCK_TOL_BINS is rejected outright -- it is a replica
       * or a fade, and the geometry says the true value cannot have moved. */
      const bool locked = (los_locked_bin_[il] >= 0.0);
      if (!locked || std::fabs(pos_c - los_locked_bin_[il]) <= LOS_LOCK_TOL_BINS) {
        los_hist_[il].push_back(pos_c);
        if (los_hist_[il].size() > LOS_HIST_MAX) {
          los_hist_[il].pop_front();
        }
      } else {
        n_cpi_rejected_[il]++;
      }
      if (los_hist_[il].size() >= LOS_LOCK_MIN_CPIS) {
        /* EARLIEST WELL-SUPPORTED ARRIVAL, not the most populous one.
         *
         * The plain mode picks whichever arrival happened to be seen most often, and in a room
         * that is frequently a REFLECTION rather than the direct path. Measured 2026-09-02 with
         * surveyed geometry (rx at the origin, gNB (0.3,4,1) -> 4.134 m, UE (0,-0.8,0) -> 0.800 m,
         * range_res 3.05 m): the mode put the gNB LOS at 8.84 m, 2.77 m late, while the earliest
         * supported cluster put it 0.14 m from truth. The UL illuminator was unaffected either way
         * -- at 0.8 m its direct path dominates so completely that nothing competes with it.
         *
         * MULTIPATH IS ALWAYS LATER, NEVER EARLIER, so among clusters that are real the first one
         * is the direct path. What stops us simply taking the minimum is that a few CPIs land
         * BELOW the truth (5 of 69 here) on noise, so the rule needs a support floor rather than
         * an extremum. Support is counted over a +/-0.5 bin window (the range mainlobe) so the
         * answer cannot depend on where a histogram cell boundary happens to fall, and the floor
         * is a FRACTION OF THE MODAL SUPPORT -- scale-free, so it does not need retuning as the
         * history fills. Sensitivity at n=69: f=0.4 -> 0.72 m, f=0.5 -> 0.14 m, f=0.6 -> 0.73 m.
         * Half the modal support is the natural value rather than a tuned one, but the choice
         * between neighbours is NOT strongly determined by one session's data -- re-measure
         * against surveyed geometry before treating 0.5 as settled. */
        size_t max_sup = 0;
        for (double c : los_hist_[il]) {
          size_t n = 0;
          for (double d : los_hist_[il]) {
            if (std::fabs(d - c) <= LOS_CLUSTER_HALFWIN_BINS) {
              n++;
            }
          }
          max_sup = std::max(max_sup, n);
        }
        const size_t floor_sup =
            std::max<size_t>(2, (size_t)std::ceil(LOS_CLUSTER_MIN_FRAC * (double)max_sup));
        double best_c = 0.0;
        size_t best_n = 0;
        bool   found  = false;
        for (double c : los_hist_[il]) {
          size_t n = 0;
          for (double d : los_hist_[il]) {
            if (std::fabs(d - c) <= LOS_CLUSTER_HALFWIN_BINS) {
              n++;
            }
          }
          if (n >= floor_sup && (!found || c < best_c)) {
            best_c = c;
            best_n = n;
            found  = true;
          }
        }
        if (found) {
          /* Sub-bin position from the members of the winning cluster, as before: the cluster says
           * WHICH arrival is real, its members say where it is. */
          double   sum = 0.0;
          uint32_t cnt = 0;
          for (double c : los_hist_[il]) {
            if (std::fabs(c - best_c) <= LOS_CLUSTER_HALFWIN_BINS) {
              sum += c;
              cnt++;
            }
          }
          if (cnt > 0) {
            los_locked_bin_[il] = sum / (double)cnt;
            n_mode_support_[il] = (uint32_t)best_n;
          }
        }
      }

      /* Report the converged value once it exists: it is a strictly better estimate of a constant
       * than any single CPI's measurement of it. */
      const double use = (los_locked_bin_[il] >= 0.0) ? los_locked_bin_[il] : pos_c;
      best_bin              = (int)std::lround(use);
      seed_bin[il]          = (uint32_t)best_bin;
      // Same bin->metre scale nominal_los_bin() inverts, so Phase 4 can consume it directly.
      measured_los_range_m_[il] = use * SPEED_OF_LIGHT / ((double)nof_subc * carrier.scs_hz);
      measured_los_bin_[il] = use;
    }
    LOG_I(PHY,
          "SENSING: sync(STO) LOS[%s] bin=%.3f -> %.2f m (%s; %s over %u CPIs mode_support=%u, "
          "%u CPIs rejected; this CPI raw=%.3f from %u rows, %u rejected, first pk=%d frac=%+.3f); "
          "nominal was %u from %.1f m; snr=%.1f dB\n",
          (il == NR_ISAC_ILLUM_UL) ? "UL/UE" : "DL/gNB",
          measured_los_bin_[il], measured_los_range_m_[il],
          (best_bin >= 0) ? "MEASURED" : "fallback: nothing cleared the gate",
          (los_locked_bin_[il] >= 0.0) ? "CONVERGED" : "converging",
          (unsigned)los_hist_[il].size(), n_mode_support_[il], n_cpi_rejected_[il],
          pos_c_dbg, n_cand, n_seed_rejected_[il],
          (n_cand > 0) ? (int)cand_pk[0] : -1, (n_cand > 0) ? cand_fr[0] : 0.0,
          nominal_bin, nominal_los_range_m,
          (best_snr > 0.0) ? 10.0 * std::log10(best_snr) : -99.0);
  }
  }

  double   last_locked_time_s = 0.0;
  uint32_t n_flywheel         = 0;

  // TEMPORARY DIAGNOSTIC (2026-07-23, WALK_HALFWIN_BINS recalibration): is the flywheel firing
  // because real drift outruns the search window, or because rows fail the SNR fade gate? Those need
  // opposite fixes (widen the window vs. lower FADE_MIN_SNR_LINEAR), so measure before tuning.
  const bool dbg_walk = (getenv("ISAC_DEBUG_WALK") != nullptr);
  int        dbg_prev_lock_bin = -1;
  uint32_t   dbg_step_hist[8]  = {0}; // |bin step| between consecutive locked rows: 0,1,2,3,4,5,6,>=7
  uint32_t   dbg_at_edge       = 0;   // locked rows whose peak sat exactly at the window edge
  double     dbg_snr_sum       = 0.0;
  uint32_t   dbg_snr_n = 0, dbg_snr_fail = 0, dbg_snr_marginal = 0;

  for (uint32_t r = 0; r < cpi_rows; r++) {
    los_row_estimate_t est;
    est.row    = r;
    est.time_s = row_time_slots[r] * slot_dur_s;
    const icf_t*   row  = &h_cpi[(size_t)r * nof_subc];
    const uint8_t* mask = &occ_all[(size_t)r * nof_subc];

    // Seed from the nominal bin with the wide seed window until the walker locks for the first
    // time this CPI; every subsequent row searches a narrow window re-centered on the previous
    // LOCKED row's own peak (SYNC_NOISE_HANDOVER.md's root-cause fix -- see class comment).
    /* Which transmitter lit this row. The downlink sources come from the gNB and the uplink ones
     * from a UE: different positions, different direct-path delays, therefore SEPARATE walkers.
     * Running one walker across both makes it chase the difference between two illuminators. */
    const unsigned il = (row_illum != nullptr && row_illum[r] < NR_ISAC_NUM_ILLUM)
                            ? (unsigned)row_illum[r]
                            : (unsigned)NR_ISAC_ILLUM_DL;
    const int      center  = (current_center_bin_[il] >= 0) ? current_center_bin_[il] : (int)seed_bin[il];
    const uint32_t halfwin = (current_center_bin_[il] >= 0) ? WALK_HALFWIN_BINS : SEARCH_HALFWIN_BINS;

    bool   faded   = false;
    double snr_lin = -1.0;
    const bool ok = estimate_row(row, mask, nof_subc, row_comb[r], center, halfwin, est, faded, &snr_lin);
    if (dbg_walk && snr_lin >= 0.0) {
      dbg_snr_sum += snr_lin;
      dbg_snr_n++;
      if (snr_lin < FADE_MIN_SNR_LINEAR) {
        dbg_snr_fail++;
        if (snr_lin > 0.5 * FADE_MIN_SNR_LINEAR) {
          dbg_snr_marginal++; // within 3 dB of passing => threshold is the binding limit, not signal absence
        }
      }
    }

    if (ok && !faded) {
      // Locked: adopt this row's peak as the walk's new position AND as the new stable reference
      // future flywheel projections measure elapsed time/drift from.
      if (current_center_bin_[il] < 0) {
        anchor_bin_cpi_start_[il] = (int)est.peak_bin;
      }
      current_center_bin_[il] = (int)est.peak_bin;
      last_locked_bin_[il]    = current_center_bin_[il];
      last_locked_time_s      = est.time_s;
      is_flywheeling_[il]     = false;
      est.flywheeling       = false;
      est.valid             = true;
      est.absolute_drift_bins =
          (double)(current_center_bin_[il] - anchor_bin_cpi_start_[il]) + est.frac_bin;
      if (dbg_walk) {
        if (dbg_prev_lock_bin >= 0) {
          const int step = std::abs(current_center_bin_[il] - dbg_prev_lock_bin);
          dbg_step_hist[std::min(step, 7)]++;
          if (step >= (int)halfwin) {
            dbg_at_edge++; // peak pinned at the window boundary => window may be the binding limit
          }
        }
        dbg_prev_lock_bin = current_center_bin_[il];
      }
    } else if (ok && faded) {
      // Faded: don't trust the in-window maximum. Project the walk forward using the most recent
      // cross-CPI SFO estimate (necessarily last CPI's, see process()'s @p sfo_ppm_hint doc) rather
      // than leaving the walk stuck (which would otherwise let the NEXT row's window miss the real
      // drifted peak entirely once the fade clears). Projects as an ABSOLUTE offset from
      // last_locked_bin_ (fixed at the moment of the last real lock), NOT by incrementing
      // current_center_bin_ itself -- the latter would re-apply the same elapsed-time-scaled
      // prediction on top of an already-shifted position every consecutive flywheel row, compounding
      // quadratically over a sustained fade (caught via a live sensing_sim run showing +-1000+ bin
      // per-CPI swings from a handful of dt-scaled ppm projections that should have summed to O(10)).
      is_flywheeling_[il] = true;
      n_flywheel++;
      if (last_locked_bin_[il] >= 0) {
        const double dt_s              = est.time_s - last_locked_time_s;
        const double predicted_delay_s = sfo_ppm_hint * 1.0e-6 * dt_s;
        const int    predicted_bins    = (int)std::lround(predicted_delay_s / bin_to_delay_s);
        current_center_bin_[il] = last_locked_bin_[il] + predicted_bins;
      }
      est.flywheeling = true;
      est.valid        = false; // projection, not a measurement -- Phase 2's CFO fit must skip this row
      est.peak_bin     = (uint32_t)std::max(0, current_center_bin_[il]);
      est.frac_bin     = 0.0;
      est.absolute_drift_bins = (current_center_bin_[il] >= 0 && anchor_bin_cpi_start_[il] >= 0)
                                    ? (double)(current_center_bin_[il] - anchor_bin_cpi_start_[il])
                                    : 0.0;
      LOG_W(PHY, "SENSING: sync(STO) row=%u flywheeling (signal fade) -- projected center_bin=%d\n", r,
            current_center_bin_[il]);
    } else {
      // Structural failure (bad occupancy / too-short CIR / center walked off this row's shorter
      // CIR span) -- leave invalid, don't move the walk.
      est.valid = false;
    }
    rows_[r] = est;
  }

  // Least-squares fit of sub-bin delay vs. each row's own absolute time (irregular spacing), locked
  // rows only. See class comment for why this residual is now expected to be small/flat even under
  // real drift (the walker absorbs it into the integer bin instead).
  double   sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
  double   tmin = 0.0, tmax = 0.0;
  bool     have_range = false;
  uint32_t n          = 0;
  for (const auto& e : rows_) {
    if (!e.valid) {
      continue;
    }
    sx += e.time_s;
    sy += e.frac_bin;
    sxx += e.time_s * e.time_s;
    sxy += e.time_s * e.frac_bin;
    n++;
    if (!have_range) {
      tmin = tmax  = e.time_s;
      have_range   = true;
    } else {
      tmin = std::min(tmin, e.time_s);
      tmax = std::max(tmax, e.time_s);
    }
  }
  fit.n_valid    = n;
  fit.n_flywheel = n_flywheel;
  for (unsigned q = 0; q < NR_ISAC_NUM_ILLUM; q++) {
    fit.los_seed_bin[q] = measured_los_bin_[q];
    fit.los_range_m[q]  = measured_los_range_m_[q];
  }
  fit.n_seed_rejected = n_seed_rejected_[NR_ISAC_ILLUM_DL] + n_seed_rejected_[NR_ISAC_ILLUM_UL];

  if (dbg_walk) {
    LOG_I(PHY,
          "ISAC_DBG_WALK cpi_rows=%u locked=%u fly=%u | step_hist(|dbin| 0..6,>=7)=%u,%u,%u,%u,%u,%u,%u,%u "
          "at_window_edge=%u | snr_lin mean=%.2f gate=%.1f fail=%u/%u marginal(within 3dB)=%u\n",
          cpi_rows, n, n_flywheel, dbg_step_hist[0], dbg_step_hist[1], dbg_step_hist[2], dbg_step_hist[3],
          dbg_step_hist[4], dbg_step_hist[5], dbg_step_hist[6], dbg_step_hist[7], dbg_at_edge,
          (dbg_snr_n > 0) ? dbg_snr_sum / (double)dbg_snr_n : 0.0, FADE_MIN_SNR_LINEAR, dbg_snr_fail,
          dbg_snr_n, dbg_snr_marginal);
  }

  // Total walked drift this CPI: the last locked row's cumulative walk (0 if never locked).
  for (auto it = rows_.rbegin(); it != rows_.rend(); ++it) {
    if (it->valid) {
      fit.total_drift_bins = it->absolute_drift_bins;
      break;
    }
  }

  if (n < MIN_VALID_ROWS) {
    LOG_I(PHY,
          "SENSING: sync(STO) CPI has only %u valid LOS rows (need >=%u, n_flywheel=%u) -- skipping "
          "fit/correction\n",
          n, MIN_VALID_ROWS, n_flywheel);
    return fit;
  }

  const double denom = (double)n * sxx - sx * sx;
  double       slope = 0.0;
  if (std::abs(denom) > 1e-30) {
    slope = ((double)n * sxy - sx * sy) / denom;
  }
  fit.slope_bins_per_s = slope;
  fit.mean_frac_bin    = sy / (double)n;
  fit.drift_bins_cpi   = std::abs(slope * (tmax - tmin));
  fit.is_constant      = fit.drift_bins_cpi < DRIFT_BIN_THRESHOLD;

  uint32_t pk_min = UINT32_MAX, pk_max = 0;
  for (const auto& e : rows_) {
    if (e.valid) {
      pk_min = std::min(pk_min, e.peak_bin);
      pk_max = std::max(pk_max, e.peak_bin);
    }
  }

  if (fit.is_constant) {
    if (apply_corr) {
      apply_correction(h_cpi, occ_all, cpi_rows, nof_subc, fit.mean_frac_bin);
    }
    LOG_I(PHY,
          "SENSING: sync(STO) n_valid=%u n_flywheel=%u peak_bin=[%u..%u] mean_frac=%.3f bins "
          "drift=%.3f bins/CPI (<%.2f) total_walk=%.2f bins -> corrected fine STO\n",
          n, n_flywheel, pk_min, pk_max, fit.mean_frac_bin, fit.drift_bins_cpi, DRIFT_BIN_THRESHOLD,
          fit.total_drift_bins);
  } else {
    LOG_I(PHY,
          "SENSING: sync(STO) n_valid=%u n_flywheel=%u peak_bin=[%u..%u] mean_frac=%.3f bins "
          "drift=%.3f bins/CPI (>=%.2f) total_walk=%.2f bins -> SFO leaking through, deferring "
          "correction (Phase 3), no STO correction applied\n",
          n, n_flywheel, pk_min, pk_max, fit.mean_frac_bin, fit.drift_bins_cpi, DRIFT_BIN_THRESHOLD,
          fit.total_drift_bins);
  }

  return fit;
}

void cpi_sto_tracker::apply_correction(icf_t*         h_cpi,
                                       const uint8_t* occ_all,
                                       uint32_t       cpi_rows,
                                       uint32_t       nof_subc,
                                       double         mean_frac_bin)
{
  // Null only the sub-bin (fractional) component of the LOS delay -- not the row's integer CIR bin,
  // which sits at the already-diagnosed ~49 m group-delay artifact and is out of scope here (see
  // docs/NR_UE_ISAC_sync_gap_analysis.md). A common frequency-domain phase ramp applied identically
  // to every row is equivalent to, and cheaper than, a fractional-delay time-domain resample when
  // only a sub-bin shift is being removed.
  const double phase_per_subc = 2.0 * M_PI * mean_frac_bin / (double)nof_subc;
  for (uint32_t r = 0; r < cpi_rows; r++) {
    icf_t*         row  = &h_cpi[(size_t)r * nof_subc];
    const uint8_t* mask = &occ_all[(size_t)r * nof_subc];
    for (uint32_t c = 0; c < nof_subc; c++) {
      if (!mask[c]) {
        continue;
      }
      const double phase = phase_per_subc * (double)c;
      row[c] *= icf_t((float)std::cos(phase), (float)std::sin(phase));
    }
  }
}

cfo_fit_result_t cpi_cfo_tracker::process(icf_t* h_cpi, const uint8_t* occ_all, uint32_t nof_subc,
                                          const std::vector<los_row_estimate_t>& rows)
{
  cfo_fit_result_t fit;

  // Sequential phase unwrap across valid rows, in time order (rows are already time-ordered by
  // construction -- see sensing_engine.cc's absolute-slot-indexed row accumulation).
  std::vector<uint32_t> valid_idx;
  std::vector<double>   raw_phase;
  std::vector<double>   unwrapped;
  double prev_unwrapped = 0.0;
  double prev_raw       = 0.0;
  bool   have_prev      = false;
  for (const auto& e : rows) {
    if (!e.valid) {
      continue;
    }
    const double raw = std::atan2((double)e.peak_val.imag(), (double)e.peak_val.real());
    double       u;
    if (!have_prev) {
      u          = raw;
      have_prev  = true;
    } else {
      double delta = raw - prev_raw;
      delta -= std::round(delta / (2.0 * M_PI)) * 2.0 * M_PI; // wrap into (-pi, pi]
      u = prev_unwrapped + delta;
    }
    valid_idx.push_back(e.row);
    raw_phase.push_back(raw);
    unwrapped.push_back(u);
    prev_unwrapped = u;
    prev_raw       = raw;
  }

  fit.n_valid = (uint32_t)valid_idx.size();
  if (fit.n_valid < CFO_MIN_VALID_ROWS) {
    LOG_I(PHY, "SENSING: sync(CFO) CPI has only %u valid LOS-phase rows (need >=%u) -- skipping fit/correction\n",
          fit.n_valid, CFO_MIN_VALID_ROWS);
    return fit;
  }

  // Least-squares fit of unwrapped phase vs. each row's own absolute time (irregular spacing).
  double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
  const double n = (double)valid_idx.size();
  for (size_t i = 0; i < valid_idx.size(); i++) {
    const double x = rows[valid_idx[i]].time_s;
    const double y = unwrapped[i];
    sx += x;
    sy += y;
    sxx += x * x;
    sxy += x * y;
  }
  const double denom = n * sxx - sx * sx;
  double       slope = 0.0, intercept = 0.0;
  if (std::abs(denom) > 1e-30) {
    slope     = (n * sxy - sx * sy) / denom;
    intercept = (sy - slope * sx) / n;
  } else {
    intercept = sy / n;
  }
  fit.cfo_hz = slope / (2.0 * M_PI);

  double sq_resid = 0.0;
  for (size_t i = 0; i < valid_idx.size(); i++) {
    const double predicted = slope * rows[valid_idx[i]].time_s + intercept;
    const double resid     = unwrapped[i] - predicted;
    sq_resid += resid * resid;
  }
  fit.residual_phase_rms_rad = std::sqrt(sq_resid / n);

  // Slow cross-CPI alpha-beta tracker (diagnostic only -- the correction below uses each row's own
  // raw observed phase, not this filtered value; Phase 4's closed loop is where a filtered/fed-back
  // estimate would actually be consumed). dt is taken as 1 CPI tick, not elapsed seconds: simple and
  // sufficient per the task, and sidesteps needing an absolute across-CPI clock (cpi_row_time resets
  // every CPI by design).
  if (!state_init_) {
    cfo_state_hz_        = fit.cfo_hz;
    cfo_rate_hz_per_cpi_ = 0.0;
    state_init_          = true;
  } else {
    const double predicted = cfo_state_hz_ + cfo_rate_hz_per_cpi_;
    const double residual  = fit.cfo_hz - predicted;
    cfo_state_hz_          = predicted + CFO_ALPHA * residual;
    cfo_rate_hz_per_cpi_   = cfo_rate_hz_per_cpi_ + CFO_BETA * residual;
  }
  fit.cfo_hz_filtered     = cfo_state_hz_;
  fit.cfo_rate_hz_per_cpi = cfo_rate_hz_per_cpi_;

  // De-rotate every valid row's occupied subcarriers by its own raw observed LOS-tap phase -- a
  // uniform (not per-subcarrier) rotation, since CFO/CPE is common to every subcarrier of a row.
  for (size_t i = 0; i < valid_idx.size(); i++) {
    const uint32_t r   = valid_idx[i];
    const icf_t    rot((float)std::cos(-raw_phase[i]), (float)std::sin(-raw_phase[i]));
    icf_t*         row  = &h_cpi[(size_t)r * nof_subc];
    const uint8_t* mask = &occ_all[(size_t)r * nof_subc];
    for (uint32_t c = 0; c < nof_subc; c++) {
      if (mask[c]) {
        row[c] *= rot;
      }
    }
  }

  LOG_I(PHY,
        "SENSING: sync(CFO) n_valid=%u cfo=%.2f Hz (filtered=%.2f Hz, rate=%.3f Hz/CPI) resid_phase_rms=%.3f rad "
        "-> CPE-corrected %u rows\n",
        fit.n_valid, fit.cfo_hz, fit.cfo_hz_filtered, fit.cfo_rate_hz_per_cpi, fit.residual_phase_rms_rad,
        fit.n_valid);

  return fit;
}

double cpi_sfo_tracker::leakage_model::expected_ratio(uint32_t M, double frac)
{
  entry_t* e = nullptr;
  for (auto& c : cache_) {
    if (c.M == M) {
      e = &c;
      break;
    }
  }
  if (e == nullptr) {
    cache_.push_back(entry_t{M, {}, {}});
    e = &cache_.back();
    build_leakage_curve(M, SFO_TRACK_HALFWIN_BINS, e->frac_grid, e->ratio);
  }

  const double af = std::min(0.5, std::abs(frac));
  const size_t n  = e->frac_grid.size();
  if (n == 0) {
    return 1.0;
  }
  if (af <= e->frac_grid.front()) {
    return e->ratio.front();
  }
  if (af >= e->frac_grid.back()) {
    return e->ratio.back();
  }
  // Linear interpolation over the uniform grid.
  for (size_t i = 1; i < n; i++) {
    if (af <= e->frac_grid[i]) {
      const double x0 = e->frac_grid[i - 1], x1 = e->frac_grid[i];
      const double y0 = e->ratio[i - 1], y1 = e->ratio[i];
      const double t  = (x1 > x0) ? (af - x0) / (x1 - x0) : 0.0;
      return y0 + t * (y1 - y0);
    }
  }
  return e->ratio.back();
}

cpi_sfo_tracker::comb_stats_t& cpi_sfo_tracker::stats_for(uint32_t comb)
{
  for (auto& s : comb_stats_) {
    if (s.comb == comb) {
      return s;
    }
  }
  comb_stats_.push_back(comb_stats_t{comb, {}});
  return comb_stats_.back();
}

bool cpi_sfo_tracker::track_row(const icf_t* row, const uint8_t* mask, uint32_t nof_subc, uint32_t comb,
                                uint32_t anchor_bin, uint32_t& out_peak, double& out_frac,
                                double& out_leakage_anomaly, bool& out_faded)
{
  out_faded = false;
  if (!cir_builder_.build(row, mask, nof_subc, comb, cir_)) {
    return false;
  }
  const uint32_t m = (uint32_t)cir_.size();
  if (m < 2 * SFO_TRACK_HALFWIN_BINS + 3) {
    return false; // too short to window-search with parabola margin
  }

  uint32_t lo = (anchor_bin > SFO_TRACK_HALFWIN_BINS) ? anchor_bin - SFO_TRACK_HALFWIN_BINS : 1;
  uint32_t hi = std::min(anchor_bin + SFO_TRACK_HALFWIN_BINS, m - 2);
  if (lo < 1) {
    lo = 1;
  }
  if (hi <= lo) {
    return false; // anchor has walked outside this row's (shorter) CIR span
  }

  uint32_t peak     = lo;
  double   peak_pow = std::norm(cir_[lo]);
  for (uint32_t b = lo + 1; b <= hi; b++) {
    const double p = std::norm(cir_[b]);
    if (p > peak_pow) {
      peak_pow = p;
      peak     = b;
    }
  }

  // Sub-bin estimate: same scale-corrected complex-domain estimator as Phase 1's estimate_row() (see
  // subbin_delta() / HANN_ESTIMATOR_SCALE's comment) -- both search a Hann-windowed compact CIR peak.
  const double delta = subbin_delta(cir_, peak);

  // Out-of-window energy: average CIR power outside [lo, hi] -- the raw ISI/contamination indicator.
  double   out_energy_sum = 0.0;
  uint32_t out_count      = 0;
  for (uint32_t b = 0; b < m; b++) {
    if (b >= lo && b <= hi) {
      continue;
    }
    out_energy_sum += std::norm(cir_[b]);
    out_count++;
  }
  const double out_win_energy_per_bin = (out_count > 0) ? (out_energy_sum / (double)out_count) : 0.0;

  // Fade gate (ROOT-CAUSE FIX, 2026-07-27): Phase 1's estimate_row() has always compared its in-window
  // peak against this exact out-of-window floor statistic and refused to trust a "peak" that isn't
  // meaningfully above it (FADE_MIN_SNR_LINEAR) -- Phase 3's own independent walk never had the
  // equivalent check, so on a low-SNR row it silently accepted whatever bin the argmax landed on,
  // noise included. MEASURED, not theorised: a `sensing_channel_set_rx_clock()`-injected KNOWN linear
  // SFO ramp (0/2/0.5 ppm) produced a per-row tau_s series with ~3.3-3.5 RANGE-BIN RMS scatter even
  // with the injected drift alone the true expected signal, EQUAL to the scatter seen with NO drift
  // injected at all -- i.e. most rows were contributing pure noise indistinguishable from a real
  // signal, which is exactly what an unconditional argmax over a window does on a faded row. This
  // single gap is sufficient to explain why the line fit's R^2 never cleared SFO_MIN_R_SQUARED even
  // under a genuine, noise-free synthetic ramp: enough noise-dominated rows corrupt any fit, no matter
  // how clean the true underlying drift is.
  if (out_count >= FADE_MIN_FLOOR_SAMPLES && out_win_energy_per_bin > 0.0
      && peak_pow < FADE_MIN_SNR_LINEAR * out_win_energy_per_bin) {
    out_faded = true;
    // Still report peak/frac/anomaly (mirrors Phase 1: "structurally fine row, just not trustworthy")
    // so a caller that wants the raw value for diagnostics can have it; process() below is the one
    // that must exclude a faded row from the fit and from advancing the walk's anchor.
  }

  // Normalize by this (M, frac)'s expected clean-signal leakage (gap-analysis doc sec. 12.3
  // follow-up): raw out_win_energy_per_bin swings 3-4 orders of magnitude as an entirely benign
  // function of M and the row's own fractional peak position (see leakage_model's comment in
  // isac_sync.h) -- dividing by peak_pow and by the modelled expected ratio at this exact (m, delta)
  // collapses that swing to a dimensionless anomaly score that should sit near 1.0 for a clean row
  // regardless of M/frac, so process()'s sliding-window sigma check only has to catch genuine
  // deviations from that expectation.
  const double expected = leakage_model_.expected_ratio(m, delta);
  const double observed = (peak_pow > 0.0) ? (out_win_energy_per_bin / peak_pow) : 0.0;
  out_leakage_anomaly    = (expected > 0.0) ? (observed / expected) : observed;

  out_peak = peak;
  out_frac = delta;
  return true;
}

sfo_fit_result_t cpi_sfo_tracker::process(icf_t*                   h_cpi,
                                          const uint8_t*           occ_all,
                                          uint32_t                 cpi_rows,
                                          uint32_t                 nof_subc,
                                          const uint32_t*          row_comb,
                                          const double*            row_time_slots,
                                          const nr_isac_carrier_t& carrier,
                                          double                   nominal_los_range_m)
{
  sfo_fit_result_t fit;
  rows_.clear();
  comb_stats_.clear();

  if (cpi_rows == 0 || nof_subc == 0 || carrier.scs_hz == 0) {
    return fit;
  }

  const double bin_to_delay_s = 1.0 / ((double)nof_subc * (double)carrier.scs_hz);
  const double slots_per_sf   = std::max(1.0, (double)carrier.scs_hz / 15000.0);
  const double slot_dur_s     = 1e-3 / slots_per_sf;

  // Sequential tracking walk: each row's search window is re-centered on the PREVIOUS ACCEPTED
  // row's own peak (not a fixed nominal bin), so cumulative drift across the CPI can be arbitrarily
  // large as long as the per-step drift between consecutive valid, non-ISI-excluded rows stays
  // within SFO_TRACK_HALFWIN_BINS. The very first row seeds from the same nominal bin Phase 1 uses.
  uint32_t anchor = nominal_los_bin(nof_subc, carrier.scs_hz, nominal_los_range_m);

  rows_.reserve(cpi_rows);
  for (uint32_t r = 0; r < cpi_rows; r++) {
    sfo_row_t rowinfo;
    rowinfo.row    = r;
    rowinfo.time_s = row_time_slots[r] * slot_dur_s;
    rowinfo.comb   = row_comb[r];

    const icf_t*   row_ptr = &h_cpi[(size_t)r * nof_subc];
    const uint8_t* mask    = &occ_all[(size_t)r * nof_subc];

    uint32_t peak = 0;
    double   frac = 0.0, anomaly = 0.0;
    bool     faded = false;
    rowinfo.valid = track_row(row_ptr, mask, nof_subc, row_comb[r], anchor, peak, frac, anomaly, faded);
    rowinfo.excluded_fade = faded;

    if (rowinfo.valid && faded) {
      // Same treatment as an ISI exclusion: don't trust this row's peak for the fit, and don't move
      // the walk's anchor off of it either (a noise-dominated argmax is not a place to re-center the
      // NEXT row's search window). Unlike Phase 1 there is no flywheel/dead-reckoning projection here
      // -- the anchor simply holds, which is safe because SFO_TRACK_HALFWIN_BINS re-searches around
      // it again next row regardless.
      LOG_D(PHY, "SENSING: sync(SFO) row=%u comb=%u excluded (faded): peak below SNR gate\n", r, row_comb[r]);
    } else if (rowinfo.valid) {
      rowinfo.tau_s           = ((double)peak + frac) * bin_to_delay_s;
      rowinfo.leakage_anomaly = anomaly;

      // Sliding-window, per-comb ISI check using only the last SFO_ISI_LOCAL_WINDOW ACCEPTED rows of
      // the same comb ("neighboring rows", not the whole CPI since it started -- see comb_stats_t's
      // comment for why a whole-CPI cumulative baseline over-triggered in testing). anomaly is now
      // leakage_model-normalized (~1.0 nominal for a clean row, see track_row), not raw energy, so
      // this sigma check only needs to catch genuine deviations from that expectation -- see
      // SFO_ISI_SIGMA's comment for why its threshold could be tightened back towards the task's
      // original spec once this normalization landed.
      comb_stats_t& st = stats_for(row_comb[r]);
      bool          isi = false;
      if (st.recent.size() >= SFO_ISI_MIN_GROUP_ROWS) {
        double mean = 0.0;
        for (double v : st.recent) {
          mean += v;
        }
        mean /= (double)st.recent.size();
        double m2 = 0.0;
        for (double v : st.recent) {
          m2 += (v - mean) * (v - mean);
        }
        const double std_dev = std::sqrt(m2 / (double)st.recent.size());
        if (std_dev > 0.0 && anomaly > mean + SFO_ISI_SIGMA * std_dev) {
          isi = true;
          // Phase 5 instrumentation: per-row exclusion detail (which row, why). LOG_D since this can
          // fire multiple times per CPI; the LOG_I summary below covers the per-CPI count.
          LOG_D(PHY,
                "SENSING: sync(SFO) row=%u comb=%u excluded (ISI): leakage_anomaly=%.3f > local_mean=%.3f + "
                "%.1f*stddev=%.3f (n_local=%zu)\n",
                r, row_comb[r], anomaly, mean, SFO_ISI_SIGMA, std_dev, st.recent.size());
        }
      }
      rowinfo.excluded_isi = isi;

      if (!isi) {
        st.recent.push_back(anomaly);
        if (st.recent.size() > SFO_ISI_LOCAL_WINDOW) {
          st.recent.pop_front();
        }
        anchor = peak; // advance the walk only on accepted (non-anomalous) rows
      }
    }

    rows_.push_back(rowinfo);
  }

  uint32_t n_candidate = 0, n_excluded = 0, n_isi = 0, n_fade = 0;
  for (const auto& e : rows_) {
    if (e.valid) {
      n_candidate++;
      if (e.excluded_isi) {
        n_isi++;
      }
      if (e.excluded_fade) {
        n_fade++;
      }
      if (e.excluded_isi || e.excluded_fade) {
        n_excluded++;
      }
    }
  }
  fit.n_candidate    = n_candidate;
  fit.n_excluded_isi = n_excluded;

  // Least-squares fit of {time, delay} over surviving (valid, non-ISI-excluded, non-faded) rows.
  double   sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
  uint32_t n  = 0;
  for (const auto& e : rows_) {
    if (!e.valid || e.excluded_isi || e.excluded_fade) {
      continue;
    }
    sx += e.time_s;
    sy += e.tau_s;
    sxx += e.time_s * e.time_s;
    sxy += e.time_s * e.tau_s;
    n++;
  }
  fit.n_fit = n;

  if (n < SFO_MIN_VALID_ROWS) {
    LOG_I(PHY,
          "SENSING: sync(SFO) CPI candidates=%u excluded_isi=%u fit_rows=%u (need >=%u) -- skipping fit/correction\n",
          n_candidate, n_excluded, n, SFO_MIN_VALID_ROWS);
    return fit;
  }

  const double denom = (double)n * sxx - sx * sx;
  double       slope = 0.0, intercept = 0.0;
  if (std::abs(denom) > 1e-30) {
    slope     = ((double)n * sxy - sx * sy) / denom;
    intercept = (sy - slope * sx) / (double)n;
  } else {
    intercept = sy / (double)n;
  }

  // Goodness of the delay-vs-time line fit. A genuine sample-clock error makes tau advance LINEARLY
  // with time, so a real SFO shows a tight fit; a noise-driven fit does not. R^2 = 1 - SS_res/SS_tot.
  {
    double ss_res = 0.0, ss_tot = 0.0;
    const double ybar = sy / (double)n;
    for (const auto& e : rows_) {
      if (!e.valid || e.excluded_isi || e.excluded_fade) {
        continue;
      }
      const double pred = slope * e.time_s + intercept;
      ss_res += (e.tau_s - pred) * (e.tau_s - pred);
      ss_tot += (e.tau_s - ybar) * (e.tau_s - ybar);
    }
    fit.r_squared = (ss_tot > 0.0) ? (1.0 - ss_res / ss_tot) : 0.0;
    fit.resid_bins = (n > 0 && bin_to_delay_s > 0.0) ? std::sqrt(ss_res / (double)n) / bin_to_delay_s : 0.0;
  }

  // slope is delay-drift rate in seconds-of-delay per second-elapsed == the dimensionless fractional
  // sample-clock error (ppm/1e6) directly -- no further unit conversion needed beyond x1e6.
  fit.sfo_ppm               = slope * 1.0e6;
  fit.sample_clock_error_hz = fit.sfo_ppm * 1.0e-6 * SAMPLE_RATE_HZ;

  // Fit-quality gate (see SFO_MIN_R_SQUARED): a poorly-fitting line means the slope is noise, and
  // applying it is actively harmful. Report the estimate, withhold the correction.
  const double slope_t = (n > 2 && fit.r_squared > 0.0 && fit.r_squared < 1.0)
                             ? std::sqrt(fit.r_squared * (double)(n - 2) / (1.0 - fit.r_squared))
                             : 0.0;
  if (fit.r_squared < SFO_MIN_R_SQUARED && slope_t < SFO_MIN_SLOPE_T) {
    fit.corrected = false;
    LOG_I(PHY,
          "SENSING: sync(SFO) fit_rows=%u/candidates=%u (excl_isi=%u excl_fade=%u) r2=%.3f < %.2f "
          "t=%.1f < %.1f (resid=%.2f bins) -- slope %.4f ppm is not a linear clock drift; "
          "correction WITHHELD\n",
          n, n_candidate, n_isi, n_fade, fit.r_squared, SFO_MIN_R_SQUARED, slope_t, SFO_MIN_SLOPE_T,
          fit.resid_bins, fit.sfo_ppm);
    return fit;
  }
  fit.corrected             = true;

  // Cross-CPI EMA (see cpi_sfo_tracker's "CROSS-CPI SMOOTHING" class comment): the CORRECTION below
  // uses the FILTERED ppm, not this CPI's raw fit -- unlike CFO's alpha-beta state (diagnostic only,
  // since Phase 2's correction bypasses its own fit entirely), Phase 3's correction is directly
  // `slope * time_s[row]`, so a noisy/wrong-sign raw fit would otherwise inject its full error into
  // every row of that CPI.
  if (!sfo_state_init_) {
    sfo_ppm_state_  = fit.sfo_ppm;
    sfo_state_init_ = true;
  } else {
    sfo_ppm_state_ = (1.0 - SFO_EMA_ALPHA) * sfo_ppm_state_ + SFO_EMA_ALPHA * fit.sfo_ppm;
  }
  fit.sfo_ppm_filtered = sfo_ppm_state_;
  const double slope_applied = sfo_ppm_state_ * 1.0e-6;

  // Correct EVERY row (not just fit-contributing ones) using the FILTERED slope's continuous
  // prediction at that row's own time, anchored so the correction is zero at CPI start (t=0):
  // tau_correct(row) = slope_applied * time_s[row] only -- the fit's intercept (its value at t=0,
  // i.e. wherever the absolute delay level sits, including the diagnosed ~49 m / bin-6 group delay)
  // is deliberately excluded, mirroring Phase 1's fractional-only correction boundary.
  for (uint32_t r = 0; r < cpi_rows; r++) {
    const double   tau_correct = slope_applied * (row_time_slots[r] * slot_dur_s);
    const double   phase_per_subc = 2.0 * M_PI * (double)carrier.scs_hz * tau_correct;
    icf_t*         row_ptr = &h_cpi[(size_t)r * nof_subc];
    const uint8_t* mask    = &occ_all[(size_t)r * nof_subc];
    for (uint32_t c = 0; c < nof_subc; c++) {
      if (!mask[c]) {
        continue;
      }
      const double phase = phase_per_subc * (double)c;
      row_ptr[c] *= icf_t((float)std::cos(phase), (float)std::sin(phase));
    }
  }

  LOG_I(PHY,
        "SENSING: sync(SFO) candidates=%u excluded_isi=%u fit_rows=%u r2=%.3f t=%.1f resid=%.2f bins sfo_raw=%.4f ppm sfo_filt=%.4f ppm "
        "(%.3f Hz @ %.2f MHz sample rate) -> corrected %u rows\n",
        n_candidate, n_excluded, n, fit.r_squared, slope_t, fit.resid_bins, fit.sfo_ppm, fit.sfo_ppm_filtered,
        fit.sample_clock_error_hz,
        SAMPLE_RATE_HZ / 1e6, cpi_rows);

  return fit;
}

los_residual_t los_baseline_tracker::update_residual(const std::vector<sensing_detection_t>& detections,
                                                     const sensing_rvm_t& rvm, double fc_hz,
                                                     double nominal_los_range_m)
{
  los_residual_t out;

  if (rvm.nof_range_bins == 0 || rvm.nof_doppler_bins == 0) {
    return out;
  }

  // Reference to match against this CPI: the established baseline once set, otherwise the running
  // init average (or, before any init CPI has succeeded, a physically-motivated initial guess --
  // the same nominal LOS range Phase 1/3 seed from, and zero Doppler since the direct path is
  // static once fully corrected -- NOT an arbitrary universal constant asserted as truth).
  uint32_t guess_range_bin, guess_doppler_bin;
  if (baseline_set_) {
    guess_range_bin   = baseline_range_bin_;
    guess_doppler_bin = baseline_doppler_bin_;
  } else if (init_count_ > 0) {
    guess_range_bin   = (uint32_t)(init_range_bin_sum_ / init_count_);
    guess_doppler_bin = (uint32_t)(init_doppler_bin_sum_ / init_count_);
  } else {
    // Physically-motivated initial guess, not an arbitrary constant: the same nominal LOS range
    // Phase 1/3 seed from (rvm.range_res_m already encodes this CPI's actual bin spacing), and
    // zero Doppler since the direct path is static once fully corrected.
    guess_range_bin   = (uint32_t)std::lround(nominal_los_range_m / (double)rvm.range_res_m);
    guess_doppler_bin = rvm.nof_doppler_bins / 2;
  }

  const sensing_detection_t* best     = nullptr;
  uint32_t                   best_dist = UINT32_MAX;
  for (const auto& d : detections) {
    const uint32_t dr = (d.range_bin > guess_range_bin) ? (d.range_bin - guess_range_bin) : (guess_range_bin - d.range_bin);
    const uint32_t dd = (d.doppler_bin > guess_doppler_bin) ? (d.doppler_bin - guess_doppler_bin)
                                                             : (guess_doppler_bin - d.doppler_bin);
    if (dr > LOS_MATCH_MAX_RANGE_BINS || dd > LOS_MATCH_MAX_DOPPLER_BINS) {
      continue;
    }
    const uint32_t dist = dr + dd;
    if (dist < best_dist) {
      best_dist = dist;
      best      = &d;
    }
  }

  if (best == nullptr) {
    out.baseline_established = baseline_set_;
    return out; // no detection near the (candidate) baseline this CPI -- nothing to measure
  }

  out.detection_found = true;
  out.range_bin        = best->range_bin;
  out.doppler_bin       = best->doppler_bin;

  if (!baseline_set_) {
    // Still accumulating the init average.
    init_count_++;
    init_range_sum_ += best->range_m;
    init_vel_sum_ += best->vel_mps;
    init_range_bin_sum_ += best->range_bin;
    init_doppler_bin_sum_ += best->doppler_bin;

    if (init_count_ >= LOS_BASELINE_INIT_CPIS) {
      baseline_range_m_     = init_range_sum_ / (double)init_count_;
      baseline_vel_mps_     = init_vel_sum_ / (double)init_count_;
      baseline_range_bin_   = (uint32_t)std::lround((double)init_range_bin_sum_ / (double)init_count_);
      baseline_doppler_bin_ = (uint32_t)std::lround((double)init_doppler_bin_sum_ / (double)init_count_);
      baseline_set_         = true;
      LOG_I(PHY,
            "SENSING: sync(LOS baseline) established over %u CPIs: range_bin=%u (%.2f m) doppler_bin=%u "
            "(%.3f m/s)\n",
            init_count_, baseline_range_bin_, baseline_range_m_, baseline_doppler_bin_, baseline_vel_mps_);
    }
    out.baseline_established = baseline_set_;
    // Report against the running average even before finalisation, for Phase 5 convergence logging.
    out.range_residual_m = best->range_m - (init_range_sum_ / (double)init_count_);
    out.vel_residual_mps = best->vel_mps - (init_vel_sum_ / (double)init_count_);
    out.delay_bias_s     = delay_bias_s_;
    out.cfo_bias_hz      = cfo_bias_hz_;
    return out;
  }

  // Baseline established: this is the real residual, and the closed-loop bias update.
  out.baseline_established = true;
  out.range_residual_m     = best->range_m - baseline_range_m_;
  out.vel_residual_mps     = best->vel_mps - baseline_vel_mps_;

  // tau = dR/c (no round-trip factor 2): the range axis reports DIFFERENTIAL BISTATIC range, so
  // inverting it back to a delay must not reintroduce the 2. Matches range_doppler.cc's
  // rvm.range_res_m derivation (2026-07-23 calibration fix).
  const double delay_residual_s = out.range_residual_m / SPEED_OF_LIGHT;
  // f_d = Ṙ/λ (no monostatic factor 2): the velocity axis reports BISTATIC RANGE-RATE, so inverting
  // it back to a frequency must not reintroduce the 2. Kept in sync with rvm.vel_res_mps's own
  // derivation in range_doppler.cc (see the calibration-fix comment there, 2026-07-23).
  const double cfo_residual_hz  = out.vel_residual_mps * fc_hz / SPEED_OF_LIGHT;

  delay_bias_s_ = (1.0 - LOS_BIAS_LEAK) * delay_bias_s_ + LOS_BIAS_KI * delay_residual_s;
  cfo_bias_hz_  = (1.0 - LOS_BIAS_LEAK) * cfo_bias_hz_ + LOS_BIAS_KI * cfo_residual_hz;

  out.delay_bias_s = delay_bias_s_;
  out.cfo_bias_hz  = cfo_bias_hz_;

  LOG_I(PHY,
        "SENSING: sync(LOS residual) range=%u (%.2f m, resid=%.3f m) doppler=%u (resid=%.4f m/s) -> "
        "bias(delay=%.3e s, cfo=%.3f Hz)\n",
        out.range_bin, best->range_m, out.range_residual_m, out.doppler_bin, out.vel_residual_mps, delay_bias_s_,
        cfo_bias_hz_);

  return out;
}

void los_baseline_tracker::apply_bias_correction(icf_t* h_cpi, const uint8_t* occ_all, uint32_t cpi_rows,
                                                 uint32_t nof_subc, const double* row_time_slots,
                                                 const nr_isac_carrier_t& carrier) const
{
  if (!baseline_set_ || (delay_bias_s_ == 0.0 && cfo_bias_hz_ == 0.0)) {
    return; // no established baseline yet, or the integrator hasn't accumulated any correction
  }

  const double slots_per_sf = std::max(1.0, (double)carrier.scs_hz / 15000.0);
  const double slot_dur_s   = 1e-3 / slots_per_sf;

  // Same two correction primitives as Phases 1-3: a linear-in-k frequency ramp cancels the
  // accumulated delay bias (identical sign convention to Phase 1/3's "+j2*pi*k*SCS*tau" cancel
  // formula), and a uniform per-row rotation, scaled by that row's own elapsed time, cancels the
  // accumulated CFO-like bias (identical sign convention to Phase 2's "-observed phase" cancel).
  const double delay_phase_per_subc = 2.0 * M_PI * (double)carrier.scs_hz * delay_bias_s_;

  for (uint32_t r = 0; r < cpi_rows; r++) {
    const double time_s   = row_time_slots[r] * slot_dur_s;
    const double cfo_phase = 2.0 * M_PI * cfo_bias_hz_ * time_s;
    const icf_t  cfo_rot((float)std::cos(-cfo_phase), (float)std::sin(-cfo_phase));

    icf_t*         row_ptr = &h_cpi[(size_t)r * nof_subc];
    const uint8_t* mask    = &occ_all[(size_t)r * nof_subc];
    for (uint32_t c = 0; c < nof_subc; c++) {
      if (!mask[c]) {
        continue;
      }
      const double delay_phase = delay_phase_per_subc * (double)c;
      const icf_t  delay_rot((float)std::cos(delay_phase), (float)std::sin(delay_phase));
      row_ptr[c] = row_ptr[c] * delay_rot * cfo_rot;
    }
  }
}

} // namespace nr_isac
