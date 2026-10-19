/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* BATCHCPU routing is removed: begin/end are no-ops and nr_pdsch_route_cpu() is always false (callers still link). */
#ifndef NR_PDSCH_CPU_ROUTE_H
#define NR_PDSCH_CPU_ROUTE_H
#include <stdbool.h>
#include <stdint.h>

typedef struct {
  uint64_t t0;
  int8_t cls;     /* size class, -1 = not tracked */
  int8_t cpu;     /* this body runs on the CPU path */
  int8_t held;    /* holds a GPU gate slot */
} nr_pdsch_route_t;

nr_pdsch_route_t nr_pdsch_route_begin(int nprb, int layers, bool probe);
void nr_pdsch_route_end(nr_pdsch_route_t *r);
/* true while the calling thread's current body must not use any GPU stage (FEP, chest, demod, LDPC) */
bool nr_pdsch_route_cpu(void);
#endif
