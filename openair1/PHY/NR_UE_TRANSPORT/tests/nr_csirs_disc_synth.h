/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_CSIRS_DISC_SYNTH_H
#define NR_CSIRS_DISC_SYNTH_H
#include "nr_csirs_disc.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
  uint8_t row, l0, l1, cdm, density;
  uint16_t fd_bitmap, rb_start, n_rb, scramb_id, period, offset;
  float power; /* received pilot/data power ratio before channel and AWGN */
} nr_csirs_disc_synth_resource_t;
typedef struct {
  int n_slots, n_rb, n_ant, slots_per_frame;
  uint32_t first_slot, seed;
  float snr_db, load; /* load=1 full-band every slot; otherwise scheduled fraction */
  bool dmrs, ssb;
  const nr_csirs_disc_synth_resource_t *resources;
  int n_resources;
} nr_csirs_disc_synth_cfg_t;
/* Owns malloc'd ring storage; independent of U1, so usable by T4/T5 alone.
 * Init OAI log/modulation once in the test executable before calling.
 * Rejects malformed configurations before OAI's assert/bitmap walks. */
int nr_csirs_disc_synth(nr_csirs_disc_ring_t *, const nr_csirs_disc_synth_cfg_t *);
void nr_csirs_disc_synth_free(nr_csirs_disc_ring_t *);
int nr_csirs_disc_synth_footprint(const nr_csirs_disc_synth_resource_t *, nr_csirs_disc_fp_t *);
void nr_csirs_disc_synth_init(void);
#ifdef __cplusplus
}
#endif
#endif
