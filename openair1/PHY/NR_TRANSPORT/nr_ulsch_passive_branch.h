/* Passive-only antenna policy shared by the UL combiner and its fixed-point
 * scale. Mode 2 means all available RX antennas, as it does for DL. */
#ifndef NR_ULSCH_PASSIVE_BRANCH_H
#define NR_ULSCH_PASSIVE_BRANCH_H
#include <stdbool.h>
#include <stdlib.h>

static inline int nr_ulsch_passive_branch_selection(bool passive, int antennas,
                                                    const char *override, const char *mrc_mode)
{
  if (!passive || antennas<=1) return -1;
  const int keep=override ? atoi(override) : (mrc_mode && atoi(mrc_mode)==2 ? -1 : 0);
  return keep>=0 && keep<antennas ? keep : -1;
}
#endif
