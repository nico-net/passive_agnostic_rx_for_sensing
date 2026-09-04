#include "nr_isac_ssb_axis.h"

void nr_isac_ssb_k_abs(int ssb_start_subcarrier, int k_ssb, int ofdm_symbol_size, uint32_t* k_abs_out)
{
  // Same derivation as nr_ue_dci_configuration.c's ssb_offset_point_a (Phase 1): integer division
  // is intentional -- it is RB-aligned, matching how nr_pdcch_channel_estimation and every other
  // MIB-derived RB offset in this codebase is computed.
  const int ssb_offset_point_a = (ssb_start_subcarrier - k_ssb) / 12;
  const int base_sc            = ssb_offset_point_a * 12;
  for (int i = 0; i < 240; i++) {
    k_abs_out[i] = (uint32_t)((base_sc + i) % ofdm_symbol_size);
  }
}
