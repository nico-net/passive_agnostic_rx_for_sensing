#include "nr_pdcch_ul_discovery.h"
#include <stdatomic.h>
#include <string.h>
#include <pthread.h>

#include "nr_pusch_passive_monitor_rt.h"
#include "nr_pusch_passive_decode.h"
#include "nr_pdcch_blind_monitor_rt.h"
#include "nr_pusch_passive_queue.h"
#include "nr_pusch_data_aided.h"

#include "common/utils/LOG/log.h"
#include "common/utils/nr/nr_common.h"

/* Ring indexed by target slot. k2 is 4 on this cell and cannot exceed 32 by the config parser's own
 * bound, so 64 entries covers every in-flight grant with margin and makes lookup O(1) rather than a
 * scan. A slot is claimed by at most one grant here: this receiver decodes one PUSCH per slot. */
#define BOOK_SIZE 64

typedef struct {
  nr_pdcch_blind_ul_result_t g;
  int      target_slot;   ///< the PUSCH's own slot, as the receive path will see it
  int      target_frame;
  bool     valid;
} book_entry_t;

static book_entry_t g_book[BOOK_SIZE];
/* Producer (the PDCCH tap) and consumer (the UL-slot hook) are BOTH on the PHY receive thread when
 * the scan runs in-line, which is the default -- but pdcch_blind_monitor_pdsch's scan_thread field
 * can move the scan to a consumer thread, and then they are not. One mutex rather than an
 * assumption about which configuration is in use; it is held for a struct copy. */
static pthread_mutex_t g_book_lock = PTHREAD_MUTEX_INITIALIZER;

static _Atomic uint64_t g_parked, g_claimed, g_expired, g_overwritten;

/* Published in executables/nr-ue.c immediately BEFORE nrue_ru_read() fills that slot's rxdata. */
extern _Atomic long nr_ue_diag_producer_absolute_slot;

void nr_pusch_grant_book_add(const nr_pdcch_blind_ul_result_t *g, int frame, int slot,
                             int slots_per_frame)
{
  if (g == NULL || !g->plausible || slots_per_frame <= 0) {
    return;
  }
  /* Key on the PROCESSED slot (proc->frame_rx / proc->nr_slot_rx), NOT on
   * nr_ue_diag_producer_absolute_slot. That counter belongs to the RF PRODUCER and runs AHEAD of
   * the slot the receive path is working on, by however deep the pipeline happens to be. Using it
   * on both sides looks symmetric and is not: the producer would park a grant against a slot number
   * the consumer never sees, and every grant would expire unclaimed.
   *
   * The 1024-frame wrap that makes frame*slots_per_frame + slot unsafe as a monotonic clock does
   * not bite here, because nothing subtracts two of these: the pair is only compared for equality,
   * and k2 <= 32 by the config parser's own bound, so a parked grant is claimed within a couple of
   * slots or not at all. */
  const int total = slot + (int)g->k2;
  const int target_slot  = total % slots_per_frame;
  const int target_frame = (frame + total / slots_per_frame) % 1024;
  const int idx = target_slot % BOOK_SIZE;

  pthread_mutex_lock(&g_book_lock);
  if (g_book[idx].valid && (g_book[idx].target_slot != target_slot
                            || g_book[idx].target_frame != target_frame)) {
    /* A stale entry for a DIFFERENT slot still occupying this bucket means the previous grant was
     * never claimed -- counted rather than silently replaced, because a rising count here means the
     * UL hook is not running when it should (wrong slot map, or the hook not reached at all). */
    atomic_fetch_add_explicit(&g_overwritten, 1, memory_order_relaxed);
  }
  g_book[idx].g            = *g;
  g_book[idx].target_slot  = target_slot;
  g_book[idx].target_frame = target_frame;
  g_book[idx].valid        = true;
  pthread_mutex_unlock(&g_book_lock);
  atomic_fetch_add_explicit(&g_parked, 1, memory_order_relaxed);
}

