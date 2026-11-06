#ifndef NR_SENSE_QUALITY_H
#define NR_SENSE_QUALITY_H
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
#define NR_SENSE_MAX_PRB 275
/* Provisional (spec §9.1): set only from general principles or sens6 lab references, never from DEIB/OTA cells. */
#define NR_SENSE_PRB_SNR_MIN_DB 3.0f
#define NR_SENSE_PRB_RESID_MAX 0.25f
/* Spec 4(d), data-aided only: a PRB fails the reconstruction check when its symbol agreement is SIGNIFICANTLY BELOW the agreement expected from the
 * record's own SNR and Qm (one-sided exact binomial tail < alpha, nr_pdsch_da_recon.h). alpha is a false-reject bound, not a calibrated level;
 * there is no absolute agreement threshold. */
#define NR_SENSE_PRB_RECON_ALPHA 1e-3
/* Floor on the expected symbol DISagreement (expected agreement <= 1 - 1e-3): a general implementation-loss bound (transmitter EVM, phase noise,
 * ADC quantisation and interference put an effective SNR ceiling of ~30 dB on any receiver; 1e-3 is the order of the symbol error rate that
 * ceiling gives for the densest constellations (~256QAM at 30 dB; QPSK and 16QAM are far below it, so there the floor is a conservative bound), NOT tuned on any cell. Without it a high-SNR PRB with one real mismatch would fail. */
#define NR_SENSE_RECON_IMPL_LOSS 1e-3
/* Why PRBs of a record are not valid, per rule, in evaluation order (a PRB is charged to the FIRST rule that rejects it). The first four are producer-side, set by nr_sense_quality_absent():
 * PRBs of the grant (nb_rb) that have no RE in the frame. TRUNC: the frame was cut at cfr_max_re; PARTIAL: data-aided partial TB (invalid REs left out); ABSENT: any other (e.g. REs not exported). */
enum { NR_SENSE_INV_TRUNC, NR_SENSE_INV_PARTIAL, NR_SENSE_INV_ABSENT, NR_SENSE_INV_FEW_RE, NR_SENSE_INV_NOSIG, NR_SENSE_INV_SNR, NR_SENSE_INV_RESID, NR_SENSE_INV_SYM, NR_SENSE_INV_RECON, NR_SENSE_INV_N };
const char *nr_sense_inv_name(int rule);
typedef struct {
  uint16_t inval[NR_SENSE_INV_N]; /* PRBs removed per rule (diagnostic only; no rule or threshold depends on it) */
  uint16_t allocated_prbs, estimated_usable_prbs, valid_cfr_prbs;
  uint8_t valid_mask[(NR_SENSE_MAX_PRB + 7) / 8]; /* bit p = PRB (k_abs/12 - first_prb) valid */
  int16_t first_prb;
  float snr_db_p50, snr_db_p10; /* over valid PRBs; NAN when none */
  float resid_db;               /* median per-PRB local-fit residual / signal, dB; NAN when none */
  float sym_consist;            /* 0..1 correlation of per-symbol estimates on shared k; NAN with < 2 symbols */
  float recon_conf;             /* data-aided only, set by the producer; NAN otherwise */
  uint8_t delay_step;           /* NR_SENSE_Q_UL_DELAY only: 0 = not requested, 1 = TA phase step estimated and removed, 2 = requested but not estimable (no adjacent REs): the fit saw H as is */
} nr_sense_quality_t;
/* noise_var: variance of the same quantity as H (post-estimation, per RE, same scaling); producers own that. <= 0 or NaN = the producer has none: the
 * PRB's own local-fit residual stands in. Either way the noise is floored at NR_SENSE_RECON_IMPL_LOSS x the PRB's mean power (30 dB SNR ceiling).
 * Per-PRB SNR = (mean|H|^2 over antennas and all REs - noise_var) / noise_var, -100 dB when the signal is <= 0.
 * The caller passes one estimate per (k, l); REs with l >= 16 or PRB >= 275 above the lowest are ignored in the fit.
 * Cost: O(n_re * n_ant), measured ~29 us for DM-RS-only input (128 PRB x 4 ant x 6 RE/PRB x 2 sym); a data-aided
 * full grid (~170k samples) is estimated at ~0.8-1.5 ms per record: S3 must measure it on the real input size.
 * iq antenna-major complex float pairs, ant_stride in complex RE (as nr_sense_push_cfr). Pure, no allocation,
 * O(n_ant * n_re). Every computation is per PRB: nothing fits/smooths/correlates across a PRG boundary. At most 8
 * antennas are used. */
/* Sparse sources (CSI-RS: 1 RE per PRB, one or few symbols; DM-RS of a precoded PDSCH/PUSCH is NOT sparse in this sense): a PRB is valid with >= 1 RE when the producer's noise estimate
 * (derived from the frame itself, three equally spaced PRBs' second difference; the producer's noise_var only stands when the frame has no such triple) gives SNR >= NR_SENSE_PRB_SNR_MIN_DB; no local fit, no residual (resid_db NAN). With >= 2 symbols sharing subcarriers the record's symbol consistency
 * must reach NR_SENSE_SPARSE_SYM_MIN, else the record has no valid PRB. */
#define NR_SENSE_Q_SPARSE 1u
/* UL source whose H is expressed in the DL frame (SRS, PUSCH DM-RS, PUSCH data): the common timing-advance phase ramp is removed before the per-PRB local fit (see nr_sense_quality.c). Dense sources only. */
/* ORDER ASSUMPTION: the REs of a symbol are consecutive and k ascending (symbol-major, as the SRS, PUSCH DM-RS and PUSCH data producers write them); a record in another order
 * gets no derotation (delay_step 2, counted in sense_ul_delay_skip) and is then judged by the plain rule: never more valid, only fewer. */
#define NR_SENSE_Q_UL_DELAY 2u
#define NR_SENSE_SPARSE_SYM_MIN 0.5f /* general bound (half the coherent amplitude), not tuned on any cell */
void nr_sense_quality_compute_ex(int n_ant, const float *iq, int ant_stride, int n_re, const uint16_t *k_abs,
                                 const uint8_t *l_sym, float noise_var, unsigned flags, nr_sense_quality_t *q);
void nr_sense_quality_compute(int n_ant, const float *iq, int ant_stride, int n_re, const uint16_t *k_abs,
                              const uint8_t *l_sym, float noise_var, nr_sense_quality_t *q);
/* Spec 4(d): AND the producer's per-PRB reconstruction pass mask (bit = ABSOLUTE PRB k/12, 1 = pass; a PRB without a verdict is 0) into
 * q->valid_mask, recount valid_cfr_prbs. snr/resid percentiles are left as computed over the PRBs that passed (a) and (b). */
/* Wide-record bookkeeping (writer, after recon): charges the grant's PRBs missing from the frame (nb_rb - allocated_prbs) to TRUNC / PARTIAL / ABSENT. Returns the PRBs removed in total. */
int nr_sense_quality_absent(nr_sense_quality_t *q, int nb_rb, bool truncated, bool partial_tb);
void nr_sense_quality_apply_recon(nr_sense_quality_t *q, const uint8_t recon_ok_abs[(NR_SENSE_MAX_PRB + 7) / 8]);
#ifdef __cplusplus
}
#endif
#endif
