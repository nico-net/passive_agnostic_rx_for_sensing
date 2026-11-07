#define _GNU_SOURCE /* dl_iterate_phdr (static-TLS guard below) */
/* The real OAI DM-RS channel estimator (nr_pdsch_channel_estimation, chest_freq 0 = linear interpolation, CPU path, 1 RX, DM-RS type 1 and 2) fed with noise only, against the noise model of
 * nr_sense_quality.c (nr_sense_quality_dmrs_model / nr_sense_quality_dmrs_rho_model): on the exported comb-2 H of interior PRBs it checks the mean and relative variance of the affine-fit residual S, the
 * relative variance of the PRB power sum T, and the ratio of the chest's own nvar to the H noise variance (rho: the SNR reference of DM-RS records is nvar / rho).
 * PLAIN C, not gtest: the library prototypes use C variably-modified array parameters. */
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
  const double rho_pred = nr_sense_quality_dmrs_rho_model(type), rho = snv / TR / v;
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

/* ---- rho (E[chest nvar] / H noise variance) on the real chest, 4 RX and 1 RX, rank 1 and 2, both types, with noise only and with a signal (flat channel with a 2-sample delay, 10 dB per RE), and
 * the null rate of the quality tests fed with the chest's own nvar. The 4-RX chest runs its antenna tasks inline (nr_passive_tpool_inline). The GPU exact chest is bit-exact with this CPU path
 * (nr_pdsch_gpu_chest.h: "bit-exact with nr_pdsch_channel_estimation()'s DMRS_TYPE1/TYPE2_linear_interp path"; asserted for nvar and the per-antenna nvar by test_nr_gpu_pass, nr_gpu_pass_test.cc
 * "chest delays/nvar differ between the models" and "per-antenna nvar (nr_dl_chest_nvar_ant)"), so the CPU measurement is the GPU's. */
