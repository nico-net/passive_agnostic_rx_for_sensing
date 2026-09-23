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


/* Pure-noise moments of a COHERENT correlation over n=3 complex pilots (one RB's DM-RS):
 *   E|corr|   = sqrt(pi)/(2*sqrt(n)),  sd|corr| = sqrt(1 - pi/4)/sqrt(n).
 * Both are exact for circular complex Gaussian noise and are what make Z below N(0,1). */
#define NR_PDCCH_SUBBAND_MU0    0.5116633539
#define NR_PDCCH_SUBBAND_SIGMA0 0.2675794151

/* DEFAULT = COHERENT, and that is a MEASURED choice, not a leftover. The 18-pilot coherent sum has
 * noise floor sqrt(pi)/(2*sqrt(18)) = 0.209 against a signal of ~0.9, i.e. 4x separation. The
 * per-RB sub-band statistic has floor sqrt(pi)/(2*sqrt(3)) = 0.512 against a ceiling of exactly 1.0
 * -- under 2x -- so non-coherent combining across 6 RBs has strictly WORSE detection power whenever
 * the channel is flat enough for the coherent sum to be valid. Measured 2026-09-21: with sub-band
 * as the default, tests/nr_pdcch_coreset_map_test.cc FindsOneSyntheticallyOccupiedWindowAboveNoise
 * FAILS (n=0) while the coherent statistic passes. Sub-band was added on the hypothesis that
 * dispersion was destroying coherence on the macro; that hypothesis was REFUTED (it recovered
 * nothing on air either -- CORESET#0 control mean Z 1.36 vs the 1.42 noise expectation), so it
 * stays available for a genuinely dispersive bed but must not be the default.
 * ISAC_COREMAP_SUBBAND=1 opts in. */
static int coherent_mode(void)
{
  static int s_coh = -1;
  if (s_coh < 0) {
    const char *e = getenv("ISAC_COREMAP_SUBBAND");
    s_coh = (e != NULL && atoi(e) != 0) ? 0 : 1;
  }
  return s_coh;
}

/* Per-RB coherent correlation, magnitudes combined non-coherently across `n_rb` RBs starting at
 * `rb_start`, returned as a noise-normalised Z score. See this file's header note for why the
 * coherent span must be one RB on a dispersive channel. */
static double nr_pdcch_subband_z(const c16_t *rxdataF, const c16_t *pilot, int ofdm_symbol_size,
                                 int first_carrier_offset, int rb_start, int n_rb)
{
  if (n_rb <= 0) {
    return 0.0;
  }
  double acc = 0.0;
  int used = 0;
  for (int rb = rb_start; rb < rb_start + n_rb; rb++) {
    double cr = 0.0, ci = 0.0, py = 0.0, px = 0.0;
    for (int p = 0; p < 3; p++) {
      const int k = (first_carrier_offset + rb * 12 + 1 + 4 * p) % ofdm_symbol_size;
      const c16_t y = rxdataF[k];
      const c16_t x = pilot[rb * 3 + p];  // already conj(transmitted DM-RS)
      cr += (double)y.r * x.r - (double)y.i * x.i;
      ci += (double)y.r * x.i + (double)y.i * x.r;
      py += (double)y.r * y.r + (double)y.i * y.i;
      px += (double)x.r * x.r + (double)x.i * x.i;
    }
    const double den = sqrt(py * px);
    if (den > 0.0) {
      acc += sqrt(cr * cr + ci * ci) / den;
      used++;
    }
  }
  if (used == 0) {
    return 0.0;
  }
  const double a = acc / (double)used;
  return (a - NR_PDCCH_SUBBAND_MU0) / (NR_PDCCH_SUBBAND_SIGMA0 / sqrt((double)used));
}


/* See nr_pdcch_coreset_map.h. Correlates the KNOWN CORESET in a buffer that has just produced a
 * CRC-verified DCI, so occupancy is not assumed -- it is proven by the decode. */
