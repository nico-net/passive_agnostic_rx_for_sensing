/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this
 * file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.openairinterface.org/?page_id=698
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_map.c
 * \brief Implementation for nr_pdcch_coreset_map.h -- see that header for the design rationale.
 */
#include "nr_pdcch_nid_gpu.h"
/* weak: test targets compile this file without nr_dl_traffic_probe.c */
extern int nr_syncq_gate_closed(void) __attribute__((weak));
#include "nr_pdcch_nid_occupancy.h"
#include <stdatomic.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nr_pdcch_coreset_map.h"
#ifndef NR_PDCCH_MAX_CANDIDATE_WINDOWS
#define NR_PDCCH_MAX_CANDIDATE_WINDOWS 46 /* 6-RB windows on a 275-PRB carrier */
#endif

/* Real current signatures (openair1/PHY/NR_REFSIG/nr_refsig.h) -- do NOT copy the task brief's
 * approximate forward declarations verbatim. In particular nr_pdcch_dmrs_ref()'s third argument
 * is an RB COUNT, not a raw output-element count: it writes nb_rb_coreset*3 complex pilots
 * (nr_dmrs_rx.c: `for (i = 0; i < (nb_rb_coreset*6)>>1; i++)`), i.e. 3 DM-RS REs/RB starting from
 * absolute RB 0 of the gold sequence generated for N_RB_DL RBs. Passing an already-tripled count
 * here (as a naive reading of "count" might suggest) overflows the pilot buffer by 3x. */
extern uint32_t *nr_gold_pdcch(int N_RB_DL, int symbols_per_slot, unsigned short n_idDMRS, int ns, int l);
extern void nr_pdcch_dmrs_ref(const unsigned int *nr_gold_pdcch, c16_t *output, unsigned short nb_rb_coreset);

// Pure noise gives |corr| ~= sqrt(pi)/(2*sqrt(18)) ~= 0.209 for an 18-pilot (6-RB, 3 DM-RS
// RE/RB) window -- this project's own already-derived figure (dci_nr.c's nr_pdcch_blind_dmrs_probe
// comment). A real DM-RS measured 0.8-0.95 live. Set the bar at 4x the noise floor (~0.836), well
// clear of noise, comfortably below a real hit -- NOT at 0.5x(noise+signal), because the noise
// distribution's own upper tail (not just its mean) is what a real significance bar must clear;
// see dci_nr.c's own comment on why "max over many trials grows only as sqrt(ln(trials)/18)".
#define CORESET_MAP_CORR_THRESHOLD 0.836


/* The window statistic is one coherent sum over the 18 pilots of a 6-RB window (noise floor sqrt(pi)/(2*sqrt(18)) = 0.209 against ~0.9 for a real
 * DM-RS; the per-RB sub-band alternative has strictly worse separation on a flat channel and was refuted on air). */

/* The on-accept DM-RS probe was removed (diagnostic); the stub stays until its call site in nr_pdcch_blind_monitor_rt.c is removed. */
void nr_pdcch_coreset_map_accept_probe(const c16_t *rxdataF, int ofdm_symbol_size, int n_rb_carrier,
                                       int first_carrier_offset, uint16_t scrambling_id, int slot,
                                       int symbol, int cs_start, int cs_nrb)
{
  (void)rxdataF; (void)ofdm_symbol_size; (void)n_rb_carrier; (void)first_carrier_offset;
  (void)scrambling_id; (void)slot; (void)symbol; (void)cs_start; (void)cs_nrb;
}


/* Set by the caller that knows the cell's CORESET#0 start (autoconf derives it from the SSB
 * offset). The scan aligns its 6-RB window grid to `start % 6`. Defaults to 0 = CRB-0 aligned. */
static int g_coreset_map_phase_hint;
void nr_pdcch_coreset_map_set_phase_hint(int start_rb)
{
  g_coreset_map_phase_hint = (start_rb > 0) ? (start_rb % 6) : 0;
}
static int nr_pdcch_coreset_map_phase_hint(void)
{
  return g_coreset_map_phase_hint;
}
int nr_pdcch_coreset_map_get_phase(void)
{
  return g_coreset_map_phase_hint;
}

