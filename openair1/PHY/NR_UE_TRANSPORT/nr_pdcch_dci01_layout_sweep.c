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
      || l->ant_ports < 2 || l->ant_ports > 5 || l->fdra_mode > NR_FDRA_DYN_CFG2
      || (l->fdra_mode != NR_FDRA_TYPE1 && (l->n_rbg == 0 || l->n_rbg > 31))) {
    return false;
  }
  uint16_t p = DCI01_ID;
  p += l->pre_riv;                 /* UL/SUL indicator + BWP indicator */
  out->riv = p;            p += (uint16_t)nr_fdra_bits(l->fdra_mode, l->n_rbg, riv_bits);
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
  out->fdra_mode = l->fdra_mode;
  out->n_rbg = l->n_rbg;
  out->riv_bits = (uint8_t)riv_bits;
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

/* Distinct group sums in first-occurrence order of the per-switch loops (see the DCI 1_1 enumerator):
 * distinct by construction and in the order the per-switch nesting emitted. */
static int push_distinct(uint8_t *v, int n, int cap, int x)
{
  for (int i = 0; i < n; i++)
    if (v[i] == x)
      return n;
  if (n >= cap)
    return n; /* cannot happen with the switch tables below (<= 18 distinct sums per group) */
  v[n] = (uint8_t)x;
  return n + 1;
}

/* Appends one FDRA mode's layouts to out[n..max). */
static int enumerate_mode(uint16_t riv_bits, uint8_t tda_bits, uint16_t observed_len, uint8_t fdra_mode,
                          uint8_t n_rbg, nr_dci01_layout_t *out, nr_dci11_offsets_t *offsets, int n, int max)
{
  /* The switch space is larger than DCI 1_1's (SRI, precoding and CSI request are each up to 7
   * values), but it collapses the same way: only the SUMS between read fields are distinguishable,
   * and the derived length then pins the rest. */
  uint8_t pa[48], po[2][48];
  int npa = 0, npo[2] = {0, 0};
  for (int d1 = 0; d1 < NELEM(kDai1); d1++)
    for (int d2 = 0; d2 < NELEM(kDai2); d2++)
      for (int sr = 0; sr < NELEM(kSri); sr++)
        for (int pc = 0; pc < NELEM(kPrecode); pc++)
          for (int cs = 0; cs < NELEM(kCsiReq); cs++)
            npa = push_distinct(pa, npa, 48, kHarq[0] + kDai1[d1] + kDai2[d2] + kTpc[0] + kSri[sr] + kPrecode[pc] + kCsiReq[cs]);
  /* A PT-RS/DM-RS association field only exists when PT-RS does, and PT-RS in the uplink requires
   * transform precoding to be off, which is also what admits the wider antenna-port tables. Allowing
   * the pair otherwise would invent layouts no RRC can produce: po[0] is the post-ports group for a
   * port field narrower than 3 bits (no PT-RS association), po[1] for the others. */
  for (int w = 0; w < 2; w++)
    for (int sq = 0; sq < NELEM(kSrsReq); sq++)
      for (int cb = 0; cb < NELEM(kCbg); cb++)
        for (int pd = 0; pd < NELEM(kPtrsDmrs); pd++)
          for (int be = 0; be < NELEM(kBeta); be++)
            for (int di = 0; di < NELEM(kDmrsInit); di++) {
              if (kPtrsDmrs[pd] != 0 && w == 0)
                continue;
              npo[w] = push_distinct(po[w], npo[w], 48, kSrsReq[sq] + kCbg[cb] + kPtrsDmrs[pd] + kBeta[be] + kDmrsInit[di]);
            }
  for (int a = 0; a < NELEM(kUlSulBwp); a++)
    for (int b = 0; b < NELEM(kFreqHop); b++)
      for (int y = 0; y < npa; y++)
        for (int an = 0; an < NELEM(kAnt); an++) {
          const int w = kAnt[an] >= 3;
          for (int z = 0; z < npo[w]; z++) {
            const nr_dci01_layout_t c = {.pre_riv = kUlSulBwp[a], .pre_mcs = kFreqHop[b], .pre_ant = pa[y],
                                         .ant_ports = kAnt[an], .post_ant = po[w][z], .fdra_mode = fdra_mode,
                                         .n_rbg = n_rbg};
            nr_dci11_offsets_t off;
            if (!nr_dci01_layout_offsets(&c, riv_bits, tda_bits, &off) || off.total != observed_len)
              continue;
            if (n >= max)
              return n;
            if (offsets != NULL)
              offsets[n] = off;
            out[n++] = c;
          }
        }
  return n;
}

