/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#include "PHY/defs_nr_UE.h"
#include "PHY/NR_UE_ESTIMATION/nr_estimation.h"
#include "executables/nr-uesoftmodem.h"
#include <stdlib.h>
#include <time.h>

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
  static int audit = -1;
  if (audit < 0)
    audit = (getenv("ISAC_TSYNC_AUDIT") != NULL) ? 1 : 0;

  // CAUSAL TEST (2026-08-04b): does actually CORRECTING with the full-symbol peak (instead of the
  // blind +-CP one) make PBCH decode succeed? This is the experiment that can directly confirm or
  // kill the timing-offset theory rather than just observe it. Gated on ISAC_FORCE_GLOBAL_SYNC=1 so
  // the unmodified path is bit-identical unless explicitly requested.
  static int force_global = -1;
  if (force_global < 0)
    force_global = (getenv("ISAC_FORCE_GLOBAL_SYNC") != NULL) ? 1 : 0;

  int g_val = 0, g_pos = 0;
  if (audit || force_global) {
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
    if (audit) {
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

  if (audit)
    LOG_I(PHY, "TSYNC_OUT frame=%d slot=%d diff=%d sampleShift=%d sample_shift=%d\n",
          frame, slot, diff, sampleShift, sample_shift);

  // reset IIR filter for next offset calculation
  ue->max_pos_iir += -round(sampleShift * PID_P) * 32768;
  ue->max_pos_acc += corr_pos;

  return sample_shift;
}
