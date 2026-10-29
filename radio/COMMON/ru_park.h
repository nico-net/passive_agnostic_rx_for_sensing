/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef RU_PARK_H
#define RU_PARK_H
/* MPCI-LIVE: a ring RU whose cell was paused (the channelizer entry stops, the cell's ring is marked away until SHM_IQ_AWAY_PAUSED) must not keep its slot thread
 * inside the radio lifetime gate: nrue_ru_restart_rx / reinit / start / stop take that gate EXCLUSIVELY and would wait for a read that cannot complete (G6 open item).
 * The shm_iq device returns SHM_IQ_READ_PAUSED at once for such a ring; ru_park_read() then leaves the gate and parks the calling slot thread on a condition
 * variable (timed, RU_PARK_POLL_MS, so a ring that came back without a wake is found; woken at once by ru_park_wake()). The thread is quiescent while parked: no read, no
 * polling of the ring, no gate held. When the ring is live again the read is simply retried, and the caller sees one read that took a long time: the forward gap of the
 * resumed ring then makes its timing loop re-acquire (RXDISCONT), exactly as after a radio gap.
 * The gate is injected (the receiver: nr_shutdown_radio_enter/leave), so the module has no dependency on the shutdown coordinator and the test can use a rwlock.
 * No environment variable, no option. */
#ifdef __cplusplus
extern "C" {
#endif
#define RU_PARK_PAUSED (-2) /* == SHM_IQ_READ_PAUSED (static assertion in ru_park.c) */
#define RU_PARK_POLL_MS 20
#define RU_PARK_MAX_CELLS 16

typedef struct {
  int (*enter)(void *ctx, int exclusive); /* 1 = entered, 0 = shutdown (the read then returns -1) */
  void (*leave)(void *ctx);
  int (*stopping)(void *ctx);             /* 1 = the process is shutting down */
  void *ctx;
} ru_park_gate_t;

/* One read of cell `cell`: enter(shared), read_once(arg), leave; while read_once returns RU_PARK_PAUSED the thread is parked outside the gate and the read retried.
 * Returns read_once's result (anything but RU_PARK_PAUSED), or -1 when the gate refuses or the process is stopping. */
int ru_park_read(const ru_park_gate_t *g, int cell, int (*read_once)(void *), void *arg);
/* Wake every parked thread now (after a ring resume, a retune, a shutdown request). */
void ru_park_wake(void);
int ru_park_parked(int cell);              /* 1 while a slot thread of the cell is parked */
unsigned long ru_park_waits(int cell);     /* parks since start (a counter, for the MPCI_LIFE line and the tests) */
void ru_park_reset(void);                  /* tests */

/* Tuning mailbox: the control thread posts the new tuning of a cell (centre, SSB, CFO) while the cell's slot thread is parked (its channelizer entry is paused); the slot
 * thread takes it at the top of its loop, outside any acquisition (nr-ue.c), applies it to its own frame parameters and re-acquires. One pending request per cell (a newer
 * one replaces an older untaken one). Wait-free fast path for the common "nothing pending" test. */
typedef struct {
  double centre_hz, cfo_hz;
  int ssb;
} ru_tune_t;
void ru_tune_post(int cell, const ru_tune_t *t);
int ru_tune_take(int cell, ru_tune_t *t); /* 1 and *t filled when a request was pending (cleared), else 0 */

/* Per-cell quiesce / resume registry (MPCI-LIVE). A module with per-cell sessions that outlive the cell's data (PRACH, SRS, sense-API session, dashboard sense view)
 * registers once (at its per-cell prealloc, before the fence); the receiver calls ru_cell_quiesce(cell) when a cell is retired (after its channelizer entry is paused) and
 * ru_cell_resume(cell) when the slot takes a cell again (after it is served). Hooks run on the control thread, bound to the cell (nr_cell_bind), must not allocate
 * and must return quickly; a session is kept, not closed (closing stays in the per-cell shutdown loop). Up to RU_CELL_HOOKS_MAX registrations. */
#define RU_CELL_HOOKS_MAX 16
typedef struct {
  const char *name;
  void (*quiesce)(void *ctx, int cell);
  void (*resume)(void *ctx, int cell);
  void *ctx;
} ru_cell_hook_t;
int ru_cell_hook_register(const ru_cell_hook_t *h); /* 0, or -1 (full) */
void ru_cell_quiesce(int cell);
void ru_cell_resume(int cell);
#ifdef __cplusplus
}
#endif
#endif
