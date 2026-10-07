/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_csirs_disc.h"

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
extern void nr_csirs_blind_rt_discovery_init(void) __attribute__((weak));
extern void nr_csirs_blind_rt_discovery_reset(void) __attribute__((weak));
extern void nr_csirs_monitor_remove_dynamic_all(void) __attribute__((weak));

#define DISC_VIEWS 128
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static nr_csirs_disc_metrics_t g_metrics;
static pthread_mutex_t pipeline_lock = PTHREAD_MUTEX_INITIALIZER;
typedef struct {nr_csirs_disc_hyp_t h; uint64_t written; bool accepted;} sweep_cache_t;
static sweep_cache_t cache[256];
static int cache_count;
static struct {nr_csirs_disc_hyp_t h;uint16_t id;} qualified[256];
static int qualified_count;
static const int16_t *cache_grid;
static uint64_t cache_written;
static bool same_seed(const nr_csirs_disc_hyp_t *a, const nr_csirs_disc_hyp_t *b)
{
  int lo=a->rb_start>b->rb_start?a->rb_start:b->rb_start;
  int hi=a->rb_start+a->n_rb<b->rb_start+b->n_rb?a->rb_start+a->n_rb:b->rb_start+b->n_rb;
  return 5*(hi-lo)>=4*(a->n_rb>b->n_rb?a->n_rb:b->n_rb) && a->row==b->row && a->freq_domain==b->freq_domain && a->symb_l0==b->symb_l0 && a->symb_l1==b->symb_l1
      && a->cdm_type==b->cdm_type && a->density==b->density && a->period==b->period && a->offset==b->offset
      && a->rb_start < b->rb_start+b->n_rb && b->rb_start < a->rb_start+a->n_rb;
}
static bool cached(const nr_csirs_disc_hyp_t *h, uint64_t written)
{
  for(int i=0;i<cache_count;++i) if(same_seed(h,&cache[i].h)) {
    uint64_t delay=3*h->period>2000?3*h->period:2000;
    if(cache[i].accepted || written-cache[i].written<delay) return true;
  }
  return false;
}
static void cache_result(const nr_csirs_disc_hyp_t *h, uint64_t written, bool accepted)
{
  int at=-1;
  for(int i=0;i<cache_count;++i) if(same_seed(h,&cache[i].h)) {at=i;break;}
  if(at<0) at=cache_count<256?cache_count++:(int)(written%256);
  cache[at]=(sweep_cache_t){*h,written,accepted};
}

static bool same_extent(const nr_csirs_disc_fp_t *a,const nr_csirs_disc_fp_t *b)
{
  int lo=a->rb_start>b->rb_start?a->rb_start:b->rb_start;
  int hi=a->rb_start+a->n_rb<b->rb_start+b->n_rb?a->rb_start+a->n_rb:b->rb_start+b->n_rb;
  return hi>lo && 5*(hi-lo)>=4*(a->n_rb>b->n_rb?a->n_rb:b->n_rb);
}