/* STAGE 1: PDCCH DM-RS scrambling-ID discovery, decode-free, fully blind (GPU sweep, nr_pdcch_nid_gpu.c).
 *
 * Nothing about the cell is assumed:
 *  - ID: all 65536 values; every (ID, symbol) is scored.
 *  - Frequency reference (TS 38.211 7.4.1.3.2): a PDCCH-Config CORESET's DM-RS is indexed from
 *    CRB 0 = Point A, CORESET#0's from its own first RB. Point A comes from SIB1 (offsetToPointA)
 *    measured against where the SSB was FOUND in the grid (nr_passive_acq_snapshot()); the sweep
 *    does not start until that is known. CORESET#0 is placed exactly from the MIB on the same CRB grid.
 *  - Slots: only slots SIB1's TDD pattern marks as downlink (unknown pattern = all). */
#include <pthread.h>
#include "openair1/PHY/gold.h"
#include "nr_passive_acq_state.h"

static int g_css0_first_rb = -1, g_css0_nrb = 0;
void nr_pdcch_coreset_map_set_css0(int first_rb, int n_rb)
{
  g_css0_first_rb = first_rb;
  g_css0_nrb = n_rb;
}
/* nr_pdcch_coreset_map_get_css0: returns the first RB and the number of RBs of CORESET#0 (the CSS0 span) as recorded from the MIB; Caller: nr_pdcch_blind_monitor_rt.c (CORESET-0 occupancy gate). */
void nr_pdcch_coreset_map_get_css0(int *first_rb, int *n_rb)
{
  *first_rb = g_css0_first_rb;
  *n_rb = g_css0_nrb;
}

#define IDSW_DUR_MAX 3 /* a CORESET spans at most 3 OFDM symbols (TS 38.211 7.3.2.2) */

/** Symbols captured per snapshot: the full 3, so 1-, 2- and 3-symbol CORESETs are scored on the same window. */
int nr_pdcch_coreset_map_idsweep_dur(void)
{
  return IDSW_DUR_MAX;
}

/* Capture metrics have one producer; readers use relaxed atomic snapshots. */
static _Atomic uint64_t nid_capture_offered, nid_capture_taken, nid_capture_skipped_empty;
static _Atomic float nid_capture_noise;
static _Atomic int nid_gate_bypass; /* ACQMISS recovery: capture every offered symbol (occupancy gate off) */
void nr_pdcch_coreset_map_idsweep_gate_bypass(int on) { atomic_store_explicit(&nid_gate_bypass, on, memory_order_relaxed); }
/* nr_pdcch_nid_capture_metrics: returns the NID capture counters: slots offered to the capture, captures taken, captures skipped as empty, and the latest noise level of the occupancy gate. Reads atomics. Caller: nr_passive_metrics.c (metrics emission). */
void nr_pdcch_nid_capture_metrics(uint64_t *offered, uint64_t *taken, uint64_t *empty, float *noise)
{
  *offered = atomic_load_explicit(&nid_capture_offered, memory_order_relaxed);
  *taken = atomic_load_explicit(&nid_capture_taken, memory_order_relaxed);
  *empty = atomic_load_explicit(&nid_capture_skipped_empty, memory_order_relaxed);
  *noise = atomic_load_explicit(&nid_capture_noise, memory_order_relaxed);
}

/* nr_pdcch_coreset_map_idsweep_want: decides whether this DL slot should feed the scrambling-ID sweep capture, and at which start symbol: every 2nd slot, rotating the start over the first 3 symbols.
 *   Requires the sweep running, the carrier verified and CORESET#0 known. Returns the start symbol, or -1 to skip. Caller: the scan path of nr_pdcch_blind_monitor_rt.c.
 */
int nr_pdcch_coreset_map_idsweep_want(uint32_t abs_slot, int sps)
{
  static uint32_t dl_calls, cap;
  if (!nr_pdcch_nid_gpu_running() || sps <= 0) return -1;
  { uint64_t co = 0, ct = 0, ce = 0; float nz; nr_pdcch_nid_capture_metrics(&co, &ct, &ce, &nz);
    if (nr_cell_state_sweep_blocked(abs_slot, co, ct, ce)) return -1; } /* PCIS2: ISAC_CELL_STATE=2, barred/idle cell */
  const nr_passive_acq_snapshot_t snap = nr_passive_acq_snapshot();
  if (snap.carrier_verified == 0 || g_css0_first_rb < 0) /* Point A (SIB1) and CORESET#0 (MIB) first */
    return -1;
  if (nr_syncq_gate_closed && nr_syncq_gate_closed()) /* ACQMISS-2: unusable symbols (timing unstable) are not worth GPU time */
    return -1;
  if (!nr_passive_acq_tdd_slot_has_downlink(abs_slot))
    return -1;
  if ((dl_calls++ % 2) != 0)
    return -1;
  /* PDCCH CORESETs sit in the first symbols; on PCI 415 (OTA 2026-10-07) the dedicated CORESET is at symbol 0 and a
   * 14-symbol rotation sampled it 1/14 of the time (first record after ~627 passes / ~50 s). */
  const int starts = sps < 3 ? sps : 3;
  return (int)(cap++ % (uint32_t)starts);
}

