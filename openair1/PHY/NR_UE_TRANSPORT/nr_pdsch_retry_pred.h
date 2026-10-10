#pragma once
/* ISAC_PDSCH_RETRY_PREDICT (default off, DECQUEUE): per-RNTI memory of the recovery arm that rescued its last grants. The retry chain
 * (nr_pdsch_passive_decode.c passive_decode_retry) otherwise decodes the plain reading first, fails, then re-decodes the alternate
 * start symbol: measured 2446 of 5803 retry chains on PCI 415 were rescued by exactly that arm (S toggle), each costing a wasted body.
 * Once an RNTI was rescued NR_RETRY_PRED_MIN times in a row, the arm is tried FIRST; a miss resets the streak. Header-only, thread-safe. */
#ifdef __cplusplus
#include <atomic>
using std::atomic_load_explicit;
using std::atomic_store_explicit;
using std::memory_order_relaxed;
#define NR_RP_ATOMIC(t) std::atomic<t>
#else
#include <stdatomic.h>
#define NR_RP_ATOMIC(t) _Atomic(t)
#endif
#include <stdbool.h>
#include <stdint.h>

#define NR_RETRY_PRED_SLOTS 256
#define NR_RETRY_PRED_MIN 2
typedef struct { NR_RP_ATOMIC(uint32_t) v; } nr_retry_pred_t; /* rnti << 8 | streak (0..255) */
static nr_retry_pred_t g_nr_retry_pred[NR_RETRY_PRED_SLOTS];

/* nr_retry_pred_hit: the arm rescued this RNTI (after the plain reading failed, or as the predicted first try): streak + 1. */
static inline void nr_retry_pred_hit(uint16_t rnti)
{
  nr_retry_pred_t *p = &g_nr_retry_pred[rnti % NR_RETRY_PRED_SLOTS];
  const uint32_t o = atomic_load_explicit(&p->v, memory_order_relaxed);
  const uint32_t s = ((o >> 8) == rnti && (o & 255) < 255) ? (o & 255) + 1 : ((o >> 8) == rnti ? 255 : 1);
  atomic_store_explicit(&p->v, ((uint32_t)rnti << 8) | s, memory_order_relaxed);
}
/* nr_retry_pred_miss: the plain reading passed, or the predicted arm failed: the streak is gone. */
static inline void nr_retry_pred_miss(uint16_t rnti)
{
  nr_retry_pred_t *p = &g_nr_retry_pred[rnti % NR_RETRY_PRED_SLOTS];
  if ((atomic_load_explicit(&p->v, memory_order_relaxed) >> 8) == rnti) atomic_store_explicit(&p->v, (uint32_t)rnti << 8, memory_order_relaxed);
}
/* nr_retry_pred_try_first: true when the arm should run before the plain reading. */
static inline bool nr_retry_pred_try_first(uint16_t rnti)
{
  const uint32_t o = atomic_load_explicit(&g_nr_retry_pred[rnti % NR_RETRY_PRED_SLOTS].v, memory_order_relaxed);
  return (o >> 8) == rnti && (o & 255) >= NR_RETRY_PRED_MIN;
}
