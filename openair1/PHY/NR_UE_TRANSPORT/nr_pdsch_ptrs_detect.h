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

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_ptrs_detect.h
 * \brief Blind PT-RS presence / density detector and its per-(RNTI, MCS band, N_RB band) evidence.
 *
 * WHY. The passive PDU builders hard-code pduBitmap = 0, so a PT-RS grant (TS 38.214 5.1.6.3: present iff
 * MCS >= ptrs-MCS1 and N_RB >= N_RB0, both dedicated RRC, invisible here) is decoded with G too large by
 * the PT-RS REs and every code block after the first PT-RS symbol fails. PT-RS appears exactly on the
 * big, high-MCS grants. The density is a small discrete set -- K in {2,4}, L in {1,2,4}, subcarrier
 * offset k_RE_ref in 0..11 -- so it is detected from the signal instead of configured.
 *
 * HOW (no reference sequence, no channel estimate). A PT-RS RE carries the SAME QPSK value in every PT-RS
 * symbol of the grant (TS 38.211 7.4.1.2.2: the DM-RS value of the associated port), times a per-symbol
 * common phase error. For two PT-RS symbols a, b the product r_a(k) * conj(r_b(k)) of the received FFT
 * outputs on a PT-RS subcarrier is |H(k)|^2 * |p|^2 * exp(j(phi_a - phi_b)): the channel H and the
 * sequence p cancel, the phase is common to every PT-RS RE and every receive antenna. On a data RE it is
 * x_a conj(x_b) |H|^2, zero-mean. So, for each hypothesis (K, k_RE_ref, L), the sum over the PT-RS subcarriers of the
 * antenna-summed products P(k), normalised by the sum of the |P(k)|^2, is Exp(1)
 * distributed with no PT-RS and grows with the number of terms with PT-RS. Summed over the
 * consecutive PT-RS symbol pairs of the hypothesis it is Gamma(pairs, 1) under no PT-RS; the tail
 * probability, with the multiplicity of the hypotheses, is the detection statistic.
 * Independent of the DM-RS port, the sequence convention, the scrambling identity and the channel
 * estimator -- and therefore the same on the CPU and GPU paths (it reads the transformed slot).
 *
 * PRESENCE depends on MCS and N_RB, so the evidence is accumulated per class (nr_ptrs_class_t) and the
 * transport-block CRC arbitrates between "decoded without PT-RS" and "decoded with the detected PT-RS".
 */

#ifndef __NR_PDSCH_PTRS_DETECT_H__
#define __NR_PDSCH_PTRS_DETECT_H__

#include <stdbool.h>
#include <stdint.h>

/* False-alarm level per grant after multiplicity. A vote needs >= 2 agreeing grants of its class and the CRC arbitrates after that. */
#define NR_PTRS_DET_ALPHA 1e-4
#define NR_PTRS_HYP_N 72  /* K in {2,4} x k_RE_ref 0..11 x L in {1,2,4}; index = ((K==4)*12 + k_re)*3 + log2(L) */

/// A PT-RS configuration as the PDU carries it (the fields nr_dlsch_demodulation.c and G read).
typedef struct {
  uint8_t K;     /* PTRSFreqDensity: 2 or 4 */
  uint8_t L;     /* time density in symbols: 1, 2 or 4 (PTRSTimeDensity = log2 L) */
  uint8_t k_re;  /* PTRSReOffset: PT-RS subcarrier offset k_RE_ref in the PRB, 0..11 */
} nr_ptrs_hyp_t;

typedef struct {
  bool fired;          /* the best hypothesis is significant */
  nr_ptrs_hyp_t hyp;   /* the best hypothesis (valid when fired) */
  float log10_p;       /* log10 of the multiplicity-corrected p-value of the best hypothesis (negative = significant) */
  uint8_t pairs;       /* PT-RS symbol pairs behind it */
} nr_ptrs_det_t;

int nr_ptrs_hyp_index(const nr_ptrs_hyp_t *h);
void nr_ptrs_hyp_from_index(int idx, nr_ptrs_hyp_t *h);

