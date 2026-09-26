#ifndef NR_PDSCH_QM_ORACLE_H
#define NR_PDSCH_QM_ORACLE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* MODULATION-ORDER ORACLE for Technique D's MCS-table hypothesis. The equalised PDSCH symbols
 * (rxdataF_comp) fit the ideal grid of their true order; for most MCS indices the three MCS tables
 * predict different orders, so the measured order prunes the table without TB-CRC trials.
 *
 * Raw EVM cannot compare grids: a denser grid always fits NOISE better. Each grid's per-dimension
 * squared residual (after RMS normalisation, which cancels the fixed-point scale as EQDIAG does) is
 * divided by 1/3, the value for input spread uniformly over the grid spacing: t << 1 on-grid, t ~ 1
 * for noise, t > 1 for a coarser constellation sitting between the grid's points. */

/** Modulation order of MCS index `mcs` in mcs_table 0 (64QAM), 1 (256QAM), 2 (64QAM LowSE):
 *  TS 38.214 Tables 5.1.3.1-1/-2/-3 including the reserved indices. 0 if out of range. */
int nr_pdsch_qm_of_mcs(uint8_t mcs, uint8_t mcs_table);

/** Qm in {2,4,6,8} whose grid fits `n` interleaved int16 (I,Q) symbols best, or 0 (abstain) unless
 *  the best t < NR_QM_ORACLE_T_MAX and t_best < NR_QM_ORACLE_RATIO * t_second, or n < NR_QM_ORACLE_MIN_N.
 *  *t_best (optional) receives the best normalised residual. */
int nr_pdsch_qm_classify(const int16_t *iq, uint32_t n, double *t_best);

#define NR_QM_ORACLE_MIN_N 256
#define NR_QM_ORACLE_T_MAX 0.5
#define NR_QM_ORACLE_RATIO 0.5

#ifdef __cplusplus
}
#endif
#endif
