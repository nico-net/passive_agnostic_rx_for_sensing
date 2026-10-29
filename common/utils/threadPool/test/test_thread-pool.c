/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#include <sched.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/sysinfo.h>
#include "thread-pool.h"
#include "log.h"
#include "task_ans.h"
#include "notified_fifo.h"
#include "common/utils/nr_cell.h"

void displayList(notifiedFIFO_t *nf)
{
  int n = 0;
  notifiedFIFO_elt_t *ptr = nf->outF;

  while (ptr) {
    printf("element: %d, key: %lu\n", ++n, ptr->key);
    ptr = ptr->next;
  }

  printf("End of list: %d elements\n", n);
}

struct testData {
  int id;
  int sleepTime;
  char txt[50];
  task_ans_t* task_ans;
};

void processing(void *arg)
{
  struct testData *in = (struct testData *)arg;
  // printf("doing: %d, %s, in thr %ld\n",in->id, in->txt,pthread_self() );
  sprintf(in->txt, "Done by %ld, job %d", pthread_self(), in->id);
  in->sleepTime = rand() % 1000;
  usleep(in->sleepTime);
  // printf("done: %d, %s, in thr %ld\n",in->id, in->txt,pthread_self() );
  completed_task_ans(in->task_ans);
}

/* mpci audit: a task runs as the cell that pushed it (the workers serve every cell) */
struct cellProbe {
  int seen;
  task_ans_t *ans;
};
static void cellProbeFn(void *arg)
{
  struct cellProbe *p = (struct cellProbe *)arg;
  p->seen = nr_cell_idx();
  completed_task_ans(p->ans);
}

int main()
{
  logInit();
  notifiedFIFO_t myFifo;
  initNotifiedFIFO(&myFifo);
  int num_elements_on_queue = 0;
  pushNotifiedFIFO(&myFifo, newNotifiedFIFO_elt(sizeof(struct testData), 1234, NULL, NULL));
  num_elements_on_queue++;

  for (int i = 10; i > 1; i--) {
    pushNotifiedFIFO(&myFifo, newNotifiedFIFO_elt(sizeof(struct testData), 1000 + i, NULL, NULL));
    num_elements_on_queue++;
  }

  displayList(&myFifo);
  notifiedFIFO_elt_t *tmp = pullNotifiedFIFO(&myFifo);
  printf("pulled: %lu\n", tmp->key);
  displayList(&myFifo);
  delNotifiedFIFO_elt(tmp);
  tmp = pullNotifiedFIFO(&myFifo);
  num_elements_on_queue--;
  printf("pulled: %lu\n", tmp->key);
  displayList(&myFifo);
  pushNotifiedFIFO(&myFifo, newNotifiedFIFO_elt(sizeof(struct testData), 12345678, NULL, NULL));
  displayList(&myFifo);
  delNotifiedFIFO_elt(tmp);

  do {
    tmp = pollNotifiedFIFO(&myFifo);

    if (tmp) {
      printf("pulled: %lu\n", tmp->key);
      displayList(&myFifo);
      delNotifiedFIFO_elt(tmp);
      num_elements_on_queue--;
    } else
      printf("Empty list \n");
  } while (num_elements_on_queue > 0);
  AssertFatal(pollNotifiedFIFO(&myFifo) == NULL, "Unexpected extra element on queue\n");

  tpool_t pool;
  char params[] = "1,2,3,4,5";
  initTpool(params, &pool, true);

  //sleep(1);
  int cumulProcessTime = 0;
  struct timespec st, end;
  clock_gettime(CLOCK_MONOTONIC, &st);
  int nb_jobs = 4;
  for (int i = 0; i < 1000; i++) {
    int parall = nb_jobs;
    task_ans_t task_ans;
    init_task_ans(&task_ans, parall);
    struct testData test_data[parall];
    memset(test_data, 0, sizeof(test_data));
    for (int j = 0; j < parall; j++) {
      task_t task = {.args = &test_data[j], .func = processing};
      struct testData *x = (struct testData *)task.args;
      x->id = i;
      x->task_ans = &task_ans;
      pushTpool(&pool, task);
    }
    join_task_ans(&task_ans);
    int sleepmax = 0;
    for (int j = 0; j < parall; j++) {
      if (test_data[j].sleepTime > sleepmax) {
        sleepmax = test_data[j].sleepTime;
      }
    }
    cumulProcessTime += sleepmax;
  }
  clock_gettime(CLOCK_MONOTONIC, &end);
  long long dur = (end.tv_sec - st.tv_sec) * 1000 * 1000 + (end.tv_nsec - st.tv_nsec) / 1000;
  printf("In µs, Total time per group of %d job:%lld, work time per job %d, overhead per job %lld\n",
         nb_jobs,
         dur / 1000,
         cumulProcessTime / 1000,
         (dur - cumulProcessTime) / (1000 * nb_jobs));
  /* cell binding of pool tasks: pushed from cell 3, cell 0, cell 7 and back to a worker that has just run another cell */
  {
    const int cells[] = {3, 0, 7, 3, 7, 0, 0, 7};
    const int n = sizeof(cells) / sizeof(cells[0]);
    task_ans_t ans;
    init_task_ans(&ans, n);
    struct cellProbe probe[sizeof(cells) / sizeof(cells[0])];
    for (int i = 0; i < n; i++) {
      probe[i] = (struct cellProbe){.seen = -1, .ans = &ans};
      nr_cell_bind(cells[i]);
      pushTpool(&pool, (task_t){.args = &probe[i], .func = cellProbeFn});
    }
    nr_cell_bind(0);
    join_task_ans(&ans);
    for (int i = 0; i < n; i++)
      AssertFatal(probe[i].seen == cells[i], "pool task %d ran as cell %d, pushed from cell %d\n", i, probe[i].seen, cells[i]);
    printf("tpool cell binding: OK\n");
  }
  abortTpool(&pool);
  return 0;
}
