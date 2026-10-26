/* mpci A5 test: symbols the executable (nr-uesoftmodem / the physims) normally defines, needed to link nr_pdsch_passive_decode.c / queue.c in a unit test. */
#include <stdatomic.h>
#include "PHY/defs_nr_UE.h"
#include "executables/nr-uesoftmodem.h"

static nrUE_params_t nrUE_params;
nrUE_params_t *get_nrUE_params(void)
{
  return &nrUE_params;
}
#include "PHY/NR_UE_TRANSPORT/nr_ue_cell_clock.h" /* per-cell slot clock (A8): replaces the extern of the former process global */
NR_UE_CELL_CLOCK_DEFINE_PRODUCER();

double cpuf;
openair0_config_t openair0_cfg_g[MAX_CARDS];
static softmodem_params_t softmodem_params;
softmodem_params_t *get_softmodem_params(void)
{
  return &softmodem_params;
}