/* nr_pdcch_coreset_map_idsweep_push: hands one FFT'd symbol of a DL slot to the NID capture: applies the CSS0 occupancy gate and pushes the symbol to the GPU ring.
 *   Keeps the occupancy history per configuration and resets it on a sync loss or configuration change. Caller: the blind PDCCH monitor scan path.
 */
void nr_pdcch_coreset_map_idsweep_push(const c16_t *rxF_sym, int fft, int fco, int n_rb, int slot, int sym, uint16_t pci, int sps)
{
  const nr_passive_acq_snapshot_t snap = nr_passive_acq_snapshot();
  const int nsc = 12 * n_rb;
  const int dur = nr_pdcch_coreset_map_idsweep_dur();
  const int pa = snap.carrier.point_a_subcarrier;
  const int pa_mod = ((pa % 12) + 12) % 12;
  const nr_pdcch_nid_config_t cfg = {nsc, pa, (12 * g_css0_first_rb + pa_mod - pa) / 12,
                                    g_css0_nrb, pci, sps, dur};
  const float ratio = 3.981071706f; /* 6 dB: margin above 54-RE noise averages */
  const int floor_pct = 10;
  static nr_nid_occupancy_t occupancy;
  static nr_pdcch_nid_config_t previous;
  static uint64_t previous_sync_losses;
  if (memcmp(&previous, &cfg, sizeof(cfg)) || previous_sync_losses != snap.sync_losses) {
    memset(&occupancy, 0, sizeof(occupancy)); previous = cfg; previous_sync_losses = snap.sync_losses;
  }
  atomic_fetch_add_explicit(&nid_capture_offered, 1, memory_order_relaxed);
  bool take = atomic_load_explicit(&nid_gate_bypass, memory_order_relaxed) || nr_nid_occupancy_take(&occupancy, rxF_sym, fft, fco, nsc, pa,
                                                   cfg.css0_crb, cfg.css0_nrb, sym, ratio, floor_pct);
  if (sym >= 0 && sym < 14)
    atomic_store_explicit(&nid_capture_noise, occupancy.noise[sym], memory_order_relaxed);
  if (!take) {
    atomic_fetch_add_explicit(&nid_capture_skipped_empty, 1, memory_order_relaxed);
    return;
  }
  if (!nr_pdcch_nid_gpu_push(rxF_sym, fft, fco, slot, sym, &cfg))
    atomic_fetch_add_explicit(&nid_capture_taken, 1, memory_order_relaxed);
}

