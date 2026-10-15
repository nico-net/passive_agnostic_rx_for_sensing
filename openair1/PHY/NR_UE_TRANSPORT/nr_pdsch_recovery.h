#ifndef NR_PDSCH_RECOVERY_H
#define NR_PDSCH_RECOVERY_H
/* Stage 1 Task 6 (ISAC_PDSCH_RECOVERY_ASYNC=1, default off): the CRC-recovery arms of a failed first pass run on a low-priority
 * worker (thread pdschRecov, SCHED_OTHER; nice 0 on explicit cores, nice 10 when unpinned) instead of the consumer. The consumer publishes the
 * failed first-pass result at once, fills a preallocated job slot (acquire -> fill -> submit; nothing is allocated on the consumer) and moves on.
 * A rescue found later is a distinct LATE event (nr_pdsch_passive_queue.c, recov_run/recov_feedback): it is discarded when a newer slot of the
 * same (rnti, harq_pid) was seen meanwhile. The worker runs on the consumer's IQ snapshot (handed off, not copied), so a job cannot expire. A full
 * queue (or a busy lock) makes acquire fail: counted dropped_full, the consumer keeps the job and feeds the failure itself.
 * nr_pdsch_recovery_arm_order is the arm-order API (tested); the decoder (nr_pdsch_passive_decode.c) does not link this module and applies the
 * same order itself (S toggle first when the recovery mode is the worker's). */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct { uint16_t rnti; uint8_t harq_pid, ndi; uint32_t abs_slot; uint32_t arm_mask; } nr_recov_key_t;

/* Worker callbacks. run(): executes the arms of one job slot, calls nr_pdsch_recovery_publish() on a rescue; returns 0 nothing rescued,
 * 1 handled (publish was called), 2 IQ expired before an arm ran, 3 abandoned (config epoch changed: feedback is delivered as FAIL and the hook
 * releases the job without credit). expired(): optional, true when the job's IQ is gone. done(): the slot's resources are released (called once
 * per job, after its feedback, before the slot returns to the pool). Any of them may be NULL. */
enum { NR_RECOV_FB_RESCUED = 0, NR_RECOV_FB_FAIL = 1, NR_RECOV_FB_FAIL_NOCREDIT = 2 };
/* feedback(job, k, outcome): the held-back learning feedback of the job (solver, Technique D, crc_note, ...), called exactly once per submitted job
 * with NR_RECOV_FB_*: RESCUED (publish returned 0), FAIL (the first-pass verdict stands: nothing rescued, the rescue was stale or the epoch
 * changed, so no CRC-OK feedback), FAIL_NOCREDIT (IQ expired: a failure on expired IQ is not evidence). */
void nr_pdsch_recovery_set_hooks(int (*run)(void *job, const nr_recov_key_t *k), bool (*expired)(const nr_recov_key_t *k),
                                 void (*feedback)(void *job, const nr_recov_key_t *k, int outcome), void (*done)(void *job));

bool nr_pdsch_recovery_enabled(void); /* ISAC_PDSCH_RECOVERY_ASYNC, read once */
void nr_pdsch_recovery_set_job_bytes(size_t n); /* job slot size; call before start */
/* depth <= 0: ISAC_PDSCH_RECOVERY_Q (default 32); cores NULL: ISAC_PDSCH_RECOVERY_CORES, else 10-14 minus the launcher's pinned cores (unpinned nice 10 if none left). Idempotent. 0 = running. */
int  nr_pdsch_recovery_start(int depth, const char *cores);
void nr_pdsch_recovery_stop(void);
/* Free job slot (zero-copy handoff target), or NULL: queue full, stopping, or the lock busy (never waits). NULL is counted dropped_full. */
void *nr_pdsch_recovery_acquire(void);
void  nr_pdsch_recovery_release(void *slot); /* give back an acquired slot that is not submitted */
/* Queue an acquired, filled slot (the worker returns it). 0 = queued; 1 = not running. */
int  nr_pdsch_recovery_submit(const nr_recov_key_t *k, void *slot);
uint64_t nr_pdsch_recovery_allocs(void); /* heap allocations made by the module (start only: submit/acquire make none) */
void nr_pdsch_recovery_test_lock(int on); /* tests: hold / release the internal lock */
/* The consumer saw a TB for (rnti, harq_pid) at abs_slot: older rescues of that process become stale. Called on every enqueue. */
void nr_pdsch_recovery_note_newer(uint16_t rnti, uint8_t harq_pid, uint8_t ndi, uint32_t abs_slot);
/* Called by the run hook on a rescue. 0 = publish (the caller delivers the late TB), 1 = stale (newer slot seen for (rnti, harq_pid)),
 * 2 = IQ expired. tb/tbs are the rescued transport block and its size in bits (unused here: the caller owns delivery). */
int  nr_pdsch_recovery_publish(const nr_recov_key_t *k, const uint8_t *tb, uint32_t tbs);
/* A rescue the caller discards itself (config epoch changed): counted as stale. */
void nr_pdsch_recovery_count_stale(void);
/* Arm order for the enabled arms (bitmask CSI 1 | TDRA-S 2 | CDM 4 | wide-MRC 8 | tinterp 16): every nonempty combination of enabled bits,
 * ascending, with the S toggle (2) first (99 % of rescues, SYSAN section 6.3). Returns the count (<= 31). */
int  nr_pdsch_recovery_arm_order(unsigned mask, int out[31]);
/* "RECOV submitted= rescued= stale= expired= dropped_full= late_ms_p50/p99=" */
void nr_pdsch_recovery_stats(char *buf, int n);
#endif
