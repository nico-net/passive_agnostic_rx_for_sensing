/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#include "PHY/defs_nr_UE.h"
#include "PHY/NR_UE_ESTIMATION/nr_estimation.h"
#include "executables/nr-uesoftmodem.h"
#include <stdlib.h>
#include <time.h>
#include <stdatomic.h>

/// Anti-windup bound on the timing PI loop's integral term -- see where it is applied. A healthy
/// receiver holds max_pos_acc ~450 flat; a runaway reached -2141 and cost the capture.
#define NR_MAX_POS_ACC_LIMIT 1024

/* Set by the RF census in nr-ue.c while the receive stream is delivering noise floor on every
 * branch. Read here to FREEZE the integral term: see where it is used. */
_Atomic int nr_ue_rf_signal_absent = 0;

void nr_ue_reset_time_sync_loop(PHY_VARS_NR_UE *ue)
{
  if (ue == NULL) {
    return;
  }
  ue->max_pos_acc = 0;
  ue->max_pos_iir = 0;
}

//#define DEBUG_PHY

// Adjust location synchronization point to account for drift
// The adjustment is performed once per frame based on the
// last channel estimate of the receiver

int nr_adjust_synch_ue(const NR_DL_FRAME_PARMS *frame_parms,
                       PHY_VARS_NR_UE *ue,
                       const c16_t dl_ch_estimates_time[][frame_parms->ofdm_symbol_size],
                       uint8_t frame,
                       uint8_t slot,
                       short coef)
{
  int max_val = 0, max_pos = 0;

  // search for maximum position within the cyclic prefix
  for (int i = -frame_parms->nb_prefix_samples; i < frame_parms->nb_prefix_samples; i++) {
    int temp = 0;

    int j = (i < 0) ? (i + frame_parms->ofdm_symbol_size) : i;
    for (int aa = 0; aa < frame_parms->nb_antennas_rx; aa++) {
      int Re = dl_ch_estimates_time[aa][j].r;
      int Im = dl_ch_estimates_time[aa][j].i;
      temp += (Re*Re/2) + (Im*Im/2);
    }

    if (temp > max_val) {
      max_pos = i;
      max_val = temp;
    }
  }

  // TIME-TRACKING AUDIT (2026-08-04, instrumentation only -- no behavioural change).
  // The search above is deliberately confined to +-nb_prefix_samples. If the true channel peak lies
  // OUTSIDE that window the loop cannot see it and locks onto whatever is inside, so we additionally
  // locate the peak over the FULL symbol (read-only) and report both. Enabled by ISAC_TSYNC_AUDIT=1.
  /* ISAC_TSYNC_AUDIT is a DECIMATION FACTOR, not a boolean: "1" traces every SSB, "10" every
   * tenth. Unset = off.
   *
   * It has to be decimated because the trace perturbs what it measures. At one LOG_I per SSB --
   * 50 lines/s on the PHY receive thread, which already misses ~96 % of its slot deadlines -- both
   * traced runs came out 100 % "signal present, PBCH dead" with not one healthy window, against
   * runs of 164 healthy windows without it. That is the observer effect, not a finding, and a
   * diagnostic that induces the fault it is investigating is worse than none. */
  static int audit = -1;
  if (audit < 0) {
    const char *e = getenv("ISAC_TSYNC_AUDIT");
    audit = (e != NULL) ? atoi(e) : 0;
    if (e != NULL && audit < 1) {
      audit = 1;
    }
  }
  static unsigned audit_n = 0;

  // CAUSAL TEST (2026-08-04b): does actually CORRECTING with the full-symbol peak (instead of the
  // blind +-CP one) make PBCH decode succeed? This is the experiment that can directly confirm or
  // kill the timing-offset theory rather than just observe it. Gated on ISAC_FORCE_GLOBAL_SYNC=1 so
  // the unmodified path is bit-identical unless explicitly requested.
  static int force_global = -1;
  if (force_global < 0)
    force_global = (getenv("ISAC_FORCE_GLOBAL_SYNC") != NULL) ? 1 : 0;

  // ---- DMRS TIMING-MEASUREMENT RELIABILITY (2026-08-06) ------------------------------------
  // A CRC protects DECODED BITS; it has no bearing on whether a timing estimate derived from the
  // DMRS channel impulse response is trustworthy. Gating this loop on PBCH CRC (the caller's old
  // behaviour) means one failed payload stops timing recovery, which is exactly backwards: timing
  // is what recovery needs most when decoding is marginal. So the reliability of the MEASUREMENT
  // is judged here, from the CIR itself, and the caller no longer needs a CRC to call us.
  //
  // Metric: fraction of total CIR energy lying within +-CP of the peak (e_win/e_tot), plus whether
  // the full-symbol peak falls inside that window at all. A clean, well-positioned channel puts
  // most of its energy in the window; a decorrelated or badly-misplaced one does not. Both
  // quantities were already being computed here, but only under the audit flag -- they are now
  // computed ALWAYS (one pass over ofdm_symbol_size, negligible next to the FFT that produced the
  // estimate) so they can gate the update.
  int g_val = 0, g_pos = 0;
  double e_win_frac = 0.0;
  bool peak_out_of_window = false;
  {
    int64_t e_tot = 0, e_win = 0;
    for (int i = 0; i < frame_parms->ofdm_symbol_size; i++) {
      int temp = 0;
      for (int aa = 0; aa < frame_parms->nb_antennas_rx; aa++) {
        int Re = dl_ch_estimates_time[aa][i].r;
        int Im = dl_ch_estimates_time[aa][i].i;
        temp += (Re * Re / 2) + (Im * Im / 2);
      }
      e_tot += temp;
      if (temp > g_val) {
        g_val = temp;
        g_pos = (i > frame_parms->ofdm_symbol_size / 2) ? (i - frame_parms->ofdm_symbol_size) : i;
      }
      int s = (i > frame_parms->ofdm_symbol_size / 2) ? (i - frame_parms->ofdm_symbol_size) : i;
      if (s >= -frame_parms->nb_prefix_samples && s < frame_parms->nb_prefix_samples)
        e_win += temp;
    }
    e_win_frac = e_tot ? (double)e_win / (double)e_tot : 0.0;
    peak_out_of_window = (g_pos < -frame_parms->nb_prefix_samples || g_pos >= frame_parms->nb_prefix_samples);
    if (audit && (audit_n % (unsigned)audit) == 0) {
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      // peak_out_of_window is the load-bearing field: 1 means the tracking loop is structurally blind
      LOG_I(PHY,
            "TSYNC utc_ns=%lld frame=%d slot=%d max_pos=%d max_val=%d global_pos=%d global_val=%d "
            "peak_out_of_window=%d cp=%d e_win_frac=%.3f iir=%lld acc=%d force_global=%d\n",
            (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec,
            frame,
            slot,
            max_pos,
            max_val,
            g_pos,
            g_val,
            (g_pos < -frame_parms->nb_prefix_samples || g_pos >= frame_parms->nb_prefix_samples) ? 1 : 0,
            frame_parms->nb_prefix_samples,
            e_tot ? (double)e_win / (double)e_tot : 0.0,
            (long long)ue->max_pos_iir,
            ue->max_pos_acc,
            force_global);
    }
  }

  // CAUSAL TEST: use the full-symbol peak as the correction input instead of the +-CP one, when
  // requested. corr_pos feeds the SAME downstream filter/PI-loop max_pos always did -- only the
  // INPUT changes, not the control law, so this isolates "was the search window too narrow" from
  // "is the filter/PI loop itself wrong".
  const int corr_pos = force_global ? g_pos : max_pos;

  // ---- RELIABILITY GATE + MEASURE-ONLY MODE (2026-08-06) -----------------------------------
  // Two independent controls, both defaulting to the previous behaviour:
  //
  //   ISAC_TSYNC_MIN_EWIN=<frac>  reject this measurement if the CIR is too diffuse or its peak
  //                               sits outside the +-CP window. Default 0 = no gate (unchanged).
  //   ISAC_TSYNC_MEASURE=1        MEASUREMENT ONLY: log what the loop WOULD do and return 0
  //                               without touching max_pos_iir / max_pos_acc or shifting anything.
  //
  // The measure-only mode exists so loop gains are chosen from observed timing-error statistics
  // (per-occasion measurement, its variance, and the residual after a feed-forward drift estimate)
  // rather than picked arbitrarily -- the mistake made by the earlier time_sync_I = 0.01 guess.
  // It is the open-loop characterisation step that must precede designing the closed loop.
  static double min_ewin = -1.0;
  if (min_ewin < 0.0) {
    const char *e = getenv("ISAC_TSYNC_MIN_EWIN");
    min_ewin = e ? atof(e) : 0.0;
  }
  static int measure_only = -1;
  if (measure_only < 0)
    measure_only = (getenv("ISAC_TSYNC_MEASURE") && atoi(getenv("ISAC_TSYNC_MEASURE"))) ? 1 : 0;

  const bool reliable = (min_ewin <= 0.0) || (e_win_frac >= min_ewin && !peak_out_of_window);

  // Open-loop drift observation: successive corr_pos values against elapsed frames give the
  // sample-clock drift DIRECTLY, in samples/frame, from this stream -- no CFO inference. Tracked
  // here (not applied) so the estimate can be compared against the CFO-derived prior.
  {
    static int prev_frame = -1;
    static int prev_pos = 0;
    static bool have_prev = false;
    static double drift_ema = 0.0;
    static int n_obs = 0;
    if (have_prev && reliable) {
      int df = (int)frame - prev_frame;
      if (df < 0)
        df += MAX_FRAME_NUMBER;
      if (df > 0 && df <= 64) { // ignore wraps/gaps; SSB period here is 2 frames
        const double d_per_frame = (double)(corr_pos - prev_pos) / (double)df;
        n_obs++;
        drift_ema = (n_obs == 1) ? d_per_frame : (0.9 * drift_ema + 0.1 * d_per_frame);
        static int obs_log_left = 60;
        if (obs_log_left > 0) {
          obs_log_left--;
          LOG_W(PHY,
                "SENSING: TSYNC_OBS frame=%d slot=%d corr_pos=%d prev_pos=%d d_frames=%d "
                "d_per_frame=%+.3f drift_ema=%+.3f n_obs=%d e_win_frac=%.3f peak_oow=%d reliable=%d "
                "max_val=%d measure_only=%d\n",
                frame, slot, corr_pos, prev_pos, df, d_per_frame, drift_ema, n_obs,
                e_win_frac, peak_out_of_window ? 1 : 0, reliable ? 1 : 0, max_val, measure_only);
        }
      }
    }
    if (reliable) {
      prev_frame = (int)frame;
      prev_pos = corr_pos;
      have_prev = true;
    }
  }

  if (!reliable || measure_only)
    return 0; // no state mutation, no correction applied

  // filter position to reduce jitter
  const int ncoef = 32767 - coef;
  ue->max_pos_iir = ((ue->max_pos_iir * coef) >> 15) + (corr_pos * ncoef);
  const int diff = (ue->max_pos_iir + 16384) >> 15;

  // FIXME: Do we really need this hysteresis for FR2?
  int sampleShift = diff;
  if (frame_parms->freq_range == FR2)
    if (abs(diff) <= 2)
      sampleShift = 0;

  // PI controller
  const double PID_P = get_nrUE_params()->time_sync_P;
  const double PID_I = get_nrUE_params()->time_sync_I;
  int sample_shift = -round(sampleShift * PID_P + ue->max_pos_acc * PID_I);

  LOG_D(PHY,
        "Frame %d, Slot %d: max_pos = %d, max_pos filtered = %f, diff = %i, sampleShift = %i, max_pos_acc = %d, sample_shift (final) = %d, max_power = %d\n",
        frame,
        slot,
        corr_pos,
        ue->max_pos_iir / 32768.0,
        diff,
        sampleShift,
        ue->max_pos_acc,
        sample_shift,
        max_val);

  /* PER-SSB TRACE. The RF census aggregates 2000 slots, which is why mode-B failures (signal
   * present, pbch_ok=0) could not be told apart from healthy windows on ANY census variable -- the
   * aggregate hides whatever differs SSB to SSB. These are the quantities the timing decision is
   * actually made from, emitted once per SSB so a failed PBCH can be paired with the correlation
   * that produced its window: raw peak position, peak POWER (the thing a census mean destroys),
   * the filtered/hysteresis output, and the integrator state before it is updated. */
  if (audit && (audit_n % (unsigned)audit) == 0) {
    LOG_I(PHY,
          "TSYNC_OUT frame=%d slot=%d corr_pos=%d max_power=%d diff=%d sampleShift=%d "
          "sample_shift=%d mpa=%d\n",
          frame, slot, corr_pos, max_val, diff, sampleShift, sample_shift, ue->max_pos_acc);
  }
  audit_n++;

  // reset IIR filter for next offset calculation
  ue->max_pos_iir += -round(sampleShift * PID_P) * 32768;
  /* Do not integrate a measurement that does not exist. When the X410 stream goes quiet -- all
   * four branches at the noise floor for seconds at a time, measured recovering on its own -- the
   * correlation above is computed on noise, and integrating it is precisely the windup this
   * function used to produce. Freezing holds the loop at the last good correction so it resumes
   * where it left off when the signal returns, which is the right behaviour for an outage on a
   * link whose true drift is ~0.14 samples/frame. The clamp below stays as the backstop for
   * anything this flag does not catch. */
  if (!atomic_load_explicit(&nr_ue_rf_signal_absent, memory_order_relaxed)) {
    ue->max_pos_acc += corr_pos;
  }

  /* ANTI-WINDUP. max_pos_acc is the integral term and was unbounded: it is advanced by corr_pos on
   * EVERY call, including calls where the correlation ran on noise because the SSB was not being
   * decoded. That is textbook integrator windup, and it is what ends passive captures.
   *
   * Measured 2026-09-01 on sens6, from the RFCENSUS trail into an RFSTALL:
   *     pbch_ok=6  fail=44   max_pos_acc= 225   shiftForNextFrame= -2
   *     pbch_ok=0  fail=50   max_pos_acc= 329   shiftForNextFrame= -3    <- PBCH dead,
   *     pbch_ok=0  fail=50   max_pos_acc= 439   shiftForNextFrame= -4       integrator still winding
   *     pbch_ok=7  fail=43   max_pos_acc=-2141  shiftForNextFrame=+21    <- discharge
   * against a healthy receiver that sits flat at max_pos_acc ~450 / shift -5 indefinitely. The +21
   * sample jump moves the FFT window far enough to lose the lock, the next correlation is worse,
   * and the loop diverges -- "RFSTALL PBCH lock lost (timing runaway)".
   *
   * Two hypotheses for that stall were tested against this same data and REFUTED, so do not revisit
   * them: offered load (anti-correlated -- 387 UL grants/s ran clean for 187 s while 24 grants/s
   * died at 13 s) and RF front-end overload (forced with gain to rf_pow 233-249, dead centre of the
   * failing range, 0 stalls).
   *
   * The bound is on the INTEGRAL, not on the output, which is what makes it anti-windup rather than
   * mere output saturation: a saturated output with a still-winding integrator takes just as long to
   * unwind. Sized from the loop's own behaviour -- a healthy receiver holds ~450, so 1024 leaves
   * >2x headroom for a genuine standing offset while capping the correction this term can ever
   * demand at 1024*time_sync_I ~ 10 samples/frame. Real clock drift needs a tiny fraction of that:
   * the measured SFO on this pair is 0.11 ppm, i.e. ~0.14 samples/frame. */
  if (ue->max_pos_acc > NR_MAX_POS_ACC_LIMIT) {
    ue->max_pos_acc = NR_MAX_POS_ACC_LIMIT;
  } else if (ue->max_pos_acc < -NR_MAX_POS_ACC_LIMIT) {
    ue->max_pos_acc = -NR_MAX_POS_ACC_LIMIT;
  }

  return sample_shift;
}
