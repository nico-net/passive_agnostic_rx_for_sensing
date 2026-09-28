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
 */

/*! \file nr_csirs_blind_synth_check.c
 * \brief END-TO-END self-check of the blind CSI-RS chain against a resource WE generate.
 *
 * WHY THIS EXISTS. On air the chain reports: strong sequence-free energy at a candidate's
 * positions, but no correlation match at ANY scramblingID (complete 1024 sweep) and ANY slot
 * offset (complete 20 sweep). Three explanations survive that evidence: the energy is not CSI-RS
 * at all; the enumeration never contains the true resource; or the scoring cannot recognise a
 * match it is handed. The last two are testable WITHOUT a radio, and had never been tested --
 * every previous test exercised the pure helpers on synthetic vectors, never the real generator
 * against the real enumeration.
 *
 * WHY C AND NOT GTEST. nr_generate_csi_rs() and csi_mapping_parms_t live in nr_phy_common.h,
 * which also declares nr_mmse_2layers() with C variably-modified array parameters
 * (c16_t ch_magb[nb_layers][pdsch_buf_size_max]). C++ rejects those outright, so a .cc test
 * cannot include the header at all -- and copying the struct into the test would let the two
 * definitions drift silently, which is worse than no test. Plain C with assert() includes the
 * real header and cannot drift.
 */

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/config/config_load_configmodule.h"
#include "common/utils/LOG/log.h"
#include "PHY/defs_nr_common.h"
#include "PHY/nr_phy_common/inc/nr_phy_common.h"
#include "openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_search.h"

/* OAI's LOG and CONFIG libraries reference these two from the softmodem's main(). Every test
 * binary that links them has to supply them; the ones that do not are the reason
 * test_nr_pdcch_dci_length_sweep has never linked. */
configmodule_interface_t *uniqCfg = NULL;
void exit_function(const char *file, const char *function, const int line, const char *s, const int a)
{
  (void)file;
  (void)function;
  (void)line;
  (void)a;
  fprintf(stderr, "exit_function: %s\n", s ? s : "");
  abort();
}

#define NRB 273 /* the cell under test: 100 MHz at 30 kHz */
#define FFT 4096
#define SYMBOLS 14
#define SUB_RES 32 /* sub-band size the RT path scores with */

static NR_DL_FRAME_PARMS make_fp(void)
{
  NR_DL_FRAME_PARMS fp;
  memset(&fp, 0, sizeof(fp));
  fp.N_RB_DL = NRB;
  fp.ofdm_symbol_size = FFT;
  fp.first_carrier_offset = FFT - (NRB * 12) / 2;
  fp.symbols_per_slot = SYMBOLS;
  fp.slots_per_frame = 20;
  fp.numerology_index = 1;
  fp.nb_antennas_rx = 1;
  return fp;
}

/* Deterministic uniform noise in [-a, a]; no rand() so the check cannot drift with libc. */
static uint32_t s_rng = 12345u;
static double noise(double a)
{
  s_rng = s_rng * 1103515245u + 12345u;
  return a * (2.0 * ((double)((s_rng >> 8) & 0xFFFFu) / 65535.0) - 1.0);
}

/* ONE BUFFER PER PORT, exactly as nr_csirs_blind_rt.c does: nr_generate_csi_rs() writes dataF[p]
 * for every port the row defines (up to 4), so a single plane segfaults on any multi-port row.
 * @p buf must hold PLANES * FFT * SYMBOLS entries; only plane 0 is ever scored, which is what a
 * single-antenna receiver sees. */
#define PLANES 4
static void generate(const NR_DL_FRAME_PARMS *fp, const nr_csirs_candidate_t *c, int slot, c16_t *buf)
{
  memset(buf, 0, (size_t)PLANES * FFT * SYMBOLS * sizeof(c16_t));
  c16_t *planes[PLANES];
  for (int p = 0; p < PLANES; p++)
    planes[p] = buf + (size_t)p * FFT * SYMBOLS;
  const csi_mapping_parms_t p = get_csi_mapping_parms(c->row, c->freq_domain, c->symb_l0, c->symb_l1);
  if (getenv("SYNTH_DEBUG"))
    printf("  parms row=%u fd=%u l0=%u l1=%u -> size=%d ports=%d kprime=%d lprime=%d k0=%d l0m=%d\n",
           c->row, c->freq_domain, c->symb_l0, c->symb_l1, p.size, p.ports, p.kprime, p.lprime,
           p.koverline[0], p.loverline[0]);
  nr_generate_csi_rs(fp, &p, 4096 /* amp */, slot, c->freq_density, c->start_rb, c->nr_of_rbs,
                     c->symb_l0, c->symb_l1, c->row, c->scramb_id, 0, c->cdm_type, planes);
}

