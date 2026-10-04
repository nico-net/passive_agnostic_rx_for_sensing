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

/* K33: the same rule, evaluated AFTER the decode. A CRC outcome (either way) is Technique D / layout
 * evidence only if the IQ it was computed from was still in the ring when the decode finished; otherwise
 * the outcome is INCONCLUSIVE (no feedback, counted stale_after_decode). Deliberately not a second
 * implementation of the rule. */
static inline bool nr_passive_credit_allowed(long producer_slot_now, long job_slot, long slots_per_frame)
{
  return nr_passive_samples_valid(producer_slot_now, job_slot, slots_per_frame);
}
#endif
