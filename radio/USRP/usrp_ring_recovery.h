/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef USRP_RING_RECOVERY_H
#define USRP_RING_RECOVERY_H
#include <stdint.h>
#include <stddef.h>
#include <algorithm>

/* A frozen X410 RX timestamp can still name the previous stream start. The
 * accepted timed START is a lower bound for the first sample of the new one. */
static inline int64_t usrp_ring_epoch_first_ticks(int64_t metadata_ticks, int64_t scheduled_ticks)
{
  return metadata_ticks > scheduled_ticks ? metadata_ticks : scheduled_ticks;
}

static inline bool usrp_ring_has_samples(uint64_t written, uint64_t read, uint64_t wanted)
{
  return written >= read && written - read >= wanted;
}

/* Pure index/retry accounting shared by the UHD producer and fault-injection test.
 * A fault never advances write_index; a successful first packet publishes one epoch. */
struct usrp_ring_recovery_state {
  uint64_t write_index = 0;
  uint64_t boundary = 0;
  uint64_t epoch = 0;
  uint64_t good_samples = 0;
  uint64_t sample_rate;
  int failures = 0;
  int limit;

  usrp_ring_recovery_state(uint64_t rate, int max_failures) : sample_rate(rate), limit(max_failures) {}
  bool fault()
  {
    boundary = write_index;
    good_samples = 0;
    return ++failures <= limit;
  }
  void first_packet() { ++epoch; }
  void samples(size_t count)
  {
    write_index += count;
    good_samples += count;
    /* A start that delivered 20 ms of samples succeeded. Count only a burst
     * of failed starts toward the fatal budget; do not spend it over several
     * short, valid streams during a persistent packet-loss episode. */
    if (good_samples >= std::max<uint64_t>(1, sample_rate / 50)) {
      failures = 0;
      good_samples = 0;
    }
  }
};
#endif
