#include "nr_prach_passive_test_bridge.h"
#include "PHY/defs_gNB.h"
#include "PHY/NR_UE_TRANSPORT/nr_prach_passive.h"
#include "common/utils/nr_cell.h"
#include "PHY/NR_TRANSPORT/nr_transport_common_proto.h"
#include "PHY/NR_TRANSPORT/nr_transport_proto.h"
#include "common/config/config_userapi.h"
#include "common/utils/LOG/log.h"
#include "executables/softmodem-common.h"
#include "common/utils/nr/nr_common.h"
#include <math.h>
#include "PHY/TOOLS/tools_defs.h"
#include <stdlib.h>
#include <string.h>
void nr_prach_rx_sample_range(prach_item_t *p, NR_DL_FRAME_PARMS *fp, int nta, int *start, int *end);
uint32_t get_samples_slot_timestamp(const NR_DL_FRAME_PARMS *fp, unsigned int slot)
{
  return slot * fp->samples_per_subframe / fp->slots_per_subframe;
}
uint32_t get_samples_per_slot(int slot, const NR_DL_FRAME_PARMS *fp)
{
  (void)slot;
  return fp->samples_per_subframe / fp->slots_per_subframe;
}
uint16_t get_first_ant_idx(bool das, uint16_t num_ports_beams, uint16_t beam_id, uint16_t fapi_start_port)
{
  (void)num_ports_beams;
  return das ? fapi_start_port : beam_id;
}

configmodule_interface_t *uniqCfg;
static log_t test_log;
void test_prach_init(void)
{
  static int loaded;
  g_log = &test_log;
  if (!loaded) { load_dftslib(); InitSinLUT(); loaded = 1; }
}
static softmodem_params_t test_params;
softmodem_params_t *get_softmodem_params(void) { return &test_params; }
void exit_function(const char *file, const char *function, int line, const char *msg, int abort_flag)
{
  (void)file; (void)function; (void)line; (void)msg; (void)abort_flag;
  abort();
}

int test_prach_occasion(int index, int frame, int slot, int mu, test_prach_occasion_t *out)
{
  nr_prach_passive_occasion_t o;
  if (!nr_prach_passive_occasion(index, frame, slot, mu, FR1, &o)) return 0;
  *out = (test_prach_occasion_t){o.format, o.start_symbol, o.num_occasions, o.duration};
  return 1;
}

int test_prach_rar_relation(int frame, int slot, int symbol, int fdm, int preamble, uint16_t rnti, uint8_t rapid)
{
  return nr_prach_passive_rar_relation(frame, slot, symbol, fdm, preamble, rnti, rapid);
}

int test_prach_rar_age(uint64_t detection_abs, uint64_t rar_abs, int slots_per_frame)
{
  return nr_prach_passive_rar_age_test(detection_abs, rar_abs, slots_per_frame);
}

int test_prach_ra_slot(int slot, int mu, int format)
{
  return nr_prach_passive_ra_slot_test(slot, mu, format);
}

int test_prach_rar_entries(const uint8_t *pdu, uint32_t len, test_prach_rar_entry_t *out, int cap)
{
  return nr_prach_passive_rar_entries(pdu, len, (nr_prach_rar_entry_t *)out, cap);
}

test_prach_detection_t test_prach_synth(int root, int shift)
{
  c16_t xu[64][839] = {{{0}}};
  c16_t buf[1][NUMBER_OF_NR_RU_PRACH_OCCASIONS_MAX][NR_PRACH_SEQ_LEN_L] = {{{{0}}}};
  compute_nr_prach_seq(1, 1, root, xu);
  for (int k = 0; k < 139; ++k) {
    const double phase = -2.0 * M_PI * k * shift / 256.0;
    const double re = xu[0][k].r * cos(phase) - xu[0][k].i * sin(phase);
    const double im = xu[0][k].r * sin(phase) + xu[0][k].i * cos(phase);
    buf[0][0][k] = (c16_t){(int16_t)(re / 4), (int16_t)(im / 4)};
  }
  time_stats_t timing = {0};
  prach_item_t p = {.nb_rx = 1, .prach_sequence_length = 1, .rootSequenceIndex = root,
                    .numrootSequenceIndex = 1, .Xu = xu, .prach_buf = buf, .rx_prach = &timing};
  p.pdu.prach_format = 4;
  p.pdu.num_cs = 13;
  rx_prach_out_t result = rx_nr_prach(&p, 0);
  return (test_prach_detection_t){result.max_preamble, result.max_preamble_delay, result.max_preamble_energy};
}

