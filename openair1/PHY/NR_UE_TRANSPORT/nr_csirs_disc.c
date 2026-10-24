/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_csirs_disc.h"
#include "nr_csirs_disc_mem.h"
/* T6b: weak, so fixtures that link this file without the platform library keep an unlimited budget */
extern size_t nr_gpu_membudget_share(nr_gpu_mem_module_t m) __attribute__((weak));
extern void nr_gpu_membudget_commit(nr_gpu_mem_module_t m, size_t bytes, double scale) __attribute__((weak));
extern size_t nr_gpu_membudget_headroom(void) __attribute__((weak));
extern int nr_gpu_membudget_dyn_acquire(nr_gpu_mem_module_t m, size_t bytes) __attribute__((weak));
extern void nr_gpu_membudget_dyn_release(nr_gpu_mem_module_t m, size_t bytes) __attribute__((weak));
extern void nr_gpu_membudget_dyn_abort(nr_gpu_mem_module_t m, size_t bytes) __attribute__((weak));
#include "PHY/TOOLS/nr_gpu_elastic.h"
#include "PHY/defs_nr_common.h"
#include "nr_csirs_disc_gate_internal.h"
#include "nr_csirs_disc_evidence_internal.h"
#include "nr_csirs_disc_rate_internal.h"
#include "nr_pdsch_passive_queue.h"

#include "common/utils/LOG/log.h"
#include "nr_passive_cfg_epoch.h"

#include <pthread.h>
#include <string.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <time.h>

extern void nr_cfg_epoch_subscribe(nr_cfg_epoch_listener_t fn) __attribute__((weak));

/* Kept weak so the detector remains usable in focused U2/U3/U4 fixtures. */
extern int nr_csirs_blind_rt_add_discovered(const nr_csirs_disc_hyp_t *, const nr_csirs_disc_win_t *)
    __attribute__((weak));
extern bool nr_csirs_blind_rt_discovery_confirmed(const nr_csirs_disc_hyp_t *) __attribute__((weak));
extern int nr_csirs_blind_rt_discovery_match(const nr_csirs_disc_hyp_t *) __attribute__((weak));
extern void nr_csirs_blind_rt_discovery_disable(void) __attribute__((weak));
extern int nr_csirs_blind_rt_discovery_prepare(const NR_DL_FRAME_PARMS *) __attribute__((weak));
extern void nr_csirs_blind_rt_discovery_init(void) __attribute__((weak));
extern void nr_csirs_blind_rt_discovery_reset(void) __attribute__((weak));
extern void nr_csirs_monitor_remove_dynamic_all(void) __attribute__((weak));
extern void nr_pdsch_passive_queue_get_stats(nr_pdsch_passive_queue_stats_t *) __attribute__((weak));
extern void nr_csirs_disc_td_feed_params(uint32_t *, uint32_t *) __attribute__((weak));
extern void nr_csirs_disc_td_set_duty(bool, uint32_t, uint32_t, bool) __attribute__((weak));
extern uint64_t csirs_disc_rl_skipped __attribute__((weak));

#define DISC_VIEWS 128
static bool pipeline_ready=true;
static int g_ring_capacity=NR_CSIRS_DISC_DEFAULT_CAPACITY; /* T6b */
int nr_csirs_disc_ring_capacity(void) {return g_ring_capacity;}
/* nr_csirs_disc_pipeline_ready: returns whether the CSI-RS discovery pipeline is usable (false after a failed GPU preallocation or discovery prepare).
 *   Read by nr_csirs_disc_start() in nr_csirs_disc_feed.c (refuses to start when the pipeline is down).
 */
bool nr_csirs_disc_pipeline_ready(void) {return pipeline_ready;}
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static nr_csirs_disc_metrics_t g_metrics;
static pthread_mutex_t pipeline_lock = PTHREAD_MUTEX_INITIALIZER;
typedef struct {nr_csirs_disc_hyp_t h; uint64_t written; bool accepted;} sweep_cache_t;
static sweep_cache_t cache[256];
static int cache_count;
static uint64_t evidence_epoch=1;
/* Refinement changes the score population. Retry that exact geometry in place
 * of ordinary hypotheses; never transfer credit from the coarse extent. */
static struct {nr_csirs_disc_hyp_t h; uint64_t written;} evidence_retry[16];
static int evidence_retries;
/* retry_refined: remembers a refined (extent-refined) hypothesis whose verification failed, so that the next batch re-verifies that exact geometry.
 *   Keeps up to 16 entries (oldest overwritten); a duplicate of an entry already stored is ignored. Caller: nr_csirs_disc_process() (CUMULATIVE).
 */
static void retry_refined(const nr_csirs_disc_hyp_t *h, uint64_t written)
{
  for (int i=0;i<evidence_retries;++i)
    if (nr_csirs_disc_evidence_key(h,&evidence_retry[i].h)) return;
  int at=evidence_retries<16?evidence_retries++:(int)(written%16);
  evidence_retry[at].h=*h; evidence_retry[at].written=written;
}
static struct {nr_csirs_disc_hyp_t h;uint16_t id;} qualified[256];
static int qualified_count;
/* Rate-limit bookkeeping (nr_csirs_disc_rate_internal.h); all under pipeline_lock. */
static float q_strength[256];
static bool q_was[256], rl_epoch;
static nr_csirs_disc_fp_t rl_known[128], rl_pending[8];
static int rl_known_n, rl_pending_n;
static const int16_t *cache_grid;
static uint64_t cache_written;
/* same_seed: true when two hypotheses describe the same CSI-RS seed: same row, symbols, CDM, density, period and offset, with at least 80 % overlap of their RB extents.
 *   Used for the sweep cache and for matching cumulative evidence. Pure function. Callers: cached(), cache_result(), nr_csirs_disc_process().
 */