void nr_pdcch_coreset_map_accept_probe(const c16_t *rxdataF, int ofdm_symbol_size, int n_rb_carrier,
                                       int first_carrier_offset, uint16_t scrambling_id, int slot,
                                       int symbol, int cs_start, int cs_nrb)
{
  static int s_on = -1;
  if (s_on < 0) {
    const char *e = getenv("ISAC_COREMAP_ONACCEPT");
    s_on = (e != NULL && atoi(e) != 0) ? 1 : 0;
  }
  if (!s_on || rxdataF == NULL || cs_nrb < 6 || cs_start < 0 || cs_start + cs_nrb > n_rb_carrier) {
    return;
  }
  uint32_t *gold = nr_gold_pdcch(n_rb_carrier, 14, scrambling_id, slot, symbol);
  c16_t pilot[n_rb_carrier * 3];
  nr_pdcch_dmrs_ref(gold, pilot, (unsigned short)n_rb_carrier);

  const int cs_sc = (first_carrier_offset + cs_start * 12) % ofdm_symbol_size;
  double best = 0.0;
  int best_cce = -1;
  for (int cce = 0; cce * 6 + 6 <= cs_nrb; cce++) {
    double cr = 0.0, ci = 0.0, py = 0.0, px = 0.0;
    for (int rb = cce * 6; rb < cce * 6 + 6; rb++) {
      for (int q = 0; q < 3; q++) {
        const int k = (cs_sc + rb * 12 + 1 + 4 * q) % ofdm_symbol_size;
        const c16_t y = rxdataF[k];
        const c16_t x = pilot[(cs_start + rb) * 3 + q];
        cr += (double)y.r * x.r - (double)y.i * x.i;
        ci += (double)y.r * x.i + (double)y.i * x.r;
        py += (double)y.r * y.r + (double)y.i * y.i;
        px += (double)x.r * x.r + (double)x.i * x.i;
      }
    }
    const double den = sqrt(py * px);
    const double a = (den > 0.0) ? sqrt(cr * cr + ci * ci) / den : 0.0;
    if (a > best) { best = a; best_cce = cce; }
  }
  /* PER-RB CORRELATIONS of the best CCE, and the CSI-style block score at several sub-band sizes.
   * See this function's header note. */
  if (best_cce >= 0) {
    double rbv[6];
    for (int i = 0; i < 6; i++) {
      const int rb = best_cce * 6 + i;
      double cr = 0.0, ci = 0.0, py = 0.0, px = 0.0;
      for (int q = 0; q < 3; q++) {
        const int k = (cs_sc + rb * 12 + 1 + 4 * q) % ofdm_symbol_size;
        const c16_t y = rxdataF[k];
        const c16_t x = pilot[(cs_start + rb) * 3 + q];
        cr += (double)y.r * x.r - (double)y.i * x.i;
        ci += (double)y.r * x.i + (double)y.i * x.r;
        py += (double)y.r * y.r + (double)y.i * y.i;
        px += (double)x.r * x.r + (double)x.i * x.i;
      }
      const double den = sqrt(py * px);
      rbv[i] = (den > 0.0) ? sqrt(cr * cr + ci * ci) / den : 0.0;
    }
    /* CSI contract: noise ~1.0 at any m, correct sequence ~1.128*sqrt(m). m in REs (3 per RB). */
    double blk[4];
    static const int kRB[4] = {1, 2, 3, 6};
    for (int bi = 0; bi < 4; bi++) {
      const int rb_per = kRB[bi];
      const int m = rb_per * 3;
      double acc = 0.0;
      int nb = 0;
      for (int b = 0; b + rb_per <= 6; b += rb_per) {
        double cr = 0.0, ci = 0.0, py = 0.0, px = 0.0;
        for (int i = 0; i < rb_per; i++) {
          const int rb = best_cce * 6 + b + i;
          for (int q = 0; q < 3; q++) {
            const int k = (cs_sc + rb * 12 + 1 + 4 * q) % ofdm_symbol_size;
            const c16_t y = rxdataF[k];
            const c16_t x = pilot[(cs_start + rb) * 3 + q];
            cr += (double)y.r * x.r - (double)y.i * x.i;
            ci += (double)y.r * x.i + (double)y.i * x.r;
            py += (double)y.r * y.r + (double)y.i * y.i;
            px += (double)x.r * x.r + (double)x.i * x.i;
          }
        }
        const double den = sqrt(py * px);
        if (den > 0.0) {
          acc += (sqrt(cr * cr + ci * ci) / den) / (0.8862269255 / sqrt((double)m));
          nb++;
        }
      }
      blk[bi] = nb ? acc / (double)nb : 0.0;
    }
    static double s_rb[6], s_blk[4];
    static unsigned long s_rbn;
    for (int i = 0; i < 6; i++) s_rb[i] += rbv[i];
    for (int i = 0; i < 4; i++) s_blk[i] += blk[i];
    s_rbn++;
    if ((s_rbn % 200) == 0) {
      printf("COREMAPRB n=%lu perRB=%.3f %.3f %.3f %.3f %.3f %.3f | blockscore(noise=1) "
             "1RB=%.2f 2RB=%.2f 3RB=%.2f 6RB=%.2f | perfect=%.2f %.2f %.2f %.2f\n",
             s_rbn, s_rb[0] / s_rbn, s_rb[1] / s_rbn, s_rb[2] / s_rbn, s_rb[3] / s_rbn,
             s_rb[4] / s_rbn, s_rb[5] / s_rbn,
             s_blk[0] / s_rbn, s_blk[1] / s_rbn, s_blk[2] / s_rbn, s_blk[3] / s_rbn,
             1.128 * sqrt(3.0), 1.128 * sqrt(6.0), 1.128 * sqrt(9.0), 1.128 * sqrt(18.0));
      fflush(stdout);
    }
  }

  /* RB-PHASE PROBE -- see this function's header note: measured, not assumed. */
  double dz[5];
  for (int di = 0; di < 5; di++) {
    const int d = di - 2;
    const int st = cs_start + d;
    dz[di] = 0.0;
    if (st < 0 || st + cs_nrb > n_rb_carrier) {
      continue;
    }
    const int sc_d = (first_carrier_offset + st * 12) % ofdm_symbol_size;
    for (int cce = 0; cce * 6 + 6 <= cs_nrb; cce++) {
      double cr = 0.0, ci = 0.0, py = 0.0, px = 0.0;
      for (int rb = cce * 6; rb < cce * 6 + 6; rb++) {
        for (int q = 0; q < 3; q++) {
          const int k = (sc_d + rb * 12 + 1 + 4 * q) % ofdm_symbol_size;
          const c16_t y = rxdataF[k];
          const c16_t x = pilot[(st + rb) * 3 + q];
          cr += (double)y.r * x.r - (double)y.i * x.i;
          ci += (double)y.r * x.i + (double)y.i * x.r;
          py += (double)y.r * y.r + (double)y.i * y.i;
          px += (double)x.r * x.r + (double)x.i * x.i;
        }
      }
      const double den = sqrt(py * px);
      const double a = (den > 0.0) ? sqrt(cr * cr + ci * ci) / den : 0.0;
      if (a > dz[di]) dz[di] = a;
    }
  }
  static double s_sum = 0.0, s_max = 0.0, s_dsum[5];
  static unsigned long s_n = 0;
  s_sum += best;
  s_n++;
  if (best > s_max) s_max = best;
  for (int di = 0; di < 5; di++) {
    s_dsum[di] += dz[di];
  }
  if (s_n <= 5 || (s_n % 200) == 0) {
    printf("COREMAPACC n=%lu slot=%d sym=%d cset=%d+%d best=%.3f cce=%d mean=%.3f max=%.3f "
           "phase[-2..+2]=%.3f %.3f %.3f %.3f %.3f\n",
           s_n, slot, symbol, cs_start, cs_nrb, best, best_cce, s_sum / (double)s_n, s_max,
           s_dsum[0] / (double)s_n, s_dsum[1] / (double)s_n, s_dsum[2] / (double)s_n,
           s_dsum[3] / (double)s_n, s_dsum[4] / (double)s_n);
    fflush(stdout);
  }
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

/* STAGE 1: PDCCH DM-RS scrambling-ID discovery, decode-free, fully blind (ISAC_COREMAP_IDSWEEP=1).
 *
 * Nothing about the cell is assumed:
 *  - ID: all 65536 values.
 *  - Symbol: all 14 (a CORESET may start at any symbol of the slot); every (ID, symbol) is scored.
 *  - Frequency reference (TS 38.211 7.4.1.3.2): a PDCCH-Config CORESET's DM-RS is indexed from
 *    CRB 0 = Point A, CORESET#0's from its own first RB. Point A comes from SIB1 (offsetToPointA)
 *    measured against where the SSB was FOUND in the grid (nr_passive_acq_snapshot()); the sweep
 *    does not start until that is known. CORESET#0 is placed exactly from the MIB on the same CRB
 *    grid. The previous sweep indexed the pilot by the receiver's own RB number, which is right only
 *    when the receive grid happens to start at Point A.
 *  - Slots: only slots SIB1's TDD pattern marks as downlink (unknown pattern = all).
 *
 * The receiver only captures single-symbol snapshots and dumps them (/tmp/passive_rx/idsweep_NNN.bin);
 * the sweep runs OFFLINE in tools idsweep_offline (captures/idsweep_offline.c). Each
 * snapshot (slot, symbol) is standardised across all 65536 IDs (robust median/MAD), so a busy region
 * and a sparse one are treated alike with no occupancy gate. Two scores per (region, symbol, ID):
 *  - hits: snapshots in which this ID's z exceeds the extreme-value bound for 65536 draws
 *    (sqrt(2 ln N) + 2 ~= 6.7, derived from N, not from any cell) -- the decision statistic;
 *  - zsum/sqrt(n): the accumulated evidence, reported for margin. */
#include <pthread.h>
#include "openair1/PHY/gold.h"
#include "nr_passive_acq_state.h"
#include <sys/stat.h>

static int g_css0_first_rb = -1, g_css0_nrb = 0;
void nr_pdcch_coreset_map_set_css0(int first_rb, int n_rb)
{
  g_css0_first_rb = first_rb;
  g_css0_nrb = n_rb;
}

#define IDSW_NID 65536
#define IDSW_NSYM 14
#define IDSW_PER_SYM 24
#define IDSW_NSNAP (IDSW_NSYM * IDSW_PER_SYM) /* single-symbol snapshots per batch */
#define IDSW_BINS 4096

static struct {
  pthread_mutex_t mu;
  pthread_cond_t cv;
  int enabled;          /* -1 unread, 0 off, 1 on */
  int full;             /* ring full, owned by the worker */
  int n;                /* snapshots captured */
  int nsc, point_a_sc, css0_crb, css0_nrb;
  int slot[IDSW_NSNAP], sym[IDSW_NSNAP];
  c16_t *re;            /* [IDSW_NSNAP][nsc] */
  int max_batches;
  unsigned nsnap;       /* snapshots per symbol so far (round-robin keeps them equal) */
  uint32_t cap;         /* capture counter: selects the symbol */
  unsigned batches;
  uint16_t pci;
  uint32_t dl_calls;
  int sps;              /* symbols per slot (CP-dependent), from the PHY */
} g_idsw = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, -1};

