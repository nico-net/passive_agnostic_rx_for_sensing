/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Self-check of the CSI-RS generator for every row of TS 38.211 table 7.4.1.5.3-1, including the 12/16/24/32-port rows 9-18
 * (CDM4/CDM8, ISAC_CSIRS_PORTS_GT8). Plain C + assert() (nr_phy_common.h does not compile as C++).
 *  - each row maps to the spec port count; ONLY `ports` plane pointers are valid (the rest NULL), so any write to dataF[p >= ports]
 *    segfaults, and every plane carries a canary after its end (a heap overrun is caught);
 *  - per port, RE count per RB = CDM group size (row 1: 3 at density 3); union over ports = `ports` REs per RB;
 *  - symbols / subcarriers occupied match the table;
 *  - ports of one CDM group are orthogonal (CDM2/CDM4/CDM8 weights, 38.211 tables 7.4.5.3-2..5).
 * Worst case here: 32 ports, 273 PRB (4 MB of planes at 4096-FFT). */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/config/config_load_configmodule.h"
#include "common/utils/LOG/log.h"
#include "PHY/defs_nr_common.h"
#include "PHY/nr_phy_common/inc/nr_phy_common.h"

/* referenced by the LOG/CONFIG libraries from the softmodem main() */
configmodule_interface_t *uniqCfg = NULL;
void exit_function(const char *file, const char *function, const int line, const char *s, const int a)
{
  (void)file, (void)function, (void)line, (void)a;
  fprintf(stderr, "exit_function: %s\n", s ? s : "");
  abort();
}

#define NRB 273
#define FFT 4096
#define SYMBOLS 14
#define CANARY 64
#define L0 2
#define L1 8

/* per row 1..18: ports, CDM group size, set bits of the frequency-domain bitmap, symbols mask expected (l0=2, l1=8) */
static const struct {
  int ports, gs, ones;
  unsigned symmask;
} spec[18] = {
    {1, 1, 1, 1u << 2},  {1, 1, 1, 1u << 2},  {2, 2, 1, 1u << 2},  {4, 2, 1, 1u << 2},  {4, 2, 1, 3u << 2},   {8, 2, 4, 1u << 2},
    {8, 2, 2, 3u << 2},  {8, 4, 2, 3u << 2},  {12, 2, 6, 1u << 2}, {12, 4, 3, 3u << 2}, {16, 2, 4, 3u << 2},  {16, 4, 4, 3u << 2},
    {24, 2, 3, 0x30Cu},  {24, 4, 3, 0x30Cu},  {24, 8, 3, 0xFu << 2}, {32, 2, 4, 0x30Cu}, {32, 4, 4, 0x30Cu}, {32, 8, 4, 0xFu << 2},
};

int main(void)
{
  char arg0[] = "nr_csirs_ports_check";
  char *cfg_argv[] = {arg0, NULL};
  uniqCfg = load_configmodule(1, cfg_argv, CONFIG_ENABLECMDLINEONLY);
  logInit();
  set_glog(OAILOG_ERR);
  nr_generate_modulation_table();

  NR_DL_FRAME_PARMS fp;
  memset(&fp, 0, sizeof(fp));
  fp.N_RB_DL = NRB;
  fp.ofdm_symbol_size = FFT;
  fp.first_carrier_offset = FFT - (NRB * 12) / 2;
  fp.symbols_per_slot = SYMBOLS;
  fp.slots_per_frame = 20;
  fp.numerology_index = 1;
  const size_t plane = (size_t)FFT * SYMBOLS;

  for (int row = 1; row <= 18; row++) {
    const int ports = spec[row - 1].ports, gs = spec[row - 1].gs;
    const int dens = row == 1 ? 3 : 2;
    const csi_mapping_parms_t m = get_csi_mapping_parms(row, (1 << spec[row - 1].ones) - 1, L0, L1);
    assert(m.ports == ports);
    assert((m.kprime + 1) * (m.lprime + 1) == gs && (row == 1 ? m.size == 3 : m.size * gs == ports));
    c16_t *planes[33] = {0}; /* NULL after the last valid port */
    for (int p = 0; p < ports; p++) {
      planes[p] = calloc(plane + CANARY, sizeof(c16_t));
      assert(planes[p]);
      for (int i = 0; i < CANARY; i++)
        planes[p][plane + i] = (c16_t){0x5a5a, 0x5a5a};
    }
    nr_generate_csi_rs(&fp, &m, 4096, 3, dens, 0, NRB, L0, L1, row, 123, 1, row == 8 || row == 10 || row == 12 || row == 14 || row == 17 ? 2 : row == 15 || row == 18 ? 3 : row >= 3 ? 1 : 0, planes);
    unsigned symmask = 0;
    long total = 0, union_re = 0;
    for (int p = 0; p < ports; p++) {
      for (int i = 0; i < CANARY; i++)
        assert(planes[p][plane + i].r == 0x5a5a && planes[p][plane + i].i == 0x5a5a && "heap overrun past a port plane");
      long n = 0;
      for (size_t i = 0; i < plane; i++)
        if (planes[p][i].r || planes[p][i].i) {
          n++;
          symmask |= 1u << (i / FFT);
        }
      assert(n == (long)NRB * (row == 1 ? 3 : gs) && "per-port RE count != CDM group size per RB");
      total += n;
    }
    for (size_t i = 0; i < plane; i++) {
      int any = 0;
      for (int p = 0; p < ports; p++)
        any |= planes[p][i].r || planes[p][i].i;
      union_re += any;
    }
    assert(union_re == (long)NRB * (row == 1 ? 3 : ports) && "union of the ports must be `ports` REs per RB");
    assert(symmask == spec[row - 1].symmask && "occupied OFDM symbols differ from table 7.4.1.5.3-1");
    /* CDM orthogonality inside the first group of every row with gs > 1 */
    for (int a = 0; a < gs; a++)
      for (int b = a + 1; b < gs; b++) {
        double re = 0, auto_a = 0;
        for (size_t i = 0; i < plane; i++) {
          re += (double)planes[a][i].r * planes[b][i].r + (double)planes[a][i].i * planes[b][i].i;
          auto_a += (double)planes[a][i].r * planes[a][i].r + (double)planes[a][i].i * planes[a][i].i;
        }
        assert((re < 0 ? -re : re) < 1e-3 * auto_a && "ports of one CDM group are not orthogonal");
      }
    printf("row %2d: %2d ports gs=%d symbols=0x%04x REs/port=%ld: OK\n", row, ports, gs, symmask, total / ports);
    for (int p = 0; p < ports; p++)
      free(planes[p]);
  }
  printf("PASS\n");
  return 0;
}
