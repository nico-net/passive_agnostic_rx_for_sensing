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

void nr_shutdown_radio_leave(void)
{
  radio_gate_depth--;
  pthread_rwlock_unlock(&radio_lifetime);
}

/* Only on a path that never returns into the device call it interrupts. */
static void drop_own_radio_gate(void)
{
  while (radio_gate_depth > 0) {
    radio_gate_depth--;
    pthread_rwlock_unlock(&radio_lifetime);
  }
}

static unsigned long monotonic_ns(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (unsigned long)ts.tv_sec * 1000000000UL + ts.tv_nsec;
}

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

void nr_shutdown_signal(int sig) { nr_shutdown_request(128 + sig); }
int nr_shutdown_requested(void) { return atomic_load(&requested) != 0; }
int nr_shutdown_status(void)
{
  int value = atomic_load(&requested);
  return value ? value - 1 : 0;
}

static void wait_event(sem_t *event)
{
  while (sem_wait(event) != 0)
    if (errno != EINTR)
      _exit(EXIT_FAILURE);
}

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

static void fork_child(void)
{
  struct sigaction action = {.sa_handler = SIG_DFL};
  sigemptyset(&action.sa_mask);
  sigaction(SIGINT, &action, NULL);
  sigaction(SIGTERM, &action, NULL);
}

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

void nr_shutdown_wait_radio(void)
{
  const struct timespec pause = {.tv_nsec = 10000000};
  while (!atomic_load(&radio_released))
    nanosleep(&pause, NULL);
}

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
