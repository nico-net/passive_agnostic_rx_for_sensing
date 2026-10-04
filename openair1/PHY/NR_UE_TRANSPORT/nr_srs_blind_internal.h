/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_SRS_BLIND_INTERNAL_H
#define NR_SRS_BLIND_INTERNAL_H
#include "nr_srs_blind.h"
#include "PHY/defs_gNB.h"
typedef struct {
  nr_srs_candidate_t key;
  uint64_t first, last, id;
  unsigned gcd, count;
} srs_track_t;
struct nr_srs_blind {
  nr_srs_grid_t grid;
  srs_track_t tracks[NR_SRS_MAX_TRACKS];
  uint64_t next_id;
  c16_t *reference;
  PHY_VARS_gNB *gnb;
  NR_DL_FRAME_PARMS *fp;
};
bool nr_srs_make_info(const nr_srs_candidate_t *c, nr_srs_info_t *info);
bool nr_srs_valid_grid(const nr_srs_grid_t *g, int stride);
bool nr_srs_valid_candidate(const nr_srs_grid_t *g, const nr_srs_candidate_t *c);
#endif