#include "PHY/NR_REFSIG/nr_refsig.h"
#include "PHY/NR_UE_TRANSPORT/nr_passive_tpool.h"
typedef struct { double rho_mean, rho_lo99; long prbs_valid, prbs_tot; } rho_res_t;
static double dmrs_scale_pilots(int type, const fapi_nr_dl_config_dlsch_pdu_rel15_t *cfg, NR_DL_FRAME_PARMS *fp, int nprb, int port, c16_t *pilot, int sym)
{
  const float beta = get_beta_dmrs(cfg->n_dmrs_cdm_groups, type == 2);
  const int16_t scal = (int16_t)((1 / beta) * (1 << 14));
  const uint32_t *gold = nr_gold_pdsch(fp->N_RB_DL, fp->symbols_per_slot, cfg->dlDmrsScramblingId, cfg->nscid, 0, sym);
  nr_pdsch_dmrs_rx(fp->Ncp, gold, pilot, 1000 + port, 0, nprb, type == 1 ? NFAPI_NR_DMRS_TYPE1 : NFAPI_NR_DMRS_TYPE2, scal);
  return (double)pilot[0].r * pilot[0].r + (double)pilot[0].i * pilot[0].i;
}
/* signal: 0 noise only, 1 flat channel + noise. rank: 1 or 2 (layer 0 or 1 is estimated and measured). Returns rho with the one-sided 99 % lower bound from 10 batch means. */
static rho_res_t measure_rho(int type, int nant, int rank, int layer, int signal, int nprb, int ntrial, int run_quality)
{
  PHY_VARS_NR_UE *ue = calloc(1, sizeof(*ue));
  make_fp(&ue->frame_parms, 42, 0, nant);
  NR_DL_FRAME_PARMS *fp = &ue->frame_parms;
  init_delay_table(fp->ofdm_symbol_size, MAX_DELAY_COMP, NR_MAX_OFDM_SYMBOL_SIZE, fp->delay_table);
  ue->chest_freq = 0;
  nr_passive_tpool_inline = true;
  const UE_nr_rxtx_proc_t proc = {.frame_rx = 124, .nr_slot_rx = 0};
  fapi_nr_dl_config_dlsch_pdu_rel15_t cfg;
  memset(&cfg, 0, sizeof cfg);
  cfg.BWPStart = 0; cfg.BWPSize = NRB; cfg.start_symbol = 2; cfg.number_symbols = 12; cfg.dlDmrsSymbPos = 1 << 2;
  cfg.dmrsConfigType = type == 1 ? NFAPI_NR_DMRS_TYPE1 : NFAPI_NR_DMRS_TYPE2;
  cfg.n_dmrs_cdm_groups = 1; cfg.dmrs_ports = rank == 2 ? 3 : 1; cfg.dlDmrsScramblingId = 42; cfg.dlDataScramblingId = 42;
  freq_alloc_bitmap_t fa;
  memset(&fa, 0, sizeof fa);
  fa.n_prb_list = nprb;
  for (int i = 0; i < nprb; i++) fa.prb_list[i] = i;
  CHECK(nr_pdsch_passive_alloc_normalise(&fa, NRB), "alloc");
  fa.n_prb_list = 0;
  const uint32_t est_sz = 14 * FFT;
  const int SYM = 2, bwp0 = fp->first_carrier_offset;
  c16_t(*rx)[fp->samples_per_slot_wCP] = aligned_alloc(64, sizeof(c16_t) * fp->samples_per_slot_wCP * nant);
  int32_t(*est)[est_sz] = aligned_alloc(64, sizeof(int32_t) * est_sz * 2 * nant);
  c16_t pil[2][3280] __attribute__((aligned(16)));
  double p2[2] = {0, 0};
  for (int l = 0; l < rank; l++) p2[l] = dmrs_scale_pilots(type, &cfg, fp, nprb, get_dmrs_port(l, cfg.dmrs_ports), pil[l], SYM);
  const double SIGN = 1500.0; /* rx noise std per component */
  const double s1 = p2[0] / 32768.0;
  const double hnoise = sqrt(p2[0]) * SIGN * sqrt(2.0) / 32768.0 / sqrt(2.0); /* LS noise std (complex, after the pair mean) */
  const double hamp = 4.0 * hnoise;
  double cgain_r[2][8], cgain_i[2][8];
  for (int l = 0; l < 2; l++) for (int a = 0; a < 8; a++) { double u, v; gauss2(&u, &v); cgain_r[l][a] = hamp * u * 0.7071, cgain_i[l][a] = hamp * v * 0.7071; }
  const int npil = type == 1 ? 6 * nprb : 4 * nprb;
  const int NB = 10, per = ntrial / NB;
  double bsum_nv[10] = {0}, bsum_v[10] = {0};
  long valid = 0, tot = 0;
  for (int t = 0; t < ntrial; t++) {
    memset(rx, 0, sizeof(c16_t) * fp->samples_per_slot_wCP * nant);
    for (int a = 0; a < nant; a++) {
      for (int k = 0; k < FFT; k++) { /* noise on the DM-RS symbol only */
        double u, v; gauss2(&u, &v);
        rx[a][SYM * FFT + k] = (c16_t){(int16_t)lround(u * SIGN), (int16_t)lround(v * SIGN)};
      }
      if (signal)
        for (int m = 0; m < npil; m++) {
          const int pos = type == 1 ? 2 * m : 12 * (m / 4) + 6 * ((m % 4) / 2) + (m % 2);
          const int idx = SYM * FFT + (bwp0 + pos) % FFT;
          double sr = 0, si = 0;
          const double ph = -2 * M_PI * 2.0 * pos / FFT; /* 2-sample delay */
          for (int l = 0; l < rank; l++) {
            const double hr = cgain_r[l][a] * cos(ph) - cgain_i[l][a] * sin(ph), hi = cgain_r[l][a] * sin(ph) + cgain_i[l][a] * cos(ph);
            const double pr = pil[l][m].r, pi = -pil[l][m].i; /* conj(pilot) */
            sr += (hr * pr - hi * pi) / s1; si += (hr * pi + hi * pr) / s1;
          }
          rx[a][idx].r = (int16_t)lround(rx[a][idx].r + sr); rx[a][idx].i = (int16_t)lround(rx[a][idx].i + si);
        }
    }
    memset(est, 0, sizeof(int32_t) * est_sz * 2 * nant);
    uint32_t nvar = 0;
    nr_pdsch_channel_estimation(ue, &proc, &cfg, &fa, layer, get_dmrs_port(layer, cfg.dmrs_ports), SYM, est_sz, est, fp->samples_per_slot_wCP, rx, &nvar);
    double err = 0; long ne = 0;
    float *iq = run_quality ? malloc(sizeof(float) * 2 * nant * 6 * nprb) : NULL;
    uint16_t *kk = run_quality ? malloc(sizeof(uint16_t) * 6 * nprb) : NULL;
    uint8_t *ll = run_quality ? malloc(6 * nprb) : NULL;
    for (int a = 0; a < nant; a++) {
      const c16_t *h = (const c16_t *)&est[layer * nant + a][FFT * SYM];
      for (int p = 0; p < nprb; p++)
        for (int i = 0; i < 6; i++) {
          const int pos = 12 * p + 2 * i;
          if (run_quality) { iq[2 * (a * 6 * nprb + 6 * p + i)] = h[pos].r; iq[2 * (a * 6 * nprb + 6 * p + i) + 1] = h[pos].i; kk[6 * p + i] = (uint16_t)pos; ll[6 * p + i] = SYM; }
          if (p < 4 || p >= nprb - 4) continue;
          double er, ei;
          if (signal) {
            const double ph = -2 * M_PI * 2.0 * pos / FFT;
            const double gr = cgain_r[layer][a], gi = cgain_i[layer][a];
            er = h[pos].r - (gr * cos(ph) - gi * sin(ph)); ei = h[pos].i - (gr * sin(ph) + gi * cos(ph));
          } else er = h[pos].r, ei = h[pos].i;
          err += er * er + ei * ei; ne++;
        }
    }
    if (run_quality) {
      nr_sense_quality_t q;
      nr_sense_quality_compute_ex(nant, iq, 6 * nprb, 6 * nprb, kk, ll, (float)nvar, NR_SENSE_Q_DMRS_INTERP | (type == 2 ? NR_SENSE_Q_DMRS_T2 : 0u), &q);
      for (int p = 4; p < nprb - 4; p++) valid += (q.valid_mask[p >> 3] >> (p & 7)) & 1;
      tot += nprb - 8;
      free(iq); free(kk); free(ll);
    }
    bsum_nv[t / per] += nvar; bsum_v[t / per] += err / ne;
  }
  double r[10], m = 0;
  for (int b = 0; b < NB; b++) { r[b] = bsum_nv[b] / bsum_v[b]; m += r[b] / NB; }
  double sd = 0;
  for (int b = 0; b < NB; b++) sd += (r[b] - m) * (r[b] - m);
  sd = sqrt(sd / (NB - 1));
  free(rx); free(est); free(ue);
  return (rho_res_t){m, m - 2.821 * sd / sqrt((double)NB), valid, tot};
}
/* the rho the code uses (nr_sense_quality.c): below the lower bound of every case measured here */
static void check_rho_and_null(void)
{
  for (int type = 1; type <= 2; type++) {
    double lo_min = 1e9;
    for (int ni = 0; ni < 2; ni++)
      for (int c = 0; c < 4; c++) {
        const int nant = ni ? 4 : 1;
        const int rank = c >= 2 ? 2 : 1, layer = c == 3 ? 1 : 0, signal = c >= 1;
        rho_res_t r = measure_rho(type, nant, rank, layer, signal, 51, 300, 0);
        printf("RHO type %d, %d RX, rank %d layer %d, %s: rho %.3f, one-sided 99 %% lower bound %.3f (model %.3f, used %.3f)\n", type, nant, rank, layer, signal ? "10 dB flat channel + noise" : "noise only", r.rho_mean, r.rho_lo99,
               nr_sense_quality_dmrs_rho_model(type), nr_sense_quality_dmrs_rho(type));
        if (r.rho_lo99 < lo_min) lo_min = r.rho_lo99;
        CHECK(nr_sense_quality_dmrs_rho(type) <= r.rho_lo99, "type %d %d RX rank %d layer %d: rho used %.3f above the measured lower bound %.3f", type, nant, rank, layer, nr_sense_quality_dmrs_rho(type), r.rho_lo99);
      }
    printf("RHO type %d: smallest lower bound over the cases %.3f\n", type, lo_min);
    /* null rate of the quality tests with nvar from the real chest: >= 1e5 interior PRBs, 4 RX, rank 1 noise only */
    rho_res_t n = measure_rho(type, 4, 1, 0, 0, 100, 1200, 1);
    printf("NULL type %d, 4 RX, chest nvar: valid %ld / %ld = %.2f alpha\n", type, n.prbs_valid, n.prbs_tot, (double)n.prbs_valid / n.prbs_tot / 1e-3);
    CHECK((double)n.prbs_valid / n.prbs_tot <= 3e-3, "type %d: noise-only valid rate %.2f alpha", type, (double)n.prbs_valid / n.prbs_tot / 1e-3);
  }
}

