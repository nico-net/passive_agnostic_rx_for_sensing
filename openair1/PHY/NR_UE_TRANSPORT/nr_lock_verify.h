/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/* nr_lock_verify: per-cell verification of a fresh PBCH lock and memory of the SSB beams whose lock never tracked.
 * Pure C, no OAI dependency (unit-tested by test_nr_lock_verify).
 *
 * Why (FIX1102 B, OTA 2026-11-02 15:45-15:59): in 6 of 6 two-cell runs one cell locked an SSB beam whose initial-sync
 * PBCH passed CRC but whose tracking PBCH never decoded once (34 of 34 such locks, 0 ok from the first window). The
 * RFSTALL watchdog waited 8 windows, then did a full device re-init that restarted every cell, and the next search
 * locked the same beam again. A lock that has never decoded a tracking PBCH is rejected after `windows` census windows,
 * and its (PCI, SSB index) is skipped by that cell's next searches so another beam of the cell is tried; the memory is
 * forgotten after a verified lock or after `max_misses` searches that found nothing else. */
#ifndef NR_LOCK_VERIFY_H
#define NR_LOCK_VERIFY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint64_t mask; /* rejected SSB indices (0..63) of `pci` */
  int pci;       /* -1: nothing rejected */
  int misses;    /* searches without a lock since the last rejection */
} nr_ssb_reject_t;

void nr_ssb_reject_init(nr_ssb_reject_t *r);
void nr_ssb_reject_add(nr_ssb_reject_t *r, int pci, int ssb_index);
bool nr_ssb_reject_skip(const nr_ssb_reject_t *r, int pci, int ssb_index);
/* a search ended: locked = a cell was accepted (a verified lock clears through nr_ssb_reject_init). */
void nr_ssb_reject_search_done(nr_ssb_reject_t *r, bool locked, int max_misses);

typedef struct {
  bool was_sync;     /* synchronised at the previous window */
  bool verified;     /* a tracking PBCH decoded since the lock */
  int unverified;    /* consecutive windows synchronised, never verified, no PBCH decoded */
  int lock_ssb;      /* SSB index and PCI at the lock */
  int lock_pci;
} nr_lock_verify_t;

typedef enum { NR_LOCK_KEEP = 0, NR_LOCK_VERIFIED = 1, NR_LOCK_REJECT = 2 } nr_lock_verdict_t;

/* One census window: synced = UE synchronised now; ssb_index / pci = the cell's current values (taken at a new lock).
 * NR_LOCK_VERIFIED is returned once per lock, on its first decoded tracking PBCH; NR_LOCK_REJECT after `windows`
 * windows without any. Returns NR_LOCK_KEEP otherwise (including every window of a lock that has been verified). */
nr_lock_verdict_t nr_lock_verify_window(nr_lock_verify_t *v, bool synced, unsigned long pbch_ok, unsigned long pbch_fail,
                                        int ssb_index, int pci, int windows);

/* The cell's memory, implemented in nr_initial_sync.c (called bound to the cell): add the beam of a rejected lock / forget
 * all after a verified lock. */
void nr_initial_sync_reject_ssb(int pci, int ssb_index);
void nr_initial_sync_ssb_verified(void);

#ifdef __cplusplus
}
#endif

#endif
