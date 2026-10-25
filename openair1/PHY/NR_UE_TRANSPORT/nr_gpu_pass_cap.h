#pragma once
#include <stddef.h>
/* T6: the memory cap (MB) of the pass's split graphs. min(a quarter of the device memory clamped to 2048..3072, 30 % of the memory free when the platform was probed), floor 256.
 * GB10 (128 GB unified): 3072, unchanged. 8 GB card: 2048. 4 GB card (3500 MB free): 1050. total / free_at_probe in bytes; free_at_probe == 0 (unknown) -> the first term alone. Pure function. */
static inline int nr_gpu_pass_cap_mb(size_t total, size_t free_at_probe)
{
  const long q = (long)(total >> 20) / 4;
  long cap = q < 2048 ? 2048 : q > 3072 ? 3072 : q;
  if (free_at_probe) {
    const long f = (long)(free_at_probe >> 20) * 3 / 10;
    if (f < cap) cap = f;
  }
  return cap < 256 ? 256 : (int)cap;
}
