/* CSI-RS rate-matching extent (LOW-MCS fix, ISAC_PDSCH_CSI_RB_EXTENT).
 *
 * TS 38.211 7.4.1.5.3: a CSI-RS resource occupies only the common RBs of its CSI-FrequencyOccupation
 * (FAPI start_rb / nr_of_rbs) and, for density 0.5, only RBs of its parity. TS 38.214 5.1.4.1 rate-matches
 * the PDSCH around those REs. Checks nr_dlsch_csi_rb_mask() / nr_dlsch_csi_unav_res() (production code in
 * PHY_NR_UE) against that rule, and that the legacy (flag off) behaviour is unchanged.
 * PLAIN C: links PHY_NR_UE like test_nr_ssb_rate_match_prod. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "PHY/defs_nr_UE.h"
#include "PHY/NR_UE_TRANSPORT/nr_transport_proto_ue.h"
#include "executables/nr-uesoftmodem.h"
#include "common/config/config_load_configmodule.h"

double cpuf;
configmodule_interface_t *uniqCfg = NULL;
openair0_config_t openair0_cfg_g[MAX_CARDS] = {};
static softmodem_params_t softmodem_params;
/* get_softmodem_params: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: common/utils/ocp_itti/intertask_interface.cpp:305, common/utils/telnetsrv/telnetsrv_o1.c:302, common/utils/telnetsrv/telnetsrv_o1.c:311, common/utils/telnetsrv/telnetsrv_o1.c:320 (+248 more).
 */
softmodem_params_t *get_softmodem_params(void)
{
  return &softmodem_params;
}
static nrUE_params_t nrUE_params;
/* get_nrUE_params: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: executables/nr-ue-ru.c:151, executables/nr-ue-ru.c:153, executables/nr-ue-ru.c:167, executables/nr-ue-ru.c:174 (+90 more).
 */
nrUE_params_t *get_nrUE_params(void)
{
  return &nrUE_params;
}
_Atomic long nr_ue_diag_producer_absolute_slot = -1;
_Atomic long nr_ue_diag_producer_wall_ns = 0;
_Atomic int nr_ue_pending_rebase_valid = 0;
_Atomic long nr_ue_pending_rebase_delta = 0;
/* trs_freq_correction: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: executables/nr-ue.c:1033, openair1/PHY/NR_UE_TRANSPORT/csi_rx.c:1251, openair1/PHY/NR_UE_TRANSPORT/tests/nr_ssb_rate_match_prod_check.c:44, openair1/SCHED_NR_UE/defs.h:131 (+1 more).
 */
void trs_freq_correction(PHY_VARS_NR_UE *ue, int cfo)
{
}
/* exit_function: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: common/config/tests/test_config_cmdline.cpp:9, common/config/yaml/tests/test_yaml_config.cpp:14, common/platform_types.h:302, common/platform_types.h:306 (+94 more).
 */
void exit_function(const char *file, const char *function, const int line, const char *s, const int assert)
{
  fprintf(stderr, "exit_function %s:%d %s: %s\n", file, line, function, s);
  abort();
}

