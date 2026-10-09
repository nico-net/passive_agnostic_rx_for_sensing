/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* PRACH is a receiver observation: use the broadcast RACH geometry and the
 * existing gNB correlator, then export only detections. No receiver state is
 * changed by an overheard RAR. */
#include "common/utils/nr_core_plan.h"
#include "nr_prach_passive.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "PHY/defs_gNB.h"
#include "PHY/NR_TRANSPORT/nr_transport_common_proto.h"
#include "PHY/NR_TRANSPORT/nr_transport_proto.h"
#include "PHY/NR_UE_TRANSPORT/nr_pdcch_sib1_prior.h"
#include "PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.h"
#include "nr_sense_records.h"
#include "PHY/NR_UE_TRANSPORT/nr_sense_api.h"
#include "PHY/NR_UE_ISAC/nr_isac.h"
#include "common/utils/LOG/log.h"
#include "common/utils/nr/nr_common.h"
#include "openair2/LAYER2/NR_MAC_COMMON/nr_mac_common.h"

#ifdef NR_PASSIVE_PRACH_TEST
static nr_pdcch_sib1_prior_t test_prior;
static bool test_ignore_sib1;
static _Atomic unsigned test_slot_calls, test_worker_calls;
static _Atomic unsigned test_ru_calls, test_correlator_calls, test_export_calls;
static _Atomic unsigned test_start_calls;
static pthread_t test_slot_tid, test_worker_tid;
#define nr_isac_prach_enabled() 1
#define nr_pdcch_sib1_prior_get() (&test_prior)
#define nr_cfg_ignore_sib1() test_ignore_sib1
#define nr_cfg_reconf_enabled() false
#endif

#define PRACH_PENDING 64
#define PRACH_RAR_RING 64
#define PRACH_RAR_WAIT_SLOTS 160
#define PRACH_MAX_RX 4
#define PRACH_PREFIX 4096
#define PRACH_WINDOW 1100000
#define PRACH_QUEUE 4

/* The two receive helpers use the same geometry; the copied variant indexes
 * from sample_base in a private buffer rather than the live frame ring. */
void nr_prach_rx_sample_range(prach_item_t *p, NR_DL_FRAME_PARMS *fp, int nta, int *start, int *end);
void nr_prach_rx_copied(prach_item_t *p, int32_t **rxdata, NR_DL_FRAME_PARMS *fp, int nta, int sample_base);

typedef struct {
  bool valid;
  uint64_t abs_slot;
  uint64_t mono_ns;
  uint32_t epoch, identity_gen, dedicated_epoch;
  int frame, slot, ra_slot, slots_per_frame, fdm, symbol, preamble, energy, delay_samples, pci;
} prach_detection_t;

static prach_detection_t pending[PRACH_PENDING]; /* worker owns this until joined */
static unsigned pending_head;
/* RAR worker publishes one packed, complete observation per cell. The low 24
 * slot bits cover an SFN cycle; the join compares modulo 1024 frames. */
static _Atomic uint64_t rar_events[PRACH_RAR_RING];
static _Atomic unsigned rar_head;
static c16_t prach_xu[64][839];
static c16_t prach_freq[PRACH_MAX_RX][NUMBER_OF_NR_RU_PRACH_OCCASIONS_MAX][NR_PRACH_SEQ_LEN_L];
static c16_t prev_tail[PRACH_MAX_RX][PRACH_PREFIX];
static uint64_t prev_tail_slot = UINT64_MAX;
static int cached_root = -1, cached_count, cached_short;
#define PRACH_FLOOR_HISTORY 32
#define PRACH_FLOOR_SEED 8
typedef struct {
  int energy[PRACH_FLOOR_HISTORY];
  unsigned count, next;
  int gain_db;
} prach_floor_t;
static prach_floor_t floors[8]; /* one noise floor per frequency occasion */
typedef struct {
  NR_DL_FRAME_PARMS fp;
  int frame, start_slot, current_slot, n_rx, nta, sample_base, gain_db;
  int format_raw, n_fdm;
  uint8_t sequence_length, subcarrier_spacing, restricted_set;
  uint64_t abs_slot, mono_ns, epoch_stamp;
  uint32_t dedicated_epoch;
  nr_prach_passive_occasion_t occasion;
  fapi_nr_num_prach_fd_occasions_t fd[8];
  c16_t samples[PRACH_MAX_RX][PRACH_WINDOW];
} prach_job_t;

