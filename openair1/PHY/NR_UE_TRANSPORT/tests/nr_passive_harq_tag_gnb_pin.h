/* P08a fix round 1: see nr_passive_harq_tag_gnb_pin.c for why this shim exists. */
#ifndef NR_PASSIVE_HARQ_TAG_GNB_PIN_H
#define NR_PASSIVE_HARQ_TAG_GNB_PIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// harq_unique_pid_base as a freshly calloc'd PHY_VARS_gNB carries it. Must be 0, or
/// nr_ulsch_decoding.c:133 stops reproducing the upstream `= ULSCH_id` for a real gNB.
/// Returns UINT32_MAX if the allocation failed.
uint32_t nr_passive_harq_tag_gnb_zero_default(void);

/// sizeof(PHY_VARS_gNB), so the test can assert it actually allocated the real struct.
size_t nr_passive_harq_tag_gnb_size(void);

#ifdef __cplusplus
}
#endif

#endif /* NR_PASSIVE_HARQ_TAG_GNB_PIN_H */
