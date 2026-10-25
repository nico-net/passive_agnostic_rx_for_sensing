/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */

#define _GNU_SOURCE
#include "common/utils/nr_bg_lane.h"
#include "common/utils/nr_cell.h"
#include "nr_csirs_disc.h"
#include "nr_csirs_disc_cell.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "PHY/TOOLS/nr_admit.h"
#include "PHY/TOOLS/nr_host_membudget.h"
#include "PHY/defs_nr_common.h"

const uint16_t nr_csirs_disc_periods[NR_CSIRS_DISC_NUM_PERIODS] = {4, 5, 8, 10, 16, 20, 32, 40, 64, 80, 160, 320, 640};

void *(*nr_csirs_disc_alloc_unified)(size_t bytes);
void (*nr_csirs_disc_free_unified)(void *ptr);
void (*nr_csirs_disc_process_hook)(nr_csirs_disc_ring_t *ring);
uint64_t csirs_disc_feed_drops;

typedef struct {
  pthread_mutex_t read_lock;
  bool unified_hook;
  pthread_mutex_t *entry_lock;
  uint64_t *serial;
  uint64_t epoch;
  const nr_csirs_disc_ring_t *source;
} ring_private_t;

typedef struct {
  nr_csirs_disc_ring_t ring;
  nr_csirs_disc_ring_t snapshot;
  pthread_t thread;
  pthread_mutex_t wake_lock;
  pthread_cond_t wake_cv;
  uint64_t wake_generation;
  int batch;
  int core;
  _Atomic bool running;
  bool stop;
  bool synchronization_ready;
  bool thread_started;
} disc_worker_t;

/* PER_CELL (mpci A7): one worker (rings, TD queue binding, wake state, thread) per cell. disc_worker_main runs bound to its cell (nr_cell_thread_create in start), so every
 * g_worker use inside the thread, in start/stop and in the TD drain/push resolves to the cell's slot. The struct is small: the grids are heap blocks (nr_csirs_disc_ring_init),
 * sized from the cell's N_RB / n_ant / host-RAM budget. Code that can run on a thread of ANOTHER cell (ring_push/ring_reset on a given ring) finds the owner from the ring. */
NR_CELL_STATIC(disc_worker_t, g_worker);
#define g_worker NR_CELL_REF(g_worker)

/* worker_cell_of: the cell whose worker ring is r, or -1 (a ring that is not a worker ring: snapshot, test rings). */
static int worker_cell_of(const nr_csirs_disc_ring_t *r)
{
  for (int c = 0; c < NR_CELL_MAX; ++c)
    if (r == &g_worker_cells[c].ring)
      return c;
  return -1;
}

/* Host-RAM ledger of the discovery instances. nr_host_membudget keeps ONE committed figure per module (commit replaces it): with several cells every cell records its
 * own bytes here and the module is committed with the sum, so the headroom seen by the next cell (and by the GrantWork pool) is real. PROCESS state, cell-indexed explicitly. */
static pthread_mutex_t cell_mem_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
  size_t host[NR_HOST_MEM_N];
  nr_csirs_disc_cell_mem_t mem;
} cell_ledger[NR_CELL_MAX];

/* U1's standalone test deliberately links no U5 object. */
extern const nr_csirs_disc_gpu_ops_t *nr_csirs_disc_gpu_load(void) __attribute__((weak));
extern bool nr_csirs_disc_pipeline_ready(void) __attribute__((weak));
extern void nr_csirs_disc_pipeline_init(const NR_DL_FRAME_PARMS *fp) __attribute__((weak));
extern int nr_csirs_disc_ring_capacity(void) __attribute__((weak));
extern void nr_host_membudget_commit(nr_host_mem_module_t m, size_t bytes, double scale) __attribute__((weak));

extern size_t nr_host_membudget_headroom(void) __attribute__((weak));
extern long nr_host_membudget_size(nr_host_mem_module_t m, int pct, size_t fixed, size_t per_unit, long lo, long hi) __attribute__((weak));
extern size_t nr_csirs_disc_td_bytes(const NR_DL_FRAME_PARMS *, int, int) __attribute__((weak));
extern void nr_csirs_disc_ring_host_cap(int capacity) __attribute__((weak));
extern int nr_csirs_disc_td_init(const NR_DL_FRAME_PARMS *, int, int) __attribute__((weak));
extern void nr_csirs_disc_td_free(void) __attribute__((weak));
extern void nr_csirs_disc_td_reset(void) __attribute__((weak));
extern void nr_csirs_disc_td_reset_cell(int cell) __attribute__((weak));
extern int nr_csirs_disc_td_pending(void) __attribute__((weak));
extern int nr_csirs_disc_td_drain(nr_csirs_disc_ring_t *) __attribute__((weak));

extern int nr_csirs_blind_rt_discovery_drain(void) __attribute__((weak));
static void worker_note_slot(int cell);

/* nr_csirs_disc_cell_host_commit: records `bytes` as the cell's holding in module m and commits the sum over all cells (N=1: exactly nr_host_membudget_commit(m, bytes, scale)).
 *   Callers: start/stop below and the elastic ring in nr_csirs_disc.c (cell = the calling thread's). */
void nr_csirs_disc_cell_host_commit(nr_host_mem_module_t m, size_t bytes, double scale)
{
  if (!nr_host_membudget_commit || m >= NR_HOST_MEM_N) return;
  const int cell = nr_cell_idx();
  pthread_mutex_lock(&cell_mem_lock);
  cell_ledger[cell].host[m] = bytes;
  size_t sum = 0;
  for (int c = 0; c < NR_CELL_MAX; ++c) sum += cell_ledger[c].host[m];
  nr_host_membudget_commit(m, sum, scale);
  pthread_mutex_unlock(&cell_mem_lock);
}

