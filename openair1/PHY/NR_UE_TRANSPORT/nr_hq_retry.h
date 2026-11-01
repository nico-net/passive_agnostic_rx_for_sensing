/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* DL HARQ soft combining x retry arms (2026-10-19). Header-only so the decoder and the unit test share one rule.
 *  - Lane: with combining on, the retry arms run only in the async recovery lane (recov_mode != 0: 1 defers them, 2 is the worker), never inline on the first pass.
 *  - Sum: only the first pass of a grant writes the soft sum; every arm body is quiet (it decodes into the private buffer, d_to_be_cleared) so an arm can neither
 *    add its LLRs (a different geometry) nor count a transmission twice.
 *  - Rescue: an arm that passes CRC retires the entry of that TB (same ndi, first transmitted no later than the job's slot).
 *  - Ordering: the entry's busy flag is the lock. Consumers take it non-blocking (busy = decode without combining); the worker waits for it (bounded). */
#ifndef NR_HQ_RETRY_H
#define NR_HQ_RETRY_H
#include <stdbool.h>
#include <stdint.h>

static inline bool nr_hq_retry_lane(bool harq_on, int recov_mode)
{
  return !harq_on || recov_mode != 0;
}
/* the soft sum is touched only by a non-quiet (first pass) decode */
static inline bool nr_hq_may_write_sum(bool quiet)
{
  return !quiet;
}
/* the stored sum belongs to the TB that the rescued job carried (the 1-bit ndi alone repeats every second TB, so the first-transmission slot orders them) */
static inline bool nr_hq_rescue_match(bool soft_valid, uint8_t e_ndi, int64_t e_first_slot, uint8_t job_ndi, int64_t job_slot)
{
  return soft_valid && e_ndi == job_ndi && job_slot >= e_first_slot;
}
/* The retransmission decision of the DL HARQ store (CPU decode and GPU pass): 1 = combine into the stored sum, 0 = a first transmission (the entry restarts),
 * -1 = decode alone and leave the entry as it is. A retransmission is the same NDI on the process (TS 38.321 5.3.2.2) AND the same TBS: the soft bits of one TB
 * exist only for its own K, C and E (TS 38.212 5.2.2 / 5.4.2.1), and a retransmission carries its initial TBS (TS 38.214 5.1.3.2; a reserved MCS already hands
 * over the recorded one). A same-NDI grant with another TBS is not this TB's retransmission (a missed DCI, or one of the two DCIs read with the wrong layout):
 * combining it with the stored TBS could only fail (2026-10-19 on air: every combined retransmission was decoded with the stored, not its own, TBS). */
static inline int nr_hq_retx_rule(bool soft_valid, uint8_t e_ndi, uint32_t e_tbs, uint8_t ndi, uint8_t rv, uint32_t tbs, bool rv0_admitted)
{
  if (!soft_valid || e_ndi != ndi)
    return 0;
  if (e_tbs != 0 && e_tbs != tbs)
    return -1;
  return (rv != 0 || rv0_admitted) ? 1 : 0;
}
/* ---- retransmission anticipation (pred1101/harq) ------------------------------------------------------------------------------------------------------------------
 * Eviction by age. The soft sum of a failed TB lives for the HARQ round-trip of the cell: the DL k1 of at most 15 slots, the gNB's scheduling of the retransmission and the
 * HARQ-RTT/retransmission timers all fit in 150 ms of air time (numerology-independent: the window is converted with the frame's slot count). An older sum is a TB the gNB gave up
 * on (or whose retransmission we missed): it is dropped instead of being combined into a different TB that reuses the pid with an unseen NDI toggle. */
#define NR_HQ_WINDOW_MS 150
static inline int64_t nr_hq_window_slots(int slots_per_frame)
{
  return (int64_t)NR_HQ_WINDOW_MS * (slots_per_frame > 0 ? slots_per_frame : 20) / 10;
}
static inline bool nr_hq_expired(bool soft_valid, int64_t soft_slot, int64_t now_slot, int64_t window_slots)
{
  return soft_valid && now_slot >= 0 && soft_slot >= 0 && now_slot - soft_slot > window_slots;
}

/* the NDI toggled on a process that still holds an undecoded sum: that TB is over (TS 38.321 5.3.2.2) and its sum is dropped when this grant restarts the process */
static inline bool nr_hq_ndi_evicts(bool soft_valid, uint8_t e_ndi, uint8_t ndi)
{
  return soft_valid && e_ndi != ndi;
}

/* Is a retransmission self-decodable, i.e. does its own E bits carry every systematic column? TS 38.212 5.4.2.1: the circular buffer of N = 66Z (BG1) / 50Z (BG2) bits is read from
 * k0(rv) = 0, 17Z, 33Z, 56Z (BG1) / 0, 13Z, 25Z, 43Z (BG2) for E bits and wraps; the first 2Z systematic columns are punctured, the systematic part is kb columns (22 / 10 at most).
 * So a transmission holds all the information bits iff k0 + E >= N + kb Z (wrapped past the end over the columns 2..kb-1 again). E per code block follows from the code rate:
 * E = kb Z / R (CRC and filler overhead ignored: the real rate is below the table's, E larger). The DCI is read before the TBS is known, so kb is not: k0 + kb Z (1/R - 1) grows with kb,
 * hence the SMALLEST kb of the graph (TS 38.212 5.2.2: BG1 10 for B <= 640, BG2 6 for B <= 192) is the conservative one -- a true answer holds for any TBS. LBRM only shrinks N, which moves
 * the wrap earlier, so the full N is conservative too. bg 0 = the base graph is not known either (TS 38.214 5.1.3.2: BG2 for R <= 0.25, BG1 above 0.67, either between): both must pass.
 * rv 0 is always self-decodable (its first columns are the systematic ones). A rate outside (0, 1) is no code rate: false. */
static inline bool nr_hq_alone_decodable(int bg, int rv, double r1024)
{
  if (rv == 0)
    return true;
  if (rv < 0 || rv > 3 || !(r1024 > 0.0) || r1024 >= 1024.0)
    return false;
  if (bg == 0) {
    if (r1024 <= 256.0)
      bg = 2;
    else if (r1024 > 0.67 * 1024.0)
      bg = 1;
    else
      return nr_hq_alone_decodable(1, rv, r1024) && nr_hq_alone_decodable(2, rv, r1024);
  }
  static const int k0z1[4] = {0, 17, 33, 56}, k0z2[4] = {0, 13, 25, 43};
  const int kb = bg == 1 ? 10 : 6, nz = bg == 1 ? 66 : 50, k0 = bg == 1 ? k0z1[rv] : k0z2[rv];
  const double ez = kb * 1024.0 / r1024;
  return k0 + ez >= nz + kb;
}
/* worker side: take the entry lock, waiting up to `spins` naps; false = still held by a consumer (the sum stays; harmless, the TB is decoded) */
static inline bool nr_hq_lock_wait(void *busy_p, void (*nap)(void), int spins)
{
  int *busy = (int *)busy_p; /* an _Atomic int in C, std::atomic<int> in the test: both are a plain int object */
  for (int i = 0;; i++) {
    int expect = 0;
    if (__atomic_compare_exchange_n(busy, &expect, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
      return true;
    if (i >= spins)
      return false;
    nap();
  }
}
#endif