static prach_job_t jobs[PRACH_QUEUE];
static pthread_mutex_t job_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t job_cv = PTHREAD_COND_INITIALIZER;
static pthread_t job_thread;
static bool worker_started, worker_stop;
static atomic_flag producer_busy = ATOMIC_FLAG_INIT;
static unsigned job_read, job_write;
static _Atomic uint64_t prach_us, prach_occasions, prach_drops, prach_skipped;
static _Atomic int observed_gain_db = INT32_MIN;

void nr_prach_passive_gain_changed(int gain_db)
{
  atomic_store_explicit(&observed_gain_db, gain_db, memory_order_release);
}

void nr_prach_passive_metrics(uint64_t *us, uint64_t *drops, uint64_t *skipped)
{
  const uint64_t count = atomic_load_explicit(&prach_occasions, memory_order_relaxed);
  *us = count ? atomic_load_explicit(&prach_us, memory_order_relaxed) / count : 0;
  *drops = atomic_load_explicit(&prach_drops, memory_order_relaxed);
  *skipped = atomic_load_explicit(&prach_skipped, memory_order_relaxed);
}

static int floor_margin_db10(void)
{
  const char *v = getenv("ISAC_PRACH_MARGIN_DB10");
  if (!v || !*v) return 80;
  char *end;
  long margin = strtol(v, &end, 10);
  return *end || margin < 1 || margin > 300 ? 80 : (int)margin;
}

static int floor_compare(const void *a, const void *b)
{
  const int x = *(const int *)a, y = *(const int *)b;
  return (x > y) - (x < y);
}

static bool floor_detect(int fdm, int gain_db, int energy)
{
  if (fdm < 0 || fdm >= 8) return false;
  prach_floor_t *f = &floors[fdm];
  if (f->count && f->gain_db != gain_db) *f = (prach_floor_t){0};
  f->gain_db = gain_db;
  bool detected = false;
  if (f->count >= PRACH_FLOOR_SEED) {
    int sorted[PRACH_FLOOR_HISTORY];
    memcpy(sorted, f->energy, f->count * sizeof(int));
    qsort(sorted, f->count, sizeof(int), floor_compare);
    detected = energy >= sorted[(f->count - 1) / 4] + floor_margin_db10();
  }
  f->energy[f->next++ % PRACH_FLOOR_HISTORY] = energy;
  if (f->count < PRACH_FLOOR_HISTORY) f->count++;
  return detected;
}

/* The gNB scheduler owns this helper in normal PHY_NR builds. The passive UE
 * links only the receive half of nr_prach.c, so provide its identical table. */
int get_nr_prach_duration(uint8_t format)
{
  static const int duration[] = {0, 0, 0, 0, 2, 4, 6, 2, 12, 2, 6, 2, 4, 6};
  return format < sizeof(duration) / sizeof(duration[0]) ? duration[format] : 0;
}

static int fapi_format(unsigned fmt)
{
  switch (fmt) {
    case 0: case 1: case 2: case 3: return fmt;
    case 0xa1: return 4;
    case 0xa2: return 5;
    case 0xa3: return 6;
    case 0xb1: return 7;
    case 0xb4: return 8;
    case 0xc0: return 9;
    case 0xc2: return 10;
    default: return -1;
  }
}

bool nr_prach_passive_occasion(int config_index, int frame, int slot, int mu, frequency_range_t range,
                               nr_prach_passive_occasion_t *out)
{
#ifdef NR_PASSIVE_PRACH_TEST
  /* A1 at frame start is a test-only geometry for the prior-frame stitch. */
  if (config_index == 254 && mu == 1) {
    if (slot == 0 && out) *out = (nr_prach_passive_occasion_t){4, 0, 1, 2};
    return slot == 0 && out;
  }
#endif
  if (!out || config_index < 0 || config_index > 255 || frame < 0 || slot < 0 || mu < 0 || mu > 4
      || (range == FR2 && mu < 2)
      || slot >= (10 << mu))
    return false;
  nr_prach_info_t info = get_nr_prach_occasion_info_from_index(config_index, range, TDD);
  uint16_t index = 0;
  if (!info.x || !get_nr_prach_sched_from_info(info, config_index, frame, slot, mu, range, &index, TDD))
    return false;
  const int fmt = fapi_format(info.format & 0xff);
  if (fmt < 0 || info.N_t_slot == 0 || info.N_t_slot > NUMBER_OF_NR_RU_PRACH_OCCASIONS_MAX)
    return false;
  *out = (nr_prach_passive_occasion_t){fmt, (int)info.start_symbol, (int)info.N_t_slot, (int)info.N_dur};
  return true;
}