/* host_size: nr_host_membudget_size() for one cell: the largest k in [lo, hi] with fixed + per_unit * k within pct % of the headroom plus what THIS cell already holds
 *   (the budget's own `own` is the module sum, which would credit the other cells' bytes); commits the sum. 0 = below the floor (this cell holds nothing then). */
static long host_size(nr_host_mem_module_t m, int pct, size_t fixed, size_t per_unit, long lo, long hi)
{
  if (hi < 1) return 0;
  pthread_mutex_lock(&cell_mem_lock);
  const size_t own = cell_ledger[nr_cell_idx()].host[m];
  pthread_mutex_unlock(&cell_mem_lock);
  const long k = nr_host_membudget_fit_pct(nr_host_membudget_headroom(), own, pct, fixed, per_unit, lo, hi);
  nr_csirs_disc_cell_host_commit(m, k ? fixed + per_unit * (size_t)k : 0, (double)k / (double)hi);
  return k;
}

/* nr_csirs_disc_cell_note_gpu: the device bytes the cell's ring commits (pipeline_init in nr_csirs_disc.c; the G1 ledger reads them through nr_csirs_disc_cell_mem). */
void nr_csirs_disc_cell_note_gpu(size_t bytes)
{
  pthread_mutex_lock(&cell_mem_lock);
  cell_ledger[nr_cell_idx()].mem.gpu_bytes = bytes;
  pthread_mutex_unlock(&cell_mem_lock);
}

/* nr_csirs_disc_enabled: true; CSI-RS blind discovery is always on (kept for the callers in the PHY receive path, the PDSCH queue and the blind monitor). */
bool nr_csirs_disc_enabled(void)
{
  return true;
}

/* env_core: returns the CPU core for the discovery worker from ISAC_CSIRS_DISC_CORE, or -1 (no pinning) when unset or invalid.
 *   Caller: nr_csirs_disc_start().
 */
static int env_core(void)
{
  const char *text = getenv("ISAC_CSIRS_DISC_CORE");
  if (text == NULL || *text == '\0')
    return -1;
  char *end = NULL;
  errno = 0;
  const long value = strtol(text, &end, 10);
  return errno == 0 && end != text && *end == '\0' && value >= 0 && value <= INT32_MAX ? (int)value : -1;
}

/* nr_csirs_disc_ring_init: allocates a CSI-RS IQ ring of capacity slots (n_ant antennas, n_rb RBs, 14 symbols each) plus its metadata, locks and serial numbers.
 *   The grid is zeroed and 64-byte aligned; with unified=true the allocator hook is used for GPU-visible memory. Returns 0, or -1 on bad arguments or allocation failure (everything freed).
 *   Callers: nr_csirs_disc_start() (worker ring and snapshot ring) and the tests.
 */
int nr_csirs_disc_ring_init(nr_csirs_disc_ring_t *r, int n_ant, int n_rb, int slots_per_frame, int capacity, bool unified)
{
  if (r == NULL || n_ant < 1 || n_ant > NR_CSIRS_DISC_MAX_ANT || n_rb < 1 || n_rb > NR_CSIRS_DISC_MAX_RB
      || slots_per_frame < 1 || capacity < 1)
    return -1;

  memset(r, 0, sizeof(*r));
  ring_private_t *state = calloc(1, sizeof(*state));
  if (state == NULL || pthread_mutex_init(&state->read_lock, NULL) != 0) {
    free(state);
    return -1;
  }

  r->n_ant = n_ant;
  r->n_rb = n_rb;
  r->slots_per_frame = slots_per_frame;
  r->capacity = capacity;
  r->slot_stride = (size_t)n_ant * NR_CSIRS_DISC_NSYM * (size_t)(24 * n_rb);
  const size_t grid_bytes = (size_t)capacity * r->slot_stride * sizeof(*r->grid_base);
  if (grid_bytes / sizeof(*r->grid_base) / r->slot_stride != (size_t)capacity) {
    pthread_mutex_destroy(&state->read_lock);
    free(state);
    memset(r, 0, sizeof(*r));
    return -1;
  }
  const size_t aligned_bytes = (grid_bytes + 63u) & ~(size_t)63u;
  state->unified_hook = unified && nr_csirs_disc_alloc_unified != NULL;
  r->grid_base = state->unified_hook ? nr_csirs_disc_alloc_unified(aligned_bytes) : aligned_alloc(64, aligned_bytes);
  r->meta = calloc((size_t)capacity, sizeof(*r->meta));
  if (r->grid_base == NULL || r->meta == NULL) {
    if (r->grid_base != NULL) {
      if (state->unified_hook && nr_csirs_disc_free_unified != NULL)
        nr_csirs_disc_free_unified(r->grid_base);
      else
        free(r->grid_base);
    }
    free(r->meta);
    pthread_mutex_destroy(&state->read_lock);
    free(state);
    memset(r, 0, sizeof(*r));
    return -1;
  }
  memset(r->grid_base, 0, grid_bytes);
  state->entry_lock = calloc(capacity, sizeof(*state->entry_lock));
  state->serial = calloc(capacity, sizeof(*state->serial));
  if (!state->entry_lock || !state->serial) {
    free(state->entry_lock); free(state->serial);
    if (state->unified_hook && nr_csirs_disc_free_unified) nr_csirs_disc_free_unified(r->grid_base);
    else free(r->grid_base);
    free(r->meta); pthread_mutex_destroy(&state->read_lock); free(state); memset(r,0,sizeof(*r)); return -1;
  }
  for (int i=0;i<capacity;++i) pthread_mutex_init(&state->entry_lock[i],NULL);
  r->private_state = state;
  return 0;
}

