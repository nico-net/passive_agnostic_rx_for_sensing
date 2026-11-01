/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* PRACH is a receiver observation: use the broadcast RACH geometry and the
 * existing gNB correlator, then export only detections. No receiver state is
 * changed by an overheard RAR. */
#include "common/utils/nr_core_plan.h"
#include "common/utils/nr_core_pool.h"
#include "common/utils/nr_cell.h"
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
#define nr_isac_source_configured(s) 1
#endif

#define PRACH_PENDING 64
#define PRACH_RAR_RING 64
#define PRACH_RAR_WAIT_SLOTS 160
#define PRACH_MAX_RX 4
#define PRACH_PREFIX 4096
#define PRACH_WINDOW 1100000
#define PRACH_QUEUE 4

/* Branch carrier offsets of the live receiver (modulation_UE.c); absent in the unit-test binary, where there are none. */
double nr_ue_get_branch_fo_hz(int ant) __attribute__((weak));

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

/* ---- per-cell PRACH state (mpci CELLFEAT, common/utils/nr_cell.h) ----
 * Every admitted cell has its own detection pending list, RAR ring, noise floors, job queue and worker (created bound to the cell by
 * nr_prach_passive_start, which the cell's start hook calls on the cell's thread). The large buffers (sequence table, frequency buffers, the
 * stitch tail, the job queue: ~70 MB at 4 RX) are NR_CELL_STATIC_HEAP objects pre-allocated before the RX fence by nr_prach_passive_cell_prealloc
 * (only when the PRACH source is configured). The worker owns pending[] until it is joined; the RAR ring is written by the PDSCH queue thread of the
 * same cell (bound) and read by the worker. */
typedef prach_detection_t prach_pending_t[PRACH_PENDING];
NR_CELL_STATIC(prach_pending_t, pending)
#define pending NR_CELL_REF(pending)
NR_CELL_STATIC(unsigned, pending_head)
#define pending_head NR_CELL_REF(pending_head)
typedef _Atomic uint64_t prach_rar_ring_t[PRACH_RAR_RING];
NR_CELL_STATIC(prach_rar_ring_t, rar_events)
#define rar_events NR_CELL_REF(rar_events)
NR_CELL_STATIC(_Atomic unsigned, rar_head)
#define rar_head NR_CELL_REF(rar_head)
typedef c16_t prach_xu_t[64][839];
NR_CELL_STATIC_HEAP(prach_xu_t, prach_xu)
#define prach_xu NR_CELL_HEAP_REF(prach_xu_t, prach_xu)
typedef c16_t prach_freq_t[PRACH_MAX_RX][NUMBER_OF_NR_RU_PRACH_OCCASIONS_MAX][NR_PRACH_SEQ_LEN_L];
NR_CELL_STATIC_HEAP(prach_freq_t, prach_freq)
#define prach_freq NR_CELL_HEAP_REF(prach_freq_t, prach_freq)
typedef c16_t prach_tail_t[PRACH_MAX_RX][PRACH_PREFIX];
NR_CELL_STATIC_HEAP(prach_tail_t, prev_tail)
#define prev_tail NR_CELL_HEAP_REF(prach_tail_t, prev_tail)
NR_CELL_STATIC_INIT(uint64_t, prev_tail_slot, UINT64_MAX)
#define prev_tail_slot NR_CELL_REF(prev_tail_slot)
NR_CELL_STATIC_INIT(int, cached_root, -1)
#define cached_root NR_CELL_REF(cached_root)
NR_CELL_STATIC(int, cached_count)
#define cached_count NR_CELL_REF(cached_count)
NR_CELL_STATIC(int, cached_short)
#define cached_short NR_CELL_REF(cached_short)
#define PRACH_FLOOR_HISTORY 32
#define PRACH_FLOOR_SEED 8
typedef struct {
  int energy[PRACH_FLOOR_HISTORY];
  unsigned count, next;
  int gain_db;
} prach_floor_t;
typedef prach_floor_t prach_floors_t[8]; /* one noise floor per frequency occasion */
NR_CELL_STATIC(prach_floors_t, floors)
#define floors NR_CELL_REF(floors)
typedef struct {
  NR_DL_FRAME_PARMS fp;
  int frame, start_slot, current_slot, n_rx, nta, sample_base, gain_db;
  int format_raw, n_fdm, n_samples;
  double fo_hz[PRACH_MAX_RX]; /* carrier offset the receiver removes from this branch (same total as the OFDM front end: common + branch) */
  uint8_t sequence_length, subcarrier_spacing, restricted_set;
  uint64_t abs_slot, mono_ns, epoch_stamp;
  uint32_t dedicated_epoch;
  nr_prach_passive_occasion_t occasion;
  fapi_nr_num_prach_fd_occasions_t fd[8];
  c16_t samples[PRACH_MAX_RX][PRACH_WINDOW];
} prach_job_t;

typedef prach_job_t prach_jobs_t[PRACH_QUEUE];
NR_CELL_STATIC_HEAP(prach_jobs_t, jobs)
#define jobs NR_CELL_HEAP_REF(prach_jobs_t, jobs)
/* RAR look-back: the last PRACH_RETAIN processed occasions (de-rotated snapshot, copied by the worker after process_job) so a RAR decoded later can be matched
 * to its occasion and its RAPID tested as a single hypothesis. Pre-allocated with the cell, no allocation after start; worker-only. */