/* TS 38.213 4.2 / nr_common.c's get_nr_N_TA_offset: N_TA_offset is 25600*Tc in FR1, expressed here
 * in samples at whatever rate this receiver is running. DERIVED from the running sample rate, not a
 * constant -- it is 1600 samples at 122.88 Msps and would be wrong at any other rate.
 *
 * N_TA itself (the per-UE advance the gNB commands) is NOT here and cannot be: it appears in no
 * DCI. It is the residual the operator supplies or a search finds. */
static int32_t n_ta_offset_samples(const NR_DL_FRAME_PARMS *fp)
{
  return (int32_t)(((uint64_t)25600 * (uint64_t)fp->samples_per_subframe) / (4096ull * 480ull));
}

void nr_pusch_passive_monitor_process(PHY_VARS_NR_UE *ue, const UE_nr_rxtx_proc_t *proc)
{
  const nr_pdcch_blind_monitor_cfg_t *cfg = nr_pdcch_blind_monitor_get_cfg();
  if (cfg == NULL || cfg->ul_pusch_decode == 0) {
    return;
  }
  const int idx = proc->nr_slot_rx % BOOK_SIZE;

  nr_pdcch_blind_ul_result_t g;
  bool have = false;
  pthread_mutex_lock(&g_book_lock);
  if (g_book[idx].valid && g_book[idx].target_slot == (int)proc->nr_slot_rx) {
    if (g_book[idx].target_frame == (int)proc->frame_rx) {
      g = g_book[idx].g;
      have = true;
    } else {
      /* Right slot, wrong frame: parked a frame ago and its slot came round again without this
       * hook running. Counted, not silently reused. */
      atomic_fetch_add_explicit(&g_expired, 1, memory_order_relaxed);
    }
    g_book[idx].valid = false;
  }
  pthread_mutex_unlock(&g_book_lock);
  if (!have) {
    return;
  }
  atomic_fetch_add_explicit(&g_claimed, 1, memory_order_relaxed);

  const int32_t ta = (cfg->ul_ta_offset_samples != 0) ? cfg->ul_ta_offset_samples
                                                      : n_ta_offset_samples(&ue->frame_parms);
  /* A one-shot marker on ENTRY, so that "the decode hung" and "the decode was never called" are
   * distinguishable from the log alone. Without it, a blocked receive thread and a hook that never
   * fires look identical: both simply stop producing output. */
  static int s_first = 1;
  if (s_first) {
    s_first = 0;
    LOG_I(PHY, "SENSING: PUSCHDIAG entering first decode %d.%d rnti=0x%x\n",
          proc->frame_rx, proc->nr_slot_rx, g.rnti);
  }
  /* ---- Deferred decode (nr_pusch_passive_queue.h). UTIM measured this decode at 1065 us mean
   * against a 500 us slot, over_slot 16830/16907 = 99.5 %: in-line, every uplink grant overruns its
   * deadline by more than 2x. Started lazily HERE for the same reason the downlink pools are: the
   * consumer needs a live PHY_VARS_NR_UE with frame_parms sized and rxdata allocated, which does
   * not exist when [sensing] is parsed. One-shot, and RT-side, so the guard stays single-threaded.
   * A refused start (--cont-fo-comp) degrades to the previous in-line behaviour rather than
   * dropping every grant. ---- */
  if (cfg->ul_thread) {
    static int s_ul_tried = 0;
    if (!s_ul_tried) {
      s_ul_tried = 1;
      const int depth = (cfg->ul_queue_depth > 0) ? cfg->ul_queue_depth : 8;
      nr_pusch_passive_queue_start(ue, depth, cfg->ul_thread, cfg->ul_thread_core);
    }
  }

  /* The producer's monotonic slot for THIS uplink slot. Computed here, on the receive thread, and
   * carried in the job: it is both the staleness reference and the CFR's slow-time index, and a
   * consumer recomputing it would read a producer counter that has moved on since these samples
   * were taken. Phase-align the producer counter to this slot -- both advance one per slot, so
   * subtracting the phase difference modulo the wrap gives this slot's value on the producer
   * timeline. */
  long abs_slot;
  {
    const long prod = atomic_load_explicit(&nr_ue_diag_producer_absolute_slot, memory_order_relaxed);
    const long spf  = (long)ue->frame_parms.slots_per_frame;
    const long wrap = spf * 1024;
    const long fd   = (long)proc->frame_rx * spf + (long)proc->nr_slot_rx;
    const long lag  = ((prod - fd) % wrap + wrap) % wrap;
    abs_slot = prod - lag;
  }

  nr_pusch_passive_out_t out;
  /* Sampled HERE, on the receive thread, in the same call that captured these samples -- see
   * nr_pusch_passive_job_t::fo_hz. A consumer reading ue-> later would get a newer value. */
  const double fo_hz = ue->cont_fo_comp ? (ue->dl_Doppler_shift + ue->freq_offset) : 0.0;
  if (nr_pusch_passive_queue_running()) {
    nr_pusch_passive_job_t job = {.grant             = g,
                                  .frame_rx          = (int)proc->frame_rx,
                                  .nr_slot_rx        = (int)proc->nr_slot_rx,
                                  .ta_offset_samples = ta,
                                  .absolute_slot     = abs_slot,
                                  .cfr_only          = (cfg->ul_pusch_decode == 2),
                                  .fo_hz             = fo_hz};
    nr_pusch_passive_queue_enqueue(&job);
    /* Nothing more to report per grant here: the outcome belongs to the consumer, and the census
     * (pusch_passive[...] / puschq[...]) is where it is read. Deliberately NOT decoded in-line on a
     * failed enqueue -- that would reintroduce the deadline overrun this exists to remove. */
    return;
  }
  nr_pusch_passive_decode(ue, 0, proc->frame_rx, proc->nr_slot_rx, &g, ta, (uint64_t)abs_slot,
                          cfg->ul_pusch_decode == 2, fo_hz, &out);
    if(!(cfg->ul_pusch_decode == 2) && (out.status==NR_PUSCH_PASSIVE_OK ||
        out.status==NR_PUSCH_PASSIVE_CRC_FAIL || out.status==NR_PUSCH_PASSIVE_ZERO_TB))
      nr_pdcch_ul_discovery_feedback(&g,out.status==NR_PUSCH_PASSIVE_OK);


}