static bool same_seed(const nr_csirs_disc_hyp_t *a, const nr_csirs_disc_hyp_t *b)
{
  int lo=a->rb_start>b->rb_start?a->rb_start:b->rb_start;
  int hi=a->rb_start+a->n_rb<b->rb_start+b->n_rb?a->rb_start+a->n_rb:b->rb_start+b->n_rb;
  return 5*(hi-lo)>=4*(a->n_rb>b->n_rb?a->n_rb:b->n_rb) && a->row==b->row && a->freq_domain==b->freq_domain && a->symb_l0==b->symb_l0 && a->symb_l1==b->symb_l1
      && a->cdm_type==b->cdm_type && a->density==b->density && a->period==b->period && a->offset==b->offset
      && a->rb_start < b->rb_start+b->n_rb && b->rb_start < a->rb_start+a->n_rb;
}
/* cached: true when this hypothesis's seed was already swept recently and need not be swept again (accepted, or rejected less than a delay ago).
 *   The delay is max(3 x period, 2000) written samples. Caller: nr_csirs_disc_process() when building the hypothesis list.
 */
static bool cached(const nr_csirs_disc_hyp_t *h, uint64_t written)
{
  for(int i=0;i<cache_count;++i) if(same_seed(h,&cache[i].h)) {
    uint64_t delay=3*h->period>2000?3*h->period:2000;
    if(cache[i].accepted || written-cache[i].written<delay) return true;
  }
  return false;
}
/* cache_result: stores the outcome (accepted or not) of a swept seed in the 256-entry sweep cache, at the write position of the ring.
 *   Overwrites an entry with the same seed if present. Caller: nr_csirs_disc_process().
 */
static void cache_result(const nr_csirs_disc_hyp_t *h, uint64_t written, bool accepted)
{
  int at=-1;
  for(int i=0;i<cache_count;++i) if(same_seed(h,&cache[i].h)) {at=i;break;}
  if(at<0) at=cache_count<256?cache_count++:(int)(written%256);
  cache[at]=(sweep_cache_t){*h,written,accepted};
}

/* same_extent: true when two footprints cover the same RB extent (overlap of at least 80 % of the larger one).
 *   Used to merge footprints of different symbol masks into union views. Caller: nr_csirs_disc_process().
 */
static bool same_extent(const nr_csirs_disc_fp_t *a,const nr_csirs_disc_fp_t *b)
{
  int lo=a->rb_start>b->rb_start?a->rb_start:b->rb_start;
  int hi=a->rb_start+a->n_rb<b->rb_start+b->n_rb?a->rb_start+a->n_rb:b->rb_start+b->n_rb;
  return hi>lo && 5*(hi-lo)>=4*(a->n_rb>b->n_rb?a->n_rb:b->n_rb);
}

/* now_us: returns CLOCK_MONOTONIC time in microseconds (batch timing for the discovery metrics). */
static uint64_t now_us(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

/* cpu_us: returns the calling thread's CPU time in microseconds (CPU-cost metrics of a discovery batch). */
static uint64_t cpu_us(void)
{
  struct timespec t;clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t);return (uint64_t)t.tv_sec*1000000+t.tv_nsec/1000;
}

/* epoch_listener: configuration-epoch listener: clears the sweep cache, evidence and the ring, and withdraws discovered CSI-RS resources unless the epoch was a soft RX recovery.
 *   Runs under pipeline_lock. A soft RX recovery keeps the monitor exports (cell configuration), a full reconfiguration removes them via nr_csirs_blind_rt_discovery_reset() and nr_csirs_monitor_remove_dynamic_all().
 *   Registered by init_once() through nr_cfg_epoch_subscribe().
 */
static void epoch_listener(const nr_cfg_epoch_snapshot_t *snapshot)
{
  const bool rx_recovery = snapshot->last_class == NR_EPOCH_SOFT
                        && snapshot->last_cause == NR_CAUSE_RX_RECOVERY;
  pthread_mutex_lock(&pipeline_lock);
  cache_count=qualified_count=evidence_retries=0;cache_grid=NULL;cache_written=0;
  rl_epoch=true;rl_known_n=0;
  ++evidence_epoch;
  nr_csirs_disc_detect_reset();
  /* The IQ feed and partial hypotheses belong to the old sample epoch, but
   * confirmed CSI-RS resources are cell configuration. Keep their monitor
   * exports through a transient RX restart, like the CORESET/RNTI banks. */
  if (!rx_recovery) {
    if (nr_csirs_blind_rt_discovery_reset)
      nr_csirs_blind_rt_discovery_reset();
    if (nr_csirs_monitor_remove_dynamic_all)
      nr_csirs_monitor_remove_dynamic_all();
  }
  nr_csirs_disc_ring_t *ring = nr_csirs_disc_worker_ring();
  nr_csirs_disc_ring_reset(ring);
  pthread_mutex_unlock(&pipeline_lock);
}

/* ---- on-demand ring (PHY/TOOLS/nr_gpu_elastic.h) ----
 * T6b cut the observation ring to what the csirs share of the device budget holds (221 of 640 slots on an 8 GB card). The ring keeps that size (base) and grows in steps of
 * 64 slots from the budget headroom when the hypotheses under test need a longer span: one period for an accumulating hypothesis (row 1: evidence adds up across batches),
 * four periods for the others (the sweep averages up to max_slots = 4 occasions that must lie inside the ring). It gives the extra back after ELASTIC_IDLE_TICKS discovery
 * batches without such a demand. A resize empties the ring (the detector and the evidence caches restart on the new grid) and runs on the worker thread under the pipeline
 * lock. A refused or failed growth leaves the ring as it is (one log, counted in the GPU_MEM census as csirs_dyn=.../denied=...). A share that holds the full ring
 * (GB10, unlimited budget) never enables it: the ring is 640 slots from the start, the code below does not run. */
