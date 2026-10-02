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
#include <stdlib.h>  // getenv/atoi (ISAC_DCI11_S1_OFF)

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
      || l->ant_ports < 4 || l->ant_ports > 6 || l->fdra_mode > NR_FDRA_DYN_CFG2
      || (l->fdra_mode != NR_FDRA_TYPE1 && (l->n_rbg == 0 || l->n_rbg > 31))) {
    return false;
  }
  uint16_t p = DCI11_FIXED_ID;   /* carrier indicator is 0 here -- see the header */
  p += l->bwp_ind;
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
  out->total = p;
  out->tda_bits = tda_bits;
  out->tda_valid = 0;
  out->fdra_mode = l->fdra_mode;
  out->n_rbg = l->n_rbg;
  out->riv_bits = (uint8_t)riv_bits;
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
static const uint8_t kHarq[]    = {4, 5};                /* harq-ProcessNumberSizeDCI-1-1-r17 (32 processes) */
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
 * would dominate a round-robin purely by appearing more often. So each group's DISTINCT sums are
 * collected first, in first-occurrence order of the per-switch loops, and only those are combined:
 * distinct by construction (no O(n^2) search -- it cost 551 ms at 26.8k layouts), and in exactly the
 * order the per-switch nesting emitted (groups are independent and nested in the same order, and the
 * DM-RS type stays innermost). */
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
                          uint8_t n_rbg, nr_dci11_layout_t *out, int n, int max)
{
  uint8_t pm[32], pa[32], po[32];
  int npm = 0, npa = 0, npo = 0;
  for (int b = 0; b < NELEM(kVrb); b++)            /* vrb | prb bundling | rate match | zp csirs */
    for (int c = 0; c < NELEM(kBundling); c++)
      for (int d = 0; d < NELEM(kRateM); d++)
        for (int e = 0; e < NELEM(kZpCsi); e++)
          npm = push_distinct(pm, npm, 32, kVrb[b] + kBundling[c] + kRateM[d] + kZpCsi[e]);
  for (int f = 0; f < NELEM(kTb2); f++)            /* tb2 | dai | harq | tpc | ri | p2h */
    for (int g = 0; g < NELEM(kDai); g++)
      for (int hq = 0; hq < NELEM(kHarq); hq++)
        for (int h = 0; h < NELEM(kP2H); h++)
          npa = push_distinct(pa, npa, 32, kTb2[f] + kHarq[hq] + kDai[g] + DCI11_TPC + DCI11_PUCCH_RI + kP2H[h]);
  for (int j = 0; j < NELEM(kTci); j++)            /* tci | srs | cbg | cbg flush */
    for (int k = 0; k < NELEM(kSrs); k++)
      for (int m = 0; m < NELEM(kCbg); m++)
        for (int q = 0; q < NELEM(kCbgFlush); q++) {
          /* A flush indicator without CBG transmission is not a configuration the RRC can produce;
           * allowing it would invent layouts. */
          if (kCbg[m] == 0 && kCbgFlush[q] != 0)
            continue;
          npo = push_distinct(po, npo, 32, kTci[j] + kSrs[k] + kCbg[m] + kCbgFlush[q]);
        }
  for (int a = 0; a < NELEM(kBwpInd); a++)
    for (int x = 0; x < npm; x++)
      for (int y = 0; y < npa; y++)
        for (int i = 0; i < NELEM(kAnt); i++)
          for (int z = 0; z < npo; z++)
            /* A 5-bit antenna-ports field is Table -2 (type 1, len 2) OR Table -3 (type 2, len 1):
             * same width, different port sets, so two layouts. */
            for (int dt = 0; dt < 2; dt++) {
              if ((kAnt[i] == 4 && dt == 1) || (kAnt[i] == 6 && dt == 0))
                continue;
              const nr_dci11_layout_t cand = {.bwp_ind = kBwpInd[a], .pre_mcs = pm[x], .pre_ant = pa[y],
                                              .ant_ports = kAnt[i], .post_ant = po[z], .dmrs_type = (uint8_t)dt,
                                              .fdra_mode = fdra_mode, .n_rbg = n_rbg};
              nr_dci11_offsets_t off;
              /* THE CONSTRAINT. The DCI length is already derived by the length sweep, so anything
               * that does not sum to it cannot be this cell. */
              if (!nr_dci11_layout_offsets(&cand, riv_bits, tda_bits, &off) || off.total != observed_len)
                continue;
              if (n >= max)
                return n;
              out[n++] = cand;
            }
  return n;
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

int nr_dci11_layout_enumerate_fdra(uint16_t riv_bits, uint8_t tda_bits, uint16_t observed_len,
                                   uint16_t bwp_start, uint16_t bwp_size, nr_dci11_layout_t *out, int max)
{
  int n = 0;
  for (int m = NR_FDRA_TYPE1; m <= NR_FDRA_DYN_CFG2 && n < max; m++) {
    const int k = nr_dci11_layout_enumerate_mode(riv_bits, tda_bits, observed_len, bwp_start, bwp_size, (uint8_t)m,
                                                 out + n, max - n);
    if (k < 0)
      return -1;
    n += k;
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

int nr_dci11_resolver_init_fdra(nr_dci11_resolver_t *r, uint16_t bwp_start, uint16_t bwp_size, uint16_t riv_bits,
                                uint8_t tda_bits, uint16_t observed_len)
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
  r->fdra_next = 1;   /* stage index: type 1 now, the others only via nr_dci11_resolver_arm_next_mode() */
  /* NR_DCI11_TDA_UNKNOWN: the TDRA list size is itself an RRC switch the receiver cannot read (the
   * dedicated pdsch-TimeDomainAllocationList travels ciphered, and SIB1's common list is only a
   * hypothesis about it). Enumerate every width 0..4 bits; each hypothesis carries its own
   * tda_bits in its offsets and the length constraint prunes most combinations. OTA 2026-09-15:
   * arming with the assumed 4-bit default on a 1-bit cell left no true layout enumerable at all. */
  const uint8_t tb_lo = (tda_bits == NR_DCI11_TDA_UNKNOWN) ? 0 : tda_bits;
  const uint8_t tb_hi = (tda_bits == NR_DCI11_TDA_UNKNOWN) ? 4 : tda_bits;
  int n = 0;
  for (uint8_t tb = tb_lo; tb <= tb_hi && n < NR_DCI11_LAYOUT_MAX; tb++) {
    const int m = nr_dci11_layout_enumerate(riv_bits, tb, observed_len, r->hyp + n, NR_DCI11_LAYOUT_MAX - n);
    for (int i = n; i < n + m; i++) {
      if (!nr_dci11_layout_offsets(&r->hyp[i], riv_bits, tb, &r->off[i])) {
        return 0;
      }
      r->alive[i] = true;
    }
    n += (m > 0) ? m : 0;
  }
  r->n_hyp = n;
  r->n_alive = n;
  /* No type-1 layout fits the length at all: "every type-1 layout refuted" holds vacuously, so the other
   * modes are armed at once (a narrow type-0 FDRA can make a DCI shorter than any type-1 one). */
  while (r->n_hyp == 0 && nr_dci11_resolver_arm_next_mode(r, NULL) >= 0) {
  }
  return r->n_hyp;
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

bool nr_dci11_resolver_all_refuted(const nr_dci11_resolver_t *r, uint32_t min_trials)
{
  if (r == NULL || r->n_alive <= 0 || r->winner >= 0)
    return false;
  /* AGGREGATE rule: no live layout has a pass, and the live set has absorbed min_trials trials per live
   * layout IN TOTAL. A per-layout floor never fires when some layout cannot receive trials at all -- its
   * reads are lost before enqueue (the stage-2 size check / extractor rejects them, which the true layout
   * does not suffer) or it is never selected -- and one such layout used to freeze staging forever.
   * The total keeps the same evidence budget; zero passes anywhere over it is the refutation. */
  uint64_t tr = 0;
  const int nh = __atomic_load_n(&r->n_hyp, __ATOMIC_ACQUIRE);
  for (int i = 0; i < nh; i++) {
    if (!r->alive[i])
      continue;
    if (layout_has_pass(r, i))
      return false;
    tr += r->trials[i] > r->probe_tr[i] ? r->trials[i] : r->probe_tr[i];
  }
  return tr >= (uint64_t)min_trials * (uint64_t)r->n_alive;
}

int nr_dci11_fdra_stage(uint8_t fdra_mode)
{
  static const int8_t stage[5] = {0, 1, 4, 2, 3}; /* see kArmOrder */
  return fdra_mode <= NR_FDRA_DYN_CFG2 ? stage[fdra_mode] : -1;
}

int nr_dci11_resolver_disarm(nr_dci11_resolver_t *r, int type1_idx)
{
  if (r == NULL)
    return 0;
  const int nh = __atomic_load_n(&r->n_hyp, __ATOMIC_ACQUIRE);
  int killed = 0;
  for (int i = 0; i < nh; i++) {
    if (r->alive[i] && r->off[i].fdra_mode != NR_FDRA_TYPE1) {
      r->alive[i] = false;
      r->n_alive--;
      killed++;
    }
    r->retired[i] = false; /* type 1 is proven: nothing retired is revivable any more */
  }
  if (type1_idx >= 0 && type1_idx < nh && r->off[type1_idx].fdra_mode == NR_FDRA_TYPE1 && !r->alive[type1_idx]) {
    r->alive[type1_idx] = true; /* killed when its stage was refuted; its own pass proves it */
    r->n_alive++;
  }
  r->fdra_next = NR_DCI11_FDRA_STAGES; /* type 1 is proven: never arm again */
  return killed;
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

/* Arming order after type 1. dynamicSwitch goes BEFORE type 0 config 2: config 2's narrow FDRA admits
 * the bulk of the switch space (up to 5458 layouts at one length), and armed first it used to fill the
 * cap before dynamicSwitch got a slot (106 PRB at 48-49 bits: dynamicSwitch 0 of 1329). */
static const uint8_t kArmOrder[NR_DCI11_FDRA_STAGES] = {NR_FDRA_TYPE1, NR_FDRA_TYPE0_CFG1, NR_FDRA_DYN_CFG1,
                                                        NR_FDRA_DYN_CFG2, NR_FDRA_TYPE0_CFG2};

int nr_dci11_resolver_arm_next_mode(nr_dci11_resolver_t *r, int *added)
{
  if (added)
    *added = 0;
  if (r == NULL || r->observed_len == 0 || r->riv_bits == 0)
    return -1;
  const int nh = __atomic_load_n(&r->n_hyp, __ATOMIC_ACQUIRE);
  /* Arm the next stage that fits (APPENDED at [nh, n_hyp), alive, fresh counters). */
  static nr_dci11_layout_t hyp[NR_DCI11_LAYOUT_MAX];   /* arming is rare and single-threaded (the observer) */
  static nr_dci11_offsets_t off[NR_DCI11_LAYOUT_MAX];
  const uint8_t tb_lo = (r->tda_bits == NR_DCI11_TDA_UNKNOWN) ? 0 : r->tda_bits;
  const uint8_t tb_hi = (r->tda_bits == NR_DCI11_TDA_UNKNOWN) ? 4 : r->tda_bits;
  int armed = -1, k = 0;
  while (armed < 0 && r->fdra_next < NR_DCI11_FDRA_STAGES) {
    const uint8_t m = kArmOrder[r->fdra_next++];
    int n = 0;
    for (uint8_t tb = tb_lo; tb <= tb_hi && n < NR_DCI11_LAYOUT_MAX; tb++) {
      const int e = nr_dci11_layout_enumerate_mode(r->riv_bits, tb, r->observed_len, r->bwp_start, r->bwp_size, m,
                                                   hyp + n, NR_DCI11_LAYOUT_MAX - n);
      for (int i = n; i < n + (e > 0 ? e : 0); i++)
        nr_dci11_layout_offsets(&hyp[i], r->riv_bits, tb, &off[i]);
      n += (e > 0) ? e : 0;
    }
    if (n == 0)
      continue;   /* this mode does not fit the length (or is a duplicate here): try the next */
    k = append_offsets(r, hyp, off, n);
    armed = m;
  }
  if (added)
    *added = k;
  /* The live set [0, nh) was refuted by TB CRC (the caller checked nr_dci11_resolver_all_refuted).
   *  - Nothing retired yet: retire it, so n_alive -- and stage 1's per-payload and pruning cost -- stays one
   *    stage's size (a layout with any pass is kept; nr_dci11_resolver_disarm() revives a type-1 layout whose
   *    late pass proves it). Only when the new stage actually added layouts: retiring with nothing to replace
   *    it would empty the set, and an empty set is never refuted again, i.e. never revived.
   *  - Layouts already retired: a later stage has been refuted as well, so the cause is not the FDRA mode.
   *    Revive them (fresh counters, so the aggregate rule needs new evidence) instead of retiring more. */
  bool have_retired = false;
  for (int i = 0; i < nh && !have_retired; i++)
    have_retired = r->retired[i];
  r->last_revived = 0;
  for (int i = 0; i < nh; i++) {
    if (have_retired && r->retired[i]) {
      r->retired[i] = false;
      r->trials[i] = r->ok[i] = r->probe_tr[i] = r->probe_ok[i] = 0;
      r->alive[i] = true;
      r->n_alive++;
      r->last_revived++;
    } else if (!have_retired && k > 0 && r->alive[i] && !layout_has_pass(r, i)) {
      r->alive[i] = false;
      r->retired[i] = true;
      r->n_alive--;
    }
  }
  return armed;
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

/* Distributional pruning. Constants chosen so the test is DECISIVE, not merely significant, before
 * it may delete anything: at the gap below, 2000 payloads is a 1000-bit likelihood ratio.
 * KEEP_MIN is the load-bearing safety: layouts that differ only in bits the true layout also reads
 * as constant (e.g. pre_ant vs post_ant around a constant pdsch-to-harq field) are GENUINELY tied on
 * payload statistics -- only a TB CRC can split them -- so the top few are never removed here, and
 * the model error "the truth is not the most compressible" can at worst demote it, never delete it. */
#define DCI11_S1_DIST_MIN   2000
#define DCI11_S1_DIST_EVERY 500
#define DCI11_S1_DIST_GAP   0.5    /* bits per payload behind the best */
#define DCI11_S1_KEEP_MIN   4

static void hist_field(const uint32_t *h, int k, uint32_t n, double *bits)
{
  if (k <= 1 || n == 0)
    return;
  double H = 0.0;
  int m = 0;
  for (int v = 0; v < k; v++) {
    if (h[v] == 0)
      continue;
    const double p = (double)h[v] / (double)n;
    H -= p * log2(p);
    m++;
  }
  H += (double)(m - 1) / (2.0 * (double)n * M_LN2); /* Miller-Madow: plug-in H is biased low */
  *bits += (double)n * (log2((double)k) - H);
}

double nr_dci11_resolver_score(const nr_dci11_resolver_t *r, int i)
{
  if (r == NULL || i < 0 || i >= r->n_hyp)
    return 0.0;
  const nr_dci11_offsets_t *o = &r->off[i];
  const uint32_t n = r->seen[i];
  double bits = 0.0;
  hist_field(&r->hist[i][0], 32, n, &bits);
  hist_field(&r->hist[i][32], 4, n, &bits);
  if (o->tda_bits > 0 && o->tda_bits <= 4)
    hist_field(&r->hist[i][36], 1 << o->tda_bits, n, &bits);
  const int apb = (o->ant_ports_bits > 6) ? 6 : o->ant_ports_bits;
  if (apb > 0)
    hist_field(&r->hist[i][52], 1 << apb, n, &bits);
  return bits;
}

void nr_dci11_resolver_set_tda_count(nr_dci11_resolver_t *r, uint8_t tda_count)
{
  if (r == NULL)
    return;
  r->tda_count = tda_count;   /* modes armed later get the same impossible-index test */
  for (int i = 0; i < r->n_hyp; i++)
    r->off[i].tda_valid = (tda_count > 0 && r->off[i].tda_bits > 0 && tda_count < (1u << r->off[i].tda_bits))
                              ? tda_count : 0;
}

static void hist_observe(nr_dci11_resolver_t *r, int i, nr_dci_bits_t payload)
{
  const nr_dci11_offsets_t *o = &r->off[i];
  r->hist[i][peek(payload, o->total, o->mcs, DCI11_MCS_BITS) & 31]++;
  r->hist[i][32 + (peek(payload, o->total, o->rv, DCI11_RV_BITS) & 3)]++;
  if (o->tda_bits > 0 && o->tda_bits <= 4)
    r->hist[i][36 + (peek(payload, o->total, o->tda, o->tda_bits) & 15)]++;
  const int apb = (o->ant_ports_bits > 6) ? 6 : o->ant_ports_bits;
  if (apb > 0)
    r->hist[i][52 + (peek(payload, o->total, o->ant_ports, (uint8_t)apb) & 63)]++;
}

static int cmp_desc(const void *a, const void *b)
{
  const double x = *(const double *)a, y = *(const double *)b;
  return (x < y) - (x > y);
}

static void prune_by_distribution(nr_dci11_resolver_t *r)
{
  static double score[NR_DCI11_LAYOUT_MAX];
  double best = -1e300;
  for (int i = 0; i < r->n_hyp; i++) {
    if (!r->alive[i])
      continue;
    score[i] = nr_dci11_resolver_score(r, i);
    if (score[i] > best)
      best = score[i];
  }
  /* Ranks by sort + binary search, not the O(n_alive^2) double loop (20 ms at 4.3k live). */
  static double sorted[NR_DCI11_LAYOUT_MAX];
  int ns = 0;
  for (int i = 0; i < r->n_hyp; i++)
    if (r->alive[i])
      sorted[ns++] = score[i];
  qsort(sorted, (size_t)ns, sizeof(sorted[0]), cmp_desc);
  for (int i = 0; i < r->n_hyp && r->n_alive > DCI11_S1_KEEP_MIN; i++) {
    if (!r->alive[i] || r->seen[i] < DCI11_S1_DIST_MIN)
      continue;
    int lo = 0, hi = ns; /* first index whose score is <= score[i]: that many are strictly better */
    while (lo < hi) {
      const int mid = (lo + hi) / 2;
      if (sorted[mid] > score[i]) lo = mid + 1; else hi = mid;
    }
    const int better = lo;
    /* OTA 2026-09-15 (v2l): the configured layout, which decodes at 72 % by TB CRC, was NOT in the
     * top 4 by this score -- a misaligned layout that reads constant RIV bits as MCS/RV/AP is MORE
     * compressible than the truth. So the score only RANKS (Thompson prior in the RT monitor);
     * it never deletes. dropped_dist now counts what it WOULD have dropped, for the report. */
    if (better >= DCI11_S1_KEEP_MIN && best - score[i] > DCI11_S1_DIST_GAP * (double)r->seen[i])
      r->dropped_dist++;
  }
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

int nr_dci11_resolver_observe(nr_dci11_resolver_t *r, nr_dci_bits_t payload)
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
    hist_observe(r, i, payload);
    if (nr_dci11_layout_plausible(&r->off[i], payload, r->bwp_size)) {
      r->pass[i]++;
    }
  }
  r->n_obs++;
  /* ISAC_DCI11_S1_OFF=1: keep every layout alive (probes rotate over the whole set). A/B knob for
   * "did stage 1 delete the true layout" -- the one question a 0 % run cannot answer otherwise. */
  static int s_s1_off = -1;
  if (s_s1_off < 0)
    s_s1_off = (getenv("ISAC_DCI11_S1_OFF") != NULL && atoi(getenv("ISAC_DCI11_S1_OFF")) != 0) ? 1 : 0;
  if (s_s1_off)
    return r->n_alive;
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
  if (r->n_obs >= DCI11_S1_DIST_MIN && (r->n_obs % DCI11_S1_DIST_EVERY) == 0)
    prune_by_distribution(r);
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
  out->dmrs_config_type = l->dmrs_type;
  out->tci_bits           = l->post_ant;
  out->srs_request_bits   = 0;
  out->cbg_bits           = 0;
  out->fdra_mode          = l->fdra_mode;
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
  return riv == want.riv && tda == want.tda && mcs == want.mcs && rv == want.rv
         && ap == want.ant_ports && di == want.dmrs_init && p == want.total;
}
