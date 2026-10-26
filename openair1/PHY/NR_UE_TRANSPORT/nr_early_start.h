/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/*
 * EARLY START of the PDCCH fast lane, per CORESET.
 *
 * The RX thread reads one slot per iteration and the read returns when the slot (plus the first symbol of the next one) is in the ring.
 * A CORESET at symbols 0-2 has been in the ring ~11 symbols before that. For every verified (banked) CORESET whose first symbol and
 * duration are known, the RX thread splits the slot read at the end of that CORESET's last symbol, publishes the symbol progress and
 * posts an EARLY scan job; the scan consumer runs the bank passes that are due, and the slot-end job skips what ran early.
 *
 *  - Triggers come from a snapshot of the bank geometry that the scan consumers publish every occasion (seqlock), so a CORESET that
 *    moves, appears or is removed (the existing reconfiguration path rewrites the bank) changes the triggers within one slot.
 *    CORESETs not yet discovered are not in the bank: they stay on the slot-end job.
 *  - A CORESET whose last symbol is the last symbol(s) of the slot fires at slot end, i.e. never later than today.
 *  - Grants enqueued before the slot ends must not start the PDSCH consumer before the PDSCH symbols are in the ring: the consumer
 *    waits on nr_slot_gate (a condvar the RX thread signals only if somebody waits; no polling).
 *  - e2e arrival stays the slot end. nr_es_progress tells at which symbol a DCI was accepted (stamp "accepted at symbol k").
 */
#ifndef NR_EARLY_START_H
#define NR_EARLY_START_H
#include <pthread.h>
#ifndef NR_ATOMIC
#ifdef __cplusplus
#include <atomic>
using std::atomic_compare_exchange_strong;
using std::atomic_compare_exchange_weak;
using std::atomic_fetch_add;
using std::atomic_fetch_add_explicit;
using std::atomic_fetch_sub;
using std::atomic_load;
using std::atomic_load_explicit;
using std::atomic_store;
using std::atomic_store_explicit;
using std::memory_order_acq_rel;
using std::memory_order_acquire;
using std::memory_order_relaxed;
using std::memory_order_release;
#define NR_ATOMIC(t) std::atomic<t>
#else
#include <stdatomic.h>
#define NR_ATOMIC(t) _Atomic(t)
#endif
#endif
#include <stdbool.h>
#include <stdint.h>
#include "common/utils/nr_cell.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NR_ES_MAX_BANKS 8
#define NR_ES_MAX_POINTS 8

typedef struct { int8_t first_symbol, duration; } nr_es_cs_t; /* duration 0 = entry unused / removed */

typedef struct {
  NR_ATOMIC(uint32_t) seq;  /* odd while a writer is inside */
  NR_ATOMIC(uint32_t) gen;  /* +1 per publish that changed anything */
  NR_ATOMIC(int) wlock;
  int n;
  nr_es_cs_t cs[NR_ES_MAX_BANKS];
} nr_es_banks_t;

/* Writer side (scan consumers, once per occasion). Index i = bank index. Returns true if the geometry changed. */
bool nr_es_banks_publish(nr_es_banks_t *b, const nr_es_cs_t *cs, int n);
/* Reader side (RX thread): consistent copy; false if a writer kept it busy (use no early start this slot). */
bool nr_es_banks_read(const nr_es_banks_t *b, nr_es_cs_t *out, int *n);

typedef struct {
  int sym_end;      /* OFDM symbols of the slot complete when this point fires */
  int sample_off;   /* sample offset of that point inside the slot's READ chunk (>= 0) */
  uint32_t mask;    /* bank indices that fire here */
} nr_es_point_t;

/* Trigger points of one slot. cum[k] = samples from the slot start to the end of symbol k-1 (cum[0] = 0, k = 0..symbols_per_slot);
 * first_sym_samples = what the read of this slot does NOT contain (symbol 0 came with the previous read). Points are returned in
 * ascending order, equal symbol ends merged. A CORESET ending at the last symbol of the slot is not a point (it fires at slot end). */
