#include "nr_isac_ssb_axis.h"

void nr_isac_ssb_k_abs(int ssb_start_subcarrier, int k_ssb, int ofdm_symbol_size, uint32_t* k_abs_out)
{
  // Same derivation as nr_ue_dci_configuration.c's ssb_offset_point_a (Phase 1): integer division
  // is intentional -- it is RB-aligned, matching how nr_pdcch_channel_estimation and every other
  // MIB-derived RB offset in this codebase is computed.
  const int ssb_offset_point_a = (ssb_start_subcarrier - k_ssb) / 12;
  const int base_sc            = ssb_offset_point_a * 12;
  for (int i = 0; i < 240; i++) {
    // Guard against negative modulo: C's % keeps sign of dividend, so ensure result is always
    // non-negative before casting to uint32_t. This protects against mismatched/un-normalized k_ssb
    // (see nr_isac_ssb_axis.h doc comment on the k_ssb parameter).
    int idx = (base_sc + i) % ofdm_symbol_size;
    if (idx < 0) {
      idx += ofdm_symbol_size;
    }
    k_abs_out[i] = (uint32_t)idx;
  }
}
