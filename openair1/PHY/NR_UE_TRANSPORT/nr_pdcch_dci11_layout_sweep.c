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

#include <math.h>    // sqrt, for the Wilson interval
#include <pthread.h> // pthread_once (switch-space tables)
#include <string.h>
#include <stddef.h>  // NULL
#include <stdio.h>   // fprintf (hard-refutation arming note)
#include <stdlib.h>

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
  if (l == NULL || out == NULL || riv_bits == 0 || l->bwp_ind > 5
      || l->ant_ports < 4 || l->ant_ports > 6 || l->fdra_mode > NR_FDRA_DYN_CFG2
      || (l->fdra_mode != NR_FDRA_TYPE1 && (l->n_rbg == 0 || l->n_rbg > 31))) {
    return false;
  }
  uint16_t p = DCI11_FIXED_ID;
  p += l->bwp_ind;                                  /* carrier indicator + BWP indicator */
  out->riv = p;            p += (uint16_t)nr_fdra_bits(l->fdra_mode, l->n_rbg, riv_bits);
  out->tda = p;            p += tda_bits;
  p += l->pre_mcs;                                  /* vrb | prb bundling | rate match | zp csirs */
  out->mcs = p;            p += DCI11_MCS_BITS;
  p += DCI11_NDI_BITS;
  out->rv  = p;            p += DCI11_RV_BITS;
  p += l->pre_ant;                                  /* tb2 | harq pid | dai | tpc | ri | p2h */
  out->ant_ports = p;      p += l->ant_ports;
  out->ant_ports_bits = l->ant_ports;
  /* Row counts for all four TS 38.212 7.3.1.2.2 tables, taken from the same g_table_7_3_2_3_3_{1,2,3,4}
   * arrays nr_pdcch_blind_monitor.c's actual DCI 1_1 extraction indexes (12/31/24/58 rows): width 4 is
   * always type 1 maxLength 1 (-1, 12 rows); width 5 is type 1 maxLength 2 (-2, 31 rows) or type 2
   * maxLength 1 (-3, 24 rows), told apart by l->dmrs_type; width 6 is type 2 maxLength 2 (-4, 58 rows). */
  out->ap_valid_rows = (l->ant_ports == 4) ? 12
                     : (l->ant_ports == 5) ? (l->dmrs_type ? 24 : 31)
                     : (l->ant_ports == 6) ? 58 : 0;
  p += l->post_ant;                                 /* tci | srs | cbg | cbg flush */
  out->dmrs_init = p;      p += DCI11_DMRS_INIT;
  p += l->tail;                                     /* priority | CPext | min offset | dormancy | PDCCH adaptation | padding */
  out->total = p;
  out->tda_bits = tda_bits;
  out->tda_valid = 0;
  out->fdra_mode = l->fdra_mode;
  out->n_rbg = l->n_rbg;
  out->riv_bits = (uint8_t)riv_bits;
  return true;
}

/* ---- THE SWITCH SPACE: TS 38.212 7.3.1.2.2 (DCI format 1_1), Rel-15/16/17, FR1 ----------------------------------------------
 * Each RRC switch as the field widths it can produce, in payload order. Enumerating switches rather than arbitrary integers keeps
 * the result a list of REAL configurations instead of every arithmetic combination that happens to sum correctly. The full table
 * with the condition of every width is in docs/superpowers/reports/csd/DCI11-RESOLVER-20261024.md; the unit test
 * Dci11Space.* rebuilds the same space from that table field by field (no group sums) and fails if one option is missing here.
 * Fixed widths: identifier 1, TB1 MCS 5 / NDI 1 / RV 2, TPC for PUCCH 2, PUCCH resource indicator 3, DM-RS sequence init 1.
 * Left out because the standard forbids them in FR1 (they exist only for SCS 480/960 kHz multi-PDSCH scheduling, FR2-2):
 * TDRA widths 5-6, per-PDSCH NDI/RV, the 1-bit type-1-codebook DAI. Couplings applied (anything else is a free product):
 *   VRB-to-PRB mapping bit: absent for RA type 0 ("only resource allocation type 0 configured");
 *   CBGTI: N_TB x {0,2,4,6,8} (two codewords double it); CBG flushing indicator only with CBG transmission configured. */
static const uint8_t kCi[]       = {0, 3};             /* carrier indicator: crossCarrierSchedulingConfig */
static const uint8_t kBwpInd[]   = {0, 1, 2};          /* n_dl_bwp (up to 4 configured BWPs) */
static const uint8_t kVrb[]      = {0, 1};             /* vrb-ToPRB-Interleaver (type 1 / dynamicSwitch only) */
static const uint8_t kBundling[] = {0, 1};             /* prb-BundlingType dynamicBundling */
static const uint8_t kRateM[]    = {0, 1, 2};          /* rateMatchPatternGroup1/2 */
static const uint8_t kZpCsi[]    = {0, 1, 2};          /* aperiodic ZP CSI-RS resource sets: ceil(log2(n + 1)), n <= 3 */
static const uint8_t kTb2[]      = {0, 8};             /* maxNrofCodeWordsScheduledByDCI = 2: TB2 MCS/NDI/RV */
static const uint8_t kHarq[]     = {4, 5};             /* harq-ProcessNumberSizeDCI-1-1-r17 */
static const uint8_t kDai[]      = {0, 2, 4, 6};       /* semi-static | dynamic 1 cell | dynamic CA or enhancedDynamic-r16 1 cell
                                                        * | enhancedDynamic-r16 CA */
