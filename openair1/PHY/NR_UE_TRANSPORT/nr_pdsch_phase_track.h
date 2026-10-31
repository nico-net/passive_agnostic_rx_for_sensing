/* SPDX-License-Identifier: LicenseRef-CSSL-1.0
 * PHASE-TRACK (D1 + D2): channel time interpolation between DM-RS symbols and per-symbol common-phase-error (CPE) tracking of the PDSCH demodulator, for every layer count.
 *
 * Why (WEAKNESS_AUDIT D1/D2, PROJECT_MEMORY 36.5): nr_rx_pdsch() equalised each data symbol against the PREVIOUS DM-RS symbol's estimate, up to 8 symbols old with DM-RS on 2 and 11. A residual
 * carrier offset f_res rotates the channel by 2 pi f_res dt between estimate and data: 100 Hz is ~0.02 rad per 35.7 us symbol at 30 kHz SCS, ~0.17 rad (-15 dB EVM) at the end of an 8-symbol gap.
 * The branch alignment (MRC ALIGN) fixed this for one layer only; for MMSE a per-branch phase error is inter-layer leakage. The EVM floor of ~15 % flat across MCS (36.5) is where Qm 6/8 fail.
 *
 * Two mechanisms, both decision-free or CRC-neutral (a wrong estimate is gated out, never forced):
 *   1. nr_pdsch_phase_tinterp(): for a grant with >= 2 DM-RS symbols the estimate of every data symbol between two DM-RS symbols is the phase-aligned linear interpolation of the two (held
 *      outside the outermost pair). The pair's phase Delta is measured per RX branch and per block of 48 subcarriers from the estimates themselves (sum over layers of hb conj(ha)), so a
 *      branch-specific phase drift (independent LOs, the MRC ALIGN case) and a phase slope across frequency (residual timing drift) are interpolated too, for every Nl.
 *   2. nr_pdsch_cpe_symbol(): after the interpolation a data symbol still carries the phase noise and the frequency error the DM-RS pair could not see (outside the pair: all of it). Per data
 *      symbol and layer the ZF output x_l is formed per RE and the unbiased clipped 4th-power statistic sum (x_l^2 / P_l)^2 is accumulated (S = n E[s^4] e^{j4 phi}, E[s^4] < 0 for QPSK and
 *      square QAM; E[s^2] = 0 and circular noise make it unbiased at any SNR, and it needs no constellation scale, which the DM-RS power boost makes unknown). phi_l = arg(-S) / 4 is applied
 *      to the layer's channel column when coherent, significant (1 sigma, shrunk near the threshold) and capped at 0.5 rad. A layer without a significant estimate takes the estimate of all layers pooled.
 *      This is the blind variant of a decision-directed estimator: same model, no slicer, so it is immune to the unknown amplitude scale and to wrong decisions at high Qm.
 * CPU and GPU share one source for the arithmetic (nr_pdsch_phase_track_impl.h) and the CPU/GPU demod self-check stays exact.
 *
 * Counters: nr_pdsch_evm_acc.cpe_* (EVMGRANT cpe= cpes=) and the per-symbol EVM spread (EVMGRANT evsd=).
 */
#ifndef NR_PDSCH_PHASE_TRACK_H
#define NR_PDSCH_PHASE_TRACK_H
#include <stdint.h>
#include "common/platform_types.h"

#ifdef __cplusplus
extern "C" {
#endif
#define NR_PT_MAX_ROWS 16
#define NRPT_MIN_RE 48 /* fewest valid REs of a symbol the CPE statistic is formed from */

/* Time interpolation of the DM-RS channel estimate into the data symbols of the grant.
 * rows[r], r = layer * nrx + branch: the estimate row of ofdm_size * 14 entries, filled at the DM-RS symbols of `dmrs_mask`, allocation-relative columns [0, ncol).
 * Writes, for every non-DM-RS symbol m in [s0, s1), row[ofdm_size * m + 0 .. ncol): the interpolation (between two DM-RS symbols) or a copy of the nearest one before (the first one after
 * when none precedes). Returns 1 when at least one symbol was interpolated (the demodulator then reads the estimate at the data symbol itself: nr_dlsch_chest_per_symbol), 0 when the
 * grant has fewer than two DM-RS symbols (nothing written). */
int nr_pdsch_phase_tinterp(c16_t *const rows[], int nl, int nrx, int ofdm_size, int ncol, uint16_t dmrs_mask, int s0, int s1);

/* Per-symbol CPE of one data symbol: y[a] (a in [a0, a0 + nb)), h[l][a]: the extraction rows after the branch weighting and alignment, nre valid entries. Rotates the layer columns h[l][a]
 * (entries [0, nre)) by the estimated e^{j phi_l}. phi_out[l] (radians, 0 = not applied) for the counters. Returns the number of layers rotated. */
int nr_pdsch_cpe_symbol(int nl, int a0, int nb, uint32_t nre, c16_t *const y[4], c16_t *const h[4][4], double phi_out[4]);

/* The same, restricted to the per-block partial sums, for the unit test and the GPU twin: S/Q/n per layer from the chunk partials, in chunk order. */
typedef struct {
  double sr[4], si[4], q[4];
  uint32_t n[4];
  double pw[4];
  uint32_t npw[4];
} nr_pt_cpe_sums_t;
void nr_pdsch_cpe_sums(int nl, int a0, int nb, uint32_t nre, c16_t *const y[4], c16_t *const h[4][4], nr_pt_cpe_sums_t *s);

#ifdef __cplusplus
}
#endif
#endif