int nr_prach_passive_rar_relation(int frame, int slot, int start_symbol, int fdm,
                                  int preamble, uint16_t ra_rnti, uint8_t rapid)
{
  (void)frame;
  if (slot < 0 || slot > 79 || start_symbol < 0 || start_symbol > 13 || fdm < 0 || fdm > 7
      || preamble < 0 || preamble > 63)
    return -1;
  const int expected = 1 + start_symbol + 14 * slot + 1120 * fdm;
  return ra_rnti != expected ? -1 : (preamble == rapid ? 1 : 0);
}

static uint32_t rar_slot_age(uint64_t detection_abs, uint32_t rar_abs, int slots_per_frame)
{
  /* The PDSCH/RAR path provides frame*slots_per_frame+slot, whereas the
   * PRACH producer includes HFN. Compare within the 1024-frame SFN cycle. */
  const uint32_t wrap = 1024u * (uint32_t)slots_per_frame;
  return ((rar_abs % wrap) + wrap - (detection_abs % wrap)) % wrap;
}

static int ra_slot(int slot, int mu, int format)
{
  /* 38.211 long preambles use the subframe index for RA-RNTI t_id. */
  return format < 4 ? slot >> mu : slot;
}

void nr_prach_passive_rar(uint16_t ra_rnti, uint8_t rapid, uint16_t ta, uint64_t abs_slot)
{
  if (!nr_isac_prach_enabled()) return;
  const uint64_t packed = ((abs_slot & 0xffffffULL) << 40) | ((uint64_t)ra_rnti << 24)
                          | ((uint64_t)rapid << 16) | ta;
  const unsigned pos = atomic_fetch_add_explicit(&rar_head, 1, memory_order_relaxed);
  atomic_store_explicit(&rar_events[pos % PRACH_RAR_RING], packed, memory_order_release);
}

int nr_prach_passive_rar_entries(const uint8_t *tb, uint32_t len, nr_prach_rar_entry_t *out, int capacity)
{
  if (!tb || !out || capacity <= 0) return 0;
  int count = 0;
  for (uint32_t i = 0; i < len;) {
    const uint8_t sub = tb[i];
    const bool more = (sub & 0x80) != 0;
    if (sub & 0x40) {
      if (i + 8 > len) break; /* validated parser already checked this */
      const uint8_t *q = tb + i + 1;
      const uint16_t ta = ((q[0] & 0x7f) << 5) | ((q[1] >> 3) & 0x1f);
      out[count++] = (nr_prach_rar_entry_t){sub & 0x3f, ta};
      if (count == capacity) break;
      i += 8;
    } else {
      i++;
    }
    if (!more) break;
  }
  return count;
}

void nr_prach_passive_rar_pdu(uint16_t ra_rnti, uint64_t abs_slot, const uint8_t *tb, uint32_t len)
{
  if (!nr_isac_prach_enabled()) return;
  nr_prach_rar_entry_t entries[64];
  const int count = nr_prach_passive_rar_entries(tb, len, entries, 64);
  for (int i = 0; i < count; ++i)
    nr_prach_passive_rar(ra_rnti, entries[i].rapid, entries[i].ta, abs_slot);
}

static nr_sense_rs_meta_t detection_meta(const prach_detection_t *d, int relation, int rapid, int ta)
{
  return (nr_sense_rs_meta_t){
    .start_rb=-1, .nb_rb=-1, .n_re=-1, .n_ant=-1, .ports=-1, .ssb_index=-1, .k_ssb=-1,
    .noise_var=NAN, .slot_frac=NAN, .abs_slot=d->abs_slot, .frame=d->frame, .slot=d->slot,
    .pci=d->pci, .rx_ts=-1, .t_mono_ns=d->mono_ns,
    .epoch=d->epoch==UINT32_MAX?-1:(int)d->epoch,
    .identity_gen=d->epoch==UINT32_MAX?-1:(int)d->identity_gen,
    .dedicated_epoch=d->epoch==UINT32_MAX?-1:(int)d->dedicated_epoch, .stamp_valid=true,
    .ta_est_samples=-1, .transform_precoding=-1, .dmrs_type=-1, .dmrs_add_pos=-1, .dmrs_n_scid=-1,
    .prach_preamble=d->preamble, .prach_energy=d->energy, .prach_delay=d->delay_samples,
    .prach_fdm=d->fdm, .prach_symbol=d->symbol,
    .prach_rar_match=relation, .prach_rar_rapid=rapid, .prach_rar_ta=ta};
}

