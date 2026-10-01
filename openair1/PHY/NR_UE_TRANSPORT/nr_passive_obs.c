/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Per-grant observation records: JSON serialiser, ring buffer and writer thread (Task A3).
 * Deliberately free of OAI logging so the unit test links standalone. Schema: see nr_passive_obs.h. */
#include "nr_passive_obs.h"
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* g_mu is re-initialised once (pthread_once, in open) as PTHREAD_PRIO_INHERIT so an RT decode thread that pushes
 * cannot be priority-inverted by the (normal-priority) writer holding it. The static initialiser keeps it valid
 * until then. */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t g_mu_once = PTHREAD_ONCE_INIT;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static nr_passive_obs_t *g_ring = NULL;
static uint32_t g_cap = 0, g_head = 0, g_count = 0;
static bool g_open = false, g_stop = false;
static FILE *g_f = NULL;
static pthread_t g_thr;
static _Atomic uint64_t g_pushed, g_written, g_dropped, g_io_errors, g_after_close;
static _Atomic bool g_closed_seen; /* a session was closed (and not re-opened) */

static void mu_init(void)
{
  pthread_mutexattr_t a;
  if (pthread_mutexattr_init(&a) == 0) {
    pthread_mutexattr_setprotocol(&a, PTHREAD_PRIO_INHERIT);
    pthread_mutex_init(&g_mu, &a);
    pthread_mutexattr_destroy(&a);
  }
}

/* Fast-path flag read by nr_passive_obs_enabled() (header); only touched through __atomic builtins. */
bool nr_passive_obs_fast_open = false;

typedef struct {
  char *p;
  size_t n, w;
  bool ovf;
} jb_t;