#define PRACH_RETAIN 2
typedef prach_job_t prach_retain_t[PRACH_RETAIN];
NR_CELL_STATIC_HEAP(prach_retain_t, retain)
#define retain NR_CELL_HEAP_REF(prach_retain_t, retain)
NR_CELL_STATIC(unsigned, retain_head)
#define retain_head NR_CELL_REF(retain_head)
typedef bool prach_valid_t[PRACH_RETAIN];
NR_CELL_STATIC(prach_valid_t, retain_valid)
#define retain_valid NR_CELL_REF(retain_valid)
NR_CELL_STATIC(unsigned, lb_cursor)
#define lb_cursor NR_CELL_REF(lb_cursor)
typedef uint16_t prach_rar_tc_t[PRACH_RAR_RING];
NR_CELL_STATIC(prach_rar_tc_t, rar_tc)
#define rar_tc NR_CELL_REF(rar_tc)
/* blind detections already exported (occasion abs slot, symbol, fdm, preamble): a look-back for the same RAPID is a duplicate */
typedef uint64_t prach_keys_t[16];
NR_CELL_STATIC(prach_keys_t, emitted_keys)
#define emitted_keys NR_CELL_REF(emitted_keys)
NR_CELL_STATIC(unsigned, emitted_head)
#define emitted_head NR_CELL_REF(emitted_head)
NR_CELL_STATIC(_Atomic uint64_t, prach_lb_tried)
#define prach_lb_tried NR_CELL_REF(prach_lb_tried)
NR_CELL_STATIC(_Atomic uint64_t, prach_lb_found)
#define prach_lb_found NR_CELL_REF(prach_lb_found)
NR_CELL_STATIC(_Atomic uint64_t, prach_lb_noring) /* a RAR whose occasion is no longer (or not) retained, or an unsupported (restricted-set) cell */
#define prach_lb_noring NR_CELL_REF(prach_lb_noring)
NR_CELL_STATIC_INIT(pthread_mutex_t, job_mu, PTHREAD_MUTEX_INITIALIZER)
#define job_mu NR_CELL_REF(job_mu)
NR_CELL_STATIC_INIT(pthread_cond_t, job_cv, PTHREAD_COND_INITIALIZER)
#define job_cv NR_CELL_REF(job_cv)
NR_CELL_STATIC(pthread_t, job_thread)
#define job_thread NR_CELL_REF(job_thread)
NR_CELL_STATIC(bool, worker_started)
#define worker_started NR_CELL_REF(worker_started)
NR_CELL_STATIC(bool, worker_stop)
#define worker_stop NR_CELL_REF(worker_stop)
NR_CELL_STATIC(atomic_flag, producer_busy)
#define producer_busy NR_CELL_REF(producer_busy)
NR_CELL_STATIC(unsigned, job_read)
#define job_read NR_CELL_REF(job_read)
NR_CELL_STATIC(unsigned, job_write)
#define job_write NR_CELL_REF(job_write)
NR_CELL_STATIC(_Atomic bool, prach_paused) /* MPCI-INT: the cell is retired (nr_prach_passive_cell_pause) */
#define prach_paused NR_CELL_REF(prach_paused)
NR_CELL_STATIC(_Atomic uint64_t, prach_us)
#define prach_us NR_CELL_REF(prach_us)
NR_CELL_STATIC(_Atomic uint64_t, prach_occasions)
#define prach_occasions NR_CELL_REF(prach_occasions)
NR_CELL_STATIC(_Atomic uint64_t, prach_detections) /* occasions whose best preamble beat the floor margin */
#define prach_detections NR_CELL_REF(prach_detections)
NR_CELL_STATIC(_Atomic int, prach_margin_max) /* best (energy - 25th-percentile floor), dB/10, over all seeded occasions; INT_MIN until seeded */
#define prach_margin_max NR_CELL_REF(prach_margin_max)
/* Per-occasion correlator statistics (dB/10 of the strongest preamble / delay of the occasion), to tell "no Msg1 on air" (peaks sit at the floor, spread
 * a few dB) from "correlator broken" (peaks all equal / zero: fixed-point underflow, wrong window, dead antenna). peak_zero counts occasions whose best
 * preamble had no energy at all. */
NR_CELL_STATIC(_Atomic int, prach_peak_last)
#define prach_peak_last NR_CELL_REF(prach_peak_last)
NR_CELL_STATIC_INIT(_Atomic int, prach_peak_max, INT32_MIN)
#define prach_peak_max NR_CELL_REF(prach_peak_max)
NR_CELL_STATIC_INIT(_Atomic int, prach_peak_min, INT32_MAX)
#define prach_peak_min NR_CELL_REF(prach_peak_min)
NR_CELL_STATIC_INIT(_Atomic int, prach_floor_last, -1) /* 25th-percentile floor at the last seeded occasion; -1 until seeded */
#define prach_floor_last NR_CELL_REF(prach_floor_last)
NR_CELL_STATIC(_Atomic uint64_t, prach_peak_zero)
#define prach_peak_zero NR_CELL_REF(prach_peak_zero)
NR_CELL_STATIC(_Atomic int64_t, prach_peak_sum)
#define prach_peak_sum NR_CELL_REF(prach_peak_sum)
NR_CELL_STATIC(_Atomic uint64_t, prach_seeded)
#define prach_seeded NR_CELL_REF(prach_seeded)
/* Input check of every snapshot (antenna 0, every 61st sample): mean power dB/10 and the count of all-zero snapshots. peak_zero with snap_zero == 0 means the
 * correlator is dead; snap_zero == occasions means the live ring held no samples at that window. */
