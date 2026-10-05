/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_SENSE_RECORDS_H
#define NR_SENSE_RECORDS_H
#include "nr_passive_obs.h"
#include "nr_sense_api.h"
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct NR_DL_FRAME_PARMS_s NR_DL_FRAME_PARMS;

/* Most integer fields use -1 for unknown; signed TA has a separate validity flag.
 * Float fields use NAN for unknown. The base
 * RS fields match the Task 3 interface; later fields carry optional UL/PRACH/SRS metadata. */
typedef struct nr_sense_rs_meta_s {
  uint16_t rnti;
  int start_rb, nb_rb, n_re, n_ant, ports, ssb_index, k_ssb;
  uint32_t l_sym_mask;
  float noise_var, slot_frac;
  uint64_t abs_slot;
  int frame, slot;
  const char *csirs_resource;
  bool truncated;
  int pci;
  int64_t rx_ts;
  uint64_t t_mono_ns;
  int epoch, identity_gen, dedicated_epoch;
  bool stamp_valid;
  int ta_est_samples, transform_precoding, dmrs_type, dmrs_add_pos, dmrs_n_scid;
  int ta_applied_samples, port_mask;
  bool ta_applied_valid;
  int prach_preamble;
  float prach_energy, prach_delay;
  int prach_fdm, prach_symbol, prach_rar_match, prach_rar_rapid, prach_rar_ta;
  int srs_comb, srs_offset, srs_cs, srs_group, srs_period;
  uint64_t srs_track_id;
  int srs_n_symbols, srs_slot_offset;
  bool srs_confirmed, srs_timing_cs_ambiguous;
  float srs_delay_ambiguity_samples;
  const char *srs_attribution;
} nr_sense_rs_meta_t;

typedef struct {
  int epoch, identity_gen, dedicated_epoch;
  bool stamp_valid;
  int64_t rx_ts;
  int dl_inline;
  const char *verif;
  int k2, dci_slot, transform_precoding, freq_hopping;
  int dmrs_type, dmrs_add_pos, dmrs_n_scid, ta_est_samples;
  int ta_applied_samples;
  bool ta_applied_valid;
  float cfo_est_hz;
  const char *ul_timing_ref;
} nr_sense_grant_meta_t;

/* Sweep index -1 means no sweep. Positive TA advances the UL FFT window. */
static inline int nr_sense_ul_ta_applied(int caller, int start, int step, int sweep_index)
{ return sweep_index < 0 ? caller : start + sweep_index * step; }
static inline int nr_sense_ul_ta_refined(int applied, int residual_delay)
{ return applied - residual_delay; }
void nr_sense_capture_grant_stamp(nr_sense_grant_meta_t *m);

/* Pure formatters also serve fixtures; no I/O or allocation. */
int nr_sense_grant_to_json(const nr_passive_obs_t *o, const nr_sense_grant_meta_t *m, char *buf, size_t n);
int nr_sense_rs_to_json(nr_sense_src_t src, const nr_sense_rs_meta_t *m, uint64_t cfr_frame_id, char *buf, size_t n);
void nr_sense_set_session_identity(const char *rx_id, const char *rx_array);
void nr_sense_emit_session(const NR_DL_FRAME_PARMS *fp, double rx_gain_db);
void nr_sense_emit_grant(const nr_passive_obs_t *o, int dl_inline);
void nr_sense_emit_grant_meta(const nr_passive_obs_t *o, const nr_sense_grant_meta_t *m);
void nr_sense_emit_rs(nr_sense_src_t src, const nr_sense_rs_meta_t *m, uint64_t cfr_frame_id);
/* 1 queued, 0 dropped, -1 unavailable (a standalone record sink may handle it). */
int nr_sense_push_rs_binary(nr_sense_src_t src, const nr_sense_rs_meta_t *m, uint64_t cfr_frame_id);
#ifdef __cplusplus
}
#endif
#endif