static const uint8_t kTpc2[]     = {0, 2};             /* Rel-17 second TPC field (two PUCCH power-control loops, mTRP) */
static const uint8_t kP2H[]      = {0, 1, 2, 3};       /* ceil(log2(|dl-DataToUL-ACK(-r16/-r17)|)), up to 8 entries */
static const uint8_t kOneShot[]  = {0, 1};             /* Rel-16 one-shot HARQ-ACK request (pdsch-HARQ-ACK-OneShotFeedback-r16) */
static const uint8_t kGrpIdx[]   = {0, 1};             /* Rel-16 PDSCH group index (enhancedDynamic-r16) */
static const uint8_t kNfi[]      = {0, 1, 2};          /* Rel-16 new feedback indicator */
static const uint8_t kNGrp[]     = {0, 1};             /* Rel-16 number of requested PDSCH group(s) */
static const uint8_t kHarqRetx[] = {0, 1};             /* Rel-17 HARQ-ACK retransmission indicator */
static const uint8_t kPucchCell[]= {0, 1};             /* Rel-17 PUCCH cell indicator (PUCCH carrier switching) */
static const uint8_t kAnt[]      = {4, 5, 6};          /* DM-RS type x maxLength (Tables 7.3.1.2.2-1..4 and their -1A..-4A twins) */
static const uint8_t kTci[]      = {0, 3};             /* tci-PresentInDCI */
static const uint8_t kSrs[]      = {2, 3};             /* supplementaryUplink */
static const uint8_t kSrsOff[]   = {0, 1, 2};          /* Rel-17 SRS offset indicator (availableSlotOffset list) */
static const uint8_t kCbg[]      = {0, 2, 4, 6, 8};    /* maxCodeBlockGroupsPerTransportBlock, per TB */
static const uint8_t kCbgFlush[] = {0, 1};             /* codeBlockGroupFlushIndicator */
/* after the DM-RS sequence initialisation (never read, so only their SUM -- the tail -- matters): */
static const uint8_t kPrio[]     = {0, 1};             /* Rel-16 priority indicator */
static const uint8_t kCpExt[]    = {0, 1, 2, 3, 4};    /* Rel-16 ChannelAccess-CPext (shared spectrum) */
static const uint8_t kMinOff[]   = {0, 1};             /* Rel-16 minimum applicable scheduling offset indicator */
static const uint8_t kDormancy[] = {0, 1, 2, 3, 4, 5}; /* Rel-16 SCell dormancy indication (dormancy groups) */
static const uint8_t kPdcchAd[]  = {0, 1, 2};          /* Rel-17 PDCCH monitoring adaptation indication */
static const uint8_t kPad[]      = {0, 1};             /* TS 38.212 7.3.1.0 size alignment: one zero bit when 1_1 equals 0_0/1_0 */

#define DCI11_HARQ_PID 4
#define DCI11_TPC      2
#define DCI11_PUCCH_RI 3

#define NELEM(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* Distinct group sums, in first-occurrence order of the per-switch loops (bitmask over sums 0..63). A layout is identified by
 * its group SUMS, so many switch combinations collapse to one entry; deduplicating is not cosmetic: without it the same layout
 * would be scored several times and would dominate a round-robin purely by appearing more often. */
typedef struct {
  uint64_t pre_mcs[2];      /* [type0]: vrb | bundling | rate match | zp */
  uint64_t pre_ant[2];      /* [tb2]: tb2 | harq | dai | tpc | 2nd tpc | pri | k1 | one-shot | group | nfi | n groups | retx | cell */
  uint64_t post_ant[2];     /* [tb2]: tci | srs | srs offset | cbgti | cbgfi */
  uint64_t tail;            /* prio | cpext | min offset | dormancy | pdcch adaptation | padding */
  uint8_t pre_ant_order[64];/* distinct pre_ant sums over both tb2 values, ascending */
  int n_pre_ant;
} dci11_space_t;

static dci11_space_t g_space;
static pthread_once_t g_space_once = PTHREAD_ONCE_INIT;
static void dci11_space_build(void)
{
  dci11_space_t sp;
  memset(&sp, 0, sizeof(sp));
  for (int t0 = 0; t0 < 2; t0++)
    for (int b = 0; b < NELEM(kVrb); b++)
      for (int c = 0; c < NELEM(kBundling); c++)
        for (int d = 0; d < NELEM(kRateM); d++)
          for (int e = 0; e < NELEM(kZpCsi); e++)
            if (!(t0 && kVrb[b]))
              sp.pre_mcs[t0] |= 1ULL << (kVrb[b] + kBundling[c] + kRateM[d] + kZpCsi[e]);
  for (int t = 0; t < 2; t++) {
    for (int h = 0; h < NELEM(kHarq); h++)
      for (int d = 0; d < NELEM(kDai); d++)
        for (int q = 0; q < NELEM(kTpc2); q++)
          for (int k = 0; k < NELEM(kP2H); k++)
            for (int a = 0; a < NELEM(kOneShot); a++)
              for (int g = 0; g < NELEM(kGrpIdx); g++)
                for (int f = 0; f < NELEM(kNfi); f++)
                  for (int n = 0; n < NELEM(kNGrp); n++)
                    for (int x = 0; x < NELEM(kHarqRetx); x++)
                      for (int y = 0; y < NELEM(kPucchCell); y++)
                        sp.pre_ant[t] |= 1ULL << (kTb2[t] + kHarq[h] + kDai[d] + DCI11_TPC + kTpc2[q] + DCI11_PUCCH_RI + kP2H[k]
                                                  + kOneShot[a] + kGrpIdx[g] + kNfi[f] + kNGrp[n] + kHarqRetx[x] + kPucchCell[y]);
    const int n_tb = kTb2[t] ? 2 : 1;
    for (int j = 0; j < NELEM(kTci); j++)
      for (int k = 0; k < NELEM(kSrs); k++)
        for (int o = 0; o < NELEM(kSrsOff); o++)
          for (int m = 0; m < NELEM(kCbg); m++)
            for (int q = 0; q < NELEM(kCbgFlush); q++) {
              /* A flush indicator without CBG transmission is not a configuration the RRC can produce. */
              if (kCbg[m] == 0 && kCbgFlush[q] != 0)
                continue;
              sp.post_ant[t] |= 1ULL << (kTci[j] + kSrs[k] + kSrsOff[o] + n_tb * kCbg[m] + kCbgFlush[q]);
            }
  }
  for (int a = 0; a < NELEM(kPrio); a++)
    for (int b = 0; b < NELEM(kCpExt); b++)
      for (int c = 0; c < NELEM(kMinOff); c++)
        for (int d = 0; d < NELEM(kDormancy); d++)
          for (int e = 0; e < NELEM(kPdcchAd); e++)
            for (int f = 0; f < NELEM(kPad); f++)
              sp.tail |= 1ULL << (kPrio[a] + kCpExt[b] + kMinOff[c] + kDormancy[d] + kPdcchAd[e] + kPad[f]);
  for (int v = 0; v < 64; v++)
    if (((sp.pre_ant[0] | sp.pre_ant[1]) >> v) & 1)
      sp.pre_ant_order[sp.n_pre_ant++] = (uint8_t)v;
  g_space = sp;
}
static const dci11_space_t *dci11_space(void)
{
  pthread_once(&g_space_once, dci11_space_build);
  return &g_space;
}

/* The (post_ant, tail) split a prefix of pre_ant sum `pa` and `rest` remaining bits admits: the largest post_ant whose remainder
 * is a legal tail, over the TB2 hypotheses consistent with pa. false if none. */
static bool dci11_split(const dci11_space_t *sp, int pa, int rest, uint8_t *post, uint8_t *tail)
{
  int best = -1;
  for (int t = 0; t < 2; t++) {
    if (pa < 0 || pa > 63 || !((sp->pre_ant[t] >> pa) & 1))
      continue;
    for (int po = rest < 63 ? rest : 63; po > best; po--)
      if (((sp->post_ant[t] >> po) & 1) && rest - po >= 0 && rest - po < 64 && ((sp->tail >> (rest - po)) & 1)) {
        best = po;
        break;
      }
  }
  if (best < 0)
    return false;
  *post = (uint8_t)best;
  *tail = (uint8_t)(rest - best);
  return true;
}

