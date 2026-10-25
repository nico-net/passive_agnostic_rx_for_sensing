/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Multi-layer PDSCH equaliser mode (nr_dlsch_mmse qam_mode) of the passive receiver: the one rule shared by the CPU path
 * (nr_rx_pdsch) and the GPU demod (nr_pdsch_gpu_demod_host.c refuse()), so the two cannot disagree. Pure header, unit-tested by
 * test_nr_llr_norm.
 *
 * Modes: 0 stock fixed-point MMSE, 1 fixed-point ZF, 2 float ZF on int16 G and H^H y (G = sum_a conj(h) h >> shift), 3 exact MMSE in
 * double straight from the int16 h and y with a level-independent output gain (nr_mmse_exact.h).
 *
 * RANK2-LEVEL 2026-10-25: two layers used mode 2 with log2_maxh - 3 (the NL2 delta, which gives the weak layer 8x the resolution).
 * Mode 2's int16 G and b = H^H y scale with the channel amplitude (shift only follows its square root), so the delta removed the
 * headroom: at a receive level 12 dB above the nr_dlsim default G and b hit the int16 rail (5 % of G, b clipped) and 2-layer 256QAM
 * BLER went from 0 to 0.85 where mode 3 stays at 0.02 [MEASURED, sens6 x86 nr_dlsim, TMP seam, PROJECT_MEMORY section 31 block
 * 2026-10-25]. Mode 3 needs a noise-only dimension for its decision-free noise estimate, so it takes two layers only when
 * nb_rx > nl (4 RX); nb_rx == nl keeps mode 2 + delta. */
#ifndef NR_QAM_MODE_H
#define NR_QAM_MODE_H

/* override: per-decode override (-1 none); base: mode without override (passive: 2); passive: passive RX build/mode. */
static inline int nr_multilayer_qam_mode(int nl, int nb_rx, int qm, int override, int base, int passive)
{
  int m = override >= 0 ? override : base;
  if (m < 0 || m > 3)
    m = 0;
  if (passive && override < 0 && (nl >= 3 || (nl == 2 && nb_rx > nl)))
    m = 3;
  if (nl >= 3 && (m == 1 || (qm == 2 && m != 3)))
    m = 0;
  return m;
}

/* NL2 delta on log2_maxh (ISAC_L2MAXH_DELTA_NL2 frozen -3): two layers on the int16 mode 2 only; mode 3 needs none. */
static inline int nr_l2maxh_delta(int nl, int qam_mode, int passive)
{
  return (nl == 2 && qam_mode != 3 && passive) ? -3 : 0;
}
#endif
