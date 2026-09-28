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
 * through the production ZP decision (enumerate -> nr_generate_csi_rs() port-union pattern ->
 * nr_csirs_blind_zero_score_ports_shift() -> zp_feed() probation/export with the same rolling-median
 * null nr_csirs_blind_rt.c uses) and demands no export, then runs genuine ZP holes under data through
 * the same chain and demands that the complete ones ARE exported. */
#define LAB_NRB 106
#define LAB_FFT 2048
#define LAB_SYM 13
#define LAB_PERIOD 160
#define LAB_OCC 12
#define ZP_NULLWIN 64
/* Ordinary visits per CSI-RS occasion, after it (observations must be in slot order). 32 and 80 are
 * the divisor-only phases of period 160 (32 | 160; 80 is a multiple of 4, 5, 8, 10, 16, 20, 40 and
 * 80), so the probation can resolve every harmonic of the true period, as it does on air where the
 * rotation visits every phase. */
static const uint32_t lab_visits[] = {32, 80, 121};

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

/* Production ZP occupancy of one candidate at LAB_SYM: the union of its first PLANES port planes,
 * as nr_csirs_blind_rt.c builds it (NR_CSIRS_BLIND_RT_MAX_PORTS planes). */
typedef struct {
  int idx;
  c16_t *plane[PLANES];
  int n_planes;
} zp_ref_t;

static double zp_score(const c16_t *sym, const zp_ref_t *r)
{
  const int16_t *refs[PLANES];
  for (int p = 0; p < r->n_planes; p++)
    refs[p] = (const int16_t *)r->plane[p];
  return nr_csirs_blind_zero_score_ports_shift((const int16_t *)sym, refs, r->n_planes, LAB_FFT, 0);
}

/* Replays LAB_OCC CSI-RS occasions (slot 160*t: symbol 13 from @p csi_sym(t)), each followed by the
 * ordinary slots lab_visits (data on every RE), scoring every symbol-13 candidate of the production
 * enumeration on each, as the RT tap would when its rotation lands there. Returns the confirmed ZP
 * index or -1; *best_out gets the highest CSI-slot score. The references are kept in *refs_out (caller
 * frees with free_zp_refs) so a later replay can keep feeding the same state. */
typedef void (*csi_sym_fn)(const NR_DL_FRAME_PARMS *fp, int t, c16_t *sym);
static int load_zp_refs(const NR_DL_FRAME_PARMS *fp, const nr_csirs_blind_state_t *zp, zp_ref_t *refs)
{
  c16_t *pl[PLANES];
  for (int p = 0; p < PLANES; p++)
    pl[p] = malloc((size_t)LAB_FFT * SYMBOLS * sizeof(c16_t));
  int n13 = 0;
  for (int i = 0; i < zp->n; i++) {
    if (zp->cand[i].symb_l0 != LAB_SYM || !nr_csirs_blind_candidate_safe(&zp->cand[i]))
      continue;
    const int ports = nr_csirs_blind_row_ports(zp->cand[i].row);
    zp_ref_t *r = &refs[n13++];
    r->idx = i;
    r->n_planes = ports < PLANES ? ports : PLANES;
    /* The ZP pattern is where the reference is non-zero; which slot's sequence fills it is irrelevant. */
    generate_ports(fp, &zp->cand[i], 0, pl, PLANES);
    for (int p = 0; p < r->n_planes; p++) {
      r->plane[p] = malloc((size_t)LAB_FFT * sizeof(c16_t));
      memcpy(r->plane[p], pl[p] + (size_t)LAB_SYM * LAB_FFT, (size_t)LAB_FFT * sizeof(c16_t));
    }
  }
  for (int p = 0; p < PLANES; p++)
    free(pl[p]);
  assert(n13 > 0);
  return n13;
}
static void free_zp_refs(zp_ref_t *refs, int n13)
{
  for (int m = 0; m < n13; m++)
    for (int p = 0; p < refs[m].n_planes; p++)
      free(refs[m].plane[p]);
}

typedef struct {
  double null_v[ZP_NULLWIN];
  int null_n, null_w;
} zp_null_t;

