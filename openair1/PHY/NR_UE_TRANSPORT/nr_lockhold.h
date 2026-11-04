/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/*! \file nr_lockhold.h
 * \brief Always-on hold diagnostics for a hot per-cell mutex (collapse-415 investigation): a hold longer than
 * NR_LH_THRESH_NS is recorded (holder thread name/TID, call site, hold, own wait) in a small ring, logged AFTER unlock
 * at most once per second ("SENSING: LOCKHOLD ..."), plus a 10 s summary line. The pure part (nr_lh_*) is header-only
 * and clock-injected for the unit test; every field of nr_lockhold_t is guarded by the mutex it instruments.
 * Cost: uncontended lock = trylock + 1 clock_gettime, unlock = 1 clock_gettime; the thread name / TID are fetched
 * only on the >5 ms path. The wrappers nr_lockhold_lock/unlock are in nr_lockhold.c.
 */
#ifndef NR_LOCKHOLD_H
#define NR_LOCKHOLD_H
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define NR_LH_THRESH_NS 5000000ull
#define NR_LH_RING 16
#define NR_LH_BY 8
#define NR_LH_SPLITS 4
#define NR_LH_LINE_GAP_NS 1000000000ull
#define NR_LH_SUM_NS 10000000000ull

typedef struct {
  const char *site;
  int line, tid;
  char tname[16];
  uint32_t hold_us, wait_us, split_us[NR_LH_SPLITS];
  uint64_t end_ns;
} nr_lh_rec_t;

typedef struct {
  /* current holder (valid while the lock is held) */
  uint64_t t_acq, wait_ns, split_ns[NR_LH_SPLITS];
  const char *site;
  int line;
  /* ring of slow holds */
  nr_lh_rec_t ring[NR_LH_RING];
  uint32_t ring_n;
  /* rate limit */
  uint64_t last_line_ns;
  uint32_t suppressed;
  /* 10 s window */
  uint64_t win_t0, win_max_hold, win_max_wait;
  uint32_t win_cnt, by_n;
  struct { char tname[16]; const char *site; int line; uint32_t cnt; } by[NR_LH_BY];
} nr_lockhold_t;

typedef struct {
  bool line, summary;
  nr_lh_rec_t rec;
  uint32_t suppressed;
  /* summary snapshot */
  uint64_t max_hold_ns, max_wait_ns;
  uint32_t cnt, by_n;
  struct { char tname[16]; const char *site; int line; uint32_t cnt; } by[NR_LH_BY];
} nr_lh_evt_t;

/* The caller has just acquired the lock at `now` after waiting `wait_ns`. */
static inline void nr_lh_acquired(nr_lockhold_t *lh, uint64_t now, uint64_t wait_ns, const char *site, int line)
{
  lh->t_acq = now;
  lh->wait_ns = wait_ns;
  lh->site = site;
  lh->line = line;
  memset(lh->split_ns, 0, sizeof(lh->split_ns));
  if (!lh->win_t0) lh->win_t0 = now;
  if (wait_ns > lh->win_max_wait) lh->win_max_wait = wait_ns;
}

static inline void nr_lh_split(nr_lockhold_t *lh, int i, uint64_t ns)
{
  if (i >= 0 && i < NR_LH_SPLITS) lh->split_ns[i] = ns;
}

/* The holder is about to unlock at `now`. ident() fills the calling thread's name and TID; it is called only for a slow hold.
 * Fills *ev; the caller unlocks and then logs ev (never log under the lock). */
