/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */

#define _GNU_SOURCE
#include "nr_csirs_disc.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

static disc_worker_t g_worker;
static pthread_once_t g_enabled_once = PTHREAD_ONCE_INIT;
static bool g_enabled;

/* U1's standalone test deliberately links no U5 object. */
extern const nr_csirs_disc_gpu_ops_t *nr_csirs_disc_gpu_load(void) __attribute__((weak));
extern bool nr_csirs_disc_pipeline_ready(void) __attribute__((weak));
extern void nr_csirs_disc_pipeline_init(const NR_DL_FRAME_PARMS *fp) __attribute__((weak));

extern int nr_csirs_disc_td_init(const NR_DL_FRAME_PARMS *, int, int) __attribute__((weak));
extern void nr_csirs_disc_td_free(void) __attribute__((weak));
extern void nr_csirs_disc_td_reset(void) __attribute__((weak));
extern int nr_csirs_disc_td_pending(void) __attribute__((weak));
extern int nr_csirs_disc_td_drain(nr_csirs_disc_ring_t *) __attribute__((weak));

extern int nr_csirs_blind_rt_discovery_drain(void) __attribute__((weak));
static void worker_note_slot(void);

/* read_enabled: reads ISAC_CSIRS_DISC once (pthread_once) and stores whether it is exactly "1".
 *   Caller: nr_csirs_disc_enabled() (the cached switch for the whole discovery pipeline).
 */
static void read_enabled(void)
{
  const char *value = getenv("ISAC_CSIRS_DISC");
  g_enabled = value != NULL && strcmp(value, "1") == 0;
}

/* nr_csirs_disc_enabled: returns the cached ISAC_CSIRS_DISC switch: true when CSI-RS blind discovery is enabled for this process.
 *   Thread-safe after the first call. Read by the PHY receive path, the PDSCH queue (owned IQ) and the blind monitor.
 */
bool nr_csirs_disc_enabled(void)
{
  pthread_once(&g_enabled_once, read_enabled);
  return g_enabled;
}

/* env_positive: reads a positive integer from the environment variable `name`, or returns fallback when unset, malformed, zero or negative.
 *   Used for ISAC_CSIRS_DISC_BATCH, _RING and _TD_RING. Caller: nr_csirs_disc_start().
 */