#define ELASTIC_STEP 64
#define ELASTIC_IDLE_TICKS 600  /* ~30 s at one batch per 100 slots (50 ms at 30 kHz) */
#define ELASTIC_BACKOFF_TICKS 200
static nr_gpu_elastic_t g_el;
static bool g_el_on;
static int g_el_demand,g_el_rb,g_el_ant;
static size_t g_el_per_slot;
/* elastic_demand: slots of ring the n hypotheses under test want (see above); capped at the full ring. */
static int elastic_demand(const nr_csirs_disc_hyp_t *h,int n)
{
  int d=0;
  for(int i=0;i<n;++i) {
    const int need=h[i].period*(h[i].row==1?1:4);
    if(need>d) d=need;
  }
  return d>NR_CSIRS_DISC_DEFAULT_CAPACITY?NR_CSIRS_DISC_DEFAULT_CAPACITY:d;
}
/* elastic_tick: one tick (called with pipeline_lock held, before a batch). Returns true when the ring was replaced by one of another size (it is empty: skip this batch). */
static bool elastic_tick(nr_csirs_disc_ring_t *ring)
{
  if(!g_el_on || ring!=nr_csirs_disc_worker_ring() || !nr_csirs_disc_worker_resizable()) return false;
  const int cur=g_el.cur,want=nr_gpu_elastic_target(&g_el,g_el_demand);
  if(want==cur) return false;
  int to=want;
  size_t take=0;
  if(want>cur) {
    const size_t headroom=nr_gpu_membudget_headroom?nr_gpu_membudget_headroom():SIZE_MAX;
    to=nr_gpu_elastic_fit(&g_el,want,g_el_per_slot,headroom);
    if(to==cur) { /* no headroom for one step: counted by the budget, the ring keeps its size */
      if(nr_gpu_membudget_dyn_acquire) nr_gpu_membudget_dyn_acquire(NR_GPU_MEM_CSIRS,(size_t)ELASTIC_STEP*g_el_per_slot);
      if(nr_gpu_elastic_done(&g_el,want,cur,1))
        LOG_W(PHY,"SENSING: CSIRS_DISC ring growth %d -> %d slots refused (memory budget headroom %zu MB); keeping %d slots, further refusals are only counted\n",cur,want,headroom>>20,cur);
      return false;
    }
    take=(size_t)(to-cur)*g_el_per_slot;
    if(nr_gpu_membudget_dyn_acquire && !nr_gpu_membudget_dyn_acquire(NR_GPU_MEM_CSIRS,take)) {
      if(nr_gpu_elastic_done(&g_el,want,cur,0))
        LOG_W(PHY,"SENSING: CSIRS_DISC ring growth %d -> %d slots refused (memory budget); keeping %d slots\n",cur,to,cur);
      return false;
    }
  }
  const nr_csirs_disc_gpu_ops_t *gpu=nr_csirs_disc_gpu_load();
  const uint64_t t0=now_us();
  int rc=nr_csirs_disc_ring_resize(ring,to);
  if(rc==0 && gpu && gpu->sweep_prepare && gpu->sweep_prepare(g_el_rb,g_el_ant,to)<0) {
    nr_csirs_disc_ring_resize(ring,cur); /* the sweep workspace did not fit: back to the old size (an empty ring either way) */
    rc=-1;
  }
  if(rc!=0) {
    if(take && nr_gpu_membudget_dyn_abort) nr_gpu_membudget_dyn_abort(NR_GPU_MEM_CSIRS,take);
    if(nr_gpu_elastic_done(&g_el,want,cur,0))
      LOG_W(PHY,"SENSING: CSIRS_DISC ring resize %d -> %d slots failed (allocation); keeping %d slots, further failures are only counted\n",cur,to,cur);
    return false;
  }
  if(to<cur && nr_gpu_membudget_dyn_release) nr_gpu_membudget_dyn_release(NR_GPU_MEM_CSIRS,(size_t)(cur-to)*g_el_per_slot);
  nr_gpu_elastic_done(&g_el,want,to,1);
  g_ring_capacity=to;
  LOG_A(PHY,"SENSING: CSIRS_DISC ring %d -> %d slots (demand %d, base %d, full %d; grows=%llu shrinks=%llu denied=%llu; %llu us)\n",cur,to,g_el_demand,g_el.base,g_el.full,
        (unsigned long long)g_el.grows,(unsigned long long)g_el.shrinks,(unsigned long long)g_el.denied,(unsigned long long)(now_us()-t0));
  return true;
}

/* init_once: one-time setup run through pthread_once from nr_csirs_disc_pipeline_init(): initialises the search lock, loads the GPU ops, warms the hypothesis builder and installs the processing hook.
 *   Subscribes epoch_listener() to configuration epochs when the epoch service is present.
 */
static void init_once(void)
{
  if(nr_csirs_blind_rt_discovery_init) nr_csirs_blind_rt_discovery_init();
  (void)nr_csirs_disc_gpu_load();
  nr_csirs_disc_fp_t warm={.sym_mask=1,.sc_mask=0x111,.density=3,.n_rb=4,.period=4};
  nr_csirs_disc_hyp_t hyp;nr_csirs_disc_hypotheses(&warm,1,&hyp,1);
  nr_csirs_disc_process_hook = nr_csirs_disc_process;
  if (nr_cfg_epoch_subscribe)
    nr_cfg_epoch_subscribe(epoch_listener);
}

/* nr_csirs_disc_pipeline_init: prepares the discovery pipeline for a carrier: runs init_once(), preallocates the GPU ring and buffers for the frame parameters, and runs the discovery prepare of the search.
 *   On failure sets pipeline_ready = false and disables discovery. Enabled only when ISAC_CSIRS_DISC is on. Caller: nr_csirs_disc_start() in nr_csirs_disc_feed.c.
 */