int nr_pdcch_coreset_map_scan(const c16_t* rxdataF,
                              int          ofdm_symbol_size,
                              int          n_rb_carrier,
                              int          first_carrier_offset,
                              uint16_t     scrambling_id,
                              int          slot,
                              int          symbol,
                              nr_pdcch_coreset_candidate_t* candidates_out,
                              int          max_candidates)
{
  /* WINDOW GRID PHASE. A CORESET's 6-RB groups start at ITS first RB, not at CRB 0; indexing from
   * 0 makes every window straddle two REG bundles (independent precoders under
   * precoderGranularity = sameAsREG-bundle). MEASURED: +1 RB took the correlation 0.510 -> 0.607
   * on this cell. Derived from the caller's known CORESET start; 0 = previous behaviour. */
  int rb_phase = nr_pdcch_coreset_map_phase_hint() % 6;
  if (rb_phase < 0) rb_phase = 0;
  const int n_windows = (n_rb_carrier - rb_phase) / 6;
  if (n_windows <= 0 || max_candidates <= 0) {
    return 0;
  }

  // Generate the reference DM-RS for the whole carrier once, indexed by absolute RB (rb*3 + p),
  // exactly mirroring dci_nr.c's own pilot layout for nr_pdcch_channel_estimation().
  uint32_t *gold = nr_gold_pdcch(n_rb_carrier, 14, scrambling_id, slot, symbol);
  c16_t pilot[n_rb_carrier * 3];
  nr_pdcch_dmrs_ref(gold, pilot, (unsigned short)n_rb_carrier);

  int found = 0;
  double wcorr[NR_PDCCH_MAX_CANDIDATE_WINDOWS];
  for (int w = 0; w < n_windows && w < NR_PDCCH_MAX_CANDIDATE_WINDOWS; w++) {
    const int rb_offset = rb_phase + w * 6;
    /* One coherent sum over all 18 pilots; valid on a flat channel (see the note above). */
    double cr = 0.0, ci = 0.0, py = 0.0, px = 0.0;
    for (int rb = rb_offset; rb < rb_offset + 6; rb++) {
      for (int p = 0; p < 3; p++) {
        const int k = (first_carrier_offset + rb * 12 + 1 + 4 * p) % ofdm_symbol_size;
        const c16_t y = rxdataF[k];
        const c16_t x = pilot[rb * 3 + p];  // already conj(transmitted DM-RS)
        cr += (double)y.r * x.r - (double)y.i * x.i;
        ci += (double)y.r * x.i + (double)y.i * x.r;
        py += (double)y.r * y.r + (double)y.i * y.i;
        px += (double)x.r * x.r + (double)x.i * x.i;
      }
    }
    const double denom = sqrt(py * px);
    const double corr = (denom > 0.0) ? sqrt(cr * cr + ci * ci) / denom : 0.0;
    wcorr[w] = corr;
  }
  /* ADAPTIVE THRESHOLD (2026-09-16). The fixed 0.836 was never reached on the X410 (peaks 0.43-0.52 in
   * every run), so no CORESET was ever discovered by correlation. A window carrying a
   * PDCCH is an OUTLIER against this symbol's own population of 6-RB windows: threshold = median +
   * 4 x MAD over the windows (MAD scaled to sigma), floored at the old constant only when the
   * population is so clean that the fixed bar is the lower one. Empty symbols have no outliers and
   * contribute nothing, as before. */
  double thr = CORESET_MAP_CORR_THRESHOLD;
  if (n_windows >= 8) {
    double v[NR_PDCCH_MAX_CANDIDATE_WINDOWS];
    int m = n_windows < NR_PDCCH_MAX_CANDIDATE_WINDOWS ? n_windows : NR_PDCCH_MAX_CANDIDATE_WINDOWS;
    for (int i = 0; i < m; i++) v[i] = wcorr[i];
    for (int i = 1; i < m; i++) { double x = v[i]; int j = i - 1; while (j >= 0 && v[j] > x) { v[j + 1] = v[j]; j--; } v[j + 1] = x; }
    const double med = v[m / 2];
    for (int i = 0; i < m; i++) v[i] = fabs(wcorr[i] - med);
    for (int i = 1; i < m; i++) { double x = v[i]; int j = i - 1; while (j >= 0 && v[j] > x) { v[j + 1] = v[j]; j--; } v[j + 1] = x; }
    const double sigma = 1.4826 * v[m / 2];
    /* Absolute floor 0.35: a window of 18 random pilot products reads ~0.1-0.25 (OTA rb0_corr,
     * rfsim empty windows) while a real PDCCH reads 0.5 (OTA) to 0.9+ (rfsim); on noiseless rfsim
     * the MAD is tiny and med + 4 MAD let 0.3 windows through as false CORESET hits. */
    double adaptive = med + 4.0 * (sigma > 0.02 ? sigma : 0.02);
    if (adaptive < 0.35) adaptive = 0.35;
    thr = adaptive < CORESET_MAP_CORR_THRESHOLD ? adaptive : CORESET_MAP_CORR_THRESHOLD;
  }
  /* OCCUPANCY GATE: without it the histogram integrates the false-positive rate of ~95 % empty slots until every window looks lit. */
  double peak = 0.0;
  for (int w = 0; w < n_windows && w < NR_PDCCH_MAX_CANDIDATE_WINDOWS; w++)
    if (wcorr[w] > peak) peak = wcorr[w];
  const bool occupied = peak >= 0.55;
  for (int w = 0; occupied && w < n_windows; w++) {
    if (wcorr[w] >= thr && found < max_candidates) {
      candidates_out[found].rb_offset = rb_phase + w * 6;
      candidates_out[found].corr      = wcorr[w];
      found++;
    }
  }
  // Insertion sort by descending corr -- found is small (<= max_candidates), no need for qsort.
  for (int i = 1; i < found; i++) {
    nr_pdcch_coreset_candidate_t v = candidates_out[i];
    int j = i - 1;
    while (j >= 0 && candidates_out[j].corr < v.corr) {
      candidates_out[j + 1] = candidates_out[j];
      j--;
    }
    candidates_out[j + 1] = v;
  }
  return found;
}

