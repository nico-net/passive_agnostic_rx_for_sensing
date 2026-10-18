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
  /* DEFERRED/DO NOT USE: ISAC_BG_LANE not in the frozen config t26zE (2026-10-18); slated for removal */
  const char *e = getenv("ISAC_BG_LANE");
  return e && atoi(e) == 1;
}
int nr_bg_lane_cores(int *cores, int max)
{
  /* DEFERRED/DO NOT USE: ISAC_BG_CORES not in the frozen config t26zE (2026-10-18); slated for removal */
  const char *e = getenv("ISAC_BG_CORES");
  if (!e) e = "2,11,12,13,14"; /* DGX default; other hosts set ISAC_BG_CORES */
  unsigned char mask[NR_CP_MAX_CPUS] = {0};
  static int warned;
  int n = 0;
  if (nr_core_parse_list(e, mask) != 0) {
    if (!warned++) fprintf(stderr, "SENSING: BG_LANE ISAC_BG_CORES=\"%s\" is not a CPU list; background threads run unpinned at nice 19\n", e);
    return 0;
  }
  const long online = sysconf(_SC_NPROCESSORS_ONLN);
  for (int c = 0; c < NR_CP_MAX_CPUS; c++) if (mask[c] && c < online) {
    if (nr_core_plan_is_critical(c)) {
      if (!warned++) fprintf(stderr, "SENSING: BG_LANE core %d of ISAC_BG_CORES is a critical-lane core and is dropped\n", c);
    } else if (n < max)
      cores[n++] = c;
  }
  if (n == 0 && !warned++) fprintf(stderr, "SENSING: BG_LANE no usable core in ISAC_BG_CORES=\"%s\"; background threads run unpinned at nice 19\n", e);
  return n;
}
void nr_bg_lane_enter(const char *own_core_env)
{
  if (!nr_bg_lane_on()) return;
  const int own = own_core_env ? nr_env_list_nth(own_core_env, 0, -1) : -1;
  if (own >= 0 && !nr_core_plan_is_critical(own)) { /* the thread is already pinned there: only demote it */
    struct sched_param sp0 = {0};
    pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp0);
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
    return;
  }
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
