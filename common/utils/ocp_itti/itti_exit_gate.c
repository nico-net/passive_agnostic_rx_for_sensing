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

__attribute__((constructor)) static void itti_exit_gate_init(void)
{
  if (sem_init(&itti_sem_block, 0, 0))
    _exit(EXIT_FAILURE);
}

void itti_wait_tasks_unblock(void)
{
  int saved_errno = errno;
  if (sem_post(&itti_sem_block))
    _exit(EXIT_FAILURE);
  errno = saved_errno;
}

static void catch_sigterm(int sig)
{
  (void)sig;
  itti_wait_tasks_unblock();
}

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
