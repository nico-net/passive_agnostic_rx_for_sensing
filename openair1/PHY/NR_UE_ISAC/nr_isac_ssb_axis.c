/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#include "nr_isac_ssb_axis.h"

void nr_isac_ssb_k_abs(int ssb_start_subcarrier, int k_ssb, int ofdm_symbol_size,
                       uint32_t *k_abs_out)
{
  if (!k_abs_out || ofdm_symbol_size <= 0)
    return;
  const int base_sc = ((ssb_start_subcarrier - k_ssb) / 12) * 12;
  for (int i = 0; i < 240; ++i) {
    int value = (base_sc + i) % ofdm_symbol_size;
    if (value < 0)
      value += ofdm_symbol_size;
    k_abs_out[i] = (uint32_t)value;
  }
}
