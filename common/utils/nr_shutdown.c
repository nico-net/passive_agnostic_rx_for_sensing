/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#define _GNU_SOURCE
#include "nr_shutdown.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

/* Signal delivery may interrupt any application lock. These atomics must never
 * use a library lock; the only other handler operations are clock_gettime and
 * sem_post. No logging, malloc, ITTI allocation, UHD, or pthread joins there. */
_Static_assert(ATOMIC_INT_LOCK_FREE == 2 && ATOMIC_LONG_LOCK_FREE == 2, "shutdown requires lock-free atomics");
static atomic_int requested, radio_released;
static atomic_ulong requested_ns;
static sem_t radio_event, watchdog_event;
static void (*notify_callback)(void), (*stop_callback)(void), (*release_callback)(void);
static unsigned timeout_s = 15;
static int log_fd = -1;
static pid_t owner_pid;
static pthread_rwlock_t radio_lifetime = PTHREAD_RWLOCK_INITIALIZER;
/* Gate holds of the calling thread. A fatal path (AssertFatal/exit_function,
 * replay EOF) can run INSIDE a device call: it must drop its own hold before
 * waiting for the release thread, which needs the exclusive gate. */
static __thread int radio_gate_depth;

/* nr_shutdown_radio_enter: takes the radio lifetime gate (read-write lock): exclusive for the start, release and re-init paths, shared for the per-call device operations.
 *   Returns 0 without taking the gate when a shutdown was already requested (the caller must then skip the device call); otherwise 1 and counts the hold for this thread.
 *   Callers: executables/nr-ue-ru.c (every public nrue_ru_* wrapper). Must be paired with nr_shutdown_radio_leave().
 */
int nr_shutdown_radio_enter(int exclusive)
{
  if (nr_shutdown_requested())
    return 0;
  if (exclusive)
    pthread_rwlock_wrlock(&radio_lifetime);
  else
    pthread_rwlock_rdlock(&radio_lifetime);
  if (nr_shutdown_requested()) {
    pthread_rwlock_unlock(&radio_lifetime);
    return 0;
  }
  radio_gate_depth++;
  return 1;
}

/* nr_shutdown_radio_leave: releases one hold of the radio lifetime gate taken by nr_shutdown_radio_enter() on this thread. Caller: executables/nr-ue-ru.c. */
void nr_shutdown_radio_leave(void)
{
  radio_gate_depth--;
  pthread_rwlock_unlock(&radio_lifetime);
}

/* drop_own_radio_gate: releases every radio gate hold of the calling thread. Used only on a fatal path that never returns into the device call, so that the release thread can take the exclusive gate.
 *   Called from exit_cleanup() and nr_shutdown_finish().
 */
static void drop_own_radio_gate(void)
{
  while (radio_gate_depth > 0) {
    radio_gate_depth--;
    pthread_rwlock_unlock(&radio_lifetime);
  }
}

/* monotonic_ns: returns CLOCK_MONOTONIC time in nanoseconds (shutdown timestamps and deadlines). */
static unsigned long monotonic_ns(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (unsigned long)ts.tv_sec * 1000000000UL + ts.tv_nsec;
}

/* nr_shutdown_step: writes one line `SENSING: SHUTDOWN elapsed_ms=... step=... status=...` to the log descriptor with a non-blocking write, timed from the first request.
 *   Async-safe in the sense of the header: no stdio, no locks, so it can run from the release and watchdog threads even when the logger is stuck. Callers: nr_shutdown_finish(), the release/watchdog threads and executables/nr-uesoftmodem.c.
 */