/** PT-RS symbol mask of an allocation, TS 38.214 5.1.6.3 (same rule as set_ptrs_symb_idx()). */
uint16_t nr_ptrs_symbol_mask(uint8_t start_symbol, uint8_t nb_symbols, uint8_t L, uint16_t dmrs_symb_pos);
/** 1 if allocation subcarrier `re` (0 = first RE of the first scheduled PRB) is a PT-RS RE, TS 38.211 7.4.1.2.2. */
bool nr_ptrs_is_re(uint16_t re, uint16_t rnti, uint8_t K, uint16_t nb_rb, uint8_t k_re);

/** Detect PT-RS in the transformed slot.
 *  rxdataF: `n_ant` rows of `ant_stride` samples (symbol m at m * symbol_sz); the allocation's first RE is
 *  subcarrier `re0` of the FFT output (wrapping modulo symbol_sz), contiguous over nb_rb PRBs.
 *  Symbols of `dmrs_symb_pos` and symbols outside the allocation are not used. Pure; ~20 us per 100 PRB, 4 RX.
 *  `alpha` is the false-alarm level per grant after multiplicity (NR_PTRS_DET_ALPHA). */
void nr_ptrs_detect(const int16_t *rxdataF, int n_ant, size_t ant_stride, int symbol_sz, int re0, int nb_rb,
                    uint16_t rnti, uint8_t start_symbol, uint8_t nb_symbols, uint16_t dmrs_symb_pos, double alpha,
                    nr_ptrs_det_t *out);

/** The number of REs PT-RS takes in the grant (TS 38.211 7.4.1.2.2, one port) -- nr_get_G()'s unav_res share. */
uint32_t nr_ptrs_hyp_unav_res(const nr_ptrs_hyp_t *h, uint16_t nb_rb, uint8_t start_symbol, uint8_t nb_symbols,
                              uint16_t dmrs_symb_pos);

/* ---- per-(RNTI, MCS band, N_RB band) evidence ------------------------------------------------------ */
#define NR_PTRS_MCS_BANDS 5
#define NR_PTRS_NRB_BANDS 8
#define NR_PTRS_CLASS_N (NR_PTRS_MCS_BANDS * NR_PTRS_NRB_BANDS)
int nr_ptrs_class_index(unsigned mcs, unsigned nb_rb);   /* log-spaced N_RB bands, MCS/6 bands */

typedef struct {
  uint8_t votes[NR_PTRS_HYP_N];  /* detector hits per hypothesis (saturating) */
  uint16_t det_n, det_hit;       /* detector runs / runs that fired */
  uint16_t det_since_hit;        /* runs since the last hit, for the back-off */
  uint16_t ok[3], tr[3];         /* CRC arbitration, by mode: [0] without PT-RS, [1] PT-RS excluded + PT-RS phase (CPE), [2] PT-RS excluded only */
  int8_t latched;                /* -1 undecided, 0 no PT-RS in this class, 1 PT-RS in this class */
} nr_ptrs_class_t;

void nr_ptrs_class_init(nr_ptrs_class_t *c);
/** Should a failed grant of this class run the detector? Undecided: yes. Latched absent: 1 in 16 (the detector is
 *  the only way back). Latched present: no (the hypothesis is applied from the first body). */
bool nr_ptrs_class_should_detect(const nr_ptrs_class_t *c);
void nr_ptrs_class_det_feed(nr_ptrs_class_t *c, const nr_ptrs_det_t *d);
/** The class's PT-RS configuration: the modal detector hypothesis (needs >= 2 votes and a plurality over the
 *  rivals), or the latched one. False when the class has none. */
bool nr_ptrs_class_hyp(const nr_ptrs_class_t *c, nr_ptrs_hyp_t *h);
/** Which PT-RS mode to decode with next: 1 = RE exclusion + PT-RS CPE, 2 = RE exclusion only (the PT-RS CPE of
 *  nr_pdsch_ptrs_processing() correlates against a reference sequence that a cell may not follow; mode 2 leaves the phase
 *  to the other estimators). The mode with the best Wilson upper bound, so both get looks and CRC picks. */
int nr_ptrs_class_pick_mode(const nr_ptrs_class_t *c);
/** The latched class's mode: the one with the higher decode rate (1 on a tie). */
int nr_ptrs_class_best_mode(const nr_ptrs_class_t *c);
/** Record the TB CRC of a decode made in `mode` (0 = without PT-RS, 1, 2 as above); may latch. */
void nr_ptrs_class_crc_feed(nr_ptrs_class_t *c, int mode, bool tb_ok);
#endif
