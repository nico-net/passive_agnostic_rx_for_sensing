#ifndef NR_SENSE_QUALITY_H
#define NR_SENSE_QUALITY_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define NR_SENSE_MAX_PRB 275
/* Provisional (spec §9.1): set only from general principles or sens6 lab references, never from DEIB/OTA cells. */
#define NR_SENSE_PRB_SNR_MIN_DB 3.0f
#define NR_SENSE_PRB_RESID_MAX 0.25f
typedef struct {
  uint16_t allocated_prbs, estimated_usable_prbs, valid_cfr_prbs;
  uint8_t valid_mask[(NR_SENSE_MAX_PRB + 7) / 8]; /* bit p = PRB (k_abs/12 - first_prb) valid */
  int16_t first_prb;
  float snr_db_p50, snr_db_p10; /* over valid PRBs; NAN when none */
  float resid_db;               /* median per-PRB local-fit residual / signal, dB; NAN when none */
  float sym_consist;            /* 0..1 correlation of per-symbol estimates on shared k; NAN with < 2 symbols */
  float recon_conf;             /* data-aided only, set by the producer; NAN otherwise */
} nr_sense_quality_t;
/* noise_var: variance of the same quantity as H (post-estimation, per RE, same scaling); producers own that.
 * Per-PRB SNR = (mean|H|^2 over antennas and all REs - noise_var) / noise_var, -100 dB when the signal is <= 0.
 * The caller passes one estimate per (k, l); REs with l >= 16 or PRB >= 275 above the lowest are ignored in the fit.
 * Cost: O(n_re * n_ant), measured ~29 us for DM-RS-only input (128 PRB x 4 ant x 6 RE/PRB x 2 sym); a data-aided
 * full grid (~170k samples) is estimated at ~0.8-1.5 ms per record: S3 must measure it on the real input size.
 * iq antenna-major complex float pairs, ant_stride in complex RE (as nr_sense_push_cfr). Pure, no allocation,
 * O(n_ant * n_re). Every computation is per PRB: nothing fits/smooths/correlates across a PRG boundary. At most 8
 * antennas are used. */
void nr_sense_quality_compute(int n_ant, const float *iq, int ant_stride, int n_re, const uint16_t *k_abs,
                              const uint8_t *l_sym, float noise_var, nr_sense_quality_t *q);
#ifdef __cplusplus
}
#endif
#endif