void nr_csirs_disc_pipeline_init(const NR_DL_FRAME_PARMS *fp)
{
  if (nr_csirs_disc_enabled()) {
    pthread_once(&g_once, init_once);
    const nr_csirs_disc_gpu_ops_t *gpu=nr_csirs_disc_gpu_load();
    if(fp && gpu && gpu->prepare) {
      size_t stride=14*(fp->ofdm_symbol_size+fp->nb_prefix_samples0+fp->nb_prefix_samples);
      /* T6b: ring capacity from the shared memory budget (640 when it fits: GB10 unchanged); the floor not fitting = GPU discovery not ready -> CPU discovery */
      const size_t share=nr_gpu_membudget_share?nr_gpu_membudget_share(NR_GPU_MEM_CSIRS):SIZE_MAX;
      const int capacity=nr_csirs_disc_mem_capacity(share,fp->N_RB_DL,fp->nb_antennas_rx,fp->ofdm_symbol_size,stride,NR_CSIRS_DISC_DEFAULT_BATCH,NR_CSIRS_DISC_DEFAULT_CAPACITY);
      if(!capacity) {
        LOG_W(PHY,"SENSING: CSIRS_DISC GPU memory budget share %zu MB < %zu MB for the smallest ring (%d slots); GPU discovery not ready, CPU discovery\n",share>>20,
              (nr_csirs_disc_mem_fixed(fp->N_RB_DL,fp->nb_antennas_rx,fp->ofdm_symbol_size,stride,NR_CSIRS_DISC_DEFAULT_BATCH)+NR_CSIRS_DISC_CAPACITY_MIN*nr_csirs_disc_mem_per_slot(fp->N_RB_DL,fp->nb_antennas_rx))>>20,NR_CSIRS_DISC_CAPACITY_MIN);
        if(nr_gpu_membudget_commit) nr_gpu_membudget_commit(NR_GPU_MEM_CSIRS,0,0.0);
        nr_csirs_disc_gpu_disable();
        gpu=NULL;
      } else {
      g_ring_capacity=capacity;
      const size_t bytes=nr_csirs_disc_mem_fixed(fp->N_RB_DL,fp->nb_antennas_rx,fp->ofdm_symbol_size,stride,NR_CSIRS_DISC_DEFAULT_BATCH)+(size_t)capacity*nr_csirs_disc_mem_per_slot(fp->N_RB_DL,fp->nb_antennas_rx);
      const size_t full=nr_csirs_disc_mem_fixed(fp->N_RB_DL,fp->nb_antennas_rx,fp->ofdm_symbol_size,stride,NR_CSIRS_DISC_DEFAULT_BATCH)+(size_t)NR_CSIRS_DISC_DEFAULT_CAPACITY*nr_csirs_disc_mem_per_slot(fp->N_RB_DL,fp->nb_antennas_rx);
      if(nr_gpu_membudget_commit) nr_gpu_membudget_commit(NR_GPU_MEM_CSIRS,bytes,(double)bytes/(double)full);
      g_el_per_slot=nr_csirs_disc_mem_per_slot(fp->N_RB_DL,fp->nb_antennas_rx);g_el_rb=fp->N_RB_DL;g_el_ant=fp->nb_antennas_rx;g_el_demand=0;
      nr_gpu_elastic_init(&g_el,capacity,NR_CSIRS_DISC_DEFAULT_CAPACITY,ELASTIC_STEP,ELASTIC_IDLE_TICKS,ELASTIC_BACKOFF_TICKS);
      g_el_on=nr_gpu_elastic_enabled(&g_el) && share!=SIZE_MAX; /* a ring cut by the budget share: grows on demand; the full ring (GB10, unlimited budget) never moves */
      if(g_el_on) LOG_I(PHY,"SENSING: CSIRS_DISC on-demand ring: base %d of %d slots (the budget share holds %d), +%d slots per step from the budget headroom\n",capacity,NR_CSIRS_DISC_DEFAULT_CAPACITY,capacity,ELASTIC_STEP);
      if(capacity!=NR_CSIRS_DISC_DEFAULT_CAPACITY)
        LOG_W(PHY,"SENSING: CSIRS_DISC ring capacity %d -> %d slots (memory budget share %zu MB, need %zu MB)\n",NR_CSIRS_DISC_DEFAULT_CAPACITY,capacity,share>>20,bytes>>20);
      if(!gpu->sweep || !gpu->refine || !gpu->confirm || !gpu->sweep_prepare || gpu->prepare(fp->N_RB_DL,fp->nb_antennas_rx,capacity,fp->ofdm_symbol_size,stride)<0
         || (gpu->sweep_prepare && gpu->sweep_prepare(fp->N_RB_DL,fp->nb_antennas_rx,capacity)<0))
        {
          /* T6b: an allocation failure makes the GPU discovery not ready (CPU discovery), not the discovery off */
          LOG_E(PHY,"SENSING: CSIRS_DISC GPU preallocation failed; GPU discovery not ready, CPU discovery\n");
          if(nr_gpu_membudget_commit) nr_gpu_membudget_commit(NR_GPU_MEM_CSIRS,0,0.0);
          nr_csirs_disc_gpu_disable();
        }
      }
    }
    if(fp && pipeline_ready && nr_csirs_blind_rt_discovery_prepare && nr_csirs_blind_rt_discovery_prepare(fp)<0) pipeline_ready=false;
    if(!pipeline_ready && nr_csirs_blind_rt_discovery_disable) nr_csirs_blind_rt_discovery_disable();
  }
}


/* off_near: true when two occasion offsets of period p are equal or adjacent slots (a TRS pair / two-offset resource shows either). */
static bool off_near(unsigned a, unsigned b, unsigned p)
{
  if (!p) return a == b;
  const unsigned d = (a + p - b) % p;
  return d <= 1 || d == p - 1;
}

/* rl_update: Builds the observation (confirmed set of this module's winners, footprint novelty, CRC counters),
 *   steps the state machine and applies the ingest duty to the TD front end. Caller: nr_csirs_disc_process() under pipeline_lock.
 */