void test_prach_json(int relation, char *line, uint32_t capacity)
{
  nr_prach_passive_format_test(relation, line, capacity);
}

void test_prach_diag(unsigned long *det, int *margin) { uint64_t o, d; nr_prach_passive_diag(&o, &d, margin); *det = (unsigned long)d; }
void test_prach_floor_reset(void) { nr_prach_passive_floor_reset_test(); }
int test_prach_floor(int fdm, int gain_db, int energy)
{
  return nr_prach_passive_floor_test(fdm, gain_db, energy);
}

/* the record a cell's PRACH worker pushed (the worker is bound to its cell, so nr_cell_idx() names it); cell 0 = the one record of before */
static char captured_cell_record[NR_CELL_MAX][2048];
bool __wrap_nr_sense_enabled(void) { return true; }
bool __wrap_nr_sense_push_record(const char *line)
{
  snprintf(captured_cell_record[nr_cell_idx()], sizeof(captured_cell_record[0]), "%s", line);
  return true;
}
void test_prach_reset(void)
{
  nr_prach_passive_test_reset();
  captured_cell_record[nr_cell_idx()][0] = 0;
}
void test_prach_bind(int cell) { nr_cell_bind(cell); }
void test_prach_pause(int cell, int pause) { nr_prach_passive_cell_pause(cell, pause != 0); }
uint64_t test_prach_drops(void)
{
  uint64_t us, drops, skipped;
  nr_prach_passive_metrics(&us, &drops, &skipped);
  return drops;
}
const char *test_prach_record(void) { return captured_cell_record[nr_cell_idx()]; }
void test_prach_drain(void) { nr_prach_passive_test_drain(); }
void test_prach_finish(void) { nr_prach_passive_flush(); }
void test_prach_threads(unsigned *slot_calls, unsigned *worker_calls, int *same_thread)
{
  bool same;
  nr_prach_passive_test_threads(slot_calls, worker_calls, &same);
  *same_thread = same;
}
void test_prach_heavy_calls(unsigned *ru, unsigned *correlator, unsigned *exported)
{
  nr_prach_passive_test_heavy_calls(ru, correlator, exported);
}
unsigned test_prach_start_calls(void) { return nr_prach_passive_test_start_calls(); }
uint64_t test_prach_skipped(void)
{
  uint64_t us, drops, skipped;
  nr_prach_passive_metrics(&us, &drops, &skipped);
  return skipped;
}

/* Feed the public slot hook a format-B4 occasion. The waveform uses OAI's
 * generated PRACH root and inverse DFT, then crosses the queue copy and the
 * receive DFT/correlator before a JSON record is emitted. */