void nr_pusch_grant_book_stats_dump(void)
{
  LOG_I(PHY,
        "SENSING: pusch_book[parked=%lu claimed=%lu expired=%lu overwritten=%lu]\n",
        (unsigned long)atomic_load_explicit(&g_parked, memory_order_relaxed),
        (unsigned long)atomic_load_explicit(&g_claimed, memory_order_relaxed),
        (unsigned long)atomic_load_explicit(&g_expired, memory_order_relaxed),
        (unsigned long)atomic_load_explicit(&g_overwritten, memory_order_relaxed));
  if (nr_pusch_passive_queue_running()) {
    nr_pusch_passive_queue_stats_t q;
    nr_pusch_passive_queue_get_stats(&q);
    LOG_I(PHY,
          "SENSING: PUSCHQ queued=%lu decoded=%lu crc_ok=%lu dropped[full=%lu stale=%lu] "
          "max_lag_slots=%lu/%d\n",
          (unsigned long)q.queued, (unsigned long)q.decoded, (unsigned long)q.crc_ok,
          (unsigned long)q.dropped_full, (unsigned long)q.dropped_stale,
          (unsigned long)q.max_lag_slots, NR_PUSCH_PASSIVE_QUEUE_MARGIN_SLOTS);
  }
  nr_pusch_passive_stats_dump();
  nr_isac_pusch_data_aided_stats_dump();
}