NR_CELL_STATIC(_Atomic int, prach_snap_db)
#define prach_snap_db NR_CELL_REF(prach_snap_db)
NR_CELL_STATIC(_Atomic uint64_t, prach_snap_zero)
#define prach_snap_zero NR_CELL_REF(prach_snap_zero)
NR_CELL_STATIC(_Atomic uint64_t, prach_drops)
#define prach_drops NR_CELL_REF(prach_drops)
NR_CELL_STATIC(_Atomic uint64_t, prach_skipped)
#define prach_skipped NR_CELL_REF(prach_skipped)
NR_CELL_STATIC_INIT(_Atomic int, observed_gain_db, INT32_MIN)
#define observed_gain_db NR_CELL_REF(observed_gain_db)

/* Before the RX fence, once per admitted cell: the cell's large buffers. No-op when the PRACH source is not configured (nothing touches them then). */
void nr_prach_passive_cell_prealloc(int cell)
{
  if (!nr_isac_source_configured(NR_ISAC_SRC_PRACH)) return;
  NR_CELL_HEAP_PREALLOC(prach_xu_t, prach_xu, cell);
  NR_CELL_HEAP_PREALLOC(prach_freq_t, prach_freq, cell);
  NR_CELL_HEAP_PREALLOC(prach_tail_t, prev_tail, cell);
  NR_CELL_HEAP_PREALLOC(prach_jobs_t, jobs, cell);
  NR_CELL_HEAP_PREALLOC(prach_retain_t, retain, cell);
}

size_t nr_prach_passive_cell_heap_bytes(void) { return sizeof(prach_xu_t) + sizeof(prach_freq_t) + sizeof(prach_tail_t) + sizeof(prach_jobs_t) + sizeof(prach_retain_t); }

/* Pause / resume the PRACH pipeline of `cell` (hook of a retired / re-activated cell). No allocation; the worker, its queue and the pending list stay. On resume the
 * stitch tail is invalidated (the stream has a forward gap): the producer flag is taken so a slot in flight is not raced; if it is busy the invalidation is skipped
 * and the first occasion needing the tail is skipped by the continuity test (prev_tail_slot + 1 + slot != now). */
void nr_prach_passive_cell_pause(int cell, bool pause)
{
  if (cell < 0 || cell >= NR_CELL_MAX) return;
  const int prev = nr_cell_idx();
  nr_cell_bind(cell);
  atomic_store_explicit(&prach_paused, pause, memory_order_release);
  if (!pause && !atomic_flag_test_and_set_explicit(&producer_busy, memory_order_acquire)) {
    prev_tail_slot = UINT64_MAX;
    atomic_flag_clear_explicit(&producer_busy, memory_order_release);
  }
  nr_cell_bind(prev);
}

void nr_prach_passive_gain_changed(int gain_db)
{
  atomic_store_explicit(&observed_gain_db, gain_db, memory_order_release);
}

/* Why no PRACH record: occasions correlated, detections, and the best margin over the floor (dB/10; a Msg1 needs >= 80). margin_max is 0 until a floor is seeded. */
void nr_prach_passive_diag(uint64_t *occasions, uint64_t *detections, int *margin_max)
{
  *occasions = atomic_load_explicit(&prach_occasions, memory_order_relaxed);
  *detections = atomic_load_explicit(&prach_detections, memory_order_relaxed);
  *margin_max = atomic_load_explicit(&prach_margin_max, memory_order_relaxed);
}

/* The per-occasion correlator statistics (see prach_peak_last): peak_mean is over all correlated occasions, floor_last is -1 until a floor is seeded. */
void nr_prach_passive_diag_peaks(nr_prach_passive_peaks_t *p)
{
  const uint64_t n = atomic_load_explicit(&prach_occasions, memory_order_relaxed);
  p->peak_last = atomic_load_explicit(&prach_peak_last, memory_order_relaxed);
  p->peak_min = atomic_load_explicit(&prach_peak_min, memory_order_relaxed);
  p->peak_max = atomic_load_explicit(&prach_peak_max, memory_order_relaxed);
  p->peak_mean = n ? (int)(atomic_load_explicit(&prach_peak_sum, memory_order_relaxed) / (int64_t)n) : 0;
  p->floor_last = atomic_load_explicit(&prach_floor_last, memory_order_relaxed);
  p->peak_zero = atomic_load_explicit(&prach_peak_zero, memory_order_relaxed);
  p->seeded = atomic_load_explicit(&prach_seeded, memory_order_relaxed);
  p->snap_db = atomic_load_explicit(&prach_snap_db, memory_order_relaxed);
  p->snap_zero = atomic_load_explicit(&prach_snap_zero, memory_order_relaxed);
  p->lb_tried = atomic_load_explicit(&prach_lb_tried, memory_order_relaxed);
  p->lb_found = atomic_load_explicit(&prach_lb_found, memory_order_relaxed);
  p->lb_noring = atomic_load_explicit(&prach_lb_noring, memory_order_relaxed);
}

void nr_prach_passive_metrics(uint64_t *us, uint64_t *drops, uint64_t *skipped)
{
  const uint64_t count = atomic_load_explicit(&prach_occasions, memory_order_relaxed);
  *us = count ? atomic_load_explicit(&prach_us, memory_order_relaxed) / count : 0;
  *drops = atomic_load_explicit(&prach_drops, memory_order_relaxed);
  *skipped = atomic_load_explicit(&prach_skipped, memory_order_relaxed);
}