void test_prach_run_slot(int delay_samples, int msg1_fdm, int mode)
{
  PHY_VARS_NR_UE *ue = calloc(1, sizeof(*ue));
  NR_DL_FRAME_PARMS *fp = &ue->frame_parms;
  *fp = (NR_DL_FRAME_PARMS){.samples_per_subframe = 30720, .samples_per_frame = 307200,
                            .samples_per_slot0 = 15360, .samples_per_slotN0 = 15360,
                            .slots_per_subframe = 2, .slots_per_frame = 20,
                            .ofdm_symbol_size = 2048, .nb_prefix_samples = 144,
                            .nb_prefix_samples0 = 176, .N_RB_UL = 106,
                            .numerology_index = 1, .freq_range = FR1, .frame_type = TDD,
                            .nb_antennas_rx = 1, .Nid_cell = 42};
  if (mode & 4) fp->frame_type = FDD;
  c16_t *rx = calloc(fp->samples_per_frame, sizeof(c16_t));
  c16_t *antennas[] = {rx};
  ue->common_vars.rxdata = antennas;
  fapi_nr_num_prach_fd_occasions_t fd[2] = {{.prach_root_sequence_index = 7,
      .num_root_sequences = 1, .k1 = 53, .prach_zero_corr_conf = 7},
      {.prach_root_sequence_index = 7, .num_root_sequences = 1, .k1 = 54, .prach_zero_corr_conf = 7}};
  ue->nrUE_config.prach_config = (fapi_nr_prach_config_t){
      .prach_sequence_length = 1, .prach_sub_c_spacing = 1,
      .num_prach_fd_occasions = msg1_fdm, .num_prach_fd_occasions_list = fd};
  fapi_nr_max_num_of_symbol_per_slot_t symbols[20][14];
  fapi_nr_max_tdd_periodicity_t slots[20];
  for (int slot = 0; slot < 20; ++slot) {
    for (int sym = 0; sym < 14; ++sym) symbols[slot][sym].slot_config = 1;
    slots[slot].max_num_of_symbol_per_slot_list = symbols[slot];
  }
  if (mode & 2) symbols[3][0].slot_config = 0; // mixed slot starts with DL
  ue->nrUE_config.tdd_table.tdd_period_in_slots = 20;
  ue->nrUE_config.tdd_table.max_tdd_periodicity_list = slots;
  nr_prach_passive_test_prior(155, true, (mode & 1) != 0);
  UE_nr_rxtx_proc_t proc = {.frame_rx = 0, .nr_slot_rx = 3, .hfn_rx = 0,
                            .rx_slot_type = NR_UPLINK_SLOT};
  if (delay_samples >= 0) {
    c16_t xu[64][839] = {{{0}}};
    c16_t freq[1024] __attribute__((aligned(32))) = {{0}};
    c16_t wave[1024] __attribute__((aligned(32))) = {{0}};
    compute_nr_prach_seq(1, 1, 7, xu);
    const int kbar = get_PRACH_k_bar(1, 1);
    int k = kbar;
    for (int i = 0; i < 139; ++i) {
      const double phase = -2.0 * M_PI * i * (delay_samples / 4) / 256.0;
      freq[k] = (c16_t){(int16_t)((xu[0][i].r * cos(phase) - xu[0][i].i * sin(phase)) / 4),
                        (int16_t)((xu[0][i].r * sin(phase) + xu[0][i].i * cos(phase)) / 4)};
      k = (k + 1) % 1024;
    }
    idft(get_idft(1024), (int16_t *)freq, (int16_t *)wave, 1);
    const int nta = set_default_nta_offset(FR1, fp->samples_per_subframe);
    prach_item_t item = {.slot = 3, .numerology_index = 1, .mu = 1,
                         .msg1_frequencystart = 53, .prach_sequence_length = 1};
    item.pdu.prach_format = 8;
    item.pdu.prach_start_symbol = 0;
    item.pdu.num_prach_ocas = 1;
    int first, last;
    nr_prach_rx_sample_range(&item, fp, nta, &first, &last);
    for (int rep = 0; rep < 12; ++rep)
      for (int n = 0; n < 1024; ++n)
        rx[first + rep * 1024 + n] = wave[n];
  }
  nr_prach_passive_process(ue, &proc);
  free(rx);
  free(ue);
}

