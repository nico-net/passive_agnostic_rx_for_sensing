/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_SENSE_API_H
#define NR_SENSE_API_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define NR_SENSE_SCHEMA 2
#define NR_SENSE_CFR_VERSION 1
typedef enum { NR_SENSE_SRC_SSB, NR_SENSE_SRC_CSIRS, NR_SENSE_SRC_PDSCH_DMRS, NR_SENSE_SRC_PDSCH_DATA,
               NR_SENSE_SRC_PUSCH_DMRS, NR_SENSE_SRC_PUSCH_DATA, NR_SENSE_SRC_COUNT } nr_sense_src_t;
typedef struct { const char *records_path, *cfr_path, *endpoint; uint32_t cfr_ring_frames, cfr_max_re, rec_ring_slots, cfr_max_ant; } nr_sense_cfg_t;
int nr_sense_open(const nr_sense_cfg_t *cfg);
void nr_sense_close(void);
bool nr_sense_enabled(void);
/* iq is antenna-major complex float pairs; ant_stride is in complex RE, not floats.
 * A gap in frame_id = frames dropped. */
uint64_t nr_sense_push_cfr(nr_sense_src_t src, int n_ant, const float *iq, int ant_stride, int n_re,
                           const uint16_t *k_abs, const uint8_t *l_sym, uint64_t abs_slot, float slot_frac,
                           float noise_var, uint32_t epoch, uint32_t identity_gen, bool *truncated);
/* Adapter entry: converts receiver-wide indices directly into the reserved frame slot. */
uint64_t nr_sense_push_cfr_u32(nr_sense_src_t src, int n_ant, const float *iq, int ant_stride, int n_re,
                               const uint32_t *k_abs, const uint32_t *l_sym, uint64_t abs_slot, float slot_frac,
                               float noise_var, uint32_t epoch, uint32_t identity_gen, bool *truncated);
void nr_sense_reject_cfr(void);
bool nr_sense_push_record(const char *json_line);
typedef struct { uint64_t cfr_pushed, cfr_dropped[NR_SENSE_SRC_COUNT], cfr_rejected, cfr_no_sink, rec_pushed, rec_dropped, io_errors, zmq_drops, truncated, after_close; } nr_sense_stats_t;
void nr_sense_stats(nr_sense_stats_t *out);
/* Deterministic fault hooks for the standalone unit test; call from its controlling thread. */
void nr_sense_test_pause_writer(bool pause);
void nr_sense_test_fail_writes(bool fail);
void nr_sense_test_write_chunk(uint32_t bytes);
void nr_sense_test_writer_limit(uint32_t items);
uint32_t nr_sense_test_written_items(void);
#ifdef NR_SENSE_HAVE_ZMQ
void *nr_sense_test_zmq_context(void);
#endif
#ifdef __cplusplus
}
#endif
#endif
