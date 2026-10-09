/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include <errno.h>
#include <semaphore.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

/* This is a shutdown notification, not a task join. Initialize before main:
 * a radio error or signal can post before main reaches its wait. Reinitializing
 * here used to discard that post (and posting an uninitialized sem was UB). */
static sem_t itti_sem_block;

/* itti_exit_gate_init: constructor that runs before main(): initialises the exit semaphore, so a radio error or signal can post it before main reaches its wait. Exits the process if the semaphore cannot be created. */
__attribute__((constructor)) static void itti_exit_gate_init(void)
{
  if (sem_init(&itti_sem_block, 0, 0))
    _exit(EXIT_FAILURE);
}

/* itti_wait_tasks_unblock: posts the exit semaphore to release the main thread waiting in itti_wait_tasks_end(). Async-signal-safe (sem_post only); preserves errno. Callers: nrue_shutdown_notify() (executables/nr-uesoftmodem.c), catch_sigterm() and the shutdown test. */
void itti_wait_tasks_unblock(void)
{
  int saved_errno = errno;
  if (sem_post(&itti_sem_block))
    _exit(EXIT_FAILURE);
  errno = saved_errno;
}

/* catch_sigterm: default SIGINT/SIGTERM handler of itti_wait_tasks_end(): unblocks the exit gate. */
static void catch_sigterm(int sig)
{
  (void)sig;
  itti_wait_tasks_unblock();
}

/* itti_wait_tasks_end: blocks the calling (main) thread until the exit gate is posted: installs `handler` (or catch_sigterm) for SIGINT and SIGTERM, then waits on the exit semaphore, retrying after unrelated signals.
 *   Exits the process if the handlers cannot be installed. Caller: executables/nr-uesoftmodem.c (main, passive and SA modes) and common/utils/tests/nr_shutdown_test.c.
 */
void itti_wait_tasks_end(void (*handler)(int))
{
  struct sigaction action = {.sa_handler = handler ? handler : catch_sigterm};
  sigemptyset(&action.sa_mask);
  sigaddset(&action.sa_mask, SIGINT);
  sigaddset(&action.sa_mask, SIGTERM);
  if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL))
    _exit(EXIT_FAILURE);
  /* An unrelated caught signal may interrupt sem_wait without posting. */
  while (sem_wait(&itti_sem_block))
    if (errno != EINTR)
      _exit(EXIT_FAILURE);
}
