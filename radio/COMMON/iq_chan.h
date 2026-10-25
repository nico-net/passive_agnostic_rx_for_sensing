/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* MPCI C2: in-process channelizer. One thread of the receiver reads the wideband ring (shm_iq_ring.h, anonymous backing) block by block and, for every cell
 * entry, writes the cell's narrow stream (mix to the cell centre + halfband decimation, iq_ddc) into a narrow ring of its own that the cell's consumer
 * (the shm_iq device, or a thread using shm_iq_ring_view()) reads. It replaces the out-of-process shm_iq_chan (a second CUDA context was 7x slower under two
 * receivers, MPCI-OTA 2026-10-16): the work is done by a BACKEND behind iq_chan_backend_t; this file provides the CPU backend (iq_ddc, one thread, any host)
 * and the selection hook the GPU backend (G5) registers behind. Contract, tested by test_iq_chan:
 *   - every entry's output is bit-exact against a fresh iq_ddc (and the frozen reference) fed the same blocks: the loop adds no arithmetic;
 *   - add / remove / retune of an entry is applied at a block boundary on the channelizer thread; the other entries keep their filter state and their
 *     streams stay bit-exact (no re-sync of the loop, no restart);
 *   - an overrun (the ring was overwritten before it was read) or a discontinuity (valid_from) skips the lost input and resets the HISTORY (filter memory and
 *     mixer phase) of every entry; entries, designs, shifts, output rings and consumers are kept, so the next block is bit-exact with a fresh filter started at the
 *     resume position and the consumers see one timestamp jump (forward gap) in their ring;
 *   - a ring retune (rf_epoch) recomputes the shifts of entries given by absolute centre, resets the histories and bumps every output ring's epoch;
 *   - an entry with decim 1 and shift 0 is a memcpy from the input block to its ring (no filter state, no scratch).
 * Sizes: the narrow rings come from shm_iq_ring_plan_capacity() under the budget given in the config (the host-RAM plan); nothing is hardcoded to a host. */
#ifndef IQ_CHAN_H
#define IQ_CHAN_H
#include "iq_ddc.h"
#include "shm_iq_ring.h"
#include <stddef.h>
#include <stdint.h>

#define IQ_CHAN_MAX_CELLS 16
#define IQ_CHAN_DEFAULT_BLOCK 65536 /* samples per block and channel (a multiple of 16 and of the mixer block); clipped to an eighth of the ring */
#define IQ_CHAN_DEFAULT_ATTEN_DB 70.0

/* A cell entry. shift_hz = cell centre - ring centre; with centre_hz > 0 the shift follows the ring centre across ring retunes. */
typedef struct {
  char tag[32];          /* names the narrow ring "<ring>_<tag>" in the in-process registry (empty: reach it with iq_chan_cell_ring()) */
  double shift_hz;       /* used when centre_hz <= 0 */
  double centre_hz;      /* absolute cell centre (Hz), 0 = use shift_hz */
  int decim;             /* 1, 2, 4, 8, 16: ring rate / cell rate */
  double passband_hz;    /* one-sided half-width the filters must preserve; iq_ddc_cell_passband() gives the H3 value (num_rb_dl x 12 x SCS) */
  double atten_db;       /* stopband, 0 = IQ_CHAN_DEFAULT_ATTEN_DB */
  unsigned consumers;    /* tracked consumers of the narrow ring (0 = 4) */
} iq_chan_entry_t;

/* The work a backend is asked to do: used by capacity() and by the selection hook. */
typedef struct {
  double in_rate;
  unsigned nch, n_cells;
  struct { int decim; int shifted; double passband_hz; /* 0 = 0.3 x the cell rate (a 106/51 PRB grid occupies about 0.6 x its rate) */ } cell[IQ_CHAN_MAX_CELLS];
} iq_chan_load_t;

typedef struct {
  double in_rate;
  unsigned nch;
  int block;             /* samples per channel in one run() call at most */
} iq_chan_geom_t;

