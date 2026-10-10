/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_PUSCH_UL_DISC_H
#define NR_PUSCH_UL_DISC_H
#include <stdbool.h>
#include <stdint.h>
#include "PHY/TOOLS/tools_defs.h"

/* CPU reference contract: one grant, contiguous PRBs, up to four RX. The grids are
 * [antenna][symbol][FFT bin]. A GPU port can replace the batched scorer without
 * changing the hypothesis/state interface. All searches run on the UL consumer. */
typedef struct {
  int fft, n_rb_carrier, first_carrier, rb_start, rb_count;
  int symbols_per_slot, slot, start_symbol, num_symbols, normal_cp;
  const c16_t *rx[4];
  int n_rx;
} nr_pusch_ul_disc_grid_t;

typedef struct {
  int id, nscid, dmrs_type, mapping_type, add_pos, front_load;
  int max_length; /* 2 when two-symbol front load proves it; 0 means 1 or 2 */
  uint16_t mask;
  double coherence, false_alarm_bound;
  bool accepted, fresh;
} nr_pusch_ul_disc_result_t;

typedef struct {
  uint16_t mask, id;
  uint8_t dmrs_type, nscid;
} nr_pusch_ul_disc_hyp_t;
typedef struct { double score; int pairs; } nr_pusch_ul_disc_metric_t;

void nr_pusch_ul_disc_score_batch(const nr_pusch_ul_disc_grid_t *grid,
                                  const nr_pusch_ul_disc_hyp_t *hypotheses, int count,
                                  nr_pusch_ul_disc_metric_t *metrics);

/* Coherent adjacent-pilot correlation across all RX, on the exact granted REs.
 * Returns 0 for invalid geometry. The null tail uses a conservative union bound
 * over 2^16 IDs, two nSCIDs, two types, and 32 symbol layouts. */
double nr_pusch_ul_disc_score(const nr_pusch_ul_disc_grid_t *grid, uint16_t mask,
                              int dmrs_type, int nscid, int id, int *pairs);
double nr_pusch_ul_disc_delay_score(const nr_pusch_ul_disc_grid_t *grid, uint16_t mask,
                                    int dmrs_type, int nscid, int id, int *pairs);
bool nr_pusch_ul_disc_accept(double score, int pairs, double *false_alarm_bound);

/* Priors are evaluated first. Background ID sweep tests 64 consecutive IDs per
 * eligible grant, covering the whole 16-bit range after at most 1024 grants.
 * A result is published only when the familywise noise false-alarm bound <=1e-6
 * and at least 128 adjacent pilot pairs were observed. */
nr_pusch_ul_disc_result_t nr_pusch_ul_disc_search(const nr_pusch_ul_disc_grid_t *grid,
                                                  uint16_t rnti, uint8_t type_a_pos,
                                                  const int *prior_ids, int npriors,
                                                  int preferred_type, int preferred_nscid,
                                                  uint8_t antenna_ports_field);
nr_pusch_ul_disc_result_t nr_pusch_ul_disc_cached(uint16_t rnti);

/* Applied advance = N_TA_offset + a signed arrival correction. Tracker stores
 * that signed correction, never calls it a decoded MAC timing-advance command. */
int nr_pusch_ul_disc_timing_seed(uint16_t rnti, int nominal_advance, long abs_slot, bool *tracked);
void nr_pusch_ul_disc_timing_update(uint16_t rnti, int nominal_advance,
                                    int measured_advance, double score, long abs_slot);
void nr_pusch_ul_disc_reset(void);
void nr_pusch_ul_disc_epoch(uint32_t dedicated_epoch);
#endif