void test_prach_run_stitched_slot(int phase_shift)
{
  PHY_VARS_NR_UE *ue = calloc(1, sizeof(*ue));
  NR_DL_FRAME_PARMS *fp = &ue->frame_parms;
  *fp = (NR_DL_FRAME_PARMS){.samples_per_subframe = 30720, .samples_per_frame = 307200,
                            .samples_per_slot0 = 15360, .samples_per_slotN0 = 15360,
                            .slots_per_subframe = 2, .slots_per_frame = 20,
                            .ofdm_symbol_size = 2048, .nb_prefix_samples = 144,
                            .nb_prefix_samples0 = 176, .N_RB_UL = 106,
                            .numerology_index = 1, .freq_range = FR1, .frame_type = TDD,
                            .nb_antennas_rx = 1, .Nid_cell = 42};
  c16_t *rx = calloc(fp->samples_per_frame, sizeof(c16_t));
  c16_t *antennas[] = {rx};
  ue->common_vars.rxdata = antennas;
  fapi_nr_num_prach_fd_occasions_t fd = {.prach_root_sequence_index = 7,
      .num_root_sequences = 1, .k1 = 53, .prach_zero_corr_conf = 7};
  ue->nrUE_config.prach_config = (fapi_nr_prach_config_t){
      .prach_sequence_length = 1, .prach_sub_c_spacing = 1,
      .num_prach_fd_occasions = 1, .num_prach_fd_occasions_list = &fd};
  fapi_nr_max_num_of_symbol_per_slot_t symbols[20][14];
  fapi_nr_max_tdd_periodicity_t slots[20];
  for (int slot = 0; slot < 20; ++slot) {
    for (int sym = 0; sym < 14; ++sym) symbols[slot][sym].slot_config = 1;
    slots[slot].max_num_of_symbol_per_slot_list = symbols[slot];
  }
  ue->nrUE_config.tdd_table.tdd_period_in_slots = 20;
  ue->nrUE_config.tdd_table.max_tdd_periodicity_list = slots;
  nr_prach_passive_test_prior(254, true, false); // test-only A1 at slot zero
  c16_t xu[64][839] = {{{0}}};
  c16_t freq[1024] __attribute__((aligned(32))) = {{0}};
  c16_t wave[1024] __attribute__((aligned(32))) = {{0}};
  compute_nr_prach_seq(1, 1, 7, xu);
  int k = get_PRACH_k_bar(1, 1);
  for (int i = 0; i < 139; ++i) {
    const double phase = -2.0 * M_PI * i * phase_shift / 256.0;
    freq[k] = (c16_t){(int16_t)((xu[0][i].r * cos(phase) - xu[0][i].i * sin(phase)) / 4),
                      (int16_t)((xu[0][i].r * sin(phase) + xu[0][i].i * cos(phase)) / 4)};
    k = (k + 1) % 1024;
  }
  idft(get_idft(1024), (int16_t *)freq, (int16_t *)wave, 1);
  prach_item_t item = {.slot = 0, .numerology_index = 1, .mu = 1,
                       .msg1_frequencystart = 53, .prach_sequence_length = 1};
  item.pdu.prach_format = 4;
  item.pdu.prach_start_symbol = 0;
  item.pdu.num_prach_ocas = 1;
  int first, last;
  nr_prach_rx_sample_range(&item, fp, set_default_nta_offset(FR1, fp->samples_per_subframe), &first, &last);
  for (int rep = 0; rep < 2; ++rep)
    for (int n = 0; n < 1024; ++n) {
      const int pos = first + rep * 1024 + n;
      if (pos < 0) rx[fp->samples_per_frame + pos] = wave[n];
    }
  UE_nr_rxtx_proc_t previous = {.frame_rx = 1023, .nr_slot_rx = 19, .hfn_rx = 0,
                                 .rx_slot_type = NR_UPLINK_SLOT};
  nr_prach_passive_process(ue, &previous); // save previous-frame tail
  memset(rx, 0, fp->samples_per_frame * sizeof(c16_t));
  for (int rep = 0; rep < 2; ++rep)
    for (int n = 0; n < 1024; ++n) {
      const int pos = first + rep * 1024 + n;
      if (pos >= 0) rx[pos] = wave[n];
    }
  UE_nr_rxtx_proc_t current = {.frame_rx = 0, .nr_slot_rx = 0, .hfn_rx = 1,
                                .rx_slot_type = NR_UPLINK_SLOT};
  nr_prach_passive_process(ue, &current);
  free(rx);
  free(ue);
}

/* ---- real-IQ chain test (test_prach_real_iq) ---- */
#include "common/utils/nr/nr_common.h"
#include "openair2/LAYER2/NR_MAC_COMMON/nr_mac_common.h"
#include "openair1/PHY/NR_TRANSPORT/nr_prach.h"
#include <complex.h>
#include <stdio.h>