static int floor_compare(const void *a, const void *b)
{
  const int x = *(const int *)a, y = *(const int *)b;
  return (x > y) - (x < y);
}

static bool floor_detect_ex(int fdm, int gain_db, int energy, bool raw_zero)
{
  if (fdm < 0 || fdm >= 8) return false;
  prach_floor_t *f = &floors[fdm];
  if (f->count && f->gain_db != gain_db) *f = (prach_floor_t){0};
  f->gain_db = gain_db;
  bool detected = false;
  atomic_store_explicit(&prach_peak_last, energy, memory_order_relaxed);
  atomic_fetch_add_explicit(&prach_peak_sum, (int64_t)energy, memory_order_relaxed);
  if (raw_zero) atomic_fetch_add_explicit(&prach_peak_zero, 1, memory_order_relaxed);
  if (energy > atomic_load_explicit(&prach_peak_max, memory_order_relaxed)) atomic_store_explicit(&prach_peak_max, energy, memory_order_relaxed);
  if (energy < atomic_load_explicit(&prach_peak_min, memory_order_relaxed)) atomic_store_explicit(&prach_peak_min, energy, memory_order_relaxed);
  if (f->count >= PRACH_FLOOR_SEED) {
    int sorted[PRACH_FLOOR_HISTORY];
    memcpy(sorted, f->energy, f->count * sizeof(int));
    qsort(sorted, f->count, sizeof(int), floor_compare);
    const int margin = energy - sorted[(f->count - 1) / 4];
    atomic_store_explicit(&prach_floor_last, sorted[(f->count - 1) / 4], memory_order_relaxed);
    atomic_fetch_add_explicit(&prach_seeded, 1, memory_order_relaxed);
    detected = margin >= 80; /* floor margin, dB/10 */
    int best = atomic_load_explicit(&prach_margin_max, memory_order_relaxed);
    if (best == 0 || margin > best) atomic_store_explicit(&prach_margin_max, margin ? margin : 1, memory_order_relaxed);
    if (detected) atomic_fetch_add_explicit(&prach_detections, 1, memory_order_relaxed);
  }
  f->energy[f->next++ % PRACH_FLOOR_HISTORY] = energy;
  if (f->count < PRACH_FLOOR_HISTORY) f->count++;
  return detected;
}

static bool floor_detect(int fdm, int gain_db, int energy) { return floor_detect_ex(fdm, gain_db, energy, energy <= 0); }

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

static void rar_push(uint16_t ra_rnti, uint8_t rapid, uint16_t ta, uint64_t abs_slot, uint16_t tc_rnti)
{
  const uint64_t packed = ((abs_slot & 0xffffffULL) << 40) | ((uint64_t)ra_rnti << 24)
                          | ((uint64_t)rapid << 16) | ta;
  const unsigned pos = atomic_fetch_add_explicit(&rar_head, 1, memory_order_relaxed);
  rar_tc[pos % PRACH_RAR_RING] = tc_rnti;
  atomic_store_explicit(&rar_events[pos % PRACH_RAR_RING], packed, memory_order_release);
}

