/* Passive-only antenna policy shared by the UL combiner and its fixed-point
 * scale. Mode 2 means all available RX antennas, as it does for DL. */
#ifndef NR_ULSCH_PASSIVE_BRANCH_H
#define NR_ULSCH_PASSIVE_BRANCH_H
#include <stdbool.h>
#include <stdlib.h>
#include <math.h>

static inline int nr_ulsch_passive_branch_selection(bool passive, int antennas,
                                                    const char *override, const char *mrc_mode)
{
  if (!passive || antennas<=1) return -1;
  const int keep=override ? atoi(override) : (mrc_mode && atoi(mrc_mode)==2 ? -1 : 0);
  return keep>=0 && keep<antennas ? keep : -1;
}

/* DMRS SNR in dB per RX antenna. sig2 = mean |H_smoothed|^2 per RE, resid2 = mean |LS - H_smoothed|^2 per RE
 * (nvar), both in channel-estimate units. With white noise sigma^2 on the LS and a smoother of noise gain g:
 *   resid2 = (1-g) sigma^2,  sig2 = |H|^2 + g sigma^2  =>  SNR = |H|^2 / sigma^2.
 * shortcut: g = 1/8 fixed (type-1 DMRS frequency-interpolation filter, ~8 overlapping pilots), edge PRBs and
 * other chest paths differ; replace by the real filter gain if an absolute calibration is needed.
 * NAN when there is no usable estimate; floor -30 dB when the signal estimate is <= 0. */
#define NR_ULSCH_SMOOTH_G 0.125
static inline float nr_ulsch_dmrs_snr_db(double sig2, double resid2)
{
  if (!(sig2 > 0.0) || !(resid2 > 0.0)) return NAN;
  const double sigma2 = resid2 / (1.0 - NR_ULSCH_SMOOTH_G);
  const double h2 = sig2 - NR_ULSCH_SMOOTH_G * sigma2;
  return (float)(10.0 * log10((h2 > 1e-3 * sigma2 ? h2 : 1e-3 * sigma2) / sigma2));
}
#endif