/* Dump one batch for the offline sweep (idsweep_offline). The sweep itself must NOT run in the
 * receiver: in-process, even at SCHED_IDLE, it starved the receive path and the X410 stream died on an
 * overflow the moment the first batch started processing (runs s01d/s01e, 2026-09-22). */
static void *idsw_worker(void *arg)
{
  (void)arg;
  for (;;) {
    pthread_mutex_lock(&g_idsw.mu);
    while (!g_idsw.full)
      pthread_cond_wait(&g_idsw.cv, &g_idsw.mu);
    pthread_mutex_unlock(&g_idsw.mu);
    char path[128];
    mkdir("/tmp/passive_rx", 0777);
    snprintf(path, sizeof(path), "/tmp/passive_rx/idsweep_%03u.bin", g_idsw.batches);
    FILE *f = fopen(path, "wb");
    if (f != NULL) {
      const int32_t hdr[8] = {0x31575349 /* "ISW1" */, g_idsw.nsc, g_idsw.point_a_sc, g_idsw.css0_crb,
                              g_idsw.css0_nrb, g_idsw.pci, g_idsw.sps, g_idsw.n};
      fwrite(hdr, sizeof(hdr), 1, f);
      fwrite(g_idsw.slot, sizeof(int), g_idsw.n, f);
      fwrite(g_idsw.sym, sizeof(int), g_idsw.n, f);
      fwrite(g_idsw.re, sizeof(c16_t), (size_t)g_idsw.n * g_idsw.nsc, f);
      fclose(f);
    }
    printf("IDSWEEP dumped batch %u (%d snapshots) -> %s%s\n", g_idsw.batches, g_idsw.n, path, f ? "" : " FAILED");
    fflush(stdout);
    pthread_mutex_lock(&g_idsw.mu);
    g_idsw.batches++;
    g_idsw.n = 0;
    const bool last = (int)g_idsw.batches >= g_idsw.max_batches;
    g_idsw.full = last; /* stays full after the last batch, which is what stops capture in want() */
    pthread_mutex_unlock(&g_idsw.mu);
    if (last) /* and the worker must stop too: "full" is also its own wake condition, so staying in the
               * loop re-dumped empty batches forever (run s01f: 1M files, /tmp inodes exhausted) */
      break;
  }
  return NULL;
}

