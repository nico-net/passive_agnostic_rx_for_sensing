/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* MPCI M1: per-cell digital down-converter: frequency shift (mixer) + decimation by 1, 2, 4, 8 or 16 (cascade of Kaiser
 * halfband stages, polyphase so every inner loop is contiguous). Input/output sc16, internal float. */
#ifndef IQREF_H
#define IQREF_H
#include <stdint.h>

#define IQREF_MAX_STAGES 4
#define IQREF_ROT_BLOCK 4096

typedef struct {
  int ntaps;           /* odd, (ntaps-1)/2 odd */
  int npairs;          /* nonzero symmetric tap pairs */
  float *pair;         /* pair[k] = h[c + (2k+1)] = h[c - (2k+1)] */
  float *hr, *hi;      /* history: ntaps-1 complex input samples (split re/im) */
} iqref_stage_t;

typedef struct {
  int decim;           /* 1, 2, 4, 8 or 16 */
  int nstages;
  iqref_stage_t st[IQREF_MAX_STAGES];
  double rate_in, shift_hz, phase; /* mixer: multiply by exp(-j 2 pi shift_hz t) */
  float *rot;          /* exp(-j step n), n < IQREF_ROT_BLOCK, interleaved */
  float *br, *bi, *er, *ei, *orr, *oi; /* scratch */
  int cap;
  uint64_t clipped;
} iqref_t;

/* passband_hz: one-sided bandwidth that must be preserved (e.g. half the channel bandwidth); stopband >= atten_db. */
int iqref_init(iqref_t *d, double rate_in, int decim, double passband_hz, double atten_db);
void iqref_free(iqref_t *d);
void iqref_set_shift(iqref_t *d, double shift_hz);
/* Forget the filter history and mixer phase (call after a discontinuity of the input). */
void iqref_reset(iqref_t *d);
/* n_in must be a multiple of decim; writes n_in/decim samples to out. Returns the number written. */
int iqref_process(iqref_t *d, const int16_t *in, int n_in, int16_t *out);
/* Halfband taps for a stage (exported for the unit test). */
int iqref_halfband_len(double fp, double atten_db);
#endif
