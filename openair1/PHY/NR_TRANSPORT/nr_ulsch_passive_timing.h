#ifndef NR_ULSCH_PASSIVE_TIMING_H
#define NR_ULSCH_PASSIVE_TIMING_H
/* Arrival-time coordinates are signed positions, not confidence scores. Pick
 * the strongest measured CIR peak among participating antennas, never the
 * numerically greatest arrival time. Explicit single-branch mode uses itself. */
static inline int nr_ulsch_passive_timing_branch(int antennas, int keep, const int *peak_power)
{
  if (antennas<=0 || !peak_power) return -1;
  if (keep>=0 && keep<antennas) return keep;
  int best=0;
  for (int a=1;a<antennas;++a)
    if (peak_power[a]>peak_power[best]) best=a;
  return best;
}
#endif