static int env_positive(const char *name, int fallback)
{
  const char *text = getenv(name);
  if (text == NULL || *text == '\0')
    return fallback;
  char *end = NULL;
  errno = 0;
  const long value = strtol(text, &end, 10);
  return errno == 0 && end != text && *end == '\0' && value > 0 && value <= INT32_MAX ? (int)value : fallback;
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
  if (r == &g_worker.ring && nr_csirs_disc_td_reset) nr_csirs_disc_td_reset();
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

/* dump_slot: writes one slot (metadata and grid) to the file named by ISAC_CSIRS_DISC_DUMP, with a header record; the file is opened once.
 *   No-op when the variable is unset. Called from ring_push() and ring_write_end() for every committed slot.
 */
static void dump_slot(nr_csirs_disc_ring_t *r, int index)
{
  static FILE *dump;
  static int state;
  if(!state) {
    const char *path=getenv("ISAC_CSIRS_DISC_DUMP");
    dump=path && *path?fopen(path,"wb"):NULL;state=dump?1:-1;
    if(dump) {
      const int32_t hdr[5]={0x43534431,r->n_ant,r->n_rb,r->slots_per_frame,NR_CSIRS_DISC_NSYM};
      fwrite(hdr,sizeof(hdr),1,dump);
    }
  }
  if(state==1) {
    fwrite(&r->meta[index],sizeof(r->meta[index]),1,dump);
    fwrite(r->grid_base+index*r->slot_stride,sizeof(int16_t),r->slot_stride,dump);
    fflush(dump);
  }
}
/* nr_csirs_disc_ring_write_end: publishes the slots reserved by nr_csirs_disc_ring_write_begin(): writes their metadata, serial numbers and the dump, then advances the write counter and unlocks.
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
    if(meta) dump_slot(r,idx);
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
  dump_slot(r,index);
  pthread_mutex_unlock(&state->read_lock);
  if (r == &g_worker.ring)
    worker_note_slot();
  return index;
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

  pthread_mutex_lock(&g_worker.wake_lock);
  uint64_t seen = 0;
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
    if(drained>0) {
      /* TD mode has a single FD writer: this worker. The pipeline mutex
       * serializes processing with epoch reset, so no second IQ copy is needed. */
      if(hook) hook(&g_worker.ring);
    } else if (!nr_csirs_disc_td_drain && g_worker.ring.written!=g_worker.snapshot.written && nr_csirs_disc_ring_snapshot(&g_worker.ring, &g_worker.snapshot) == 0) {
      if(hook) hook(&g_worker.snapshot);
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
  if (!nr_csirs_disc_enabled())
    return 0;
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
  g_worker.batch = env_positive("ISAC_CSIRS_DISC_BATCH", NR_CSIRS_DISC_DEFAULT_BATCH);
  g_worker.core = env_core();
  if (pthread_mutex_init(&g_worker.wake_lock, NULL) != 0)
    return -1;
  if (pthread_cond_init(&g_worker.wake_cv, NULL) != 0) {
    pthread_mutex_destroy(&g_worker.wake_lock);
    return -1;
  }
  g_worker.synchronization_ready = true;
  const int capacity = env_positive("ISAC_CSIRS_DISC_RING", NR_CSIRS_DISC_DEFAULT_CAPACITY);
  if (nr_csirs_disc_ring_init(&g_worker.ring, n_ant, fp->N_RB_DL, fp->slots_per_frame, capacity, true) != 0)
    goto fail;
  const nr_csirs_disc_gpu_ops_t *gpu=nr_csirs_disc_gpu_load?nr_csirs_disc_gpu_load():NULL;
  if(gpu && gpu->device_alloc) {
    g_worker.ring.device_grid=gpu->device_alloc((size_t)capacity*g_worker.ring.slot_stride*2);
    if(!g_worker.ring.device_grid) goto fail;
  }
  if (nr_csirs_disc_ring_init(&g_worker.snapshot, n_ant, fp->N_RB_DL, fp->slots_per_frame, capacity, false) != 0)
    goto fail;
  if (nr_csirs_disc_td_init && nr_csirs_disc_td_init(fp, n_ant,
          env_positive("ISAC_CSIRS_DISC_TD_RING", 1024)) != 0)
    goto fail;
  pthread_attr_t attr;
  if (pthread_attr_init(&attr) != 0)
    goto fail;
  struct sched_param sched_param = {0};
  const bool attr_ready = pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED) == 0
                          && pthread_attr_setschedpolicy(&attr, SCHED_OTHER) == 0
                          && pthread_attr_setschedparam(&attr, &sched_param) == 0;
  const int thread_result = attr_ready ? pthread_create(&g_worker.thread, &attr, disc_worker_main, NULL) : -1;
  pthread_attr_destroy(&attr);
  if (thread_result != 0)
    goto fail;
  g_worker.thread_started = true;
  atomic_store_explicit(&g_worker.running, true, memory_order_release);
  return 0;

fail:
  if (nr_csirs_disc_td_free) nr_csirs_disc_td_free();
  nr_csirs_disc_ring_free(&g_worker.snapshot);
  nr_csirs_disc_ring_free(&g_worker.ring);
  pthread_cond_destroy(&g_worker.wake_cv);
  pthread_mutex_destroy(&g_worker.wake_lock);
  memset(&g_worker, 0, sizeof(g_worker));
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
static void worker_note_slot(void)
{
  if (!nr_csirs_disc_running())
    return;
  if ((g_worker.ring.written % (uint64_t)g_worker.batch) == 0) {
    __atomic_add_fetch(&g_worker.wake_generation, 1, __ATOMIC_RELEASE);
    pthread_cond_signal(&g_worker.wake_cv);
  }
}