/* ---- ZERO-POWER EXPORT ON AN 8-PORT CELL SEEN BY ONE ANTENNA ------------------------------------
 * Lab G4 (OAI phy-test, 2026-09-28): NZP row 6 (8 ports, k=0..7), symbol 13, period 160, identity
 * channel, 1-RX receiver. Only TX antenna 0 -- CSI-RS port 0, CDM group 0, k=0,1 -- reaches the
 * receiver; ports 1..7 leave their REs empty at this antenna. Every run exported ONE ZP-CSI-RS in
 * symbol 13 on REs that are dark only for that reason. What follows replays such CSI-RS occasions
 * through the production ZP decision (enumerate -> nr_generate_csi_rs() pattern -> zero score ->
 * zp_feed() with the same rolling-median null nr_csirs_blind_rt.c uses) and demands no export, then
 * runs a genuine ZP hole under data through the same chain and demands that it IS exported. */
#define LAB_NRB 106
#define LAB_FFT 2048
#define LAB_SYM 13
#define LAB_PERIOD 160
#define LAB_OCC 12
#define ZP_NULLWIN 64

static NR_DL_FRAME_PARMS make_lab_fp(void)
{
  NR_DL_FRAME_PARMS fp = make_fp();
  fp.N_RB_DL = LAB_NRB;
  fp.ofdm_symbol_size = LAB_FFT;
  fp.first_carrier_offset = LAB_FFT - (LAB_NRB * 12) / 2;
  return fp;
}

/* All ports of @p c into planes[0..n_planes-1] (each LAB_FFT * SYMBOLS long, zeroed here). */
static void generate_ports(const NR_DL_FRAME_PARMS *fp, const nr_csirs_candidate_t *c, int slot,
                           c16_t **planes, int n_planes)
{
  for (int p = 0; p < n_planes; p++)
    memset(planes[p], 0, (size_t)LAB_FFT * SYMBOLS * sizeof(c16_t));
  const csi_mapping_parms_t mp = get_csi_mapping_parms(c->row, c->freq_domain, c->symb_l0, c->symb_l1);
  nr_generate_csi_rs(fp, &mp, 4096, slot, c->freq_density, c->start_rb, c->nr_of_rbs, c->symb_l0,
                     c->symb_l1, c->row, c->scramb_id, 0, c->cdm_type, planes);
}

static double zp_median(const double *v, int n)
{
  if (n < 8)
    return -1.0; /* as nr_csirs_blind_rt.c: no population yet */
  double t[ZP_NULLWIN];
  memcpy(t, v, sizeof(t[0]) * (size_t)n);
  for (int i = 1; i < n; i++) {
    const double x = t[i];
    int j = i - 1;
    while (j >= 0 && t[j] > x) {
      t[j + 1] = t[j];
      j--;
    }
    t[j + 1] = x;
  }
  return t[n / 2];
}

/* A unit-power-ish QPSK data RE (PDSCH stand-in). */
static c16_t data_re(void)
{
  s_rng = s_rng * 1103515245u + 12345u;
  const int16_t a = 700;
  const c16_t d = {(s_rng & 0x100) ? a : -a, (s_rng & 0x200) ? a : -a};
  return d;
}

/* Replays LAB_OCC CSI-RS occasions (slot 160*t: symbol 13 and its neighbour, symbol 12, from
 * @p csi_sym(t)) interleaved with ordinary slots (data on every RE of both symbols), scoring every
 * symbol-13 candidate of the production enumeration on each with the slot-level score, as the RT tap
 * would when its round-robin lands there. Returns the confirmed ZP index or -1; *best_out gets the
 * highest CSI-slot score. */