/* nr_csirs_disc_ring_free: frees a ring created by nr_csirs_disc_ring_init(): grid (unified or host), device grid, metadata, locks and private state.
 *   NULL-safe. Callers: nr_csirs_disc_start() (failure path), nr_csirs_disc_stop() and the tests.
 */
void nr_csirs_disc_ring_free(nr_csirs_disc_ring_t *r)
{
  if (r == NULL)
    return;
  ring_private_t *state = r->private_state;
  if (r->grid_base != NULL) {
    if (state != NULL && state->unified_hook && nr_csirs_disc_free_unified != NULL)
      nr_csirs_disc_free_unified(r->grid_base);
    else
      free(r->grid_base);
  }
  if(r->device_grid) {
    const nr_csirs_disc_gpu_ops_t *gpu=nr_csirs_disc_gpu_load();
    if(gpu && gpu->device_free) gpu->device_free(r->device_grid);
  }
  free(r->meta);
  if (state != NULL) {
    for (int i=0;i<r->capacity;++i) pthread_mutex_destroy(&state->entry_lock[i]);
    free(state->entry_lock); free(state->serial);
    pthread_mutex_destroy(&state->read_lock);
    free(state);
  }
  memset(r, 0, sizeof(*r));
}

/* nr_csirs_disc_ring_resize: see nr_csirs_disc.h. Allocation first (host grid by the ring's own allocator, metadata, entry locks, serials, device grid), then the swap under the
 *   read lock and every entry lock of the old arrays (no producer or reader is inside), then the old arrays are released. */
int nr_csirs_disc_ring_resize(nr_csirs_disc_ring_t *r, int capacity)
{
  ring_private_t *state = r != NULL ? r->private_state : NULL;
  if (state == NULL || capacity < 1) return -1;
  if (capacity == r->capacity) return 0;
  const size_t grid_bytes = (size_t)capacity * r->slot_stride * sizeof(*r->grid_base);
  const size_t aligned_bytes = (grid_bytes + 63u) & ~(size_t)63u;
  int16_t *grid = state->unified_hook ? nr_csirs_disc_alloc_unified(aligned_bytes) : aligned_alloc(64, aligned_bytes);
  nr_csirs_disc_slot_meta_t *meta = calloc((size_t)capacity, sizeof(*meta));
  pthread_mutex_t *locks = calloc((size_t)capacity, sizeof(*locks));
  uint64_t *serial = calloc((size_t)capacity, sizeof(*serial));
  int16_t *device_grid = NULL;
  const nr_csirs_disc_gpu_ops_t *gpu = r->device_grid && nr_csirs_disc_gpu_load ? nr_csirs_disc_gpu_load() : NULL;
  if (r->device_grid) {
    if (gpu && gpu->device_alloc) device_grid = gpu->device_alloc((size_t)capacity * r->slot_stride * 2);
    if (!device_grid) goto fail;
  }
  if (!grid || !meta || !locks || !serial) goto fail;
  memset(grid, 0, grid_bytes);
  for (int i = 0; i < capacity; ++i) pthread_mutex_init(&locks[i], NULL);

  pthread_mutex_lock(&state->read_lock);
  const int old_capacity = r->capacity;
  for (int i = 0; i < old_capacity; ++i) pthread_mutex_lock(&state->entry_lock[i]);
  int16_t *old_grid = r->grid_base, *old_device = r->device_grid;
  nr_csirs_disc_slot_meta_t *old_meta = r->meta;
  pthread_mutex_t *old_locks = state->entry_lock;
  uint64_t *old_serial = state->serial;
  r->grid_base = grid; r->meta = meta; r->device_grid = device_grid; r->capacity = capacity; r->device_valid = false;
  state->entry_lock = locks; state->serial = serial;
  __atomic_store_n(&r->written, 0, __ATOMIC_RELEASE);
  __atomic_add_fetch(&state->epoch, 1, __ATOMIC_RELEASE);
  for (int i = 0; i < old_capacity; ++i) {
    pthread_mutex_unlock(&old_locks[i]);
    pthread_mutex_destroy(&old_locks[i]);
  }
  pthread_mutex_unlock(&state->read_lock);
  if (state->unified_hook && nr_csirs_disc_free_unified) nr_csirs_disc_free_unified(old_grid);
  else free(old_grid);
  if (old_device) {
    const nr_csirs_disc_gpu_ops_t *g = nr_csirs_disc_gpu_load ? nr_csirs_disc_gpu_load() : NULL;
    if (g && g->device_free) g->device_free(old_device);
  }
  free(old_meta); free(old_locks); free(old_serial);
  return 0;
fail:
  if (grid) {
    if (state->unified_hook && nr_csirs_disc_free_unified) nr_csirs_disc_free_unified(grid);
    else free(grid);
  }
  if (device_grid && gpu && gpu->device_free) gpu->device_free(device_grid);
  free(meta); free(locks); free(serial);
  return -1;
}

/* nr_csirs_disc_worker_resizable: the worker ring exists and the time-domain drain feeds it (the snapshot ring, which has to keep the capacity of its source, is then unused). */
bool nr_csirs_disc_worker_resizable(void)
{
  return atomic_load_explicit(&g_worker.running, memory_order_acquire) && nr_csirs_disc_td_drain != NULL;
}

/* nr_csirs_disc_ring_read_lock: takes the ring's read lock (serialises readers against reset and push), returns true on success.
 *   Pair with nr_csirs_disc_ring_read_unlock(). No production caller; used by tests/nr_csirs_disc_feed_test.cc.
 */
bool nr_csirs_disc_ring_read_lock(nr_csirs_disc_ring_t *r)
{
  ring_private_t *state = r != NULL ? r->private_state : NULL;
  return state != NULL && pthread_mutex_lock(&state->read_lock) == 0;
}

/* nr_csirs_disc_ring_read_unlock: releases the ring's read lock taken by nr_csirs_disc_ring_read_lock(); a NULL ring or missing state is a no-op.
 *   No production caller; used by tests/nr_csirs_disc_feed_test.cc.
 */
