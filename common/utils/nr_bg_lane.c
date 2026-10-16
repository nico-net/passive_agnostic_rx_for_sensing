/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#define _GNU_SOURCE
#include "nr_bg_lane.h"
#include "nr_core_plan.h"
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

int nr_bg_lane_on(void)
{
  const char *e = getenv("ISAC_BG_LANE");
  return e && atoi(e) == 1;
}
int nr_bg_lane_cores(int *cores, int max)
{
  const char *e = getenv("ISAC_BG_CORES");
  if (!e) e = "2,10,11,12,13,14"; /* DGX default; other hosts set ISAC_BG_CORES */
  unsigned char mask[NR_CP_MAX_CPUS] = {0};
  static int warned;
  int n = 0;
  if (nr_core_parse_list(e, mask) != 0) {
    if (!warned++) fprintf(stderr, "SENSING: BG_LANE ISAC_BG_CORES=\"%s\" is not a CPU list; background threads run unpinned at nice 19\n", e);
    return 0;
  }
  for (int c = 0; c < NR_CP_MAX_CPUS; c++) if (mask[c]) {
    if (nr_core_plan_is_critical(c)) {
      if (!warned++) fprintf(stderr, "SENSING: BG_LANE core %d of ISAC_BG_CORES is a critical-lane core and is dropped\n", c);
    } else if (n < max)
      cores[n++] = c;
  }
  if (n == 0 && !warned++) fprintf(stderr, "SENSING: BG_LANE no usable core in ISAC_BG_CORES=\"%s\"; background threads run unpinned at nice 19\n", e);
  return n;
}
void nr_bg_lane_enter(void)
{
  if (!nr_bg_lane_on()) return;
  int cores[NR_CP_MAX_CPUS];
  const int n = nr_bg_lane_cores(cores, NR_CP_MAX_CPUS);
  struct sched_param sp = {0};
  pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);
  if (n > 0) {
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int i = 0; i < n; i++) CPU_SET(cores[i], &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
  }
  setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), n > 0 ? 10 : 19); /* per-thread on Linux */
}