#define RIQ_RATE 122880
#define RIQ_FRAME (RIQ_RATE * 10)
#define RIQ_NRX 4

static int riq_load_frame(const char *dir, int ch, long first_sample, c16_t *dst)
{
  char path[1024];
  snprintf(path, sizeof(path), "%s/rx%d.sc16", dir, ch);
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  const int ok = fseek(f, first_sample * 4, SEEK_SET) == 0 && fread(dst, 4, RIQ_FRAME, f) == RIQ_FRAME;
  fclose(f);
  return ok;
}

/* TS 38.211 6.3.3.1 / 5.3.2, long preamble format 0 (1.25 kHz, N_ZC 839) of a 30 kHz carrier at 122.88 Ms/s: CP + one 98304-sample
 * sequence, as double complex. Generated from the formulas only: x_u(n) = exp(-j pi u n (n+1) / N_ZC), cyclic shift C_v, DFT to 839
 * tones, tone k on 1.25 kHz bin (K * k_1 + kbar + k) with k_1 = 12 * n_RA_start - 6 * N_RB (subcarriers of the 30 kHz grid from the
 * carrier centre), K = 30/1.25 = 24, kbar = 1 (Table 6.3.3.2-1). Returns the sequence part (98304) and the CP (12672) separately. */
#define RIQ_NU 98304
#define RIQ_NCP 12672
static void riq_preamble(int root, int NCS, int preamble, int n_ra_start_prb, int n_rb, double complex *seq)
{
  const int N = 839;
  const int r = NCS ? N / NCS : 64;
  const int L = (root + preamble / r) % (N - 1);
  const int Cv = NCS ? (preamble % r) * NCS : 0;
  const int u = prach_root_sequence_map_0_3[L];
  double complex x[839], W[839];
  for (int n = 0; n < N; ++n) W[n] = cexp(-2.0 * M_PI * I * n / N);
  for (int n = 0; n < N; ++n) {
    const long m = ((long)u * n * (n + 1)) % (2L * N); /* exp(-j pi u n (n+1) / N) has period 2N */
    x[n] = cexp(-1.0 * I * M_PI * (double)m / N);
  }
  double complex y[839];
  for (int k = 0; k < N; ++k) {
    double complex a = 0;
    for (int n = 0; n < N; ++n) a += x[(n + Cv) % N] * W[((long)n * k) % N];
    y[k] = a;
  }
  int k1 = 12 * n_ra_start_prb - 6 * n_rb; /* subcarriers from the carrier centre */
  const long bin0 = (long)24 * k1 + 1;
  memset(seq, 0, sizeof(*seq) * RIQ_NU);
  for (int k = 0; k < N; ++k) {
    const long b = ((bin0 + k) % RIQ_NU + RIQ_NU) % RIQ_NU;
    const double complex step = cexp(2.0 * M_PI * I * (double)b / RIQ_NU);
    double complex ph = 1;
    for (int n = 0; n < RIQ_NU; ++n) {
      seq[n] += y[k] * ph;
      ph *= step;
      if ((n & 4095) == 4095) ph = cexp(2.0 * M_PI * I * (double)b * (n + 1) / RIQ_NU); /* re-anchor the recurrence */
    }
  }
}

