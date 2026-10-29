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

/* A11 test helpers (C: PHY_VARS_NR_UE is not C++-clean): bring one cell's PDSCH queue up on a synthetic UE (1 RX, 106 PRB mu 1) and down again.
 * The calling thread must be bound to the cell. Returns nr_pdsch_passive_queue_recov_state() after the start, -1 when the queue did not start. */
#include "PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.h"
#include "PHY/NR_UE_TRANSPORT/nr_pdsch_cell_prealloc.h"
#include "common/utils/nr_cell.h"
static PHY_VARS_NR_UE *a11_ue[NR_CELL_MAX];
int nr_a11_queue_up(int cell)
{
  nr_pdsch_cell_prealloc(cell); /* what the cell manager does before the fence: arrays and the parked recovery worker */
  PHY_VARS_NR_UE *ue = calloc(1, sizeof(*ue));
  ue->frame_parms.nb_antennas_rx = 1;
  ue->frame_parms.N_RB_DL = 106;
  ue->frame_parms.ofdm_symbol_size = 2048;
  ue->frame_parms.symbols_per_slot = 14;
  ue->frame_parms.slots_per_frame = 20;
  ue->frame_parms.numerology_index = 1;
  ue->frame_parms.samples_per_frame = 307200;
  ue->frame_parms.samples_per_slot_wCP = 14 * 2048;
  a11_ue[cell] = ue;
  if (!nr_pdsch_passive_queue_start(ue, 8, 1, -1))
    return -1;
  return nr_pdsch_passive_queue_recov_state();
}
void nr_a11_queue_down(int cell)
{
  nr_pdsch_passive_queue_stop();
  free(a11_ue[cell]);
  a11_ue[cell] = NULL;
}