/* Calls emit(l) for every layout of one FDRA mode and TDA width; stops (returning the count so far) when emit returns false.
 * Order: bwp/CI sum, pre_mcs, pre_ant, antenna-ports width, DM-RS type (innermost). */
typedef bool (*dci11_emit_fn)(void *ctx, const nr_dci11_layout_t *l, const nr_dci11_offsets_t *off);
static long enumerate_mode_cb(uint16_t riv_bits, uint8_t tda_bits, uint16_t observed_len, uint8_t fdra_mode, uint8_t n_rbg,
                              dci11_emit_fn emit, void *ctx)
{
  const dci11_space_t *sp = dci11_space();
  const bool t0 = fdra_mode == NR_FDRA_TYPE0_CFG1 || fdra_mode == NR_FDRA_TYPE0_CFG2;
  uint8_t g0[8];
  int n0 = 0;
  for (int c = 0; c < NELEM(kCi); c++)
    for (int b = 0; b < NELEM(kBwpInd); b++) {
      const int v = kCi[c] + kBwpInd[b];
      bool dup = false;
      for (int i = 0; i < n0; i++) dup |= g0[i] == v;
      if (!dup) g0[n0++] = (uint8_t)v;
    }
  const int fdra = nr_fdra_bits(fdra_mode, n_rbg, riv_bits);
  long n = 0;
  for (int a = 0; a < n0; a++)
    for (int pm = 0; pm < 64; pm++) {
      if (!((sp->pre_mcs[t0] >> pm) & 1))
        continue;
      for (int y = 0; y < sp->n_pre_ant; y++) {
        const int pa = sp->pre_ant_order[y];
        for (int i = 0; i < NELEM(kAnt); i++)
          for (int dt = 0; dt < 2; dt++) {
            /* A 5-bit antenna-ports field is Table -2 (type 1, len 2) OR Table -3 (type 2, len 1): same width, different
             * port sets, so two layouts. */
            if ((kAnt[i] == 4 && dt == 1) || (kAnt[i] == 6 && dt == 0))
              continue;
            const int used = DCI11_FIXED_ID + g0[a] + fdra + tda_bits + pm + DCI11_MCS_BITS + DCI11_NDI_BITS + DCI11_RV_BITS + pa
                             + kAnt[i] + DCI11_DMRS_INIT;
            /* THE CONSTRAINT. The DCI length is already derived by the length sweep, so anything that does not sum to it
             * cannot be this cell. */
            uint8_t post, tail;
            if (used > observed_len || !dci11_split(sp, pa, observed_len - used, &post, &tail))
              continue;
            const nr_dci11_layout_t cand = {.bwp_ind = g0[a], .pre_mcs = (uint8_t)pm, .pre_ant = (uint8_t)pa,
                                            .ant_ports = kAnt[i], .post_ant = post, .dmrs_type = (uint8_t)dt,
                                            .fdra_mode = fdra_mode, .n_rbg = n_rbg, .tail = tail};
            nr_dci11_offsets_t off;
            if (!nr_dci11_layout_offsets(&cand, riv_bits, tda_bits, &off) || off.total != observed_len)
              continue;
            if (emit && !emit(ctx, &cand, &off))
              return n;
            n++;
          }
      }
    }
  return n;
}

typedef struct { nr_dci11_layout_t *out; int n, max; } dci11_store_t;
static bool store_emit(void *ctx, const nr_dci11_layout_t *l, const nr_dci11_offsets_t *off)
{
  (void)off;
  dci11_store_t *st = (dci11_store_t *)ctx;
  if (st->n >= st->max)
    return false;
  st->out[st->n++] = *l;
  return true;
}

/* Appends one FDRA mode's layouts to out[n..max). */
static int enumerate_mode(uint16_t riv_bits, uint8_t tda_bits, uint16_t observed_len, uint8_t fdra_mode,
                          uint8_t n_rbg, nr_dci11_layout_t *out, int n, int max)
{
  dci11_store_t st = {.out = out, .n = n, .max = max};
  enumerate_mode_cb(riv_bits, tda_bits, observed_len, fdra_mode, n_rbg, store_emit, &st);
  return st.n;
}

int nr_dci11_layout_enumerate(uint16_t riv_bits, uint8_t tda_bits, uint16_t observed_len,
                              nr_dci11_layout_t *out, int max)
{
  if (out == NULL || max <= 0 || riv_bits == 0 || observed_len == 0) {
    return -1;
  }
  return enumerate_mode(riv_bits, tda_bits, observed_len, NR_FDRA_TYPE1, 0, out, 0, max);
}

/* N_RBG of a mode on this BWP, or -1 when the mode is not searched there: no RBG table entry, or
 * rbg-Size config2 with config1's RBG size (it reads exactly the same bits the same way). */
static int mode_n_rbg(int m, uint16_t bwp_start, uint16_t bwp_size)
{
  if (m == NR_FDRA_TYPE1)
    return 0;
  const int P = nr_fdra_rbg_size(m, bwp_size);
  if (P == 0 || ((m == NR_FDRA_TYPE0_CFG2 || m == NR_FDRA_DYN_CFG2) && P == nr_fdra_rbg_size(m - 1, bwp_size)))
    return -1;
  return nr_rbg_count(bwp_start, bwp_size, P);
}

int nr_dci11_layout_enumerate_mode(uint16_t riv_bits, uint8_t tda_bits, uint16_t observed_len, uint16_t bwp_start,
                                   uint16_t bwp_size, uint8_t fdra_mode, nr_dci11_layout_t *out, int max)
{
  if (out == NULL || max <= 0 || riv_bits == 0 || observed_len == 0 || bwp_size == 0 || fdra_mode > NR_FDRA_DYN_CFG2) {
    return -1;
  }
  const int n_rbg = mode_n_rbg(fdra_mode, bwp_start, bwp_size);
  return n_rbg < 0 ? 0 : enumerate_mode(riv_bits, tda_bits, observed_len, fdra_mode, (uint8_t)n_rbg, out, 0, max);
}

/* The resolver's mode order: type 1, type 0 cfg1, dynamicSwitch cfg1, dynamicSwitch cfg2, type 0 cfg2 (only an ordering of ids:
 * every mode is in the set from the start). */
static const uint8_t kModeOrder[5] = {NR_FDRA_TYPE1, NR_FDRA_TYPE0_CFG1, NR_FDRA_DYN_CFG1, NR_FDRA_DYN_CFG2, NR_FDRA_TYPE0_CFG2};

