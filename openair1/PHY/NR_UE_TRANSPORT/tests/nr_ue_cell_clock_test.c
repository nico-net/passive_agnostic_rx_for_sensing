/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* A8: the slot clock, timing hand-offs and early-start instances are one per cell: two threads bound to cells 0 and 1 publish different
 * slots / complete different slot gates and never see each other's; a thread re-binding sees each cell's value; cell 0 starts as the old
 * process globals did (absolute slot -1, gate complete -1). [OFFLINE VERIFIED, unit] */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include "PHY/NR_UE_TRANSPORT/nr_ue_cell_clock.h"
#include "PHY/NR_UE_TRANSPORT/nr_early_start.h"
NR_UE_CELL_CLOCK_DEFINE_PRODUCER();
NR_UE_CELL_CLOCK_DEFINE_REBASE();
NR_UE_CELL_CLOCK_DEFINE_SIGNAL();

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static pthread_barrier_t bar;
static void *cell_thread(void *arg)
{
  const int k = (int)(long)arg;
  nr_cell_bind(k);
  atomic_store(&nr_ue_diag_producer_absolute_slot, 1000 + k);
  atomic_store(&nr_ue_pending_rebase_valid, k + 1);
  atomic_store(&nr_ue_rf_signal_absent, k);
  nr_slot_gate_complete(&nr_slot_gate, 50 + 10 * k);
  nr_es_progress_publish(&nr_es_progress, 50 + 10 * k, 14);
  pthread_barrier_wait(&bar);
  CHECK(atomic_load(&nr_ue_diag_producer_absolute_slot) == 1000 + k);
  CHECK(atomic_load(&nr_ue_pending_rebase_valid) == k + 1);
  CHECK(atomic_load(&nr_ue_rf_signal_absent) == k);
  CHECK(nr_slot_gate_is_complete(&nr_slot_gate, 50 + 10 * k));
  CHECK(!nr_slot_gate_is_complete(&nr_slot_gate, 61));
  return NULL;
}

int main(void)
{
  CHECK(atomic_load(&nr_ue_diag_producer_absolute_slot) == -1); /* N = 1 initial values */
  CHECK(!nr_slot_gate_is_complete(&nr_slot_gate, 0));
  pthread_barrier_init(&bar, NULL, 2);
  pthread_t t[2];
  for (long k = 0; k < 2; k++)
    nr_cell_thread_create(&t[k], NULL, cell_thread, (void *)k, (int)k);
  for (int k = 0; k < 2; k++)
    pthread_join(t[k], NULL);
  for (int k = 1; k >= 0; k--) { /* one thread re-binding */
    nr_cell_bind(k);
    CHECK(atomic_load(&nr_ue_diag_producer_absolute_slot) == 1000 + k);
    CHECK(nr_slot_gate_is_complete(&nr_slot_gate, 50 + 10 * k) && !nr_slot_gate_is_complete(&nr_slot_gate, 51 + 10 * k));
  }
  nr_cell_bind(2); /* an unused cell keeps the initial values */
  CHECK(atomic_load(&nr_ue_diag_producer_absolute_slot) == -1);
  CHECK(!nr_slot_gate_is_complete(&nr_slot_gate, 0));
  printf("%s\n", fails ? "FAILED" : "ALL OK");
  return fails != 0;
}