/* Per-window PDCCH DM-RS correlation with an explicit reference RB (passive BWP/CORESET discovery).
 * 38.211 7.4.1.3.2 references a PDCCH-Config CORESET's DM-RS to CRB 0 (ref_rb = 0); OAI references it
 * to the BWP start instead (see nr_pdcch_blind_monitor_rt.h's coreset_type), so a dedicated-BWP
 * CORESET on an OAI cell only correlates when ref_rb is that BWP's first RB. `pilot` is the
 * conjugated reference for n_pilot_rb RBs from nr_pdcch_coreset_pilot(), indexed from ref_rb.
 * Returns |corr| in [0,1], or -1 when the window starts below ref_rb or runs past the pilots. */
double nr_pdcch_coreset_window_corr(const c16_t *rxdataF, int ofdm_symbol_size, int first_carrier_offset,
                                    const c16_t *pilot, int n_pilot_rb, int rb_offset, int ref_rb)
{
  if (rb_offset < ref_rb || rb_offset - ref_rb + 6 > n_pilot_rb)
    return -1.0;
  double cr = 0.0, ci = 0.0, py = 0.0, px = 0.0;
  for (int rb = rb_offset; rb < rb_offset + 6; rb++) {
    for (int p = 0; p < 3; p++) {
      const int k = (first_carrier_offset + rb * 12 + 1 + 4 * p) % ofdm_symbol_size;
      const c16_t y = rxdataF[k];
      const c16_t x = pilot[(rb - ref_rb) * 3 + p];
      cr += (double)y.r * x.r - (double)y.i * x.i;
      ci += (double)y.r * x.i + (double)y.i * x.r;
      py += (double)y.r * y.r + (double)y.i * y.i;
      px += (double)x.r * x.r + (double)x.i * x.i;
    }
  }
  const double denom = sqrt(py * px);
  return denom > 0.0 ? sqrt(cr * cr + ci * ci) / denom : 0.0;
}

/* Conjugated PDCCH DM-RS for n_rb RBs of one (slot, symbol), sequence index 0 at the reference RB. */
void nr_pdcch_coreset_pilot(uint16_t scrambling_id, int slot, int symbol, int n_rb, c16_t *pilot)
{
  uint32_t *gold = nr_gold_pdcch(n_rb, 14, scrambling_id, slot, symbol);
  nr_pdcch_dmrs_ref(gold, pilot, (unsigned short)n_rb);
}


bool nr_pdcch_dmrs_rank_grid(nr_pdcch_dmrs_rank_grid_t *g, const c16_t *rx,
                             int fft, int carrier, int n_rb, uint16_t id,
                             int slot, int first_symbol, int duration, int reference_rb)
{
  if (!g) return false;
  g->n_rb = g->duration = 0;
  if (!rx || fft < n_rb * 12 || carrier < 0 || carrier >= fft || n_rb < 1
      || n_rb > NR_PDCCH_RANK_MAX_RB || duration < 1 || duration > 3
      || first_symbol < 0 || first_symbol + duration > 14 || slot < 0
      || reference_rb < 0 || reference_rb >= n_rb)
    return false;
  for (int s = 0; s < duration; ++s) {
    c16_t pilot[NR_PDCCH_RANK_MAX_RB * 3];
    nr_pdcch_coreset_pilot(id, slot, first_symbol + s, n_rb, pilot);
    g->re[s][0] = g->im[s][0] = g->py[s][0] = g->px[s][0] = 0;
    for (int rb = 0; rb < n_rb; ++rb) {
      double re = 0, im = 0, py = 0, px = 0;
      if (rb >= reference_rb) {
        for (int q = 0; q < 3; ++q) {
          const c16_t y = rx[(first_symbol + s) * fft + (carrier + rb * 12 + 1 + 4*q) % fft];
          const c16_t x = pilot[(rb - reference_rb) * 3 + q]; /* conjugated reference */
          re += (double)y.r*x.r - (double)y.i*x.i;
          im += (double)y.r*x.i + (double)y.i*x.r;
          py += (double)y.r*y.r + (double)y.i*y.i;
          px += (double)x.r*x.r + (double)x.i*x.i;
        }
      }
      g->re[s][rb+1] = g->re[s][rb] + re;
      g->im[s][rb+1] = g->im[s][rb] + im;
      g->py[s][rb+1] = g->py[s][rb] + py;
      g->px[s][rb+1] = g->px[s][rb] + px;
    }
  }
  g->n_rb = n_rb;
  g->duration = duration;
  return true;
}