typedef void (*csi_sym_fn)(const NR_DL_FRAME_PARMS *fp, int t, c16_t *sym, c16_t *nb);
static int run_zp_chain(const NR_DL_FRAME_PARMS *fp, csi_sym_fn csi_sym, nr_csirs_blind_state_t *zp,
                        double *best_out)
{
  assert(nr_csirs_blind_init(zp, LAB_NRB, 0) > 0);
  c16_t *pl[PLANES];
  for (int p = 0; p < PLANES; p++)
    pl[p] = malloc((size_t)LAB_FFT * SYMBOLS * sizeof(c16_t));
  int idx13[NR_CSIRS_BLIND_MAX_CAND];
  c16_t *ref13[NR_CSIRS_BLIND_MAX_CAND];
  int n13 = 0;
  for (int i = 0; i < zp->n; i++) {
    if (zp->cand[i].symb_l0 != LAB_SYM || !nr_csirs_blind_candidate_safe(&zp->cand[i]))
      continue;
    /* The ZP pattern is where the reference is non-zero; which slot's sequence fills it is irrelevant. */
    generate_ports(fp, &zp->cand[i], 0, pl, PLANES);
    ref13[n13] = malloc((size_t)LAB_FFT * sizeof(c16_t));
    memcpy(ref13[n13], pl[0] + (size_t)LAB_SYM * LAB_FFT, (size_t)LAB_FFT * sizeof(c16_t));
    idx13[n13++] = i;
  }
  assert(n13 > 0);
  double null_v[ZP_NULLWIN];
  int null_n = 0, null_w = 0;
  double best = -1.0;
  c16_t *csi = malloc((size_t)LAB_FFT * sizeof(c16_t));
  c16_t *dat = malloc((size_t)LAB_FFT * sizeof(c16_t));
  c16_t *csi_nb = malloc((size_t)LAB_FFT * sizeof(c16_t));
  c16_t *dat_nb = malloc((size_t)LAB_FFT * sizeof(c16_t));
  for (int t = 0; t < LAB_OCC && zp->confirmed < 0; t++) {
    memset(csi, 0, (size_t)LAB_FFT * sizeof(c16_t));
    memset(csi_nb, 0, (size_t)LAB_FFT * sizeof(c16_t));
    csi_sym(fp, t, csi, csi_nb);
    for (int k = 0; k < LAB_NRB * 12; k++) {
      dat[k] = data_re();
      dat_nb[k] = data_re();
      c16_t *const b[4] = {&csi[k], &dat[k], &csi_nb[k], &dat_nb[k]};
      for (int q = 0; q < 4; q++) {
        b[q]->r = (int16_t)(b[q]->r + noise(20.0));
        b[q]->i = (int16_t)(b[q]->i + noise(20.0));
      }
    }
    /* The RT tap scores one candidate per slot, so each candidate meets ~period ordinary slots per
     * CSI-RS occasion and the rolling null is made of those. Three ordinary visits per CSI-RS visit
     * keep that proportion (null ~ ordinary slots) at a fraction of the cost. */
    for (int m = 0; m < n13 && zp->confirmed < 0; m++) {
      for (int v = 0; v < 4 && zp->confirmed < 0; v++) {
        const bool on_csi = (v == 3);
        const uint32_t slot = (uint32_t)(LAB_PERIOD * t + (on_csi ? 0 : 1 + 40 * v));
        const int16_t *nb[1] = {(const int16_t *)(on_csi ? csi_nb : dat_nb)};
        const double zs = nr_csirs_blind_zero_score_slot((const int16_t *)(on_csi ? csi : dat), nb, 1,
                                                         (const int16_t *)ref13[m], LAB_FFT);
        if (zs < 0.0)
          continue;
        if (on_csi && zs > best)
          best = zs;
        nr_csirs_blind_zp_feed(zp, idx13[m], slot, zs, zp_median(null_v, null_n));
        null_v[null_w] = zs;
        null_w = (null_w + 1) % ZP_NULLWIN;
        if (null_n < ZP_NULLWIN)
          null_n++;
      }
    }
  }
  free(csi);
  free(dat);
  free(csi_nb);
  free(dat_nb);
  for (int m = 0; m < n13; m++)
    free(ref13[m]);
  for (int p = 0; p < PLANES; p++)
    free(pl[p]);
  *best_out = best;
  return zp->confirmed;
}

/* The lab's CSI-RS slot: NZP row 6 (bitmap 15 -> k=0..7, fd-CDM2) at symbol 13, 8 ports generated,
 * but only plane 0 (TX antenna 0 = port 0) reaches the single receive antenna. Nothing else is sent
 * in that symbol. */