static void jb_put(jb_t *j, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void jb_put(jb_t *j, const char *fmt, ...)
{
  if (j->ovf)
    return;
  va_list ap;
  va_start(ap, fmt);
  const int k = vsnprintf(j->p + j->w, j->n - j->w, fmt, ap);
  va_end(ap);
  if (k < 0 || (size_t)k >= j->n - j->w) {
    j->ovf = true;
    return;
  }
  j->w += (size_t)k;
}

static void jb_int(jb_t *j, const char *key, int64_t v)
{
  if (v < 0)
    jb_put(j, ",\"%s\":null", key);
  else
    jb_put(j, ",\"%s\":%" PRId64, key, v);
}

static void jb_flt(jb_t *j, const char *key, float v)
{
  if (!isfinite(v))
    jb_put(j, ",\"%s\":null", key);
  else
    jb_put(j, ",\"%s\":%.7g", key, (double)v);
}

int nr_passive_obs_to_json(const nr_passive_obs_t *o, char *buf, size_t n)
{
  if (!o || !buf || n == 0)
    return -1;
  jb_t j = {buf, n, 0, false};
  jb_put(&j, "{\"schema\":%d", NR_PASSIVE_OBS_SCHEMA);
  jb_int(&j, "abs_slot", o->abs_slot);
  jb_put(&j, ",\"t_mono_ns\":%" PRIu64, o->t_mono_ns);
  jb_int(&j, "frame", o->frame);
  jb_int(&j, "slot", o->slot);
  jb_int(&j, "pci", o->pci);
  jb_put(&j, ",\"dir\":\"%s\",\"rnti\":%u", o->dir == NR_OBS_DIR_UL ? "UL" : "DL", (unsigned)o->rnti);
  jb_int(&j, "rnti_class", o->rnti_class);
  jb_int(&j, "start_rb", o->start_rb);
  jb_int(&j, "nb_rb", o->nb_rb);
  jb_int(&j, "start_sym", o->start_sym);
  jb_int(&j, "nb_sym", o->nb_sym);
  jb_int(&j, "mcs", o->mcs);
  jb_int(&j, "mcs_table", o->mcs_table);
  jb_int(&j, "qm", o->qm);
  jb_int(&j, "nl", o->nl);
  jb_put(&j, ",\"dmrs_symb_pos\":%u", (unsigned)o->dmrs_symb_pos);
  jb_int(&j, "dmrs_scrambling_id", o->dmrs_scrambling_id);
  jb_int(&j, "tbs", o->tbs);
  jb_int(&j, "harq_pid", o->harq_pid);
  jb_int(&j, "rv", o->rv);
  jb_int(&j, "ndi", o->ndi);
  jb_int(&j, "crc", o->crc);
  jb_flt(&j, "nvar", o->nvar);
  jb_flt(&j, "snr_db", o->snr_db);
  jb_flt(&j, "fo_comp_hz", o->fo_comp_hz);
  jb_flt(&j, "delay_samples", o->delay_samples);
  jb_int(&j, "carrier_hz", o->carrier_hz);
  jb_int(&j, "scs_khz", o->scs_khz);
  jb_int(&j, "fs_hz", o->fs_hz);
  jb_put(&j, "}");
  return j.ovf ? -1 : (int)j.w;
}

/* Flush the stdio buffer; the `pending` lines become `written` on success, io_errors on failure. Returns 0. */
static uint64_t obs_flush(uint64_t pending)
{
  const bool bad = fflush(g_f) != 0 || ferror(g_f);
  if (bad) {
    clearerr(g_f);
    atomic_fetch_add(&g_io_errors, pending ? pending : 1);
  } else {
    atomic_fetch_add(&g_written, pending);
  }
  return 0;
}

static void *writer(void *arg)
{
  (void)arg;
  /* Test hook only: a no-op unless the variable is set. Makes the ring fill so overflow can be tested. */
  const char *pause = getenv("ISAC_OBS_TEST_WRITER_PAUSE_MS");
  if (pause && atoi(pause) > 0)
    usleep((useconds_t)atoi(pause) * 1000);
  char line[1024];
  uint64_t pending = 0; /* lines in the stdio buffer, counted as `written` only once a flush succeeded */
  for (;;) {
    pthread_mutex_lock(&g_mu);
    if (g_count == 0 && !g_stop) {
      /* ring drained: flush so a live tail sees the lines (I/O outside the lock) */
      pthread_mutex_unlock(&g_mu);
      pending = obs_flush(pending);
      pthread_mutex_lock(&g_mu);
    }
    while (g_count == 0 && !g_stop)
      pthread_cond_wait(&g_cv, &g_mu);
    if (g_count == 0 && g_stop) {
      pthread_mutex_unlock(&g_mu);
      break;
    }
    const nr_passive_obs_t o = g_ring[g_head];
    g_head = (g_head + 1) % g_cap;
    g_count--;
    pthread_mutex_unlock(&g_mu);
    const int k = nr_passive_obs_to_json(&o, line, sizeof line);
    if (k > 0 && fwrite(line, 1, (size_t)k, g_f) == (size_t)k && fputc('\n', g_f) != EOF) {
      if (++pending >= 256)
        pending = obs_flush(pending);
    } else {
      atomic_fetch_add(&g_io_errors, 1);
    }
  }
  obs_flush(pending);
  return NULL;
}

bool nr_passive_obs_open(const char *path, uint32_t capacity)
{
  pthread_once(&g_mu_once, mu_init);
  pthread_mutex_lock(&g_mu);
  const bool already = g_open;
  pthread_mutex_unlock(&g_mu);
  if (already || capacity == 0 || !path)
    return false;
  g_f = fopen(path, "a");
  if (!g_f) {
    fprintf(stderr, "nr_passive_obs: cannot open %s\n", path);
    return false;
  }
  g_ring = calloc(capacity, sizeof(*g_ring));
  if (!g_ring) {
    fclose(g_f);
    g_f = NULL;
    return false;
  }
  g_cap = capacity;
  g_head = g_count = 0;
  g_stop = false;
  atomic_store(&g_pushed, 0);
  atomic_store(&g_written, 0);
  atomic_store(&g_dropped, 0);
  atomic_store(&g_io_errors, 0);
  atomic_store(&g_after_close, 0);
  atomic_store(&g_closed_seen, false);
  pthread_mutex_lock(&g_mu);
  g_open = true;
  pthread_mutex_unlock(&g_mu);
  __atomic_store_n(&nr_passive_obs_fast_open, true, __ATOMIC_RELAXED);
  if (pthread_create(&g_thr, NULL, writer, NULL) != 0) {
    __atomic_store_n(&nr_passive_obs_fast_open, false, __ATOMIC_RELAXED);
    pthread_mutex_lock(&g_mu);
    g_open = false;
    pthread_mutex_unlock(&g_mu);
    fclose(g_f);
    free(g_ring);
    g_f = NULL;
    g_ring = NULL;
    return false;
  }
  return true;
}

bool nr_passive_obs_push(const nr_passive_obs_t *o)
{
  if (!__atomic_load_n(&nr_passive_obs_fast_open, __ATOMIC_RELAXED)) { /* off / closed: no lock */
    if (atomic_load(&g_closed_seen))
      atomic_fetch_add(&g_after_close, 1);
    return false;
  }
  pthread_mutex_lock(&g_mu);
  if (!g_open) { /* re-check under the lock: close() may have won the race */
    pthread_mutex_unlock(&g_mu);
    atomic_fetch_add(&g_after_close, 1);
    return false;
  }
  if (g_count == g_cap) {
    pthread_mutex_unlock(&g_mu);
    atomic_fetch_add(&g_dropped, 1);
    return false;
  }
  g_ring[(g_head + g_count) % g_cap] = *o;
  g_count++;
  atomic_fetch_add(&g_pushed, 1); /* inside the lock: written <= pushed at every instant */
  pthread_cond_signal(&g_cv);
  pthread_mutex_unlock(&g_mu);
  return true;
}

void nr_passive_obs_close(void)
{
  pthread_mutex_lock(&g_mu);
  if (!g_open) {
    pthread_mutex_unlock(&g_mu);
    return;
  }
  g_open = false;
  __atomic_store_n(&nr_passive_obs_fast_open, false, __ATOMIC_RELAXED);
  atomic_store(&g_closed_seen, true);
  g_stop = true; /* no push is accepted after this; the writer drains every accepted record */
  pthread_cond_signal(&g_cv);
  pthread_mutex_unlock(&g_mu);
  pthread_join(g_thr, NULL);
  fclose(g_f);
  free(g_ring);
  g_ring = NULL;
  g_f = NULL;
}

void nr_passive_obs_stats(uint64_t *pushed, uint64_t *written, uint64_t *dropped)
{
  if (pushed)
    *pushed = atomic_load(&g_pushed);
  if (written)
    *written = atomic_load(&g_written);
  if (dropped)
    *dropped = atomic_load(&g_dropped);
}

uint64_t nr_passive_obs_io_errors(void)
{
  return atomic_load(&g_io_errors);
}

uint64_t nr_passive_obs_after_close(void)
{
  return atomic_load(&g_after_close);
}