int nr_pdcch_coreset_map_idsweep_want(uint32_t abs_slot, int sps)
{
  if (g_idsw.enabled < 0) {
    const char *e = getenv("ISAC_COREMAP_IDSWEEP");
    g_idsw.max_batches = (e != NULL) ? atoi(e) : 0;  /* ISAC_COREMAP_IDSWEEP=<batches to dump> */
    g_idsw.enabled = g_idsw.max_batches > 0 ? 1 : 0;
  }
  if (g_idsw.enabled != 1 || g_idsw.full || sps <= 0)
    return -1;
  const nr_passive_acq_snapshot_t snap = nr_passive_acq_snapshot();
  if (snap.carrier_verified == 0 || g_css0_first_rb < 0) /* Point A (SIB1) and CORESET#0 (MIB) first */
    return -1;
  if (!nr_passive_acq_tdd_slot_has_downlink(abs_slot))
    return -1;
  /* ONE symbol, rotating through the slot, on every 10th DL slot: a single-symbol FEP of RT cost, and
   * a batch spread over ~20 s rather than packed into the ~0.3 s right after SIB1, when the timing and
   * CFO loops are still settling (run s01e: every early symbol-0 snapshot read as noise).
   * ponytail: 10 is a sampling stride, not a cell parameter. */
  if ((g_idsw.dl_calls++ % 10) != 0)
    return -1;
  return (int)(g_idsw.cap++ % (uint32_t)sps);
}

