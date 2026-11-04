/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* See nr_lockhold.h. */
#include "nr_lockhold.h"
#include "common/utils/LOG/log.h"
#include "common/utils/nr_cell.h"
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static uint64_t lh_now(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void lh_ident(char name[16], int *tid)
{
  prctl(PR_GET_NAME, name, 0, 0, 0);
  *tid = (int)syscall(SYS_gettid);
}

void nr_lockhold_lock(nr_lockhold_t *lh, pthread_mutex_t *m, const char *site, int line)
{
  uint64_t t0 = 0, wait = 0;
  if (pthread_mutex_trylock(m) != 0) {
    t0 = lh_now();
    pthread_mutex_lock(m);
  }
  const uint64_t t1 = lh_now();
  if (t0) wait = t1 - t0;
  nr_lh_acquired(lh, t1, wait, site, line);
}

void nr_lockhold_unlock(nr_lockhold_t *lh, pthread_mutex_t *m, const char *lock)
{
  nr_lh_evt_t ev;
  nr_lh_release(lh, lh_now(), lh_ident, &ev);
  pthread_mutex_unlock(m);
  char b[512];
  if (ev.line) {
    nr_lh_fmt_line(b, sizeof(b), nr_cell_idx(), lock, &ev);
    LOG_W(PHY, "%s\n", b);
  }
  if (ev.summary) {
    nr_lh_fmt_summary(b, sizeof(b), nr_cell_idx(), lock, &ev);
    LOG_W(PHY, "%s\n", b);
  }
}