static void rl_update(const nr_csirs_disc_ring_t *ring, const nr_csirs_disc_fp_t *fps, int n_fp)
{
  static nr_csirs_rl_t st;
  static nr_csirs_rl_cfg_t cfg;
  static bool init;
  if (!init) {
    uint32_t win = 640, div = 7;
    if (nr_csirs_disc_td_feed_params) nr_csirs_disc_td_feed_params(&win, &div);
    cfg = (nr_csirs_rl_cfg_t){win, div, (uint64_t)(60.0 * ring->slots_per_frame * 100), 0.01, 0.5, 2};
    init = true;
    LOG_I(PHY, "SENSING: CSIRS_DISC RATELIMIT cfg win=%u div=%u notice_slots=%llu checks=%u cycles=%d\n", win, div,
          (unsigned long long)cfg.notice_slots, nr_csirs_rl_checks(&cfg), cfg.stable_cycles);
  }
  nr_csirs_rl_obs_t o = {.epoch = rl_epoch};
  rl_epoch = false;
  if (ring->written) o.slot = ring->meta[(ring->written - 1) % ring->capacity].abs_slot;
  float min_strength = 1e30f;
  uint64_t seen = 0; /* distinct confirmed blind_rt resources matched (tolerant to widening/merging; two hypotheses of one resource count once) */
  for (int i = 0; i < qualified_count; ++i) {
    const int k = nr_csirs_blind_rt_discovery_match ? nr_csirs_blind_rt_discovery_match(&qualified[i].h) : -1;
    const bool c = k >= 0;
    o.lost += q_was[i] && !c;
    if (c) { q_was[i] = true; seen |= 1ull << k; }
    else if (!st.stable) q_was[i] = false; /* in SEARCH a loss is an edge; in STABLE it persists and is debounced by the state machine */
    if (!c) continue;
    if (qualified[i].h.period > o.max_period) o.max_period = qualified[i].h.period;
    if (q_strength[i] < min_strength) min_strength = q_strength[i];
  }
  o.n_conf = __builtin_popcountll(seen);
  /* new pattern: a footprint outside the learned baseline (period, density, symbols, extent, offset within +-1 slot = TRS pair), as strong as the
   * weakest confirmed one, AND seen again in the next observation (noise footprints do not recur at the same place) */
  nr_csirs_disc_fp_t pend[8]; int pend_n = 0;
  for (int i = 0; i < n_fp; ++i) {
    bool known = false;
    for (int k = 0; k < rl_known_n && !known; ++k)
      known = rl_known[k].period == fps[i].period && off_near(rl_known[k].offset, fps[i].offset, fps[i].period)
           && rl_known[k].density == fps[i].density && (rl_known[k].sym_mask & fps[i].sym_mask)
           && same_extent(&rl_known[k], &fps[i]);
    if (known) continue;
    if (!st.stable) { if (rl_known_n < 128) rl_known[rl_known_n++] = fps[i]; }
    else if (o.n_conf && fps[i].strength >= min_strength) {
      bool again = false;
      for (int k = 0; k < rl_pending_n && !again; ++k)
        again = rl_pending[k].period == fps[i].period && rl_pending[k].offset == fps[i].offset
             && rl_pending[k].density == fps[i].density && (rl_pending[k].sym_mask & fps[i].sym_mask) && same_extent(&rl_pending[k], &fps[i]);
      if (again) o.new_pattern = true;
      if (pend_n < 8) pend[pend_n++] = fps[i];
    }
  }
  memcpy(rl_pending, pend, sizeof(pend)); rl_pending_n = pend_n;
  if (nr_pdsch_passive_queue_get_stats) {
    nr_pdsch_passive_queue_stats_t pq;
    nr_pdsch_passive_queue_get_stats(&pq);
    o.crc_ok = pq.crc_ok; o.crc_dec = pq.decoded;
  }
  const bool was = st.stable;
  const bool changed = nr_csirs_rl_step(&st, &cfg, &o);
  static uint64_t wait_log; /* why STABLE is not reached, every 30 s of stream time */
  if (!st.stable && o.slot >= wait_log + (uint64_t)ring->slots_per_frame * 100 * 30) {
    wait_log = o.slot;
    LOG_I(PHY, "SENSING: CSIRS_DISC RATELIMIT WAIT n_conf=%d lost=%d slot=%llu since_change=%llu need=%llu\n", o.n_conf, o.lost,
          (unsigned long long)o.slot, (unsigned long long)(o.slot - st.last_change), (unsigned long long)st.need);
  }
  if (changed || was != st.stable) {
    if (nr_csirs_disc_td_set_duty) nr_csirs_disc_td_set_duty(st.stable, st.G, st.div_s, st.rearm);
    struct timespec rts; clock_gettime(CLOCK_MONOTONIC, &rts);
    LOG_I(PHY, "SENSING: CSIRS_DISC RATELIMIT %s reason=%s n_conf=%d slot=%llu G=%u div_s=%u p0=%.3f skipped=%llu t=%ld.%03ld\n",
          st.stable ? "STABLE" : "SEARCH", st.reason, o.n_conf, (unsigned long long)o.slot, st.G, st.div_s, st.p0,
          &csirs_disc_rl_skipped ? (unsigned long long)csirs_disc_rl_skipped : 0ull, (long)rts.tv_sec, rts.tv_nsec / 1000000);
    st.rearm = false;
  }
}

int nr_csirs_disc_test_sweep_limit = 32; /* hypotheses swept per batch; tests raise it */

/* nr_csirs_disc_process: one discovery batch on a snapshot of the IQ ring: detects footprints, builds hypotheses, sweeps them (GPU or CPU), gates the winners, refines and verifies them, and admits the survivors to the CSI-RS search.
 *   Steps: footprints (nr_csirs_disc_detect_candidates) -> union views -> hypotheses (cache and confirmed-skip filters) -> sweep -> per-footprint best -> gate -> refine -> verify -> nr_csirs_blind_rt_add_discovered().
 *   Logs WINNER, AMBIGUOUS and BATCH lines.
 *   Takes pipeline_lock for the batch and g_lock for the metrics. Caller: the discovery worker through nr_csirs_disc_process_hook (nr_csirs_disc_feed.c); tests.
 */
