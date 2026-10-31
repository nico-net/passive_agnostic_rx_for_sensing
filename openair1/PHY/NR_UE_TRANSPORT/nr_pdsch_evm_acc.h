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
  /* XDIAG (GPU demod, k_tail sums folded by nr_pdsch_gpu_demod_host.c): decision-directed error of the equaliser output the LLRs are cut from, per layer. Lattice mode:
   * xd_e = sum |z/u - slice|^2, xd_n = REs; moment mode (QPSK, multi-layer): xd_nm REs, xd_p sum |z|^2, xd_s sum |Re z| + |Im z|; xd_ge / xd_gn: layer 0 per group of 48 packed REs */
  int xd_qm;
  double xd_e[4], xd_n[4], xd_nm[4], xd_p[4], xd_s[4];
  float xd_ge[72], xd_gn[72];
  /* PHASE-TRACK (nr_pdsch_phase_track.h): per (data symbol, layer) the CPE the tracker applied: sum of |phi| in rad (0 for not applied), count of (symbol, layer) pairs evaluated, count rotated */
  double cpe_abs;
  uint32_t cpe_n, cpe_app;
} nr_pdsch_evm_acc_t;
extern __thread nr_pdsch_evm_acc_t nr_pdsch_evm_acc;
/* RESID (nr_slotphase_resid.h): the position-keyed layer-0 decision-directed error of the GPU equaliser output (k_tail, nr_gpu_xdiag_t rs_e / rs_n), per symbol, of the decode(s) this
 * thread ran since the queue worker cleared it. `want` is set by the worker (nr_slotphase_resid_want) only for decodes of a flagged slot phase and for a bounded reference budget; the
 * GPU demod then fills `e` / `n` (summed over the decode's demod calls) and sets `valid`. Keys: see NR_GPU_RS_KEYS in nr_pdsch_gpu_demod.h (48 position keys, then 36 buckets of 8 CRBs). */
#define NR_RESID_NSYM 14
#define NR_RESID_KEYS 48
#define NR_RESID_BKT 36
#define NR_RESID_N (NR_RESID_KEYS + NR_RESID_BKT)
typedef struct {
  int want, valid;
  float e[NR_RESID_NSYM][NR_RESID_N];
  uint32_t n[NR_RESID_NSYM][NR_RESID_N];
} nr_pdsch_resid_t;
extern __thread nr_pdsch_resid_t nr_pdsch_resid;
/* LLRZERO ISAC_MMSE2_DIAG=1: per-grant, per-layer statistics of nr_dlsch_mmse mode 2 (CPU path only). Zeroed per decode, printed as MMSE2DIAG. */
typedef struct {
  int on, shift, nl, qm, gain_k;
  uint32_t n[4], w0[4], z0[4], mag0[4], sing[4], sat[4];
  double sumw[4], sumabsz[4];
} nr_mmse2_diag_t;
extern __thread nr_mmse2_diag_t nr_mmse2_diag;
#endif
