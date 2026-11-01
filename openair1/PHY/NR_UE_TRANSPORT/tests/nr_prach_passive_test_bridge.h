#ifndef NR_PRACH_PASSIVE_TEST_BRIDGE_H
#define NR_PRACH_PASSIVE_TEST_BRIDGE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { int format, start_symbol, num_occasions, duration; } test_prach_occasion_t;
void test_prach_init(void);
int test_prach_occasion(int index, int frame, int slot, int mu, test_prach_occasion_t *out);
int test_prach_rar_relation(int frame, int slot, int symbol, int fdm, int preamble, uint16_t rnti, uint8_t rapid);
int test_prach_rar_age(uint64_t detection_abs, uint64_t rar_abs, int slots_per_frame);
int test_prach_ra_slot(int slot, int mu, int format);
typedef struct { uint8_t rapid; uint16_t ta; uint16_t tc_rnti; } test_prach_rar_entry_t;
int test_prach_rar_entries(const uint8_t *pdu, uint32_t len, test_prach_rar_entry_t *out, int cap);
typedef struct { int preamble, delay, energy; } test_prach_detection_t;
test_prach_detection_t test_prach_synth(int root, int shift);
void test_prach_json(int relation, char *line, uint32_t capacity);
void test_prach_diag(unsigned long *detections, int *margin_max);
void test_prach_floor_reset(void);
int test_prach_floor(int fdm, int gain_db, int energy);
void test_prach_reset(void);
void test_prach_bind(int cell);
void test_prach_pause(int cell, int pause);
uint64_t test_prach_drops(void);
void test_prach_run_slot(int delay_samples, int msg1_fdm, int mode);
void test_prach_run_stitched_slot(int phase_shift);
void test_prach_drain(void);
void test_prach_finish(void);
const char *test_prach_record(void);
void test_prach_threads(unsigned *slot_calls, unsigned *worker_calls, int *same_thread);
void test_prach_heavy_calls(unsigned *ru, unsigned *correlator, unsigned *exported);
unsigned test_prach_start_calls(void);
uint64_t test_prach_skipped(void);
/* Real-IQ chain test: a 4-RX, 122.88 Ms/s, 30 kHz capture of the DDDDDDDSUU cell (capture_info.json), PRACH config of the cell
 * (L839, 1.25 kHz, format 0). The harness feeds `n_noise` occasions of the capture as they are, then one occasion carrying a synthetic
 * preamble generated from TS 38.211 6.3.3 (independent of the receiver code), and returns what the PRODUCTION slot hook + worker did. */
typedef struct {
  const char *capture_dir;  /* directory with rx0..rx3.sc16 */
  long t0;                  /* capture sample index of a 10 ms frame start */
  int n_noise;              /* occasions run before the injected one (>= 8 seeds the floor) */
  int config_index;         /* prach-ConfigurationIndex (FR1 unpaired) */
  int root, zczc, restricted; /* prach-RootSequenceIndex (L839), zeroCorrelationZoneConfig, restrictedSetConfig */
  int msg1_fstart, msg1_fdm; /* msg1-FrequencyStart (PRB, from PRB 0 of the UL BWP), log2 of msg1-FDM occasions */
  int ul_bwp_start;         /* initial UL BWP start PRB (from PRB 0 of the carrier grid) */
  int preamble, delay;      /* injected preamble index and delay in receiver samples; preamble < 0: nothing injected */
  double snr_db;            /* per-antenna sample power of the preamble / measured sample power of the occasion window (receiver rate) */
  double cfo_hz;            /* residual carrier offset of the received preamble (the capture is not CFO-corrected, see rx_cfo_applied_hz) */
  double rx_fo_hz;          /* carrier offset the receiver estimated and removes (ue->freq_offset with cont_fo_comp); 0: none */
  int rar_on, rar_rapid, rar_ta, rar_tc; /* a RAR (TS 38.321 RA-RNTI of the injected occasion) naming rar_rapid is decoded after the occasion */
  int fdm_index;            /* which of the msg1-FDM occasions carries it */
} test_prach_real_cfg_t;
typedef struct {
  int ok;                   /* capture read and config accepted */
  int records;              /* PRACH records emitted for the injected occasion */
  int preamble, delay_samples, energy;
  unsigned long occasions, detections, skipped;
  int margin_max;
  int peak_last, floor_last, peak_max, peak_min, peak_mean;
  unsigned long peak_zero;
  unsigned long lb_tried, lb_found, lb_noring;
  unsigned long us_per_occasion; /* worker time per correlated occasion (prach_us) */
  double noise_rms;         /* per-component rms of the occasion window, antenna 0 */
  char record[2048];
} test_prach_real_out_t;
int test_prach_real_iq(const test_prach_real_cfg_t *cfg, test_prach_real_out_t *out);
#ifdef __cplusplus
}
#endif
#endif
