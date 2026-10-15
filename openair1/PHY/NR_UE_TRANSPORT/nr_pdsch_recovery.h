#ifndef NR_PDSCH_RECOVERY_H
#define NR_PDSCH_RECOVERY_H
/* Stage 1 Task 6 (ISAC_PDSCH_RECOVERY_ASYNC=1, default off): the CRC-recovery arms of a failed first pass run on a low-priority
 * worker (thread pdschRecov, SCHED_OTHER nice 0) instead of the consumer. The consumer publishes the failed first-pass result at once,
 * hands {job copy, key} to this queue and moves on. A rescue found later is a distinct LATE event (see nr_pdsch_passive_queue.c,
 * recov_late_publish): it is discarded when a newer slot of the same (rnti, harq_pid) was seen meanwhile, and the job is dropped when its
 * IQ left the sample ring before the worker ran. Submit never blocks: a full queue drops (counted). */
#include <stdint.h>
#include <stdbool.h>

typedef struct { uint16_t rnti; uint8_t harq_pid, ndi; uint32_t abs_slot; uint32_t arm_mask; } nr_recov_key_t;

/* Worker callbacks. run(): executes the arms of one job (job_owned), calls nr_pdsch_recovery_publish() on a rescue; returns 0 nothing
 * rescued, 1 handled (published or discarded by publish), 2 IQ expired before an arm ran. The worker frees job_owned with free().
 * expired(): true when the IQ of the key's slot is no longer in the sample ring. Both optional (NULL: job is only freed / never expired). */
void nr_pdsch_recovery_set_hooks(int (*run)(void *job_owned, const nr_recov_key_t *k), bool (*expired)(const nr_recov_key_t *k));

bool nr_pdsch_recovery_enabled(void); /* ISAC_PDSCH_RECOVERY_ASYNC, read once */
/* depth <= 0: ISAC_PDSCH_RECOVERY_Q (default 32); cores NULL: ISAC_PDSCH_RECOVERY_CORES (default "3,4"). Idempotent. 0 = running. */
int  nr_pdsch_recovery_start(int depth, const char *cores);
void nr_pdsch_recovery_stop(void);
/* 0 = queued (job_owned now belongs to the worker), 1 = queue full (counted dropped_full; the caller keeps job_owned). Never blocks on the worker. */
int  nr_pdsch_recovery_submit(const nr_recov_key_t *k, void *job_owned);
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
