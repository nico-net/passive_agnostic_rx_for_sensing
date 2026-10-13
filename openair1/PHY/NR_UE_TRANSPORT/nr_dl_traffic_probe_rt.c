/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* See nr_dl_traffic_probe_rt.h. One-slot mailbox filled by the RX actor, drained by a SCHED_OTHER worker. */
#include "nr_dl_traffic_probe_rt.h"
#include "nr_dl_traffic_probe.h"
#include "nr_pdcch_coreset_map.h"
#include "nr_pdcch_blind_monitor_rt.h"
#include "nr_passive_acq_state.h"
#include "nr_csirs_disc_td.h"
#include "PHY/MODULATION/modulation_UE.h"
#include "PHY/MODULATION/nr_modulation.h"
#include "PHY/nr_phy_common/inc/nr_phy_common.h"
#include "common/utils/nr_core_plan.h"
#include "common/utils/LOG/log.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DLTP_PA_H 275                /* Point-A hypotheses below the grid: CRB index of grid RB 0 (a carrier is at most 275 RB) */
#define DLTP_PA_N (DLTP_PA_H + NR_DLTP_MAX_RB) /* ... plus CRB 0 inside the grid (grid wider than the carrier): ref_rb = i - DLTP_PA_H + 1 */
#define DLTP_CORR_LIT 0.836          /* 4 x sqrt(pi)/(2 sqrt(18)): the nr_pdcch_coreset_map significance bar */
#define DLTP_LOG_EVERY 256           /* probed slots per TRAFFIC line */
#define DLTP_MAX_GRANTS 64
#define DLTP_RING 16                 /* slots held back so the scan threads have decoded the slot's DCIs first */
#define DLTP_DELAY_SLOTS 400         /* 200 ms at mu=1: beyond the scan queue's measured max lag (STALLS: PDSCH stale drops < 8 ms, scan lag tens of ms) */

static struct {
  pthread_mutex_t mu;
  pthread_cond_t cv;
  int every, sweep;                  /* env, -1 unread */
  const NR_DL_FRAME_PARMS *fp;
  bool running;
  uint32_t dl_calls, now_slot;
  /* delayed ring of slot copies: [14][fft] each, antenna 0, CP stripped */
  struct dltp_entry {
    c16_t *td;
    int offset[NR_DLTP_NSYM];
    c16_t rot[NR_DLTP_NSYM];
    double fo;
    bool compensate, full;
    uint32_t abs_slot;
    uint16_t frame;
    uint8_t slot;
  } ring[DLTP_RING];
  int head, tail; /* tail = next to process, head = next to fill */
  /* worker */
  c16_t *in, *out, *pilot;
  float *e, *scratch;
  nr_dltp_stats_t st;
  uint32_t probed, pa_hits[DLTP_PA_N], pa_sym_hits[3], pa_syms, pci_windows, pci_sym_windows[3];
  bool pa_done;
} g = {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER, .every = -1, .sweep = -1};

static int env_int(const char *k, int def, int lo, int hi)
{
  const char *s = getenv(k);
  if (!s || !*s) return def;
  char *end;
  const long n = strtol(s, &end, 10);
  return *end || n < lo || n > hi ? def : (int)n;
}

/* pdcch_pci_windows: lit 6-RB windows of the PDCCH DM-RS of nID = PCI in `sym`, groups aligned to CRB 0 = grid RB ref_rb
 *   (negative when CRB 0 is below the grid), CORESET#0 and SSB RBs skipped. Caller: worker(). */
static int pdcch_pci_windows(const c16_t *symF, int ref_rb, int css0_rb0, int css0_rb1, int ssb_rb0, int ssb_rb1)
{
  const NR_DL_FRAME_PARMS *fp = g.fp;
  const int n_pilot = fp->N_RB_DL - ref_rb;
  int hits = 0;
  for (int w = ((ref_rb % 6) + 6) % 6; w + 6 <= fp->N_RB_DL; w += 6) {
    if (w < ref_rb || (w < css0_rb1 && w + 6 > css0_rb0) || (w < ssb_rb1 && w + 6 > ssb_rb0)) continue;
    if (nr_pdcch_coreset_window_corr(symF, fp->ofdm_symbol_size, fp->first_carrier_offset, g.pilot, n_pilot, w, ref_rb)
        > DLTP_CORR_LIT)
      hits++;
  }
  return hits;
}

