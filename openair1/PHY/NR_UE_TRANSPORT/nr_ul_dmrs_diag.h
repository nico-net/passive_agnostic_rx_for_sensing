/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_UL_DMRS_DIAG_H
#define NR_UL_DMRS_DIAG_H
/* UL_DMRS_DIAG: a diagnostic that separates "the receiver looks at the wrong place" from "the UE is too weak here".
 *
 * For ONE granted PUSCH it correlates the UL DM-RS the grant announces (the production generator, the pilot is the conjugate of the
 * transmitted sequence) on the claimed PRBs and on the rest of the UL BWP, and searches the places a wrong assumption would hide the
 * energy: the PRB offset, the symbol, the slot (+-1) and the timing (the window start, +-1.75 FFT sizes). Pure maths over
 * caller-supplied frequency-domain symbols (a FEP callback) and a pilot callback: no I/O, no state, no thread.
 *
 * Statistic. Per PRB, antenna and delay tau (a phase ramp over the PRB's DM-RS REs, N/128 samples apart, +-N/8) the coherent sum
 * C = |sum_j y_j p_j e^{-j 2 pi off_j tau / N}|^2, and S2 = sum_j |y_j p_j|^2. For a set of PRBs R = sum C / sum S2 (max over tau):
 * noise only gives R = 1 (0 dB), a clean pilot gives nre (6: 7.8 dB), and in between R = (nre g + 1) / (g + 1) with g the per-RE
 * SNR, so g = (R - 1) / (nre - R) is reported too. Every search is repeated with a control pilot (another scrambling id) over the same
 * hypotheses: its best R is what pure noise reaches after the same maximisation ("floor"). A hypothesis is evidence only above it.
 *
 * Sign convention of the timing results: the same as `ta=` of UL_PUSCH_CRC, a positive value starts the FFT window EARLIER. */
#include <stdbool.h>
#include <stdint.h>
#include "common/platform_types.h" /* c16_t */

#define NR_UL_DMRS_DIAG_MAX_ANT 4
#define NR_UL_DMRS_DIAG_MAX_RB 275
#define NR_UL_DMRS_DIAG_NTAU 33     /* fine delays -16..16 steps of N/128 (+-N/8) */
#define NR_UL_DMRS_DIAG_NCOARSE 15  /* window starts -7..7 steps of N/4 (+-1.75 N); with the fine grid they join without a gap */
#define NR_UL_DMRS_DIAG_MAX_SYM 14

typedef struct {
  int fft, nrb_carrier, first_carrier, nant, symbols_per_slot;
  int bwp_start, bwp_size;       /* UL BWP, carrier PRBs; size 0 = the whole carrier */
  int claim_start, claim_n;      /* the granted PRBs, carrier PRBs (bwp_start + start_rb) */
  int dmrs_sym;                  /* first DM-RS symbol of the grant */
  int grant_first_sym, grant_n_sym; uint16_t dmrs_mask; /* data symbols = the grant's symbols outside dmrs_mask */
  int nre, re_off[6];            /* DM-RS REs per PRB (6 type 1, 4 type 2) and their subcarrier offsets inside the PRB */
  /* The FFT of symbol `sym` of slot (target + slot_delta) with the window advanced by `adv` samples more than the decode used
   * (positive = earlier), one antenna, into dst[fft]. false = the samples are not available. */
  bool (*fep)(void *u, int ant, int slot_delta, int sym, int adv, c16_t *dst);
  /* The conjugate DM-RS pilot of every carrier PRB: out[nre * nrb_carrier] (index nre * prb + j). control = a different scrambling id.
   * NULL pilot (transform precoding): only the power figures are computed. */
  bool (*pilot)(void *u, int slot_delta, int sym, bool control, c16_t *out);
  void *u;
} nr_ul_dmrs_diag_in_t;

typedef struct {
  bool valid;      /* the nominal DM-RS symbol could be transformed */
  bool have_corr;  /* the correlation figures are valid (a pilot was given) */
  float e_claim_db, e_unclaim_db, e_floor_db, snr_claim_db; /* R in dB: claimed PRBs, the rest of the BWP, the control on the claimed PRBs; per-RE SNR of R */
  float pw_exc_db;  /* mean power per RE of the claimed PRBs over the grant's data symbols vs the rest of the BWP (sequence independent); NAN = no data symbol */
  int   best_t_off; float e_t_db, fl_t_db;                  /* timing only: claimed PRBs, best window start (relative to the decode's) */
  int   best_rb_off, best_rb_t_off; float e_rb_db, fl_rb_db; /* PRB offset x timing, a window of claim_n PRBs anywhere in the BWP */
  int   best_sym_off; float e_sym_db, fl_sym_db;             /* symbol: DM-RS generated for each symbol of the slot, claimed PRBs */
  int   best_slot_off; float e_slot_db, fl_slot_db;          /* slot -1, 0, +1 */
} nr_ul_dmrs_diag_out_t;

/* false: invalid input or the nominal symbol is unavailable (out->valid false). */
bool nr_ul_dmrs_diag_run(const nr_ul_dmrs_diag_in_t *in, nr_ul_dmrs_diag_out_t *out);

#endif
