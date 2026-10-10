/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include "executables/nr-ue-ru.h"
#include "common/utils/nr_shutdown.h"
#include "common/utils/LOG/log.h"

/* Link the production RU read/lifetime code, with no RF plugin or PHY runtime. */
void nrue_ru_test_device(openair0_device_t device);
void itti_wait_tasks_end(void (*handler)(int));
void itti_wait_tasks_unblock(void);
int oai_exit;
static log_t test_log;
log_t *g_log = &test_log;
/* logRecord_mt: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: common/utils/LOG/log.c:588, common/utils/LOG/log.c:874, common/utils/LOG/log.h:243, common/utils/LOG/log.h:346 (+12 more).
 */
void logRecord_mt(const char *file, const char *func, int line, int comp, int level, const char *format, ...)
{
  va_list args;
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
}

static const char *mode;
static atomic_int reads, reading, stopped, released, reader_entered;
static PHY_VARS_NR_UE ue;

/* fake_read: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: none found in tree.
 */
static int fake_read(openair0_device_t *dev, openair0_timestamp_t *ts, void **buf, int n, int antennas)
{
  if (atomic_load(&released))
    _exit(90);
  atomic_store(&reading, 1);
  atomic_store(&reader_entered, 1);
  int read = atomic_fetch_add(&reads, 1);
  *ts = read * n;
  if (!strcmp(mode, "active_read"))
    usleep(200000);
  atomic_store(&reading, 0);
  if (!strcmp(mode, "fatal_in_read") && read == 3) {
    /* AssertFatal/replay EOF inside the driver: exit_function never returns
     * into this read, so it must not keep the release thread out. */
    nr_shutdown_request(1);
    nr_shutdown_finish();
  }
  if (!strcmp(mode, "zero"))
    return 0;
  if (!strcmp(mode, "short"))
    return read == 0 ? n / 2 : -1;
  if (!strcmp(mode, "discontinuous"))
  {
    if (read)
      *ts = 10000; /* second timestamp != first + n/2 */
    return read == 0 ? n / 2 : n;
  }
  return !strcmp(mode, "injected") || read < 3 ? n : -1;
}

/* fake_stop: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: none found in tree.
 */
static int fake_stop(openair0_device_t *dev)
{
  if (atomic_fetch_add(&stopped, 1))
    _exit(91);
  fprintf(stderr, "FAKE_STREAM_STOP\n");
  return 0;
}

/* fake_end: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: none found in tree.
 */
static void fake_end(openair0_device_t *dev)
{
  if (!atomic_load(&stopped) || atomic_load(&reading) || atomic_fetch_add(&released, 1))
    _exit(92);
  if (!strcmp(mode, "blocked_radio")) {
    for (;;)
      usleep(10000);
  }
  dev->priv = NULL;
  dev->trx_end_func = NULL;
  fprintf(stderr, "FAKE_CLAIM_RELEASED\n");
}

/* notify_main: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: none found in tree.
 */
static void notify_main(void)
{
  oai_exit = 1; /* Reproduce the already-set exit flag, without suppressing the post. */
  itti_wait_tasks_unblock();
}

/* reader: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: none found in tree.
 */
static void *reader(void *unused)
{
  int32_t iq[4][16];
  void *buffers[4] = {iq[0], iq[1], iq[2], iq[3]};
  openair0_timestamp_t timestamp;
  while (!nr_shutdown_requested())
    if (nrue_ru_read(&ue, &timestamp, buffers, 16, 4) < 0)
      break;
  return NULL;
}

/* harmless_signal: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: none found in tree.
 */
static void harmless_signal(int sig) {}

/* interrupt_wait: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: none found in tree.
 */
static void *interrupt_wait(void *unused)
{
  usleep(30000);
  pthread_kill(*(pthread_t *)unused, SIGUSR1);
  usleep(30000);
  nr_shutdown_request(3);
  return NULL;
}

/* main: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: none (program entry point).
 */
int main(int argc, char **argv)
{
  if (argc != 2)
    return 99;
  mode = argv[1];
  nrue_ru_test_device((openair0_device_t){.priv = &ue, .trx_read_func = fake_read,
                                        .trx_stop_func = fake_stop, .trx_end_func = fake_end});
  if (nr_shutdown_init(notify_main, nrue_ru_shutdown_stop, nrue_ru_shutdown))
    return 98;
  pthread_t thread, main_thread = pthread_self();
  int join_thread = 0;
  if (!strcmp(mode, "fork_child")) {
    pid_t child = fork();
    if (child == 0)
      exit(0); /* inherited atexit must not wait for the parent's threads */
    int status;
    if (child < 0 || waitpid(child, &status, 0) != child || status != 0)
      return 95;
    nr_shutdown_request(3);
  } else if (!strcmp(mode, "direct_exit")) {
    exit(3); /* exercise atexit fallback before the main wait */
  } else if (!strcmp(mode, "sigint") || !strcmp(mode, "sigterm") || !strcmp(mode, "repeated")) {
    raise(!strcmp(mode, "sigterm") ? SIGTERM : SIGINT);
    if (!strcmp(mode, "repeated"))
      for (int i = 0; i < 100; i++) {
        raise(SIGTERM);
        raise(SIGINT);
      }
  } else if (!strcmp(mode, "blocked_main") || !strcmp(mode, "blocked_radio")) {
    nr_shutdown_request(3);
    /* Simulate a stuck join/startup: the radio releaser and watchdog must run
     * without ever depending on main reaching the ITTI wait or cleanup tail. */
    for (;;)
      usleep(10000);
  } else if (!strcmp(mode, "eintr")) {
    struct sigaction action = {.sa_handler = harmless_signal};
    sigemptyset(&action.sa_mask);
    sigaction(SIGUSR1, &action, NULL);
    pthread_create(&thread, NULL, interrupt_wait, &main_thread);
    join_thread = 1;
  } else {
    pthread_create(&thread, NULL, reader, NULL);
    join_thread = 1;
    if (!strcmp(mode, "early_fatal")) {
      pthread_join(thread, NULL);
      join_thread = 0;
      nr_shutdown_wait_radio(); /* post definitely precedes wait */
    } else if (!strcmp(mode, "active_read")) {
      while (!atomic_load(&reader_entered))
        usleep(1000);
      raise(SIGTERM);
    }
  }
  itti_wait_tasks_end(nr_shutdown_signal);
  if (!nr_shutdown_requested())
    return 97;
  nr_shutdown_wait_radio();
  if (join_thread)
    pthread_join(thread, NULL);
  /* New I/O cannot enter after release, and must not touch a freed device. */
  int32_t iq[4][16];
  void *buffers[4] = {iq[0], iq[1], iq[2], iq[3]};
  openair0_timestamp_t ts;
  if (nrue_ru_read(&ue, &ts, buffers, 16, 4) != -1 || ts != 0)
    return 96;
  nr_shutdown_finish();
}
