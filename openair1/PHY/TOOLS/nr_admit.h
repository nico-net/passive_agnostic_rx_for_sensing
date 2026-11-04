/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Admission control for background (speculative/discovery) work (mpci B6): skips the lowest-value classes while the core pool is saturated and serves >= 2 cells; always runs otherwise. */
#ifndef NR_ADMIT_H
#define NR_ADMIT_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { NR_ADMIT_JANON, NR_ADMIT_USS, NR_ADMIT_NID, NR_ADMIT_CSIRS, NR_ADMIT_DMRS_SCAN,
               NR_ADMIT_DMRS_ID, NR_ADMIT_PROBE, NR_ADMIT_UL_DISC, NR_ADMIT_N } nr_admit_class_t;
typedef int (*nr_admit_backlog_fn)(void); /* >0 = critical work waiting */
void nr_admit_register_backlog(nr_admit_backlog_fn f); /* up to 8; duplicates ignored */
int nr_admit_on(void);                                 /* the pool serves >= 2 active cells (admission can skip) */
int nr_admit_tier(nr_admit_class_t c);                 /* shed value tier (0 = shed first), -1 = never shed */
/* 1 = run now, 0 = skip (counted). deadline_ns = end of sample lifetime; now_ns from CLOCK_MONOTONIC. */
int nr_admit_try(nr_admit_class_t c, uint64_t now_ns, uint64_t deadline_ns, int priority_token);
void nr_admit_done(nr_admit_class_t c, uint64_t busy_ns); /* feeds the duty floor */
void nr_admit_stats(char *buf, int n);                    /* "ADMIT janon[run= skip_busy= expired= floor=] ..." */
uint64_t nr_admit_now_ns(void);
/* Per-class counters of the CALLING thread's cell (nr_cell_idx()); attempts are counted only while nr_admit_on() (>= 2 active cells), so a single cell
 * counts nothing and prints nothing new. attempts = every nr_admit_try call while on, skipped = refused calls, recovered = backlog items replayed, expired = backlog items dropped (age or full). */
void nr_admit_class_counts(nr_admit_class_t c, uint64_t *attempts, uint64_t *skipped, uint64_t *recovered, uint64_t *expired);
/* Skip backlog (JANON, DMRS_ID): a skipped item is kept, not freed. One per cell and class, used by the single worker that owns the class; a spin lock makes a shared worker safe.
 * Zero-initialised = empty. Push drops the oldest when full (counted expired). Pop returns the oldest live item only while no tier is shed
 * (nr_pool_site_shed_tiers() == 0, or the weak symbol absent), so a replay never adds CPU under overload; items past their deadline are dropped (counted expired) on every call.
 * free_fn releases a dropped item; a popped item is owned by the caller. */
#define NR_ADMIT_SKIPQ_DEPTH 4
typedef struct { void *p[NR_ADMIT_SKIPQ_DEPTH]; uint64_t deadline[NR_ADMIT_SKIPQ_DEPTH]; int head, n, lock; } nr_admit_skipq_t;
void nr_admit_skipq_push(nr_admit_class_t c, nr_admit_skipq_t *q, void *item, uint64_t deadline_ns, void (*free_fn)(void *));
void *nr_admit_skipq_pop(nr_admit_class_t c, nr_admit_skipq_t *q, uint64_t now_ns, void (*free_fn)(void *));
void nr_admit_skipq_drain(nr_admit_class_t c, nr_admit_skipq_t *q, void (*free_fn)(void *)); /* shutdown: free all, counted expired */
int nr_admit_skipq_len(const nr_admit_skipq_t *q);
/* test hooks */
void nr_admit_reset_for_test(void);
void nr_admit_note_duty_for_test(nr_admit_class_t c, int pct); /* pct% duty in the 100 ms window containing t=0 */
#ifdef __cplusplus
}
#endif
#endif
