/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* MPCI M1: per-cell digital down-converter: frequency shift (mixer) + decimation by 1, 2 or 4 (cascade of Kaiser
 * halfband stages, polyphase so every inner loop is contiguous). Input/output sc16, internal float. */
#ifndef IQ_DDC_H
#define IQ_DDC_H
#include <stdint.h>

#define IQ_DDC_MAX_STAGES 2
#define IQ_DDC_ROT_BLOCK 4096

typedef struct {
  int ntaps;           /* odd, (ntaps-1)/2 odd */
  int npairs;          /* nonzero symmetric tap pairs */
  float *pair;         /* pair[k] = h[c + (2k+1)] = h[c - (2k+1)] */
  float *hr, *hi;      /* history: ntaps-1 complex input samples (split re/im) */
} iq_ddc_stage_t;

typedef struct {
  int decim;           /* 1, 2 or 4 */
  int nstages;
  iq_ddc_stage_t st[IQ_DDC_MAX_STAGES];
  double rate_in, shift_hz, phase; /* mixer: multiply by exp(-j 2 pi shift_hz t) */
  float *rot;          /* exp(-j step n), n < IQ_DDC_ROT_BLOCK, interleaved */
  float *br, *bi, *er, *ei, *orr, *oi; /* scratch */
  int cap;
  uint64_t clipped;
} iq_ddc_t;

/* passband_hz: one-sided bandwidth that must be preserved (e.g. half the channel bandwidth); stopband >= atten_db. */
int iq_ddc_init(iq_ddc_t *d, double rate_in, int decim, double passband_hz, double atten_db);
void iq_ddc_free(iq_ddc_t *d);
void iq_ddc_set_shift(iq_ddc_t *d, double shift_hz);
/* Forget the filter history and mixer phase (call after a discontinuity of the input). */
void iq_ddc_reset(iq_ddc_t *d);
/* n_in must be a multiple of decim; writes n_in/decim samples to out. Returns the number written. */
int iq_ddc_process(iq_ddc_t *d, const int16_t *in, int n_in, int16_t *out);
/* Halfband taps for a stage (exported for the unit test). */
int iq_ddc_halfband_len(double fp, double atten_db);
#endif