int nr_pdcch_candidate_rbs(int span, int duration, int bundle, int interleaver, int shift,
                            int cce, int al, uint16_t *rbs, int capacity)
{
  const int b = bundle ? bundle : 6;
  if (!rbs || span < 6 || span > NR_PDCCH_RANK_MAX_RB || span % 6 || duration < 1 || duration > 3
      || (b != 2 && b != 3 && b != 6) || b % duration || (bundle && duration < 3 && b == 3)
      || (al != 1 && al != 2 && al != 4 && al != 8 && al != 16)
      || cce < 0 || cce % al || cce + al > span * duration / 6 || shift < 0
      || capacity < al * 6 / duration)
    return -1;
  const int nb = span * duration / b;
  if (bundle && ((interleaver != 2 && interleaver != 3 && interleaver != 6) || nb % interleaver))
    return -1;
  bool used[NR_PDCCH_RANK_MAX_RB] = {false};
  for (int j = cce * 6/b; j < (cce+al)*6/b; ++j) {
    const int f = bundle ? ((j % interleaver) * (nb / interleaver) + j / interleaver + shift) % nb : j;
    for (int k = 0; k < b / duration; ++k) used[f * (b / duration) + k] = true;
  }
  int n = 0;
  for (int rb = 0; rb < span; ++rb) if (used[rb]) rbs[n++] = rb;
  return n;
}

double nr_pdcch_dmrs_candidate_score(const nr_pdcch_dmrs_rank_grid_t *g, int offset,
                                       int span, int bundle, int interleaver, int shift, int cce, int al)
{
  if (!g || offset < 0 || offset + span > g->n_rb || g->duration < 1) return -INFINITY;
  uint16_t rbs[96];
  const int n = nr_pdcch_candidate_rbs(span, g->duration, bundle, interleaver, shift, cce, al, rbs, 96);
  if (n < 1) return -INFINITY;
  const int rb_bundle = (bundle ? bundle : 6) / g->duration;
  double sum = 0;
  int count = 0;
  /* Coherent within each REG bundle and symbol, power-combine bundles/symbols. This preserves
   * the wide coherent span without assuming that independent precoders share phase. */
  for (int i = 0; i < n; i += rb_bundle) {
    const int lo = offset + rbs[i], hi = lo + rb_bundle;
    for (int s = 0; s < g->duration; ++s) {
      const double re = g->re[s][hi] - g->re[s][lo], im = g->im[s][hi] - g->im[s][lo];
      const double py = g->py[s][hi] - g->py[s][lo], px = g->px[s][hi] - g->px[s][lo];
      const double rho2 = py > 0 && px > 0 ? (re*re + im*im) / (py*px) : 0;
      const double noise_mean = 1.0 / (3 * rb_bundle);
      sum += (rho2 - noise_mean) / (1.0 - noise_mean);
      count++;
    }
  }
  return sum / count;
}

