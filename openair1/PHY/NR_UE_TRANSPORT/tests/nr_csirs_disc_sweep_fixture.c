/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* C bridge: nr_phy_common.h exposes C VLA parameters, so cannot be included by gtest/CUDA. */
#include "nr_csirs_disc.h"
#include "nr_csirs_disc_sweep_internal.h"
#include "PHY/nr_phy_common/inc/nr_phy_common.h"
#include "common/config/config_load_configmodule.h"
#include "common/utils/LOG/log.h"
#include <stdlib.h>
#include <string.h>

configmodule_interface_t *uniqCfg;
void exit_function(const char *file, const char *func, const int line, const char *message, const int assert)
{
  abort();
}
void nr_csirs_disc_test_init(void)
{
  char name[] = "csirs-disc-sweep-test";
  char *argv[] = {name, NULL};
  uniqCfg = load_configmodule(1, argv, CONFIG_ENABLECMDLINEONLY);
  logInit();
  set_glog(OAILOG_ERR);
  nr_generate_modulation_table();
}
int nr_csirs_disc_test_reference(const nr_csirs_disc_hyp_t *h, int nrb, int slot, int id, int16_t *buf)
{
  NR_DL_FRAME_PARMS fp = {.N_RB_DL = nrb, .ofdm_symbol_size = 12*nrb, .symbols_per_slot = 14, .slots_per_frame = 20};
  const csi_mapping_parms_t m = get_csi_mapping_parms(h->row, h->freq_domain, h->symb_l0, h->symb_l1);
  c16_t *planes[32];
  memset(buf, 0, (size_t)m.ports * 14*24*nrb*sizeof(*buf));
  for (int p = 0; p < m.ports; ++p)
    planes[p] = (c16_t *)buf + (size_t)p * 14*12*nrb;
  /* Include the trailing unoccupied odd RB when the generator would truncate
   * the last even-RB Gold symbol. Independent sequence checks below cover this. */
  int count = h->n_rb + (h->density == 0 && ((h->rb_start + h->n_rb) & 1));
  nr_generate_csi_rs(&fp, &m, 16384, slot, h->density, h->rb_start, count, h->symb_l0, h->symb_l1,
                     h->row, id, 1, h->cdm_type, planes);
  return m.ports;
}

/* Compare the GPU's compact mapping metadata with the actual reference at every
 * occupied RE, including CDM signs and absolute-RB Gold indices. */
int nr_csirs_disc_test_mapping(const nr_csirs_disc_hyp_t *h, int nrb, int slot, int id)
{
  nr_csirs_disc_sweep_map_t map;
  if (nr_csirs_disc_sweep_map(h, nrb, &map)) return -1;
  int plane = 14*12*nrb;
  c16_t *ref = calloc((size_t)32*plane, sizeof(*ref));
  if (!ref) { free(map.re); return -1; }
  nr_csirs_disc_test_reference(h,nrb,slot,id,(int16_t *)ref);
  int bad = 0;
  for (int p = 0; p < map.ports; ++p)
    for (int i = map.begin[p]; i < map.begin[p+1]; ++i) {
      nr_csirs_disc_re_t re = map.re[i];
      const uint32_t *gold = nr_gold_csi_rs(nrb,14,slot,re.symbol,id);
      unsigned bits = (gold[re.m / 16] >> (2*(re.m % 16))) & 3;
      int xr = (bits & 1 ? -8192 : 8192)*re.sign;
      int xi = (bits & 2 ? -8192 : 8192)*re.sign;
      c16_t actual = ref[(size_t)p*plane+re.grid_index];
      bad += actual.r != xr || actual.i != xi;
    }
  free(ref);
  free(map.re);
  return bad;
}