void nr_csirs_disc_ring_read_unlock(nr_csirs_disc_ring_t *r)
{
  ring_private_t *state = r != NULL ? r->private_state : NULL;
  if (state != NULL)
    pthread_mutex_unlock(&state->read_lock);
}

/* nr_csirs_disc_ring_reset: empties the ring for a new sample epoch: clears the metadata, resets the write counter and bumps the epoch, while holding every entry lock.
 *   Also resets the TD front end when this is the worker ring. Caller: epoch_listener() in nr_csirs_disc.c (under the pipeline lock).
 */
void nr_csirs_disc_ring_reset(nr_csirs_disc_ring_t *r)
{
  ring_private_t *state=r?r->private_state:NULL;
  if(!state) return;
  const int wc = worker_cell_of(r);
  if (wc >= 0 && nr_csirs_disc_td_reset_cell) nr_csirs_disc_td_reset_cell(wc);
  pthread_mutex_lock(&state->read_lock);
  for(int i=0;i<r->capacity;++i) pthread_mutex_lock(&state->entry_lock[i]);
  memset(r->meta,0,r->capacity*sizeof(*r->meta));
  memset(state->serial,0,r->capacity*sizeof(*state->serial));
  __atomic_store_n(&r->written,0,__ATOMIC_RELEASE);
  __atomic_add_fetch(&state->epoch,1,__ATOMIC_RELEASE);
  for(int i=r->capacity-1;i>=0;--i) pthread_mutex_unlock(&state->entry_lock[i]);
  pthread_mutex_unlock(&state->read_lock);
}

/* nr_csirs_disc_ring_snapshot_current: true when the snapshot ring was taken in the same epoch as its source ring (no reset happened since), or when it has no source.
 *   Read-only. Caller: nr_csirs_disc_process() before a batch is processed.
 */
bool nr_csirs_disc_ring_snapshot_current(const nr_csirs_disc_ring_t *r)
{
  const ring_private_t *state=r?r->private_state:NULL;
  if(!state || !state->source) return true;
  const ring_private_t *source=state->source->private_state;
  return state->epoch==__atomic_load_n(&source->epoch,__ATOMIC_ACQUIRE);
}

/* nr_csirs_disc_ring_snapshot: copies the committed slots behind the producer cursor of ring r into snapshot ring dst (incrementally: only slots newer than dst->written).
 *   On a GPU-resident ring it first reads the device grid back. Each entry is copied under its entry lock; a slot overwritten during the copy makes it fail (-1).
 *   Returns 0 on success. Caller: disc_worker_main() when the TD drain is not used.
 */
int nr_csirs_disc_ring_snapshot(nr_csirs_disc_ring_t *r, nr_csirs_disc_ring_t *dst)
{
  ring_private_t *state = r ? r->private_state : NULL;
  if (!state || !dst || !dst->private_state || dst->capacity != r->capacity || dst->slot_stride != r->slot_stride) return -1;
  uint64_t epoch=__atomic_load_n(&state->epoch,__ATOMIC_ACQUIRE);
  uint64_t end = __atomic_load_n(&r->written, __ATOMIC_ACQUIRE);
  if(r->device_valid) {
    const nr_csirs_disc_gpu_ops_t *gpu=nr_csirs_disc_gpu_load?nr_csirs_disc_gpu_load():NULL;
    if(!gpu || !gpu->readback) return -1;
    pthread_mutex_lock(&state->read_lock);
    int rc=gpu->readback(r);
    pthread_mutex_unlock(&state->read_lock);
    if(rc<0) return -1;
  }
  ring_private_t *copy=dst->private_state;
  if (epoch!=copy->epoch || end < dst->written) {dst->written=0;memset(dst->meta,0,dst->capacity*sizeof(*dst->meta));}
  uint64_t first = dst->written;
  if (end-first > (uint64_t)r->capacity) first=end-r->capacity;
  for (uint64_t w=first;w<end;++w) {
    int i=w%r->capacity;
    pthread_mutex_lock(&state->entry_lock[i]);
    if (state->serial[i] != w+1) {pthread_mutex_unlock(&state->entry_lock[i]);return -1;}
    memcpy(dst->grid_base+i*dst->slot_stride,r->grid_base+i*r->slot_stride,r->slot_stride*sizeof(int16_t));
    dst->meta[i]=r->meta[i];
    pthread_mutex_unlock(&state->entry_lock[i]);
  }
  if(epoch!=__atomic_load_n(&state->epoch,__ATOMIC_ACQUIRE)) return -1;
  copy->source=r;copy->epoch=epoch;dst->written=end;
  return 0;
}

/* nr_csirs_disc_ring_write_begin: reserves count consecutive slots of the ring for the TD producer (unified-memory rings only), locking the read lock and the entries.
 *   Returns the first reserved index, or -1 when the ring is not unified or count is out of range. Must be followed by nr_csirs_disc_ring_write_end().
 *   Callers: nr_csirs_disc_td.c (TD drain) and the tests.
 */
int nr_csirs_disc_ring_write_begin(nr_csirs_disc_ring_t *r, int count)
{
  ring_private_t *s=r?r->private_state:NULL;
  if(!s || !s->unified_hook || count<1 || count>r->capacity) return -1;
  pthread_mutex_lock(&s->read_lock);
  int first=r->written%r->capacity;
  for(int i=0;i<count;++i) pthread_mutex_lock(&s->entry_lock[(first+i)%r->capacity]);
  return first;
}

/* nr_csirs_disc_ring_write_end: publishes the slots reserved by nr_csirs_disc_ring_write_begin(): writes their metadata and serial numbers, then advances the write counter and unlocks.
 *   With meta == NULL the reservation is released without publishing. Caller: nr_csirs_disc_td.c (TD drain).
 */