void nr_shutdown_step(const char *step)
{
  char line[320];
  unsigned long start = atomic_load(&requested_ns);
  double ms = start ? (monotonic_ns() - start) / 1e6 : 0;
  int n = snprintf(line, sizeof(line), "SENSING: SHUTDOWN elapsed_ms=%.3f step=%s status=%d\n",
                   ms, step, nr_shutdown_status());
  if (n > 0 && log_fd >= 0) {
    /* A full log pipe must not defeat the shutdown deadline. No stdio locks. */
    ssize_t ignored = write(log_fd, line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
    (void)ignored;
  }
}

/* nr_shutdown_request: requests shutdown with status (the first request wins; an error after a normal end replaces status 0). Starts the timer and wakes the release and watchdog threads.
 *   Async-signal-safe: only atomics, clock_gettime and sem_post. errno is preserved. Callers: nr_shutdown_signal(), nr-ue-ru.c, usrp_lib.cpp and the start-up code.
 */
void nr_shutdown_request(int status)
{
  int saved_errno = errno;
  int expected = 0;
  if (atomic_compare_exchange_strong(&requested, &expected, status + 1)) {
    atomic_store(&requested_ns, monotonic_ns());
    sem_post(&watchdog_event);
    sem_post(&radio_event);
  } else if (status && expected == 1) {
    /* An error after a normal end must not be reported as success. */
    atomic_compare_exchange_strong(&requested, &expected, status + 1);
  }
  errno = saved_errno;
}

/* nr_shutdown_signal: signal handler for SIGINT and SIGTERM: requests shutdown with status 128 + signal number. */
void nr_shutdown_signal(int sig) { nr_shutdown_request(128 + sig); }
/* nr_shutdown_requested: returns 1 once shutdown has been requested, else 0. Lock-free atomic read; safe in any thread and in signal context. */
int nr_shutdown_requested(void) { return atomic_load(&requested) != 0; }
/* nr_shutdown_status: returns the exit status of the shutdown request (0 when none or a normal end was requested). */
int nr_shutdown_status(void)
{
  int value = atomic_load(&requested);
  return value ? value - 1 : 0;
}

/* wait_event: waits on a semaphore, retrying on EINTR; exits the process if the semaphore fails for any other reason. */
static void wait_event(sem_t *event)
{
  while (sem_wait(event) != 0)
    if (errno != EINTR)
      _exit(EXIT_FAILURE);
}

/* release_thread: the radio release thread: after the first request it calls the notify callback, stops the radio under the shared gate (cancelling blocking I/O), and then releases it under the exclusive gate.
 *   Logs each step and sets radio_released when the release completes. Started by nr_shutdown_init().
 */
static void *release_thread(void *unused)
{
  (void)unused;
  pthread_setname_np(pthread_self(), "radio_shutdown");
  wait_event(&radio_event);
  nr_shutdown_step("requested");
  notify_callback();
  /* Stop is the backend's concurrent-I/O cancellation hook. A shared lock
   * prevents init/reinit from replacing priv while allowing recv to unwind. */
  nr_shutdown_step("radio_stop_begin");
  pthread_rwlock_rdlock(&radio_lifetime);
  stop_callback();
  pthread_rwlock_unlock(&radio_lifetime);
  nr_shutdown_step("radio_stop_complete");
  nr_shutdown_step("radio_release_begin");
  pthread_rwlock_wrlock(&radio_lifetime);
  release_callback();
  pthread_rwlock_unlock(&radio_lifetime);
  nr_shutdown_step("radio_release_complete");
  atomic_store(&radio_released, 1);
  return NULL;
}

/* watchdog_thread: the shutdown watchdog: once shutdown is requested it waits until `timeout_s` seconds after the first request (ISAC_SHUTDOWN_TIMEOUT_S, default 15) and then exits the process with the shutdown status.
 *   The timer starts at the first request, so the radio cleanup gets the whole budget. Started by nr_shutdown_init().
 */
static void *watchdog_thread(void *unused)
{
  (void)unused;
  pthread_setname_np(pthread_self(), "shutdown_timer");
  wait_event(&watchdog_event);
  unsigned long ns = atomic_load(&requested_ns);
  struct timespec deadline = {.tv_sec = ns / 1000000000UL + timeout_s, .tv_nsec = ns % 1000000000UL};
  while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL) == EINTR) {}
  /* Cleanup started at the FIRST request, not at the deadline: this gives UHD
   * the whole budget, even when main is stuck in a join/startup. Never race a
   * second destructor against the release owner. If UHD itself is stuck, exit
   * stops claim renewal; remote lease expiry cannot be guaranteed locally. */
  nr_shutdown_step(atomic_load(&radio_released) ? "watchdog_exit_radio_released" : "watchdog_exit_radio_release_incomplete");
  _exit(nr_shutdown_status() ? nr_shutdown_status() : EXIT_FAILURE);
}

