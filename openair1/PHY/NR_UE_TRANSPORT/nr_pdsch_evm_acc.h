/* ISAC_PDSCH_EVM_GRANT (T26c): per-grant post-equalisation EVM and per-RX-branch SNR.
 *
 * nr_rx_pdsch() accumulates, per data symbol of a 1-layer grant, into the calling thread's
 * nr_pdsch_evm_acc when nr_pdsch_evm_acc.armed is set:
 *   - combined: x = sum_a conj(h_a) y_a / sum_a |h_a|^2 over the branches the receiver actually
 *     combines, RMS-normalised per symbol to the QAM grid of the grant's Qm, sliced to the nearest
 *     constellation point d; err += |x - d|^2, ref += ideal power (decision-directed EVM).
 *   - per branch a (every branch, combined or not): least-squares fit y_a = alpha_a h_a s + n_a with
 *     s = d / scale (the combined decision), accumulated per symbol as signal |c|^2/P and residual
 *     Y - |c|^2/P (c = sum y conj(h s), P = sum |h s|^2, Y = sum |y|^2). Scale-free, so the
 *     fixed-point shifts of y and h cancel.
 * The caller (nr_pdsch_passive_decode.c) zeroes and arms it before demodulation and reads it after.
 * Decision-directed: at EVM >~ 1/(2 sqrt(ideal grid)) decisions err and EVM is UNDER-estimated. */
#ifndef NR_PDSCH_EVM_ACC_H
#define NR_PDSCH_EVM_ACC_H
#include <stdint.h>
#define NR_PDSCH_EVM_MAX_RX 4
typedef struct {
  int armed;
  int qm;
  uint32_t n_re, n_sym;
  double err, ref;
  double err_cpe;
  double kappa_sum; uint32_t kappa_n;
  double alpha_abs[NR_PDSCH_EVM_MAX_RX]; uint32_t alpha_n[NR_PDSCH_EVM_MAX_RX]; /* per-branch |LS gain| (estimate bias) */
  double rho_sum; uint32_t rho_n;     /* mean |cross-branch residual correlation| */ /* ISAC_RX_QAM_AGC diagnostic: combined-amplitude / threshold ratio */ /* same decisions, after a per-symbol per-layer complex LS scale (common phase/amplitude) */
  double sig[NR_PDSCH_EVM_MAX_RX], res[NR_PDSCH_EVM_MAX_RX];
  int nbrx;
  /* QAM-FIX/low-MCS diagnostic: decision-directed error per OFDM symbol and per (symbol, data-RE index mod 12) */
  double err_sym[14], ref_sym[14];
  float err_k[14][12];
  uint16_t n_k[14][12];
  double err_layer[4], ref_layer[4]; /* per-layer decision-directed error */
  /* DECODE-RES: decision-directed error per allocation-relative RB on symbols 0, 1 and 3 (row 0/1/2), layer 0, no DM-RS on those symbols */
  float err_rb[3][276];
  uint16_t n_rb[3][276];
} nr_pdsch_evm_acc_t;
extern __thread nr_pdsch_evm_acc_t nr_pdsch_evm_acc;
#endif