int nr_dci11_layout_enumerate_fdra(uint16_t riv_bits, uint8_t tda_bits, uint16_t observed_len,
                                   uint16_t bwp_start, uint16_t bwp_size, nr_dci11_layout_t *out, int max)
{
  int n = 0;
  for (int k = 0; k < 5 && n < max; k++) {
    const int e = nr_dci11_layout_enumerate_mode(riv_bits, tda_bits, observed_len, bwp_start, bwp_size, kModeOrder[k],
                                                 out + n, max - n);
    if (e < 0)
      return -1;
    n += e;
  }
  return n;
}

/* MSB-first field read, mirroring nr_pdcch_blind_monitor.c's read_field(). */
static uint32_t peek(nr_dci_bits_t payload, uint16_t total, uint16_t off, uint8_t nbits)
{
  if (nbits == 0 || off + nbits > total) {
    return 0;
  }
  return nr_dci_bits_field(&payload, total, off, nbits);
}

bool nr_dci11_layout_plausible(const nr_dci11_offsets_t *off, nr_dci_bits_t payload, uint16_t bwp_size)
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
  /* The RIV must address a real allocation inside the BWP, and a type-0 bitmap must select at least
   * one RBG. Either failing is impossible for a correctly-placed field and common for a misplaced one.
   * dynamicSwitch: the MSB picks which of the two tests applies (TS 38.212 7.3.1.2.2). */
  uint32_t fdra = peek(payload, off->total, off->riv, (uint8_t)(off->tda - off->riv));
  bool type0 = off->fdra_mode == NR_FDRA_TYPE0_CFG1 || off->fdra_mode == NR_FDRA_TYPE0_CFG2;
  if (off->fdra_mode == NR_FDRA_DYN_CFG1 || off->fdra_mode == NR_FDRA_DYN_CFG2)
    type0 = !nr_fdra_dynamic_split(fdra, off->n_rbg, off->riv_bits, &fdra, &fdra);
  if (type0 ? fdra == 0 : fdra >= (uint32_t)bwp_size * ((uint32_t)bwp_size + 1u) / 2u) {
    return false;
  }
  /* Antenna ports: at width 4 the indexed table (TS 38.212 Table 7.3.1.2.2-1, dmrs-Type=1 /
   * maxLength=1) defines only 12 of the 16 codepoints -- the same 12-row table already carried in
   * nr_pdcch_blind_monitor.c. So a quarter of misplaced reads land on an undefined row, for free.
   * Applied ONLY at width 4: the wider tables' row counts are not independently verified here, and
   * guessing them would invent rejections. */
  if (off->ap_valid_rows > 0 && off->ant_ports_bits > 0) {
    const uint32_t ap = peek(payload, off->total, off->ant_ports, off->ant_ports_bits);
    if (ap >= off->ap_valid_rows) {
      return false;
    }
  }
  /* TDA index beyond the configured list: impossible for the true layout. Off until the caller has
   * the list length (tda_valid == 0), because the 16-entry default table would test nothing. */
  if (off->tda_valid > 0 && off->tda_bits > 0) {
    const uint32_t tda = peek(payload, off->total, off->tda, off->tda_bits);
    if (tda >= off->tda_valid) {
      return false;
    }
  }
  return true;
}


/* nr_dci11_layout_impossible: true when a CRC-verified DCI 1_1 payload cannot come from this layout: the frequency-domain resource assignment is out of range for the BWP (or zero in type 0), or the antenna-ports or TDA field exceeds its valid row count.
 *   Pure function. Callers: nr_dci11_resolver_observe() (hard refutation) and the tests.
 */
bool nr_dci11_layout_impossible(const nr_dci11_offsets_t *off, nr_dci_bits_t payload, uint16_t bwp_size)
{
  if (off == NULL || bwp_size == 0)
    return false;
  uint32_t fdra = peek(payload, off->total, off->riv, (uint8_t)(off->tda - off->riv));
  bool type0 = off->fdra_mode == NR_FDRA_TYPE0_CFG1 || off->fdra_mode == NR_FDRA_TYPE0_CFG2;
  if (off->fdra_mode == NR_FDRA_DYN_CFG1 || off->fdra_mode == NR_FDRA_DYN_CFG2)
    type0 = !nr_fdra_dynamic_split(fdra, off->n_rbg, off->riv_bits, &fdra, &fdra);
  if (type0 ? fdra == 0 : fdra >= (uint32_t)bwp_size * ((uint32_t)bwp_size + 1u) / 2u)
    return true;
  if (off->ap_valid_rows > 0 && off->ant_ports_bits > 0
      && peek(payload, off->total, off->ant_ports, off->ant_ports_bits) >= off->ap_valid_rows)
    return true;
  if (off->tda_valid > 0 && off->tda_bits > 0 && peek(payload, off->total, off->tda, off->tda_bits) >= off->tda_valid)
    return true;
  return false;
}

/* layout_eval: nr_dci11_layout_plausible() and nr_dci11_layout_impossible() of one layout in one pass. The two read the same FDRA, antenna-port and
 * TDA fields; observe() called both for every live layout of every accepted DCI. plausible == no impossible read AND mcs <= 27. Same results
 * (checked by Dci11ObserveGolden against the state hash of the previous implementation). */
static void layout_eval(const nr_dci11_offsets_t *off, nr_dci_bits_t payload, uint16_t bwp_size, bool *plausible, bool *impossible)
{
  *plausible = false;
  *impossible = false;
  if (off == NULL || bwp_size == 0)
    return;
  const uint32_t mcs = peek(payload, off->total, off->mcs, DCI11_MCS_BITS);
  uint32_t fdra = peek(payload, off->total, off->riv, (uint8_t)(off->tda - off->riv));
  bool type0 = off->fdra_mode == NR_FDRA_TYPE0_CFG1 || off->fdra_mode == NR_FDRA_TYPE0_CFG2;
  if (off->fdra_mode == NR_FDRA_DYN_CFG1 || off->fdra_mode == NR_FDRA_DYN_CFG2)
    type0 = !nr_fdra_dynamic_split(fdra, off->n_rbg, off->riv_bits, &fdra, &fdra);
  bool bad = type0 ? fdra == 0 : fdra >= (uint32_t)bwp_size * ((uint32_t)bwp_size + 1u) / 2u;
  if (!bad && off->ap_valid_rows > 0 && off->ant_ports_bits > 0
      && peek(payload, off->total, off->ant_ports, off->ant_ports_bits) >= off->ap_valid_rows)
    bad = true;
  if (!bad && off->tda_valid > 0 && off->tda_bits > 0 && peek(payload, off->total, off->tda, off->tda_bits) >= off->tda_valid)
    bad = true;
  *impossible = bad;
  *plausible = !bad && mcs <= 27;
}

/* ---- STATEFUL RESOLVER ------------------------------------------------------------------------ */

#include <string.h>

/* Stage 1 drops a candidate whose plausibility rate falls far below what the TRUE layout must
 * show. The true layout is plausible on essentially every payload (it is what the gNB emitted), so
 * the bar can be high -- but not 100 %: a cell WITH retransmissions legitimately emits MCS 28-31,
 * and a single such grant must not delete the answer. */
