/* Production-path check of passive PDSCH SSB rate matching (TS 38.214 5.1.4).
 *
 * Drives the functions nr_pdsch_passive_decode() calls -- nr_ssb_rm_candidates/observe/plan/
 * first_data_symbol (nr_ssb_rate_match.c) -- and then the real nr_rx_pdsch()/nr_dlsch_extract_rbs()
 * and nr_get_G(), from a slot whose SSB is actually transmitted (PSS/SSS from an independent
 * TS 38.211 7.4.2 generator) to the LLR stream. An independent transmitter model maps a known QPSK
 * stream onto the REs TS 38.214 leaves to PDSCH; every LLR must carry its symbol's sign in order,
 * and sum(dl_valid_re) * Qm must equal G.
 *
 * PLAIN C, not gtest: nr_rx_pdsch()'s prototype uses C variably-modified array parameters. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "PHY/defs_nr_UE.h"
#include "PHY/NR_UE_TRANSPORT/nr_transport_proto_ue.h"
#include "PHY/NR_TRANSPORT/nr_transport_common_proto.h"
#include "executables/nr-uesoftmodem.h"
#include "common/config/config_load_configmodule.h"
#include "PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.h"
#include "PHY/NR_UE_TRANSPORT/nr_pdsch_data_aided.h"
#include "PHY/TOOLS/tools_defs.h"
#include <math.h>
#include <stdatomic.h>
#include "PHY/NR_UE_TRANSPORT/nr_pdsch_passive_schedule.h"

// Normally defined by the executable (nr-uesoftmodem / the physims).
double cpuf;
configmodule_interface_t *uniqCfg = NULL;
openair0_config_t openair0_cfg_g[MAX_CARDS] = {};
static softmodem_params_t softmodem_params;
softmodem_params_t *get_softmodem_params(void)
{
  return &softmodem_params;
}
static nrUE_params_t nrUE_params;
nrUE_params_t *get_nrUE_params(void)
{
  return &nrUE_params;
}
#include "PHY/NR_UE_TRANSPORT/nr_ue_cell_clock.h" /* per-cell slot clock (A8): replaces the extern of the former process global */
NR_UE_CELL_CLOCK_DEFINE_PRODUCER();
NR_UE_CELL_CLOCK_DEFINE_REBASE();
void trs_freq_correction(PHY_VARS_NR_UE *ue, int cfo)
{
}
void exit_function(const char *file, const char *function, const int line, const char *s, const int assert)
{
  fprintf(stderr, "exit_function %s:%d %s: %s\n", file, line, function, s);
  abort();
}

