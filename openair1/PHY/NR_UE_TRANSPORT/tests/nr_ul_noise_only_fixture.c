/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* C side of nr_ul_noise_only_test.cc (the gNB headers are not C++-clean): noise-only records through the PRODUCTION PUSCH DM-RS estimator (nr_pusch_channel_estimation, the one
 * nr_pusch_passive_decode.c exports) and the production SRS estimator (nr_srs_estimate), written through the real writer quality path (sensing mode). For each record the SAME estimates
 * are also pushed as a DL source (no UL-delay flag): the false-valid rate of the DL rule on identical noise is the reference. TA ramp of the OTA runs: 1600 samples. */
#include "PHY/defs_gNB.h"
#include "PHY/NR_ESTIMATION/nr_ul_estimation.h"
#include "PHY/NR_TRANSPORT/nr_ulsch_passive_branch.h"
#include "PHY/nr_phy_common/inc/nr_phy_common.h"
#include "PHY/NR_UE_ISAC/nr_isac_mode.h"
#include "nr_sense_api.h"
#include "nr_sense_ul.h"
#include "nr_srs_blind.h"
#include "nr_srs_blind_internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define N 4096
#define NRB 217
#define FCO (N - NRB * 6)
#define NANT 4
typedef struct { uint64_t prbs, valid_ul, valid_dl; double ratio; uint64_t nratio; } ul_noise_t;
static uint64_t rngs = 0x9e3779b97f4a7c15ull;
static float gauss(void)
{
  double u = 0;
  for (int i = 0; i < 12; i++) { rngs ^= rngs << 13; rngs ^= rngs >> 7; rngs ^= rngs << 17; u += (rngs >> 11) * (1.0 / 9007199254740992.0); }
  return (float)(u - 6);
}
static int wait_q(uint64_t id, nr_sense_quality_t *q)
{
  for (int i = 0; i < 3000; i++) { if (nr_sense_quality_for_frame(id, q)) return 1; usleep(2000); }
  return 0;
}
void srs_test_init(void);
/* link stubs: the estimator under test needs the layer-0 DM-RS offset (type 1, port 0: 0) and the softmodem parameter block (zero: not passive-mode, the non-passive delay path) */
uint8_t get_delta(uint8_t ap, uint8_t config) { (void)ap; (void)config; return 0; }
/* TS 38.211 Gold sequence, c_init as refsig.c nr_gold_pdsch (refsig.c itself needs the hashtable library) */
uint32_t *nr_gold_pusch(int N_RB_UL, int symbols_per_slot, int nid, int nscid, int slot, int symbol)
{
  static __thread uint32_t g[4096 / 32 * 4 + 8];
  const int len = ((N_RB_UL * 24) >> 5) + 1;
  uint64_t x2tmp0 = (((uint64_t)symbols_per_slot * slot + symbol + 1) * (((uint64_t)nid << 1) + 1)) << 17;
  uint32_t x2 = (x2tmp0 + (nid << 1) + nscid) % (1U << 31);
  static __thread uint32_t bits[32 * (4096 / 32 * 4 + 8)];
  pseudo_random_sequence(len * 32, bits, x2);
  memset(g, 0, sizeof g);
  for (int i = 0; i < len * 32; i++) g[i / 32] |= bits[i] << (i % 32);
  return g;
}
softmodem_params_t *get_softmodem_params(void) { static char z[65536]; return (softmodem_params_t *)z; }
/* which 0 = PUSCH DM-RS noise only, 1 = positive control (PUSCH DM-RS with a 15 dB flat signal on PRBs 40..49 of the 100-PRB grant only; res->valid_ul = valid PRBs inside, valid_dl = valid PRBs outside), 2 / 4 = SRS comb. Returns 0 on success. */
int ul_noise_only_run(int which, int trials, ul_noise_t *res)
{
  *res = (ul_noise_t){0};
  srs_test_init();
  char a[] = "/tmp/ulnoise_XXXXXX", b[] = "/tmp/ulnoise_XXXXXX";
  close(mkstemp(a)); close(mkstemp(b));
  nr_isac_mode_set(NR_ISAC_MODE_SENSING);
  nr_sense_cfg_t cfg = {a, b, NULL, 16, 46200, 16};
  if (nr_sense_open(&cfg)) return 1;
  int rc = 0;
  if (which <= 1) {
    PHY_VARS_gNB *gNB = calloc(1, sizeof(PHY_VARS_gNB));
    NR_DL_FRAME_PARMS *fp = &gNB->frame_parms;
    fp->ofdm_symbol_size = N; fp->N_RB_UL = fp->N_RB_DL = NRB; fp->first_carrier_offset = FCO; fp->symbols_per_slot = 14; fp->Ncp = NORMAL;
    init_delay_table(N, MAX_DELAY_COMP, NR_MAX_OFDM_SYMBOL_SIZE, fp->delay_table);
    gNB->chest_freq = 0; gNB->dmrs_num_antennas_per_thread = NANT;
    const size_t grid = (size_t)RU_RX_SLOT_DEPTH * 14 * N;
    c16_t **rx = calloc(NANT, sizeof(c16_t *));
    int32_t *ulch[NANT];
    for (int i = 0; i < NANT; i++) { rx[i] = aligned_alloc(32, grid * sizeof(c16_t)); ulch[i] = aligned_alloc(32, 14 * N * sizeof(c16_t)); memset(rx[i], 0, grid * sizeof(c16_t)); }
    gNB->common_vars.rxdataF = rx;
    NR_gNB_PUSCH pv = {0};
    pv.ul_ch_estimates = ulch;
    const int symbol = 2, rb_size = 100, rb_start = 20;
    nfapi_nr_pusch_pdu_t pdu = {0};
    pdu.rb_size = rb_size; pdu.rb_start = rb_start; pdu.bwp_start = 0; pdu.dmrs_config_type = pusch_dmrs_type1; pdu.num_dmrs_cdm_grps_no_data = 2;
    pdu.scid = 0; pdu.ul_dmrs_scrambling_id = 923; pdu.nrOfLayers = 1; pdu.transform_precoding = transformPrecoder_disabled; pdu.param_v4.numSpatialStreamIndices = NANT;
    const int bwp_sc = (rb_start * 12 + FCO) % N, nsc = rb_size * 12;
    float *h0 = malloc(sizeof(float) * 2 * NANT * nsc), *h1 = malloc(sizeof(float) * 2 * NANT * nsc);
    uint16_t k[nsc]; uint8_t l[nsc];
    for (int t = 0; t < trials && !rc; t++) {
      for (int i = 0; i < NANT; i++) for (int j = 0; j < N; j++) rx[i][symbol * N + j] = (c16_t){(int16_t)lrintf(gauss() * 100), (int16_t)lrintf(gauss() * 100)};
      if (which == 1) { /* signal = conj(pilot) (the chest multiplies by the pilot) at the DM-RS REs of PRBs 40..49, amplitude for 15 dB over the per-RE noise (2 x 100^2) */
        c16_t pilot[3280];
        const float beta = get_beta_dmrs(pdu.num_dmrs_cdm_grps_no_data, false);
        nr_pusch_dmrs_rx(NORMAL, nr_gold_pusch(NRB, 14, 923, 0, 0, symbol), pilot, 1000, 0, rb_size, (rb_start) * 12, pusch_dmrs_type1, (int16_t)((1 / beta) * (1 << 14)));
        for (int n = 3 * 40; n < 3 * 50; n++) for (int kl = 0; kl <= 1; kl++) {
          const c16_t pl = pilot[2 * n + kl];
          const double m = sqrt((double)pl.r * pl.r + (double)pl.i * pl.i);
          if (m < 1) continue;
          const double amp = sqrt(2.0 * 100 * 100 * pow(10.0, 1.5));
          for (int i = 0; i < NANT; i++) {
            c16_t *r = &rx[i][symbol * N + (bwp_sc + (n << 2) + (kl << 1)) % N];
            r->r = (int16_t)lrint(r->r + amp * pl.r / m); r->i = (int16_t)lrint(r->i - amp * pl.i / m);
          }
        }
      }
      int max_ch = 0; uint32_t nvar = 0;
      nr_pusch_channel_estimation(gNB, 0, 0, 0, 0, symbol, &pv, 0, bwp_sc, &pdu, &max_ch, &nvar, NULL, NULL);
      const float noise_var = nr_ulsch_dmrs_noise_var((double)nvar);
      if (!(noise_var > 0)) { rc = 2; break; }
      double pw = 0;
      for (int j = 0; j < nsc; j++) { k[j] = (uint16_t)nr_sense_crb_k((bwp_sc + j) % N, FCO, N); l[j] = symbol; }
      for (int i = 0; i < NANT; i++) for (int j = 0; j < nsc; j++) {
        const c16_t h = ((c16_t *)ulch[i])[symbol * N + j];
        const size_t o = 2 * ((size_t)i * nsc + j);
        h0[o] = h.r; h0[o + 1] = h.i; h1[o] = h.r; h1[o + 1] = h.i;
        nr_sense_ul_to_dl(&h1[o], &h1[o + 1], k[j], FCO, N, 1600);
        pw += (double)h.r * h.r + (double)h.i * h.i;
      }
      res->ratio += pw / ((double)NANT * nsc) / noise_var; res->nratio++;
      nr_sense_quality_t q;
      uint64_t id = nr_sense_push_cfr(NR_SENSE_SRC_PUSCH_DMRS, NANT, h1, nsc, nsc, k, l, 2 * t, 0.f, noise_var, 0, 0, NULL);
      if (!id || !wait_q(id, &q)) { rc = 3; break; }
      res->prbs += rb_size;
      if (which == 1) { /* positive control: count the valid PRBs inside / outside the signal PRBs from the mask (bit p = PRB first_prb + p; the grant starts at PRB rb_start) */
        for (int p = 0; p < rb_size; p++) {
          const int pp = rb_start + p - q.first_prb;
          if (pp >= 0 && (q.valid_mask[pp >> 3] >> (pp & 7) & 1)) { if (p >= 40 && p < 50) res->valid_ul++; else if (p < 38 || p >= 52) res->valid_dl++; /* 2 PRBs of smoother reach around the signal are not counted */ }
        }
        continue;
      }
      res->valid_ul += q.valid_cfr_prbs;
      id = nr_sense_push_cfr(NR_SENSE_SRC_PDSCH_DMRS, NANT, h0, nsc, nsc, k, l, 2 * t + 1, 0.f, noise_var, 0, 0, NULL);
      if (!id || !wait_q(id, &q)) { rc = 3; break; }
      res->valid_dl += q.valid_cfr_prbs;
    }
    free(h0); free(h1);
  } else {
    const nr_srs_grid_t G = {N, FCO, NRB, NANT, 14, 20};
    nr_srs_blind_t *s = nr_srs_blind_create(&G);
    int16_t *iq = calloc((size_t)NANT * 14 * N * 2 + 64, sizeof(int16_t));
    float *out = malloc(sizeof(float) * 2 * NANT * NR_SRS_MAX_RE), *h1 = malloc(sizeof(float) * 2 * NANT * NR_SRS_MAX_RE);
    uint16_t *k = malloc(sizeof(uint16_t) * NR_SRS_MAX_RE); uint8_t *l = malloc(NR_SRS_MAX_RE);
    for (int t = 0; t < trials && !rc; t++) {
      for (int i = 0; i < NANT; i++) for (int j = 12 * N; j < 13 * N; j++) { iq[2 * ((size_t)i * 14 * N + j)] = (int16_t)lrintf(gauss() * 100); iq[2 * ((size_t)i * 14 * N + j) + 1] = (int16_t)lrintf(gauss() * 100); }
      nr_srs_candidate_t c = {0};
      c.comb = which; c.comb_offset = t % which; c.start_symbol = 12; c.n_symbols = 1; c.start_rb = 0; c.nb_rb = 216; c.group = t % 30; c.cs = t % (which == 2 ? 8 : 12);
      if (!nr_srs_valid_candidate(&G, &c)) continue;
      const int nre = nr_srs_estimate(s, iq, 14 * N, &c, out, k, l);
      if (!nre) continue;
      memcpy(h1, out, sizeof(float) * 2 * NANT * nre);
      double pw = 0;
      for (int i = 0; i < NANT; i++) for (int r = 0; r < nre; r++) { float *p = &h1[2 * ((size_t)i * nre + r)]; pw += (double)p[0] * p[0] + (double)p[1] * p[1]; nr_sense_ul_to_dl(p, p + 1, k[r], FCO, N, 1600); }
      res->ratio += pw / ((double)NANT * nre) / c.noise_var; res->nratio++;
      nr_sense_quality_t q;
      uint64_t id = nr_sense_push_cfr(NR_SENSE_SRC_SRS, NANT, h1, nre, nre, k, l, 2 * t, 0.857f, c.noise_var, 0, 0, NULL);
      if (!id || !wait_q(id, &q)) { rc = 3; break; }
      res->prbs += c.nb_rb; res->valid_ul += q.valid_cfr_prbs;
      id = nr_sense_push_cfr(NR_SENSE_SRC_PDSCH_DMRS, NANT, out, nre, nre, k, l, 2 * t + 1, 0.857f, c.noise_var, 0, 0, NULL);
      if (!id || !wait_q(id, &q)) { rc = 3; break; }
      res->valid_dl += q.valid_cfr_prbs;
    }
    nr_srs_blind_destroy(s);
  }
  nr_sense_close(); nr_isac_mode_set(NR_ISAC_MODE_LEGACY); unlink(a); unlink(b);
  return rc;
}