int nr_es_plan(const nr_es_cs_t *cs, int n, int symbols_per_slot, const uint32_t *cum, uint32_t first_sym_samples,
               nr_es_point_t *pts, int max_pts);

/* Symbol progress of the slot being read: (absolute slot << 8) | symbols complete. */
typedef struct { NR_ATOMIC(uint64_t) v; } nr_es_progress_t;
static inline void nr_es_progress_publish(nr_es_progress_t *p, long abs_slot, int symbols)
{
  atomic_store_explicit(&p->v, ((uint64_t)abs_slot << 8) | (uint64_t)(symbols & 0xff), memory_order_release);
}
static inline int nr_es_progress_symbols(const nr_es_progress_t *p, long abs_slot, int symbols_per_slot)
{
  const uint64_t v = atomic_load_explicit(&p->v, memory_order_acquire);
  if ((long)(v >> 8) == abs_slot) return (int)(v & 0xff);
  return (long)(v >> 8) > abs_slot ? symbols_per_slot : 0; /* later slot published: this one is complete; earlier: nothing yet */
}

/* Banks already run by an early job, per slot: the slot-end job skips them. */
typedef struct { NR_ATOMIC(long) slot[NR_ES_MAX_BANKS]; } nr_es_done_t;
static inline void nr_es_done_mark(nr_es_done_t *d, int bank, long slot)
{
  if (bank >= 0 && bank < NR_ES_MAX_BANKS) atomic_store_explicit(&d->slot[bank], slot + 1, memory_order_release);
}
static inline bool nr_es_done_is(const nr_es_done_t *d, int bank, long slot)
{
  return bank >= 0 && bank < NR_ES_MAX_BANKS && atomic_load_explicit(&d->slot[bank], memory_order_acquire) == slot + 1;
}

/* Slot gate: the consumer of a grant enqueued before the slot ended waits here until the RX thread says the slot's samples are in. */
typedef struct {
  pthread_mutex_t mu;
  pthread_cond_t cv;
  NR_ATOMIC(long) complete; /* highest slot whose read has returned */
  NR_ATOMIC(int) waiters;
} nr_slot_gate_t;
void nr_slot_gate_init(nr_slot_gate_t *g);
void nr_slot_gate_complete(nr_slot_gate_t *g, long slot);          /* RX thread; one atomic load when nobody waits */
bool nr_slot_gate_wait(nr_slot_gate_t *g, long slot, int max_ms);  /* true once complete >= slot; false on timeout */
static inline bool nr_slot_gate_is_complete(const nr_slot_gate_t *g, long slot)
{
  return atomic_load_explicit(&g->complete, memory_order_acquire) >= slot;
}

/* Set by the scan consumer while it runs an EARLY job: the PDSCH queue stamps the grants it accepts then (gate + "accepted at symbol k"). */
extern __thread int nr_es_tls_early;
extern __thread int nr_es_tls_symbols_per_slot;

/* One instance per cell (nr_early_start.c; multi-PCI A8): the bank geometry, symbol progress, early-done set and slot gate all describe the
 * slot stream of ONE cell (its RX thread publishes, its scan / PDSCH consumers read). The names select the calling thread's cell (nr_cell_idx();
 * every user is bound to its cell), so `&nr_slot_gate` etc. are unchanged at the call sites. N = 1: cell 0's instance. */
extern nr_es_banks_t nr_es_banks_cells[NR_CELL_MAX];
extern nr_es_progress_t nr_es_progress_cells[NR_CELL_MAX];
extern nr_es_done_t nr_es_done_cells[NR_CELL_MAX];
extern nr_slot_gate_t nr_slot_gate_cells[NR_CELL_MAX];
#define nr_es_banks (nr_es_banks_cells[nr_cell_idx()])
#define nr_es_progress (nr_es_progress_cells[nr_cell_idx()])
#define nr_es_done (nr_es_done_cells[nr_cell_idx()])
#define nr_slot_gate (nr_slot_gate_cells[nr_cell_idx()])

#ifdef __cplusplus
}
#endif
#endif
