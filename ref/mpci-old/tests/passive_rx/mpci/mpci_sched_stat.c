/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Per-cell view of the MPCI arbiter segment: mpci_sched_stat <segment> [period_s (0 = once)] */
#include "nr_mpci_sched.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
int main(int argc, char **argv) {
  if (argc < 2) {fprintf(stderr, "usage: %s <segment> [period_s]\n", argv[0]);return 2;}
  nr_mpci_t *h = nr_mpci_attach_ro(argv[1]);
  if (!h) {fprintf(stderr, "cannot open segment %s\n", argv[1]);return 1;}
  int period = argc > 2 ? atoi(argv[2]) : 0;
  do {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC, &t);
    uint64_t now = (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
    nr_mpci_cell_stat_t c[NR_MPCI_MAX_CELLS];
    int n = nr_mpci_snapshot(h, now, c, NR_MPCI_MAX_CELLS);
    for (int i = 0; i < n; i++)
      printf("MPCI_CELL tag=%s pid=%d share=%d acq=%d disc_used_pct=%.1f disc_total_ms=%llu yields=%llu rt_inflight=%d rt_jobs=%llu "
             "scanq_drop=%llu/%llu/%llu pdschq_drop=%llu/%llu/%llu/%llu hb_age_ms=%llu\n",
             c[i].tag, c[i].pid, c[i].share, c[i].acquire, c[i].window_used_us / 10000.0, (unsigned long long)c[i].total_used_us / 1000,
             (unsigned long long)c[i].yields, c[i].rt_inflight, (unsigned long long)c[i].rt_jobs,
             (unsigned long long)c[i].drops.scanq_full, (unsigned long long)c[i].drops.scanq_stale, (unsigned long long)c[i].drops.scanq_epoch,
             (unsigned long long)c[i].drops.pdschq_full, (unsigned long long)c[i].drops.pdschq_stale,
             (unsigned long long)c[i].drops.pdschq_epoch, (unsigned long long)c[i].drops.pdschq_narrow,
             (unsigned long long)c[i].heartbeat_age_us / 1000);
    fflush(stdout);
    if (period) sleep(period);
  } while (period);
  return 0;
}
