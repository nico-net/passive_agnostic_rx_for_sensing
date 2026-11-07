#ifndef NR_SENSE_QUALITY_H
#define NR_SENSE_QUALITY_H
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#ifdef __cplusplus
extern "C" {
#endif
#define NR_SENSE_MAX_PRB 275
/* Spec 4(d), data-aided only: a PRB fails the reconstruction check when its symbol agreement is SIGNIFICANTLY BELOW the agreement expected from the
 * record's own SNR and Qm (one-sided exact binomial tail < alpha, nr_pdsch_da_recon.h). alpha is a false-reject bound, not a calibrated level;
 * there is no absolute agreement threshold. */
#define NR_SENSE_PRB_RECON_ALPHA 1e-3
/* Per-PRB validity tests (nr_sense_quality.c) have ONE false-reject level, the reconstruction check's: no threshold is set on any cell. SNR test: the PRB's power sum is
 * significantly above what noise alone gives (upper tail < alpha); residual test: the affine-fit residual is NOT significantly above what noise alone leaves (upper tail >= alpha). */
#define NR_SENSE_PRB_ALPHA NR_SENSE_PRB_RECON_ALPHA
/* Floor on the expected symbol DISagreement (expected agreement <= 1 - 1e-3): a general implementation-loss bound (transmitter EVM, phase noise,
 * ADC quantisation and interference put an effective SNR ceiling of ~30 dB on any receiver; 1e-3 is the order of the symbol error rate that
 * ceiling gives for the densest constellations (~256QAM at 30 dB; QPSK and 16QAM are far below it, so there the floor is a conservative bound), NOT tuned on any cell. Without it a high-SNR PRB with one real mismatch would fail. */
#define NR_SENSE_RECON_IMPL_LOSS 1e-3
/* Why PRBs of a record are not valid, per rule, in evaluation order (a PRB is charged to the FIRST rule that rejects it). The first four are producer-side, set by nr_sense_quality_absent():
 * PRBs of the grant (nb_rb) that have no RE in the frame. TRUNC: the frame was cut at cfr_max_re; PARTIAL: data-aided partial TB (invalid REs left out); ABSENT: any other (e.g. REs not exported). */
enum { NR_SENSE_INV_TRUNC, NR_SENSE_INV_PARTIAL, NR_SENSE_INV_ABSENT, NR_SENSE_INV_FEW_RE, NR_SENSE_INV_NOSIG, NR_SENSE_INV_SNR, NR_SENSE_INV_RESID, NR_SENSE_INV_SYM, NR_SENSE_INV_RECON, NR_SENSE_INV_N };
const char *nr_sense_inv_name(int rule);
/* DM-RS interpolation noise model, per antenna: {kT, thT, kR, thR, E[S]/s2} (Gamma shape and scale of the PRB power sum and of the affine-fit residual, units of the per-RE noise variance). Test hook. */
void nr_sense_quality_dmrs_model(int dmrs_type, double out[5]);
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
 * Cost: O(n_re * n_ant) + O(PRBs). Measured per record on the DGX (SenseQualityRound1.CostPerRecord, 4 ant): DM-RS chest 51/100/273 PRB 12/19/58 us; data-aided Nl=1 full grid (13 symbols)
 * 75/142/386 us; window rows (Nl>=2, ~15 rows per PRB) 12/19/48 us. A first sight of a new (shape, reference dof) costs one bisection (0.1-1 ms; saddlepoint kinds more), cached per thread.
 * iq antenna-major complex float pairs, ant_stride in complex RE (as nr_sense_push_cfr). Pure, no allocation,
 * O(n_ant * n_re). Every computation is per PRB: nothing fits/smooths/correlates across a PRG boundary. At most 8
 * antennas are used. */
/* Sparse sources (CSI-RS: 1 RE per PRB, one or few symbols; DM-RS of a precoded PDSCH/PUSCH is NOT sparse in this sense): a PRB is valid with >= 1 RE when the producer's noise estimate
 * (derived from the frame itself, three equally spaced PRBs' second difference; the producer's noise_var only stands when the frame has no such triple) passes the SNR test (NR_SENSE_PRB_ALPHA); no local fit, no residual (resid_db NAN). With >= 2 symbols sharing subcarriers the record's symbol consistency
 * must reach NR_SENSE_SPARSE_SYM_MIN, else the record has no valid PRB. */
#define NR_SENSE_Q_SPARSE 1u
/* UL source whose H is expressed in the DL frame (SRS, PUSCH DM-RS, PUSCH data): the common timing-advance phase ramp is removed before the per-PRB local fit (see nr_sense_quality.c). Dense sources only. */
/* ORDER ASSUMPTION: the REs of a symbol are consecutive and k ascending (symbol-major, as the SRS, PUSCH DM-RS and PUSCH data producers write them); a record in another order
 * gets no derotation (delay_step 2, counted in sense_ul_delay_skip) and is then judged by the plain rule: never more valid, only fewer. */
#define NR_SENSE_Q_UL_DELAY 2u
/* Decoder-estimate PDSCH DM-RS (src PDSCH_DMRS, comb-2 H of the linear-interpolation chest): its noise is correlated across the PRB's 6 REs and the producer's nvar (the LS-pair residual) is NOT in
 * H scale (3.3x the H noise for type 1, 1.75x for type 2), so the noise comes from the record's median fit residual and the chi-square degrees of freedom from the interpolation operator
 * (nr_sense_quality.c; DM-RS type 1 operator, the type is not carried by the frame). Set by the frame reader for that source only. */
#define NR_SENSE_Q_DMRS_INTERP 4u
/* NR_SENSE_Q_DMRS_T2: the DM-RS is type 2 (the type-2 operator; meta dmrs_type). NR_SENSE_Q_WINDOW: the rows are windowed-LS multi-layer data-aided estimates (one per LS window, no per-symbol fit): SNR test only,
 * noise_var = the producer's mean per-row noise variance of the layer (from the LS solve), eta (flags bits 16-23, /255) = (sum v)^2 / (R sum v^2) over its rows. NR_SENSE_Q_DATA_AIDED: H = Y/X per RE (per-RE noise
 * variance s2/|X|^2): effective dof N eta with eta from the constellation order in flags bits 8-11 (Qm/2; 0 = unknown -> 256QAM, the most conservative). */
#define NR_SENSE_Q_DMRS_T2 8u
#define NR_SENSE_Q_WINDOW 16u
#define NR_SENSE_Q_DATA_AIDED 32u
#define NR_SENSE_Q_QM(qm) ((unsigned)((qm) / 2) << 8)
#define NR_SENSE_Q_HET(eta) ((unsigned)lroundf(fminf(fmaxf((float)(eta), 0.f), 1.f) * 255.f) << 16)
/* Producer hint for the writer's quality (sensing mode only; carried in the in-memory frame's reserved header bytes 18-19, zeroed before the frame is written): bit0 DMRS type 2, bit1 window rows,
 * bits 2-5 Qm/2, bits 8-15 eta*255. Set per thread just before the push; consumed by it. */
void nr_sense_quality_hint(uint16_t hint);
double nr_sense_quality_qam_eta(int qm);
double nr_sense_quality_window_law_eta(double eta); /* test hook: effective eta of the discrete law behind the window threshold (<= the stated eta) */
double nr_sense_quality_dmrs_rho(int dmrs_type);    /* E[chest nvar] / H noise variance: 3.29 (type 1), 1.75 (type 2) */
void nr_sense_quality_gcrit_stats(uint64_t *calls, uint64_t *misses); /* test hook: this thread's threshold-cache lookups and misses */
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