void nr_csirs_disc_process(nr_csirs_disc_ring_t *ring)
{
  if (!ring || !nr_csirs_disc_enabled())
    return;
  pthread_mutex_lock(&pipeline_lock);
  if(elastic_tick(ring)) {pthread_mutex_unlock(&pipeline_lock);return;} /* the ring was replaced by an empty one of another size: the next batch starts on it */
  if(!nr_csirs_disc_ring_snapshot_current(ring)) {pthread_mutex_unlock(&pipeline_lock);return;}
  if(cache_grid!=ring->grid_base || ring->written<cache_written) {
    cache_count=qualified_count=evidence_retries=0; ++evidence_epoch;
    rl_epoch=true;rl_known_n=0;
  }
  cache_grid=ring->grid_base;cache_written=ring->written;
  const uint64_t begin = now_us(), cpu_begin=cpu_us();
  nr_csirs_disc_fp_t fps[DISC_VIEWS];
  nr_csirs_disc_hyp_t all_hyps[NR_CSIRS_DISC_MAX_HYP];
  nr_csirs_disc_hyp_t hyps[NR_CSIRS_DISC_MAX_HYP];
  nr_csirs_disc_win_t wins[NR_CSIRS_DISC_MAX_HYP];
  bool coarse_only[NR_CSIRS_DISC_MAX_HYP] = {false}; /* screened out: never eligible for admission */
  const nr_csirs_disc_fp_cfg_t fp_cfg = {.min_strength = 1.03f};

  /* Resident GPU normalization, folding and clustering; serial CPU reference. */
  int n_fp = nr_csirs_disc_detect_candidates(ring, &fp_cfg, fps, 64);
  if(n_fp<0) { /* Recover after a full ring overrun; unavailable IQ cannot be folded. */
    nr_csirs_disc_detect_reset();
    n_fp=nr_csirs_disc_detect_candidates(ring,&fp_cfg,fps,64);
  }
  const uint64_t detected = now_us(), cpu_detected=cpu_us();
  if (n_fp < 0) {pthread_mutex_unlock(&pipeline_lock);return;}
  for (int i = 0; i < n_fp; ++i)
    LOG_I(PHY,
          "SENSING: CSIRS_DISC FOOTPRINT sym=0x%x sc=0x%x dens=%u rb=%u:%u P/O=%u/%u strength=%.3f\n",
          fps[i].sym_mask,
          fps[i].sc_mask,
          fps[i].density,
          fps[i].rb_start,
          fps[i].n_rb,
          fps[i].period,
          fps[i].offset,
          fps[i].strength);

  /* Keep all single-symbol row-1 views adjacent in priority order. */
  int n_views=n_fp;
  for(int i=0;i<n_views;++i) if(fps[i].density==3 && __builtin_popcount(fps[i].sym_mask)>1) {
    unsigned symbols=fps[i].sym_mask;
    fps[i].sym_mask=symbols&-symbols;symbols &= symbols-1;
    while(symbols && n_views<DISC_VIEWS) {
      memmove(&fps[i+2],&fps[i+1],(n_views-i-1)*sizeof(*fps));
      fps[i+1]=fps[i];fps[i+1].sym_mask=symbols&-symbols;
      symbols &= symbols-1;++n_views;++i;
    }
  }
  /* Multi-symbol/CDM rows can appear as one detector footprint per distinct
   * per-symbol comb. Add bounded union views; U3 accepts one only when the
   * union is an exact legal 38.211 row, so unrelated colocated pilots do not
   * become candidates. Preserve the detector outputs for narrow rows. */
  int n_fp_hyp = n_views;
  for (int i = 0; i < n_views && n_fp_hyp < DISC_VIEWS; ++i)
    for (int j = i + 1; j < n_views && n_fp_hyp < DISC_VIEWS; ++j)
      if (fps[i].density != 3 && fps[i].period == fps[j].period && fps[i].offset == fps[j].offset && fps[i].density == fps[j].density
          && same_extent(&fps[i],&fps[j])) {
        nr_csirs_disc_fp_t u = fps[i];
        u.sym_mask |= fps[j].sym_mask;
        u.sc_mask |= fps[j].sc_mask;
        u.strength = fps[i].strength < fps[j].strength ? fps[i].strength : fps[j].strength;
        bool duplicate = false;
        for (int k = 0; k < n_fp_hyp; ++k)
          duplicate |= fps[k].sym_mask == u.sym_mask && fps[k].sc_mask == u.sc_mask && fps[k].density == u.density
              && fps[k].rb_start == u.rb_start && fps[k].n_rb == u.n_rb && fps[k].period == u.period
              && fps[k].offset == u.offset;
        if (!duplicate)
          fps[n_fp_hyp++] = u;
      }
  for (int i = 0; i < n_views && n_fp_hyp < DISC_VIEWS; ++i) {
    nr_csirs_disc_fp_t u = fps[i];
    for (int j = i + 1; j < n_views; ++j)
      if (u.period == fps[j].period && u.offset == fps[j].offset && u.density == fps[j].density
          && same_extent(&u,&fps[j])) {
        u.sym_mask |= fps[j].sym_mask;
        u.sc_mask |= fps[j].sc_mask;
        if (fps[j].strength < u.strength)
          u.strength = fps[j].strength;
      }
    bool duplicate = false;
    for (int k = 0; k < n_fp_hyp; ++k)
      duplicate |= fps[k].sym_mask == u.sym_mask && fps[k].sc_mask == u.sc_mask && fps[k].density == u.density
          && fps[k].rb_start == u.rb_start && fps[k].n_rb == u.n_rb && fps[k].period == u.period
          && fps[k].offset == u.offset;
    if (!duplicate)
      fps[n_fp_hyp++] = u;
  }

  const nr_csirs_disc_gpu_ops_t *ops = nr_csirs_disc_gpu_load();
  int budget = nr_csirs_disc_test_sweep_limit;
  const int n_all = nr_csirs_disc_hypotheses(fps, n_fp_hyp, all_hyps, NR_CSIRS_DISC_MAX_HYP);
  int n_hyp = 0;
  for (int i=0;i<evidence_retries && n_hyp<budget/4;++i) {
    nr_csirs_disc_hyp_t h=evidence_retry[i].h;
    if (ring->written-evidence_retry[i].written>10000 || cached(&h,ring->written)) continue;
    // Keep only currently observed persistent footprints; no blind stale probes.
    for (int j=0;j<n_all;++j) if (same_seed(&h,&all_hyps[j])) {
      h.fp_index=all_hyps[j].fp_index;
      if (!nr_csirs_blind_rt_discovery_confirmed || !nr_csirs_blind_rt_discovery_confirmed(&h))
        hyps[n_hyp++]=h;
      break;
    }
  }
  // Priority changes ordering only, not the feed, sweep cap, planner or GPU budget.
  for (int pass=0;pass<2;++pass)
    for (int i=0;i<n_all && n_hyp<budget;++i) {
      const bool trs=all_hyps[i].row==1 && all_hyps[i].period>=80;
      if (trs!=(pass==0)) continue;
      bool duplicate=false;
      for (int j=0;j<n_hyp;++j)
        duplicate |= nr_csirs_disc_evidence_key(&hyps[j],&all_hyps[i]);
      if (!duplicate && !cached(&all_hyps[i],ring->written)
          && (!nr_csirs_blind_rt_discovery_confirmed || !nr_csirs_blind_rt_discovery_confirmed(&all_hyps[i])))
        hyps[n_hyp++]=all_hyps[i];
    }
  struct timespec hts; clock_gettime(CLOCK_MONOTONIC, &hts); /* OTATAIL: align batches with PDTIMSLOW t= */
  LOG_I(PHY, "SENSING: CSIRS_DISC HYPOTHESES n=%d sweep=%d t=%ld.%03ld\n", n_all, n_hyp, (long)hts.tv_sec, hts.tv_nsec / 1000000);

  if(g_el_on) g_el_demand=elastic_demand(hyps,n_hyp);
  const uint64_t built = now_us(), cpu_built=cpu_us();
  int n_win = 0;
  bool gpu = false;
  const nr_csirs_disc_sweep_cfg_t sweep_cfg = {
      .id_first = 0, .id_count = 1024, .delta_max = 1, .block_re = 32, .max_slots = 4,
      .evidence_epoch = evidence_epoch};
  if (n_hyp > 0 && ops && ops->sweep) {
    n_win = ops->sweep(ring, hyps, n_hyp, &sweep_cfg, wins);
    gpu = n_win >= 0;
  }
  if (n_hyp > 0 && n_win < 0)
    n_win = 0;
  if(n_hyp>0 && !gpu && ring->device_valid) {pthread_mutex_unlock(&pipeline_lock);return;}
  if (n_hyp > 0 && !gpu) {
    n_win=0;
    for(int i=0;i<n_hyp;++i) {
      /* Expensive multiport hypotheses first face a bounded CPU screen. The
       * complete geometry/occasion sweep is still mandatory for admission. */
      nr_csirs_disc_hyp_t coarse=hyps[i];
      nr_csirs_disc_sweep_cfg_t screen=sweep_cfg;screen.max_slots=1;
      if(coarse.n_rb>24) coarse.n_rb=24;
      int ok=1;
      if(hyps[i].row>2) {
        ok=nr_csirs_disc_sweep_cpu(ring,&coarse,1,&screen,&wins[i]);
        if(ok==1 && wins[i].z>=3) ok=nr_csirs_disc_sweep_cpu(ring,&hyps[i],1,&sweep_cfg,&wins[i]);
        else coarse_only[i]=true;
      } else ok=nr_csirs_disc_sweep_cpu(ring,&hyps[i],1,&sweep_cfg,&wins[i]);
      if(ok!=1) break;
      wins[i].hyp_index=i;++n_win;
    }
  }

  uint64_t winners = 0, ambiguous = 0;
  if (n_win > 0) {
    int best[DISC_VIEWS], leader[DISC_VIEWS];
    for (int f = 0; f < DISC_VIEWS; ++f)
      best[f] = leader[f] = -1;
    for (int i = 0; i < n_win; ++i) {
      const nr_csirs_disc_win_t *w = &wins[i];
      if (w->hyp_index < 0 || w->hyp_index >= n_hyp)
        continue;
      const nr_csirs_disc_hyp_t *h = &hyps[w->hyp_index];
      if (h->fp_index >= DISC_VIEWS)
        continue;
      const int lead = leader[h->fp_index];
      if (lead < 0 || w->z > wins[lead].z)
        leader[h->fp_index] = i;
      const int old = best[h->fp_index];
      if (old < 0 || (h->row < hyps[wins[old].hyp_index].row
                      && w->z + 0.5f >= wins[leader[h->fp_index]].z))
        best[h->fp_index] = i;
    }

    const nr_csirs_disc_gate_t gate = nr_csirs_disc_gate_config();
    static uint64_t gate_logged;
    static bool gate_has_logged;
    if (!gate_has_logged || g_metrics.batches - gate_logged >= 50) {
      gate_logged = g_metrics.batches;
      gate_has_logged = true;
      LOG_I(PHY, "SENSING: CSIRS_DISC WIN_GATE mode=%s K=%.3f M=%.3f floor=%.3f fixed=%.3f\n",
            gate.fixed > 0 ? "legacy" : "per-sweep", gate.k, gate.m, gate.floor, gate.fixed);
    }
    int selected[DISC_VIEWS], n_selected = 0;
    for (int f = 0; f < DISC_VIEWS; ++f) {
      const int i = best[f];
      if (i < 0)
        continue;
      const nr_csirs_disc_win_t *w = &wins[i];
      const nr_csirs_disc_hyp_t *h = &hyps[w->hyp_index];
      const bool accumulated=h->row==1 && sweep_cfg.delta_max==1;
      // Re-verification may read a cached vector, but admission needs new IQ.
      if (accumulated && !w->fresh_slots) {
        cache_result(h,ring->written,false);
        continue;
      }
      const bool accepted = !coarse_only[w->hyp_index] && nr_csirs_disc_gate_accept(w, &gate);
      if (accepted) {
        int replace = -1;
        for (int k = 0; k < n_selected; ++k) {
          const nr_csirs_disc_win_t *other_w = &wins[selected[k]];
          const nr_csirs_disc_hyp_t *other_h = &hyps[other_w->hyp_index];
          if (other_h->period == h->period && other_h->offset == h->offset
              && (fps[other_h->fp_index].sym_mask & fps[h->fp_index].sym_mask)
              && other_w->scramb_id == w->scramb_id && other_h->rb_start == h->rb_start && other_h->n_rb == h->n_rb) {
            const int symbols = __builtin_popcount((unsigned)fps[h->fp_index].sym_mask);
            const int other_symbols = __builtin_popcount((unsigned)fps[other_h->fp_index].sym_mask);
            if (w->z > other_w->z + 0.1f || (w->z + 0.1f >= other_w->z && symbols > other_symbols))
              replace = k;
            else
              replace = -2;
            break;
          }
        }
        if (replace >= 0)
          selected[replace] = i;
        else if (replace == -1 && n_selected < DISC_VIEWS)
          selected[n_selected++] = i;
      } else {
        ambiguous++;
        cache_result(h,ring->written,false);
        if(ambiguous==1 && g_metrics.batches%20==0) LOG_I(PHY,
              "SENSING: CSIRS_DISC AMBIGUOUS row=%u fd=%u l0=%u l1=%u id=%u z=%.6f z2=%.6f mu=%.6f sigma=%.6f P/O=%u/%u\n",
              h->row,
              h->freq_domain,
              h->symb_l0,
              h->symb_l1,
              w->scramb_id,
              w->z,
              w->z_second,
              w->noise_mu,
              w->noise_sigma,
              h->period,
              h->offset);
      }
    }

    for (int k = 0; k < n_selected; ++k) {
      const nr_csirs_disc_win_t *w = &wins[selected[k]];
      const int original_index=w->hyp_index;
      nr_csirs_disc_hyp_t refined = hyps[w->hyp_index];
      if ((gpu && ops->refine ? ops->refine(ring,&refined,w) : nr_csirs_disc_refine_cpu(ring, &refined, w)) < 0) continue;
      /* A weaker burst phase must not create a cropped clone of a wider,
       * already sequence-qualified resource. Reuse that measured geometry and
       * verify the entire extent again on this phase before admission. */
      for(int q=0;q<qualified_count;++q) if(qualified[q].id==w->scramb_id) {
        nr_csirs_disc_hyp_t canonical=qualified[q].h;canonical.offset=refined.offset;
        if(canonical.n_rb>refined.n_rb && same_seed(&canonical,&refined)) refined=canonical;
      }
      nr_csirs_disc_win_t verified;
      int checked=gpu && ops->sweep?ops->sweep(ring,&refined,1,&sweep_cfg,&verified):-1;
      if(checked<0 && !ring->device_valid) checked=nr_csirs_disc_sweep_cpu(ring,&refined,1,&sweep_cfg,&verified);
      if(checked!=1
          || !nr_csirs_disc_gate_accept(&verified, &gate) || verified.scramb_id!=w->scramb_id) {
        if (checked==1 && refined.row==1 && sweep_cfg.delta_max==1)
          retry_refined(&refined,ring->written);
        continue;
      }
      w=&verified;
      const nr_csirs_disc_hyp_t *h = &refined;
      if (nr_csirs_blind_rt_add_discovered && nr_csirs_blind_rt_add_discovered(h, w) >= 0) {
        winners++;
        if(qualified_count<256) {
          q_strength[qualified_count]=h->fp_index<DISC_VIEWS?fps[h->fp_index].strength:0;q_was[qualified_count]=false;
          qualified[qualified_count].h=*h;qualified[qualified_count++].id=w->scramb_id;
        }
        cache_result(&hyps[original_index],ring->written,true);
      }
      LOG_A(PHY,
            "SENSING: CSIRS_DISC WINNER row=%u fd=%u l0=%u l1=%u cdm=%u dens=%u rb=%u:%u id=%u delta=%d z=%.6f z2=%.6f mu=%.6f sigma=%.6f P/O=%u/%u\n",
            h->row,
            h->freq_domain,
            h->symb_l0,
            h->symb_l1,
            h->cdm_type,
            h->density,
            h->rb_start,
            h->n_rb,
            w->scramb_id,
            w->slot_delta,
            w->z,
            w->z_second,
            w->noise_mu,
            w->noise_sigma,
            h->period,
            h->offset);
    }
  }

  rl_update(ring, fps, n_fp);
  pthread_mutex_lock(&g_lock);
  uint64_t cpu_end=cpu_us();
  g_metrics.cpu_us+=cpu_end-cpu_begin;g_metrics.detect_cpu_us+=cpu_detected-cpu_begin;
  g_metrics.hyp_cpu_us+=cpu_built-cpu_detected;g_metrics.sweep_cpu_us+=cpu_end-cpu_built;
  g_metrics.detect_us += detected - begin;
  g_metrics.hyp_us += built - detected;
  g_metrics.sweep_us += now_us() - built;
  g_metrics.batches++;
  g_metrics.footprints += (uint64_t)n_fp;
  g_metrics.hyps += (uint64_t)n_all;
  g_metrics.winners += winners;
  g_metrics.ambiguous += ambiguous;
  g_metrics.feed_drops = __atomic_load_n(&csirs_disc_feed_drops, __ATOMIC_RELAXED);
  g_metrics.gpu_batches += gpu;
  g_metrics.cpu_batches += !gpu;
  uint64_t elapsed=now_us()-begin;
  g_metrics.batch_us += elapsed;
  if(g_metrics.batches%20==0) LOG_I(PHY,
      "SENSING: CSIRS_DISC BATCH total_us=%llu detect_us=%llu hyp_us=%llu sweep_us=%llu gpu_detect=%llu cpu_detect=%llu gpu_lattice=%llu cpu_lattice=%llu sweep_backend=%s\n",
      (unsigned long long)elapsed,(unsigned long long)(detected-begin),(unsigned long long)(built-detected),
      (unsigned long long)(now_us()-built),
      (unsigned long long)__atomic_load_n(&csirs_disc_gpu_detect_batches,__ATOMIC_RELAXED),
      (unsigned long long)__atomic_load_n(&csirs_disc_cpu_detect_batches,__ATOMIC_RELAXED),
      (unsigned long long)__atomic_load_n(&csirs_disc_gpu_lattice_calls,__ATOMIC_RELAXED),
      (unsigned long long)__atomic_load_n(&csirs_disc_cpu_lattice_calls,__ATOMIC_RELAXED),gpu?"gpu":"cpu/idle");
  pthread_mutex_unlock(&g_lock);
  pthread_mutex_unlock(&pipeline_lock);
}

/* nr_csirs_disc_get_metrics: copies the discovery pipeline metrics (batches, footprints, hypotheses, winners, CPU and GPU times, feed drops) into *out.
 *   Takes g_lock. Callers: nr_passive_metrics.c (metrics emission) and tests.
 */
void nr_csirs_disc_get_metrics(nr_csirs_disc_metrics_t *out)
{
  if (!out)
    return;
  pthread_mutex_lock(&g_lock);
  *out = g_metrics;
  out->feed_drops = __atomic_load_n(&csirs_disc_feed_drops, __ATOMIC_RELAXED);
  pthread_mutex_unlock(&g_lock);
}
