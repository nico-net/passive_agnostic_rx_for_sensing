/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Per-grant observation records: JSON serialiser, ring buffer and writer thread (Task A3).
 * Deliberately free of OAI logging so the unit test links standalone. Schema: see nr_passive_obs.h. */
#include "common/utils/nr_core_plan.h"
#include "nr_passive_obs.h"
#include "nr_sense_records.h"
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

/* mu_init: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: none found in tree.
 */
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
/* jb_put: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_passive_obs.c:50, openair1/PHY/NR_UE_TRANSPORT/nr_passive_obs.c:69, openair1/PHY/NR_UE_TRANSPORT/nr_passive_obs.c:71, openair1/PHY/NR_UE_TRANSPORT/nr_passive_obs.c:76 (+16 more).
 */
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

/* jb_int: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_passive_obs.c:94, openair1/PHY/NR_UE_TRANSPORT/nr_passive_obs.c:96, openair1/PHY/NR_UE_TRANSPORT/nr_passive_obs.c:97, openair1/PHY/NR_UE_TRANSPORT/nr_passive_obs.c:98 (+25 more).
 */
static void jb_int(jb_t *j, const char *key, int64_t v)
{
  if (v < 0)
    jb_put(j, ",\"%s\":null", key);
  else
    jb_put(j, ",\"%s\":%" PRId64, key, v);
}

/* jb_signed_optional: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_passive_obs.c:159.
 */
static void jb_signed_optional(jb_t *j, const char *key, int64_t v, bool valid)
{
  if (valid) jb_put(j, ",\"%s\":%" PRId64, key, v);
  else jb_put(j, ",\"%s\":null", key);
}

/* jb_flt: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_passive_obs.c:116, openair1/PHY/NR_UE_TRANSPORT/nr_passive_obs.c:117, openair1/PHY/NR_UE_TRANSPORT/nr_passive_obs.c:118, openair1/PHY/NR_UE_TRANSPORT/nr_passive_obs.c:119 (+1 more).
 */
static void jb_flt(jb_t *j, const char *key, float v)
{
  if (!isfinite(v))
    jb_put(j, ",\"%s\":null", key);
  else
    jb_put(j, ",\"%s\":%.7g", key, (double)v);
}

/* nr_passive_obs_to_json: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_passive_obs.c:145, openair1/PHY/NR_UE_TRANSPORT/nr_passive_obs.c:206, openair1/PHY/NR_UE_TRANSPORT/nr_passive_obs.h:111, openair1/PHY/NR_UE_TRANSPORT/tests/nr_passive_obs_test.cc:25 (+3 more).
 */
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

/* Schema 2 extends the exact v1 field/value spelling. The legacy writer above
 * remains the only serializer used by ISAC_OBS_PATH. */
/* jb_str: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_passive_obs.c:152, openair1/PHY/NR_UE_TRANSPORT/nr_passive_obs.c:153, openair1/PHY/NR_UE_TRANSPORT/nr_passive_obs.c:161.
 */
static void jb_str(jb_t *j, const char *key, const char *v)
{
  if (!v) { jb_put(j, ",\"%s\":null", key); return; }
  jb_put(j, ",\"%s\":\"", key);
  for (; *v; v++) {
    const unsigned char c=(unsigned char)*v;
    if (c=='"' || c=='\\') jb_put(j,"\\%c",c);
    else if (c<32) jb_put(j,"\\u%04x",c);
    else jb_put(j,"%c",c);
  }
  jb_put(j,"\"");
}
/* nr_sense_grant_to_json: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_sense_records.c:197, openair1/PHY/NR_UE_TRANSPORT/nr_sense_records.h:65, openair1/PHY/NR_UE_TRANSPORT/tests/nr_sense_records_test.cc:34, openair1/PHY/NR_UE_TRANSPORT/tests/nr_sense_records_test.cc:54 (+5 more).
 */