static void emit_detection(const prach_detection_t *d, uint64_t now)
{
  int relation = -1, rapid = -1, ta = -1;
  const unsigned count = atomic_load_explicit(&rar_head, memory_order_acquire);
  for (unsigned i = 0; i < PRACH_RAR_RING && i < count; ++i) {
    const uint64_t v = atomic_load_explicit(&rar_events[i], memory_order_acquire);
    const uint32_t rar_slot = (v >> 40) & 0xffffff;
    const uint32_t age = rar_slot_age(d->abs_slot, rar_slot, d->slots_per_frame);
    if (age > PRACH_RAR_WAIT_SLOTS || age > now - d->abs_slot) continue;
    const int r = nr_prach_passive_rar_relation(d->frame, d->ra_slot, d->symbol, d->fdm,
                                               d->preamble, (v >> 24) & 0xffff, (v >> 16) & 0xff);
    if (r >= 0 && r > relation) {
      relation = r;
      rapid = (v >> 16) & 0xff;
      ta = v & 0xffff;
    }
  }
  const nr_sense_rs_meta_t meta=detection_meta(d,relation,rapid,ta);
#ifdef NR_PASSIVE_PRACH_TEST
  atomic_fetch_add(&test_export_calls, 1);
#endif
  nr_sense_emit_rs(NR_SENSE_SRC_PRACH,&meta,0);
}

#ifdef NR_PASSIVE_PRACH_TEST
int nr_prach_passive_rar_age_test(uint64_t detection_abs, uint64_t rar_abs, int slots_per_frame)
{
  return rar_slot_age(detection_abs, rar_abs, slots_per_frame);
}
int nr_prach_passive_ra_slot_test(int slot, int mu, int format)
{
  return ra_slot(slot, mu, format);
}
void nr_prach_passive_format_test(int relation, char *line, uint32_t capacity)
{
  const prach_detection_t d = {.valid = true, .abs_slot = 1234, .mono_ns = 987654321,
                               .epoch = 4, .identity_gen = 2, .dedicated_epoch = 3,
                               .frame = 61, .slot = 14, .ra_slot = 14, .slots_per_frame = 20,
                               .fdm = 1, .symbol = 2,
                               .preamble = 17, .energy = 420, .delay_samples = 32, .pci = 42};
  const nr_sense_rs_meta_t meta=detection_meta(&d,relation,relation<0?-1:17,relation<0?-1:20);
  nr_sense_rs_to_json(NR_SENSE_SRC_PRACH,&meta,0,line,capacity);
}
void nr_prach_passive_floor_reset_test(void) { memset(floors, 0, sizeof(floors)); }
int nr_prach_passive_floor_test(int fdm, int gain_db, int energy)
{
  return floor_detect(fdm, gain_db, energy);
}
#endif

static void flush_pending(uint64_t now)
{
  for (int i = 0; i < PRACH_PENDING; ++i) {
    prach_detection_t *d = &pending[i];
    if (!d->valid) continue;
    /* A matching RAR is decisive immediately; otherwise hold the record long
     * enough to distinguish a different RAPID from a missing RAR. */
    bool matched = false;
    const unsigned count = atomic_load_explicit(&rar_head, memory_order_acquire);
    for (unsigned j = 0; j < PRACH_RAR_RING && j < count; ++j) {
      const uint64_t v = atomic_load_explicit(&rar_events[j], memory_order_acquire);
      const uint32_t age = rar_slot_age(d->abs_slot, (v >> 40) & 0xffffff, d->slots_per_frame);
      if (age > PRACH_RAR_WAIT_SLOTS || age > now - d->abs_slot) continue;
      if (nr_prach_passive_rar_relation(d->frame, d->ra_slot, d->symbol, d->fdm,
                                       d->preamble, (v >> 24) & 0xffff, (v >> 16) & 0xff) == 1) {
        matched = true;
        break;
      }
    }
    if (matched || now - d->abs_slot >= PRACH_RAR_WAIT_SLOTS) {
      emit_detection(d, now);
      d->valid = false;
    }
  }
}

