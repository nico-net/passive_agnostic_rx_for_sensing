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

#include "PHY/defs_nr_common.h"

const uint16_t nr_csirs_disc_periods[NR_CSIRS_DISC_NUM_PERIODS] = {4, 5, 8, 10, 16, 20, 32, 40, 64, 80, 160, 320, 640};

void *(*nr_csirs_disc_alloc_unified)(size_t bytes);
void (*nr_csirs_disc_free_unified)(void *ptr);
void (*nr_csirs_disc_process_hook)(nr_csirs_disc_ring_t *ring);
uint64_t csirs_disc_feed_drops;

typedef struct {
  pthread_mutex_t read_lock;
  bool unified_hook;
} ring_private_t;

typedef struct {
  nr_csirs_disc_ring_t ring;
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

static void worker_note_slot(void);

static void read_enabled(void)
{
  const char *value = getenv("ISAC_CSIRS_DISC");
  g_enabled = value != NULL && strcmp(value, "1") == 0;
}

bool nr_csirs_disc_enabled(void)
{
  pthread_once(&g_enabled_once, read_enabled);
  return g_enabled;
}

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
  r->private_state = state;
  return 0;
}

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
  free(r->meta);
  if (state != NULL) {
    pthread_mutex_destroy(&state->read_lock);
    free(state);
  }
  memset(r, 0, sizeof(*r));
}

bool nr_csirs_disc_ring_read_lock(nr_csirs_disc_ring_t *r)
{
  ring_private_t *state = r != NULL ? r->private_state : NULL;
  return state != NULL && pthread_mutex_lock(&state->read_lock) == 0;
}

void nr_csirs_disc_ring_read_unlock(nr_csirs_disc_ring_t *r)
{
  ring_private_t *state = r != NULL ? r->private_state : NULL;
  if (state != NULL)
    pthread_mutex_unlock(&state->read_lock);
}

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
  const int first_count = grid_sc < ofdm_symbol_size - first_carrier_offset ? grid_sc : ofdm_symbol_size - first_carrier_offset;
  const int second_count = grid_sc - first_count;
  for (int ant = 0; ant < r->n_ant; ++ant) {
    if (rxdataF[ant] == NULL) {
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
  r->written++;
  pthread_mutex_unlock(&state->read_lock);
  if (r == &g_worker.ring)
    worker_note_slot();
  return index;
}

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
    while (!g_worker.stop && seen == g_worker.wake_generation)
      pthread_cond_wait(&g_worker.wake_cv, &g_worker.wake_lock);
    if (g_worker.stop)
      break;
    seen = g_worker.wake_generation;
    pthread_mutex_unlock(&g_worker.wake_lock);
    if (nr_csirs_disc_ring_read_lock(&g_worker.ring)) {
      void (*hook)(nr_csirs_disc_ring_t *) = nr_csirs_disc_process_hook;
      if (hook != NULL)
        hook(&g_worker.ring);
      nr_csirs_disc_ring_read_unlock(&g_worker.ring);
    }
    pthread_mutex_lock(&g_worker.wake_lock);
  }
  pthread_mutex_unlock(&g_worker.wake_lock);
  return NULL;
}

int nr_csirs_disc_start(const NR_DL_FRAME_PARMS *fp, int n_ant)
{
  if (!nr_csirs_disc_enabled())
    return 0;
  if (atomic_load_explicit(&g_worker.running, memory_order_acquire))
    return 0;
  if (fp == NULL || fp->symbols_per_slot != NR_CSIRS_DISC_NSYM)
    return -1;

  memset(&g_worker, 0, sizeof(g_worker));
  g_worker.batch = env_positive("ISAC_CSIRS_DISC_BATCH", 100);
  g_worker.core = env_core();
  if (pthread_mutex_init(&g_worker.wake_lock, NULL) != 0)
    return -1;
  if (pthread_cond_init(&g_worker.wake_cv, NULL) != 0) {
    pthread_mutex_destroy(&g_worker.wake_lock);
    return -1;
  }
  g_worker.synchronization_ready = true;
  const int capacity = env_positive("ISAC_CSIRS_DISC_RING", 640);
  if (nr_csirs_disc_ring_init(&g_worker.ring, n_ant, fp->N_RB_DL, fp->slots_per_frame, capacity, true) != 0)
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
  nr_csirs_disc_ring_free(&g_worker.ring);
  pthread_cond_destroy(&g_worker.wake_cv);
  pthread_mutex_destroy(&g_worker.wake_lock);
  memset(&g_worker, 0, sizeof(g_worker));
  return -1;
}

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
  nr_csirs_disc_ring_free(&g_worker.ring);
  if (g_worker.synchronization_ready) {
    pthread_cond_destroy(&g_worker.wake_cv);
    pthread_mutex_destroy(&g_worker.wake_lock);
  }
  memset(&g_worker, 0, sizeof(g_worker));
}

bool nr_csirs_disc_running(void)
{
  return atomic_load_explicit(&g_worker.running, memory_order_acquire);
}

nr_csirs_disc_ring_t *nr_csirs_disc_worker_ring(void)
{
  return nr_csirs_disc_running() ? &g_worker.ring : NULL;
}

static void worker_note_slot(void)
{
  if (!nr_csirs_disc_running())
    return;
  pthread_mutex_lock(&g_worker.wake_lock);
  if ((g_worker.ring.written % (uint64_t)g_worker.batch) == 0) {
    g_worker.wake_generation++;
    pthread_cond_signal(&g_worker.wake_cv);
  }
  pthread_mutex_unlock(&g_worker.wake_lock);
}
