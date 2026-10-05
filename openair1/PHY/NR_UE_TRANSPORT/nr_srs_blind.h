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
#define NR_SRS_CPU_SAMPLES 4096
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
  bool confirmed, delay_resolved;
  double delay_ambiguity_samples, residual_delay;
} nr_srs_candidate_t;
typedef struct nr_srs_blind nr_srs_blind_t;
/* Worker-owned idle policy. CPU samples form a rolling one-second window. */
typedef struct {
  uint64_t cpu_time[NR_SRS_CPU_SAMPLES], cpu_ns[NR_SRS_CPU_SAMPLES], cpu_used;
  unsigned cpu_head, cpu_count, pct;
  unsigned empty, interval;
  uint64_t eligible, backoff_skip, budget_skip, pregate_skip, scans, resets;
} nr_srs_idle_t;
bool nr_srs_pregate(const nr_srs_grid_t *g, const int16_t *iq, int stride, uint16_t ul_mask);
bool nr_srs_idle_scan(nr_srs_idle_t *idle, uint64_t slot, bool evidence);
void nr_srs_idle_result(nr_srs_idle_t *idle, bool candidate);
bool nr_srs_cpu_allow(nr_srs_idle_t *idle, uint64_t now_ns, uint64_t estimate_ns);
void nr_srs_cpu_charge(nr_srs_idle_t *idle, uint64_t now_ns, uint64_t cpu_ns);
/* Return only due, confirmed resource geometries. count is zero when still blind. */
int nr_srs_confirmed_due(const nr_srs_blind_t *s, uint64_t slot,
                         nr_srs_candidate_t out[NR_SRS_MAX_TRACKS]);
bool nr_srs_has_confirmed(const nr_srs_blind_t *s, uint64_t slot);
void nr_srs_confirmed_miss(nr_srs_blind_t *s, uint64_t slot,
                           const nr_srs_candidate_t *seen, int nseen);
/* Lifecycle only: allocate all scratch before producer threads start. */
nr_srs_blind_t *nr_srs_blind_create(const nr_srs_grid_t *grid);
void nr_srs_blind_destroy(nr_srs_blind_t *s);
void nr_srs_blind_reset(nr_srs_blind_t *s);
/* Grids are antenna-major, interleaved int16 IQ; stride is complex samples.
 * ul_mask limits examination to the last six symbols actually available for UL. */
int nr_srs_comb_detect(const nr_srs_grid_t *g, const int16_t *iq, int stride, uint16_t ul_mask,
                      nr_srs_candidate_t out[NR_SRS_MAX_CANDIDATES]);
bool nr_srs_group_cs_search(nr_srs_blind_t *s, const int16_t *iq, int stride,
                            nr_srs_candidate_t *c, const nr_srs_candidate_t *hint);
bool nr_srs_valid_capture_grid(const nr_srs_grid_t *g);
bool nr_srs_refresh_grid(nr_srs_blind_t *s,const nr_srs_grid_t *g);
bool nr_srs_sources_selected(const char *sources);
void nr_srs_note_grant(nr_srs_blind_t *s, uint64_t slot, uint16_t dmrs_mask, int start_rb, int nb_rb);
bool nr_srs_search_slot(nr_srs_blind_t *s, const int16_t *iq, int stride,
                        nr_srs_candidate_t *c, const nr_srs_candidate_t *hint, uint64_t slot);
uint64_t nr_srs_sequence_trials(const nr_srs_blind_t *s);
uint64_t nr_srs_unsupported_lengths(const nr_srs_blind_t *s);
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
