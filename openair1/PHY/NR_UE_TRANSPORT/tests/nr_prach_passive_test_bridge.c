#include "nr_prach_passive_test_bridge.h"
#include "PHY/defs_gNB.h"
#include "PHY/NR_UE_TRANSPORT/nr_prach_passive.h"
#include "PHY/NR_TRANSPORT/nr_transport_common_proto.h"
#include "PHY/NR_TRANSPORT/nr_transport_proto.h"
#include "common/config/config_userapi.h"
#include "common/utils/LOG/log.h"
#include "executables/softmodem-common.h"
#include <math.h>
#include <stdlib.h>

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
