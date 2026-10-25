/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_CSIRS_DISC_SWEEP_INTERNAL_H
#define NR_CSIRS_DISC_SWEEP_INTERNAL_H
#include "nr_csirs_disc.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Host mapping service exported by the CPU implementation to the dlopen'd module.
 * Ordering is port, then symbol/subcarrier, exactly the dense reference correlator. */
typedef struct {
  int grid_index;
  uint16_t m;
  uint8_t symbol;
  int8_t sign;
} nr_csirs_disc_re_t;
typedef struct {
  int ports, n_re, words;
  unsigned sym_mask;
  int begin[33];
  nr_csirs_disc_re_t *re;
  /* CDM group size L of the row (TS 38.211 Table 7.4.1.5.3-1: 1 no CDM, 2 fd-CDM2, 4 cdm4-FD2-TD2, 8 cdm8-FD2-TD4). With L > 1 each
   * port's list is made of whole CDM units (the L REs of one CDM group in one RB, consecutive, ordered by the unit's first RE), so a
   * scorer can despread a unit with its OCC (the sign of each RE) before correlating. */
  int cdm_len;
} nr_csirs_disc_sweep_map_t;
/* Per-port block correlation of a CDM-despread port: units of L REs, z_u = sum y conj(x) over a unit (x carries the OCC sign), and
 * |sum z_u|^2 / (n_u sum |z_u|^2). With L = 1 this is the plain normalised correlation (|x| constant). A match reads 1 whatever the
 * power split among the ports of the group: the other ports' energy, which plain correlation counts as noise, is removed by the OCC
 * (TS 38.211 7.4.1.5.3), so a CDM row scores on the per-port scale of a single-port row. */
int nr_csirs_disc_sweep_validate(const nr_csirs_disc_ring_t *, const nr_csirs_disc_hyp_t *, int,
                                const nr_csirs_disc_sweep_cfg_t *, nr_csirs_disc_sweep_cfg_t *);
int nr_csirs_disc_sweep_map(const nr_csirs_disc_hyp_t *, int, nr_csirs_disc_sweep_map_t *);
int nr_csirs_disc_sweep_map_into(const nr_csirs_disc_hyp_t *,int,nr_csirs_disc_sweep_map_t *,nr_csirs_disc_re_t *,size_t);
/* Scores are [id][delta], already averaged over lattice slots. Strict > breaks ties
 * in ascending id/delta order on both backends. No lattice => z=z_second=0. */
void nr_csirs_disc_sweep_winner(const float *, const nr_csirs_disc_sweep_cfg_t *, int, int, nr_csirs_disc_win_t *);
#ifdef __cplusplus
}
#endif
#endif
