/* Stubs for nr_pusch_uci_blind.c in test_nr_uci_small: a single code block and no LDPC (the TB callback of the test is the oracle). */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include "PHY/CODING/coding_defs.h"
#include "PHY/NR_TRANSPORT/nr_transport_common_proto.h"
#include "nr_pdsch_passive_decode.h"
#include "nr_td_grantwork.h"
#include "common/config/config_userapi.h"

configmodule_interface_t *uniqCfg = NULL;
void exit_function(const char *file, const char *function, const int line, const char *s, const int assert)
{
  (void)file; (void)function; (void)line; (void)s; (void)assert;
  abort();
}

int32_t nr_segmentation(unsigned char *input_buffer, unsigned char **output_buffers, unsigned int B, unsigned int *C, unsigned int *K,
                        unsigned int *Zout, unsigned int *F, uint8_t BG)
{
  (void)input_buffer; (void)output_buffers; (void)B; (void)BG;
  *C = 1;
  *K = 2560;
  *Zout = 128;
  *F = 0;
  return 0;
}
int nr_get_E(uint32_t G, uint8_t C, uint8_t Qm, uint8_t Nl, uint8_t r)
{
  (void)C; (void)Qm; (void)Nl; (void)r;
  return (int)G;
}
int nr_td_gw_cb0_extract(const int16_t *llr, uint32_t n_llr, const nr_td_cb0_params_t *p, int16_t *d)
{
  (void)llr; (void)n_llr; (void)p; (void)d;
  return NR_TD_GW_OK;
}
bool nr_pdsch_passive_gw_cb_decode_iter(const nr_pdsch_gw_cb0_t *cb, int max_iter, bool *ldpc_ok)
{
  (void)cb; (void)max_iter;
  *ldpc_ok = false;
  return true;
}