void nr_csirs_disc_ring_write_end(nr_csirs_disc_ring_t *r, int count, const nr_csirs_disc_slot_meta_t *meta)
{
  ring_private_t *s=r->private_state;
  uint64_t start=r->written;
  for(int i=0;i<count;++i) {
    int idx=(start+i)%r->capacity;
    r->meta[idx]=meta?meta[i]:(nr_csirs_disc_slot_meta_t){0};
    s->serial[idx]=meta?start+i+1:0;
    pthread_mutex_unlock(&s->entry_lock[idx]);
  }
  if(meta) __atomic_store_n(&r->written,start+count,__ATOMIC_RELEASE);
  pthread_mutex_unlock(&s->read_lock);
}

/* nr_csirs_disc_ring_push: copies one slot of the 14 symbols of all antennas from the FEP buffers (rxdataF, with the carrier offset wrap) into the next ring entry.
 *   Non-blocking: uses trylock on the read and entry locks and returns -1 (counted as a feed drop) when busy. Wakes the worker every batch slots when this is the worker ring.
 *   Callers: nr_csirs_disc_td.c (TD front end) and the tests. Returns the entry index, or -1.
 */
int nr_csirs_disc_ring_push(nr_csirs_disc_ring_t *r,
                            const int16_t *const *rxdataF,
                            int ofdm_symbol_size,
                            int first_carrier_offset,
                            uint32_t abs_slot,
                            uint16_t frame,
                            uint8_t slot)
{
  ring_private_t *state = r != NULL ? r->private_state : NULL;
  const int grid_sc = r != NULL ? 12 * r->n_rb : 0;
  if (state == NULL || rxdataF == NULL || ofdm_symbol_size < grid_sc || first_carrier_offset < 0
      || first_carrier_offset >= ofdm_symbol_size)
    return -1;
  if (pthread_mutex_trylock(&state->read_lock) != 0) {
    __atomic_fetch_add(&csirs_disc_feed_drops, 1, __ATOMIC_RELAXED);
    return -1;
  }

  const int index = (int)(r->written % (uint64_t)r->capacity);
  if (pthread_mutex_trylock(&state->entry_lock[index]) != 0) {
    pthread_mutex_unlock(&state->read_lock);
    __atomic_fetch_add(&csirs_disc_feed_drops, 1, __ATOMIC_RELAXED);
    return -1;
  }
  const int first_count = grid_sc < ofdm_symbol_size - first_carrier_offset ? grid_sc : ofdm_symbol_size - first_carrier_offset;
  const int second_count = grid_sc - first_count;
  for (int ant = 0; ant < r->n_ant; ++ant) {
    if (rxdataF[ant] == NULL) {
      pthread_mutex_unlock(&state->entry_lock[index]);
      pthread_mutex_unlock(&state->read_lock);
      return -1;
    }
    for (int sym = 0; sym < NR_CSIRS_DISC_NSYM; ++sym) {
      int16_t *dst = nr_csirs_disc_ring_sym(r, index, ant, sym);
      const int16_t *src = rxdataF[ant] + 2 * ((size_t)sym * ofdm_symbol_size + first_carrier_offset);
      memcpy(dst, src, (size_t)first_count * 2 * sizeof(*dst));
      if (second_count > 0) {
        src = rxdataF[ant] + 2 * (size_t)sym * ofdm_symbol_size;
        memcpy(dst + 2 * first_count, src, (size_t)second_count * 2 * sizeof(*dst));
      }
    }
  }
  r->meta[index] = (nr_csirs_disc_slot_meta_t){.abs_slot = abs_slot, .frame = frame, .slot = slot, .valid = 1};
  state->serial[index] = r->written + 1;
  __atomic_add_fetch(&r->written, 1, __ATOMIC_RELEASE);
  pthread_mutex_unlock(&state->entry_lock[index]);
  pthread_mutex_unlock(&state->read_lock);
  const int wc = worker_cell_of(r);
  if (wc >= 0)
    worker_note_slot(wc);
  return index;
}

int nr_csirs_disc_should_run(int drained, int *deferred, int admitted)
{
  if (drained <= 0 && !*deferred) return 0;
  *deferred = !admitted;
  return admitted;
}

/* disc_worker_main: thread body of the CSI-RS discovery worker: waits for a batch of slots, drains the TD front end, and runs nr_csirs_disc_process() (through the hook) on the ring or its snapshot.
 *   Also drains the blind search's discovery queue (nr_csirs_blind_rt_discovery_drain) each time it wakes. Wakes every 5 ms at least. Runs on core ISAC_CSIRS_DISC_CORE with SCHED_OTHER.
 *   Started by nr_csirs_disc_start() and stopped by nr_csirs_disc_stop().
 */