/* Backend: designs filters (prepare), runs blocks (run), reports what it sustains (capacity). The CPU backend is iq_chan_backend_cpu. A GPU backend (G5) registers
 * a second struct with iq_chan_backend_register(); which one a channelizer uses is decided by the selection hook (R5: a path in nr_gpu_path).
 * prepare/release run on the caller's thread of the add/retune API (allocation, filter design); install/retire/set_shift/reset/run run on the channelizer thread
 * at block boundaries and must not allocate. slot < IQ_CHAN_MAX_CELLS names an entry; one cell state per slot. */
typedef struct iq_chan_backend {
  const char *name;
  void *(*create)(const iq_chan_geom_t *geom);                       /* backend instance, NULL on failure */
  void (*destroy)(void *ctx);
  void *(*prepare)(void *ctx, const iq_chan_entry_t *e);             /* design the entry's filters; NULL on failure */
  void (*release)(void *ctx, void *cell);                            /* free a prepared or retired cell */
  void (*install)(void *ctx, int slot, void *cell);                  /* start serving the cell in `slot` */
  void *(*retire)(void *ctx, int slot);                              /* stop serving, return the cell for release() */
  void (*set_shift)(void *ctx, int slot, double shift_hz);           /* retune the mixer of one entry, phase continuous, history kept */
  void (*reset)(void *ctx, int slot);                                /* forget the history of one entry (slot < 0: all) after an input discontinuity */
  /* One block of n samples per channel (n a multiple of every installed decim). out[slot * nch + ch] is the destination of that entry's n / decim samples,
   * NULL for slots that are not served this block. Returns 0, or a negative error (the channelizer stops). */
  int (*run)(void *ctx, const int16_t *const *in, int n, int16_t *const *out);
  /* Sustainable input rate (samples/s per channel) for `load` on this host: measured, not assumed. 0 = unknown/cannot serve. */
  size_t (*capacity)(const iq_chan_load_t *load);
  /* Optional: run() on k threads including the caller (worker i pinned to cpus[i % ncpus]); results must not depend on k. NULL = single thread only. */
  int (*set_workers)(void *ctx, int k, const int *cpus, int ncpus);
} iq_chan_backend_t;

extern const iq_chan_backend_t iq_chan_backend_cpu;

/* Registry and selection hook (R5). iq_chan_backend_cpu is always registered. */
int iq_chan_backend_register(const iq_chan_backend_t *b);            /* 0, or -1 (full / duplicate name) */
const iq_chan_backend_t *iq_chan_backend_find(const char *name);
typedef const iq_chan_backend_t *(*iq_chan_select_fn)(const iq_chan_load_t *load, void *user);
void iq_chan_set_selector(iq_chan_select_fn fn, void *user);         /* NULL restores the default */
const iq_chan_backend_t *iq_chan_backend_select(const iq_chan_load_t *load); /* the selector's answer; default: the CPU backend */

/* Self-bench of the CPU backend on this host: one thread, synthetic noise, `seconds` of wall time, no ring. Used for the capacity input of admission. */
typedef struct {
  double seconds, blocks, samples_per_s, rt_factor; /* samples/s per channel consumed; rt_factor = samples_per_s / in_rate */
  double cpu_s;                                      /* thread CPU time (CLOCK_THREAD_CPUTIME_ID) */
} iq_chan_bench_t;
/* Threads needed to keep real time for `load` given the measured per-thread rate (MS/s per channel, from iq_chan_cpu_bench) and a margin (>= 1): ceil(margin x in_rate / rate), clamped to [1, cells x channels]. Pure. */
int iq_chan_threads_needed(const iq_chan_load_t *load, double per_thread_msps_measured, double margin);
int iq_chan_cpu_bench_k(const iq_chan_load_t *load, int block, double seconds, int k, const int *cpus, int ncpus, iq_chan_bench_t *out); /* k threads (test/bench) */
int iq_chan_cpu_bench(const iq_chan_load_t *load, int block, double seconds, iq_chan_bench_t *out);

