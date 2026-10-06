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
} nr_csirs_disc_sweep_map_t;
int nr_csirs_disc_sweep_validate(const nr_csirs_disc_ring_t *, const nr_csirs_disc_hyp_t *, int,
                                const nr_csirs_disc_sweep_cfg_t *, nr_csirs_disc_sweep_cfg_t *);
int nr_csirs_disc_sweep_map(const nr_csirs_disc_hyp_t *, int, nr_csirs_disc_sweep_map_t *);
/* Scores are [id][delta], already averaged over lattice slots. Strict > breaks ties
 * in ascending id/delta order on both backends. No lattice => z=z_second=0. */
void nr_csirs_disc_sweep_winner(const float *, const nr_csirs_disc_sweep_cfg_t *, int, int, nr_csirs_disc_win_t *);
#ifdef __cplusplus
}
#endif
#endif