#define DCI11_S1_MIN_SEEN  64     /* evidence before a candidate may be dropped at all */
/* 0.75, NOT ~1.0, and the margin is load-bearing. Two effects make the TRUE layout look
 * implausible on a minority of payloads, and deleting it is unrecoverable:
 *   - MCS 28-31 are reserved retransmission rows, so a cell that RETRANSMITS emits them
 *     legitimately. The reserved-MCS test is therefore a soft signal, not an invariant.
 *   - blind-accepted DCIs include occasional false accepts (a random payload whose CRC happened to
 *     mask to an in-range RNTI); those are noise under EVERY layout, the true one included.
 * The RIV-range and antenna-ports tests ARE invariants for the true layout, which is why the bar
 * can still sit far above the ~0.66 mean a wrong layout scores. */
#define DCI11_S1_MIN_RATE  0.75

/* Stage 2 mirrors Technique D: enough trials on the leader, and its Wilson lower bound must clear
 * every rival's upper bound. Constants intentionally the same, for the same measured reason -- the
 * true configuration decodes at ~40 % on this link, so an absolute floor near 0.6 could never fire. */
#define DCI11_S2_MIN_TRIALS 64
#define DCI11_S2_MIN_RATE   0.02

/* Wilson score interval, same form nr_crc_interval uses, kept local so this file stays pure. */
static void wilson(uint32_t ok, uint32_t n, double *lo, double *hi)
{
  if (n == 0) {
    *lo = 0.0;
    *hi = 1.0;
    return;
  }
  const double z = 1.96, nn = (double)n, p = (double)ok / nn;
  const double d = 1.0 + z * z / nn;
  const double c = p + z * z / (2.0 * nn);
  const double s = z * sqrt(p * (1.0 - p) / nn + z * z / (4.0 * nn * nn));
  *lo = (c - s) / d;
  *hi = (c + s) / d;
  if (*lo < 0.0) *lo = 0.0;
  if (*hi > 1.0) *hi = 1.0;
}

/* Every mode and TDA width of one resolver, through one callback (the init, the evidence filter and the space count share it). */
static long enumerate_all_cb(uint16_t bwp_start, uint16_t bwp_size, uint16_t riv_bits, uint8_t tda_bits, uint16_t observed_len,
                             dci11_emit_fn emit, void *ctx)
{
  /* NR_DCI11_TDA_UNKNOWN: the TDRA list size is itself an RRC switch the receiver cannot read (the
   * dedicated pdsch-TimeDomainAllocationList travels ciphered, and SIB1's common list is only a
   * hypothesis about it). Enumerate every width 0..4 bits; each hypothesis carries its own
   * tda_bits in its offsets and the length constraint prunes most combinations. OTA 2026-09-15:
   * arming with the assumed 4-bit default on a 1-bit cell left no true layout enumerable at all. */
  const uint8_t tb_lo = (tda_bits == NR_DCI11_TDA_UNKNOWN) ? 0 : tda_bits;
  const uint8_t tb_hi = (tda_bits == NR_DCI11_TDA_UNKNOWN) ? 4 : tda_bits;
  long n = 0;
  for (int k = 0; k < 5; k++) {
    const int m = kModeOrder[k];
    const int n_rbg = (bwp_size > 0) ? mode_n_rbg(m, bwp_start, bwp_size) : (m == NR_FDRA_TYPE1 ? 0 : -1);
    if (n_rbg < 0)
      continue;
    for (uint8_t tb = tb_lo; tb <= tb_hi; tb++) {
      const long e = enumerate_mode_cb(riv_bits, tb, observed_len, (uint8_t)m, (uint8_t)n_rbg, emit, ctx);
      n += e;
      if (emit && e < 0)
        return n;
    }
  }
  return n;
}

long nr_dci11_layout_space_count(uint16_t bwp_start, uint16_t bwp_size, uint16_t riv_bits, uint8_t tda_bits, uint16_t observed_len)
{
  if (riv_bits == 0 || observed_len == 0)
    return 0;
  return enumerate_all_cb(bwp_start, bwp_size, riv_bits, tda_bits, observed_len, NULL, NULL);
}

typedef struct {
  nr_dci11_resolver_t *r;
  const nr_dci_bits_t *ev;
  int n_ev;
  long seen;
} dci11_init_ctx_t;
static bool init_emit(void *vctx, const nr_dci11_layout_t *l, const nr_dci11_offsets_t *off)
{
  dci11_init_ctx_t *c = (dci11_init_ctx_t *)vctx;
  nr_dci11_resolver_t *r = c->r;
  c->seen++;
  if (c->n_ev > 0) { /* the stage-1 hard refutation, applied before storing (only when the space does not fit) */
    int bad = 0;
    for (int e = 0; e < c->n_ev && bad < NR_DCI11_HARD_REFUTE_N; e++)
      bad += nr_dci11_layout_impossible(off, c->ev[e], r->bwp_size);
    if (bad >= NR_DCI11_HARD_REFUTE_N)
      return true;
  }
  if (r->n_hyp >= NR_DCI11_LAYOUT_MAX) {
    r->n_truncated++;
    return true;
  }
  r->hyp[r->n_hyp] = *l;
  r->off[r->n_hyp] = *off;
  r->alive[r->n_hyp] = true;
  r->n_hyp++;
  return true;
}

int nr_dci11_resolver_init_ev(nr_dci11_resolver_t *r, uint16_t bwp_start, uint16_t bwp_size, uint16_t riv_bits, uint8_t tda_bits,
                              uint16_t observed_len, const nr_dci_bits_t *ev, int n_ev)
{
  if (r == NULL) {
    return 0;
  }
  memset(r, 0, sizeof(*r));
  r->winner = -1;
  r->bwp_start = bwp_start;
  r->bwp_size = bwp_size;
  r->riv_bits = riv_bits;
  r->tda_bits = tda_bits;
  r->observed_len = observed_len;
  if (riv_bits == 0 || observed_len == 0)
    return 0;
  const long space = nr_dci11_layout_space_count(bwp_start, bwp_size, riv_bits, tda_bits, observed_len);
  r->n_space = space > (long)UINT32_MAX ? UINT32_MAX : (uint32_t)space;
  /* Evidence only when the space does not fit: for every length that fits, the set is the whole standard space. */
  dci11_init_ctx_t c = {.r = r, .ev = (space > NR_DCI11_LAYOUT_MAX) ? ev : NULL, .n_ev = (space > NR_DCI11_LAYOUT_MAX && ev) ? n_ev : 0};
  enumerate_all_cb(bwp_start, bwp_size, riv_bits, tda_bits, observed_len, init_emit, &c);
  r->n_alive = r->n_hyp;
  return r->n_hyp;
}

