/* Sensing-mode scheduler constants (spec 2026-11-03-two-modes-design.md section 5). Compile-time only. */
#ifndef NR_SENSE_SCHED_H
#define NR_SENSE_SCHED_H

#define SENSE_BUCKET_NS 50000000ull      /* cost bucket */
#define SENSE_DECIDE_NS 4000000000ull    /* decision period */
#define SENSE_CONFIRM 2                  /* consecutive agreeing windows before a change */
#define SENSE_WIN_DECISIONS 4            /* cost window, in decisions */
#define SENSE_HEADROOM 0.40              /* share of each resource a unit may use at its p99 */
#define SENSE_HYST 0.20                  /* hysteresis margin */
#define SENSE_MAX_ENABLES 2              /* units enabled per decision */
#define SENSE_AGE_CAP 2.0                /* fairness multiplier cap ... */
#define SENSE_AGE_FULL_NS 10000000000ull /* ... reached at this age since the last valid wideband CFR */
/* buckets in the cost window: 4 decisions x 4 s / 50 ms = 320 */
#define SENSE_WIN_BUCKETS ((SENSE_WIN_DECISIONS * SENSE_DECIDE_NS) / SENSE_BUCKET_NS)

#endif