static nr_csirs_candidate_t lab_row6(void)
{
  nr_csirs_candidate_t c;
  memset(&c, 0, sizeof(c));
  c.row = 6;
  c.freq_domain = 15;
  c.symb_l0 = LAB_SYM;
  c.cdm_type = 1;
  c.freq_density = 2;
  c.scramb_id = 0;
  c.start_rb = 0;
  c.nr_of_rbs = LAB_NRB;
  return c;
}
static void fill_data(c16_t *sym)
{
  for (int k = 0; k < LAB_NRB * 12; k++)
    sym[k] = data_re();
}
/* The lab: port 0 only at symbol 13, and no PDSCH in the slot (neighbour symbol left dark). */
static void lab_port0_only(const NR_DL_FRAME_PARMS *fp, int t, c16_t *sym, c16_t *nb)
{
  (void)nb;
  const nr_csirs_candidate_t c = lab_row6();
  c16_t *pl[8];
  for (int p = 0; p < 8; p++)
    pl[p] = malloc((size_t)LAB_FFT * SYMBOLS * sizeof(c16_t));
  generate_ports(fp, &c, (LAB_PERIOD * t) % fp->slots_per_frame, pl, 8);
  memcpy(sym, pl[0] + (size_t)LAB_SYM * LAB_FFT, (size_t)LAB_FFT * sizeof(c16_t));
  for (int p = 0; p < 8; p++)
    free(pl[p]);
}
/* Control: the same port-0 pilot, but the symbol is a scheduled PDSCH symbol with a genuine ZP hole
 * -- a row-4 pattern at bitmap 4 (k=8..11) -- carved out of the data: dark REs whose neighbours carry
 * data, which is what the ZP search exists to find. */
static void lab_hole_under_data(const NR_DL_FRAME_PARMS *fp, int t, c16_t *sym, c16_t *nb)
{
  lab_port0_only(fp, t, sym, nb);
  fill_data(nb);
  nr_csirs_candidate_t zp;
  memset(&zp, 0, sizeof(zp));
  zp.row = 4;
  zp.freq_domain = 4;
  zp.symb_l0 = LAB_SYM;
  zp.cdm_type = 1;
  zp.freq_density = 2;
  zp.nr_of_rbs = LAB_NRB;
  c16_t *pl[PLANES];
  for (int p = 0; p < PLANES; p++)
    pl[p] = malloc((size_t)LAB_FFT * SYMBOLS * sizeof(c16_t));
  generate_ports(fp, &zp, 0, pl, PLANES);
  for (int k = 0; k < LAB_NRB * 12; k++) {
    bool hole = false;
    for (int p = 0; p < PLANES; p++) {
      const c16_t v = pl[p][(size_t)LAB_SYM * LAB_FFT + k];
      hole |= v.r != 0 || v.i != 0;
    }
    if (!hole && sym[k].r == 0 && sym[k].i == 0)
      sym[k] = data_re();
  }
  for (int p = 0; p < PLANES; p++)
    free(pl[p]);
}

/* Wide genuine holes (commercial-cell shapes): an 8-RE/RB ZP at k=4..11 with data on k=0..3, and two
 * 4-RE holes (ZP + CSI-IM) at k=0..3 and k=8..11 with data on k=4..7. */
static void wide_hole(int t, c16_t *sym)
{
  (void)t;
  for (int k = 0; k < LAB_NRB * 12; k++)
    if (k % 12 < 4)
      sym[k] = data_re();
}
static void lab_wide_hole(const NR_DL_FRAME_PARMS *fp, int t, c16_t *sym, c16_t *nb)
{
  (void)fp;
  wide_hole(t, sym);
  fill_data(nb);
}
static void lab_two_holes(const NR_DL_FRAME_PARMS *fp, int t, c16_t *sym, c16_t *nb)
{
  (void)fp;
  (void)t;
  for (int k = 0; k < LAB_NRB * 12; k++)
    if (k % 12 >= 4 && k % 12 < 8)
      sym[k] = data_re();
  fill_data(nb);
}

