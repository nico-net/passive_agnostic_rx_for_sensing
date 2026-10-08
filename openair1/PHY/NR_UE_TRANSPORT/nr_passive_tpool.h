/* ISAC_PDSCH_INLINE_POOL=1 (T26c): passive PDSCH consumers run their FEP / channel-estimation / LDPC fan-out tasks
 * INLINE instead of pushing them to the UE's shared thread pool. MEASURED (replay raw_pci415_12s_b, PDTIM with thread
 * CPU time): chest wall 85.6 ms vs 1.6 ms CPU, fep 7.3 ms vs 32 us, ldpc 3.1 ms vs 0.1 ms -- every consumer joined on
 * tasks queued behind the single `--thread-pool -1` worker that also serves the RT path (gdb: join_task_ans in
 * nr_pdsch_channel_estimation / passive_decode_body), so 5 consumers decoded ~40 jobs/s live. */
#ifndef NR_PASSIVE_TPOOL_H
#define NR_PASSIVE_TPOOL_H
#include <stdbool.h>
#include "common/utils/threadPool/thread-pool.h"
extern __thread bool nr_passive_tpool_inline; /* set by a passive consumer thread; defined in nr_dl_channel_estimation.c */
static inline tpool_t *nr_passive_tpool(tpool_t *dflt)
{
  static __thread tpool_t inline_pool; /* len_thr == 0: pushTpool() runs the task in the caller */
  return nr_passive_tpool_inline ? &inline_pool : dflt;
}
#endif