int nr_dci11_resolver_init_fdra(nr_dci11_resolver_t *r, uint16_t bwp_start, uint16_t bwp_size, uint16_t riv_bits,
                                uint8_t tda_bits, uint16_t observed_len)
{
  return nr_dci11_resolver_init_ev(r, bwp_start, bwp_size, riv_bits, tda_bits, observed_len, NULL, 0);
}

int nr_dci11_resolver_init(nr_dci11_resolver_t *r, uint16_t bwp_size, uint16_t riv_bits,
                           uint8_t tda_bits, uint16_t observed_len)
{
  return nr_dci11_resolver_init_fdra(r, 0, bwp_size, riv_bits, tda_bits, observed_len);
}

/* ANY pass -- its own feed(), a code-block probe, or its interpretation family's (a layout reading
 * identical fields decodes identically) -- means a layout is not refuted. */
static bool layout_has_pass(const nr_dci11_resolver_t *r, int i)
{
  return r->ok[i] || r->probe_ok[i] || r->fam_ok[r->layout_fam[i] % NR_DCI11_FAM_N];
}

/* Append entries at n_hyp and publish them with one release store: readers on other threads walk
 * [0, n_hyp) and must never see a half-written entry. */
static int append_offsets(nr_dci11_resolver_t *r, const nr_dci11_layout_t *hyp, const nr_dci11_offsets_t *off, int n)
{
  const int base = r->n_hyp;
  int k = 0;
  for (; k < n && base + k < NR_DCI11_LAYOUT_MAX; k++) {
    if (off[k].total != r->observed_len)
      break;
    if (hyp)
      r->hyp[base + k] = hyp[k];
    r->off[base + k] = off[k];
    const uint8_t tb = r->off[base + k].tda_bits;
    r->off[base + k].tda_valid = (r->tda_count > 0 && tb > 0 && r->tda_count < (1u << tb)) ? r->tda_count : 0;
    r->alive[base + k] = true;
  }
  r->n_alive += k;
  __atomic_store_n(&r->n_hyp, base + k, __ATOMIC_RELEASE);
  return k;
}

int nr_dci_resolver_append_offsets(nr_dci11_resolver_t *r, const nr_dci11_offsets_t *offsets, int n)
{
  if (r == NULL || offsets == NULL || n <= 0 || r->n_hyp <= 0)
    return 0;
  return append_offsets(r, NULL, offsets, n);
}

int nr_dci_resolver_init_from_offsets(nr_dci11_resolver_t *r, uint16_t bwp_size,
                                      const nr_dci11_offsets_t *offsets, int n)
{
  if (r == NULL || offsets == NULL || n <= 0 || n > NR_DCI11_LAYOUT_MAX) {
    return 0;
  }
  memset(r, 0, sizeof(*r));
  r->winner = -1;
  r->bwp_size = bwp_size;
  for (int i = 0; i < n; i++) {
    /* An offsets list that disagrees with itself about the payload size would make stage 1 read
     * past the end of some candidates, so reject the whole list rather than score part of it. */
    if (offsets[i].total != offsets[0].total || offsets[i].total == 0) {
      return 0;
    }
    r->off[i] = offsets[i];
    r->alive[i] = true;
  }
  r->observed_len = offsets[0].total;
  r->n_hyp = n;
  r->n_alive = n;
  return n;
}

void nr_dci11_resolver_set_tda_count(nr_dci11_resolver_t *r, uint8_t tda_count)
{
  if (r == NULL)
    return;
  r->tda_count = tda_count;
  for (int i = 0; i < r->n_hyp; i++)
    r->off[i].tda_valid = (tda_count > 0 && r->off[i].tda_bits > 0 && tda_count < (1u << r->off[i].tda_bits))
                              ? tda_count : 0;
}

/* ---- Thompson sampling ------------------------------------------------------------------------ */
static double ts_u(uint64_t *s)
{
  *s ^= *s >> 12; *s ^= *s << 25; *s ^= *s >> 27;
  return (double)((*s * 2685821657736338717ULL) >> 11) * (1.0 / 9007199254740992.0);
}
static double ts_n(uint64_t *s)
{
  const double u1 = ts_u(s) + 1e-300, u2 = ts_u(s);
  return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}
static double ts_gamma(double a, uint64_t *s)   /* Marsaglia-Tsang, a >= 1 */
{
  const double d = a - 1.0 / 3.0, c = 1.0 / sqrt(9.0 * d);
  for (;;) {
    double x, v;
    do { x = ts_n(s); v = 1.0 + c * x; } while (v <= 0.0);
    v = v * v * v;
    const double u = ts_u(s);
    if (u < 1.0 - 0.0331 * x * x * x * x)
      return d * v;
    if (log(u) < 0.5 * x * x + d * (1.0 - v + log(v)))
      return d * v;
  }
}
int nr_dci11_thompson_pick(const uint32_t *ok, const uint32_t *trials, const double *prior, int n,
                           uint64_t *rng)
{
  if (ok == NULL || trials == NULL || n <= 0 || rng == NULL)
    return -1;
  if (*rng == 0)
    *rng = 0x9E3779B97F4A7C15ULL;
  int arg = 0;
  double bestp = -1.0;
  for (int i = 0; i < n; i++) {
    const double a = 1.0 + (double)ok[i] + (prior ? prior[i] : 0.0);
    const double b = 1.0 + (double)(trials[i] >= ok[i] ? trials[i] - ok[i] : 0);
    const double x = ts_gamma(a, rng), y = ts_gamma(b, rng);
    const double p = x / (x + y);
    if (p > bestp) {
      bestp = p;
      arg = i;
    }
  }
  return arg;
}

/* nr_dci11_resolver_observe: observes one accepted DCI 1_1 payload: updates the pass and impossible counters of every live layout, hard-refutes layouts that are impossible for enough payloads, and drops layouts whose optimistic pass rate is below the bar.
 *   Drops are applied in a second pass and never remove the last live layout.. Returns the number of live layouts. Callers: nr_dci11_resolver_observe_rnti() and nr_pdcch_blind_monitor_rt.c (via nr_pdcch_dci11_layout_observe()).
 */