static void *disc_worker_main(void *unused)
{
  (void)unused;
  pthread_setname_np(pthread_self(), "csirsDisc");
  if (g_worker.core >= 0 && g_worker.core < CPU_SETSIZE) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(g_worker.core, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
  }
  nr_bg_lane_enter("ISAC_CSIRS_DISC_CORE");

  pthread_mutex_lock(&g_worker.wake_lock);
  uint64_t seen = 0;
  int deferred = 0; /* a drained batch is waiting in the ring because the critical lane was busy */
  while (!g_worker.stop) {
    while (!g_worker.stop && seen == __atomic_load_n(&g_worker.wake_generation, __ATOMIC_ACQUIRE)
           && (!nr_csirs_disc_td_pending || nr_csirs_disc_td_pending() < g_worker.batch)) {
      struct timespec until;
      clock_gettime(CLOCK_REALTIME, &until);
      until.tv_nsec += 5000000;
      if (until.tv_nsec >= 1000000000) { ++until.tv_sec; until.tv_nsec -= 1000000000; }
      if(pthread_cond_timedwait(&g_worker.wake_cv, &g_worker.wake_lock, &until)==ETIMEDOUT) break;
    }
    if (g_worker.stop)
      break;
    seen = __atomic_load_n(&g_worker.wake_generation, __ATOMIC_ACQUIRE);
    pthread_mutex_unlock(&g_worker.wake_lock);
    if(nr_csirs_blind_rt_discovery_drain) nr_csirs_blind_rt_discovery_drain();
    int drained=nr_csirs_disc_td_drain?nr_csirs_disc_td_drain(&g_worker.ring):0;
    seen = __atomic_load_n(&g_worker.wake_generation, __ATOMIC_ACQUIRE);
    void (*hook)(nr_csirs_disc_ring_t *) = nr_csirs_disc_process_hook;
    const uint64_t t_adm = nr_admit_now_ns();
    const int work = drained > 0 || deferred || (!nr_csirs_disc_td_drain && g_worker.ring.written != g_worker.snapshot.written);
    const int admitted = !work || nr_admit_try(NR_ADMIT_CSIRS, t_adm, 0, 0); /* idle wake-ups are not jobs */
    if(nr_csirs_disc_should_run(drained, &deferred, admitted)) {
      /* TD mode has a single FD writer: this worker. The pipeline mutex
       * serializes processing with epoch reset, so no second IQ copy is needed. */
      if(hook) hook(&g_worker.ring);
      nr_admit_done(NR_ADMIT_CSIRS, nr_admit_now_ns() - t_adm);
    } else if (drained <= 0 && !deferred && admitted && !nr_csirs_disc_td_drain && g_worker.ring.written!=g_worker.snapshot.written && nr_csirs_disc_ring_snapshot(&g_worker.ring, &g_worker.snapshot) == 0) {
      if(hook) hook(&g_worker.snapshot);
      nr_admit_done(NR_ADMIT_CSIRS, nr_admit_now_ns() - t_adm);
    }
    pthread_mutex_lock(&g_worker.wake_lock);
  }
  pthread_mutex_unlock(&g_worker.wake_lock);
  return NULL;
}

/* nr_csirs_disc_start: starts the discovery pipeline for this UE: sets up the worker ring and snapshot ring (GPU-backed when available), the TD front end, and the worker thread.
 *   Returns 0 when discovery is off or already running; -1 when the frame parameters are unsupported or any setup step fails (everything is then freed).
 *   Caller: executables/nr-uesoftmodem.c (receiver set-up, after the discovery UE is built). Calls nr_csirs_disc_pipeline_init() and checks nr_csirs_disc_pipeline_ready().
 */