/* exit_cleanup: atexit handler: in the owning process, requests a clean shutdown, drops this thread's gate holds and waits for the radio release. Does nothing in a forked child.
 *   Registered by nr_shutdown_init().
 */
static void exit_cleanup(void)
{
  /* T tracer can fork after initialization; its child has no shutdown threads
   * and never owns the radio. Do not wait for vanished threads in that child. */
  if (getpid() != owner_pid)
    return;
  nr_shutdown_request(0);
  drop_own_radio_gate();
  nr_shutdown_wait_radio();
}

/* fork_child: pthread_atfork child handler: restores the default SIGINT and SIGTERM handlers in the child process. */
static void fork_child(void)
{
  struct sigaction action = {.sa_handler = SIG_DFL};
  sigemptyset(&action.sa_mask);
  sigaction(SIGINT, &action, NULL);
  sigaction(SIGTERM, &action, NULL);
}

/* nr_shutdown_init: starts the shutdown subsystem: stores the three callbacks (notify, stop and release the radio), reads ISAC_SHUTDOWN_TIMEOUT_S, opens the log descriptor, starts the release and watchdog threads, installs the SIGINT/SIGTERM handlers, and registers the fork and exit hooks.
 *   Returns 0, or -1 when a semaphore, thread or handler could not be set up. Caller: executables/nr-uesoftmodem.c (start-up).
 */
int nr_shutdown_init(void (*notify_main)(void), void (*stop_radio)(void), void (*release_radio)(void))
{
  owner_pid = getpid();
  notify_callback = notify_main;
  stop_callback = stop_radio;
  release_callback = release_radio;
  const char *e = getenv("ISAC_SHUTDOWN_TIMEOUT_S");
  if (e) {
    char *end;
    long value = strtol(e, &end, 10);
    if (*e && !*end && value >= 1 && value <= 300)
      timeout_s = value;
  }
  /* Separate open description: do not make the application's stderr nonblocking. */
  log_fd = open("/proc/self/fd/2", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
  if (sem_init(&radio_event, 0, 0) || sem_init(&watchdog_event, 0, 0))
    return -1;
  pthread_t radio, watchdog;
  if (pthread_create(&watchdog, NULL, watchdog_thread, NULL) || pthread_create(&radio, NULL, release_thread, NULL))
    return -1;
  pthread_detach(watchdog);
  pthread_detach(radio);
  struct sigaction action = {.sa_handler = nr_shutdown_signal};
  sigemptyset(&action.sa_mask);
  sigaddset(&action.sa_mask, SIGINT);
  sigaddset(&action.sa_mask, SIGTERM);
  if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL))
    return -1;
  sigset_t termination_signals;
  sigemptyset(&termination_signals);
  sigaddset(&termination_signals, SIGINT);
  sigaddset(&termination_signals, SIGTERM);
  pthread_sigmask(SIG_UNBLOCK, &termination_signals, NULL);
  pthread_atfork(NULL, NULL, fork_child);
  atexit(exit_cleanup);
  return 0;
}

/* nr_shutdown_wait_radio: blocks until the radio release has completed (radio_released). Callers: exit_cleanup(), nr_shutdown_finish() and executables/nr-uesoftmodem.c. */
void nr_shutdown_wait_radio(void)
{
  const struct timespec pause = {.tv_nsec = 10000000};
  while (!atomic_load(&radio_released))
    nanosleep(&pause, NULL);
}

/* nr_shutdown_finish: final exit path of the receiver: drops the thread's gate holds, waits for the radio release, flushes stdio and exits with the shutdown status (_exit, so no stdio teardown).
 *   The watchdog still bounds a stuck flush. Declared noreturn. Caller: executables/nr-uesoftmodem.c (main exit).
 */
_Noreturn void nr_shutdown_finish(void)
{
  drop_own_radio_gate();
  nr_shutdown_wait_radio();
  nr_shutdown_step("complete");
  /* _exit skips stdio teardown: flush buffered logs/printf output first. A
   * stuck stdio lock is still bounded by the watchdog. */
  fflush(NULL);
  _exit(nr_shutdown_status());
}
