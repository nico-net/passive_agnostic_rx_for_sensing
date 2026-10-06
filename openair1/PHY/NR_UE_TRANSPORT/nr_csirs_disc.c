/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_csirs_disc.h"

#include "common/utils/LOG/log.h"
#include "nr_passive_cfg_epoch.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern void nr_cfg_epoch_subscribe(nr_cfg_epoch_listener_t fn) __attribute__((weak));

/* Kept weak so the detector remains usable in focused U2/U3/U4 fixtures. */
extern int nr_csirs_blind_rt_add_discovered(const nr_csirs_disc_hyp_t *, const nr_csirs_disc_win_t *)
    __attribute__((weak));
extern bool nr_csirs_blind_rt_discovery_confirmed(const nr_csirs_disc_hyp_t *) __attribute__((weak));
extern void nr_csirs_blind_rt_discovery_reset(void) __attribute__((weak));
extern void nr_csirs_monitor_remove_dynamic_all(void) __attribute__((weak));

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static nr_csirs_disc_metrics_t g_metrics;

static uint64_t now_us(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

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
  nr_csirs_disc_detect_reset();
  if (nr_csirs_blind_rt_discovery_reset)
    nr_csirs_blind_rt_discovery_reset();
  if (nr_csirs_monitor_remove_dynamic_all)
    nr_csirs_monitor_remove_dynamic_all();
  nr_csirs_disc_ring_t *ring = nr_csirs_disc_worker_ring();
  if (ring && ring->private_state && nr_csirs_disc_ring_read_lock(ring)) {
    memset(ring->meta, 0, (size_t)ring->capacity * sizeof(*ring->meta));
    ring->written = 0;
    nr_csirs_disc_ring_read_unlock(ring);
  }
}

