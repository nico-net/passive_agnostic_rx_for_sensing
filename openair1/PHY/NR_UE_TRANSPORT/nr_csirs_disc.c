/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_csirs_disc.h"
#include "PHY/defs_nr_common.h"
#include "nr_csirs_disc_gate_internal.h"
#include "nr_csirs_disc_evidence_internal.h"

#include "common/utils/LOG/log.h"
#include "nr_passive_cfg_epoch.h"

#include <pthread.h>
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
extern void nr_csirs_blind_rt_discovery_disable(void) __attribute__((weak));
extern int nr_csirs_blind_rt_discovery_prepare(const NR_DL_FRAME_PARMS *) __attribute__((weak));
extern void nr_csirs_blind_rt_discovery_init(void) __attribute__((weak));
extern void nr_csirs_blind_rt_discovery_reset(void) __attribute__((weak));
extern void nr_csirs_monitor_remove_dynamic_all(void) __attribute__((weak));

#define DISC_VIEWS 128
static bool pipeline_ready=true;
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
/* feature: returns true when the environment variable `name` is exactly "1" (feature flag reader for the ISAC_CSIRS_DISC_* switches).
 *   Read on every batch; callers are nr_csirs_disc_process() (CUMULATIVE, TRS_PRIORITY).
 */
static bool feature(const char *name)
{
  const char *s=getenv(name); return s && !strcmp(s,"1");
}
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

/* sweep_limit: returns ISAC_CSIRS_DISC_SWEEP_HYPS (default 32, max NR_CSIRS_DISC_MAX_HYP): the number of hypotheses swept per batch.
 *   Invalid values fall back to 32. Caller: nr_csirs_disc_process().
 */
static int sweep_limit(void)
{
  const char *s = getenv("ISAC_CSIRS_DISC_SWEEP_HYPS");
  const long value = s ? strtol(s, NULL, 10) : 32;
  return value > 0 && value <= NR_CSIRS_DISC_MAX_HYP ? (int)value : 32;
}

/* delta_limit: returns ISAC_CSIRS_DISC_DELTA_MAX (default 1): the largest slot-delta searched by the sweep, clipped to the slots per frame.
 *   Caller: nr_csirs_disc_process() (sweep configuration).
 */
static int delta_limit(const nr_csirs_disc_ring_t *ring)
{
  const char *s = getenv("ISAC_CSIRS_DISC_DELTA_MAX");
  const long value = s ? strtol(s, NULL, 10) : 1;
  return value > 0 && value <= ring->slots_per_frame ? (int)value : 1;
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
      const char *e=getenv("ISAC_CSIRS_DISC_RING");int capacity=e?atoi(e):640;
      if(capacity<1) capacity=640;
      size_t stride=14*(fp->ofdm_symbol_size+fp->nb_prefix_samples0+fp->nb_prefix_samples);
      if(!gpu->sweep || !gpu->refine || !gpu->confirm || !gpu->sweep_prepare || gpu->prepare(fp->N_RB_DL,fp->nb_antennas_rx,capacity,fp->ofdm_symbol_size,stride)<0
         || (gpu->sweep_prepare && gpu->sweep_prepare(fp->N_RB_DL,fp->nb_antennas_rx,capacity)<0))
        {pipeline_ready=false;LOG_E(PHY,"SENSING: CSIRS_DISC GPU preallocation failed; discovery disabled\n");}
    }
    if(fp && pipeline_ready && nr_csirs_blind_rt_discovery_prepare && nr_csirs_blind_rt_discovery_prepare(fp)<0) pipeline_ready=false;
    if(!pipeline_ready && nr_csirs_blind_rt_discovery_disable) nr_csirs_blind_rt_discovery_disable();
  }
}

/* nr_csirs_disc_process: one discovery batch on a snapshot of the IQ ring: detects footprints, builds hypotheses, sweeps them (GPU or CPU), gates the winners, refines and verifies them, and admits the survivors to the CSI-RS search.
 *   Steps: footprints (nr_csirs_disc_detect_candidates) -> union views -> hypotheses (cache and confirmed-skip filters, ISAC_CSIRS_DISC_CUMULATIVE/_TRS_PRIORITY) -> sweep -> per-footprint best -> gate -> refine -> verify -> nr_csirs_blind_rt_add_discovered().
 *   Flags: ISAC_CSIRS_DISC_DIAG, ISAC_CSIRS_DISC_SWEEP_HYPS, ISAC_CSIRS_DISC_DELTA_MAX, ISAC_CSIRS_DISC_CUMULATIVE, ISAC_CSIRS_DISC_TRS_PRIORITY. Logs WINNER, AMBIGUOUS and BATCH lines.
 *   Takes pipeline_lock for the batch and g_lock for the metrics. Caller: the discovery worker through nr_csirs_disc_process_hook (nr_csirs_disc_feed.c); tests.
 */
