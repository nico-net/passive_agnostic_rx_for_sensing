/* No-op receiver adapter for ENABLE_ISAC_SENSING=OFF. */

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


uint32_t nr_isac_subslot_config(uint32_t *min_re, float *min_snr_db)
{
  if (min_re)
    *min_re = 0;
  if (min_snr_db)
    *min_snr_db = 0.0f;
  return 0; /* 0 groups = sub-slot sampling disabled */
}