void nr_pdcch_coreset_map_idsweep_push(const c16_t *rxF_sym, int fft, int fco, int n_rb, int slot, int sym, uint16_t pci, int sps)
{
  const nr_passive_acq_snapshot_t snap = nr_passive_acq_snapshot();
  const int nsc = 12 * n_rb;
  pthread_mutex_lock(&g_idsw.mu);
  if (g_idsw.re == NULL) {
    g_idsw.nsc = nsc;
    g_idsw.re = malloc(sizeof(c16_t) * (size_t)IDSW_NSNAP * nsc);
    if (!g_idsw.re) {
      g_idsw.enabled = 0;
      pthread_mutex_unlock(&g_idsw.mu);
      return;
    }
    pthread_t th;
    pthread_create(&th, NULL, idsw_worker, NULL);
    pthread_detach(th);
    printf("IDSWEEP capture started: Point A at grid subcarrier %d (SIB1), CORESET#0 grid RB %d +%d (MIB), %d batches of %d snapshots\n",
           snap.carrier.point_a_subcarrier, g_css0_first_rb, g_css0_nrb, g_idsw.max_batches, IDSW_NSNAP);
    fflush(stdout);
  }
  if (g_idsw.full || nsc != g_idsw.nsc) {
    pthread_mutex_unlock(&g_idsw.mu);
    return;
  }
  const int pa = snap.carrier.point_a_subcarrier;
  const int pa_mod = ((pa % 12) + 12) % 12;
  g_idsw.point_a_sc = pa;
  /* CORESET#0's exact first subcarrier on the SAME CRB grid: the MIB gives its offset from the
   * SSB-overlapping CRB, whose grid index autoconf rounded down to whole grid RBs. */
  g_idsw.css0_crb = (12 * g_css0_first_rb + pa_mod - pa) / 12;
  g_idsw.css0_nrb = g_css0_nrb;
  g_idsw.pci = pci;
  g_idsw.sps = sps;
  const int k = g_idsw.n;
  c16_t *dst = g_idsw.re + (size_t)k * nsc;
  for (int i = 0; i < nsc; i++)
    dst[i] = rxF_sym[(fco + i) % fft];
  g_idsw.slot[k] = slot;
  g_idsw.sym[k] = sym;
  if (++g_idsw.n == IDSW_NSNAP) {
    g_idsw.full = 1;
    pthread_cond_signal(&g_idsw.cv);
  }
  pthread_mutex_unlock(&g_idsw.mu);
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
  int rb_phase = 0;
  {
    static int s_ph = -2;
    if (s_ph == -2) {
      const char *e = getenv("ISAC_COREMAP_PHASE");
      s_ph = (e != NULL) ? atoi(e) : -1;
    }
    rb_phase = (s_ph >= 0) ? (s_ph % 6) : (nr_pdcch_coreset_map_phase_hint() % 6);
    if (rb_phase < 0) rb_phase = 0;
  }
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
  /* DIAGNOSTIC (env-gated, kept permanently -- same convention as this project's other ISAC_*
   * debug flags) (2026-09-05, Task 5 live validation): zero candidates ever cleared the
   * significance bar on live air over 2800+ calls. Track raw max/rb0 correlation regardless of
   * threshold, rate-limited, to see how close (or far) live air gets vs the 0.836 bar and vs the
   * synthetic test's 0.8-0.95 assumption. */
  static int s_diag = -1;
  if (s_diag < 0)
    s_diag = (getenv("ISAC_DISCOVER_DIAG") != NULL) ? 1 : 0;
  static int s_calls = 0;
  s_calls++;
  double diag_max_corr = 0.0;
  int diag_max_rb = -1;
  double diag_rb0_corr = -1.0;
  double wcorr[NR_PDCCH_MAX_CANDIDATE_WINDOWS];
  for (int w = 0; w < n_windows && w < NR_PDCCH_MAX_CANDIDATE_WINDOWS; w++) {
    const int rb_offset = rb_phase + w * 6;
    double corr;
    if (coherent_mode()) {
      /* LEGACY: one coherent sum over all 18 pilots. Valid only on a flat channel -- see this
       * file's header note. Kept reachable via ISAC_COREMAP_COH=1. */
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
      corr = (denom > 0.0) ? sqrt(cr * cr + ci * ci) / denom : 0.0;
    } else {
      corr = nr_pdcch_subband_z(rxdataF, pilot, ofdm_symbol_size, first_carrier_offset, rb_offset, 6);
    }
    wcorr[w] = corr;
    if (s_diag) {
      if (rb_offset == 0) diag_rb0_corr = corr;
      if (corr > diag_max_corr) { diag_max_corr = corr; diag_max_rb = rb_offset; }
    }
  }
  /* GROUND-TRUTH PROFILE (ISAC_COREMAP_GT=1, default off). See this file's note above: the MEAN
   * correlation per window over many calls, which reveals a CORESET that a thresholded hit count
   * misses because it only transmits on a fraction of calls. Windows 0..8 hold CORESET#0 on this
   * deployment and are the known-good reference. */
  {
    static int s_gt = -1;
    if (s_gt < 0) {
      const char *e = getenv("ISAC_COREMAP_GT");
      s_gt = (e != NULL && atoi(e) != 0) ? 1 : 0;
    }
    if (s_gt) {
      static double s_sum[NR_PDCCH_MAX_CANDIDATE_WINDOWS];
      static double s_max[NR_PDCCH_MAX_CANDIDATE_WINDOWS];
      static unsigned long s_n = 0;
      const int m = n_windows < NR_PDCCH_MAX_CANDIDATE_WINDOWS ? n_windows : NR_PDCCH_MAX_CANDIDATE_WINDOWS;
      for (int w = 0; w < m; w++) {
        s_sum[w] += wcorr[w];
        if (wcorr[w] > s_max[w]) s_max[w] = wcorr[w];
      }
      s_n++;
      /* CONTROL: the known CORESET, correlated exactly as dci_nr.c does it. */
      {
        static int s_cs_start = -2, s_cs_nrb = 0;
        if (s_cs_start == -2) {
          const char *e = getenv("ISAC_COREMAP_GT_CSET");
          s_cs_start = -1;
          if (e != NULL && sscanf(e, "%d:%d", &s_cs_start, &s_cs_nrb) != 2)
            s_cs_start = -1;
        }
        if (s_cs_start >= 0 && s_cs_nrb >= 6 && s_cs_start + s_cs_nrb <= n_rb_carrier) {
          const int cs_sc = (first_carrier_offset + s_cs_start * 12) % ofdm_symbol_size;
          double best = 0.0, best_z = -1e9;
          for (int cce = 0; cce * 6 + 6 <= s_cs_nrb; cce++) {
            double cr = 0.0, ci = 0.0, py = 0.0, px = 0.0;
            for (int rb = cce * 6; rb < cce * 6 + 6; rb++) {
              for (int q = 0; q < 3; q++) {
                const int k = (cs_sc + rb * 12 + 1 + 4 * q) % ofdm_symbol_size;
                const c16_t y = rxdataF[k];
                const c16_t x = pilot[(s_cs_start + rb) * 3 + q];
                cr += (double)y.r * x.r - (double)y.i * x.i;
                ci += (double)y.r * x.i + (double)y.i * x.r;
                py += (double)y.r * y.r + (double)y.i * y.i;
                px += (double)x.r * x.r + (double)x.i * x.i;
              }
            }
            const double den = sqrt(py * px);
            const double a = (den > 0.0) ? sqrt(cr * cr + ci * ci) / den : 0.0;
            if (a > best) best = a;
            {
              /* The SAME sub-band statistic, on the same known CORESET: this is the number that
               * decides whether the fix works. Noise -> ~0; real DM-RS -> large positive Z. */
              const double z = nr_pdcch_subband_z(rxdataF, pilot, ofdm_symbol_size,
                                                  first_carrier_offset, s_cs_start + cce * 6, 6);
              if (z > best_z) best_z = z;
            }
          }
          /* SEQUENCE SWEEP. One (slot_delta, symbol) hypothesis per call, round-robin, scored on
           * the same known CORESET. NSLOT_MOD = 20 assumes mu=1; see this block's header note. */
          {
#define NR_PDCCH_GT_NSLOT 20
#define NR_PDCCH_GT_NSYM  2
            static double s_hz[NR_PDCCH_GT_NSLOT][NR_PDCCH_GT_NSYM];
            static unsigned long s_hn[NR_PDCCH_GT_NSLOT][NR_PDCCH_GT_NSYM];
            static unsigned long s_hyp = 0;
            const unsigned long h = s_hyp++;
            const int d_slot = (int)((h / NR_PDCCH_GT_NSYM) % NR_PDCCH_GT_NSLOT);
            const int t_sym = (int)(h % NR_PDCCH_GT_NSYM);
            const int t_slot = (slot + d_slot) % NR_PDCCH_GT_NSLOT;
            uint32_t *g2 = nr_gold_pdcch(n_rb_carrier, 14, scrambling_id, t_slot, t_sym);
            c16_t pil2[n_rb_carrier * 3];
            nr_pdcch_dmrs_ref(g2, pil2, (unsigned short)n_rb_carrier);
            double bz = -1e9;
            for (int cce = 0; cce * 6 + 6 <= s_cs_nrb; cce++) {
              const double z = nr_pdcch_subband_z(rxdataF, pil2, ofdm_symbol_size,
                                                  first_carrier_offset, s_cs_start + cce * 6, 6);
              if (z > bz) bz = z;
            }
            s_hz[d_slot][t_sym] += bz;
            s_hn[d_slot][t_sym]++;
            if ((s_n % 20000) == 0) {
              char b[900];
              int u = 0;
              for (int ds = 0; ds < NR_PDCCH_GT_NSLOT && u < (int)sizeof(b) - 20; ds++)
                for (int sy = 0; sy < NR_PDCCH_GT_NSYM && u < (int)sizeof(b) - 20; sy++)
                  u += snprintf(b + u, sizeof(b) - u, "%d/%d:%.2f ", ds, sy,
                                s_hn[ds][sy] ? s_hz[ds][sy] / (double)s_hn[ds][sy] : 0.0);
              printf("COREMAPSEQ n=%lu slot_now=%d meanZ_by[dslot/sym]: %s\n", s_n, slot, b);
              fflush(stdout);
            }
          }
          static double s_ctl_sum = 0.0, s_ctl_max = 0.0, s_ctlz_sum = 0.0, s_ctlz_max = -1e9;
          s_ctl_sum += best;
          if (best > s_ctl_max) s_ctl_max = best;
          s_ctlz_sum += best_z;
          if (best_z > s_ctlz_max) s_ctlz_max = best_z;
          if ((s_n % 5000) == 0) {
            printf("COREMAPCTL n=%lu cset=%d+%d coherent[mean=%.3f max=%.3f] subband_z[mean=%.2f max=%.2f]\n",
                   s_n, s_cs_start, s_cs_nrb, s_ctl_sum / (double)s_n, s_ctl_max,
                   s_ctlz_sum / (double)s_n, s_ctlz_max);
            fflush(stdout);
          }
        }
      }
      if ((s_n % 5000) == 0) {
        char b[1400];
        int u = 0;
        for (int w = 0; w < m && u < (int)sizeof(b) - 12; w++)
          u += snprintf(b + u, sizeof(b) - u, "%.3f ", s_sum[w] / (double)s_n);
        printf("COREMAPGT n=%lu symbol=%d mean_corr: %s\n", s_n, symbol, b);
        u = 0;
        for (int w = 0; w < m && u < (int)sizeof(b) - 12; w++)
          u += snprintf(b + u, sizeof(b) - u, "%.2f ", s_max[w]);
        printf("COREMAPGT n=%lu symbol=%d max_corr:  %s\n", s_n, symbol, b);
        fflush(stdout);
      }
    }
  }
  /* ADAPTIVE THRESHOLD (2026-09-16). The fixed 0.836 was never reached on the X410 (peaks 0.43-0.52 in
   * every run, COREMAPDIAG), so no CORESET was ever discovered by correlation. A window carrying a
   * PDCCH is an OUTLIER against this symbol's own population of 6-RB windows: threshold = median +
   * 4 x MAD over the windows (MAD scaled to sigma), floored at the old constant only when the
   * population is so clean that the fixed bar is the lower one. Empty symbols have no outliers and
   * contribute nothing, as before. */
  double thr = CORESET_MAP_CORR_THRESHOLD;
  if (!coherent_mode()) {
    /* Z is scale-free, so ONE absolute bar is meaningful here -- unlike the coherent statistic,
     * whose floor had to be retuned per deployment. Default 5 sigma: with 45 windows scanned every
     * DL slot the per-call false-positive count is 45*2.9e-7 ~= 1.3e-5, i.e. one false lit window
     * per ~77000 calls, against a dwell of ~1000. The adaptive median+4*MAD guard is kept ON TOP
     * so a correlated background (not white noise) still cannot flood the histogram. */
    static double s_zbar = -1.0;
    if (s_zbar < 0.0) {
      const char *e = getenv("ISAC_COREMAP_Z");
      const double v = (e != NULL) ? atof(e) : 0.0;
      /* 3.0, NOT 5.0: a_r <= 1 caps Z at (1-mu0)/(sigma0/sqrt(6)) = 4.47 for a 6-RB window, so a
       * 5-sigma bar was unreachable by construction and this mode could never have detected
       * anything. Measured mistake, corrected 2026-09-21. */
      s_zbar = (v > 0.0) ? v : 3.0;
    }
    thr = s_zbar;
    if (n_windows >= 8) {
      double v[NR_PDCCH_MAX_CANDIDATE_WINDOWS];
      const int m = n_windows < NR_PDCCH_MAX_CANDIDATE_WINDOWS ? n_windows : NR_PDCCH_MAX_CANDIDATE_WINDOWS;
      for (int i = 0; i < m; i++) v[i] = wcorr[i];
      for (int i = 1; i < m; i++) { double x = v[i]; int j = i - 1; while (j >= 0 && v[j] > x) { v[j + 1] = v[j]; j--; } v[j + 1] = x; }
      const double med = v[m / 2];
      for (int i = 0; i < m; i++) v[i] = fabs(wcorr[i] - med);
      for (int i = 1; i < m; i++) { double x = v[i]; int j = i - 1; while (j >= 0 && v[j] > x) { v[j + 1] = v[j]; j--; } v[j + 1] = x; }
      const double sigma = 1.4826 * v[m / 2];
      const double adaptive = med + 4.0 * (sigma > 0.05 ? sigma : 0.05);
      if (adaptive > thr) thr = adaptive;
    }
  } else if (n_windows >= 8) {
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
  /* FIXED EMISSION BAR -- see this file's header note. The adaptive rule above is inflated by the
   * signal itself (measured: thresh 0.57-0.79 against real peaks 0.41-0.55, found=0 every call),
   * so it is replaced by a measured constant whenever ISAC_COREMAP_EMITBAR is non-zero. The
   * histogram plus the background-significance lit rule downstream do the discrimination that a
   * per-call bar provably cannot: noise max-over-45 ~0.50 vs occupied peak ~0.497. */
  {
    static double s_emitbar = -1.0;
    if (s_emitbar < 0.0) {
      const char *e = getenv("ISAC_COREMAP_EMITBAR");
      /* DEFAULT 0 = keep the adaptive rule. MEASURED 2026-09-21: a fixed 0.40 per-WINDOW bar is
       * below the per-window noise tail (mean 0.209) and re-flattened the histogram -- 277 hits
       * per 1000 calls spread over all 45 windows, destroying the 27-sigma peak that the adaptive
       * rule plus the occupancy gate had produced (window 39: 64 hits vs 4.75 background). The
       * 0.40 was justified against the noise MAX-over-45 (~0.50), but the bar applies per window,
       * which is a different distribution. Opt in with ISAC_COREMAP_EMITBAR if ever recalibrated. */
      s_emitbar = (e != NULL) ? atof(e) : 0.0;
      if (s_emitbar < 0.0) s_emitbar = 0.0;
    }
    if (s_emitbar > 0.0 && coherent_mode()) {
      thr = s_emitbar;
    }
  }
  /* OCCUPANCY GATE -- see this file's header note. Without it the histogram integrates the
   * false-positive rate of ~95 % empty slots until every window looks lit. */
  bool occupied = true;
  {
    static double s_occbar = -1.0;
    if (s_occbar < 0.0) {
      const char *e = getenv("ISAC_COREMAP_OCCBAR");
      s_occbar = (e != NULL) ? atof(e) : 0.55;
      if (s_occbar < 0.0) s_occbar = 0.0;
    }
    if (s_occbar > 0.0 && !coherent_mode()) {
      /* The bar is calibrated in COHERENT [0,1] units; in Z units it is meaningless, so the gate
       * only applies to the statistic it was measured against. */
      occupied = true;
    } else if (s_occbar > 0.0) {
      double peak = 0.0;
      for (int w = 0; w < n_windows && w < NR_PDCCH_MAX_CANDIDATE_WINDOWS; w++)
        if (wcorr[w] > peak) peak = wcorr[w];
      occupied = (peak >= s_occbar);
      static unsigned long s_seen, s_occ;
      s_seen++;
      if (occupied) s_occ++;
      if ((s_seen % 20000) == 0) {
        printf("COREMAPOCC calls=%lu occupied=%lu (%.2f %%) bar=%.2f\n",
               s_seen, s_occ, 100.0 * (double)s_occ / (double)s_seen, s_occbar);
        fflush(stdout);
      }
    }
  }
  for (int w = 0; occupied && w < n_windows; w++) {
    if (wcorr[w] >= thr && found < max_candidates) {
      candidates_out[found].rb_offset = rb_phase + w * 6;
      candidates_out[found].corr      = wcorr[w];
      found++;
    }
  }
  if (s_diag && (s_calls % 200) == 1) {
    printf("COREMAPDIAG calls=%d rb0_corr=%.4f max_corr=%.4f max_rb=%d thresh=%.3f found=%d\n", s_calls,
          diag_rb0_corr, diag_max_corr, diag_max_rb, thr, found);
    fflush(stdout);
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
