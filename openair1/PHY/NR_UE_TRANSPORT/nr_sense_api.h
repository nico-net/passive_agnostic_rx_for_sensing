/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_SENSE_API_H
#define NR_SENSE_API_H
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#ifdef __cplusplus
extern "C" {
#endif
#define NR_SENSE_SCHEMA 2
#define NR_SENSE_CFR_VERSION 1
typedef enum { NR_SENSE_SRC_SSB, NR_SENSE_SRC_CSIRS, NR_SENSE_SRC_PDSCH_DMRS, NR_SENSE_SRC_PDSCH_DATA,
               NR_SENSE_SRC_PUSCH_DMRS, NR_SENSE_SRC_PUSCH_DATA, NR_SENSE_SRC_PRACH, NR_SENSE_SRC_SRS,
               NR_SENSE_SRC_COUNT } nr_sense_src_t;
typedef struct { const char *records_path, *cfr_path, *endpoint; uint32_t cfr_ring_frames, cfr_max_re, rec_ring_slots, cfr_max_ant; } nr_sense_cfg_t;
/* k_abs: absolute subcarrier index relative to CRB0 / point A, never an FFT bin.
 * Passive UL exports undo their FFT advance on the copied CFR, so phase refers
 * to the unadvanced DL-frame FFT window (after symbol/CP rotation). SRS without
 * attribution still has the explicitly reported CS/delay ambiguity. */
static inline uint32_t nr_sense_crb_re(int crb, int subcarrier)
{
  return crb * 12 + subcarrier;
}
static inline uint16_t nr_sense_crb_k(int bin,int first_carrier_offset,int fft_size)
{
  return (bin-first_carrier_offset+fft_size)%fft_size;
}
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
typedef enum { NR_SENSE_COST_PROD, NR_SENSE_COST_PACK, NR_SENSE_COST_REC, NR_SENSE_COST_WRITE } nr_sense_cost_t;
/* Call only inside an enabled source path; timestamps and counters use monotonic nanoseconds. */
static inline uint64_t nr_sense_now_ns(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
void nr_sense_cost_add(nr_sense_cost_t kind, nr_sense_src_t src, uint64_t ns);
typedef struct { nr_sense_src_t src; uint64_t start_ns; } nr_sense_prod_scope_t;
static inline void nr_sense_prod_scope_stop(nr_sense_prod_scope_t *scope)
{
  if (scope->start_ns) {
    nr_sense_cost_add(NR_SENSE_COST_PROD,scope->src,nr_sense_now_ns()-scope->start_ns);
    scope->start_ns=0;
  }
}
typedef struct {
  uint64_t cfr_pushed, cfr_dropped[NR_SENSE_SRC_COUNT], cfr_rejected, cfr_no_sink;
  uint64_t rec_pushed, rec_dropped, io_errors, zmq_drops, truncated, after_close;
  uint64_t prod_ns[NR_SENSE_SRC_COUNT], pack_ns[NR_SENSE_SRC_COUNT];
  uint64_t rec_ns, write_ns, prod_count[NR_SENSE_SRC_COUNT], pack_count[NR_SENSE_SRC_COUNT];
  uint64_t rec_count, write_count;
} nr_sense_stats_t;
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