int nr_dci01_layout_enumerate(uint16_t riv_bits, uint8_t tda_bits, uint16_t observed_len,
                              nr_dci01_layout_t *out, nr_dci11_offsets_t *offsets, int max)
{
  if (out == NULL || max <= 0 || riv_bits == 0 || observed_len == 0) {
    return -1;
  }
  return enumerate_mode(riv_bits, tda_bits, observed_len, NR_FDRA_TYPE1, 0, out, offsets, 0, max);
}

int nr_dci01_layout_enumerate_mode(uint16_t riv_bits, uint8_t tda_bits, uint16_t observed_len, uint16_t bwp_start,
                                   uint16_t bwp_size, uint8_t fdra_mode, nr_dci01_layout_t *out,
                                   nr_dci11_offsets_t *offsets, int max)
{
  if (out == NULL || max <= 0 || riv_bits == 0 || observed_len == 0 || bwp_size == 0 || fdra_mode > NR_FDRA_DYN_CFG2) {
    return -1;
  }
  int n_rbg = 0;
  if (fdra_mode != NR_FDRA_TYPE1) {
    const int P = nr_fdra_rbg_size(fdra_mode, bwp_size);
    if (P == 0
        || ((fdra_mode == NR_FDRA_TYPE0_CFG2 || fdra_mode == NR_FDRA_DYN_CFG2) && P == nr_fdra_rbg_size(fdra_mode - 1, bwp_size)))
      return 0;
    n_rbg = nr_rbg_count(bwp_start, bwp_size, P);
  }
  return enumerate_mode(riv_bits, tda_bits, observed_len, fdra_mode, (uint8_t)n_rbg, out, offsets, 0, max);
}

int nr_dci01_layout_enumerate_fdra(uint16_t riv_bits, uint8_t tda_bits, uint16_t observed_len, uint16_t bwp_start,
                                   uint16_t bwp_size, nr_dci01_layout_t *out, nr_dci11_offsets_t *offsets, int max)
{
  int n = 0;
  for (int m = NR_FDRA_TYPE1; m <= NR_FDRA_DYN_CFG2 && n < max; m++) {
    const int k = nr_dci01_layout_enumerate_mode(riv_bits, tda_bits, observed_len, bwp_start, bwp_size, (uint8_t)m,
                                                 out + n, offsets ? offsets + n : NULL, max - n);
    if (k < 0)
      return -1;
    n += k;
  }
  return n;
}

int nr_dci01_fdra_verdict(uint32_t tb_try, uint32_t tb_ok, bool armed, const nr_dci11_resolver_t *r)
{
  if (tb_ok > 0)
    return NR_DCI01_FDRA_BOOK;
  if (!armed)
    return tb_try >= NR_DCI11_FDRA_ARM_MIN_TRIALS ? NR_DCI01_FDRA_ARM : NR_DCI01_FDRA_BOOK;
  if (r != NULL)
    for (int i = 0; i < r->n_hyp; i++)
      if (r->alive[i] && r->off[i].fdra_mode != NR_FDRA_TYPE1)
        return NR_DCI01_FDRA_REFUSE;
  return NR_DCI01_FDRA_BOOK;
}
