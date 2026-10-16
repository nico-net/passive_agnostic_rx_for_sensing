/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Background lane: one core list (ISAC_BG_CORES) for speculative/discovery threads, never on critical cores. Umbrella ISAC_BG_LANE=1. */
#ifndef NR_BG_LANE_H
#define NR_BG_LANE_H
#ifdef __cplusplus
extern "C" {
#endif
int nr_bg_lane_on(void);                          /* ISAC_BG_LANE=1 */
/* Parses ISAC_BG_CORES (default "2,10,11,12,13,14"), drops critical cores (logged once); returns the count, 0 = none usable. */
int nr_bg_lane_cores(int *cores, int max);
/* Called first thing in a background thread: SCHED_OTHER, affinity = the list, nice 10; list empty -> unpinned nice 19. No-op when off. */
void nr_bg_lane_enter(void);
#ifdef __cplusplus
}
#endif
#endif
