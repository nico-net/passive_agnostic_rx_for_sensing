#include "nr_prach_passive_test_bridge.h"
#include "PHY/defs_gNB.h"
#include "PHY/NR_UE_TRANSPORT/nr_prach_passive.h"
#include "PHY/NR_TRANSPORT/nr_transport_common_proto.h"
#include "PHY/NR_TRANSPORT/nr_transport_proto.h"
#include "common/config/config_userapi.h"
#include "common/utils/LOG/log.h"
#include "executables/softmodem-common.h"
#include "common/utils/nr/nr_common.h"
#include <math.h>
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
  if (!loaded) { load_dftslib(); loaded = 1; }
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

void test_prach_floor_reset(void) { nr_prach_passive_floor_reset_test(); }
int test_prach_floor(int fdm, int gain_db, int energy)
{
  return nr_prach_passive_floor_test(fdm, gain_db, energy);
}

static char captured_record[2048];
bool __wrap_nr_sense_enabled(void) { return true; }
bool __wrap_nr_sense_push_record(const char *line)
{
  snprintf(captured_record, sizeof(captured_record), "%s", line);
  return true;
}
void test_prach_reset(void)
{
  nr_prach_passive_test_reset();
  captured_record[0] = 0;
}
const char *test_prach_record(void) { return captured_record; }
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
