#ifndef NR_PASSIVE_SAMPLE_LIFETIME_H
#define NR_PASSIVE_SAMPLE_LIFETIME_H
#include <stdbool.h>

/* The producer publishes before writing. Keep two slots clear, including the
 * prefetched first symbol. Check both before AND after copying/transforming IQ;
 * an expired window must never contribute a negative CRC trial. */
static inline bool nr_passive_samples_valid(long producer, long source, long slots_per_frame)
{
  return source >= 0 && producer >= source && slots_per_frame > 2
      && producer - source < slots_per_frame - 2;
}
#endif
