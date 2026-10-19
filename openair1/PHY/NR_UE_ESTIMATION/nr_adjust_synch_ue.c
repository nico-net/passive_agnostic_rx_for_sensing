/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#include "PHY/defs_nr_UE.h"
#include "PHY/NR_UE_ESTIMATION/nr_estimation.h"
#include "executables/nr-uesoftmodem.h"
#include <stdlib.h>
#include <time.h>
#include <stdatomic.h>

/* Anti-windup bound on the timing PI loop's integral term (a healthy receiver holds max_pos_acc ~450). */
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
  }

  const int corr_pos = max_pos;

  /* GATED GLOBAL REBASE (2026-09-16). Measured on the X410 at 4 RX: the acquisition placed the FFT
   * window 342 samples EARLY on every miss (TSYNC global_pos=342, global_val 25x the in-window peak,
   * e_win_frac 0.06 on all 60 observations; the 4-ch SIB1 hits had e_win_frac 0.99). The +-CP search
   * above is structurally blind to it, and always chasing the global peak was measured to kill
   * healthy locks. So: only when the global peak is out of window AND dominant, for
   * NR_TSYNC_GLOBAL_N consecutive observations, request ONE deferred rebase by that offset through
   * the same frame-boundary path the ANCHOR uses (positive = channel later than the window, discard
   * that many samples). Never fires on a healthy lock (peak in window). */
  {
    extern _Atomic long nr_ue_pending_rebase_delta;
    extern _Atomic int nr_ue_pending_rebase_valid;
    /* LATE WINDOW TOO (2026-09-19). Macro PCI 64 at 217 PRB: global_pos -341 (window LATE, the mirror
     * of the 4-ch +342 case), global/in-window ratio only 4.8-6.5x, e_win_frac 0.14-0.17 on every
     * observation, SIB1 0. Both `g_pos > 0` and the hard 8x blocked it. Negative deltas are now
     * applied by the consumer as a one-frame-minus-|d| discard. */
    const int s_gr_ratio = 8;
    static int s_gr_cnt = 0;
    const bool dominant = peak_out_of_window && g_pos != 0 && g_val > s_gr_ratio * (int64_t)(max_val > 0 ? max_val : 1);
    s_gr_cnt = dominant ? s_gr_cnt + 1 : 0;
    if (s_gr_cnt >= 4 && !atomic_load_explicit(&nr_ue_pending_rebase_valid, memory_order_relaxed)) {
      atomic_store_explicit(&nr_ue_pending_rebase_delta, (long)g_pos, memory_order_relaxed);
      atomic_store_explicit(&nr_ue_pending_rebase_valid, 1, memory_order_relaxed);
      LOG_W(PHY, "SENSING: TSYNC_GLOBAL_REBASE requesting deferred rebase of %+d samples (global %d vs in-window %d, e_win_frac %.3f)\n",
            g_pos, g_val, max_val, e_win_frac);
      s_gr_cnt = 0;
    }
  }

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
  const int acc_limit = NR_MAX_POS_ACC_LIMIT;
  if (ue->max_pos_acc > acc_limit) {
    ue->max_pos_acc = acc_limit;
  } else if (ue->max_pos_acc < -acc_limit) {
    ue->max_pos_acc = -acc_limit;
  }

  return sample_shift;
}
