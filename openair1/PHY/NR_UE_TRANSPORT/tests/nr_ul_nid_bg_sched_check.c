/* ulNidBg worker policy: SCHED_OTHER nice 19 (not SCHED_IDLE), whatever policy its creator has. The worker takes the per-cell UL-layout lock g_mu; a SCHED_IDLE holder can be
 * starved indefinitely and convoys both scan consumers of the cell. */
#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include "PHY/defs_nr_UE.h"
#include "executables/nr-uesoftmodem.h"
#include "common/config/config_load_configmodule.h"
#include "common/utils/LOG/log.h"
#include "nr_ul_nid_bg.h"

/* link stubs, as in nr_re_pattern_check.c (the PHY_NR_UE group is linked whole) */
double cpuf;
configmodule_interface_t *uniqCfg = NULL;
openair0_config_t openair0_cfg_g[MAX_CARDS] = {};
static softmodem_params_t softmodem_params;
softmodem_params_t *get_softmodem_params(void) { return &softmodem_params; }
static nrUE_params_t nrUE_params;
nrUE_params_t *get_nrUE_params(void) { return &nrUE_params; }
#include "PHY/NR_UE_TRANSPORT/nr_ue_cell_clock.h"
NR_UE_CELL_CLOCK_DEFINE_PRODUCER();
NR_UE_CELL_CLOCK_DEFINE_REBASE();
void trs_freq_correction(PHY_VARS_NR_UE *ue, int cfo) {}
void exit_function(const char *file, const char *function, const int line, const char *s, const int assert)
{
  fprintf(stderr, "exit_function %s:%d %s: %s\n", file, line, function, s);
  abort();
}

int main(void)
{
  logInit();
  set_glog(OAILOG_ERR);
  /* created by a SCHED_OTHER caller here; the worker must not depend on that */
  nr_ul_nid_bg_start(NULL, NULL);
  int pol = -1, nv = 0;
  for (int i = 0; i < 500 && !nr_ul_nid_bg_worker_sched(&pol, &nv); i++) usleep(10000);
  nr_ul_nid_bg_stop();
  if (pol < 0) { printf("FAIL worker did not start\n"); return 1; }
  printf("worker policy=%d (SCHED_OTHER=%d, SCHED_IDLE=%d) nice=%d\n", pol, SCHED_OTHER, SCHED_IDLE, nv);
  if (pol != SCHED_OTHER || nv != 19) { printf("FAIL\n"); return 1; }
  printf("PASS\n");
  return 0;
}
