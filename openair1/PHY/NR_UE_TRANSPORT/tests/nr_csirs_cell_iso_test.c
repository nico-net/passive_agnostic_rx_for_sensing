/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* mpci DBG2: per-cell isolation of the CSI-RS search / confirm queue (nr_csirs_blind_rt.c + nr_csirs_disc_confirm_internal.h) and of the
 * CSI-RS monitor bank (nr_csirs_monitor.c). Two cells with different grids (the 2-cell replay: 217 PRB / FFT 4096 and 51 PRB / FFT 1024,
 * scaled down here to keep the confirm-queue storage small) are prepared on one thread re-binding between calls and on two bound threads.
 * Before the fix (one process-wide instance): the second cell's prepare returned -1 because the queue was prepared for the first cell's
 * grid, its disable() switched discovery off for every cell, and a resource confirmed on cell 0 was due on cell 1's grid.
 * Plain C (the PHY headers carry C VLAs); the translation unit is included to read the private state, no test hook in production code. */
#include "../nr_csirs_blind_rt.c"
#include "executables/softmodem-common.h"
#include <stdio.h>

configmodule_interface_t *uniqCfg = NULL;
static softmodem_params_t test_params = {.passive_rx = 1};
softmodem_params_t *get_softmodem_params(void) { return &test_params; }
void exit_function(const char *file, const char *function, const int line, const char *s, const int assert)
{
  (void)file; (void)function; (void)line; (void)s; (void)assert;
  abort();
}

/* the slot FEP of the scoring path (not reached: no slot is scored here) */
int nr_slot_fep_ant(PHY_VARS_NR_UE *ue, const NR_DL_FRAME_PARMS *frame_parms, unsigned int slot, unsigned int symbol, unsigned int ant,
                    c16_t rxdataF[][frame_parms->samples_per_slot_wCP], enum nr_Link link, uint32_t sample_offset, c16_t **rxdata)
{
  (void)ue; (void)slot; (void)symbol; (void)ant; (void)rxdataF; (void)link; (void)sample_offset; (void)rxdata;
  abort();
}

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

static void make_fp(NR_DL_FRAME_PARMS *fp, int fft, int n_rb)
{
  memset(fp, 0, sizeof(*fp));
  fp->ofdm_symbol_size = fft;
  fp->nb_antennas_rx = 1;
  fp->nb_prefix_samples0 = fft / 16 + fft / 128;
  fp->nb_prefix_samples = fft / 16 + fft / 512;
  fp->samples_per_slot_wCP = 14 * fft;
  fp->N_RB_DL = n_rb;
  fp->slots_per_frame = 20;
  fp->first_carrier_offset = fft - 6 * n_rb;
}

static fapi_nr_dl_config_csirs_pdu_rel15_t trs(int start_rb, int n_rb, int id)
{
  return (fapi_nr_dl_config_csirs_pdu_rel15_t){.row = 1, .freq_domain = 4, .symb_l0 = 4, .cdm_type = 0, .freq_density = 3,
                                               .start_rb = start_rb, .nr_of_rbs = n_rb, .scramb_id = id, .csi_type = 1};
}

/* the queue and the frame of the bound cell */
static int cell_fft(void) { return discovery_queue_ready ? discovery_frame.ofdm_symbol_size : -1; }

typedef struct { int cell; const NR_DL_FRAME_PARMS *fp; int rc; } prep_job_t;
static void *prep_thread(void *arg)
{
  prep_job_t *j = arg; /* created with nr_cell_thread_create: bound to j->cell */
  j->rc = nr_csirs_blind_rt_discovery_prepare(j->fp);
  return NULL;
}

int main(void)
{
  logInit();
  NR_DL_FRAME_PARMS big, small;
  make_fp(&big, 512, 24);   /* "cell 0": the started cell, the larger grid */
  make_fp(&small, 128, 6);  /* "cell 1": the inventory cell, another grid */

  /* T1 one thread re-binding: each cell prepares its own queue for its own grid */
  nr_cell_bind(0);
  CHECK(nr_csirs_blind_rt_discovery_prepare(&big) == 0);
  nr_cell_bind(1);
  CHECK(nr_csirs_blind_rt_discovery_prepare(&small) == 0); /* process-wide queue: -1 (prepared for cell 0's grid) */
  CHECK(cell_fft() == 128);
  nr_cell_bind(0);
  CHECK(cell_fft() == 512);
  CHECK(nr_csirs_blind_rt_discovery_prepare(&big) == 0);  /* idempotent for the same grid */
  CHECK(nr_csirs_blind_rt_discovery_prepare(&small) < 0); /* still refuses another grid for the same cell */

  /* T2 a disable (failed pipeline init) of cell 1 leaves cell 0's discovery on */
  nr_cell_bind(1);
  nr_csirs_blind_rt_discovery_disable();
  CHECK(discovery_disabled);
  nr_cell_bind(0);
  CHECK(!discovery_disabled && discovery_queue_ready);

  /* T3 two bound threads prepare cells 2 and 3 concurrently: each gets its own grid */
  pthread_t t[2];
  prep_job_t jobs[2] = {{2, &big, -9}, {3, &small, -9}};
  for (int i = 0; i < 2; i++) CHECK(nr_cell_thread_create(&t[i], NULL, prep_thread, &jobs[i], jobs[i].cell) == 0);
  for (int i = 0; i < 2; i++) pthread_join(t[i], NULL);
  CHECK(jobs[0].rc == 0 && jobs[1].rc == 0);
  nr_cell_bind(2);
  CHECK(cell_fft() == 512);
  nr_cell_bind(3);
  CHECK(cell_fft() == 128);

  /* T4 monitor bank: a resource confirmed on cell 0 (RBs 128..179 of a 217-PRB grid) is due on cell 0 only */
  const fapi_nr_dl_config_csirs_pdu_rel15_t r0 = trs(128, 52, 713), r1 = trs(0, 24, 403);
  const fapi_nr_dl_config_csirs_pdu_rel15_t *out[8];
  int16_t delta[8];
  nr_cell_bind(0);
  CHECK(nr_csirs_monitor_add_dynamic(&r0, 160, 15, 0) >= 0);
  CHECK(nr_csirs_monitor_enabled() == 1);
  nr_cell_bind(1);
  CHECK(nr_csirs_monitor_enabled() == 0);
  CHECK(nr_csirs_monitor_due_ex(0, 15, 20, out, delta, 8) == 0); /* process-wide bank: 1 (cell 0's RBs on cell 1's grid) */
  CHECK(nr_csirs_monitor_add_dynamic(&r1, 40, 3, 0) >= 0);
  CHECK(nr_csirs_monitor_due_ex(0, 3, 20, out, delta, 8) == 1 && out[0]->scramb_id == 403);
  nr_cell_bind(0);
  CHECK(nr_csirs_monitor_due_ex(0, 15, 20, out, delta, 8) == 1 && out[0]->scramb_id == 713);
  CHECK(nr_csirs_monitor_due_ex(0, 3, 20, out, delta, 8) == 0);
  nr_csirs_monitor_remove_dynamic_all(); /* cell 0's epoch: cell 1 keeps its resource */
  CHECK(nr_csirs_monitor_enabled() == 0);
  nr_cell_bind(1);
  CHECK(nr_csirs_monitor_due_ex(0, 3, 20, out, delta, 8) == 1);

  printf("nr_csirs_cell_iso_test: OK\n");
  return 0;
}
