/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_mimo_ls.h
 * \brief Data-aided per-layer channel estimate for a multi-layer PDSCH: windowed least squares on Y = H X + N.
 *
 * After a CRC-ok transport block the transmitted layer symbols X_l are known at every data RE. At Nl = 1 the CFR is the scalar
 * Y/X (nr_pdsch_data_aided.c). At Nl > 1 the receive sample is y_a = sum_l h_{a,l} x_l, so dividing by one layer leaves the others
 * in the "channel"; h_{a,l} is recovered by pooling W adjacent data REs of one symbol over which the channel is flat and solving
 * the Nl x Nl normal equations G h = X^H y, G = X^H X. Pure function of its arguments: no state, no allocation beyond the caller's
 * buffers, safe on any thread and for any cell.
 *
 * The window width is not a constant: it is the widest W on a doubling ladder (2*Nl, 4*Nl, ...) whose LS residual is still within
 * the statistical spread of the residual of the narrowest window (which sees a flat channel for certain), i.e. the channel is
 * flat over W REs to within the noise. It is therefore derived from the received signal itself (frequency selectivity of the cell's
 * channel as seen through the data), for any delay spread. A window is skipped when X^H X is ill-conditioned: some layer's
 * noise gain (G^-1)_ll * G_ll exceeds a numerical bound.
 */
#ifndef NR_PDSCH_MIMO_LS_H
#define NR_PDSCH_MIMO_LS_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define NR_MIMO_LS_MAX_NL 4
#define NR_MIMO_LS_MAX_ANT 8
#define NR_MIMO_LS_MAX_WIN 256 /* largest window the ladder may reach, in data REs */

/// Largest tolerated per-layer noise gain of the window's Gram matrix (1 = orthogonal layers); numerical, not cell-dependent.
#define NR_MIMO_LS_RHO_MAX 10.0

/**
 * One window. x: n x nl complex int16 (row-major: x[2*(i*nl+l)] = re, +1 = im), the layer symbols exactly as the modulator emits
 * them (i-th RE, layer l = modulation symbol i*nl+l, TS 38.211 7.3.1.3). y: nant planes of n complex int16 at `ystride` int16-complex
 * entries. h: out, h[2*(l*nant+a)] re / +1 im. resid: out, sum over REs and antennas of |y - H x|^2.
 * Returns 0, or -1 when the window is ill-conditioned (h and resid untouched).
 */
int nr_pdsch_mimo_ls_window(int n, int nl, int nant, const int16_t *x, const int16_t *y, size_t ystride, float *h, double *resid);

typedef struct {
  int win;          ///< window width chosen by the flatness ladder (data REs)
  int n_windows;    ///< output rows written
  int n_skipped;    ///< windows rejected as ill-conditioned at the chosen width
} nr_pdsch_mimo_ls_info_t;

/**
 * Whole grant. The nre data REs are in transmit order: x[nre*nl] as above, k_abs[nre]/sym[nre] the subcarrier (any monotone
 * numbering whose neighbours differ by 1..few) and OFDM symbol of each; y: nant planes of nre gathered samples at `ystride`.
 * A window never spans a symbol change or a gap in k larger than 6 (a PRB hole, never a DM-RS hole inside a PRB).
 * Outputs (capacity out_cap windows): out_h planes [l][a] of `out_stride` complex float each (out_h[2*((l*nant+a)*out_stride+w)]),
 * out_k[w] = k of the window's centre RE, out_sym[w], out_raw[w] = data REs pooled into the window.
 * Returns the number of windows written (0 when nothing is estimable); info may be NULL.
 */
int nr_pdsch_mimo_ls_cfr(int nre, int nl, int nant, const int16_t *x, const uint32_t *k_abs, const uint32_t *sym, const int16_t *y,
                         size_t ystride, int out_cap, size_t out_stride, float *out_h, uint32_t *out_k, uint32_t *out_sym,
                         uint16_t *out_raw, nr_pdsch_mimo_ls_info_t *info);

/// As nr_pdsch_mimo_ls_cfr, plus out_nv (NULL ok): planes [l] of out_stride floats, the per-RE noise variance of layer l's window estimate in H scale,
/// sigma_y^2 (G^-1)_ll with sigma_y^2 = LS residual / (nant (n - nl)); the quality tests use it as the noise reference of the window rows.
int nr_pdsch_mimo_ls_cfr_nv(int nre, int nl, int nant, const int16_t *x, const uint32_t *k_abs, const uint32_t *sym, const int16_t *y,
                            size_t ystride, int out_cap, size_t out_stride, float *out_h, uint32_t *out_k, uint32_t *out_sym,
                            uint16_t *out_raw, float *out_nv, nr_pdsch_mimo_ls_info_t *info);

#ifdef __cplusplus
}
#endif
#endif
