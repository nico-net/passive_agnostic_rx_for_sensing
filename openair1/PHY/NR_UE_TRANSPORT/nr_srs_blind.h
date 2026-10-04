/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_SRS_BLIND_H
#define NR_SRS_BLIND_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define NR_SRS_MAX_CANDIDATES 16
#define NR_SRS_MAX_TRACKS 32
#define NR_SRS_MAX_RE (272 * 12 * 4)
typedef struct {
  int fft_size, first_carrier_offset, n_rb, n_ant, symbols_per_slot, slots_per_frame;
} nr_srs_grid_t;
typedef struct {
  int comb, comb_offset, start_symbol, n_symbols, start_rb, nb_rb, group, cs;
  int period, slot_offset, delay_samples;
  float score, noise_var;
  uint64_t track_id;
  uint16_t rnti;
  const char *attribution;
} nr_srs_candidate_t;
typedef struct nr_srs_blind nr_srs_blind_t;
/* Lifecycle only: allocate all scratch before producer threads start. */
nr_srs_blind_t *nr_srs_blind_create(const nr_srs_grid_t *grid);
void nr_srs_blind_destroy(nr_srs_blind_t *s);
/* Grids are antenna-major, interleaved int16 IQ; stride is complex samples.
 * ul_mask limits examination to the last six symbols actually available for UL. */
int nr_srs_comb_detect(const nr_srs_grid_t *g, const int16_t *iq, int stride, uint16_t ul_mask,
                      nr_srs_candidate_t out[NR_SRS_MAX_CANDIDATES]);
bool nr_srs_group_cs_search(nr_srs_blind_t *s, const int16_t *iq, int stride,
                            nr_srs_candidate_t *c, const nr_srs_candidate_t *hint);
void nr_srs_observe(nr_srs_blind_t *s, nr_srs_candidate_t *c, uint64_t abs_slot);
/* LS + interpolation; returns antenna-major complex float CFR in caller storage. */
int nr_srs_estimate(nr_srs_blind_t *s, const int16_t *iq, int stride, nr_srs_candidate_t *c,
                    float *out, uint16_t *k_abs, uint8_t *l_sym);
/* A trigger window is evidence, not a dedicated scheduling configuration. Multiple
 * matching RNTIs remain anonymous. All evidence expires after 256 slots. */
void nr_srs_note_trigger(nr_srs_blind_t *s, uint16_t rnti, uint64_t slot, unsigned request);
void nr_srs_note_pusch(nr_srs_blind_t *s, uint16_t rnti, uint64_t slot, int delay,
                     const float *cfr, const uint16_t *k, int n_re);
void nr_srs_attribute(nr_srs_blind_t *s, nr_srs_candidate_t *c, uint64_t slot,
                     const float *cfr, const uint16_t *k, int n_re);
#ifdef __cplusplus
}
#endif
#endif