/* Occasions t0 .. t1-1; stops at the first export when @p stop_on_export. */
static void replay_zp(const NR_DL_FRAME_PARMS *fp, csi_sym_fn csi_sym, nr_csirs_blind_state_t *zp,
                      const zp_ref_t *refs, int n13, zp_null_t *nl, int t0, int t1, bool stop_on_export,
                      double *best)
{
  c16_t *csi = malloc((size_t)LAB_FFT * sizeof(c16_t));
  c16_t *dat = malloc((size_t)LAB_FFT * sizeof(c16_t));
  for (int t = t0; t < t1 && !(stop_on_export && zp->confirmed >= 0); t++) {
    memset(csi, 0, (size_t)LAB_FFT * sizeof(c16_t));
    csi_sym(fp, t, csi);
    for (int k = 0; k < LAB_NRB * 12; k++) {
      dat[k] = data_re();
      c16_t *const b[2] = {&csi[k], &dat[k]};
      for (int q = 0; q < 2; q++) {
        b[q]->r = (int16_t)(b[q]->r + noise(20.0));
        b[q]->i = (int16_t)(b[q]->i + noise(20.0));
      }
    }
    for (int m = 0; m < n13 && !(stop_on_export && zp->confirmed >= 0); m++) {
      const int nv = 1 + (int)(sizeof(lab_visits) / sizeof(lab_visits[0]));
      for (int v = 0; v < nv && !(stop_on_export && zp->confirmed >= 0); v++) {
        const bool on_csi = (v == 0);
        const uint32_t slot = (uint32_t)(LAB_PERIOD * t) + (on_csi ? 0 : lab_visits[v - 1]);
        const double zs = zp_score(on_csi ? csi : dat, &refs[m]);
        if (zs < 0.0)
          continue;
        if (on_csi && zs > *best)
          *best = zs;
        nr_csirs_blind_zp_feed(zp, refs[m].idx, slot, zs, zp_median(nl->null_v, nl->null_n));
        nl->null_v[nl->null_w] = zs;
        nl->null_w = (nl->null_w + 1) % ZP_NULLWIN;
        if (nl->null_n < ZP_NULLWIN)
          nl->null_n++;
      }
    }
  }
  free(csi);
  free(dat);
}