typedef struct {
  const shm_iq_ring_t *in;     /* the wideband ring: its producer handle (anonymous) or an attached handle; the channelizer takes its own view/attach */
  int block;                   /* 0 = IQ_CHAN_DEFAULT_BLOCK clipped to an eighth of the ring */
  const iq_chan_backend_t *backend; /* NULL = iq_chan_backend_select(load_hint) */
  const iq_chan_load_t *load_hint;  /* for the selection hook, may be NULL */
  size_t out_ring_budget_bytes; /* budget for ALL narrow rings together (0 = unlimited): the host-RAM plan's share; an entry that does not fit is refused */
  double out_target_ms;        /* duration wanted for a narrow ring, 0 = SHM_IQ_RING_NARROW_TARGET_MS */
  unsigned flags;              /* IQ_CHAN_F_* */
} iq_chan_cfg_t;
#define IQ_CHAN_F_NONE 0u

typedef struct iq_chan iq_chan_t;

typedef struct {
  uint64_t blocks, in_samples;       /* blocks processed, input samples per channel consumed */
  uint64_t overruns, discont, dropped_in, epochs;
  uint64_t work_ns, work_max_ns;     /* time spent in backend run() + ring writes */
  uint64_t run_ns;                   /* part of work_ns spent in the backend run() (the rest is the narrow-ring writes) */
  uint64_t lag_max;                  /* largest backlog (ring head - read position, samples) seen when a block was taken */
  uint64_t step_gap_max_ns;          /* longest time between two iq_chan_step() calls that processed or polled */
  unsigned n_cells;
  const char *backend;
  int threads, threads_needed;       /* running / needed to keep real time with margin; threads < threads_needed is a shortfall */
  double capacity_sps;               /* measured capacity (samples/s per channel) of the load at start with `threads` threads: the admission input */
} iq_chan_stats_t;

iq_chan_t *iq_chan_create(const iq_chan_cfg_t *cfg);                 /* NULL (errno) on failure */
void iq_chan_destroy(iq_chan_t *c);                                  /* stops the thread; narrow rings stay valid for their remaining views */
/* Entries. Return the entry id (>= 0) or -errno. Thread-safe; applied at the next block boundary when the thread runs (the call waits for it). */
int iq_chan_cell_add(iq_chan_t *c, const iq_chan_entry_t *e);
int iq_chan_cell_remove(iq_chan_t *c, int id);
int iq_chan_cell_retune(iq_chan_t *c, int id, double shift_hz, double centre_hz); /* same decim/passband; resets that entry's history, bumps its ring epoch */
shm_iq_ring_t *iq_chan_cell_ring(iq_chan_t *c, int id);              /* the narrow ring (producer handle); consumers call shm_iq_ring_view() on it */
/* Position: where reading starts (default: the newest data, aligned to 16). Seek is for replay and tests: reading from index `pos`. */
void iq_chan_seek(iq_chan_t *c, uint64_t pos);
uint64_t iq_chan_pos(const iq_chan_t *c);
/* Run. step() does one iteration on the calling thread: returns 1 when a block was processed, 0 when idle or after a skip, -1 on error. start() runs step() in a
 * thread, sleeping 20 us when idle; stop() joins it. Do not call step() while the thread runs. */
int iq_chan_step(iq_chan_t *c);
int iq_chan_start(iq_chan_t *c, const int *cpus, int ncpus);  /* derives k (see iq_chan.c); cpus[0] = channelizer thread, the rest = extra workers */
int iq_chan_set_threads(iq_chan_t *c, int k, const int *cpus, int ncpus); /* k backend threads for iq_chan_step() callers; not while the thread runs */
int iq_chan_start_k(iq_chan_t *c, const int *cpus, int ncpus, int k); /* explicit k (tests, benches) */         /* optional affinity list (the core plan's channelizer CPU); NULL = unpinned */
void iq_chan_stop(iq_chan_t *c);
void iq_chan_stats(const iq_chan_t *c, iq_chan_stats_t *st);
#endif
