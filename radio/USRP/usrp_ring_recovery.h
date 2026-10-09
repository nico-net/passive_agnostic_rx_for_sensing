/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef USRP_RING_RECOVERY_H
#define USRP_RING_RECOVERY_H
#include <stdint.h>
#include <stddef.h>
#include <algorithm>
#include <deque>

/* A frozen X410 RX timestamp can still name the previous stream start. The
 * accepted timed START is a lower bound for the first sample of the new one. */
static inline int64_t usrp_ring_epoch_first_ticks(int64_t metadata_ticks, int64_t scheduled_ticks)
{
  return metadata_ticks > scheduled_ticks ? metadata_ticks : scheduled_ticks;
}

/* Re-arm back-off before a START. Grows with consecutive failures (base, 2*base, 4*base, ... up to
 * 16*base) and is bounded by max_ms. While acquisition has not synced and this is not the first
 * unsynced fault, it is at least 4*base so a mixed-acquisition fault burst cannot loop rapidly. */
static inline int usrp_ring_backoff_ms(int base_ms, int failures, int unsynced_faults, int max_ms)
{
  if (base_ms <= 0)
    return 0;
  const int shift = std::min(std::max(failures - 1, 0), 4);
  int64_t b = (int64_t)base_ms << shift;
  if (unsynced_faults > 1)
    b = std::max<int64_t>(b, 4LL * base_ms);
  return (int)std::min<int64_t>(b, std::max(max_ms, base_ms));
}

/* Local estimate of the device time (ticks) without a control read: the end of the last good
 * sample plus the host monotonic time elapsed since that packet, plus a lead that grows with
 * failed attempts (a late command is just another recoverable failure). */
static inline int64_t usrp_ring_local_start_ticks(int64_t last_end_ticks, double elapsed_s, double rate, double lead_s,
                                                  int failures)
{
  return last_end_ticks + (int64_t)((std::max(elapsed_s, 0.0) + lead_s * std::max(failures, 1)) * rate);
}

/* POLL MODE (ISAC_USRP_RING_POLL): once the stream runs, the producer calls recv() with a zero timeout,
 * so UHD never parks it on a condition variable and the DPDK I/O lcore never has to futex-wake it (with
 * the blocking recv() that is one wake syscall + IPI for most packets). Classify one zero-timeout result:
 * data -> consume it; empty -> poll again until limit_s of continuous silence, then it is a real receive
 * timeout (the same fault the blocking 0.1 s recv() raised). */
enum usrp_ring_poll_action { USRP_RING_POLL_DATA = 0, USRP_RING_POLL_RETRY = 1, USRP_RING_POLL_TIMEOUT = 2 };
static inline usrp_ring_poll_action usrp_ring_poll_classify(size_t got, bool timeout_code, double idle_s,
                                                            double limit_s)
{
  if (got || !timeout_code)
    return USRP_RING_POLL_DATA; /* samples, or an empty result without error: handled by the normal path */
  return idle_s < limit_s ? USRP_RING_POLL_RETRY : USRP_RING_POLL_TIMEOUT;
}

/* Kernel cpulist ("15-19", "2,4-6", "" or "\n") -> bitmask of CPUs 0..63. Used by ISAC_USRP_ISOLATION_CHECK
 * on /sys/devices/system/cpu/{isolated,nohz_full}. Malformed input yields the CPUs parsed so far. */
static inline uint64_t usrp_cpulist_mask(const char *s)
{
  uint64_t m = 0;
  while (s && *s) {
    if (*s < '0' || *s > '9') { s++; continue; }
    int a = 0;
    while (*s >= '0' && *s <= '9') a = a * 10 + (*s++ - '0');
    int b = a;
    if (*s == '-') {
      s++;
      b = 0;
      while (*s >= '0' && *s <= '9') b = b * 10 + (*s++ - '0');
    }
    for (int c = a; c <= b && c < 64; c++)
      m |= 1ULL << c;
  }
  return m;
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
  /* Second budget: at most window_max faults inside window_s seconds (0 = disabled). A stream
   * that faults every 25-100 ms resets `failures` each time, so only this bound ends it. */
  int window_max;
  double window_s;
  std::deque<double> fault_times;
  /* Faults seen while the receiver was not synced and no clean sync has been seen since. */
  int unsynced_faults = 0;

  usrp_ring_recovery_state(uint64_t rate, int max_failures, int max_in_window = 0, double window_seconds = 60.0)
      : sample_rate(rate), limit(max_failures), window_max(max_in_window), window_s(window_seconds) {}
  /* now_s: monotonic seconds; only used by the fault-window budget. */
  bool fault(double now_s = 0.0, bool synced = true)
  {
    unsynced_faults = synced ? 0 : unsynced_faults + 1;
    boundary = write_index;
    good_samples = 0;
    bool ok = ++failures <= limit;
    if (window_max > 0) {
      fault_times.push_back(now_s);
      while (!fault_times.empty() && now_s - fault_times.front() > window_s)
        fault_times.pop_front();
      if ((int)fault_times.size() > window_max)
        ok = false;
    }
    return ok;
  }
  void first_packet() { ++epoch; }
  void samples(size_t count, bool synced = true)
  {
    if (synced)
      unsynced_faults = 0;
    write_index += count;
    good_samples += count;
    /* A start that delivered 20 ms of samples succeeded. Count only a burst
     * of failed starts toward the fatal budget; do not spend it over several
     * short, valid streams during a persistent packet-loss episode. */
    if (good_samples >= std::max<uint64_t>(1, sample_rate / 50)) {
      if (unsynced_faults > 0) { /* acquisition not done: short valid streams do not clear the budget */
        good_samples = 0;
        return;
      }
      failures = 0;
      good_samples = 0;
    }
  }
};
#endif