static int run_zp_chain(const NR_DL_FRAME_PARMS *fp, csi_sym_fn csi_sym, nr_csirs_blind_state_t *zp,
                        double *best_out)
{
  assert(nr_csirs_blind_init(zp, LAB_NRB, 0) > 0);
  zp_ref_t *refs = calloc(NR_CSIRS_BLIND_MAX_CAND, sizeof(*refs));
  const int n13 = load_zp_refs(fp, zp, refs);
  zp_null_t nl = {.null_n = 0, .null_w = 0};
  double best = -1.0;
  replay_zp(fp, csi_sym, zp, refs, n13, &nl, 0, LAB_OCC, true, &best);
  free_zp_refs(refs, n13);
  free(refs);
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
/* The lab: port 0 only at symbol 13, and no PDSCH in the slot. */
static void lab_port0_only(const NR_DL_FRAME_PARMS *fp, int t, c16_t *sym)
{
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
static void lab_hole_under_data(const NR_DL_FRAME_PARMS *fp, int t, c16_t *sym)
{
  lab_port0_only(fp, t, sym);
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

/* Wider holes (commercial-cell shapes): an 8-RE/RB ZP at k=4..11 with data on k=0..3, and two
 * 4-RE holes (ZP + CSI-IM) at k=0..3 and k=8..11 with data on k=4..7. */
static void lab_wide_hole(const NR_DL_FRAME_PARMS *fp, int t, c16_t *sym)
{
  (void)fp;
  (void)t;
  for (int k = 0; k < LAB_NRB * 12; k++)
    if (k % 12 < 4)
      sym[k] = data_re();
}
static void lab_two_holes(const NR_DL_FRAME_PARMS *fp, int t, c16_t *sym)
{
  (void)fp;
  (void)t;
  for (int k = 0; k < LAB_NRB * 12; k++)
    if (k % 12 >= 4 && k % 12 < 8)
      sym[k] = data_re();
}

/* Wide NZP beside unused REs (G5 round 3 on sdd/gap-csirs): through a real channel one antenna sees
 * EVERY port, so an 8-port row-6 resource lights 8 classes (k=0..7) and k=8..11 are simply unused.
 * Sum of all ports' planes through a flat channel whose gain alternates 1, j between the ports of a
 * CDM pair (unit gains would cancel the FD-CDM2 pair on every second subcarrier), scaled to the data
 * EPRE plus s_nzp_boost_db (powerControlOffset; 0 dB unless a case boosts it), plus, when
 * @p load_every > 0, PDSCH on the RBs rb % load_every == 0 (k=8..11 of the CSI-RS symbol carry data there). */
static double s_nzp_boost_db;
static void nzp_all_ports(const NR_DL_FRAME_PARMS *fp, const nr_csirs_candidate_t *c, int t, c16_t *sym)
{
  const int np = nr_csirs_blind_row_ports(c->row);
  c16_t *pl[8];
  for (int p = 0; p < 8; p++)
    pl[p] = malloc((size_t)LAB_FFT * SYMBOLS * sizeof(c16_t));
  generate_ports(fp, c, (LAB_PERIOD * t) % fp->slots_per_frame, pl, np > PLANES ? 8 : PLANES);
  double *re = calloc(2 * LAB_NRB * 12, sizeof(double));
  double pw = 0.0;
  int n_lit = 0;
  for (int k = 0; k < LAB_NRB * 12; k++) {
    for (int p = 0; p < np; p++) {
      const c16_t v = pl[p][(size_t)c->symb_l0 * LAB_FFT + k];
      re[2 * k] += (p & 1) ? -v.i : v.r;
      re[2 * k + 1] += (p & 1) ? v.r : v.i;
    }
    const double e = re[2 * k] * re[2 * k] + re[2 * k + 1] * re[2 * k + 1];
    if (e > 0.0) {
      pw += e;
      n_lit++;
    }
  }
  assert(n_lit > 0);
  const double g = sqrt(2.0 * 700.0 * 700.0 / (pw / n_lit)) * pow(10.0, s_nzp_boost_db / 20.0);
  for (int k = 0; k < LAB_NRB * 12; k++) {
    sym[k].r = (int16_t)(sym[k].r + lrint(g * re[2 * k]));
    sym[k].i = (int16_t)(sym[k].i + lrint(g * re[2 * k + 1]));
  }
  free(re);
  for (int p = 0; p < 8; p++)
    free(pl[p]);
}
static void nzp8_loaded(const NR_DL_FRAME_PARMS *fp, int t, c16_t *sym, int load_every)
{
  const nr_csirs_candidate_t c = lab_row6();
  nzp_all_ports(fp, &c, t, sym);
  for (int k = 0; load_every > 0 && k < LAB_NRB * 12; k++)
    if ((k / 12) % load_every == 0 && sym[k].r == 0 && sym[k].i == 0)
      sym[k] = data_re();
}
static void nzp8_no_data(const NR_DL_FRAME_PARMS *fp, int t, c16_t *sym)
{
  nzp8_loaded(fp, t, sym, 0);
}
static void nzp8_light_load(const NR_DL_FRAME_PARMS *fp, int t, c16_t *sym)
{
  nzp8_loaded(fp, t, sym, 10);
}
static void nzp8_full_load(const NR_DL_FRAME_PARMS *fp, int t, c16_t *sym)
{
  nzp8_loaded(fp, t, sym, 1);
}
static void nzp8_third_load(const NR_DL_FRAME_PARMS *fp, int t, c16_t *sym)
{
  nzp8_loaded(fp, t, sym, 3);
}
/* Two 2-port NZP resources in one symbol (row 3 at k=0,1 and k=4,5), no data. */
static void two_nzp_no_data(const NR_DL_FRAME_PARMS *fp, int t, c16_t *sym)
{
  nr_csirs_candidate_t c = lab_row6();
  c.row = 3;
  c.freq_domain = 1;
  nzp_all_ports(fp, &c, t, sym);
  c.freq_domain = 4;
  nzp_all_ports(fp, &c, t, sym);
}

/* REs (subcarrier-in-RB bitmask) the port planes of @p c occupy at symbol 13 of RB 0 and RB 1. */
static uint32_t port_mask(const NR_DL_FRAME_PARMS *fp, const nr_csirs_candidate_t *c, int n_planes)
{
  c16_t *pl[8];
  for (int p = 0; p < 8; p++)
    pl[p] = malloc((size_t)LAB_FFT * SYMBOLS * sizeof(c16_t));
  generate_ports(fp, c, 0, pl, nr_csirs_blind_row_ports(c->row) > PLANES ? 8 : PLANES);
  uint32_t m = 0;
  for (int p = 0; p < n_planes; p++)
    for (int k = 0; k < 24; k++) {
      const c16_t v = pl[p][(size_t)c->symb_l0 * LAB_FFT + k];
      if (v.r != 0 || v.i != 0)
        m |= 1u << k;
    }
  for (int p = 0; p < 8; p++)
    free(pl[p]);
  return m;
}
static uint32_t union_mask(const NR_DL_FRAME_PARMS *fp, const nr_csirs_candidate_t *c)
{
  const int ports = nr_csirs_blind_row_ports(c->row);
  return port_mask(fp, c, ports < PLANES ? ports : PLANES);
}

/* DECODED-GRANT evidence with the library pieces nr_pdsch_passive_zp_grant_score() uses (that function itself is
 * pinned by test_nr_ssb_rate_match_prod, Z1-Z9): the exported ZP's REs inside the grant's PRBs (rb % load_every
 * == 0) of the CSI-RS symbol against every RE of those PRBs on a data-only symbol and its guard noise floor.
 * Occasions t0.. at the ZP's predicted slots; returns the number of decoded grants rate-matched around the
 * still-exported ZP up to and including the revoking one, or -1 if still exported after @p n occasions. */
static int grant_revoke(const NR_DL_FRAME_PARMS *fp, csi_sym_fn csi_sym, int load_every, nr_csirs_blind_state_t *zp,
                        int w, int t0, int n)
{
  const uint32_t m = union_mask(fp, &zp->cand[w]);
  uint32_t rbs[(LAB_NRB + 31) / 32] = {0};
  for (int rb = 0; rb < LAB_NRB; rb += load_every)
    rbs[rb / 32] |= 1u << (rb % 32);
  c16_t *csi = malloc((size_t)LAB_FFT * sizeof(c16_t));
  c16_t *dat = malloc((size_t)LAB_FFT * sizeof(c16_t));
  int wrong = -1;
  for (int t = t0; t < t0 + n && wrong < 0; t++) {
    memset(csi, 0, (size_t)LAB_FFT * sizeof(c16_t));
    memset(dat, 0, (size_t)LAB_FFT * sizeof(c16_t));
    csi_sym(fp, t, csi);
    for (int k = 0; k < LAB_FFT; k++) { /* noise on the guard bins too: the scorer's noise floor */
      if (k < LAB_NRB * 12 && ((rbs[(k / 12) / 32] >> ((k / 12) % 32)) & 1))
        dat[k] = data_re();
      c16_t *const b[2] = {&csi[k], &dat[k]};
      for (int q = 0; q < 2; q++) {
        b[q]->r = (int16_t)(b[q]->r + noise(20.0));
        b[q]->i = (int16_t)(b[q]->i + noise(20.0));
      }
    }
    double ez = 0.0, ed = 0.0, en = 0.0;
    uint32_t nz = 0, nd = 0, nn = 0;
    nr_csirs_blind_re_energy((const int16_t *)csi, LAB_FFT, 0, rbs, LAB_NRB, 0, m & 0xFFF, (m >> 12) & 0xFFF, &ez, &nz);
    nr_csirs_blind_re_energy((const int16_t *)dat, LAB_FFT, 0, rbs, LAB_NRB, 0, 0xFFF, 0xFFF, &ed, &nd);
    nr_csirs_blind_guard_energy((const int16_t *)dat, LAB_FFT, 0, LAB_NRB * 12, &en, &nn);
    const double gs = nr_csirs_blind_zp_grant_score(ez, nz, ed, nd, en, nn);
    assert(gs >= 0.0 && gs <= NR_CSIRS_BLIND_ZP_MIN_SCORE && "data on the false ZP must contradict it");
    if (!nr_csirs_blind_zp_grant_feed(zp, w, (uint32_t)(LAB_PERIOD * t), gs))
      wrong = t - t0 + 1;
  }
  free(csi);
  free(dat);
  return wrong;
}

static void check_zp_dark_ports(void)
{
  const NR_DL_FRAME_PARMS fp = make_lab_fp();

  /* The premise: port 0 of the row-6 resource lands on k=0,1 only (both RB parities). */
  const nr_csirs_candidate_t row6 = lab_row6();
  const uint32_t m6 = port_mask(&fp, &row6, 1);
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
  lab_port0_only(&fp, 0, sym);
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
  char line[128];
  int w = run_zp_chain(&fp, lab_port0_only, zp, &best);
  if (w >= 0) {
    nr_csirs_blind_format(&zp->cand[w], zp->period, zp->offset, line, sizeof(line));
    printf("ZPDARK: dark-ports case EXPORTED ZP \"%s\" (best CSI-slot score %.3f)\n", line, best);
  } else {
    printf("ZPDARK: dark-ports case: no ZP export (best CSI-slot score %.3f)\n", best);
  }
  assert(w < 0 && "REs dark only because ports 1..7 never reach the receiver were exported as ZP-CSI-RS");
  assert(best <= NR_CSIRS_BLIND_ZP_MIN_SCORE && "a dark-ports candidate scored as a hole");

  /* Control: a genuine hole under data is still exported, on the hole, at the resource's period. */
  w = run_zp_chain(&fp, lab_hole_under_data, zp, &best);
  assert(w >= 0 && "a genuine ZP hole under data must still be exported");
  nr_csirs_blind_format(&zp->cand[w], zp->period, zp->offset, line, sizeof(line));
  const uint32_t mw = union_mask(&fp, &zp->cand[w]);
  printf("ZPDARK: hole-under-data case exported ZP \"%s\" (port-union mask 0x%06x, best %.3f)\n", line, mw, best);
  assert(zp->cand[w].symb_l0 == LAB_SYM);
  assert(mw == 0xF00F00u && "the exported ZP must be the complete k=8..11 hole");
  assert(zp->period == LAB_PERIOD && zp->offset == 0);

  /* Wider holes than any rows-1..5 candidate (8 RE/RB), or two holes in one symbol: no candidate
   * explains the complete quiet pattern of its RBs, and a partial explanation is unidentifiable, so the
   * weakest-class baseline deliberately refuses them (no export; see nr_csirs_blind_search.c). */
  const struct {
    csi_sym_fn fn;
    const char *what;
  } wide[] = {{lab_wide_hole, "8-RE hole k4..11, data k0..3"},
              {lab_two_holes, "two 4-RE holes k0..3 + k8..11, data k4..7"}};
  for (size_t c = 0; c < sizeof(wide) / sizeof(wide[0]); c++) {
    w = run_zp_chain(&fp, wide[c].fn, zp, &best);
    if (w < 0) {
      printf("ZPDARK: %s: not exported, as designed (best CSI-slot score %.3f)\n", wide[c].what, best);
    } else {
      nr_csirs_blind_format(&zp->cand[w], zp->period, zp->offset, line, sizeof(line));
      printf("ZPDARK: %s: EXPORTED ZP \"%s\" (best %.3f)\n", wide[c].what, line, best);
    }
    assert(w < 0 && "a partial explanation of a wider hole was exported");
  }

  /* Dark REs beside NZP only, no PDSCH on them: the lab's other G5 shapes. */
  const struct {
    csi_sym_fn fn;
    const char *what;
  } neg[] = {{two_nzp_no_data, "two 2-class NZP k0,1 + k4,5, no PDSCH"}};
  for (size_t c = 0; c < sizeof(neg) / sizeof(neg[0]); c++) {
    w = run_zp_chain(&fp, neg[c].fn, zp, &best);
    if (w >= 0) {
      nr_csirs_blind_format(&zp->cand[w], zp->period, zp->offset, line, sizeof(line));
      printf("ZPDARK: %s: EXPORTED ZP \"%s\" (best CSI-slot score %.3f)\n", neg[c].what, line, best);
    } else {
      printf("ZPDARK: %s: no ZP export (best CSI-slot score %.3f)\n", neg[c].what, best);
    }
    assert(w < 0 && "dark REs beside NZP in a slot without PDSCH on them were exported as ZP-CSI-RS");
  }

  /* An 8-class NZP beside the unused k=8..11: the row-4 candidate covering all of k=8..11 explains the
   * complete quiet pattern and one symbol cannot tell it from data + a ZP, so it IS exported (and only
   * there: never on a lit RE) -- asserted, so the revocation below is always exercised (G5 review of
   * 060edd290c, Important 2: this used to accept "no export" and skip it). The cure is revocation once
   * PDSCH is scheduled on k=8..11. Two evidence paths, each replayed from the same export:
   * - the full-band score (RT maintenance): revokes a full-band PDSCH only while the NZP is at the PDSCH
   *   EPRE; at +6 dB data on the pattern still reads 0.75 and is NOT a contradiction (pinned limitation);
   * - decoded-grant evidence (the fix): revokes after exactly 2 wrong occasions, full band or 1/3 of the
   *   RBs, NZP at 0 or +6 dB. */
  const struct {
    csi_sym_fn fn;
    const char *what;
    double boost_db;
  } nzp[] = {{nzp8_no_data, "8-class NZP k0..7, k8..11 unused, no PDSCH", 0.0},
             {nzp8_light_load, "8-class NZP k0..7, PDSCH on 10% of RBs", 0.0},
             {nzp8_no_data, "8-class NZP k0..7 boosted +6 dB, k8..11 unused, no PDSCH", 6.0}};
  nr_csirs_blind_state_t *exported = malloc(sizeof(*exported));
  for (size_t c = 0; c < sizeof(nzp) / sizeof(nzp[0]); c++) {
    s_nzp_boost_db = nzp[c].boost_db;
    w = run_zp_chain(&fp, nzp[c].fn, zp, &best);
    printf("ZPDARK: %s: %s (best CSI-slot score %.3f)\n", nzp[c].what, w >= 0 ? "exported" : "NOT exported", best);
    assert(w >= 0 && "the known false export beside a wide NZP must happen, so its revocation is exercised");
    nr_csirs_blind_format(&zp->cand[w], zp->period, zp->offset, line, sizeof(line));
    const uint32_t m = union_mask(&fp, &zp->cand[w]);
    printf("ZPDARK: %s: exported ZP \"%s\" (port-union mask 0x%06x)\n", nzp[c].what, line, m);
    assert(zp->cand[w].symb_l0 == LAB_SYM);
    assert((m & ~0xF00F00u) == 0 && "a ZP beside an NZP was exported on lit REs");
    memcpy(exported, zp, sizeof(*zp));
    zp_ref_t *refs = calloc(NR_CSIRS_BLIND_MAX_CAND, sizeof(*refs));
    const int n13 = load_zp_refs(&fp, zp, refs);
    zp_null_t nl = {.null_n = 0, .null_w = 0};
    double b2 = -1.0;
    replay_zp(&fp, nzp8_full_load, zp, refs, n13, &nl, LAB_OCC, LAB_OCC + 4, false, &b2);
    free_zp_refs(refs, n13);
    free(refs);
    const bool full_band_revoked = !nr_csirs_blind_is_confirmed(zp, w);
    printf("ZPDARK: %s: full-band score only, PDSCH on k8..11 for 4 occasions: %s (revocations %u)\n", nzp[c].what,
           full_band_revoked ? "revoked" : "STILL EXPORTED", zp->zp_revocations[w]);
    if (nzp[c].boost_db == 0.0)
      assert(full_band_revoked && "a false ZP was not revoked once full-band PDSCH used its REs");
    else
      assert(!full_band_revoked && "pinned limitation changed: re-derive the boosted-NZP full-band bound");
    const struct {
      csi_sym_fn fn;
      int every;
    } load[] = {{nzp8_full_load, 1}, {nzp8_third_load, 3}};
    for (size_t l = 0; l < sizeof(load) / sizeof(load[0]); l++) {
      memcpy(zp, exported, sizeof(*zp));
      const int wrong = grant_revoke(&fp, load[l].fn, load[l].every, zp, w, LAB_OCC, 20);
      printf("ZPDARK: %s: decoded grants on 1/%d of the RBs: revoked after %d wrong occasion(s)\n", nzp[c].what,
             load[l].every, wrong);
      assert(wrong == 2 && "decoded-grant evidence must revoke a false ZP after exactly 2 wrong occasions");
      assert(!nr_csirs_blind_is_confirmed(zp, w));
    }
  }
  s_nzp_boost_db = 0.0;
  free(exported);
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
