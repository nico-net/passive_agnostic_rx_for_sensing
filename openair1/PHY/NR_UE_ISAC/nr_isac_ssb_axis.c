#include "nr_isac_ssb_axis.h"
int nr_isac_ssb_normalize_k_ssb(int raw_k_ssb, int numerology, int fr1)
{
  return fr1 && raw_k_ssb < 24 ? raw_k_ssb >> numerology : raw_k_ssb;
}

void nr_isac_ssb_k_abs(int ssb_start_subcarrier, int k_ssb, int ofdm_symbol_size, uint32_t* k_abs_out)
{
  /* The estimator starts at the first SSB RE, not at the containing CRB boundary.
   * fp->ssb_start_subcarrier is already relative to Point A. FFT wrapping applies
   * only when reading rxdataF (first_carrier_offset + k), never to this axis. */
  (void)k_ssb;
  (void)ofdm_symbol_size;
  for (int i = 0; i < 240; i++) k_abs_out[i] = ssb_start_subcarrier + i;
}