static inline void nr_lh_release(nr_lockhold_t *lh, uint64_t now, void (*ident)(char name[16], int *tid), nr_lh_evt_t *ev)
{
  ev->summary = ev->line = false;
  const uint64_t hold = now - lh->t_acq;
  if (hold > NR_LH_THRESH_NS) {
    nr_lh_rec_t *r = &lh->ring[lh->ring_n++ % NR_LH_RING];
    memset(r, 0, sizeof(*r));
    ident(r->tname, &r->tid);
    r->tname[15] = 0;
    r->site = lh->site;
    r->line = lh->line;
    r->hold_us = (uint32_t)(hold / 1000);
    r->wait_us = (uint32_t)(lh->wait_ns / 1000);
    r->end_ns = now;
    for (int i = 0; i < NR_LH_SPLITS; i++) r->split_us[i] = (uint32_t)(lh->split_ns[i] / 1000);
    lh->win_cnt++;
    if (hold > lh->win_max_hold) lh->win_max_hold = hold;
    uint32_t k = 0;
    while (k < lh->by_n && !(lh->by[k].site == r->site && lh->by[k].line == r->line && !strcmp(lh->by[k].tname, r->tname))) k++;
    if (k == lh->by_n && lh->by_n < NR_LH_BY) {
      strcpy(lh->by[k].tname, r->tname);
      lh->by[k].site = r->site;
      lh->by[k].line = r->line;
      lh->by[k].cnt = 0;
      lh->by_n++;
    }
    if (k < lh->by_n) lh->by[k].cnt++; /* shortcut: >8 distinct (thread,site) pairs per window are counted in win_cnt only */
    if (lh->last_line_ns == 0 || now - lh->last_line_ns >= NR_LH_LINE_GAP_NS) {
      ev->line = true;
      ev->rec = *r;
      ev->suppressed = lh->suppressed;
      lh->suppressed = 0;
      lh->last_line_ns = now;
    } else {
      lh->suppressed++;
    }
  }
  if (lh->win_t0 && now - lh->win_t0 >= NR_LH_SUM_NS) {
    if (lh->win_cnt) {
      ev->summary = true;
      ev->cnt = lh->win_cnt;
      ev->max_hold_ns = lh->win_max_hold;
      ev->max_wait_ns = lh->win_max_wait;
      ev->by_n = lh->by_n;
      memcpy(ev->by, lh->by, sizeof(ev->by));
    }
    lh->win_t0 = now;
    lh->win_cnt = lh->by_n = 0;
    lh->win_max_hold = lh->win_max_wait = 0;
  }
}

/* "SENSING: LOCKHOLD ..." formatting (pure). */
static inline int nr_lh_fmt_line(char *b, size_t n, int cell, const char *lock, const nr_lh_evt_t *e)
{
  const nr_lh_rec_t *r = &e->rec;
  int k = snprintf(b, n, "SENSING: LOCKHOLD cell=%d lock=%s holder=%s tid=%d site=%s:%d hold_ms=%.2f wait_ms=%.2f suppressed=%u",
                   cell, lock, r->tname, r->tid, r->site, r->line, r->hold_us / 1000.0, r->wait_us / 1000.0, e->suppressed);
  for (int i = 0; i < NR_LH_SPLITS && k > 0 && (size_t)k < n; i++)
    if (r->split_us[i]) {
      static const char *const nm[NR_LH_SPLITS] = {"note_pci", "step_ex", "poll", "snapshot"};
      k += snprintf(b + k, n - (size_t)k, " %s_ms=%.2f", nm[i], r->split_us[i] / 1000.0);
    }
  return k;
}

static inline int nr_lh_fmt_summary(char *b, size_t n, int cell, const char *lock, const nr_lh_evt_t *e)
{
  int k = snprintf(b, n, "SENSING: LOCKHOLD_SUMMARY cell=%d lock=%s window_s=10 holds_gt5ms=%u max_hold_ms=%.2f max_wait_ms=%.2f by=",
                   cell, lock, e->cnt, e->max_hold_ns / 1e6, e->max_wait_ns / 1e6);
  for (uint32_t i = 0; i < e->by_n && k > 0 && (size_t)k < n; i++)
    k += snprintf(b + k, n - (size_t)k, "%s%s@%s:%d=%u", i ? "," : "", e->by[i].tname, e->by[i].site, e->by[i].line, e->by[i].cnt);
  return k;
}

/* Instrumented lock/unlock (nr_lockhold.c). `lock` is a static name; the cell is nr_cell_idx() of the caller. */
void nr_lockhold_lock(nr_lockhold_t *lh, pthread_mutex_t *m, const char *site, int line);
void nr_lockhold_unlock(nr_lockhold_t *lh, pthread_mutex_t *m, const char *lock);
#endif