int nr_pdcch_dmrs_candidate_order(const double *score, const uint8_t *al, int n,
                                    uint64_t visit, bool full, uint8_t *order)
{
  if (!score || !al || !order || n < 0 || n > NR_PDCCH_RANK_MAX_CAND) return -1;
  bool keep[NR_PDCCH_RANK_MAX_CAND] = {false};
  for (int level = 1; level <= 16; level *= 2) {
    int idx[NR_PDCCH_RANK_MAX_CAND], m = 0;
    for (int i = 0; i < n; ++i) if (al[i] == level) idx[m++] = i;
    if (!m) continue;
    /* Exploration is chosen from the original order, BEFORE sorting; even a perpetually weak
     * candidate is visited once every m opportunities. No DMRS hard veto. */
    keep[idx[visit % m]] = true;
    for (int i = 1; i < m; ++i) {
      int v = idx[i], j = i;
      while (j > 0 && (isfinite(score[v]) ? score[v] : -INFINITY)
                         > (isfinite(score[idx[j-1]]) ? score[idx[j-1]] : -INFINITY)) {
        idx[j] = idx[j-1]; --j;
      }
      idx[j] = v;
    }
    /* One strongest plus one rotating exploration point per AL. This bound is independent
     * of CORESET width; every candidate is still visited, while wide extents no longer make the
     * prepass grow linearly to tens of candidates per AL. */
    const int budget = full ? m : (m < 2 ? m : 2);
    int have = 1;
    for (int j = 0; j < m && have < budget; ++j)
      if (!keep[idx[j]]) { keep[idx[j]] = true; ++have; }
  }
  /* Round-robin aggregation levels. A global score sort starved AL1/2 whenever AL8 had a
   * stronger prior, and a bounded length sweep then never reached the lower-AL candidates. Scores
   * remain descending within each AL; AL is a separate unknown, not a comparable score term. */
  bool emitted[NR_PDCCH_RANK_MAX_CAND] = {false};
  int out = 0;
  for (;;) {
    bool any = false;
    for (int level = 1; level <= 16; level *= 2) {
      int best = -1;
      for (int i = 0; i < n; ++i) {
        if (al[i] != 1 && al[i] != 2 && al[i] != 4 && al[i] != 8 && al[i] != 16) return -1;
        if (!keep[i] || emitted[i] || al[i] != level) continue;
        if (best < 0 || (isfinite(score[i]) ? score[i] : -INFINITY)
                         > (isfinite(score[best]) ? score[best] : -INFINITY))
          best = i;
      }
      if (best >= 0) {
        order[out++] = (uint8_t)best;
        emitted[best] = true;
        any = true;
      }
    }
    if (!any) break;
  }
  return out;
}


void nr_pdcch_uss_candidate_supports(int n_cces, int slot, const uint16_t *rntis, int n_rntis,
                                     const uint16_t *cce, const uint8_t *al, int n_candidates,
                                     uint16_t *support)
{
  if (!support || n_candidates <= 0)
    return;
  memset(support, 0, (size_t)n_candidates * sizeof(*support));
  if (n_cces <= 0 || slot < 0 || !rntis || n_rntis <= 0 || !cce || !al)
    return;

  static const uint32_t A[3] = {39827, 39829, 39839};
  static const uint8_t M_set[] = {1, 2, 3, 4, 5, 6, 8};
  /* Direct (AL, CCE/L) lookup removes the old O(candidate-count) scan from every hash
   * hypothesis. Duration 3 permits 135 CCEs, so 136 is the complete 38.331 CORESET domain. */
  int16_t lookup[5][136];
  for (int ai = 0; ai < 5; ++ai)
    for (int pos = 0; pos < 136; ++pos)
      lookup[ai][pos] = -1;
  for (int i = 0; i < n_candidates; ++i) {
    int ai = 0;
    while (ai < 5 && (1 << ai) != al[i])
      ++ai;
    if (ai < 5 && cce[i] % al[i] == 0 && cce[i] / al[i] < 136)
      lookup[ai][cce[i] / al[i]] = (int16_t)i;
  }
  for (int r = 0; r < n_rntis; ++r) {
    if (rntis[r] == 0)
      continue;
    for (int cid = 0; cid < 3; ++cid) {
      uint32_t Y = rntis[r];
      for (int s = 0; s <= slot; ++s)
        Y = (A[cid] * Y) % 65537u;
      for (int ai = 0; ai < 5; ++ai) {
        const int L = 1 << ai;
        const int N = n_cces / L;
        if (N < 1)
          continue;
        for (unsigned mi = 0; mi < sizeof(M_set) / sizeof(M_set[0]); ++mi) {
          const int M = M_set[mi];
          bool seen[136] = {false};
          for (int m = 0; m < M; ++m) {
            const int cce_pos = (int)((Y + (uint32_t)((m * n_cces) / (L * M))) % (uint32_t)N);
            if (cce_pos >= (int)(sizeof(seen) / sizeof(seen[0])) || seen[cce_pos])
              continue;
            seen[cce_pos] = true;
            const int i = lookup[ai][cce_pos];
            if (i >= 0 && support[i] != UINT16_MAX)
              support[i]++;
          }
        }
      }
    }
  }
}