int nr_dci11_resolver_observe(nr_dci11_resolver_t *r, nr_dci_bits_t payload)
{
  if (r == NULL || r->n_hyp <= 0) {
    return 0;
  }
  if (r->winner >= 0) {
    return 1;
  }
  for (int i = nr_alive_next(r->alive, 0, r->n_hyp); i < r->n_hyp; i = nr_alive_next(r->alive, i + 1, r->n_hyp)) {
    r->seen[i]++;
    bool plausible, impossible;
    layout_eval(&r->off[i], payload, r->bwp_size, &plausible, &impossible);
    if (plausible) {
      r->pass[i]++;
    }
    if (impossible)
      r->impossible[i]++;
  }
  r->n_obs++;
  /* HARD REFUTATION (T26). The soft rate rule below keeps a layout whose impossible reads
   * are rare: OTA PCI 415 (replay raw_pci415_12s_20261008) sends a full-band type-0 bitmap in 13 % of its DCIs,
   * which every type-1 layout reads as RIV = 0x7fff > N(N+1)/2 -- impossible, yet 87 % plausible, so type 1 was
   * never refuted and RA type 0 waited for a TB-CRC refutation that 12 s of traffic cannot supply. A CRC-verified
   * C-RNTI DCI that no correct layout can produce refutes the layout outright; NR_DCI11_HARD_REFUTE_N of them
   * (guarding against a rare false DCI) remove it. Every FDRA mode is in the set from the start, so this is also what
   * separates type 0 from type 1 on a cell whose DCIs make one of them impossible. */
  if (r->winner < 0) {
    for (int i = nr_alive_next(r->alive, 0, r->n_hyp); i < r->n_hyp; i = nr_alive_next(r->alive, i + 1, r->n_hyp)) {
      if (layout_has_pass(r, i) || r->impossible[i] < NR_DCI11_HARD_REFUTE_N)
        continue;
      if (r->n_alive > 1) {
        r->alive[i] = false;
        r->n_alive--;
      }
    }
  }
  /* Drop in a second pass, and never drop the last one. Dropping inside the loop above would make
   * the outcome depend on candidate order; and an empty set can never converge, so a run of
   * unlucky payloads must not be able to erase the answer. */
  for (int i = nr_alive_next(r->alive, 0, r->n_hyp); i < r->n_hyp && r->n_alive > 1; i = nr_alive_next(r->alive, i + 1, r->n_hyp)) {
    if (r->seen[i] < DCI11_S1_MIN_SEEN) {
      continue;
    }
    /* Drop on the UPPER confidence bound, not the point estimate. A drop is IRREVERSIBLE, so the
     * question is not "is this candidate below the bar right now" but "can it still be above it".
     * The point estimate answered the wrong one: at a true rate of 0.80 and n = 64, ordinary
     * binomial noise puts the sample under 0.75 about one time in six -- and that deleted the true
     * layout outright on any cell that retransmits. Requiring the optimistic bound to be under the
     * bar costs only a little more evidence and cannot delete a candidate that is really above it. */
    double lo, hi;
    wilson(r->pass[i], r->seen[i], &lo, &hi);
    if (hi < DCI11_S1_MIN_RATE) {
      r->alive[i] = false;
      r->n_alive--;
    }
  }
  return r->n_alive;
}

/* One window: does the 4-bit field at `at` read exactly one above its previous value (mod 16)? */
static int harq_step(nr_dci_bits_t prev, nr_dci_bits_t cur, uint16_t total, int at)
{
  if (at < 0 || at + 4 > total)
    return -1;
  return ((peek(cur, total, (uint16_t)at, 4) - peek(prev, total, (uint16_t)at, 4)) & 15) == 1;
}

/* nr_dci11_resolver_observe_rnti: observes a DCI 1_1 payload with its RNTI: when HARQ-sequence pruning is on, tracks the previous payload of each RNTI and counts how often a layout's HARQ-process field steps by one; then runs nr_dci11_resolver_observe().
 *   A layout reading no HARQ step loses its place only after another layout shows a clear rate (3/4 of the best rate). Returns the live count. Caller: nr_pdcch_dci11_layout_observe() in nr_pdcch_blind_monitor_rt.c.
 */
int nr_dci11_resolver_observe_rnti(nr_dci11_resolver_t *r, nr_dci_bits_t payload, uint16_t rnti)
{
  if (r == NULL || r->n_hyp <= 0 || rnti == 0 || r->winner >= 0)
    return nr_dci11_resolver_observe(r, payload);
  int slot = -1, empty = -1;
  for (int k = 0; k < NR_DCI11_HARQ_RNTIS; k++) {
    if (r->harq_rnti[k] == rnti) slot = k;
    if (r->harq_rnti[k] == 0 && empty < 0) empty = k;
  }
  if (slot >= 0) {
    const nr_dci_bits_t prev = r->harq_prev[slot];
    for (int i = nr_alive_next(r->alive, 0, r->n_hyp); i < r->n_hyp; i = nr_alive_next(r->alive, i + 1, r->n_hyp)) {
      if (r->harq_pairs[i] == UINT16_MAX)
        continue;
      const nr_dci11_offsets_t *o = &r->off[i];
      const int a = harq_step(prev, payload, o->total, o->rv + DCI11_RV_BITS);
      const int b = harq_step(prev, payload, o->total, o->rv + DCI11_RV_BITS + 8); /* behind TB2 MCS/NDI/RV */
      r->harq_pairs[i]++;
      r->harq_inc[i] += (a == 1 || b == 1);
    }
    r->harq_prev[slot] = payload;
  } else {
    const int k = empty >= 0 ? empty : (int)(rnti % NR_DCI11_HARQ_RNTIS);
    r->harq_rnti[k] = rnti;
    r->harq_prev[k] = payload;
  }
  const int n_alive = nr_dci11_resolver_observe(r, payload);
  /* Evidence first: only once SOME live layout shows a HARQ counter far above chance does a layout reading none
   * lose its place -- a cell with random HARQ PIDs never triggers this and keeps every layout. */
  double best = 0.0;
  for (int i = nr_alive_next(r->alive, 0, r->n_hyp); i < r->n_hyp; i = nr_alive_next(r->alive, i + 1, r->n_hyp))
    if (r->alive[i] && r->harq_pairs[i] >= 16 && r->harq_inc[i] >= 12 && 5u * r->harq_inc[i] >= 2u * r->harq_pairs[i]
        && (double)r->harq_inc[i] / r->harq_pairs[i] > best)
      best = (double)r->harq_inc[i] / r->harq_pairs[i];
  if (best <= 0.0)
    return n_alive;
  /* Relative bar: a window one bit off the counter still steps by one on ~1/2 of the pairs (RV bit + PID >> 1), two
   * bits off on ~1/4; the aligned window steps on nearly every pair. 3/4 of the best rate separates them. */
  for (int i = nr_alive_next(r->alive, 0, r->n_hyp); i < r->n_hyp && r->n_alive > 1; i = nr_alive_next(r->alive, i + 1, r->n_hyp))
    if (r->alive[i] && !layout_has_pass(r, i) && r->harq_pairs[i] >= 16
        && (double)r->harq_inc[i] / r->harq_pairs[i] < 0.75 * best) {
      r->alive[i] = false;
      r->n_alive--;
    }
  return r->n_alive;
}