static void init_once(void)
{
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
  const uint64_t begin = now_us();
  nr_csirs_disc_fp_t fps[NR_CSIRS_DISC_MAX_FP];
  nr_csirs_disc_hyp_t all_hyps[NR_CSIRS_DISC_MAX_HYP];
  nr_csirs_disc_hyp_t hyps[NR_CSIRS_DISC_MAX_HYP];
  nr_csirs_disc_win_t wins[NR_CSIRS_DISC_MAX_HYP];
  const nr_csirs_disc_fp_cfg_t fp_cfg = {.min_strength = 1.03f};

  /* This is deliberately the detector's sole call for this batch. It owns GPU/CPU
   * footprint fallback and T2b's cross-ring history. */
  const int n_fp = nr_csirs_disc_detect(ring, &fp_cfg, fps, NR_CSIRS_DISC_MAX_FP);
  if (n_fp < 0)
    return;
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

  /* Multi-symbol/CDM rows can appear as one detector footprint per distinct
   * per-symbol comb. Add bounded union views; U3 accepts one only when the
   * union is an exact legal 38.211 row, so unrelated colocated pilots do not
   * become candidates. Preserve the detector outputs for narrow rows. */
  int n_fp_hyp = n_fp;
  for (int i = 0; i < n_fp && n_fp_hyp < NR_CSIRS_DISC_MAX_FP; ++i)
    for (int j = i + 1; j < n_fp && n_fp_hyp < NR_CSIRS_DISC_MAX_FP; ++j)
      if (fps[i].period == fps[j].period && fps[i].offset == fps[j].offset && fps[i].density == fps[j].density
          && fps[i].rb_start == fps[j].rb_start && fps[i].n_rb == fps[j].n_rb) {
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
  for (int i = 0; i < n_fp && n_fp_hyp < NR_CSIRS_DISC_MAX_FP; ++i) {
    nr_csirs_disc_fp_t u = fps[i];
    for (int j = i + 1; j < n_fp; ++j)
      if (u.period == fps[j].period && u.offset == fps[j].offset && u.density == fps[j].density
          && u.rb_start == fps[j].rb_start && u.n_rb == fps[j].n_rb) {
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

  const int n_all = nr_csirs_disc_hypotheses(fps, n_fp_hyp, all_hyps, NR_CSIRS_DISC_MAX_HYP);
  int n_hyp = 0;
  for (int i = 0; i < n_all && n_hyp < sweep_limit(); ++i)
    if (!nr_csirs_blind_rt_discovery_confirmed || !nr_csirs_blind_rt_discovery_confirmed(&all_hyps[i]))
      hyps[n_hyp++] = all_hyps[i];
  LOG_I(PHY, "SENSING: CSIRS_DISC HYPOTHESES n=%d sweep=%d\n", n_all, n_hyp);

  int n_win = 0;
  bool gpu = false;
  const nr_csirs_disc_sweep_cfg_t sweep_cfg = {
      .id_first = 0, .id_count = 1024, .delta_max = delta_limit(ring), .block_re = 32};
  const nr_csirs_disc_gpu_ops_t *ops = nr_csirs_disc_gpu_load();
  if (n_hyp > 0 && ops && ops->sweep) {
    n_win = ops->sweep(ring, hyps, n_hyp, &sweep_cfg, wins);
    gpu = n_win >= 0;
  }
  if (n_hyp > 0 && n_win < 0)
    n_win = 0;
  if (n_hyp > 0 && !gpu)
    n_win = nr_csirs_disc_sweep_cpu(ring, hyps, n_hyp, &sweep_cfg, wins);

  uint64_t winners = 0, ambiguous = 0;
  if (n_win > 0) {
    int best[NR_CSIRS_DISC_MAX_FP], leader[NR_CSIRS_DISC_MAX_FP];
    for (int f = 0; f < NR_CSIRS_DISC_MAX_FP; ++f)
      best[f] = leader[f] = -1;
    for (int i = 0; i < n_win; ++i) {
      const nr_csirs_disc_win_t *w = &wins[i];
      if (w->hyp_index < 0 || w->hyp_index >= n_hyp)
        continue;
      const nr_csirs_disc_hyp_t *h = &hyps[w->hyp_index];
      if (h->fp_index >= NR_CSIRS_DISC_MAX_FP)
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

    for (int f = 0; f < NR_CSIRS_DISC_MAX_FP; ++f)
      if (best[f] >= 0 && leader[f] >= 0 && best[f] != leader[f]) {
        wins[best[f]].scramb_id = wins[leader[f]].scramb_id;
        wins[best[f]].slot_delta = wins[leader[f]].slot_delta;
        wins[best[f]].z = wins[leader[f]].z;
        wins[best[f]].z_second = wins[leader[f]].z_second;
      }

    int selected[NR_CSIRS_DISC_MAX_FP], n_selected = 0;
    for (int f = 0; f < NR_CSIRS_DISC_MAX_FP; ++f) {
      const int i = best[f];
      if (i < 0)
        continue;
      const nr_csirs_disc_win_t *w = &wins[i];
      const nr_csirs_disc_hyp_t *h = &hyps[w->hyp_index];
      const bool accepted = w->z >= 4.0f && w->z - w->z_second >= 1.5f;
      if (accepted) {
        int replace = -1;
        for (int k = 0; k < n_selected; ++k) {
          const nr_csirs_disc_win_t *other_w = &wins[selected[k]];
          const nr_csirs_disc_hyp_t *other_h = &hyps[other_w->hyp_index];
          if (other_w->scramb_id == w->scramb_id && other_h->rb_start == h->rb_start && other_h->n_rb == h->n_rb) {
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
        else if (replace == -1 && n_selected < NR_CSIRS_DISC_MAX_FP)
          selected[n_selected++] = i;
      } else {
        ambiguous++;
        LOG_I(PHY,
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
      const nr_csirs_disc_hyp_t *h = &hyps[w->hyp_index];
      if (nr_csirs_blind_rt_add_discovered && nr_csirs_blind_rt_add_discovered(h, w) >= 0)
        winners++;
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
  g_metrics.batches++;
  g_metrics.footprints += (uint64_t)n_fp;
  g_metrics.hyps += (uint64_t)n_all;
  g_metrics.winners += winners;
  g_metrics.ambiguous += ambiguous;
  g_metrics.feed_drops = __atomic_load_n(&csirs_disc_feed_drops, __ATOMIC_RELAXED);
  g_metrics.gpu_batches += gpu;
  g_metrics.cpu_batches += !gpu;
  g_metrics.batch_us += now_us() - begin;
  pthread_mutex_unlock(&g_lock);
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
