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


#include "nr_passive_ul_grant_book.h"
#define HISTORY_SLOTS 160 /* maximum supported slots/frame at mu=4 */
static nr_passive_ul_book_t g_book;
static pthread_mutex_t g_book_lock=PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t g_parked,g_claimed,g_expired,g_overwritten;
extern _Atomic long nr_ue_diag_producer_absolute_slot;
typedef struct {long source;double fo_hz;bool valid;} ul_sample_history_t;
static ul_sample_history_t g_history[HISTORY_SLOTS];
void nr_pusch_grant_book_add(const nr_pdcch_blind_ul_result_t *g, long source_absolute_slot)
{
  pthread_mutex_lock(&g_book_lock);
  const int status=nr_passive_ul_book_put(&g_book,g,source_absolute_slot);
  pthread_mutex_unlock(&g_book_lock);
  if(status>0) atomic_fetch_add_explicit(&g_parked,1,memory_order_relaxed);
  if(status<0) atomic_fetch_add_explicit(&g_overwritten,1,memory_order_relaxed);
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

static void passive_ul_deliver(PHY_VARS_NR_UE *ue, const UE_nr_rxtx_proc_t *proc,
    const nr_pdcch_blind_monitor_cfg_t *cfg, const nr_pdcch_blind_ul_result_t *grant,
    long abs_slot, double fo_hz)
{
  const nr_pdcch_blind_ul_result_t g=*grant;
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

  nr_pusch_passive_out_t out;
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
        out.status==NR_PUSCH_PASSIVE_CRC_FAIL || out.status==NR_PUSCH_PASSIVE_ZERO_TB)) {
      nr_pdcch_ul_discovery_feedback(&g,out.status==NR_PUSCH_PASSIVE_OK);
      nr_pdcch_dci01_fdra_feedback(&g, out.status==NR_PUSCH_PASSIVE_OK);
    }


}

/* Called on every received slot, including mixed slots and slots after a late DCI. */
void nr_pusch_passive_monitor_process(PHY_VARS_NR_UE *ue, const UE_nr_rxtx_proc_t *proc)
{
  const nr_pdcch_blind_monitor_cfg_t *cfg=nr_pdcch_blind_monitor_get_cfg();
  if(!cfg || !cfg->ul_pusch_decode) return;
  const long spf=ue->frame_parms.slots_per_frame;
  const long now=((long)proc->hfn_rx*1024+proc->frame_rx)*spf+proc->nr_slot_rx;
  static long previous=-1;
  if(previous>=0 && (now<=previous || now-previous>1)) {
    pthread_mutex_lock(&g_book_lock);
    memset(&g_book,0,sizeof(g_book));
    memset(g_history,0,sizeof(g_history));
    pthread_mutex_unlock(&g_book_lock);
  }
  previous=now;
  g_history[now%HISTORY_SLOTS]=(ul_sample_history_t){.source=now,
    .fo_hz=ue->cont_fo_comp?ue->dl_Doppler_shift+ue->freq_offset:0.0,.valid=true};
  for(int n=0;n<NR_PASSIVE_UL_BOOK_CAPACITY;n++) {
    nr_passive_ul_book_entry_t entry;
    unsigned expired=0;
    pthread_mutex_lock(&g_book_lock);
    bool have=nr_passive_ul_book_take(&g_book,now,spf,&entry,&expired);
    pthread_mutex_unlock(&g_book_lock);
    atomic_fetch_add_explicit(&g_expired,expired,memory_order_relaxed);
    if(!have) break;
    const ul_sample_history_t *sample=&g_history[entry.target%HISTORY_SLOTS];
    if(!sample->valid || sample->source!=entry.target) {
      atomic_fetch_add_explicit(&g_expired,1,memory_order_relaxed); continue;
    }
    UE_nr_rxtx_proc_t target=*proc;
    target.nr_slot_rx=entry.target%spf;
    target.frame_rx=(entry.target/spf)%1024;
    target.hfn_rx=(entry.target/spf)/1024;
    atomic_fetch_add_explicit(&g_claimed,1,memory_order_relaxed);
    passive_ul_deliver(ue,&target,cfg,&entry.grant,entry.target,sample->fo_hz);
  }
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