/* Wide NZP beside unused REs (G5 round 3): through a real channel one antenna sees EVERY port, so an
 * 8-port row-6 resource lights 8 classes (k=0..7) and k=8..11 are simply unused. Sum of all 8 ports'
 * planes (unit gains), plus, when @p load_every > 0, PDSCH on the RBs rb % load_every == 0 of both
 * symbols (k=8..11 of the CSI-RS symbol carry data there). */
static void nzp_all_ports(const NR_DL_FRAME_PARMS *fp, const nr_csirs_candidate_t *c, int t, c16_t *sym)
{
  const int np = nr_csirs_blind_row_ports(c->row);
  c16_t *pl[8];
  for (int p = 0; p < 8; p++)
    pl[p] = malloc((size_t)LAB_FFT * SYMBOLS * sizeof(c16_t));
  generate_ports(fp, c, (LAB_PERIOD * t) % fp->slots_per_frame, pl, np > PLANES ? 8 : PLANES);
  for (int p = 0; p < np; p++)
    for (int k = 0; k < LAB_NRB * 12; k++) {
      sym[k].r = (int16_t)(sym[k].r + pl[p][(size_t)c->symb_l0 * LAB_FFT + k].r);
      sym[k].i = (int16_t)(sym[k].i + pl[p][(size_t)c->symb_l0 * LAB_FFT + k].i);
    }
  for (int p = 0; p < 8; p++)
    free(pl[p]);
}
static void nzp8_loaded(const NR_DL_FRAME_PARMS *fp, int t, c16_t *sym, c16_t *nb, int load_every)
{
  const nr_csirs_candidate_t c = lab_row6();
  nzp_all_ports(fp, &c, t, sym);
  for (int k = 0; load_every > 0 && k < LAB_NRB * 12; k++)
    if ((k / 12) % load_every == 0) {
      nb[k] = data_re();
      if (sym[k].r == 0 && sym[k].i == 0)
        sym[k] = data_re();
    }
}
static void nzp8_no_data(const NR_DL_FRAME_PARMS *fp, int t, c16_t *sym, c16_t *nb)
{
  nzp8_loaded(fp, t, sym, nb, 0);
}
static void nzp8_light_load(const NR_DL_FRAME_PARMS *fp, int t, c16_t *sym, c16_t *nb)
{
  nzp8_loaded(fp, t, sym, nb, 10);
}
/* Two 2-port NZP resources in one symbol (row 3 at k=0,1 and k=4,5), no data. */
static void two_nzp_no_data(const NR_DL_FRAME_PARMS *fp, int t, c16_t *sym, c16_t *nb)
{
  (void)nb;
  nr_csirs_candidate_t c = lab_row6();
  c.row = 3;
  c.freq_domain = 1;
  nzp_all_ports(fp, &c, t, sym);
  c.freq_domain = 4;
  nzp_all_ports(fp, &c, t, sym);
}

/* REs (subcarrier-in-RB bitmask) plane 0 of @p c occupies at symbol 13 of RB 0 and RB 1. */
static uint32_t plane0_mask(const NR_DL_FRAME_PARMS *fp, const nr_csirs_candidate_t *c)
{
  c16_t *pl[8];
  for (int p = 0; p < 8; p++)
    pl[p] = malloc((size_t)LAB_FFT * SYMBOLS * sizeof(c16_t));
  generate_ports(fp, c, 0, pl, nr_csirs_blind_row_ports(c->row) > PLANES ? 8 : PLANES);
  uint32_t m = 0;
  for (int k = 0; k < 24; k++) {
    const c16_t v = pl[0][(size_t)c->symb_l0 * LAB_FFT + k];
    if (v.r != 0 || v.i != 0)
      m |= 1u << k;
  }
  for (int p = 0; p < 8; p++)
    free(pl[p]);
  return m;
}

