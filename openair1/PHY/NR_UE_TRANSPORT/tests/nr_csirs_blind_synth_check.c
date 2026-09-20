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
  printf("nr_csirs_blind_synth_check: PASS\n");
  return 0;
}
