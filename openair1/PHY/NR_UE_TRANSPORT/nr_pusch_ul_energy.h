/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_PUSCH_UL_ENERGY_H
#define NR_PUSCH_UL_ENERGY_H
#include "common/platform_types.h"

/* Mean energy per PRB for a BWP-relative contiguous allocation in one FFT symbol. */
static inline double nr_pusch_ul_allocation_energy(const c16_t *row, int fft, int first_carrier,
                                                    int carrier_rb, int bwp_start, int start_rb, int n_rb)
{
  const int first = bwp_start + start_rb;
  if (!row || fft <= 0 || first_carrier < 0 || first_carrier >= fft || first < 0 || n_rb <= 0
      || first + n_rb > carrier_rb)
    return 0;
  double energy = 0;
  for (int p = first; p < first + n_rb; p++)
    for (int r = 0; r < 12; r++) {
      const c16_t y = row[(first_carrier + p * 12 + r) % fft];
      energy += (double)y.r * y.r + (double)y.i * y.i;
    }
  return energy / n_rb;
}
#endif