int nr_csirs_disc_start(const NR_DL_FRAME_PARMS *fp, int n_ant)
{
  if (atomic_load_explicit(&g_worker.running, memory_order_acquire))
    return 0;
  if (fp == NULL || fp->symbols_per_slot != NR_CSIRS_DISC_NSYM)
    return -1;

  /* U5 loads the CUDA backend here, before the ring chooses its allocator, and installs
   * the process hook. The weak edge keeps the U1-only fixture self-contained. */
  if (nr_csirs_disc_pipeline_init)
    nr_csirs_disc_pipeline_init(fp);
  if(nr_csirs_disc_pipeline_ready && !nr_csirs_disc_pipeline_ready()) return -1;

  memset(&g_worker, 0, sizeof(g_worker));
  g_worker.batch = NR_CSIRS_DISC_DEFAULT_BATCH;
  g_worker.core = env_core();
  if (pthread_mutex_init(&g_worker.wake_lock, NULL) != 0)
    return -1;
  if (pthread_cond_init(&g_worker.wake_cv, NULL) != 0) {
    pthread_mutex_destroy(&g_worker.wake_lock);
    return -1;
  }
  g_worker.synchronization_ready = true;
  int capacity = nr_csirs_disc_ring_capacity ? nr_csirs_disc_ring_capacity() : NR_CSIRS_DISC_DEFAULT_CAPACITY; /* T6b: from the memory budget */
  const bool snapshot_used = nr_csirs_disc_td_init == NULL;
  /* Host-RAM budget (nr_host_membudget.h): the host grid of the ring (plus the snapshot ring when used) takes at most 30 % of the headroom; floor 160 slots
   * (a 160-slot CSI-RS period; longer periods then fold less evidence per batch). Below the floor discovery does not start (logged), the receiver runs on. */
  if (nr_host_membudget_headroom && nr_host_membudget_commit) {
    const size_t per_slot = (size_t)n_ant * NR_CSIRS_DISC_NSYM * (size_t)(24 * fp->N_RB_DL) * sizeof(int16_t) * (snapshot_used ? 2 : 1);
    const int lo = capacity < 160 ? capacity : 160;
    const long k = host_size(NR_HOST_MEM_CSIRS, 30, 0, per_slot, lo, capacity);
    if (k == 0) {
      fprintf(stderr, "[PHY] SENSING: HOST_MEM CSIRS_DISC ring: below the %d-slot floor (%zu MB); slot discovery off\n", lo, ((size_t)lo * per_slot) >> 20);
      goto fail;
    }
    if (k != capacity) {
      fprintf(stderr, "[PHY] SENSING: HOST_MEM CSIRS_DISC ring %d -> %ld slots (%zu KB each, host-RAM budget)\n", capacity, k, per_slot >> 10);
      if (nr_csirs_disc_ring_host_cap) nr_csirs_disc_ring_host_cap((int)k);
    }
    capacity = (int)k;
  }
  if (nr_csirs_disc_ring_init(&g_worker.ring, n_ant, fp->N_RB_DL, fp->slots_per_frame, capacity, true) != 0)
    goto fail;
  const nr_csirs_disc_gpu_ops_t *gpu=nr_csirs_disc_gpu_load?nr_csirs_disc_gpu_load():NULL;
  if(gpu && gpu->device_alloc) {
    g_worker.ring.device_grid=gpu->device_alloc((size_t)capacity*g_worker.ring.slot_stride*2);
    if(!g_worker.ring.device_grid) goto fail;
  }
  /* The snapshot ring only serves the frequency-domain feed (no time-domain drain): with the TD front end it would be capacity x slot_stride x 2 bytes of dead host RAM
   * (470 MB at 273 PRB / 4 RX / 640 slots, 161 MB at 220), so it is not allocated then. */
  if (snapshot_used && nr_csirs_disc_ring_init(&g_worker.snapshot, n_ant, fp->N_RB_DL, fp->slots_per_frame, capacity, false) != 0)
    goto fail;
  int td_capacity = 1024; /* time-domain slot queue (512 ms at 30 kHz): 1138 MB at 273 PRB / 4 RX */
  if (nr_csirs_disc_td_init && nr_host_membudget_headroom && nr_host_membudget_commit && nr_csirs_disc_td_bytes) {
    /* shrinkable: a shorter queue overflows sooner under a discovery backlog (counted drops, the RT path never waits); floor 256 slots, 60 % of the headroom (discovery before the GrantWork pool) */
    /* per slot averaged over the full queue (the pool is rounded to 2 MB, so a one-slot difference can be 0); f0 = the capacity-independent part (batch slots + FFT buffers) */
    const size_t b1 = nr_csirs_disc_td_bytes(fp, n_ant, 1), per_slot = (nr_csirs_disc_td_bytes(fp, n_ant, td_capacity + 1) - b1) / (size_t)td_capacity;
    const size_t f0 = b1 > per_slot ? b1 - per_slot : 0;
    const long k = host_size(NR_HOST_MEM_TD, 60, f0 + (2u << 20), per_slot, 256, td_capacity);
    if (k == 0) {
      fprintf(stderr, "[PHY] SENSING: HOST_MEM CSIRS_DISC time-domain queue: below the 256-slot floor (%zu MB); slot discovery off\n", (f0 + 256 * per_slot) >> 20);
      goto fail;
    }
    if (k != td_capacity) fprintf(stderr, "[PHY] SENSING: HOST_MEM CSIRS_DISC time-domain queue %d -> %ld slots (%zu KB each, host-RAM budget)\n", td_capacity, k, per_slot >> 10);
    td_capacity = (int)k;
    nr_csirs_disc_cell_host_commit(NR_HOST_MEM_TD, nr_csirs_disc_td_bytes(fp, n_ant, td_capacity), (double)td_capacity / 1024.0);
  }
  if (nr_csirs_disc_td_init && nr_csirs_disc_td_init(fp, n_ant, td_capacity) != 0)
    goto fail;
  pthread_attr_t attr;
  if (pthread_attr_init(&attr) != 0)
    goto fail;
  struct sched_param sched_param = {0};
  const bool attr_ready = pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED) == 0
                          && pthread_attr_setschedpolicy(&attr, SCHED_OTHER) == 0
                          && pthread_attr_setschedparam(&attr, &sched_param) == 0;
  /* the worker serves this cell only: it is created bound to it (nr_cell_idx() inside the thread = this cell) */
  const int thread_result = attr_ready ? nr_cell_thread_create(&g_worker.thread, &attr, disc_worker_main, NULL, nr_cell_idx()) : -1;
  pthread_attr_destroy(&attr);
  if (thread_result != 0)
    goto fail;
  g_worker.thread_started = true;
  atomic_store_explicit(&g_worker.running, true, memory_order_release);
  {
    nr_csirs_disc_cell_mem_t *m = &cell_ledger[nr_cell_idx()].mem;
    pthread_mutex_lock(&cell_mem_lock);
    m->active = true;
    m->n_rb = fp->N_RB_DL;
    m->n_ant = n_ant;
    m->ring_slots = capacity;
    m->td_slots = nr_csirs_disc_td_init ? td_capacity : 0;
    m->ring_host_bytes = (size_t)capacity * g_worker.ring.slot_stride * sizeof(int16_t) * (snapshot_used ? 2 : 1);
    m->td_host_bytes = nr_csirs_disc_td_init && nr_csirs_disc_td_bytes ? nr_csirs_disc_td_bytes(fp, n_ant, td_capacity) : 0;
    pthread_mutex_unlock(&cell_mem_lock);
  }
  return 0;

fail:
  if (nr_host_membudget_commit) { nr_csirs_disc_cell_host_commit(NR_HOST_MEM_CSIRS, 0, 0.0); nr_csirs_disc_cell_host_commit(NR_HOST_MEM_TD, 0, 0.0); }
  if (nr_csirs_disc_td_free) nr_csirs_disc_td_free();
  nr_csirs_disc_ring_free(&g_worker.snapshot);
  nr_csirs_disc_ring_free(&g_worker.ring);
  pthread_cond_destroy(&g_worker.wake_cv);
  pthread_mutex_destroy(&g_worker.wake_lock);
  memset(&g_worker, 0, sizeof(g_worker));
  pthread_mutex_lock(&cell_mem_lock);
  memset(&cell_ledger[nr_cell_idx()].mem, 0, sizeof(cell_ledger[0].mem));
  pthread_mutex_unlock(&cell_mem_lock);
  return -1;
}

/* nr_csirs_disc_stop: stops the discovery worker (sets stop, wakes it, joins) and frees the TD front end and both rings; idempotent when not running.
 *   Callers: executables/nr-uesoftmodem.c (teardown and the start-failure path).
 */