int nr_sense_grant_to_json(const nr_passive_obs_t *o, const nr_sense_grant_meta_t *m, char *buf, size_t n)
{
  if (!o || !m || !buf || !n) return -1;
  char v1[1024];
  if (nr_passive_obs_to_json(o,v1,sizeof v1)<0) return -1;
  const char *tail=strchr(v1,',');
  if (!tail) return -1;
  jb_t j={buf,n,0,false};
  jb_put(&j,"{\"schema\":2,\"type\":\"grant\"%.*s",(int)(strlen(tail)-1),tail);
  jb_int(&j,"epoch",m->epoch); jb_int(&j,"identity_gen",m->identity_gen);
  jb_int(&j,"dedicated_epoch",m->dedicated_epoch); jb_int(&j,"rx_ts",m->rx_ts);
  jb_str(&j,"dl_coverage",o->dir==NR_OBS_DIR_DL?(m->dl_inline?"inline":"queue"):NULL);
  jb_str(&j,"verif",m->verif);
  jb_int(&j,"k2",m->k2); jb_int(&j,"dci_slot",m->dci_slot);
  jb_int(&j,"transform_precoding",m->transform_precoding);
  jb_int(&j,"freq_hopping",m->freq_hopping);
  jb_int(&j,"dmrs_type",m->dmrs_type); jb_int(&j,"dmrs_add_pos",m->dmrs_add_pos);
  jb_int(&j,"dmrs_n_scid",m->dmrs_n_scid); jb_int(&j,"ta_est_samples",m->ta_est_samples);
  jb_signed_optional(&j,"ta_applied_samples",m->ta_applied_samples,m->ta_applied_valid);
  jb_flt(&j,"cfo_est_hz",m->cfo_est_hz);
  jb_str(&j,"ul_timing_ref",o->dir==NR_OBS_DIR_UL?(m->ul_timing_ref?m->ul_timing_ref:"dl_frame"):NULL);
  jb_put(&j,"}");
  return j.ovf?-1:(int)j.w;
}

/* Flush the stdio buffer; the `pending` lines become `written` on success, io_errors on failure. Returns 0. */
/* obs_flush: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_passive_obs.c:193, openair1/PHY/NR_UE_TRANSPORT/nr_passive_obs.c:209, openair1/PHY/NR_UE_TRANSPORT/nr_passive_obs.c:214.
 */
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

/* writer: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: ISAC_OBS_TEST_WRITER_PAUSE_MS.
 *   Callers: none found in tree.
 */
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

/* nr_passive_obs_open: opens the per-grant observation file once: allocates a ring of `capacity` records and starts the writer thread. Returns false when already open, when the path or the capacity is invalid, or when the file or the ring cannot be created.
 *   Records are dropped (counted) when the ring is full. Caller: executables/nr-uesoftmodem.c (passive receiver start-up, when the observation path is set); closed by nr_passive_obs_close().
 */
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
  if (nr_core_plan_create(&g_thr, NULL, writer, NULL, -1) != 0) {
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

/* nr_passive_obs_push: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_passive_obs.h:129, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c:2007, openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.c:1624, openair1/PHY/NR_UE_TRANSPORT/tests/nr_passive_obs_test.cc:52 (+6 more).
 */
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

/* nr_passive_obs_close: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: executables/nr-uesoftmodem.c:755, openair1/PHY/NR_UE_TRANSPORT/nr_passive_obs.c:219, openair1/PHY/NR_UE_TRANSPORT/nr_passive_obs.h:124, openair1/PHY/NR_UE_TRANSPORT/tests/nr_passive_obs_test.cc:62 (+3 more).
 */
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

/* nr_passive_obs_stats: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_passive_metrics.c:39, openair1/PHY/NR_UE_TRANSPORT/nr_passive_metrics.c:181, openair1/PHY/NR_UE_TRANSPORT/nr_passive_obs.h:140, openair1/PHY/NR_UE_TRANSPORT/tests/nr_passive_obs_test.cc:64 (+3 more).
 */
void nr_passive_obs_stats(uint64_t *pushed, uint64_t *written, uint64_t *dropped)
{
  if (pushed)
    *pushed = atomic_load(&g_pushed);
  if (written)
    *written = atomic_load(&g_written);
  if (dropped)
    *dropped = atomic_load(&g_dropped);
}

/* nr_passive_obs_io_errors: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_passive_obs.h:144, openair1/PHY/NR_UE_TRANSPORT/tests/nr_passive_obs_test.cc:88, openair1/PHY/NR_UE_TRANSPORT/tests/nr_passive_obs_test.cc:103.
 */
uint64_t nr_passive_obs_io_errors(void)
{
  return atomic_load(&g_io_errors);
}

/* nr_passive_obs_after_close: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_passive_obs.h:123, openair1/PHY/NR_UE_TRANSPORT/nr_passive_obs.h:145, openair1/PHY/NR_UE_TRANSPORT/tests/nr_passive_obs_test.cc:99, openair1/PHY/NR_UE_TRANSPORT/tests/nr_passive_obs_test.cc:101.
 */
uint64_t nr_passive_obs_after_close(void)
{
  return atomic_load(&g_after_close);
}