static void *worker(void *arg)
{
  (void)arg;
  const NR_DL_FRAME_PARMS *fp = g.fp;
  const int fft = fp->ofdm_symbol_size, n_rb = fp->N_RB_DL;
  nr_dltp_grant_t grants[DLTP_MAX_GRANTS];
  for (;;) {
    pthread_mutex_lock(&g.mu);
    /* process the oldest entry once it is DLTP_DELAY_SLOTS old (its DCIs are decoded and registered by then) */
    while (!g.ring[g.tail].full || (int32_t)(g.now_slot - g.ring[g.tail].abs_slot) < DLTP_DELAY_SLOTS)
      pthread_cond_wait(&g.cv, &g.mu);
    struct dltp_entry *e = &g.ring[g.tail];
    for (int l = 0; l < NR_DLTP_NSYM; ++l) {
      c16_t *dst = g.in + (size_t)l * fft;
      if (e->compensate) nr_fo_compensation(e->fo, fp->samples_per_subframe, e->offset[l], e->td + (size_t)l * fft, dst, fft);
      else memcpy(dst, e->td + (size_t)l * fft, sizeof(c16_t) * (size_t)fft);
    }
    const uint32_t abs_slot = e->abs_slot;
    const uint16_t frame = e->frame;
    const uint8_t slot = e->slot;
    c16_t rot[NR_DLTP_NSYM];
    memcpy(rot, e->rot, sizeof(rot));
    e->full = false;
    g.tail = (g.tail + 1) % DLTP_RING;
    pthread_mutex_unlock(&g.mu);

    nr_csirs_disc_fft_cpu(fft, NR_DLTP_NSYM, (const int16_t *)g.in, (int16_t *)g.out);
    for (int l = 0; l < NR_DLTP_NSYM; ++l)
      apply_nr_rotation_symbol_RX(NR_DLTP_NSYM, 1, fp->timeshift_symbol_rotation, fp->first_carrier_offset,
                                  g.out + (size_t)l * fft, rot, n_rb, 0, l);

    const nr_passive_acq_snapshot_t snap = nr_passive_acq_snapshot();
    int css0_rb0 = -1, css0_nrb = 0;
    nr_pdcch_coreset_map_get_css0(&css0_rb0, &css0_nrb);
    const nr_pdcch_blind_monitor_cfg_t *c0 = nr_pdcch_blind_monitor_css0_cfg();
    nr_dltp_geom_t geom = {.n_rb = n_rb, .carrier_rb0 = 0, .carrier_rb1 = n_rb,
                           .css0_rb0 = css0_rb0 >= 0 ? css0_rb0 : 0, .css0_rb1 = css0_rb0 >= 0 ? css0_rb0 + css0_nrb : 0,
                           .css0_nsym = css0_rb0 < 0 ? 0 : (c0 && c0->coreset_duration > 0 ? c0->coreset_duration : 3),
                           .ssb_rb0 = fp->ssb_start_subcarrier / 12, .ssb_rb1 = fp->ssb_start_subcarrier / 12 + 20};
    if (snap.carrier_verified == 1 && snap.carrier.carrier_n_rb > 0) {
      geom.carrier_rb0 = snap.carrier.carrier_start_subcarrier / 12;
      geom.carrier_rb1 = geom.carrier_rb0 + snap.carrier.carrier_n_rb;
    }
    nr_dltp_energy((const int16_t *)g.out, fft, fp->first_carrier_offset, n_rb, g.e);
    const float floor_mean = nr_dltp_floor(g.e, NR_DLTP_NSYM * n_rb, g.scratch);
    const int ng = nr_dltp_grants_for(frame, slot, grants, DLTP_MAX_GRANTS);
    nr_dltp_slot(g.e, &geom, floor_mean, nr_passive_acq_tdd_known(), grants, ng, &g.st);
    g.probed++;

    /* PDCCH DM-RS of nID = PCI: at the known CRB reference (traffic evidence), or swept over Point-A hypotheses. */
    if (g.sweep == 1 && css0_rb0 >= 0) {
      if (snap.carrier_verified == 1) {
        const int ref_rb = snap.carrier.point_a_subcarrier / 12; /* grid RB of CRB 0 (negative below the grid) */
        for (int s = 0; s < 3; ++s) {
          nr_pdcch_coreset_pilot((uint16_t)fp->Nid_cell, slot, s, n_rb - ref_rb, g.pilot);
          const int h = pdcch_pci_windows(g.out + (size_t)s * fft, ref_rb, geom.css0_rb0, geom.css0_rb1, geom.ssb_rb0, geom.ssb_rb1);
          g.pci_windows += (uint32_t)h;
          g.pci_sym_windows[s] += (uint32_t)h;
        }
      } else if (snap.carrier_verified == 0 && !g.pa_done) {
        for (int s = 0; s < 3; ++s) {
          nr_pdcch_coreset_pilot((uint16_t)fp->Nid_cell, slot, s, n_rb + DLTP_PA_H, g.pilot);
          for (int i = 0; i < DLTP_PA_N; ++i) { /* ref_rb = grid RB of CRB 0: -(DLTP_PA_H-1) .. n_rb-6 */
            const int ref_rb = i - DLTP_PA_H + 1;
            if (ref_rb > n_rb - 6) break;
            const int k = pdcch_pci_windows(g.out + (size_t)s * fft, ref_rb, geom.css0_rb0, geom.css0_rb1, geom.ssb_rb0, geom.ssb_rb1);
            g.pa_hits[i] += (uint32_t)k;
            g.pa_sym_hits[s] += (uint32_t)k;
          }
        }
        g.pa_syms += 3;
        if ((g.probed % 64) == 0) {
          int best = 0, second = -1;
          for (int i = 1; i < DLTP_PA_N; ++i)
            if (g.pa_hits[i] > g.pa_hits[best]) { second = best; best = i; }
            else if (second < 0 || g.pa_hits[i] > g.pa_hits[second]) second = i;
          const uint32_t hb = g.pa_hits[best], hs = second >= 0 ? g.pa_hits[second] : 0;
          const int best_rb = best - DLTP_PA_H + 1, second_rb = second - DLTP_PA_H + 1; /* grid RB of CRB 0 */
          if (hb >= 6 && hb >= 4 * hs) {
            char ev[160];
            snprintf(ev, sizeof(ev), "POINTA_SWEEP nID=PCI %d: CRB 0 = grid RB %d lit=%u, runner-up grid RB %d lit=%u, %u symbols",
                     fp->Nid_cell, best_rb, hb, second_rb, hs, g.pa_syms);
            nr_passive_acq_note_carrier_hypothesis(12 * best_rb, ev);
            g.pa_done = true;
          } else if ((g.probed % DLTP_LOG_EVERY) == 0)
            LOG_A(PHY, "SENSING: POINTA_SWEEP undecided: symbols=%u best CRB0=grid RB %d lit=%u runner-up grid RB %d lit=%u (per symbol %u/%u/%u)\n",
                  g.pa_syms, best_rb, hb, second_rb, hs, g.pa_sym_hits[0], g.pa_sym_hits[1], g.pa_sym_hits[2]);
        }
      }
    }
    if ((g.probed % DLTP_LOG_EVERY) == 0) {
      char buf[640];
      nr_dltp_verdict(&g.st, buf, sizeof(buf));
      LOG_A(PHY, "SENSING: %s pdcch_pci_windows=%u (%u/%u/%u) carrier_rb=%d+%d abs_slot=%u\n", buf, g.pci_windows,
            g.pci_sym_windows[0], g.pci_sym_windows[1], g.pci_sym_windows[2], geom.carrier_rb0,
            geom.carrier_rb1 - geom.carrier_rb0, abs_slot);
    }
  }
  return NULL;
}