void nr_csirs_disc_stop(void)
{
  if (!atomic_load_explicit(&g_worker.running, memory_order_acquire))
    return;
  pthread_mutex_lock(&g_worker.wake_lock);
  g_worker.stop = true;
  pthread_cond_signal(&g_worker.wake_cv);
  pthread_mutex_unlock(&g_worker.wake_lock);
  if (g_worker.thread_started)
    pthread_join(g_worker.thread, NULL);
  atomic_store_explicit(&g_worker.running, false, memory_order_release);
  if (nr_csirs_disc_td_free) nr_csirs_disc_td_free();
  nr_csirs_disc_ring_free(&g_worker.snapshot);
  nr_csirs_disc_ring_free(&g_worker.ring);
  if (g_worker.synchronization_ready) {
    pthread_cond_destroy(&g_worker.wake_cv);
    pthread_mutex_destroy(&g_worker.wake_lock);
  }
  memset(&g_worker, 0, sizeof(g_worker));
  /* the cell's bytes go back to the ledger; with other cells running the module stays committed with their sum (the last cell leaves the commitment as today) */
  {
    const int cell = nr_cell_idx();
    pthread_mutex_lock(&cell_mem_lock);
    memset(&cell_ledger[cell].mem, 0, sizeof(cell_ledger[cell].mem));
    size_t other[NR_HOST_MEM_N] = {0};
    bool any = false;
    for (int m = 0; m < NR_HOST_MEM_N; ++m) {
      cell_ledger[cell].host[m] = 0;
      for (int c = 0; c < NR_CELL_MAX; ++c) other[m] += cell_ledger[c].host[m];
      any |= other[m] != 0;
    }
    if (any && nr_host_membudget_commit)
      for (int m = NR_HOST_MEM_CSIRS; m <= NR_HOST_MEM_TD; ++m)
        nr_host_membudget_commit((nr_host_mem_module_t)m, other[m], 1.0);
    pthread_mutex_unlock(&cell_mem_lock);
  }
}

/* nr_csirs_disc_running: true while the discovery worker is running (started and not stopped).
 *   Callers: phy_procedures_nr_ue.c (PHY receive path, together with nr_csirs_disc_enabled()) and the feed itself.
 */
bool nr_csirs_disc_running(void)
{
  return atomic_load_explicit(&g_worker.running, memory_order_acquire);
}

/* nr_csirs_disc_worker_ring: returns the worker's IQ ring while discovery runs, or NULL. The discovery pipeline uses it for the epoch reset and the batch processing.
 *   Caller: epoch_listener() in nr_csirs_disc.c.
 */
nr_csirs_disc_ring_t *nr_csirs_disc_worker_ring(void)
{
  return nr_csirs_disc_running() ? &g_worker.ring : NULL;
}

/* worker_note_slot: called after each pushed worker-ring slot: when the write counter hits a multiple of the batch size, bumps the wake generation and signals the worker.
 *   Does nothing when the worker is not running. Caller: nr_csirs_disc_ring_push() on the worker ring.
 */
static void worker_note_slot(int cell)
{
  disc_worker_t *w = &g_worker_cells[cell];
  if (!atomic_load_explicit(&w->running, memory_order_acquire))
    return;
  if ((w->ring.written % (uint64_t)w->batch) == 0) {
    __atomic_add_fetch(&w->wake_generation, 1, __ATOMIC_RELEASE);
    pthread_cond_signal(&w->wake_cv);
  }
}

/* nr_csirs_disc_cell_prealloc: see nr_csirs_disc_cell.h. The calling thread is bound to `cell` for the call and restored afterwards, so the allocation (rings, TD queue, GPU
 *   ring) and the worker thread belong to the cell. Called once per active cell before the RX fence by the cell activation (A8). */
int nr_csirs_disc_cell_prealloc(int cell, const NR_DL_FRAME_PARMS *fp, int n_ant)
{
  if (cell < 0 || cell >= NR_CELL_MAX) return -1;
  const int prev = nr_cell_idx();
  nr_cell_bind(cell);
  const int rc = nr_csirs_disc_start(fp, n_ant);
  nr_cell_bind(prev);
  return rc;
}

/* nr_csirs_disc_cell_release: stops the discovery instance of `cell` (see nr_csirs_disc_cell.h). */
void nr_csirs_disc_cell_release(int cell)
{
  if (cell < 0 || cell >= NR_CELL_MAX) return;
  const int prev = nr_cell_idx();
  nr_cell_bind(cell);
  nr_csirs_disc_stop();
  nr_cell_bind(prev);
}

bool nr_csirs_disc_cell_mem(int cell, nr_csirs_disc_cell_mem_t *out)
{
  if (cell < 0 || cell >= NR_CELL_MAX || !out) return false;
  pthread_mutex_lock(&cell_mem_lock);
  *out = cell_ledger[cell].mem;
  pthread_mutex_unlock(&cell_mem_lock);
  out->active = atomic_load_explicit(&g_worker_cells[cell].running, memory_order_acquire);
  return out->active;
}

/* nr_csirs_disc_cell_host_need: host bytes at full size for the carrier (default ring, default TD queue); pure. */
size_t nr_csirs_disc_cell_host_need(const NR_DL_FRAME_PARMS *fp, int n_ant)
{
  if (!fp || n_ant < 1 || n_ant > NR_CSIRS_DISC_MAX_ANT) return 0;
  const bool snapshot_used = nr_csirs_disc_td_init == NULL;
  const size_t ring = (size_t)NR_CSIRS_DISC_DEFAULT_CAPACITY * n_ant * NR_CSIRS_DISC_NSYM * (size_t)(24 * fp->N_RB_DL) * sizeof(int16_t) * (snapshot_used ? 2 : 1);
  const size_t td = nr_csirs_disc_td_bytes ? nr_csirs_disc_td_bytes(fp, n_ant, 1024) : 0;
  return ring + td;
}