int nr_dci11_resolver_next(nr_dci11_resolver_t *r, nr_dci11_offsets_t *out)
{
  if (r == NULL || out == NULL || r->n_hyp <= 0 || r->n_alive <= 0) {
    return -1;
  }
  if (r->winner >= 0) {
    *out = r->off[r->winner];
    return r->winner;
  }
  /* Round-robin over the LIVE set only. Walking the full array and skipping dead entries keeps the
   * rotation uniform as candidates are removed. */
  for (int step = 0; step < r->n_hyp; step++) {
    const int idx = (r->cursor + step) % r->n_hyp;
    if (r->alive[idx]) {
      r->cursor = (idx + 1) % r->n_hyp;
      *out = r->off[idx];
      return idx;
    }
  }
  return -1;
}

int nr_dci11_resolver_feed(nr_dci11_resolver_t *r, int idx, bool tb_crc_ok)
{
  if (r == NULL || idx < 0 || idx >= r->n_hyp) {
    return (r != NULL) ? r->winner : -1;
  }
  if (r->winner >= 0) {
    return r->winner;
  }
  r->trials[idx]++;
  if (tb_crc_ok) {
    r->ok[idx]++;
  }
  /* One live candidate left is the answer by elimination -- but only once it has actually decoded
   * something, so "everything else was dropped" cannot promote a layout that never worked. */
  if (r->n_alive == 1 && r->ok[idx] > 0 && r->alive[idx]) {
    r->winner = idx;
    return idx;
  }
  if ((r->trials[idx] % 16) != 0) {
    return -1;
  }
  int leader = -1;
  for (int i = 0; i < r->n_hyp; i++) {
    if (!r->alive[i]) {
      continue;
    }
    const double ri = r->trials[i] ? (double)r->ok[i] / (double)r->trials[i] : 0.0;
    const double rl = (leader < 0 || !r->trials[leader])
                          ? -1.0
                          : (double)r->ok[leader] / (double)r->trials[leader];
    if (leader < 0 || ri > rl) {
      leader = i;
    }
  }
  if (leader < 0 || r->trials[leader] < DCI11_S2_MIN_TRIALS) {
    return -1;
  }
  double lo, hi;
  wilson(r->ok[leader], r->trials[leader], &lo, &hi);
  if (lo < DCI11_S2_MIN_RATE) {
    return -1;   /* a dead link must not be able to "separate" */
  }
  for (int i = 0; i < r->n_hyp; i++) {
    if (i == leader || !r->alive[i]) {
      continue;
    }
    double olo, ohi;
    wilson(r->ok[i], r->trials[i], &olo, &ohi);
    if (ohi >= lo) {
      return -1;   /* not separated from this rival yet */
    }
  }
  r->winner = leader;
  return leader;
}

int nr_dci11_resolver_winner(const nr_dci11_resolver_t *r)
{
  return (r != NULL) ? r->winner : -1;
}


/* ---- handing a resolved layout back to the extractor ------------------------------------------ */

bool nr_dci11_layout_to_field_bits(const nr_dci11_layout_t *l, nr_dci11_field_bits_t *out)
{
  if (l == NULL || out == NULL) {
    return false;
  }
  /* TPC (2) and the PUCCH resource indicator (3) sit inside pre_ant here but are counted as
   * CONSTANTS by nr_pdcch_blind_dci_size_ex(), so they must be removed before the remainder is
   * handed over -- otherwise the payload grows by 5 bits and every field after the MCS shifts. */
  const int pre_ant_variable = (int)l->pre_ant - (DCI11_TPC + DCI11_PUCCH_RI);
  if (pre_ant_variable < 0) {
    return false;
  }
  out->bwp_indicator_bits = l->bwp_ind;
  out->vrb_to_prb_bits    = l->pre_mcs;
  out->prb_bundling_bits  = 0;
  out->rate_matching_bits = 0;
  out->zp_csirs_bits      = 0;
  out->tb2_bits           = pre_ant_variable;
  out->harq_pid_bits      = 0;
  out->dai_bits           = 0;
  /* HARQPID: the blob (tb2 + harq 4 + dai + pdsch_to_harq) used to be read as ONE tb2 field, so every grant got harq_pid 0
   * (HARQ soft buffers of all processes aliased, DCIRV scorer read the right bits but the decode path did not). Read the pid
   * 4 bits behind the RV (as solver_hq_of does, tb2 assumed absent) and park the rest in dai; the total is unchanged. */
  if (pre_ant_variable >= 4) {
    out->tb2_bits      = 0;
    out->harq_pid_bits = 4;
    out->dai_bits      = pre_ant_variable - 4;
  }
  out->pdsch_to_harq_bits = 0;
  out->antenna_ports_bits = l->ant_ports;
  out->dmrs_config_type = l->dmrs_type;
  out->tci_bits           = l->post_ant;
  out->srs_request_bits   = 0;
  out->cbg_bits           = 0;
  out->fdra_mode          = l->fdra_mode;
  out->tail_bits          = l->tail;
  return true;
}

bool nr_dci11_layout_apply_roundtrip(const nr_dci11_layout_t *l, uint16_t riv_bits, uint8_t tda_bits)
{
  nr_dci11_offsets_t want;
  if (!nr_dci11_layout_offsets(l, riv_bits, tda_bits, &want)) {
    return false;
  }
  nr_dci11_field_bits_t f;
  if (!nr_dci11_layout_to_field_bits(l, &f)) {
    return false;
  }
  /* Rebuild the offsets the way nr_pdcch_blind_dci_size_ex() accumulates them: the constants it
   * owns (identifier, MCS/NDI/RV, TPC, PUCCH-RI, DM-RS init) plus the per-field widths. If this
   * disagrees with `want`, the handover silently shifts a field and the decode dies with no
   * diagnostic -- which is exactly how the original bwp_indicator/TDA bug behaved. */
  uint16_t p = DCI11_FIXED_ID;
  p += f.bwp_indicator_bits;
  const uint16_t riv = p; p += (uint16_t)nr_fdra_bits(f.fdra_mode, l->n_rbg, riv_bits);
  const uint16_t tda = p; p += tda_bits;
  p += f.vrb_to_prb_bits + f.prb_bundling_bits + f.rate_matching_bits + f.zp_csirs_bits;
  const uint16_t mcs = p; p += DCI11_MCS_BITS + DCI11_NDI_BITS;
  const uint16_t rv = p; p += DCI11_RV_BITS;
  p += f.tb2_bits + f.harq_pid_bits + f.dai_bits + DCI11_TPC + DCI11_PUCCH_RI + f.pdsch_to_harq_bits;
  const uint16_t ap = p; p += f.antenna_ports_bits;
  p += f.tci_bits + f.srs_request_bits + f.cbg_bits;
  const uint16_t di = p; p += DCI11_DMRS_INIT;
  p += f.tail_bits;
  return riv == want.riv && tda == want.tda && mcs == want.mcs && rv == want.rv
         && ap == want.ant_ports && di == want.dmrs_init && p == want.total;
}
