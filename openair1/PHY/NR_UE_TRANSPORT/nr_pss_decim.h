/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/* nr_pss_decim: decimated floating-point PSS correlator + multi-capture non-coherent fold (ISAC_PSS_DECIM,
 * ISAC_PSS_ACC). Pure C, no OAI dependency (unit-tested by test_nr_pss_decim).
 *
 * Why (2026-10-11, WEAKSYNC): the stock full-rate search runs 4096-tap int16 dot products at every 4th sample of
 * 2 frames x 3 N_ID_2 x 4 RX (~30 G MAC, ~17 s per attempt OTA at 217 PRB), and dot_product() shifts every
 * product pair right (floor) before accumulating, so a weak input (RMS ~20 counts) is dominated by quantisation
 * bias: the receiver saw PSS peak/avg 8 dB where a float correlator sees 40 dB on the same capture.
 * Here: mix the PSS centre bin to DC, CIC-3 low-pass, decimate by D = N/256, correlate in float with the
 * identically filtered reference (~240 M MAC per frame), then refine at full rate (float) around the peak. */
#ifndef NR_PSS_DECIM_H
#define NR_PSS_DECIM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  float r, i;
} nr_cf_t;

/* decimation factor for an N-point OFDM symbol: N/256 when >= 2, else 0 (no decimated path) */
int nr_pss_decim_factor(int N);
/* mix bin kc to DC, CIC-3 (boxcar^3, 3D-2 taps), keep every D-th output: out[m] = sum_t h[t] x[s0+m*D-t] e^{-j2pi kc n/N}.
 * Samples outside [0, xlen) are zero. out has nout entries. */
void nr_pss_decimate(const int16_t *x_iq, int xlen, int s0, int nout, int D, int kc, int N, nr_cf_t *out);
/* reference (N samples, int16 iq; its IDFT DC bias and stray components are removed by bandlimiting to the 127 PSS bins
 * centred on bin kc) through the same chain; returns its length L = N/D + 3 */
int nr_pss_decim_ref(const int16_t *ref_iq, int N, int D, int kc, nr_cf_t *out);
/* prof[m] += |sum_j conj(rd[j]) xd[m+j]|^2 for m in [0, npos); xd must hold npos + L - 1 samples */
void nr_pss_corr_acc(const nr_cf_t *xd, int npos, const nr_cf_t *rd, int L, float *prof);
/* full-rate float correlation sum_k conj(ref[k]) x[k] (re, im) */
void nr_pss_cdot(const int16_t *ref_iq, const int16_t *x_iq, int n, double *re, double *im);

/* Non-coherent fold of decimated PSS profiles over captures, modulo `period` decimated samples (the 20 ms SSB grid),
 * indexed by ABSOLUTE decimated sample time. acc = acc*(1-1/K) + new. Reset when the key (centre bin, N) changes. */
typedef struct {
  float *acc[3];
  int period;
  int key;
  int K;
  int64_t last_abs; /* last absolute decimated index folded; a step backwards (stream restart) resets */
  int captures;
} nr_pss_fold_t;
void nr_pss_fold_reset(nr_pss_fold_t *f);
/* fold prof[0..n) at absolute decimated index a0 for sequence s; returns the folded values in place in prof */
void nr_pss_fold_update(nr_pss_fold_t *f, int s, int64_t a0, float *prof, int n, int period, int K, int key);

#ifdef __cplusplus
}
#endif
#endif
