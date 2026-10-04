/* Receiver-side CFR decisions shared by the blind PDSCH path and its offline test. */
#ifndef NR_SENSE_GATING_H
#define NR_SENSE_GATING_H
#include "PHY/NR_UE_ISAC/nr_isac.h"

/* The two legacy tokens select separate API bits. Both describe the same
 * blind receiver DM-RS tap, so choose the enabled bit for its submission. */
static inline int nr_sense_blind_dmrs_source(void)
{
  if (nr_isac_source_enabled(NR_ISAC_SRC_PDSCH_DMRS_BLIND))
    return NR_ISAC_SRC_PDSCH_DMRS_BLIND;
  if (nr_isac_source_enabled(NR_ISAC_SRC_PDSCH_DMRS))
    return NR_ISAC_SRC_PDSCH_DMRS;
  return -1;
}

static inline int nr_sense_blind_need_chest(int dmrs_source, float min_snr_lin)
{
  return dmrs_source >= 0 || min_snr_lin > 0.0f;
}

static inline int nr_sense_blind_skip_capped(int dmrs_source, int want_decode, int capped)
{
  return dmrs_source < 0 && want_decode && capped;
}
#endif
