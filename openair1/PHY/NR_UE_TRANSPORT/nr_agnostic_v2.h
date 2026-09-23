#ifndef __NR_AGNOSTIC_V2_H__
#define __NR_AGNOSTIC_V2_H__
#include <stdlib.h>
/* ISAC_AGNOSTIC_V2=1: one switch for the convergence changes built offline on 2026-09-15 while the
 * X410 was unavailable (Thompson allocation, stage-1 hand-over, discovery sampling, SS-registry
 * driving, HARQ soft combining, PT-RS density sweep, noise-weighted MRC). Default OFF so every
 * existing capture recipe is unchanged; one flag makes the next OTA run a clean A/B. */
static inline int nr_agnostic_v2(void)
{
  static int s = -1;
  if (s < 0) {
    const char *e = getenv("ISAC_AGNOSTIC_V2");
    s = (e != NULL && atoi(e) != 0) ? 1 : 0;
  }
  return s;
}
#endif
