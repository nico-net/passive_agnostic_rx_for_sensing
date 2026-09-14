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

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_dci11_layout_sweep.c
 * \brief Pure core of the DCI 1_1 layout sweep. See the header for the design argument.
 */

#include "nr_pdcch_dci11_layout_sweep.h"

#include <stddef.h>  // NULL

/* TS 38.212 7.3.1.2.2, fields at fixed width regardless of any RRC switch:
 *   format identifier 1 | MCS 5 | NDI 1 | RV 2 | TPC for PUCCH 2 | PUCCH resource indicator 3
 *   | DM-RS sequence initialisation 1
 * TPC and the PUCCH resource indicator sit between the DAI and the PDSCH-to-HARQ indicator, so
 * they are folded into pre_ant by the enumerator rather than counted here. */
#define DCI11_FIXED_ID    1
#define DCI11_MCS_BITS    5
#define DCI11_NDI_BITS    1
#define DCI11_RV_BITS     2
#define DCI11_DMRS_INIT   1

bool nr_dci11_layout_offsets(const nr_dci11_layout_t *l, uint16_t riv_bits, uint8_t tda_bits,
                             nr_dci11_offsets_t *out)
{
  if (l == NULL || out == NULL || riv_bits == 0 || l->bwp_ind > 2
      || l->ant_ports < 4 || l->ant_ports > 6) {
    return false;
  }
  uint16_t p = DCI11_FIXED_ID;   /* carrier indicator is 0 here -- see the header */
  p += l->bwp_ind;
  out->riv = p;            p += riv_bits;
  out->tda = p;            p += tda_bits;
  p += l->pre_mcs;                                  /* vrb | prb bundling | rate match | zp csirs */
  out->mcs = p;            p += DCI11_MCS_BITS;
  p += DCI11_NDI_BITS;
  out->rv  = p;            p += DCI11_RV_BITS;
  p += l->pre_ant;                                  /* tb2 | harq pid | dai | tpc | ri | p2h */
  out->ant_ports = p;      p += l->ant_ports;
  out->ant_ports_bits = l->ant_ports;
  p += l->post_ant;                                 /* tci | srs | cbg | cbg flush */
  out->dmrs_init = p;      p += DCI11_DMRS_INIT;
  out->total = p;
  return true;
}

/* The RRC switches, each as the widths it can produce. Enumerating these rather than arbitrary
 * integers is what keeps the result a list of REAL configurations instead of every arithmetic
 * combination that happens to sum correctly. */
static const uint8_t kBwpInd[]  = {0, 1, 2};             /* n_dl_bwp */
static const uint8_t kVrb[]     = {0, 1};                /* interleaved VRB-to-PRB configured */
static const uint8_t kBundling[]= {0, 1};                /* dynamic PRB bundling */
static const uint8_t kRateM[]   = {0, 1, 2};             /* rateMatchPatternGroup1/2 */
static const uint8_t kZpCsi[]   = {0, 1, 2};             /* aperiodic ZP CSI-RS resource sets */
static const uint8_t kTb2[]     = {0, 8};                /* maxNrofCodeWordsScheduledByDCI = 2 */
static const uint8_t kDai[]     = {0, 2, 4};             /* semi-static | dynamic | dynamic+CA */
static const uint8_t kP2H[]     = {0, 1, 2, 3};          /* ceil(log2(|dl_DataToUL_ACK|)) */
static const uint8_t kAnt[]     = {4, 5, 6};             /* DM-RS type x maxLength */
static const uint8_t kTci[]     = {0, 3};                /* tci_PresentInDCI */
static const uint8_t kSrs[]     = {2, 3};                /* supplementaryUplink */
static const uint8_t kCbg[]     = {0, 2, 4, 6, 8};       /* maxCodeBlockGroupsPerTransportBlock */
static const uint8_t kCbgFlush[]= {0, 1};                /* codeBlockGroupFlushIndicator */

#define DCI11_HARQ_PID 4
#define DCI11_TPC      2
#define DCI11_PUCCH_RI 3

#define NELEM(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* A layout is identified by its group SUMS, so many switch combinations collapse to one entry.
 * Deduplicating is not cosmetic: without it the same layout would be scored several times and
 * would dominate a round-robin purely by appearing more often. */
static bool already_have(const nr_dci11_layout_t *out, int n, const nr_dci11_layout_t *c)
{
  for (int i = 0; i < n; i++) {
    if (out[i].bwp_ind == c->bwp_ind && out[i].pre_mcs == c->pre_mcs && out[i].pre_ant == c->pre_ant
        && out[i].ant_ports == c->ant_ports && out[i].post_ant == c->post_ant) {
      return true;
    }
  }
  return false;
}

