/*
 * P08a fix round 1: a C shim so the gtest can pin the one property the tag arithmetic cannot
 * express -- that a PHY_VARS_gNB's harq_unique_pid_base defaults to 0, which is the whole reason
 * nr_ulsch_decoding.c:133 is upstream-neutral.
 *
 * Why a shim and not just #include "PHY/defs_gNB.h" in the .cc: defs_gNB.h pulls
 * common/utils/threadPool/thread-pool.h, whose `_Atomic(uint64_t) dead_mask` is C11 syntax that
 * does not compile as C++. The struct is therefore only reachable from a C translation unit. This
 * file needs no link dependencies -- it uses the type definition, calloc and free, nothing else.
 */

#include <stdlib.h>

#include "PHY/defs_gNB.h"

#include "nr_passive_harq_tag_gnb_pin.h"

uint32_t nr_passive_harq_tag_gnb_zero_default(void)
{
  PHY_VARS_gNB *g = (PHY_VARS_gNB *)calloc(1, sizeof(*g));
  if (!g) {
    return UINT32_MAX; /* distinguishable from a real 0; the caller asserts on it */
  }
  const uint32_t base = g->harq_unique_pid_base;
  free(g);
  return base;
}

size_t nr_passive_harq_tag_gnb_size(void)
{
  return sizeof(PHY_VARS_gNB);
}
