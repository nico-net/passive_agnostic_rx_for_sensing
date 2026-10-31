/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_SENSE_NOISE_EST_H
#define NR_SENSE_NOISE_EST_H
#include <math.h>
#include <stdint.h>

/* Noise variance of a data-aided channel estimate H = Y/X (E|noise|^2 of one RE), measured from H itself.
 * Where three consecutive subcarriers of one symbol (k, k+1, k+2) are present, the second difference
 * H[i-1] - 2 H[i] + H[i+1] removes a constant and a linear (residual-delay) channel term, and has variance 6 sigma^2 for
 * white noise of variance sigma^2. The estimate is the mean over all triples and antennas; a channel that bends over three
 * subcarriers (or a wrong X) only raises it, so it is an upper bound. NAN when no triple exists.
 * h: interleaved re,im per RE, antenna a at h + 2*a*ant_stride_re; k/l: subcarrier and symbol of each RE. */
static inline float nr_sense_h_noise_var(const float *h, const uint32_t *k, const uint32_t *l, uint32_t n_re,
                                         uint32_t ant_stride_re, uint32_t n_ant)
{
  double acc = 0.0;
  uint64_t cnt = 0;
  for (uint32_t i = 1; i + 1 < n_re; i++) {
    if (l[i - 1] != l[i] || l[i + 1] != l[i] || k[i - 1] + 1 != k[i] || k[i] + 1 != k[i + 1]) continue;
    for (uint32_t a = 0; a < n_ant; a++) {
      const float *p = h + 2 * ((size_t)a * ant_stride_re + i);
      const double dr = (double)p[-2] - 2.0 * p[0] + p[2], di = (double)p[-1] - 2.0 * p[1] + p[3];
      acc += dr * dr + di * di;
      cnt++;
    }
  }
  return cnt ? (float)(acc / (6.0 * (double)cnt)) : NAN;
}
#endif