extern __thread int nr_dlsch_csi_extent_override;
static int failures;
#define CHECK(c, ...)                                             \
  do {                                                            \
    if (!(c)) {                                                   \
      failures++;                                                 \
      fprintf(stderr, "FAIL %s:%d %s: ", __FILE__, __LINE__, #c); \
      fprintf(stderr, __VA_ARGS__);                               \
      fprintf(stderr, "\n");                                      \
    }                                                             \
  } while (0)

/* trs: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/tests/nr_csi_rb_extent_check.c:85, openair1/PHY/NR_UE_TRANSPORT/tests/nr_csi_rb_extent_check.c:86, openair1/PHY/NR_UE_TRANSPORT/tests/nr_csi_rb_extent_check.c:103.
 */
static void trs(fapi_nr_dl_config_csirs_pdu_rel15_t *p, int start, int n, int l0, int fd, int density)
{
  memset(p, 0, sizeof(*p));
  p->row = 1;
  p->start_rb = start;
  p->nr_of_rbs = n;
  p->symb_l0 = l0;
  p->freq_domain = fd;
  p->freq_density = density;
}

/* alloc: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/tests/nr_csi_rb_extent_check.c:88, openair1/PHY/NR_UE_TRANSPORT/tests/nr_csi_rb_extent_check.c:97, openair1/PHY/NR_UE_TRANSPORT/tests/nr_csi_rb_extent_check.c:105.
 */
static void alloc(freq_alloc_bitmap_t *fa, int first, int n)
{
  memset(fa, 0, sizeof(*fa));
  fa->first_rb = first;
  fa->last_rb = first + n - 1;
  fa->num_rbs = n;
  for (int rb = first; rb < first + n; rb++)
    fa->bitmap[rb / 32] |= 1u << (rb % 32);
}

/* main: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: none (program entry point).
 */
int main(void)
{
  fapi_nr_dl_config_dlsch_pdu_rel15_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.BWPStart = 0;
  cfg.BWPSize = 217;
  cfg.start_symbol = 0;
  cfg.number_symbols = 14;
  cfg.numCsiRsForRateMatching = 2;
  /* The measured serving-cell TRS: RB 0-51, symbols 4 and 8, k = 0,4,8 (density 3). */
  trs(&cfg.csiRsForRateMatching[0], 0, 52, 4, 0x1, 3);
  trs(&cfg.csiRsForRateMatching[1], 0, 52, 8, 0x1, 3);
  freq_alloc_bitmap_t fa;
  alloc(&fa, 0, 176);

  nr_dlsch_csi_extent_override = 1;
  CHECK(nr_dlsch_csi_rb_mask(&cfg, 4, 10) == 0x111, "in-extent RB: 0x%x", nr_dlsch_csi_rb_mask(&cfg, 4, 10));
  CHECK(nr_dlsch_csi_rb_mask(&cfg, 4, 51) == 0x111, "last RB of the extent");
  CHECK(nr_dlsch_csi_rb_mask(&cfg, 4, 52) == 0, "first RB past the extent: 0x%x", nr_dlsch_csi_rb_mask(&cfg, 4, 52));
  CHECK(nr_dlsch_csi_rb_mask(&cfg, 5, 10) == 0, "symbol without CSI-RS");
  uint32_t n = nr_dlsch_csi_unav_res(&cfg, &fa);
  CHECK(n == 2 * 52 * 3, "extent: %u REs (want %d)", n, 2 * 52 * 3);
  alloc(&fa, 64, 16); /* a narrow grant above the TRS band loses nothing */
  n = nr_dlsch_csi_unav_res(&cfg, &fa);
  CHECK(n == 0, "grant outside the extent: %u", n);

  /* density 0.5, odd RBs only, RB 24..47 */
  cfg.numCsiRsForRateMatching = 1;
  trs(&cfg.csiRsForRateMatching[0], 24, 24, 6, 0x1, 1);
  cfg.csiRsForRateMatching[0].row = 2;
  alloc(&fa, 0, 100);
  CHECK(nr_dlsch_csi_rb_mask(&cfg, 6, 25) == 0x1 && nr_dlsch_csi_rb_mask(&cfg, 6, 26) == 0
            && nr_dlsch_csi_rb_mask(&cfg, 6, 49) == 0,
        "density 0.5 parity/extent");
  n = nr_dlsch_csi_unav_res(&cfg, &fa);
  CHECK(n == 12, "density 0.5: %u REs (want 12)", n);

  /* CSI-FULL: a segmented grant demodulated as a virtual allocation. Virtual RB v holds physical PRB map[v];
   * the extent and parity must follow the physical PRB (here virtual 0..3 = physical 25,26,60,61). */
  {
    static const uint16_t map[4] = {25, 26, 60, 61};
    nr_dlsch_csi_prb_map = map;
    nr_dlsch_csi_prb_map_n = 4;
    CHECK(nr_dlsch_csi_rb_mask(&cfg, 6, 0) == 0x1 && nr_dlsch_csi_rb_mask(&cfg, 6, 1) == 0
              && nr_dlsch_csi_rb_mask(&cfg, 6, 2) == 0 && nr_dlsch_csi_rb_mask(&cfg, 6, 3) == 0,
          "virtual->physical PRB map (extent + parity)");
    CHECK(nr_dlsch_csi_rb_mask(&cfg, 6, 27) == 0x1, "beyond the map: identity CRB");
    nr_dlsch_csi_prb_map = NULL;
    nr_dlsch_csi_prb_map_n = 0;
  }

  /* Legacy (flag off): every allocated RB of the right parity, as before. */
  nr_dlsch_csi_extent_override = 0;
  n = nr_dlsch_csi_unav_res(&cfg, &fa);
  CHECK(n == 50, "legacy density 0.5: %u REs (want 50)", n);

  printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
  return failures ? 1 : 0;
}