static void check_zp_dark_ports(void)
{
  const NR_DL_FRAME_PARMS fp = make_lab_fp();

  /* The premise: port 0 of the row-6 resource lands on k=0,1 only (both RB parities). */
  const nr_csirs_candidate_t row6 = lab_row6();
  const uint32_t m6 = plane0_mask(&fp, &row6);
  printf("ZPDARK: row-6 port-0 subcarrier mask (RB0|RB1) = 0x%06x\n", m6);
  assert(m6 == 0x003003u && "row-6 bitmap 15: port 0 must sit on k=0,1 of every RB");

  /* The NZP part is handled as today: the port-0 pilot still correlates as a row-3 k=0,1 resource. */
  nr_csirs_candidate_t *cand = calloc(NR_CSIRS_BLIND_MAX_CAND, sizeof(*cand));
  const int n = nr_csirs_blind_enumerate(cand, NR_CSIRS_BLIND_MAX_CAND, LAB_NRB, 0);
  int r3 = -1;
  for (int i = 0; i < n; i++)
    if (cand[i].row == 3 && cand[i].freq_domain == 1 && cand[i].symb_l0 == LAB_SYM && cand[i].freq_density == 2)
      r3 = i;
  assert(r3 >= 0);
  c16_t *sym = calloc(LAB_FFT, sizeof(c16_t));
  lab_port0_only(&fp, 0, sym, NULL);
  c16_t *pl[PLANES];
  for (int p = 0; p < PLANES; p++)
    pl[p] = malloc((size_t)LAB_FFT * SYMBOLS * sizeof(c16_t));
  generate_ports(&fp, &cand[r3], 0, pl, PLANES);
  int used = 0;
  const int16_t *pilot = (const int16_t *)(pl[0] + (size_t)LAB_SYM * LAB_FFT);
  const double z = nr_csirs_blind_correlate_blocks((const int16_t *)sym, pilot, LAB_FFT, SUB_RES, &used);
  printf("ZPDARK: NZP row3 fd1 l13 on the port-0 pilot: z=%.2f (n_used=%d, bar 4)\n", z, used);
  assert(z > 4.0 && "the port-0 NZP pilot must still correlate as today");
  for (int p = 0; p < PLANES; p++)
    free(pl[p]);
  free(sym);
  free(cand);

  /* RED case: ports 1..7 dark at the receiver, nothing else in the symbol. No ZP may be exported. */
  nr_csirs_blind_state_t *zp = malloc(sizeof(*zp));
  double best = 0.0;
  int w = run_zp_chain(&fp, lab_port0_only, zp, &best);
  if (w >= 0) {
    char line[128];
    nr_csirs_blind_format(&zp->cand[w], zp->period, zp->offset, line, sizeof(line));
    printf("ZPDARK: dark-ports case EXPORTED ZP \"%s\" (best CSI-slot score %.3f)\n", line, best);
  } else {
    printf("ZPDARK: dark-ports case: no ZP export (best CSI-slot score %.3f)\n", best);
  }
  assert(w < 0 && "REs dark only because ports 1..7 never reach the receiver were exported as ZP-CSI-RS");

  /* Control: a genuine hole under data is still exported, on the hole, at the resource's period. */
  w = run_zp_chain(&fp, lab_hole_under_data, zp, &best);
  assert(w >= 0 && "a genuine ZP hole under data must still be exported");
  char line[128];
  nr_csirs_blind_format(&zp->cand[w], zp->period, zp->offset, line, sizeof(line));
  const uint32_t mw = plane0_mask(&fp, &zp->cand[w]);
  printf("ZPDARK: hole-under-data case exported ZP \"%s\" (plane-0 mask 0x%06x, best %.3f)\n", line, mw, best);
  assert(zp->cand[w].symb_l0 == LAB_SYM);
  assert(mw != 0 && (mw & ~0xF00F00u) == 0 && "the exported ZP must lie inside the k=8..11 hole");
  assert(zp->period == LAB_PERIOD && zp->offset == 0);

  /* Wide holes: dark REs are the majority of the RB, data sets the baseline all the same. */
  const struct {
    csi_sym_fn fn;
    uint32_t hole; /* subcarrier mask, RB0|RB1 */
    const char *what;
  } wide[] = {{lab_wide_hole, 0xFF0FF0u, "8-RE hole k4..11, data k0..3"},
              {lab_two_holes, 0xF0FF0Fu, "two 4-RE holes k0..3 + k8..11, data k4..7"}};
  for (size_t c = 0; c < sizeof(wide) / sizeof(wide[0]); c++) {
    w = run_zp_chain(&fp, wide[c].fn, zp, &best);
    if (w < 0) {
      printf("ZPDARK: %s: NOT exported (best CSI-slot score %.3f)\n", wide[c].what, best);
    } else {
      nr_csirs_blind_format(&zp->cand[w], zp->period, zp->offset, line, sizeof(line));
      printf("ZPDARK: %s: exported ZP \"%s\" (best %.3f)\n", wide[c].what, line, best);
    }
    assert(w >= 0 && "a genuine wide ZP hole under data must be exported");
    const uint32_t m = plane0_mask(&fp, &zp->cand[w]);
    assert(zp->cand[w].symb_l0 == LAB_SYM);
    assert(m != 0 && (m & ~wide[c].hole) == 0 && "the exported ZP must lie inside the hole");
    assert(zp->period == LAB_PERIOD && zp->offset == 0);
  }

  /* Wide NZP beside unused REs, no (or light) PDSCH: nothing dark there is a hole. */
  const struct {
    csi_sym_fn fn;
    const char *what;
  } neg[] = {{nzp8_no_data, "8-class NZP k0..7, k8..11 unused, no PDSCH"},
             {nzp8_light_load, "8-class NZP k0..7, PDSCH on 10% of RBs"},
             {two_nzp_no_data, "two 2-class NZP k0,1 + k4,5, no PDSCH"}};
  for (size_t c = 0; c < sizeof(neg) / sizeof(neg[0]); c++) {
    w = run_zp_chain(&fp, neg[c].fn, zp, &best);
    if (w >= 0) {
      nr_csirs_blind_format(&zp->cand[w], zp->period, zp->offset, line, sizeof(line));
      printf("ZPDARK: %s: EXPORTED ZP \"%s\" (best CSI-slot score %.3f)\n", neg[c].what, line, best);
    } else {
      printf("ZPDARK: %s: no ZP export (best CSI-slot score %.3f)\n", neg[c].what, best);
    }
    assert(w < 0 && "dark REs beside a wide NZP in a slot without PDSCH on them were exported as ZP-CSI-RS");
  }
  free(zp);
  printf("ZPDARK: PASS\n");
}