int test_prach_real_iq(const test_prach_real_cfg_t *cfg, test_prach_real_out_t *out)
{
  memset(out, 0, sizeof(*out));
  if (cfg->restricted != 0) return 0;
  PHY_VARS_NR_UE *ue = calloc(1, sizeof(*ue));
  NR_DL_FRAME_PARMS *fp = &ue->frame_parms;
  *fp = (NR_DL_FRAME_PARMS){.samples_per_subframe = RIQ_RATE, .samples_per_frame = RIQ_FRAME,
                            .samples_per_slot0 = RIQ_RATE / 2, .samples_per_slotN0 = RIQ_RATE / 2,
                            .slots_per_subframe = 2, .slots_per_frame = 20,
                            .ofdm_symbol_size = 4096, .nb_prefix_samples = 288,
                            .nb_prefix_samples0 = 352, .N_RB_UL = 217, .N_RB_DL = 217,
                            .numerology_index = 1, .freq_range = FR1, .frame_type = TDD,
                            .nb_antennas_rx = RIQ_NRX, .Nid_cell = 415};
  c16_t *rx[RIQ_NRX];
  for (int a = 0; a < RIQ_NRX; ++a) rx[a] = calloc(RIQ_FRAME, sizeof(c16_t));
  ue->common_vars.rxdata = rx;

  const int n_fd = 1 << cfg->msg1_fdm;
  fapi_nr_num_prach_fd_occasions_t fd[8];
  const int NCS = get_NCS(cfg->zczc, 0, cfg->restricted);
  const int r = NCS ? 839 / NCS : 64;
  const int n_roots = NCS ? (64 + r - 1) / r : 64;
  for (int i = 0; i < n_fd; ++i)
    fd[i] = (fapi_nr_num_prach_fd_occasions_t){.num_prach_fd_occasions = i, .prach_root_sequence_index = cfg->root,
                                               .num_root_sequences = n_roots, .prach_zero_corr_conf = cfg->zczc,
                                               .k1 = cfg->ul_bwp_start + cfg->msg1_fstart + (int)get_N_RA_RB(4, 1) * i};
  ue->nrUE_config.prach_config = (fapi_nr_prach_config_t){.prach_sequence_length = 0, .prach_sub_c_spacing = 4,
                                                         .restricted_set_config = cfg->restricted,
                                                         .num_prach_fd_occasions = n_fd, .num_prach_fd_occasions_list = fd};
  /* DDDDDDDSUU, 5 ms: slot 7 = 6 DL, 4 guard, 4 UL symbols */
  static fapi_nr_max_num_of_symbol_per_slot_t symbols[10][14];
  fapi_nr_max_tdd_periodicity_t slots[10];
  for (int s = 0; s < 10; ++s) {
    for (int sym = 0; sym < 14; ++sym)
      symbols[s][sym].slot_config = s < 7 ? 0 : s > 7 ? 1 : sym < 6 ? 0 : sym < 10 ? 2 : 1;
    slots[s].max_num_of_symbol_per_slot_list = symbols[s];
  }
  ue->nrUE_config.tdd_table.tdd_period_in_slots = 10;
  ue->nrUE_config.tdd_table.max_tdd_periodicity_list = slots;
  ue->cont_fo_comp = cfg->rx_fo_hz != 0.0;
  ue->freq_offset = cfg->rx_fo_hz;
  nr_prach_passive_test_prior(cfg->config_index, true, false);

  unsigned ru0, c0, exp0, exp_base;
  nr_prach_passive_test_heavy_calls(&ru0, &c0, &exp_base);
  int ok = 1;
  for (int f = 0; f <= cfg->n_noise && ok; ++f) {
    const bool last = f == cfg->n_noise;
    const bool inject = last && cfg->preamble >= 0;
    for (int a = 0; a < RIQ_NRX && ok; ++a) ok = riq_load_frame(cfg->capture_dir, a, cfg->t0 + (long)f * 2 * RIQ_FRAME, rx[a]);
    if (!ok) break;
    if (inject) {
      /* the occasion window of the PRODUCTION geometry is only used to MEASURE the noise power; the preamble position is spec-derived */
      const int nta = set_default_nta_offset(FR1, fp->samples_per_subframe);
      const int slot_start = 18 * RIQ_RATE / 2;
      const int seq_start = slot_start - nta + RIQ_NCP + 64 + cfg->delay; /* +16 Ts extended CP of the first symbol of a half subframe */
      double complex *seq = malloc(sizeof(*seq) * RIQ_NU);
      riq_preamble(cfg->root, NCS, cfg->preamble, fd[cfg->fdm_index].k1, 217, seq);
      double ps = 0;
      for (int n = 0; n < RIQ_NU; ++n) ps += creal(seq[n]) * creal(seq[n]) + cimag(seq[n]) * cimag(seq[n]);
      ps /= RIQ_NU;
      for (int a = 0; a < RIQ_NRX; ++a) {
        double pn = 0;
        for (int n = 0; n < RIQ_NU; ++n) {
          const c16_t v = rx[a][seq_start - cfg->delay + n];
          pn += (double)v.r * v.r + (double)v.i * v.i;
        }
        pn /= RIQ_NU;
        if (a == 0) out->noise_rms = sqrt(pn / 2);
        const double amp = sqrt(pn * pow(10.0, cfg->snr_db / 10.0) / ps);
        const double complex rot = cexp(I * (0.7 + 1.3 * a));
        for (int n = -RIQ_NCP; n < RIQ_NU; ++n) {
          const double complex s = amp * rot * seq[(n + RIQ_NU) % RIQ_NU] * cexp(2.0 * M_PI * I * cfg->cfo_hz * (double)n / RIQ_RATE / 1000.0);
          c16_t *d = &rx[a][seq_start + n];
          const double re = d->r + creal(s), im = d->i + cimag(s);
          d->r = (int16_t)fmax(-32768, fmin(32767, lround(re)));
          d->i = (int16_t)fmax(-32768, fmin(32767, lround(im)));
        }
      }
      free(seq);
    }
    for (int slot = 7; slot < 20; ++slot) {
      if (slot % 10 < 7) continue;
      UE_nr_rxtx_proc_t proc = {.frame_rx = 2 * f, .nr_slot_rx = slot, .hfn_rx = 0,
                                .rx_slot_type = slot % 10 == 7 ? NR_MIXED_SLOT : NR_UPLINK_SLOT};
      nr_prach_passive_process(ue, &proc);
    }
    if (last && cfg->rar_on) {
      /* RA-RNTI = 1 + s_id + 14 t_id + 14*80 f_id + 14*80*8 ul_carrier; long preamble: t_id = slot of the 15 kHz grid = subframe 9 */
      const uint16_t ra_rnti = 1 + 0 + 14 * 9 + 1120 * cfg->fdm_index;
      uint8_t tb[8] = {(uint8_t)(0x40 | cfg->rar_rapid), (uint8_t)(cfg->rar_ta >> 5), (uint8_t)((cfg->rar_ta & 31) << 3), 0, 0, 0,
                       (uint8_t)(cfg->rar_tc >> 8), (uint8_t)cfg->rar_tc};
      nr_prach_passive_rar_pdu(ra_rnti, (uint64_t)(2 * f) * 20 + 19 + 4, tb, sizeof(tb));
    }
    nr_prach_passive_test_drain(); /* real frames are 20 ms apart; the queue holds 4 */
  }
  nr_prach_passive_test_heavy_calls(&ru0, &c0, &exp0);
  nr_prach_passive_test_drain();
  nr_prach_passive_flush();
  unsigned ru1, c1, exp1;
  nr_prach_passive_test_heavy_calls(&ru1, &c1, &exp1);
  (void)exp0;
  out->records = exp1 - exp_base;
  snprintf(out->record, sizeof(out->record), "%s", test_prach_record());
  out->ok = ok;
  uint64_t us, drops, skipped;
  nr_prach_passive_metrics(&us, &drops, &skipped);
  out->skipped = skipped;
  out->us_per_occasion = us;
  uint64_t occ, det;
  nr_prach_passive_diag(&occ, &det, &out->margin_max);
  out->occasions = occ;
  out->detections = det;
  nr_prach_passive_peaks_t pk;
  nr_prach_passive_diag_peaks(&pk);
  out->peak_last = pk.peak_last;
  out->floor_last = pk.floor_last;
  out->peak_max = pk.peak_max;
  out->peak_min = pk.peak_min;
  out->peak_mean = pk.peak_mean;
  out->peak_zero = pk.peak_zero;
  out->lb_tried = pk.lb_tried;
  out->lb_found = pk.lb_found;
  out->lb_noring = pk.lb_noring;
  out->snap_zero = pk.snap_zero;
  out->snap_db = pk.snap_db;
  for (int a = 0; a < RIQ_NRX; ++a) free(rx[a]);
  free(ue);
  return ok;
}
