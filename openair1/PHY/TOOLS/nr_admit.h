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
/* test hooks */
void nr_admit_reset_for_test(void);
void nr_admit_note_duty_for_test(nr_admit_class_t c, int pct); /* pct% duty in the 100 ms window containing t=0 */
#ifdef __cplusplus
}
#endif
#endif