static uint64_t mono_ns(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static void process_job(const prach_job_t *job)
{
#ifdef NR_PASSIVE_PRACH_TEST
  test_worker_tid = pthread_self();
  atomic_fetch_add(&test_worker_calls, 1);
#endif
  const uint64_t started = mono_ns();
  flush_pending(job->abs_slot);
  int32_t *time_rx[PRACH_MAX_RX];
  unsigned processed = 0;
  for (int a = 0; a < job->n_rx; ++a) time_rx[a] = (int32_t *)job->samples[a];
  for (int fdm = 0; fdm < job->n_fdm; ++fdm) {
    const fapi_nr_num_prach_fd_occasions_t *fd = &job->fd[fdm];
    if (fd->num_root_sequences == 0 || fd->num_root_sequences > 64) continue;
    processed += job->occasion.num_occasions;
    if (cached_root != fd->prach_root_sequence_index || cached_count != fd->num_root_sequences
        || cached_short != job->sequence_length) {
      compute_nr_prach_seq(job->sequence_length, fd->num_root_sequences,
                           fd->prach_root_sequence_index, prach_xu);
      cached_root = fd->prach_root_sequence_index;
      cached_count = fd->num_root_sequences;
      cached_short = job->sequence_length;
    }
    memset(prach_freq, 0, sizeof(prach_freq));
    time_stats_t timing = {0};
    prach_item_t item = {
        .frame = job->frame, .slot = job->start_slot, .num_slots = 1,
        .rootSequenceIndex = fd->prach_root_sequence_index,
        .numrootSequenceIndex = fd->num_root_sequences, .msg1_frequencystart = fd->k1,
        .mu = job->subcarrier_spacing, .prach_sequence_length = job->sequence_length,
        .restricted_set = job->restricted_set, .numerology_index = job->fp.numerology_index,
        .nb_rx = job->n_rx, .Xu = prach_xu, .rx_prach = &timing, .prach_buf = prach_freq};
    item.pdu.prach_format = job->occasion.format;
    item.pdu.prach_start_symbol = job->occasion.start_symbol;
    item.pdu.num_prach_ocas = job->occasion.num_occasions;
    item.pdu.num_ra = fdm;
    item.pdu.num_cs = get_NCS(fd->prach_zero_corr_conf, job->format_raw, job->restricted_set);
    nr_prach_rx_copied(&item, time_rx, (NR_DL_FRAME_PARMS *)&job->fp, job->nta, job->sample_base);
#ifdef NR_PASSIVE_PRACH_TEST
    atomic_fetch_add(&test_ru_calls, 1);
#endif
    for (int td = 0; td < job->occasion.num_occasions; ++td) {
      const rx_prach_out_t res = rx_nr_prach(&item, td);
#ifdef NR_PASSIVE_PRACH_TEST
      atomic_fetch_add(&test_correlator_calls, 1);
#endif
      if (!floor_detect(fdm, job->gain_db, res.max_preamble_energy)) continue;
      prach_detection_t *d = &pending[pending_head++ % PRACH_PENDING];
      if (d->valid) emit_detection(d, job->abs_slot);
      *d = (prach_detection_t){
          .valid = true,
          .abs_slot = job->abs_slot - (job->current_slot - job->start_slot),
          .mono_ns = job->mono_ns,
          .epoch = job->epoch_stamp == UINT64_MAX ? UINT32_MAX : job->epoch_stamp >> 32,
          .identity_gen = job->epoch_stamp,
          .dedicated_epoch = job->dedicated_epoch,
          .frame = job->frame, .slot = job->start_slot,
          .ra_slot = ra_slot(job->start_slot, job->fp.numerology_index, job->occasion.format),
          .slots_per_frame = job->fp.slots_per_frame, .fdm = fdm,
          .symbol = job->occasion.start_symbol + td * job->occasion.duration,
          .preamble = res.max_preamble, .energy = res.max_preamble_energy,
          .delay_samples = (int)lround((double)res.max_preamble_delay * job->fp.samples_per_subframe
                                       / (1920.0 * (1 << job->fp.numerology_index))),
          .pci = job->fp.Nid_cell};
    }
  }
  const uint64_t elapsed_ns=mono_ns()-started;
  atomic_fetch_add_explicit(&prach_us, elapsed_ns / 1000, memory_order_relaxed);
  nr_sense_cost_add(NR_SENSE_COST_PROD,NR_SENSE_SRC_PRACH,elapsed_ns);
  atomic_fetch_add_explicit(&prach_occasions, processed, memory_order_relaxed);
}

static void *prach_worker(void *unused)
{
  (void)unused;
  pthread_mutex_lock(&job_mu);
  for (;;) {
    while (job_read == job_write && !worker_stop) pthread_cond_wait(&job_cv, &job_mu);
    if (job_read == job_write && worker_stop) break;
    prach_job_t *job = &jobs[job_read % PRACH_QUEUE];
    pthread_mutex_unlock(&job_mu);
    process_job(job);
    pthread_mutex_lock(&job_mu);
    ++job_read;
    pthread_cond_broadcast(&job_cv);
  }
  pthread_mutex_unlock(&job_mu);
  return NULL;
}

/* nr_prach_passive_start: starts the passive PRACH worker thread when ISAC PRACH is enabled and it is not yet running (through the core plan, unpinned). Logs a warning and drops PRACH occasions when the thread cannot start. Caller: executables/nr-uesoftmodem.c (passive receiver start-up). */
void nr_prach_passive_start(void)
{
  if (!nr_isac_prach_enabled()) return;
  pthread_mutex_lock(&job_mu);
  if (!worker_started && !worker_stop) {
    if (nr_core_plan_create(&job_thread, NULL, prach_worker, NULL, -1))
      LOG_W(PHY, "SENSING: passive PRACH worker could not start; occasions will be dropped\n");
    else {
      worker_started = true;
#ifdef NR_PASSIVE_PRACH_TEST
      atomic_fetch_add(&test_start_calls, 1);
#endif
    }
  }
  pthread_mutex_unlock(&job_mu);
}

/* The caller has stopped the UE slot thread before this function. The worker
 * drains queued jobs, is joined, then pending[] is flushed on this thread. */
void nr_prach_passive_flush(void)
{
  if (!nr_isac_prach_enabled()) return;
  pthread_mutex_lock(&job_mu);
  if (worker_started) {
    worker_stop = true;
    pthread_cond_signal(&job_cv);
  }
  pthread_mutex_unlock(&job_mu);
  if (worker_started) pthread_join(job_thread, NULL);
  worker_started = false;
  for (int i = 0; i < PRACH_PENDING; ++i) {
    if (pending[i].valid) {
      emit_detection(&pending[i], pending[i].abs_slot + PRACH_RAR_WAIT_SLOTS);
      pending[i].valid = false;
    }
  }
}

static bool symbols_allow_prach(const fapi_nr_config_request_t *cfg, int start_slot,
                                int final_slot, nr_prach_passive_occasion_t o)
{
  const fapi_nr_tdd_table_t *t = &cfg->tdd_table;
  if (!t->max_tdd_periodicity_list || !t->tdd_period_in_slots) return false;
  for (int slot = start_slot; slot <= final_slot; ++slot) {
    const fapi_nr_max_tdd_periodicity_t *entry =
        &t->max_tdd_periodicity_list[slot % t->tdd_period_in_slots];
    if (!entry->max_num_of_symbol_per_slot_list) return false;
    const int first = slot == start_slot ? o.start_symbol : 0;
    const int last = slot == start_slot && start_slot == final_slot && o.format >= 4
                         ? o.start_symbol + o.num_occasions * o.duration : NR_SYMBOLS_PER_SLOT;
    if (last > NR_SYMBOLS_PER_SLOT) return false;
    for (int symbol = first; symbol < last; ++symbol)
      if (entry->max_num_of_symbol_per_slot_list[symbol].slot_config == 0) return false;
  }
  return true;
}

static void process_slot(PHY_VARS_NR_UE *ue, const UE_nr_rxtx_proc_t *proc)
{
  if (!nr_isac_prach_enabled() || !ue || !proc) return;
#ifdef NR_PASSIVE_PRACH_TEST
  test_slot_tid = pthread_self();
  atomic_fetch_add(&test_slot_calls, 1);
#endif
  const NR_DL_FRAME_PARMS *fp = &ue->frame_parms;
  const uint64_t now = ((uint64_t)proc->hfn_rx * 1024 + proc->frame_rx) * fp->slots_per_frame + proc->nr_slot_rx;
  if (fp->frame_type == FDD) {
    static bool warned_fdd;
    if (!warned_fdd) {
      warned_fdd = true;
      LOG_I(PHY, "PRACH passive inactive: FDD not supported\n");
    }
    return;
  }
  if (nr_cfg_ignore_sib1()) return;
  const nr_pdcch_sib1_prior_t *prior = nr_pdcch_sib1_prior_get();
  if (!prior || !prior->rach_valid) {
    static bool warned;
    if (!warned) {
      warned = true;
      LOG_I(PHY, "SENSING: passive PRACH inactive until SIB1 RACH config is available\n");
    }
    return;
  }
  if (proc->nr_slot_rx == fp->slots_per_frame - 1 && fp->samples_per_frame >= PRACH_PREFIX) {
    for (int a = 0; a < fp->nb_antennas_rx && a < PRACH_MAX_RX; ++a)
      memcpy(prev_tail[a], ue->common_vars.rxdata[a] + fp->samples_per_frame - PRACH_PREFIX,
             sizeof(prev_tail[a]));
    prev_tail_slot = now;
  }
  if (proc->rx_slot_type == NR_DOWNLINK_SLOT) return;
  const fapi_nr_prach_config_t *cfg = &ue->nrUE_config.prach_config;
  if (!cfg->num_prach_fd_occasions_list || !cfg->num_prach_fd_occasions) return;
  const nr_prach_info_t table = get_nr_prach_occasion_info_from_index(prior->prach_config_index, fp->freq_range, TDD);
  const int format = fapi_format(table.format & 0xff);
  if (format < 0) return;
  const int span = format < 4 ? get_long_prach_dur(format, fp->numerology_index) : 1;
  const int start_slot = proc->nr_slot_rx - span + 1;
  if (start_slot < 0) {
    /* This occasion starts in the previous frame; no prior-frame body is retained. */
    const int previous_start = fp->slots_per_frame + start_slot;
    nr_prach_passive_occasion_t previous;
    if (nr_prach_passive_occasion(prior->prach_config_index, (proc->frame_rx + 1023) % 1024,
                                  previous_start, fp->numerology_index, fp->freq_range, &previous)) {
      static bool warned_boundary;
      atomic_fetch_add_explicit(&prach_skipped, (uint64_t)cfg->num_prach_fd_occasions * previous.num_occasions,
                                memory_order_relaxed);
      if (!warned_boundary) {
        warned_boundary = true;
        LOG_I(PHY, "SENSING: passive PRACH skipped occasion starting in previous frame\n");
      }
    }
    return;
  }
  nr_prach_passive_occasion_t o;
  if (!nr_prach_passive_occasion(prior->prach_config_index, proc->frame_rx, start_slot,
                                 fp->numerology_index, fp->freq_range, &o)) return;
  if (!symbols_allow_prach(&ue->nrUE_config, start_slot, proc->nr_slot_rx, o)) {
    atomic_fetch_add_explicit(&prach_skipped, (uint64_t)cfg->num_prach_fd_occasions * o.num_occasions,
                              memory_order_relaxed);
    return;
  }
  const int n_rx = fp->nb_antennas_rx < PRACH_MAX_RX ? fp->nb_antennas_rx : PRACH_MAX_RX;
  if (n_rx <= 0 || cfg->num_prach_fd_occasions > 8) {
    atomic_fetch_add_explicit(&prach_skipped, 1, memory_order_relaxed);
    return;
  }
  prach_item_t geometry = {.slot = start_slot, .numerology_index = fp->numerology_index,
                            .mu = cfg->prach_sub_c_spacing,
                            .msg1_frequencystart = cfg->num_prach_fd_occasions_list[0].k1,
                            .prach_sequence_length = cfg->prach_sequence_length};
  geometry.pdu.prach_format = o.format;
  geometry.pdu.prach_start_symbol = o.start_symbol;
  geometry.pdu.num_prach_ocas = o.num_occasions;
  const int nta = set_default_nta_offset(fp->freq_range, fp->samples_per_subframe);
  int first, last;
  nr_prach_rx_sample_range(&geometry, (NR_DL_FRAME_PARMS *)fp, nta, &first, &last);
  const int filled = get_samples_slot_timestamp(fp, proc->nr_slot_rx)
                     + get_samples_per_slot(proc->nr_slot_rx, fp) + fp->ofdm_symbol_size;
  if (first < -PRACH_PREFIX || last <= 0 || last > filled || last > fp->samples_per_frame
      || last <= first || last - first > PRACH_WINDOW
      || (first < 0 && (prev_tail_slot == UINT64_MAX || prev_tail_slot + 1 + proc->nr_slot_rx != now))) {
    atomic_fetch_add_explicit(&prach_skipped, 1, memory_order_relaxed);
    return;
  }
  pthread_mutex_lock(&job_mu);
  if (!worker_started || worker_stop || job_write - job_read == PRACH_QUEUE) {
    atomic_fetch_add_explicit(&prach_drops, 1, memory_order_relaxed);
    pthread_mutex_unlock(&job_mu);
    return;
  }
  prach_job_t *job = &jobs[job_write % PRACH_QUEUE];
  job->fp = *fp;
  job->frame = proc->frame_rx;
  job->start_slot = start_slot;
  job->current_slot = proc->nr_slot_rx;
  job->n_rx = n_rx;
  job->nta = nta;
  job->sample_base = first;
  const int observed_gain = atomic_load_explicit(&observed_gain_db, memory_order_acquire);
  job->gain_db = observed_gain != INT32_MIN ? observed_gain : (int)ue->rx_total_gain_dB;
  job->format_raw = table.format & 0xff;
  job->n_fdm = cfg->num_prach_fd_occasions;
  job->sequence_length = cfg->prach_sequence_length;
  job->subcarrier_spacing = cfg->prach_sub_c_spacing;
  job->restricted_set = cfg->restricted_set_config;
  job->abs_slot = now;
  job->mono_ns = mono_ns();
  job->epoch_stamp = nr_cfg_reconf_enabled() ? nr_cfg_epoch_cfr_stamp() : UINT64_MAX;
  job->dedicated_epoch = nr_cfg_reconf_enabled() ? nr_cfg_epoch_dedicated_current() : 0;
  job->occasion = o;
  memcpy(job->fd, cfg->num_prach_fd_occasions_list, job->n_fdm * sizeof(job->fd[0]));
  for (int a = 0; a < n_rx; ++a) {
    c16_t *dest = job->samples[a];
    if (first < 0) {
      const int prefix = -first;
      memcpy(dest, prev_tail[a] + PRACH_PREFIX - prefix, prefix * sizeof(c16_t));
      memcpy(dest + prefix, ue->common_vars.rxdata[a], last * sizeof(c16_t));
    } else {
      memcpy(dest, ue->common_vars.rxdata[a] + first, (last - first) * sizeof(c16_t));
    }
  }
  ++job_write;
  pthread_cond_signal(&job_cv);
  pthread_mutex_unlock(&job_mu);
}

void nr_prach_passive_process(PHY_VARS_NR_UE *ue, const UE_nr_rxtx_proc_t *proc)
{
  if (!nr_isac_prach_enabled()) return;
  if (atomic_flag_test_and_set_explicit(&producer_busy, memory_order_acquire)) {
    atomic_fetch_add_explicit(&prach_drops, 1, memory_order_relaxed);
    return;
  }
  process_slot(ue, proc);
  atomic_flag_clear_explicit(&producer_busy, memory_order_release);
}

#ifdef NR_PASSIVE_PRACH_TEST
void nr_prach_passive_test_reset(void)
{
  if (worker_started) nr_prach_passive_flush();
  pthread_mutex_lock(&job_mu);
  worker_stop = false;
  job_read = job_write = 0;
  pthread_mutex_unlock(&job_mu);
  memset(pending, 0, sizeof(pending));
  pending_head = 0;
  memset(floors, 0, sizeof(floors));
  prev_tail_slot = UINT64_MAX;
  atomic_store(&test_slot_calls, 0);
  atomic_store(&test_worker_calls, 0);
  atomic_store(&test_ru_calls, 0);
  atomic_store(&test_correlator_calls, 0);
  atomic_store(&test_export_calls, 0);
  atomic_store(&test_start_calls, 0);
  atomic_store(&prach_drops, 0);
  atomic_store(&prach_skipped, 0);
  nr_prach_passive_start();
}
void nr_prach_passive_test_prior(int index, bool valid, bool ignore)
{
  test_prior = (nr_pdcch_sib1_prior_t){.valid = true, .rach_valid = valid, .prach_config_index = index};
  test_ignore_sib1 = ignore;
}
void nr_prach_passive_test_drain(void)
{
  pthread_mutex_lock(&job_mu);
  while (job_read != job_write) pthread_cond_wait(&job_cv, &job_mu);
  pthread_mutex_unlock(&job_mu);
}
void nr_prach_passive_test_threads(unsigned *slot_calls, unsigned *worker_calls, bool *same_thread)
{
  *slot_calls = atomic_load(&test_slot_calls);
  *worker_calls = atomic_load(&test_worker_calls);
  *same_thread = *worker_calls && pthread_equal(test_slot_tid, test_worker_tid);
}
void nr_prach_passive_test_heavy_calls(unsigned *ru, unsigned *correlator, unsigned *exported)
{
  *ru = atomic_load(&test_ru_calls);
  *correlator = atomic_load(&test_correlator_calls);
  *exported = atomic_load(&test_export_calls);
}
unsigned nr_prach_passive_test_start_calls(void) { return atomic_load(&test_start_calls); }
#endif
