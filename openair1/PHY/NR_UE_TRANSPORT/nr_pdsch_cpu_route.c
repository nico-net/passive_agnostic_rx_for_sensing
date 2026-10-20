/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_pdsch_cpu_route.h"

/* CPU/GPU size-class routing and GPU batch gate are removed (always off): stubs keep the callers linking. */
bool nr_pdsch_route_cpu(void) { return false; }

nr_pdsch_route_t nr_pdsch_route_begin(int nprb, int layers, bool probe)
{
  (void)nprb; (void)layers; (void)probe;
  nr_pdsch_route_t r = {0, -1, 0, 0};
  return r;
}

void nr_pdsch_route_end(nr_pdsch_route_t *r) { (void)r; }
