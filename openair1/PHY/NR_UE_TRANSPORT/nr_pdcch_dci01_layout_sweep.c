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

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_dci01_layout_sweep.c */

#include "nr_pdcch_dci01_layout_sweep.h"

#include <stddef.h>

/* TS 38.212 7.3.1.1.2 constant-width fields: identifier 1 | MCS 5 | NDI 1 | RV 2 |
 * UL-SCH indicator 1. Everything else is switch-dependent and lives in the group sums. */
#define DCI01_ID       1
#define DCI01_MCS      5
#define DCI01_NDI      1
#define DCI01_RV       2
#define DCI01_ULSCH    1

bool nr_dci01_layout_offsets(const nr_dci01_layout_t *l, uint16_t riv_bits, uint8_t tda_bits,
                             nr_dci11_offsets_t *out)
{
  if (l == NULL || out == NULL || riv_bits == 0 || l->pre_riv > 3 || l->pre_mcs > 1
      || l->ant_ports < 2 || l->ant_ports > 5) {
    return false;
  }
  uint16_t p = DCI01_ID;
  p += l->pre_riv;                 /* UL/SUL indicator + BWP indicator */
  out->riv = p;            p += riv_bits;
  out->tda = p;            p += tda_bits;
  p += l->pre_mcs;                 /* frequency hopping flag */
  out->mcs = p;            p += DCI01_MCS + DCI01_NDI;
  out->rv  = p;            p += DCI01_RV;
  p += l->pre_ant;                 /* harq | dai1 | dai2 | tpc | sri | precoding | csi request */
  out->ant_ports = p;      p += l->ant_ports;
  out->ant_ports_bits = l->ant_ports;
  /* 0 = do not test. The uplink antenna-port tables (TS 38.212 7.3.1.1.2-6..23) are a different
   * family from the downlink one and their row counts are not verified in-tree; guessing them
   * would reject valid grants, which is exactly what reusing the downlink rule did. */
  out->ap_valid_rows = 0;
  p += l->post_ant;                /* srs req | cbg | ptrs-dmrs | beta offset | dmrs seq init */
  /* DCI 0_1 has no separate DM-RS-init field position for the extractor to read after the ports
   * group -- the UL-SCH indicator closes the payload. Point dmrs_init at it so the shared struct
   * stays fully defined and `total` is never left implicit. */
  out->dmrs_init = p;      p += DCI01_ULSCH;
  out->total = p;
  out->tda_bits = tda_bits;
  out->tda_valid = 0;
  return true;
}

static const uint8_t kUlSulBwp[] = {0, 1, 2, 3};   /* UL/SUL (0..1) + BWP indicator (0..2) */
static const uint8_t kFreqHop[]  = {0, 1};
static const uint8_t kHarq[]     = {4};            /* fixed in NR */
static const uint8_t kDai1[]     = {1, 2};
static const uint8_t kDai2[]     = {0, 2};
static const uint8_t kTpc[]      = {2};            /* fixed */
static const uint8_t kSri[]      = {0, 1, 2, 3, 4};/* SRS resource indicator */
static const uint8_t kPrecode[]  = {0, 1, 2, 3, 4, 5, 6};
static const uint8_t kCsiReq[]   = {0, 1, 2, 3, 4, 5, 6};
static const uint8_t kAnt[]      = {2, 3, 4, 5};
static const uint8_t kSrsReq[]   = {2, 3};
static const uint8_t kCbg[]      = {0, 2, 4, 6, 8};
static const uint8_t kPtrsDmrs[] = {0, 2};
static const uint8_t kBeta[]     = {0, 2};
static const uint8_t kDmrsInit[] = {0, 1};

#define NELEM(a) ((int)(sizeof(a) / sizeof((a)[0])))

static bool have(const nr_dci01_layout_t *o, int n, const nr_dci01_layout_t *c)
{
  for (int i = 0; i < n; i++) {
    if (o[i].pre_riv == c->pre_riv && o[i].pre_mcs == c->pre_mcs && o[i].pre_ant == c->pre_ant
        && o[i].ant_ports == c->ant_ports && o[i].post_ant == c->post_ant) {
      return true;
    }
  }
  return false;
}

int nr_dci01_layout_enumerate(uint16_t riv_bits, uint8_t tda_bits, uint16_t observed_len,
                              nr_dci01_layout_t *out, nr_dci11_offsets_t *offsets, int max)
{
  if (out == NULL || max <= 0 || riv_bits == 0 || observed_len == 0) {
    return -1;
  }
  int n = 0;
  /* The switch space is larger than DCI 1_1's (SRI, precoding and CSI request are each up to 7
   * values), but it collapses the same way: only the SUMS between read fields are distinguishable,
   * and the derived length then pins the rest. */
  for (int a = 0; a < NELEM(kUlSulBwp); a++)
  for (int b = 0; b < NELEM(kFreqHop); b++)
  for (int d1 = 0; d1 < NELEM(kDai1); d1++)
  for (int d2 = 0; d2 < NELEM(kDai2); d2++)
  for (int sr = 0; sr < NELEM(kSri); sr++)
  for (int pc = 0; pc < NELEM(kPrecode); pc++)
  for (int cs = 0; cs < NELEM(kCsiReq); cs++)
  for (int an = 0; an < NELEM(kAnt); an++)
  for (int sq = 0; sq < NELEM(kSrsReq); sq++)
  for (int cb = 0; cb < NELEM(kCbg); cb++)
  for (int pd = 0; pd < NELEM(kPtrsDmrs); pd++)
  for (int be = 0; be < NELEM(kBeta); be++)
  for (int di = 0; di < NELEM(kDmrsInit); di++) {
    /* A PT-RS/DM-RS association field only exists when PT-RS does, and PT-RS in the uplink
     * requires transform precoding to be off, which is also what admits the wider antenna-port
     * tables. Allowing the pair otherwise would invent layouts no RRC can produce. */
    if (kPtrsDmrs[pd] != 0 && kAnt[an] < 3) {
      continue;
    }
    nr_dci01_layout_t c;
    c.pre_riv = kUlSulBwp[a];
    c.pre_mcs = kFreqHop[b];
    c.pre_ant = (uint8_t)(kHarq[0] + kDai1[d1] + kDai2[d2] + kTpc[0] + kSri[sr] + kPrecode[pc]
                          + kCsiReq[cs]);
    c.ant_ports = kAnt[an];
    c.post_ant = (uint8_t)(kSrsReq[sq] + kCbg[cb] + kPtrsDmrs[pd] + kBeta[be] + kDmrsInit[di]);
    nr_dci11_offsets_t off;
    if (!nr_dci01_layout_offsets(&c, riv_bits, tda_bits, &off)) {
      continue;
    }
    if (off.total != observed_len || have(out, n, &c)) {
      continue;
    }
    if (n >= max) {
      return n;
    }
    if (offsets != NULL) {
      offsets[n] = off;
    }
    out[n++] = c;
  }
  return n;
}