static bool start(const NR_DL_FRAME_PARMS *fp)
{
  const size_t fft = (size_t)fp->ofdm_symbol_size;
  g.fp = fp;
  for (int i = 0; i < DLTP_RING; ++i)
    if (!(g.ring[i].td = malloc(sizeof(c16_t) * NR_DLTP_NSYM * fft))) return false;
  g.in = malloc(sizeof(c16_t) * NR_DLTP_NSYM * fft);
  g.out = malloc(sizeof(c16_t) * NR_DLTP_NSYM * fft);
  g.pilot = malloc(sizeof(c16_t) * 3 * (size_t)(fp->N_RB_DL + DLTP_PA_H));
  g.e = malloc(sizeof(float) * NR_DLTP_NSYM * (size_t)fp->N_RB_DL);
  g.scratch = malloc(sizeof(float) * NR_DLTP_NSYM * (size_t)fp->N_RB_DL);
  if (!g.in || !g.out || !g.pilot || !g.e || !g.scratch) return false;
  pthread_t th;
  if (nr_core_plan_create(&th, NULL, worker, NULL, -1) != 0) return false;
  pthread_detach(th);
  LOG_A(PHY, "SENSING: DL_TRAFFIC_PROBE on: one DL slot in %d, antenna 0, SCHED_OTHER worker; POINTA_SWEEP=%d\n", g.every, g.sweep);
  return true;
}

