/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* nr_cell: cell identity core of the single-process multi-PCI receiver (plan 2026-10-23-mpci-main-plan.md, A1/B.2).
 * FROZEN seam header (changed only by group C with a handover note).
 *
 *  - nr_cell_idx(): the cell index of the calling thread, one TLS read, 0 when never set (single-cell path).
 *  - nr_cell_thread_create(): every thread of a cell is created through it; the child is bound to `cell`
 *    (cell < 0: inherit the caller's index, which is how nested thread creation keeps the cell).
 *  - nr_cell_path(): host-global default path -> per-cell path; cell 0 returns the default byte for byte.
 *  - NR_CELL_STATIC*: cell-indexed statics. Threads that serve a cell are bound to it; GPU host callbacks and
 *    CUDA event threads are NOT (they carry `cell` in the job and must not use nr_cell_idx()).
 *
 * ---- Converting a file-scope static (declaration-only, uses stay textually unchanged) ----
 *     before:   static int foo = 3;
 *     after:    NR_CELL_STATIC_INIT(int, foo, 3)
 *               #define foo NR_CELL_REF(foo)
 *   C cannot define a macro from a macro, so the second line (the alias) is unavoidable; keep the two lines adjacent.
 *   Every use of `foo` (read, write, ++, &foo, sizeof) then maps to foo_cells[nr_cell_idx()] and is an lvalue.
 *   Caveat: the alias is a macro, so a local variable or struct member named `foo` in the same file is also rewritten.
 *   The audit table (tools/mpci/mpci_statics.tsv) lists every object.
 *
 * Initializer semantics: NR_CELL_STATIC(type,name) zero-initialises all NR_CELL_MAX slots (C and C++).
 *   NR_CELL_STATIC_INIT(type,name,init...) gives EVERY cell the same initial value (GNU range designator
 *   [0 ... NR_CELL_MAX-1], C only: gcc/clang). A scalar `3` or a brace initializer `{1, 2}` are both accepted.
 *   Slots are not re-initialised when a cell is rebound; a cell that is torn down and re-activated must reset its own state.
 *
 * Large objects (queues, tables, histories): NR_CELL_STATIC_HEAP(type,name) keeps NR_CELL_MAX pointers and allocates
 *   the object of a cell (zeroed, calloc) on the first access of that cell. Allocation is allowed ONLY BEFORE the RX fence:
 *   the owner of the receiver calls nr_cell_alloc_fence() once streaming starts (the GPU pass forbids allocations after
 *   nr_gpu_rx_started()); a first access of a not-yet-allocated slot after the fence is fatal. Activate a cell by touching
 *   (or nr_cell_heap_prealloc) its objects before the fence. HEAP objects start zeroed, there is no initializer.
 *   Usage:    NR_CELL_STATIC_HEAP(big_t, big)
 *             #define big NR_CELL_HEAP_REF(big_t, big)
 */
#ifndef NR_CELL_H
#define NR_CELL_H

#include <pthread.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NR_CELL_MAX 16 /* must equal NR_POOL_MAX_CELLS (common/utils/nr_core_pool.h); checked in nr_cell.c */

extern __thread int nr_cell_tls_idx_; /* private: use nr_cell_idx() */

static inline int nr_cell_idx(void)
{
  return nr_cell_tls_idx_;
}

/* Process-wide number of active cells (seam addition D2, plan Task log). 1 (the default) is the single-cell receiver:
 * every N=1 output is byte-identical to the pre-multi-cell code. Only the LOG tag (log.c) and the nr_cprintf shim
 * (nr_cprintf.h) read it: they prefix "[c<k>] " when it is > 1. The cell manager calls nr_cell_set_count(n) once,
 * before the cell threads start. n outside 1..NR_CELL_MAX is fatal. */
extern int nr_cell_count_; /* private: use nr_cell_count() */

static inline int nr_cell_count(void)
{
  return __atomic_load_n(&nr_cell_count_, __ATOMIC_RELAXED);
}

void nr_cell_set_count(int n);

/* Bind the calling thread to cell idx. idx outside 0..NR_CELL_MAX-1 is fatal (message on stderr, exit 2). */
void nr_cell_bind(int idx);

/* pthread_create whose child is bound to `cell`; cell < 0: the child inherits nr_cell_idx() of the caller.
 * Returns pthread_create's result (EINVAL/ENOMEM for the wrapper). cell >= NR_CELL_MAX is fatal. */
int nr_cell_thread_create(pthread_t *t, const pthread_attr_t *attr, void *(*fn)(void *), void *arg, int cell);

/* Per-cell path from the default host-global path `def`. cell 0: `def` copied unchanged.
 * cell k > 0 (tag "cell<k>"): file with extension  /tmp/a.txt -> /tmp/a.cell<k>.txt ;
 *                             no extension / directory  /tmp/passive_rx -> /tmp/passive_rx/cell<k>.
 * Output is always NUL terminated and truncated safely (n == 0: nothing written). cell outside range is fatal.
 * Callers must not pass a user-chosen explicit path (explicit env overrides win untouched). */
void nr_cell_path(int cell, const char *def, char *out, size_t n);

/* RX fence for heap-allocated cell objects (see NR_CELL_STATIC_HEAP). */
void nr_cell_alloc_fence(void);
int nr_cell_alloc_fenced(void);
/* Allocate (zeroed) *slot of `size` bytes if still NULL; thread safe; returns the object. Fatal after the fence. */
void *nr_cell_heap_get(void **slot, size_t size, const char *name);

#ifdef __cplusplus
}
#endif

/* ---- cell-indexed statics ---- */
#define NR_CELL_REF(name) (name##_cells[nr_cell_idx()])

#define NR_CELL_STATIC(type, name) static type name##_cells[NR_CELL_MAX];

#if defined(__GNUC__) && !defined(__cplusplus)
#define NR_CELL_STATIC_INIT(type, name, ...) static type name##_cells[NR_CELL_MAX] = {[0 ... NR_CELL_MAX - 1] = __VA_ARGS__};
#endif

#define NR_CELL_STATIC_HEAP(type, name) static type *name##_cellp[NR_CELL_MAX];
#define NR_CELL_HEAP_REF(type, name) \
  (*(type *)(name##_cellp[nr_cell_idx()] ? (void *)name##_cellp[nr_cell_idx()] \
                                         : nr_cell_heap_get((void **)&name##_cellp[nr_cell_idx()], sizeof(type), #name)))
/* Pre-allocate cell `c`'s object before the fence (e.g. at cell activation). */
#define NR_CELL_HEAP_PREALLOC(type, name, c) ((void)nr_cell_heap_get((void **)&name##_cellp[(c)], sizeof(type), #name))

#endif