static int failures;
#define CHECK(c, ...)                                                      \
  do {                                                                     \
    if (!(c)) {                                                            \
      failures++;                                                          \
      fprintf(stderr, "FAIL %s:%d %s: ", __FILE__, __LINE__, #c);          \
      fprintf(stderr, __VA_ARGS__);                                        \
      fprintf(stderr, "\n");                                               \
    }                                                                      \
  } while (0)


/* The real OAI DM-RS channel estimator (nr_pdsch_channel_estimation, chest_freq 0 = linear interpolation, CPU path) fed with noise only: checks the noise model of nr_sense_quality.c
 * (nr_sense_quality_dmrs_model) against what the production chest produces, per DM-RS type: (1) the exported comb-2 H of an interior PRB has E[S]/v = mR for the affine-fit residual S and a power sum
 * T whose relative variance is 1/kT and S's is 1/kR, (2) the estimator's own nvar is rho x the H noise variance (type 1: 3.29, type 2: 1.75). Single RX antenna (direct path, no tpool). */
#include "PHY/NR_UE_TRANSPORT/nr_sense_quality.h"
enum { FFT = 2048, NRB = 106, MAX_RX = 1, CP = FFT / 128 * 9, CP0 = FFT / 128 * 11 };

static void make_fp(NR_DL_FRAME_PARMS *fp, int pci, int ssb_sc, int nb_rx)
{
  memset(fp, 0, sizeof(*fp));
  fp->numerology_index = 1;
  fp->ssb_type = nr_ssb_type_C;
  fp->Lmax = 8; // case C candidates: symbols 2 and 8 of slots 0..3 of each half frame
  fp->slots_per_frame = 20;
  fp->slots_per_subframe = 2;
  fp->symbols_per_slot = 14;
  fp->N_RB_DL = NRB;
  fp->ofdm_symbol_size = FFT;
  fp->first_carrier_offset = FFT - NRB * 6;
  fp->ssb_start_subcarrier = ssb_sc;
  fp->Nid_cell = pci;
  fp->nb_antennas_rx = nb_rx;
  fp->Ncp = NR_NORMAL;
  fp->nb_prefix_samples = CP;
  fp->nb_prefix_samples0 = CP0;
  fp->samples_per_slot_wCP = 14 * FFT;
  fp->samples_per_slotN0 = (CP + FFT) * 14;
  fp->samples_per_slot0 = CP0 + 13 * CP + 14 * FFT;
  fp->samples_per_subframe = fp->samples_per_slot0 + fp->samples_per_slotN0;
  fp->samples_per_frame = 10 * fp->samples_per_subframe;
  fp->ofdm_offset_divisor = UINT32_MAX;
  for (int l = 0; l < 3; l++)
    for (int i = 0; i < 224; i++)
      fp->symbol_rotation[l][i] = (c16_t){32767, 0};
  for (int i = 0; i < 4096 * 2; i++)
    fp->timeshift_symbol_rotation[i] = (c16_t){32767, 0};
}

static void gauss2(double *a, double *b)
{
  double u1 = (rand() + 1.0) / (RAND_MAX + 2.0), u2 = (rand() + 1.0) / (RAND_MAX + 2.0);
  const double r = sqrt(-2.0 * log(u1));
  *a = r * cos(2 * M_PI * u2), *b = r * sin(2 * M_PI * u2);
}
static void check_type(int type, int nprb)
{
  PHY_VARS_NR_UE *ue = calloc(1, sizeof(*ue));
  make_fp(&ue->frame_parms, 42, 0, 1);
  NR_DL_FRAME_PARMS *fp = &ue->frame_parms;
  init_delay_table(fp->ofdm_symbol_size, MAX_DELAY_COMP, NR_MAX_OFDM_SYMBOL_SIZE, fp->delay_table);
  ue->chest_freq = 0;
  const UE_nr_rxtx_proc_t proc = {.frame_rx = 124, .nr_slot_rx = 0};
  fapi_nr_dl_config_dlsch_pdu_rel15_t cfg;
  memset(&cfg, 0, sizeof cfg);
  cfg.BWPStart = 0; cfg.BWPSize = NRB; cfg.start_symbol = 2; cfg.number_symbols = 12; cfg.dlDmrsSymbPos = 1 << 2;
  cfg.dmrsConfigType = type == 1 ? NFAPI_NR_DMRS_TYPE1 : NFAPI_NR_DMRS_TYPE2;
  cfg.n_dmrs_cdm_groups = 1; cfg.dmrs_ports = 1; cfg.dlDmrsScramblingId = 42; cfg.dlDataScramblingId = 42;
  freq_alloc_bitmap_t fa;
  memset(&fa, 0, sizeof fa);
  fa.n_prb_list = nprb;
  for (int i = 0; i < nprb; i++) fa.prb_list[i] = i;
  CHECK(nr_pdsch_passive_alloc_normalise(&fa, NRB), "alloc");
  fa.n_prb_list = 0;
  const uint32_t est_sz = 14 * FFT;
  c16_t(*rx)[fp->samples_per_slot_wCP] = aligned_alloc(64, sizeof(c16_t) * fp->samples_per_slot_wCP);
  int32_t(*est)[est_sz] = aligned_alloc(64, sizeof(int32_t) * est_sz);
  const int TR = 400, SIG = 3000; /* noise std per component in rx LSB */
  double sT = 0, sT2 = 0, sS = 0, sS2 = 0, sv = 0, snv = 0;
  long nT = 0, nS = 0;
  for (int t = 0; t < TR; t++) {
    memset(rx, 0, sizeof(c16_t) * fp->samples_per_slot_wCP);
    for (int s = 0; s < 14; s++)
      for (int k = 0; k < FFT; k++) {
        double a, b; gauss2(&a, &b);
        rx[0][s * FFT + k] = (c16_t){(int16_t)lround(a * SIG), (int16_t)lround(b * SIG)};
      }
    memset(est, 0, sizeof(int32_t) * est_sz);
    uint32_t nvar = 0;
    nr_pdsch_channel_estimation(ue, &proc, &cfg, &fa, 0, get_dmrs_port(0, cfg.dmrs_ports), 2, est_sz, est, fp->samples_per_slot_wCP, rx, &nvar);
    const c16_t *h = (const c16_t *)&est[0][FFT * 2];
    for (int p = 4; p < nprb - 4; p++) { /* interior PRBs */
      double hr[6], hi[6], T = 0;
      for (int i = 0; i < 6; i++) { hr[i] = h[12 * p + 2 * i].r; hi[i] = h[12 * p + 2 * i].i; T += hr[i] * hr[i] + hi[i] * hi[i]; }
      double S = 0;
      for (int part = 0; part < 2; part++) {
        const double *y = part ? hi : hr;
        double m = 0, sxy = 0, sxx = 0;
        for (int i = 0; i < 6; i++) m += y[i] / 6;
        for (int i = 0; i < 6; i++) { sxy += (i - 2.5) * (y[i] - m); sxx += (i - 2.5) * (i - 2.5); }
        const double b = sxy / sxx;
        for (int i = 0; i < 6; i++) { const double e = y[i] - m - b * (i - 2.5); S += e * e; }
      }
      sT += T; sT2 += T * T; sS += S; sS2 += S * S; nT++; sv += T / 6;
    }
    snv += nvar;
  }
  nS = nT;
  const double v = sv / nT, mT = sT / nT, mS = sS / nS;
  const double relT = (sT2 / nT - mT * mT) / (mT * mT), relS = (sS2 / nS - mS * mS) / (mS * mS);
  double m[5];
  nr_sense_quality_dmrs_model(type, m);
  const double rho_pred = type == 1 ? 3.2857 : 1.75, rho = snv / TR / v;
  printf("CHEST type %d (%d PRB, %ld interior samples): E[S]/v %.4f (model %.4f) | relvar T %.4f (1/kT %.4f) | relvar S %.4f (1/kR %.4f) | nvar/v %.3f (model %.3f)\n", type, nprb, nT,
         mS / v, m[4], relT, 1 / m[0], relS, 1 / m[2], rho, rho_pred);
  /* measured 2026-11-07 on the DGX: E[S]/v is +11 % (type 1) / +9 % (type 2) over the model, relvar S +12 %: on noise only the chest's delay estimate is random and its de-rotation adds residual
   (the model has none, see nr_sense_quality.c); T's variance and nvar/v agree within 4 % */
  CHECK(fabs(mS / v / m[4] - 1) < 0.15, "type %d: E[S]/v %.4f vs model %.4f", type, mS / v, m[4]);
  CHECK(fabs(relT * m[0] - 1) < 0.12, "type %d: relvar T %.4f vs %.4f", type, relT, 1 / m[0]);
  CHECK(fabs(relS * m[2] - 1) < 0.20, "type %d: relvar S %.4f vs %.4f", type, relS, 1 / m[2]);
  CHECK(fabs(rho / rho_pred - 1) < 0.08, "type %d: nvar/v %.3f vs %.3f", type, rho, rho_pred);
  free(rx); free(est); free(ue);
}
int main(void)
{
  char arg0[] = "test_nr_sense_chest_noise";
  char *cfg_argv[] = {arg0, NULL};
  uniqCfg = load_configmodule(1, cfg_argv, CONFIG_ENABLECMDLINEONLY);
  logInit();
  set_glog(OAILOG_ERR);
  load_dftslib();
  srand(7);
  check_type(1, 51);
  check_type(2, 51);
  printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
  return failures ? 1 : 0;
}