void nr_prach_passive_rar(uint16_t ra_rnti, uint8_t rapid, uint16_t ta, uint64_t abs_slot)
{
  if (!nr_isac_prach_enabled()) return;
  rar_push(ra_rnti, rapid, ta, abs_slot, 0);
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
      out[count++] = (nr_prach_rar_entry_t){sub & 0x3f, ta, (uint16_t)((q[5] << 8) | q[6])}; /* MAC RAR: R, TA(12), UL grant(27), TC-RNTI(16) */
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
    rar_push(ra_rnti, entries[i].rapid, entries[i].ta, abs_slot, entries[i].tc_rnti);
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
  emitted_keys[emitted_head++ % 16] = (d->abs_slot << 24) | ((uint64_t)(d->symbol & 15) << 20) | ((uint64_t)(d->fdm & 15) << 16) | (uint64_t)d->preamble;
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

/* x[n] *= exp(-j 2 pi fo n / fs), phase re-anchored in double every block. (nr_fo_compensation's table rotation drifts by up to ~1 kHz equivalent over a
 * 98304-sample window, which is a whole preamble tone: fine for one OFDM symbol, not for a PRACH window.) */
static void derotate(c16_t *x, int n, double fo_hz, double fs)
{
  enum { BLOCK = 256 };
  const double w = 2.0 * M_PI * fo_hz / fs;
  float tr[BLOCK], ti[BLOCK], cr[BLOCK], ci[BLOCK]; /* exp(+j w i) within a block, then times the block's start phasor */
  for (int i = 0; i < BLOCK; ++i) {
    tr[i] = (float)cos(w * i);
    ti[i] = (float)sin(w * i);
  }
  for (int b = 0; b < n; b += BLOCK) {
    const float br = (float)cos(w * b), bi = (float)sin(w * b);
    const int len = b + BLOCK < n ? BLOCK : n - b;
    c16_t *xb = x + b;
    for (int i = 0; i < BLOCK; ++i) {
      cr[i] = br * tr[i] - bi * ti[i];
      ci[i] = br * ti[i] + bi * tr[i];
    }
    for (int i = 0; i < len; ++i) { /* x * conj(c): remove +fo */
      const float re = xb[i].r * cr[i] + xb[i].i * ci[i], im = xb[i].i * cr[i] - xb[i].r * ci[i];
      xb[i].r = (int16_t)(re > 32767.f ? 32767.f : re < -32768.f ? -32768.f : re);
      xb[i].i = (int16_t)(im > 32767.f ? 32767.f : im < -32768.f ? -32768.f : im);
    }
  }
}

/* ---- RAR look-back ----
 * A decoded RAR names the occasion (RA-RNTI: s_id, t_id, f_id) and the RAPID. "Is preamble v present on that occasion" is ONE hypothesis over the NCS2 delay
 * bins of its zone, not 64 zones, so it needs far less than the blind 8 dB over the floor. Noise model per correlation bin: energy exponential with the mean
 * of the occasion's own bins (trimmed to the lowest 95 %, so a preamble or a PUSCH burst does not raise it): P(bin > x) = exp(-x / mean). This is an UPPER
 * bound of the tail for any number of RX branches combined (Gamma(k) with the same mean has a lighter tail), which keeps it valid when the branches share
 * interferers (measured on the capture: effective k of 2-3, not 4). Threshold: NCS2 * exp(-x / mean) = pfa. */
#define PRACH_LB_PFA 1e-3

static int u32_compare(const void *a, const void *b)
{
  const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
  return (x > y) - (x < y);
}

/* Single-preamble test on a retained occasion. Returns 1 found, 0 absent, -1 not supported (restricted set). Unrestricted sets only: same preamble -> (root offset,
 * cyclic shift) map and window as rx_nr_prach. */
static int lift_freq_bins(int n_rx, int n_occ, int n_zc);
static int lookback_detect(const prach_job_t *rj, int fdm, int td, int rapid, int *energy, int *delay_samples)
{
  if (rj->restricted_set != 0 || fdm >= rj->n_fdm || td >= rj->occasion.num_occasions) return -1;
  const fapi_nr_num_prach_fd_occasions_t *fd = &rj->fd[fdm];
  if (fd->num_root_sequences == 0 || fd->num_root_sequences > 64) return -1;
  const int N_ZC = rj->sequence_length == 0 ? 839 : 139;
  const int NCS = get_NCS(fd->prach_zero_corr_conf, rj->format_raw, rj->restricted_set);
  const int r = NCS ? N_ZC / NCS : 1;
  const int root_off = rapid / r;
  if (root_off >= fd->num_root_sequences) return -1;
  if (cached_root != fd->prach_root_sequence_index || cached_count != fd->num_root_sequences || cached_short != rj->sequence_length) {
    compute_nr_prach_seq(rj->sequence_length, fd->num_root_sequences, fd->prach_root_sequence_index, prach_xu);
    cached_root = fd->prach_root_sequence_index;
    cached_count = fd->num_root_sequences;
    cached_short = rj->sequence_length;
  }
  int32_t *time_rx[PRACH_MAX_RX];
  for (int a = 0; a < rj->n_rx; ++a) time_rx[a] = (int32_t *)rj->samples[a];
  memset(prach_freq, 0, sizeof(prach_freq));
  time_stats_t timing = {0};
  prach_item_t item = {.frame = rj->frame, .slot = rj->start_slot, .num_slots = 1, .rootSequenceIndex = fd->prach_root_sequence_index,
                       .numrootSequenceIndex = fd->num_root_sequences, .msg1_frequencystart = fd->k1, .mu = rj->subcarrier_spacing,
                       .prach_sequence_length = rj->sequence_length, .restricted_set = rj->restricted_set,
                       .numerology_index = rj->fp.numerology_index, .nb_rx = rj->n_rx, .Xu = prach_xu, .rx_prach = &timing,
                       .prach_buf = prach_freq};
  item.pdu.prach_format = rj->occasion.format;
  item.pdu.prach_start_symbol = rj->occasion.start_symbol;
  item.pdu.num_prach_ocas = rj->occasion.num_occasions;
  item.pdu.num_ra = fdm;
  item.pdu.num_cs = NCS;
  nr_prach_rx_copied(&item, time_rx, (NR_DL_FRAME_PARMS *)&rj->fp, rj->nta, rj->sample_base);
  const int lift = lift_freq_bins(rj->n_rx, rj->occasion.num_occasions, N_ZC);

  const int dft_sz = N_ZC == 839 ? 1024 : 256, log2_sz = N_ZC == 839 ? 10 : 8;
  c16_t *Xu = prach_xu[root_off];
  uint32_t e[1024] = {0};
  for (int aa = 0; aa < rj->n_rx; ++aa) {
    c16_t prachF[1024] __attribute__((aligned(32))), tmp[1024] __attribute__((aligned(32)));
    for (int k = 0; k < N_ZC; ++k) prachF[k] = c16MulConjShift(Xu[k], prach_freq[aa][td][k], 15);
    memset(prachF + N_ZC, 0, sizeof(c16_t) * (dft_sz - N_ZC));
    idft(get_idft(dft_sz), (int16_t *)prachF, (int16_t *)tmp, 1);
    for (int i = 0; i < dft_sz; ++i) e[i] += squaredMod(tmp[i]);
  }
  uint32_t sorted[1024];
  memcpy(sorted, e, sizeof(uint32_t) * dft_sz);
  qsort(sorted, dft_sz, sizeof(uint32_t), u32_compare);
  int shift = NCS ? (int)((N_ZC - (long)(rapid % r) * NCS % N_ZC) % N_ZC) : 0;
  const int shift2 = shift == 0 ? 0 : (shift << log2_sz) / N_ZC;
  int NCS2 = N_ZC == 839 ? (NCS << 10) / 839 : (NCS << 8) / 139;
  if (NCS2 == 0) NCS2 = N_ZC;
  uint32_t peak = 0;
  int best = 0;
  for (int i = 0; i < NCS2 && shift2 + i < dft_sz; ++i)
    if (e[shift2 + i] > peak) {
      peak = e[shift2 + i];
      best = i;
    }
  const int n_trim = dft_sz - dft_sz / 20;
  double mean = 0;
  for (int i = 0; i < n_trim; ++i) mean += sorted[i];
  mean /= n_trim;
  if (mean <= 0 || peak == 0) return 0;
  if (peak < mean * log(NCS2 / PRACH_LB_PFA)) return 0;
  *energy = dB_fixed_times10((peak >> log2_sz) / rj->n_rx) - (int)lround(60.206 * lift);
  int d = best;
  const int mu = rj->fp.numerology_index;
  if (rj->sequence_length == 0) {
    if (rj->occasion.format <= 2) d *= 3 * (1 << mu) / 2;
    else if (rj->occasion.format == 3) d *= 3 * (1 << mu) / 8;
  } else {
    d /= 2;
  }
  *delay_samples = (int)lround((double)d * rj->fp.samples_per_subframe / (1920.0 * (1 << mu)));
  return 1;
}

static void retain_job(const prach_job_t *job)
{
  if (job->n_samples <= 0) return;
  const unsigned i = retain_head++ % PRACH_RETAIN;
  prach_job_t *r = &retain[i];
  memcpy(r, job, offsetof(prach_job_t, samples));
  for (int a = 0; a < job->n_rx; ++a) memcpy(r->samples[a], job->samples[a], (size_t)job->n_samples * sizeof(c16_t));
  retain_valid[i] = true;
}

static bool key_emitted(uint64_t oabs, int symbol, int fdm, int preamble)
{
  const uint64_t key = (oabs << 24) | ((uint64_t)(symbol & 15) << 20) | ((uint64_t)(fdm & 15) << 16) | (uint64_t)preamble;
  for (int i = 0; i < 16; ++i)
    if (emitted_keys[i] == key) return true;
  return false;
}

/* Worker: every RAR seen since the last scan whose RAPID the blind path did not export is tested on its retained occasion. */
static void lookback_scan(void)
{
  const unsigned count = atomic_load_explicit(&rar_head, memory_order_acquire);
  if (count - lb_cursor > PRACH_RAR_RING) lb_cursor = count - PRACH_RAR_RING;
  while (lb_cursor != count) {
    const unsigned slot = lb_cursor++ % PRACH_RAR_RING;
    const uint64_t v = atomic_load_explicit(&rar_events[slot], memory_order_acquire);
    const uint16_t tc = rar_tc[slot];
    const uint32_t rar_slot = (v >> 40) & 0xffffff;
    const uint16_t rnti = (v >> 24) & 0xffff;
    const int rapid = (v >> 16) & 0xff, ta = v & 0xffff;
    bool covered = false, candidate = false;
    for (unsigned back = 1; back <= PRACH_RETAIN && !covered; ++back) {
      const unsigned idx = (retain_head - back) % PRACH_RETAIN;
      if (retain_head < back || !retain_valid[idx]) continue;
      const prach_job_t *rj = &retain[idx];
      const uint64_t oabs = rj->abs_slot - (rj->current_slot - rj->start_slot);
      const uint32_t age = rar_slot_age(oabs, rar_slot, rj->fp.slots_per_frame);
      if (age > PRACH_RAR_WAIT_SLOTS) continue; /* the RAR is already decoded: it may be newer than this job's own slot */
      const int ras = ra_slot(rj->start_slot, rj->fp.numerology_index, rj->occasion.format);
      for (int fdm = 0; fdm < rj->n_fdm && !covered; ++fdm)
        for (int td = 0; td < rj->occasion.num_occasions && !covered; ++td) {
          const int symbol = rj->occasion.start_symbol + td * rj->occasion.duration;
          if (nr_prach_passive_rar_relation(rj->frame, ras, symbol, fdm, rapid, rnti, rapid) != 1) continue;
          candidate = true;
          bool blind_has = key_emitted(oabs, symbol, fdm, rapid);
          for (int pi = 0; pi < PRACH_PENDING && !blind_has; ++pi) /* a blind detection of the same RAPID is still waiting and will carry this RAR */
            blind_has = pending[pi].valid && pending[pi].abs_slot == oabs && pending[pi].symbol == symbol && pending[pi].fdm == fdm
                        && pending[pi].preamble == rapid;
          if (blind_has) {
            covered = true;
            break;
          }
          int energy = 0, delay = 0;
          atomic_fetch_add_explicit(&prach_lb_tried, 1, memory_order_relaxed);
          const int r = lookback_detect(rj, fdm, td, rapid, &energy, &delay);
          if (r < 0) atomic_fetch_add_explicit(&prach_lb_noring, 1, memory_order_relaxed);
          if (r != 1) {
            covered = true; /* the newest matching occasion answers; older ones are other frames of the same RA-RNTI */
            continue;
          }
          atomic_fetch_add_explicit(&prach_lb_found, 1, memory_order_relaxed);
          const prach_detection_t d = {
              .valid = true, .abs_slot = oabs, .mono_ns = rj->mono_ns,
              .epoch = rj->epoch_stamp == UINT64_MAX ? UINT32_MAX : rj->epoch_stamp >> 32, .identity_gen = rj->epoch_stamp,
              .dedicated_epoch = rj->dedicated_epoch, .frame = rj->frame, .slot = rj->start_slot, .ra_slot = ras,
              .slots_per_frame = rj->fp.slots_per_frame, .fdm = fdm, .symbol = symbol, .preamble = rapid, .energy = energy,
              .delay_samples = delay, .pci = rj->fp.Nid_cell};
          nr_sense_rs_meta_t meta = detection_meta(&d, 1, rapid, ta);
          meta.rnti = tc; /* TC-RNTI of the RAR that named this RAPID */
          emitted_keys[emitted_head++ % 16] = (oabs << 24) | ((uint64_t)(symbol & 15) << 20) | ((uint64_t)(fdm & 15) << 16) | (uint64_t)rapid;
#ifdef NR_PASSIVE_PRACH_TEST
          atomic_fetch_add(&test_export_calls, 1);
#endif
          nr_sense_emit_rs(NR_SENSE_SRC_PRACH, &meta, 0);
          covered = true;
        }
    }
    if (!candidate) atomic_fetch_add_explicit(&prach_lb_noring, 1, memory_order_relaxed);
  }
}

/* The correlator is fixed point (int16 DFT bins, energy = (sum |ifft|^2 >> 10) / n_rx as an integer): an input 12 dB under the capture's UL noise floor (rms
 * ~21 LSB) collapses the energy to 0..1 everywhere, no preamble can be seen and the floor never moves. The occasion's frequency-domain bins are therefore lifted by a
 * power-of-two so their rms is >= 16 LSB (never lowered, never past int16 headroom), and the reported energy is referred back to the input scale (minus 6.02 dB per bit),
 * so floors and margins stay comparable between occasions. */
#define PRACH_MIN_RMS 16
static int lift_freq_bins(int n_rx, int n_occ, int n_zc)
{
  double pw = 0;
  int maxabs = 0;
  for (int a = 0; a < n_rx; ++a)
    for (int o = 0; o < n_occ; ++o)
      for (int k = 0; k < n_zc; ++k) {
        const c16_t v = prach_freq[a][o][k];
        pw += (double)v.r * v.r + (double)v.i * v.i;
        const int m = abs(v.r) > abs(v.i) ? abs(v.r) : abs(v.i);
        if (m > maxabs) maxabs = m;
      }
  const double rms = sqrt(pw / (2.0 * n_rx * n_occ * n_zc));
  int s = 0;
  while (s < 10 && rms * (1 << s) < PRACH_MIN_RMS && (maxabs << (s + 1)) < 16384) ++s;
  if (s == 0) return 0;
  for (int a = 0; a < n_rx; ++a)
    for (int o = 0; o < n_occ; ++o)
      for (int k = 0; k < n_zc; ++k) {
        prach_freq[a][o][k].r = (int16_t)(prach_freq[a][o][k].r * (1 << s));
        prach_freq[a][o][k].i = (int16_t)(prach_freq[a][o][k].i * (1 << s));
      }
  return s;
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
  for (int a = 0; a < job->n_rx; ++a) {
    time_rx[a] = (int32_t *)job->samples[a];
    /* The snapshot is raw receiver samples: remove the carrier offset exactly as the slot front end does. A 4.8 kHz offset is ~4 preamble tones
     * (1.25 kHz): the ZC peak lands on another preamble index and delay (wrong RAPID / TA) and a fraction of a tone costs peak energy. */
    if (job->fo_hz[a] != 0.0 && job->n_samples > 0)
      derotate((c16_t *)job->samples[a], job->n_samples, job->fo_hz[a], 1000.0 * job->fp.samples_per_subframe);
  }
  {
    uint64_t pw = 0;
    unsigned cnt = 0;
    for (int i = 0; i < job->n_samples; i += 61, ++cnt) pw += (uint64_t)((int)job->samples[0][i].r * job->samples[0][i].r + (int)job->samples[0][i].i * job->samples[0][i].i);
    if (pw == 0) atomic_fetch_add_explicit(&prach_snap_zero, 1, memory_order_relaxed);
    atomic_store_explicit(&prach_snap_db, cnt ? dB_fixed_times10((uint32_t)(pw / cnt)) : 0, memory_order_relaxed);
  }
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
    const int lift = lift_freq_bins(job->n_rx, job->occasion.num_occasions, job->sequence_length == 0 ? 839 : 139);
#ifdef NR_PASSIVE_PRACH_TEST
    atomic_fetch_add(&test_ru_calls, 1);
#endif
    for (int td = 0; td < job->occasion.num_occasions; ++td) {
      const rx_prach_out_t res = rx_nr_prach(&item, td);
#ifdef NR_PASSIVE_PRACH_TEST
      atomic_fetch_add(&test_correlator_calls, 1);
#endif
      const int energy = res.max_preamble_energy - (int)lround(60.206 * lift); /* input-referred, dB/10 */
      if (!floor_detect_ex(fdm, job->gain_db, energy, res.max_preamble_energy <= 0)) continue;
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
          .preamble = res.max_preamble, .energy = energy,
          .delay_samples = (int)lround((double)res.max_preamble_delay * job->fp.samples_per_subframe
                                       / (1920.0 * (1 << job->fp.numerology_index))),
          .pci = job->fp.Nid_cell};
    }
  }
  const uint64_t elapsed_ns=mono_ns()-started;
  atomic_fetch_add_explicit(&prach_us, elapsed_ns / 1000, memory_order_relaxed);
  nr_sense_cost_add(NR_SENSE_COST_PROD,NR_SENSE_SRC_PRACH,elapsed_ns);
  atomic_fetch_add_explicit(&prach_occasions, processed, memory_order_relaxed);
  retain_job(job);
  lookback_scan(); /* after the retain: a RAR that overtook its own occasion's worker job is still resolved */
}