static uint64_t now_us(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

/* Noise-adaptive winner gate. Each sweep's runner-up z2 is the best score among the ~1023 WRONG
 * scrambling IDs, i.e. a direct sample of the noise maximum for that geometry and integration.
 * Gate = median(z2) + K * sigma(z2) (robust: 1.4826 * MAD) over the last 512 sweeps, clamped to
 * [2.5, 4.5]; K = ISAC_CSIRS_DISC_WIN_K (default 10). Live OTA 2026-10-07 (PCI 415, 409 sweeps):
 * z2 mean 1.63, std 0.19 -> gate ~3.5, the fixed value proven live (k0=1 sym 5+9 pair confirmed in
 * 5/6 runs; 4.0 left it AMBIGUOUS). Until 32 samples exist, or with ISAC_CSIRS_DISC_WIN_Z=<z>, a
 * fixed gate (default 3.5) is used. The per-sweep margin z - z2 >= 1.5 is unchanged. */
#define WINZ_HIST 512
static float g_z2_hist[WINZ_HIST];
static int g_z2_n, g_z2_w;
static float g_gate = 3.5f, g_gate_med, g_gate_sig;
static int cmp_float(const void *a, const void *b)
{
  const float x = *(const float *)a, y = *(const float *)b;
  return (x > y) - (x < y);
}
static void win_noise_add(float z2)
{
  if (!(z2 > 0.0f) || z2 > 50.0f)
    return;
  g_z2_hist[g_z2_w] = z2;
  g_z2_w = (g_z2_w + 1) % WINZ_HIST;
  if (g_z2_n < WINZ_HIST)
    ++g_z2_n;
}
static void win_gate_update(void)
{
  static float fixed = -1.0f, k = -1.0f;
  if (fixed < 0) {
    const char *s = getenv("ISAC_CSIRS_DISC_WIN_Z"), *kk = getenv("ISAC_CSIRS_DISC_WIN_K");
    fixed = s && atof(s) > 0 ? (float)atof(s) : 0.0f;
    k = kk && atof(kk) > 0 ? (float)atof(kk) : 10.0f;
  }
  if (fixed > 0 || g_z2_n < 32) {
    g_gate = fixed > 0 ? fixed : 3.5f;
    return;
  }
  float v[WINZ_HIST];
  memcpy(v, g_z2_hist, (size_t)g_z2_n * sizeof(*v));
  qsort(v, g_z2_n, sizeof(*v), cmp_float);
  const float med = v[g_z2_n / 2];
  for (int i = 0; i < g_z2_n; ++i)
    v[i] = fabsf(v[i] - med);
  qsort(v, g_z2_n, sizeof(*v), cmp_float);
  const float sig = 1.4826f * v[g_z2_n / 2];
  float gate = med + k * sig;
  gate = gate < 2.5f ? 2.5f : gate > 4.5f ? 4.5f : gate;
  g_gate = gate; g_gate_med = med; g_gate_sig = sig;
}
static float win_z(void) { return g_gate; }

static int sweep_limit(void)
{
  const char *s = getenv("ISAC_CSIRS_DISC_SWEEP_HYPS");
  const long value = s ? strtol(s, NULL, 10) : 32;
  return value > 0 && value <= NR_CSIRS_DISC_MAX_HYP ? (int)value : 32;
}

static int delta_limit(const nr_csirs_disc_ring_t *ring)
{
  const char *s = getenv("ISAC_CSIRS_DISC_DELTA_MAX");
  const long value = s ? strtol(s, NULL, 10) : 1;
  return value > 0 && value <= ring->slots_per_frame ? (int)value : 1;
}

static void epoch_listener(const nr_cfg_epoch_snapshot_t *snapshot)
{
  (void)snapshot;
  pthread_mutex_lock(&pipeline_lock);
  cache_count=qualified_count=0;cache_grid=NULL;cache_written=0;
  nr_csirs_disc_detect_reset();
  if (nr_csirs_blind_rt_discovery_reset)
    nr_csirs_blind_rt_discovery_reset();
  if (nr_csirs_monitor_remove_dynamic_all)
    nr_csirs_monitor_remove_dynamic_all();
  nr_csirs_disc_ring_t *ring = nr_csirs_disc_worker_ring();
  nr_csirs_disc_ring_reset(ring);
  pthread_mutex_unlock(&pipeline_lock);
}

static void init_once(void)
{
  if(nr_csirs_blind_rt_discovery_init) nr_csirs_blind_rt_discovery_init();
  (void)nr_csirs_disc_gpu_load();
  nr_csirs_disc_process_hook = nr_csirs_disc_process;
  if (nr_cfg_epoch_subscribe)
    nr_cfg_epoch_subscribe(epoch_listener);
}

void nr_csirs_disc_pipeline_init(const NR_DL_FRAME_PARMS *fp)
{
  (void)fp;
  if (nr_csirs_disc_enabled())
    pthread_once(&g_once, init_once);
}

void nr_csirs_disc_process(nr_csirs_disc_ring_t *ring)
{
  if (!ring || !nr_csirs_disc_enabled())
    return;
  pthread_mutex_lock(&pipeline_lock);
  if(!nr_csirs_disc_ring_snapshot_current(ring)) {pthread_mutex_unlock(&pipeline_lock);return;}
  if(cache_grid!=ring->grid_base || ring->written<cache_written) cache_count=qualified_count=0;
  cache_grid=ring->grid_base;cache_written=ring->written;
  const uint64_t begin = now_us();
  nr_csirs_disc_fp_t fps[DISC_VIEWS];
  nr_csirs_disc_hyp_t all_hyps[NR_CSIRS_DISC_MAX_HYP];
  nr_csirs_disc_hyp_t hyps[NR_CSIRS_DISC_MAX_HYP];
  nr_csirs_disc_win_t wins[NR_CSIRS_DISC_MAX_HYP];
  bool coarse_only[NR_CSIRS_DISC_MAX_HYP] = {false}; /* screened out: z2 from the short coarse sweep */
  const nr_csirs_disc_fp_cfg_t fp_cfg = {.min_strength = 1.03f};

  /* Incremental GPU local normalization/folding, CPU bounded clustering. */
  int n_fp = nr_csirs_disc_detect_candidates(ring, &fp_cfg, fps, 64);
  if(n_fp<0) { /* Recover after a full ring overrun; unavailable IQ cannot be folded. */
    nr_csirs_disc_detect_reset();
    n_fp=nr_csirs_disc_detect_candidates(ring,&fp_cfg,fps,64);
  }
  const uint64_t detected = now_us();
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
  for (int i = 0; i < n_all && n_hyp < budget; ++i)
    if (!cached(&all_hyps[i],ring->written)
        && (!nr_csirs_blind_rt_discovery_confirmed || !nr_csirs_blind_rt_discovery_confirmed(&all_hyps[i])))
      hyps[n_hyp++] = all_hyps[i];
  LOG_I(PHY, "SENSING: CSIRS_DISC HYPOTHESES n=%d sweep=%d\n", n_all, n_hyp);

  const uint64_t built = now_us();
  int n_win = 0;
  bool gpu = false;
  const nr_csirs_disc_sweep_cfg_t sweep_cfg = {
      .id_first = 0, .id_count = 1024, .delta_max = delta_limit(ring), .block_re = 32, .max_slots = 4};
  if (n_hyp > 0 && ops && ops->sweep) {
    n_win = ops->sweep(ring, hyps, n_hyp, &sweep_cfg, wins);
    gpu = n_win >= 0;
  }
  if (n_hyp > 0 && n_win < 0)
    n_win = 0;
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
      if (!coarse_only[w->hyp_index]) win_noise_add(w->z_second); /* full-integration noise only */
      if (h->fp_index >= DISC_VIEWS)
        continue;
      if (getenv("ISAC_CSIRS_DISC_DIAG"))
        LOG_I(PHY,
              "SENSING: CSIRS_DISC DIAG row=%u fd=%u l0=%u l1=%u fp=%u id=%u z=%.3f z2=%.3f P/O=%u/%u\n",
              h->row,
              h->freq_domain,
              h->symb_l0,
              h->symb_l1,
              h->fp_index,
              w->scramb_id,
              w->z,
              w->z_second,
              h->period,
              h->offset);
      const int lead = leader[h->fp_index];
      if (lead < 0 || w->z > wins[lead].z)
        leader[h->fp_index] = i;
      const int old = best[h->fp_index];
      if (old < 0 || (h->row < hyps[wins[old].hyp_index].row
                      && w->z + 0.5f >= wins[leader[h->fp_index]].z))
        best[h->fp_index] = i;
    }

    win_gate_update();
    static uint64_t gate_logged;
    if (g_metrics.batches - gate_logged >= 50 || !gate_logged) {
      gate_logged = g_metrics.batches ? g_metrics.batches : 1;
      LOG_I(PHY, "SENSING: CSIRS_DISC WIN_GATE z>=%.2f (noise z2 median %.2f sigma %.3f, n=%d)\n", g_gate, g_gate_med, g_gate_sig, g_z2_n);
    }
    int selected[DISC_VIEWS], n_selected = 0;
    for (int f = 0; f < DISC_VIEWS; ++f) {
      const int i = best[f];
      if (i < 0)
        continue;
      const nr_csirs_disc_win_t *w = &wins[i];
      const nr_csirs_disc_hyp_t *h = &hyps[w->hyp_index];
      const bool accepted = w->z >= win_z() && w->z - w->z_second >= 1.5f;
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
              "SENSING: CSIRS_DISC AMBIGUOUS row=%u fd=%u l0=%u l1=%u id=%u z=%.2f z2=%.2f P/O=%u/%u\n",
              h->row,
              h->freq_domain,
              h->symb_l0,
              h->symb_l1,
              w->scramb_id,
              w->z,
              w->z_second,
              h->period,
              h->offset);
      }
    }

    for (int k = 0; k < n_selected; ++k) {
      const nr_csirs_disc_win_t *w = &wins[selected[k]];
      const int original_index=w->hyp_index;
      nr_csirs_disc_hyp_t refined = hyps[w->hyp_index];
      if (nr_csirs_disc_refine_cpu(ring, &refined, w) < 0) continue;
      /* A weaker burst phase must not create a cropped clone of a wider,
       * already sequence-qualified resource. Reuse that measured geometry and
       * verify the entire extent again on this phase before admission. */
      for(int q=0;q<qualified_count;++q) if(qualified[q].id==w->scramb_id) {
        nr_csirs_disc_hyp_t canonical=qualified[q].h;canonical.offset=refined.offset;
        if(canonical.n_rb>refined.n_rb && same_seed(&canonical,&refined)) refined=canonical;
      }
      nr_csirs_disc_win_t verified;
      int checked=gpu && ops->sweep?ops->sweep(ring,&refined,1,&sweep_cfg,&verified):-1;
      if(checked<0) checked=nr_csirs_disc_sweep_cpu(ring,&refined,1,&sweep_cfg,&verified);
      if(checked!=1
          || verified.z<win_z() || verified.z-verified.z_second<1.5f || verified.scramb_id!=w->scramb_id) continue;
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
            "SENSING: CSIRS_DISC WINNER row=%u fd=%u l0=%u l1=%u cdm=%u dens=%u rb=%u:%u id=%u delta=%d z=%.2f z2=%.2f P/O=%u/%u\n",
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
            h->period,
            h->offset);
    }
  }

  pthread_mutex_lock(&g_lock);
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

void nr_csirs_disc_get_metrics(nr_csirs_disc_metrics_t *out)
{
  if (!out)
    return;
  pthread_mutex_lock(&g_lock);
  *out = g_metrics;
  out->feed_drops = __atomic_load_n(&csirs_disc_feed_drops, __ATOMIC_RELAXED);
  pthread_mutex_unlock(&g_lock);
}