int main(void)
{
  /* nr_gold_csi_rs() -> gold_cache() logs on every new entry, and the LOG macros dereference
   * global state that only logInit() sets up -- without this the generator segfaults before it
   * produces a single RE. */
  setvbuf(stdout, NULL, _IONBF, 0); /* assert() aborts; buffered diagnostics would be lost */
  char arg0[] = "nr_csirs_blind_synth_check";
  char *cfg_argv[] = {arg0, NULL};
  uniqCfg = load_configmodule(1, cfg_argv, CONFIG_ENABLECMDLINEONLY);
  logInit();
  set_glog(OAILOG_ERR);
  /* The QAM constellations are GLOBAL TABLES filled at PHY init (nr_init_ue.c:188), not constants.
   * Without this every modulated symbol is (0,0), nr_generate_csi_rs() emits an all-zero grid, and
   * the correlator reports "no usable REs" -- which reads exactly like an enumeration miss. */
  nr_generate_modulation_table();

  const NR_DL_FRAME_PARMS fp = make_fp();
  nr_csirs_candidate_t *cand = calloc(NR_CSIRS_BLIND_MAX_CAND, sizeof(*cand));
  assert(cand != NULL);

  /* ---- 1. The enumeration must reach the whole row-1 frequency-domain space. Row 1 carries a
   * 4-bit bitmap (TS 38.211 Table 7.4.1.5.3-1); it was given 3, so the top position was
   * unreachable -- and row 1 is where the OTA energy test keeps pointing. ---- */
  const int n = nr_csirs_blind_enumerate(cand, NR_CSIRS_BLIND_MAX_CAND, NRB, 382);
  assert(n > 0 && n <= NR_CSIRS_BLIND_MAX_CAND);
  int row1 = 0, row2 = 0, fd_seen = 0;
  for (int i = 0; i < n; i++) {
    if (cand[i].row == 1) {
      row1++;
      if (cand[i].freq_domain < 16)
        fd_seen |= 1 << cand[i].freq_domain;
    } else if (cand[i].row == 2) {
      row2++;
    }
  }
  printf("ENUM: %d candidates (row1=%d row2=%d) row1 fd bitmap=0x%04x\n", n, row1, row2, fd_seen);
  assert(row1 > 0 && "row 1 absent from the enumeration");
  assert(row2 > 0 && "row 2 absent from the enumeration");
  /* A 4-bit row-1 bitmap has exactly one set bit, so the four reachable values are 1,2,4,8. */
  assert((fd_seen & (1 << 8)) && "row-1 freq_domain 8 unreachable: the bitmap is still 3 bits");

  /* ---- 2. Hand the scorer a resource we generated ourselves, through a channel that defeats a
   * flat whole-band correlation (a phase ramp of tens of radians end to end), and require the
   * sub-band scorer to rank it top and clear the confirmation bar. ---- */
  nr_csirs_candidate_t truth = cand[0];
  for (int i = 0; i < n; i++)
    if (cand[i].row == 1 && nr_csirs_blind_candidate_safe(&cand[i])) {
      truth = cand[i];
      break;
    }
  printf("TRUTH: row=%u fd=%u l0=%u density=%u cdm=%u id=%u rbs=%u\n", truth.row, truth.freq_domain,
         truth.symb_l0, truth.freq_density, truth.cdm_type, truth.scramb_id, truth.nr_of_rbs);

  const size_t plane_sz = (size_t)PLANES * FFT * SYMBOLS * sizeof(c16_t);
  c16_t *tx = malloc(plane_sz);
  c16_t *ref = malloc(plane_sz);
  c16_t *rx = malloc(plane_sz);
  assert(tx && ref && rx);

  const int slot = 7;
  generate(&fp, &truth, slot, tx);
  for (int pl = 0; getenv("SYNTH_DEBUG") && pl < PLANES; pl++) {
    for (int l = 0; l < SYMBOLS; l++) {
      int nz = 0;
      const c16_t *sym = tx + (size_t)pl * FFT * SYMBOLS + (size_t)l * FFT;
      for (int k = 0; k < FFT; k++)
        if (sym[k].r || sym[k].i) nz++;
      if (nz) printf("  TX plane%d symbol%d: %d non-zero REs\n", pl, l, nz);
    }
  }
  memcpy(rx, tx, plane_sz);
  const uint32_t off = (uint32_t)truth.symb_l0 * FFT;
  for (int k = 0; k < FFT; k++) {
    const double ph = 40.0 * ((double)k / (double)FFT); /* ~40 rad across the band */
    const double r = tx[off + k].r, i = tx[off + k].i;
    rx[off + k].r = (int16_t)(r * cos(ph) - i * sin(ph) + noise(60.0));
    rx[off + k].i = (int16_t)(r * sin(ph) + i * cos(ph) + noise(60.0));
  }

  double z_true = -1.0, z_best_other = -1.0, epr_true = 0.0;
  int used_true = 0;
  for (int i = 0; i < n; i++) {
    if (!nr_csirs_blind_candidate_safe(&cand[i]))
      continue;
    generate(&fp, &cand[i], slot, ref);
    const uint32_t o = (uint32_t)cand[i].symb_l0 * FFT;
    int used = 0;
    const double z = nr_csirs_blind_correlate_blocks((const int16_t *)&rx[o], (const int16_t *)&ref[o],
                                                     FFT, SUB_RES, &used);
    const int is_truth = cand[i].row == truth.row && cand[i].freq_domain == truth.freq_domain
                         && cand[i].symb_l0 == truth.symb_l0 && cand[i].freq_density == truth.freq_density
                         && cand[i].cdm_type == truth.cdm_type;
    if (is_truth) {
      if (getenv("SYNTH_DEBUG"))
        printf("  matched truth at idx %d: z=%.3f used=%d\n", i, z, used);
      z_true = z;
      used_true = used;
      epr_true = nr_csirs_blind_energy_ratio((const int16_t *)&rx[o], (const int16_t *)&ref[o], FFT);
    } else if (z > z_best_other) {
      z_best_other = z;
    }
  }
  printf("SCAN: z_true=%.2f (n_used=%d) best_other=%.2f epr_true=%.2f\n", z_true, used_true,
         z_best_other, epr_true);

  assert(z_true > 0.0 && "the enumeration does not contain the resource we transmitted");
  assert(z_true > z_best_other && "the true resource is not the top-scoring candidate");
  assert(z_true > 4.0 && "the true resource does not clear the confirmation bar");
  assert(epr_true > 2.0 && "the sequence-free energy test does not see its own transmission");

  free(tx);
  free(ref);
  free(rx);
  free(cand);

  /* ---- 3. No ZP export from REs that are dark only because their ports never reach the receiver. */
  check_zp_dark_ports();
  printf("nr_csirs_blind_synth_check: PASS\n");
  return 0;
}