void nr_dl_traffic_probe_push(PHY_VARS_NR_UE *ue, uint32_t abs_slot, uint16_t frame, uint8_t slot)
{
  if (g.every < 0) {
    g.every = env_int("ISAC_DL_TRAFFIC_PROBE", 0, 0, 100000);
    g.sweep = env_int("ISAC_POINTA_SWEEP", 0, 0, 1);
  }
  if (!g.every) return;
  const NR_DL_FRAME_PARMS *fp = &ue->frame_parms;
  if (!g.running) {
    if (fp->N_RB_DL > NR_DLTP_MAX_RB || !start(fp)) { g.every = 0; return; }
    g.running = true;
  }
  if (pthread_mutex_trylock(&g.mu)) return;
  g.now_slot = abs_slot; /* the worker's delay clock */
  const bool take = nr_passive_acq_tdd_slot_has_downlink(abs_slot) && (g.dl_calls++ % (uint32_t)g.every) == 0
                    && slot < fp->slots_per_frame && !g.ring[g.head].full;
  if (!take) { pthread_cond_signal(&g.cv); pthread_mutex_unlock(&g.mu); return; }
  struct dltp_entry *e = &g.ring[g.head];
  /* Same symbol origins as nr_csirs_disc_td_push(): the span nr_slot_fep reads, CP fraction included. */
  const unsigned total = 2 * fp->samples_per_frame;
  unsigned offset = get_samples_slot_timestamp(fp, slot);
  const unsigned cp0 = ue->is_synchronized ? fp->nb_prefix_samples0 : fp->nb_prefix_samples;
  for (int l = 0; l < NR_DLTP_NSYM; ++l) {
    offset += ((slot * 14 + l) % (7 << fp->numerology_index)) ? fp->nb_prefix_samples : cp0;
    e->offset[l] = (int)(offset - fp->nb_prefix_samples / fp->ofdm_offset_divisor);
    const unsigned start = (unsigned)e->offset[l] % total, first = (unsigned)fp->ofdm_symbol_size < total - start ? (unsigned)fp->ofdm_symbol_size : total - start;
    memcpy(e->td + (size_t)l * fp->ofdm_symbol_size, ue->common_vars.rxdata[0] + start, first * sizeof(c16_t));
    if (first < (unsigned)fp->ofdm_symbol_size)
      memcpy(e->td + (size_t)l * fp->ofdm_symbol_size + first, ue->common_vars.rxdata[0], (fp->ofdm_symbol_size - first) * sizeof(c16_t));
    e->rot[l] = fp->symbol_rotation[link_type_dl][(slot % fp->slots_per_subframe) * 14 + l];
    offset += fp->ofdm_symbol_size;
  }
  e->compensate = ue->cont_fo_comp;
  e->fo = isnan(nr_slot_fep_fo_override_hz) ? ue->dl_Doppler_shift + ue->freq_offset : nr_slot_fep_fo_override_hz;
  e->abs_slot = abs_slot; e->frame = frame; e->slot = slot; e->full = true;
  g.head = (g.head + 1) % DLTP_RING;
  pthread_cond_signal(&g.cv);
  pthread_mutex_unlock(&g.mu);
}
