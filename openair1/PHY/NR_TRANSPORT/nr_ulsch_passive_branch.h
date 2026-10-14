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

/* DMRS SNR in dB from the mean channel-estimate power per RE (sig2, signal+noise) and the mean LS-vs-smoothed
 * residual per RE (noise2), both in channel-estimate units. NAN when there is no usable estimate. */
static inline float nr_ulsch_dmrs_snr_db(double sig2, double noise2)
{
  if (!(sig2 > 0.0) || !(noise2 > 0.0)) return NAN;
  const double s = sig2 > noise2 ? sig2 - noise2 : 1e-3 * noise2;
  return (float)(10.0 * log10(s / noise2));
}
#endif