void nr_csirs_disc_process(nr_csirs_disc_ring_t *ring)
{
  if (!ring || !nr_csirs_disc_enabled())
    return;
  pthread_mutex_lock(&pipeline_lock);
  if(!nr_csirs_disc_ring_snapshot_current(ring)) {pthread_mutex_unlock(&pipeline_lock);return;}
  if(cache_grid!=ring->grid_base || ring->written<cache_written) {
    cache_count=qualified_count=evidence_retries=0; ++evidence_epoch;
  }
  const bool cumulative=feature("ISAC_CSIRS_DISC_CUMULATIVE");
  const bool trs_priority=feature("ISAC_CSIRS_DISC_TRS_PRIORITY");
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
  int budget = sweep_limit();
  const int n_all = nr_csirs_disc_hypotheses(fps, n_fp_hyp, all_hyps, NR_CSIRS_DISC_MAX_HYP);
  int n_hyp = 0;
  if (cumulative) for (int i=0;i<evidence_retries && n_hyp<budget/4;++i) {
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
  for (int pass=0;pass<(trs_priority?2:1);++pass)
    for (int i=0;i<n_all && n_hyp<budget;++i) {
      const bool trs=all_hyps[i].row==1 && all_hyps[i].period>=80;
      if (trs_priority && trs!=(pass==0)) continue;
      bool duplicate=false;
      if (cumulative) for (int j=0;j<n_hyp;++j)
        duplicate |= nr_csirs_disc_evidence_key(&hyps[j],&all_hyps[i]);
      if (!duplicate && !cached(&all_hyps[i],ring->written)
          && (!nr_csirs_blind_rt_discovery_confirmed || !nr_csirs_blind_rt_discovery_confirmed(&all_hyps[i])))
        hyps[n_hyp++]=all_hyps[i];
    }
  LOG_I(PHY, "SENSING: CSIRS_DISC HYPOTHESES n=%d sweep=%d\n", n_all, n_hyp);

  const uint64_t built = now_us(), cpu_built=cpu_us();
  int n_win = 0;
  bool gpu = false;
  const nr_csirs_disc_sweep_cfg_t sweep_cfg = {
      .id_first = 0, .id_count = 1024, .delta_max = delta_limit(ring), .block_re = 32, .max_slots = 4,
      .evidence_epoch = cumulative ? evidence_epoch : 0};
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
      if (getenv("ISAC_CSIRS_DISC_DIAG"))
        LOG_I(PHY,
              "SENSING: CSIRS_DISC DIAG row=%u fd=%u l0=%u l1=%u fp=%u rb=%u:%u coarse=%u slots=%u id=%u z=%.6f z2=%.6f mu=%.6f sigma=%.6f P/O=%u/%u\n",
              h->row,
              h->freq_domain,
              h->symb_l0,
              h->symb_l1,
              h->fp_index,
              h->rb_start,
              h->n_rb,
              (unsigned)coarse_only[w->hyp_index],
              w->n_slots,
              w->scramb_id,
              w->z,
              w->z_second,
              w->noise_mu,
              w->noise_sigma,
              h->period,
              h->offset);
      if (cumulative && h->row==1 && getenv("ISAC_CSIRS_DISC_DIAG"))
        LOG_I(PHY,"SENSING: CSIRS_DISC EVIDENCE row=%u fd=%u l0=%u rb=%u:%u P/O=%u/%u slots=%u fresh=%u epoch=%llu\n",
              h->row,h->freq_domain,h->symb_l0,h->rb_start,h->n_rb,h->period,h->offset,
              w->n_slots,w->fresh_slots,(unsigned long long)evidence_epoch);
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
      const bool accumulated=cumulative && h->row==1 && sweep_cfg.delta_max==1;
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
      if (checked == 1 && getenv("ISAC_CSIRS_DISC_DIAG"))
        LOG_I(PHY,
              "SENSING: CSIRS_DISC VERIFY row=%u fd=%u l0=%u l1=%u rb=%u:%u id=%u z=%.6f z2=%.6f mu=%.6f sigma=%.6f P/O=%u/%u accepted=%u\n",
              refined.row, refined.freq_domain, refined.symb_l0, refined.symb_l1,
              refined.rb_start, refined.n_rb, verified.scramb_id, verified.z, verified.z_second,
              verified.noise_mu, verified.noise_sigma, refined.period, refined.offset,
              (unsigned)(nr_csirs_disc_gate_accept(&verified, &gate) && verified.scramb_id == w->scramb_id));
      if(checked!=1
          || !nr_csirs_disc_gate_accept(&verified, &gate) || verified.scramb_id!=w->scramb_id) {
        if (cumulative && checked==1 && refined.row==1 && sweep_cfg.delta_max==1)
          retry_refined(&refined,ring->written);
        continue;
      }
      w=&verified;
      const nr_csirs_disc_hyp_t *h = &refined;
      if (nr_csirs_blind_rt_add_discovered && nr_csirs_blind_rt_add_discovered(h, w) >= 0) {
        winners++;
        if(qualified_count<256) {
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
