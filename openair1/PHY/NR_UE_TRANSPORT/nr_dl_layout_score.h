/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* E2 (CRC-free DCI parameter extraction, Level 1): full-layout DM-RS scorer and constellation Qm classifier. Pure C, no state, no
 * allocation, no threads. Wiring into the traffic path is E3.
 *
 * GRID. o->rxF[a * o->symbols + l] points at n_sc c16_t of antenna a, OFDM symbol l of the slot (subcarrier 0 = CRB 0 subcarrier 0, i.e. the
 * carrier grid after the FFT), l < o->symbols (= OFDM symbols per slot, 14 for normal CP; it is also the N_symb^slot of TS 38.211 7.4.1.1.1).
 *
 * PRECODING (operator 2026-11-04). The DM-RS channel is H*W with W constant only inside a PRG. Every product the scorer forms is inside ONE
 * PRB (the same-k' pilot pairs m, m+2 of nr_dmrs_scan_prb_score_gold; W cancels in y(m+2) conj(y(m))), occupancy and energies are per PRB, and the
 * Qm equaliser estimates H per PRB. Nothing correlates, smooths or averages complex values across a PRB boundary.
 *
 * coh. SUM over the hypothesis' (DM-RS symbol, CDM group) cells of (c - c_alpha), c = same-k' coherence of the PRB list (0..1, ~1 for the
 * true identity), c_alpha = the alpha-quantile of c under noise: with n pairs |sum S|^2 / (4 (sum|P|)^2) = c^2 is ~ exponential with mean
 * KAPPA / n (KAPPA = E|P|^2 / (E|P|)^2 = 16/pi^2 = 1.62 for a product of two complex Gaussians, the worst case; fewer if several antennas are summed),
 * so c_alpha = sqrt(KAPPA ln(1/alpha_cell) / n), alpha_cell = NBS_SIGNIF_ALPHA / 42 (Bonferroni over the 14 symbols x 3 groups a hypothesis could claim,
 * so a claimed but empty cell costs (c_alpha - c) > 0 and the truth beats a superset in all but ~2e-5 of the cases). A claimed cell that is not DM-RS
 * thus lowers coh; a DM-RS cell the hypothesis omits is a missing (positive) term: the sum rewards the complete mask. coh is negative when nothing
 * is coherent and cannot be significant (n so small that c_alpha > 1) -- "cannot confirm", never a certain answer.
 * occ_f1. F1 of the occupied PRBs (pilot-RE energy of the claimed cells above the alpha-quantile of the noise, Wilson-Hilferty chi-square, noise_var)
 * against h->crb.
 * comb_ratio. Noise-corrected mean |y|^2 of the REs of the OTHER CDM groups in the DM-RS symbols over that of the pilot REs. "Other" = the groups
 * the hypothesis declares without data (the first cdm_no_data groups, minus those carrying DM-RS) when there are any, else every group of the DM-RS
 * type that carries no pilot. ~0 when empty, ~1/beta^2 (beta = get_beta_dmrs) when they carry data. nr_dl_cdm_consistent() compares it with the
 * declaration at the midpoint of those two model values (no tuned constant).
 * qm. If data_sym_mask != 0: the equalised data REs (PRB-flat H from the nearest DM-RS symbol, port 1 of the first claimed group, MRC over antennas; a
 * rank-1 estimate -- it abstains, qm = 0, when the data is not decodable that way) are classified by nr_qm_classify(). */
#ifndef NR_DL_LAYOUT_SCORE_H
#define NR_DL_LAYOUT_SCORE_H
#include <stdint.h>
#include "common/platform_types.h" /* c16_t */
#ifdef __cplusplus
extern "C" {
#endif

#define NR_DL_MAX_ANT 8
#define NR_DL_MAX_PRB 275
#define NR_DL_MAX_DMRS_SYM 8
#define NR_DL_QM_MAX_RE 256 /* data REs sampled for the Qm classifier */

typedef struct {
  const uint16_t *crb; /* CRBs of the allocation (RIV + VRB-to-PRB), any order */
  int n_crb;
  uint16_t dmrs_sym_mask; /* bit l = DM-RS in symbol l (nr_pdsch_solver_dmrs_mask) */
  uint8_t dmrs_type;      /* 1 or 2 */
  uint8_t cdm_groups_mask; /* bit g = CDM group g carries DM-RS ports (type 1: g 0..1, type 2: g 0..2) */
  uint8_t cdm_no_data;    /* number of CDM groups without data (TS 38.214 7.3.1.2.2 / 5.1.6.2): groups 0..cdm_no_data-1 */
  uint32_t nid;           /* scramblingID (0..65535) */
  uint8_t nscid;
  uint16_t data_sym_mask; /* E2 extension: symbols carrying PDSCH data (TDRA S/L); 0 = no Qm classification */
} nr_dl_hyp_t;

typedef struct {
  const c16_t *const *rxF; /* rxF[a * symbols + l], see above */
  int n_ant, n_sc, symbols;
  float noise_var; /* complex noise variance per RE and antenna, same units as |rxF|^2 */
  int slot;        /* E2 extension: slot number within the frame (the n_s of c_init) */
} nr_dl_obs_t;

typedef struct {
  float coh, occ_f1, comb_ratio;
  int qm;       /* 2, 4, 6, 8 or 0 (undecided / not computed) */
  float qm_llr; /* log-likelihood margin of the best Qm over the runner-up, nats */
} nr_dl_score_t;

void nr_dl_layout_score(const nr_dl_hyp_t *h, const nr_dl_obs_t *o, nr_dl_score_t *s);

/** Is the measured comb_ratio consistent with the hypothesis' cdm_no_data declaration? (1/0) */
int nr_dl_cdm_consistent(const nr_dl_hyp_t *h, const nr_dl_score_t *s);

/** Ranking key of a hypothesis: coh + occ_f1, -INFINITY when the CDM declaration is inconsistent. */
float nr_dl_score_rank(const nr_dl_hyp_t *h, const nr_dl_score_t *s);

/** Modulation order of the TS 38.211 square QAM (2 QPSK, 4, 6, 8) that the n equalised symbols follow, or 0 when undecided.
 *  eq: any common scale (the symbols are normalised to unit mean energy: Es = mean|eq|^2 / (1 + noise_var)). noise_var = N0 / Es of the equalised
 *  symbols (1 / linear SNR), complex. The log-likelihood of each constellation (equiprobable points, circular Gaussian noise; I and Q independent) is
 *  summed over the symbols; a Qm is returned only when it beats EVERY other order by ln(3 / NBS_SIGNIF_ALPHA) nats: by Markov's inequality on the likelihood
 *  ratio a wrong order wins that margin against a given true one with probability <= alpha / 3, so a wrong certain answer has probability <= alpha whatever
 *  the SNR (when noise_var is right); at low SNR the margin is simply not reached and the answer is 0. *llr (optional) receives the margin to the runner-up. */
int nr_qm_classify(const c16_t *eq, int n, float noise_var, float *llr);

#ifdef __cplusplus
}
#endif
#endif