/* Static-TLS guard. Every thread's static TLS block lives in its own stack and the SENSE_SCHED thread is created with a 4 MB stack (nr_sense_sched_rt.c; it was 2 MB until round 3, when 98 KB of static TLS in
 * the quality caches pushed nr-uesoftmodem's PT_TLS from 2.03 to 2.13 MB and pthread_create failed: "SENSE_SCHED thread not started", seen only in the replay). This executable links the same modules; the
 * guard asserts that the process's static TLS (PT_TLS of every loaded object) leaves the scheduler thread 1.5 MB of its stack (the decision's candidate table is ~250 KB), and that a thread with the
 * scheduler's stack can be created. */
#include <link.h>
#include <pthread.h>
static int tls_cb(struct dl_phdr_info *info, size_t sz, void *data)
{
  (void)sz;
  for (int i = 0; i < info->dlpi_phnum; i++)
    if (info->dlpi_phdr[i].p_type == PT_TLS) *(size_t *)data += info->dlpi_phdr[i].p_memsz;
  return 0;
}
static void *tls_probe(void *a) { (void)a; return (void *)1; }
static void check_sched_thread_stack_fits_tls(void)
{
  size_t tls = 0;
  dl_iterate_phdr(tls_cb, &tls);
  printf("TLS static TLS of this process: %zu bytes (scheduler stack 4 MB)\n", tls);
  CHECK(tls + (1536u << 10) <= (4u << 20), "static TLS %zu bytes leaves the 4 MB scheduler stack less than 1.5 MB", tls);
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, 4u << 20);
  pthread_t th;
  void *ret = NULL;
  const int rc = pthread_create(&th, &at, tls_probe, NULL);
  CHECK(rc == 0, "pthread_create with the scheduler's stack failed (%d)", rc);
  if (rc == 0) pthread_join(th, &ret);
  pthread_attr_destroy(&at);
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
  check_sched_thread_stack_fits_tls();
  check_type(1, 51);
  check_type(2, 51);
  check_rho_and_null();
  printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
  return failures ? 1 : 0;
}
