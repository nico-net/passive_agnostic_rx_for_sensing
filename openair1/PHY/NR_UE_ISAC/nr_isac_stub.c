/*
 * nr_isac_stub.c -- no-op implementation of the NR_UE_ISAC API.
 *
 * WHY THIS EXISTS
 * ---------------
 * The PASSIVE RECEIVER (blind PDCCH -> DCI -> PDSCH decode) and the ISAC SENSING PIPELINE
 * (CFR accumulation, CPI, range-Doppler, CFAR, tracking, AoA) are separate concerns that happened
 * to share one build. The receiver only ever calls into sensing to HAND OVER channel estimates; it
 * never needs anything back. So on a passive-receiver-only build the sensing library can be replaced
 * wholesale by these no-ops, and nothing in the receive path changes.
 *
 * This is not cosmetic. MEASURED on the X410 (120 s, identical binary, only the [sensing] section
 * differing): running the sensing pipeline alongside the receiver cost 5.4x the recovered DCIs
 * (14659 -> 78616 with it off), because the CSI-RS monitor FEP and CFR submission compete with the
 * blind monitor on the PHY receive thread. CLAUDE.md section 12 had already flagged that the passive
 * decode runs on that thread and "would be the first thing to break on real hardware".
 *
 * Selected by -DENABLE_ISAC_SENSING=OFF (the default on the passive-rx-only branch). Build with
 * -DENABLE_ISAC_SENSING=ON to link the real NR_UE_ISAC library instead; the receiver is identical
 * either way, and nr_isac_enabled() then reports whatever the [sensing] config says.
 *
 * NOTE: nr_isac_submit_csirs_ls() and nr_isac_framescan() are deliberately NOT stubbed here -- they
 * are defined in csi_rx.c and nr_pbch.c respectively, i.e. they live in the receive path, not in the
 * sensing library.
 */

#include "PHY/NR_UE_ISAC/nr_isac.h"

void nr_isac_init(void)
{
}

void nr_isac_start(void)
{
}

void nr_isac_stop(void)
{
}

/* The single gate every sensing-only path keys off. Reporting 0 makes each of them compile to a
 * predictable branch that is never taken. */
int nr_isac_enabled(void)
{
  return 0;
}

int nr_isac_source(void)
{
  return 0;
}

int nr_isac_source_enabled(int source)
{
  (void)source;
  return 0;
}

void nr_isac_submit_cfr(uint32_t slot_idx,
                        int source,
                        const nr_isac_carrier_t *carrier,
                        const float *h,
                        const uint32_t *k_abs,
                        const uint32_t *l_sym,
                        uint32_t nof_re,
                        float noise_var)
{
  (void)slot_idx; (void)source; (void)carrier; (void)h;
  (void)k_abs; (void)l_sym; (void)nof_re; (void)noise_var;
}

void nr_isac_submit_cfr_at(uint32_t slot_idx,
                           float slot_frac,
                           int source,
                           const nr_isac_carrier_t *carrier,
                           const float *h,
                           const uint32_t *k_abs,
                           const uint32_t *l_sym,
                           uint32_t nof_re,
                           float noise_var)
{
  (void)slot_idx; (void)slot_frac; (void)source; (void)carrier; (void)h;
  (void)k_abs; (void)l_sym; (void)nof_re; (void)noise_var;
}

void nr_isac_submit_cfr_multi(uint32_t slot_idx,
                              float slot_frac,
                              int source,
                              const nr_isac_carrier_t *carrier,
                              const float *h,
                              uint32_t nof_ant,
                              uint32_t ant_stride_re,
                              const uint32_t *k_abs,
                              const uint32_t *l_sym,
                              uint32_t nof_re,
                              float noise_var)
{
  (void)slot_idx; (void)slot_frac; (void)source; (void)carrier; (void)h;
  (void)nof_ant; (void)ant_stride_re; (void)k_abs; (void)l_sym;
  (void)nof_re; (void)noise_var;
}

/* 0 = "no receive array configured", which is what the AoA-capable taps test before doing any
 * per-antenna work. */
uint32_t nr_isac_aoa_antennas(void)
{
  return 0;
}

uint32_t nr_isac_subslot_config(uint32_t *min_re, float *min_snr_db)
{
  if (min_re)
    *min_re = 0;
  if (min_snr_db)
    *min_snr_db = 0.0f;
  return 0; /* 0 groups = sub-slot sampling disabled */
}
