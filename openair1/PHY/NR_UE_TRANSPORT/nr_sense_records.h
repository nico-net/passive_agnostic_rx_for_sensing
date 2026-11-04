/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_SENSE_RECORDS_H
#define NR_SENSE_RECORDS_H
#include "nr_passive_obs.h"
#include "nr_sense_api.h"
#include "nr_sense_quality.h"
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
  /* Receiver-measured quality of the exported estimate: DM-RS / post-estimation SNR in dB, only meaningful when snr_valid
   * (a zero-initialised meta has no estimate and prints null). Set through nr_sense_meta_set_snr(). */
  float snr_db;
  bool snr_valid;
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
  /* Multi-layer PDSCH data-aided CFR (one frame per layer): layer_idx1 = 1-based layer of this frame, n_layers = rank of the grant.
   * 0 = not a per-layer frame; the JSON keys are then absent (records of single-layer sources are unchanged). */
  int layer_idx1, n_layers;
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
  /* Receiver state in force when the record's slot was read (nr_sense_rx_state_lookup, stamped by nr_sense_emit_rs).
   * rx_state_valid=false -> the four fields print as null. rx_shift_cum_samples: FFT-window/timing steps summed since the
   * session started, positive = the read window moved later in the stream; a step changes the CFR phase of subcarrier k by
   * 2*pi*k*step/N between two slots, so coherent processing must restart where rx_shift_count changes. */
  bool rx_state_valid;
  double rx_cfo_hz;
  int64_t rx_shift_cum_samples;
  uint32_t rx_shift_count;
  float rx_gain_db;
  /* Sensing-mode CFR quality + link context (S3). q_valid=false (legacy, traffic, no CFR) -> none of the keys below is printed.
   * nr_sense_emit_rs fills q from the frame-id ring (nr_sense_quality_for_frame); a producer sets only recon_valid + q.recon_conf. */
  nr_sense_quality_t q;
  bool q_valid;
  bool wideband;         /* set by nr_sense_emit_rs with q_valid: q.valid_cfr_prbs >= nr_isac_sense_min_prb() at emit time */
  bool recon_valid;      /* the producer measured q.recon_conf (data-aided, rank 1) */
  bool prg_valid;        /* prg_size is the per-RNTI PRG sweep's latched value (nr_pdsch_prg_latched); false prints null */
  int prg_size;          /* 0 = wideband, 2, 4 */
  uint32_t cfg_ref_ver;  /* with identity_gen + dedicated_epoch = cfg_ref; 0 = unknown (prints null) */
} nr_sense_rs_meta_t;

/* The estimators floor a signal below their noise estimate at -30 dB (nr_ulsch_dmrs_snr_db) and the DL side reports <= -90 for "none":
 * neither is a measurement, so they leave snr_valid false and the record prints snr_db:null. */
#define NR_SENSE_SNR_FLOOR_DB (-29.99f)
static inline void nr_sense_meta_set_snr(nr_sense_rs_meta_t *m, float snr_db)
{
  m->snr_valid = (snr_db == snr_db) && snr_db > NR_SENSE_SNR_FLOOR_DB && snr_db < 200.f;
  m->snr_db = m->snr_valid ? snr_db : 0.f;
}
/* meta of the same grant with its measured quality attached (a compound literal cannot call the setter) */
static inline nr_sense_rs_meta_t nr_sense_meta_with_snr(nr_sense_rs_meta_t m, float snr_db)
{ nr_sense_meta_set_snr(&m, snr_db); return m; }

/* Hyper-frame-extended slot index (the producer's un-wrapped absolute_slot): ((hfn*1024)+frame)*slots_per_frame+slot.
 * frame_rx alone wraps every 10.24 s. */
static inline uint64_t nr_sense_abs_slot_hfn(uint32_t hfn, uint32_t frame, uint32_t slot, uint32_t slots_per_frame)
{ return ((uint64_t)hfn * 1024u + frame) * slots_per_frame + slot; }
/* Same index for a caller that has no hfn but knows the producer's current un-wrapped counter `prod`: the value of the slot
 * (frame, slot) that is closest below `prod` (the pipeline lag stays far under one wrap, 1024 frames). prod < 0 -> wrapped value. */
static inline uint64_t nr_sense_abs_slot_align(int64_t prod, uint32_t frame, uint32_t slot, uint32_t slots_per_frame)
{
  const int64_t wrap = (int64_t)slots_per_frame * 1024, fd = (int64_t)frame * slots_per_frame + slot;
  if (prod < 0) return (uint64_t)fd;
  const int64_t lag = (((prod - fd) % wrap) + wrap) % wrap;
  return prod - lag < 0 ? (uint64_t)fd : (uint64_t)(prod - lag);
}

/* Per-cell receiver state, written once per slot by the receive thread and looked up by slot index from any thread. */
typedef struct {
  int64_t rx_ts;            /* sample counter (RF timestamp) at the start of the slot's read */
  double cfo_hz;            /* digital DL frequency compensation applied to that slot (0 when none is applied) */
  int64_t shift_cum;        /* cumulative window shift, samples */
  uint32_t shift_count;     /* number of non-zero window steps so far */
} nr_sense_rx_state_t;
void nr_sense_rx_note_slot(uint64_t abs_slot, int64_t rx_ts, double cfo_hz);
void nr_sense_rx_note_shift(int samples);   /* a window step of `samples` (positive = later) taken at a frame boundary; 0 is ignored */
void nr_sense_rx_note_gain(double rx_gain_db);
void nr_sense_rx_reset(void);               /* new session: counters back to 0 */
/* true when the slot is in the ring (exact per-slot state); false -> *out holds the latest state with rx_ts -1 */
bool nr_sense_rx_state_lookup(uint64_t abs_slot, nr_sense_rx_state_t *out);
double nr_sense_rx_gain(void);

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
/* Stamp epoch/identity (unless stamp_valid), rx_ts and verif on a grant meta; shared by the grant and DCI emitters. */
void nr_sense_grant_meta_finish(const nr_passive_obs_t *o, nr_sense_grant_meta_t *m);
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