static void *prach_worker(void *arg)
{
  nr_cell_bind((int)(intptr_t)arg); /* the worker serves the cell that started it (nr_core_plan_create has no cell argument) */
  pthread_mutex_lock(&job_mu);
  for (;;) {
    while (job_read == job_write && !worker_stop) pthread_cond_wait(&job_cv, &job_mu);
    if (job_read == job_write && worker_stop) break;
    prach_job_t *job = &jobs[job_read % PRACH_QUEUE];
    pthread_mutex_unlock(&job_mu);
    nr_pool_site_float_acquire(NR_POOL_HOUSE, nr_cell_idx(), 0); /* mpci B5: one HOUSE lease per job (bound cell); no-op when the float lane is not merged */
    process_job(job);
    nr_pool_site_float_release();
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
    if (nr_core_plan_create(&job_thread, NULL, prach_worker, (void *)(intptr_t)nr_cell_idx(), -1))
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
  lookback_scan();
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
    NR_CELL_STATIC(bool, warned_fdd)
#define warned_fdd NR_CELL_REF(warned_fdd)
    if (!warned_fdd) {
      warned_fdd = true;
      LOG_I(PHY, "PRACH passive inactive: FDD not supported\n");
    }
    return;
  }
  if (nr_cfg_ignore_sib1()) return;
  const nr_pdcch_sib1_prior_t *prior = nr_pdcch_sib1_prior_get();
  if (!prior || !prior->rach_valid) {
    NR_CELL_STATIC(bool, warned)
#define warned NR_CELL_REF(warned)
    if (!warned) {
      warned = true;
      LOG_I(PHY, "SENSING: passive PRACH inactive until SIB1 RACH config is available\n");
    }
    return;
  }
  {
    NR_CELL_STATIC(bool, logged_cfg)
#define logged_cfg NR_CELL_REF(logged_cfg)
    const fapi_nr_prach_config_t *pc = &ue->nrUE_config.prach_config;
    if (!logged_cfg && pc->num_prach_fd_occasions_list && pc->num_prach_fd_occasions) {
      logged_cfg = true;
      const nr_prach_info_t t = get_nr_prach_occasion_info_from_index(prior->prach_config_index, fp->freq_range, TDD);
      LOG_I(PHY, "SENSING: PRACH CONFIG idx=%d format=0x%x L=%d scs_idx=%d root=%d n_roots=%d zczc=%d restricted=%d fdm=%d k1=%d start_sym=%d n_t_slot=%d dur=%d N_RB_UL=%d fs=%d\n",
            prior->prach_config_index, (unsigned)t.format, pc->prach_sequence_length ? 139 : 839, pc->prach_sub_c_spacing,
            pc->num_prach_fd_occasions_list[0].prach_root_sequence_index, pc->num_prach_fd_occasions_list[0].num_root_sequences,
            pc->num_prach_fd_occasions_list[0].prach_zero_corr_conf, pc->restricted_set_config, pc->num_prach_fd_occasions,
            pc->num_prach_fd_occasions_list[0].k1, (int)t.start_symbol, (int)t.N_t_slot, (int)t.N_dur, fp->N_RB_UL, fp->samples_per_subframe * 1000);
    }
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
      NR_CELL_STATIC(bool, warned_boundary)
#define warned_boundary NR_CELL_REF(warned_boundary)
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
  job->n_samples = last - first < 0 ? 0 : last - first;
  const double common_fo = ue->cont_fo_comp ? ue->dl_Doppler_shift + ue->freq_offset : 0.0;
  for (int a = 0; a < n_rx; ++a) job->fo_hz[a] = common_fo + (nr_ue_get_branch_fo_hz ? nr_ue_get_branch_fo_hz(a) : 0.0);
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
  if (atomic_load_explicit(&prach_paused, memory_order_acquire)) return; /* retired cell: no job, no count */
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
  atomic_store(&prach_detections, 0);
  atomic_store(&prach_margin_max, 0);
  atomic_store(&prach_peak_last, 0);
  atomic_store(&prach_peak_max, INT32_MIN);
  atomic_store(&prach_peak_min, INT32_MAX);
  atomic_store(&prach_floor_last, -1);
  atomic_store(&prach_peak_zero, 0);
  atomic_store(&prach_peak_sum, 0);
  atomic_store(&prach_seeded, 0);
  atomic_store(&prach_snap_db, 0);
  atomic_store(&prach_snap_zero, 0);
  atomic_store(&prach_occasions, 0);
  atomic_store(&prach_us, 0);
  atomic_store(&prach_lb_tried, 0);
  atomic_store(&prach_lb_found, 0);
  atomic_store(&prach_lb_noring, 0);
  memset(retain_valid, 0, sizeof(retain_valid));
  retain_head = 0;
  lb_cursor = atomic_load(&rar_head);
  emitted_head = 0;
  memset(emitted_keys, 0, sizeof(emitted_keys));
  atomic_store(&prach_detections, 0);
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