int nr_dci11_layout_enumerate(uint16_t riv_bits, uint8_t tda_bits, uint16_t observed_len,
                              nr_dci11_layout_t *out, int max)
{
  if (out == NULL || max <= 0 || riv_bits == 0 || observed_len == 0) {
    return -1;
  }
  int n = 0;
  for (int a = 0; a < NELEM(kBwpInd); a++) {
    for (int b = 0; b < NELEM(kVrb); b++) {
      for (int c = 0; c < NELEM(kBundling); c++) {
        for (int d = 0; d < NELEM(kRateM); d++) {
          for (int e = 0; e < NELEM(kZpCsi); e++) {
            for (int f = 0; f < NELEM(kTb2); f++) {
              for (int g = 0; g < NELEM(kDai); g++) {
                for (int h = 0; h < NELEM(kP2H); h++) {
                  for (int i = 0; i < NELEM(kAnt); i++) {
                    for (int j = 0; j < NELEM(kTci); j++) {
                      for (int k = 0; k < NELEM(kSrs); k++) {
                        for (int m = 0; m < NELEM(kCbg); m++) {
                          for (int q = 0; q < NELEM(kCbgFlush); q++) {
                            /* A flush indicator without CBG transmission is not a configuration
                             * the RRC can produce; allowing it would invent layouts. */
                            if (kCbg[m] == 0 && kCbgFlush[q] != 0) {
                              continue;
                            }
                            nr_dci11_layout_t cand;
                            cand.bwp_ind   = kBwpInd[a];
                            cand.pre_mcs   = (uint8_t)(kVrb[b] + kBundling[c] + kRateM[d] + kZpCsi[e]);
                            cand.pre_ant   = (uint8_t)(kTb2[f] + DCI11_HARQ_PID + kDai[g]
                                                       + DCI11_TPC + DCI11_PUCCH_RI + kP2H[h]);
                            cand.ant_ports = kAnt[i];
                            cand.post_ant  = (uint8_t)(kTci[j] + kSrs[k] + kCbg[m] + kCbgFlush[q]);
                            nr_dci11_offsets_t off;
                            if (!nr_dci11_layout_offsets(&cand, riv_bits, tda_bits, &off)) {
                              continue;
                            }
                            /* THE CONSTRAINT. The DCI length is already derived by the length
                             * sweep, so anything that does not sum to it cannot be this cell. */
                            if (off.total != observed_len || already_have(out, n, &cand)) {
                              continue;
                            }
                            if (n >= max) {
                              return n;
                            }
                            out[n++] = cand;
                          }
                        }
                      }
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  }
  return n;
}

/* MSB-first field read, mirroring nr_pdcch_blind_monitor.c's read_field(). */
static uint32_t peek(uint64_t payload, uint16_t total, uint16_t off, uint8_t nbits)
{
  if (nbits == 0 || off + nbits > total) {
    return 0;
  }
  const int shift = total - off - nbits;
  return (uint32_t)((payload >> shift) & ((1ULL << nbits) - 1ULL));
}

bool nr_dci11_layout_plausible(const nr_dci11_offsets_t *off, uint64_t payload, uint16_t bwp_size)
{
  if (off == NULL || bwp_size == 0) {
    return false;
  }
  /* MCS 28-31 are the reserved retransmission rows: they carry no modulation order or code rate of
   * their own, so an initial transmission never uses them. A misaligned read lands there 4 times in
   * 32 by chance, which is the cheapest wrong-layout signal available. */
  const uint32_t mcs = peek(payload, off->total, off->mcs, DCI11_MCS_BITS);
  if (mcs > 27) {
    return false;
  }
  /* RV is 2 bits and all four values are legal, so it cannot reject on range -- but rv != 0 on a
   * link with no retransmissions is the same evidence that exposed the original field-width bug
   * (233/316 grants decoding as rv != 0). Treated as a soft signal by the caller, not asserted
   * here, because a cell WITH retransmissions would legitimately show it. */
  /* The RIV must address a real allocation inside the BWP. Out-of-range is impossible for a
   * correctly-placed field and common for a misplaced one. */
  const uint32_t riv = peek(payload, off->total, off->riv,
                            (uint8_t)(off->tda - off->riv));
  const uint32_t riv_max = (uint32_t)bwp_size * ((uint32_t)bwp_size + 1u) / 2u;
  if (riv >= riv_max) {
    return false;
  }
  /* Antenna ports: at width 4 the indexed table (TS 38.212 Table 7.3.1.2.2-1, dmrs-Type=1 /
   * maxLength=1) defines only 12 of the 16 codepoints -- the same 12-row table already carried in
   * nr_pdcch_blind_monitor.c. So a quarter of misplaced reads land on an undefined row, for free.
   * Applied ONLY at width 4: the wider tables' row counts are not independently verified here, and
   * guessing them would invent rejections. */
  if (off->ant_ports_bits == 4) {
    const uint32_t ap = peek(payload, off->total, off->ant_ports, 4);
    if (ap >= 12) {
      return false;
    }
  }
  return true;
}
