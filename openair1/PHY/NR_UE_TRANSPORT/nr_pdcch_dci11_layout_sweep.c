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
  /* Only width 4 has a row count verified in-tree (g_table_7_3_2_3_3_1, 12 rows). */
  out->ap_valid_rows = (l->ant_ports == 4) ? 12 : 0;
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
  if (off->ap_valid_rows > 0 && off->ant_ports_bits > 0) {
    const uint32_t ap = peek(payload, off->total, off->ant_ports, off->ant_ports_bits);
    if (ap >= off->ap_valid_rows) {
      return false;
    }
  }
  return true;
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

int nr_dci11_resolver_init(nr_dci11_resolver_t *r, uint16_t bwp_size, uint16_t riv_bits,
                           uint8_t tda_bits, uint16_t observed_len)
{
  if (r == NULL) {
    return 0;
  }
  memset(r, 0, sizeof(*r));
  r->winner = -1;
  r->bwp_size = bwp_size;
  r->riv_bits = riv_bits;
  r->tda_bits = tda_bits;
  r->observed_len = observed_len;
  const int n = nr_dci11_layout_enumerate(riv_bits, tda_bits, observed_len, r->hyp,
                                          NR_DCI11_LAYOUT_MAX);
  if (n <= 0) {
    return 0;
  }
  for (int i = 0; i < n; i++) {
    if (!nr_dci11_layout_offsets(&r->hyp[i], riv_bits, tda_bits, &r->off[i])) {
      return 0;
    }
    r->alive[i] = true;
  }
  r->n_hyp = n;
  r->n_alive = n;
  return n;
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

int nr_dci11_resolver_observe(nr_dci11_resolver_t *r, uint64_t payload)
{
  if (r == NULL || r->n_hyp <= 0) {
    return 0;
  }
  if (r->winner >= 0) {
    return 1;
  }
  for (int i = 0; i < r->n_hyp; i++) {
    if (!r->alive[i]) {
      continue;
    }
    r->seen[i]++;
    if (nr_dci11_layout_plausible(&r->off[i], payload, r->bwp_size)) {
      r->pass[i]++;
    }
  }
  /* Drop in a second pass, and never drop the last one. Dropping inside the loop above would make
   * the outcome depend on candidate order; and an empty set can never converge, so a run of
   * unlucky payloads must not be able to erase the answer. */
  for (int i = 0; i < r->n_hyp && r->n_alive > 1; i++) {
    if (!r->alive[i] || r->seen[i] < DCI11_S1_MIN_SEEN) {
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
  out->pdsch_to_harq_bits = 0;
  out->antenna_ports_bits = l->ant_ports;
  out->tci_bits           = l->post_ant;
  out->srs_request_bits   = 0;
  out->cbg_bits           = 0;
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
  const uint16_t riv = p; p += riv_bits;
  const uint16_t tda = p; p += tda_bits;
  p += f.vrb_to_prb_bits + f.prb_bundling_bits + f.rate_matching_bits + f.zp_csirs_bits;
  const uint16_t mcs = p; p += DCI11_MCS_BITS + DCI11_NDI_BITS;
  const uint16_t rv = p; p += DCI11_RV_BITS;
  p += f.tb2_bits + f.harq_pid_bits + f.dai_bits + DCI11_TPC + DCI11_PUCCH_RI + f.pdsch_to_harq_bits;
  const uint16_t ap = p; p += f.antenna_ports_bits;
  p += f.tci_bits + f.srs_request_bits + f.cbg_bits;
  const uint16_t di = p; p += DCI11_DMRS_INIT;
  return riv == want.riv && tda == want.tda && mcs == want.mcs && rv == want.rv
         && ap == want.ant_ports && di == want.dmrs_init && p == want.total;
}
