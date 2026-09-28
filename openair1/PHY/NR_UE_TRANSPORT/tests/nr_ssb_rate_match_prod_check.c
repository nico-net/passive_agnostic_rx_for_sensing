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
_Atomic long nr_ue_diag_producer_absolute_slot = -1; // no device timing producer (as in the physims)
_Atomic long nr_ue_diag_producer_wall_ns = 0;
_Atomic int nr_ue_pending_rebase_valid = 0;
_Atomic long nr_ue_pending_rebase_delta = 0;
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

enum { FFT = 2048, NRB = 106, SSB_SC = 516, SSB_CRB0 = 43, SSB_CRB1 = 62, A = 512, TX_PCI = 42 };
enum { EST = 14 * FFT, CP = FFT / 128 * 9, CP0 = FFT / 128 * 11, MAX_RX = 2 };

/* Numerology 1, 106 PRBs, 2048-point FFT, case C. The timing fields are the ones nr_slot_fep() reads
 * (nr_init_frame_parms_ue_sa() arithmetic), with an identity symbol/timeshift rotation and the FFT
 * window at the CP end, so the test transmitter below is a plain IDFT + CP. */
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

/* TS 38.211 7.4.2.2 / 7.4.2.3, written independently of nr_ssb_rate_match.h. */
static void sync_seq(int pci, int *pss, int *sss)
{
  int x[127] = {0, 1, 1, 0, 1, 1, 1}, a[127] = {1}, b[127] = {1};
  for (int n = 0; n < 120; n++) {
    x[n + 7] = (x[n + 4] + x[n]) % 2;
    a[n + 7] = (a[n + 4] + a[n]) % 2;
    b[n + 7] = (b[n + 1] + b[n]) % 2;
  }
  const int id1 = pci / 3, id2 = pci % 3, m0 = 15 * (id1 / 112) + 5 * id2, m1 = id1 % 112;
  for (int n = 0; n < 127; n++) {
    pss[n] = 1 - 2 * x[(n + 43 * id2) % 127];
    sss[n] = (1 - 2 * a[(n + m0) % 127]) * (1 - 2 * b[(n + m1) % 127]);
  }
}

static uint32_t rng = 1;
static int bit(void)
{
  rng = rng * 1103515245u + 12345u;
  return (rng >> 16) & 1;
}

/* Seeded, platform-independent Gaussian (xorshift64* + Box-Muller) for the detection-rate check. */
static uint64_t grng = 0x9e3779b97f4a7c15ull;
static double uniform01(void)
{
  grng ^= grng >> 12;
  grng ^= grng << 25;
  grng ^= grng >> 27;
  return ((grng * 2685821657736338717ull) >> 11) * (1.0 / 9007199254740992.0);
}
static double gauss(void)
{
  const double u = uniform01() + 1e-300, v = uniform01();
  return sqrt(-2.0 * log(u)) * cos(2.0 * M_PI * v);
}

typedef struct {
  int bwp_start, bwp_size;
  int start, nsym, dmrs_sym; // DM-RS type 1, one CDM group without data: even REs of dmrs_sym
  int n_prb;
  uint16_t prb[NR_PRB_SET_MAX]; // BWP-relative, DATA order
  bool segmented;
  bool ssb_tx; // SSB transmitted at symbols 2..5 of this slot
  int rx_pci;
  uint16_t rnti;
  int ssb_sc; // SSB first subcarrier (CRB 0 reference); 0 = SSB_SC
  int nb_rx; // 0 = 1
  bool dead_ant0; // 2 RX: antenna 0 receives nothing
  int csi_sym; // > 0: a row-1 (TRS-like, density 3, REs 0/4/8 of every RB) CSI-RS rate-matching resource here
  int csi_copies; // the same resource listed this many times (NZP + ZP over the same REs); 0 = 1
} scene_t;

typedef struct {
  bool planned;
  uint16_t event_symbols;
  uint32_t unav, G, valid_sum, n_tx;
  int first;
  uint32_t dl_valid_re[NR_SYMBOLS_PER_SLOT];
  int sign_errors;
} result_t;

static int scene_ssb_sc(const scene_t *sc)
{
  return sc->ssb_sc ? sc->ssb_sc : SSB_SC;
}
static int scene_nb_rx(const scene_t *sc)
{
  return sc->nb_rx ? sc->nb_rx : 1;
}

// TS 38.214 5.1.4: every PRB the SSB touches, partial edge PRBs included, is unavailable.
static bool ssb_re(const scene_t *sc, int m, int crb)
{
  const int sc0 = scene_ssb_sc(sc);
  return sc->ssb_tx && m >= 2 && m <= 5 && crb >= sc0 / 12 && crb <= (sc0 + 239) / 12;
}

/* Transmitter: SSB (PSS/SSS + PBCH-like fill), DM-RS, and a known QPSK stream in data order, on one
 * frequency grid copied to every live receive antenna. Returns the number of data REs. */
static uint32_t transmit(const scene_t *sc, const NR_DL_FRAME_PARMS *fp, c16_t grid[][14 * FFT], c16_t *tx)
{
  const int nb_rx = scene_nb_rx(sc), ssb_sc = scene_ssb_sc(sc);
  memset(grid, 0, sizeof(c16_t) * 14 * FFT * nb_rx);
  c16_t *g = grid[nb_rx - 1];
  int pss[127], sss[127];
  sync_seq(TX_PCI, pss, sss);
  uint32_t ntx = 0;
  for (int m = sc->start; m < sc->start + sc->nsym; m++)
    for (int i = 0; i < sc->n_prb; i++) {
      const int crb = sc->bwp_start + sc->prb[i];
      for (int k = 0; k < 12; k++) {
        const int bin = (fp->first_carrier_offset + crb * 12 + k) % FFT;
        if (ssb_re(sc, m, crb))
          continue; // written below
        if (sc->csi_sym > 0 && m == sc->csi_sym && k % 4 == 0)
          continue; // CSI-RS RE: no PDSCH (TS 38.214 5.1.4.1)
        if (m == sc->dmrs_sym && !(k & 1)) {
          g[m * FFT + bin] = (c16_t){A, A};
          continue;
        }
        const c16_t x = {bit() ? -A : A, bit() ? -A : A};
        g[m * FFT + bin] = x;
        tx[ntx++] = x;
      }
    }
  if (sc->ssb_tx) {
    for (int m = 2; m <= 5; m++)
      for (int k = 0; k < 240; k++)
        g[m * FFT + (fp->first_carrier_offset + ssb_sc + k) % FFT] = (c16_t){bit() ? -A : A, bit() ? -A : A};
    for (int n = 0; n < 127; n++) {
      const int bin = (fp->first_carrier_offset + ssb_sc + 56 + n) % FFT;
      g[2 * FFT + bin] = (c16_t){pss[n] * A, 0};
      g[4 * FFT + bin] = (c16_t){sss[n] * A, 0};
    }
  }
  for (int a = 0; a < nb_rx - 1; a++)
    if (!sc->dead_ant0 || a != 0)
      memcpy(grid[a], g, sizeof(c16_t) * 14 * FFT);
  return ntx;
}

static void grant_config(const scene_t *sc, fapi_nr_dl_config_dlsch_pdu_rel15_t *cfg, freq_alloc_bitmap_t *fa)
{
  memset(cfg, 0, sizeof(*cfg));
  cfg->BWPStart = sc->bwp_start;
  cfg->BWPSize = sc->bwp_size;
  cfg->start_symbol = sc->start;
  cfg->number_symbols = sc->nsym;
  cfg->dlDmrsSymbPos = 1 << sc->dmrs_sym;
  cfg->dmrsConfigType = NFAPI_NR_DMRS_TYPE1;
  cfg->n_dmrs_cdm_groups = 1;
  cfg->dmrs_ports = 1;
  cfg->dlDmrsScramblingId = TX_PCI;
  cfg->dlDataScramblingId = TX_PCI;
  memset(fa, 0, sizeof(*fa));
  fa->n_prb_list = sc->n_prb;
  memcpy(fa->prb_list, sc->prb, sc->n_prb * sizeof(uint16_t));
  if (!nr_pdsch_passive_alloc_normalise(fa, sc->bwp_size))
    CHECK(0, "bad allocation");
  if (!sc->segmented)
    fa->n_prb_list = 0; // contiguous: the legacy first_rb..last_rb form the decoder sees
  for (int i = 0; sc->csi_sym > 0 && i < (sc->csi_copies ? sc->csi_copies : 1); i++) {
    fapi_nr_dl_config_csirs_pdu_rel15_t *c = &cfg->csiRsForRateMatching[cfg->numCsiRsForRateMatching++];
    memset(c, 0, sizeof(*c));
    c->row = 1;
    c->freq_density = 3;
    c->freq_domain = 1; // k0 = 0: REs 0, 4, 8
    c->symb_l0 = sc->csi_sym;
    c->start_rb = 0;
    c->nr_of_rbs = 275;
  }
}

static result_t run(const scene_t *sc)
{
  result_t r = {0};
  PHY_VARS_NR_UE *ue = calloc(1, sizeof(*ue));
  make_fp(&ue->frame_parms, sc->rx_pci, scene_ssb_sc(sc), scene_nb_rx(sc));
  NR_DL_FRAME_PARMS *fp = &ue->frame_parms;
  const UE_nr_rxtx_proc_t proc = {.frame_rx = 124, .nr_slot_rx = 0};

  fapi_nr_dl_config_dlsch_pdu_rel15_t cfg;
  freq_alloc_bitmap_t fa;
  grant_config(sc, &cfg, &fa);
  nr_prb_seg_t seg[NR_PRB_SET_MAX];
  const int nseg = nr_prb_segments(sc->prb, sc->n_prb, sc->bwp_start, 0, seg, NR_PRB_SET_MAX);

  c16_t(*rx)[fp->samples_per_slot_wCP] = aligned_alloc(64, sizeof(c16_t) * fp->samples_per_slot_wCP * fp->nb_antennas_rx);
  c16_t *tx = malloc(sizeof(c16_t) * NR_PRB_SET_MAX * 12 * 14);
  const uint32_t ntx = transmit(sc, fp, rx, tx);
  r.n_tx = ntx;

  // ---- the production SSB path, in the order nr_pdsch_passive_decode() runs it
  const uint16_t cand = nr_ssb_rm_candidates(fp, proc.nr_slot_rx, cfg.start_symbol, cfg.number_symbols);
  const nr_ssb_rm_event_t ev = nr_ssb_rm_observe(fp, proc.frame_rx, proc.nr_slot_rx, cand, rx);
  r.event_symbols = ev.symbols;
  nr_ssb_rm_plan_t plan;
  r.planned = nr_ssb_rm_plan(&ev, proc.frame_rx, proc.nr_slot_rx, fp->Nid_cell, sc->rnti, &cfg, &fa,
                             sc->segmented ? seg : NULL, sc->segmented ? nseg : 0, &plan);
  if (r.planned) { // a refused grant is not demodulated
    r.unav = plan.unav;
    r.first = nr_ssb_rm_first_data_symbol(&cfg, &fa, &plan);
    r.G = nr_get_G(fa.num_rbs, cfg.number_symbols, 6, 1, plan.unav, 2, 1);

    // ---- demodulate with the real nr_rx_pdsch(); a segmented grant as the decoder's virtual allocation
    freq_alloc_bitmap_t fa_dem = fa;
    if (sc->segmented) {
      int gidx[NR_PRB_SET_MAX * 12];
      const int nre = nr_prb_gather_index(seg, nseg, 12, gidx, NR_PRB_SET_MAX * 12);
      CHECK(nre == fa.num_rbs * 12, "gather %d", nre);
      c16_t *virt = aligned_alloc(64, sizeof(c16_t) * fp->samples_per_slot_wCP);
      const int off0 = fp->first_carrier_offset + cfg.BWPStart * 12;
      for (int a = 0; a < fp->nb_antennas_rx; a++) {
        memcpy(virt, rx[a], sizeof(c16_t) * fp->samples_per_slot_wCP);
        for (int m = cfg.start_symbol; m < cfg.start_symbol + cfg.number_symbols; m++)
          for (int i = 0; i < nre; i++)
            virt[m * FFT + (off0 + i) % FFT] = rx[a][m * FFT + (off0 + gidx[i]) % FFT];
        memcpy(rx[a], virt, sizeof(c16_t) * fp->samples_per_slot_wCP);
      }
      free(virt);
      fa_dem = set_bitmap_from_start_size(0, fa.num_rbs);
    }
    const uint32_t buf = (fa.num_rbs * 12 + 15) & ~15;
    int32_t(*est)[EST] = aligned_alloc(64, sizeof(int32_t) * EST * NR_MAX_NB_LAYERS);
    for (int i = 0; i < EST * NR_MAX_NB_LAYERS; i++)
      ((c16_t *)est)[i] = (c16_t){1024, 0};
    const size_t csz = sizeof(c16_t) * 14 * NR_MAX_NB_LAYERS * buf;
    c16_t(*comp)[NR_MAX_NB_LAYERS][buf] = aligned_alloc(64, csz);
    c16_t(*mag)[NR_MAX_NB_LAYERS][buf] = aligned_alloc(64, csz);
    c16_t(*magb)[NR_MAX_NB_LAYERS][buf] = aligned_alloc(64, csz);
    c16_t(*magr)[NR_MAX_NB_LAYERS][buf] = aligned_alloc(64, csz);
    memset(comp, 0, csz);
    memset(mag, 0, csz);
    memset(magb, 0, csz);
    memset(magr, 0, csz);
    int16_t *llr = aligned_alloc(64, sizeof(int16_t) * 14 * buf * 8);
    memset(llr, 0, sizeof(int16_t) * 14 * buf * 8);
    c16_t ptrs_phase[MAX_RX][NR_SYMBOLS_PER_SLOT] = {0};
    int32_t ptrs_re[MAX_RX][NR_SYMBOLS_PER_SLOT] = {0};
    NR_UE_DLSCH_t dlsch = {0};
    dlsch.cw_info.Nl = 1;
    dlsch.cw_info.qamModOrder = 2;
    dlsch.rnti = sc->rnti;
    dlsch.rnti_type = TYPE_C_RNTI_;
    NR_DL_UE_HARQ_t harq = {0};
    harq.status = NR_ACTIVE;
    pdsch_scope_req_t scope = {0};
    int32_t log2_maxh = 0;
    for (int m = cfg.start_symbol; m < cfg.start_symbol + cfg.number_symbols; m++)
      CHECK(nr_rx_pdsch(ue, &proc, &dlsch, &fa_dem, &cfg, &harq, m, m == r.first, 0, EST, est, llr, r.dl_valid_re, rx,
                        &log2_maxh, buf, fp->nb_antennas_rx, comp, mag, magb, magr, ptrs_phase, ptrs_re, 0, &scope, NULL,
                        plan.unav ? &plan.dem : NULL)
                >= 0,
            "nr_rx_pdsch symbol %d", m);
    for (int m = 0; m < NR_SYMBOLS_PER_SLOT; m++)
      r.valid_sum += r.dl_valid_re[m];
    for (uint32_t j = 0; j < ntx && j < r.valid_sum; j++)
      if ((llr[2 * j] > 0) != (tx[j].r > 0) || (llr[2 * j + 1] > 0) != (tx[j].i > 0) || !llr[2 * j] || !llr[2 * j + 1])
        r.sign_errors++;
    free(est);
    free(comp);
    free(mag);
    free(magb);
    free(magr);
    free(llr);
  }
  free(tx);
  free(rx);
  free(ue);
  return r;
}

static scene_t contiguous(int bwp_start, int bwp_size, int prb0, int n, int start, int nsym, int dmrs)
{
  scene_t sc = {.bwp_start = bwp_start, .bwp_size = bwp_size, .start = start, .nsym = nsym, .dmrs_sym = dmrs,
                .n_prb = n, .ssb_tx = true, .rx_pci = TX_PCI, .rnti = 0x1234};
  for (int i = 0; i < n; i++)
    sc.prb[i] = prb0 + i;
  return sc;
}

// Every data RE of the grant, in order, with sum(dl_valid_re)*Qm == G == the transmitter's count.
static void expect_decodable(const char *name, const result_t *r)
{
  CHECK(r->planned, "%s: refused", name);
  CHECK(r->valid_sum == r->n_tx, "%s: extracted %u REs, transmitted %u", name, r->valid_sum, r->n_tx);
  CHECK(r->G == 2 * r->n_tx, "%s: G %u, transmitted %u bits", name, r->G, 2 * r->n_tx);
  CHECK(r->sign_errors == 0, "%s: %d LLRs out of order", name, r->sign_errors);
}

/* ---- The decoder itself: nr_pdsch_passive_decode() from the time-domain samples ------------------
 * The scene is OFDM-modulated into ue->common_vars.rxdata and the real decoder runs from there, so its
 * own wiring is under test: the early PSS/SSS FEP on a slot-share miss, observe/plan, G (out->G), the
 * mask handed to nr_rx_pdsch() and the RE-budget invariant. The LDPC interface is a recorder (the
 * payload is random QPSK, so no CRC could pass anyway): it keeps the G the decoder handed it. */
void nr_pdsch_passive_set_slot_share(int on, int rb_lo, int rb_n); // nr_pdsch_passive_decode.c (no header)

static uint32_t ldpc_seen_G;
static int ldpc_calls;
static int32_t ldpc_recorder(nrLDPC_slot_decoding_parameters_t *p)
{
  ldpc_calls++;
  ldpc_seen_G = p->TBs[0].G;
  for (uint32_t r = 0; r < p->TBs[0].C; r++)
    p->TBs[0].decodeSuccess[r] = false;
  return 0;
}

typedef struct {
  nr_pdsch_passive_decode_status_t status;
  uint32_t G, ldpc_G, n_tx;
  int ldpc_calls;
  fapi_nr_dl_config_dlsch_pdu_rel15_t cfg;
  freq_alloc_bitmap_t fa;
  fapi_nr_dl_cw_info_t cw;
} decode_result_t;

static decode_result_t run_decoder(const scene_t *sc)
{
  decode_result_t d = {0};
  PHY_VARS_NR_UE *ue = calloc(1, sizeof(*ue));
  make_fp(&ue->frame_parms, sc->rx_pci, scene_ssb_sc(sc), 1);
  NR_DL_FRAME_PARMS *fp = &ue->frame_parms;
  ue->is_synchronized = 1;
  ue->nrLDPC_coding_interface.nrLDPC_coding_decoder = ldpc_recorder;
  const UE_nr_rxtx_proc_t proc = {.frame_rx = 124, .nr_slot_rx = 0};

  grant_config(sc, &d.cfg, &d.fa);
  c16_t(*grid)[14 * FFT] = aligned_alloc(64, sizeof(c16_t) * 14 * FFT);
  c16_t *tx = malloc(sizeof(c16_t) * NR_PRB_SET_MAX * 12 * 14);
  d.n_tx = transmit(sc, fp, grid, tx);

  // Slot 0 of the frame: symbol 0 carries the long CP (nr_slot_fep(), synchronised UE).
  c16_t *rxdata = aligned_alloc(64, sizeof(c16_t) * 2 * fp->samples_per_frame);
  memset(rxdata, 0, sizeof(c16_t) * 2 * fp->samples_per_frame);
  c16_t *rxdata_ant[1] = {rxdata};
  ue->common_vars.rxdata = rxdata_ant;
  c16_t *time = aligned_alloc(64, sizeof(c16_t) * FFT);
  uint32_t pos = 0;
  for (int l = 0; l < 14; l++) {
    const int cp = l == 0 ? CP0 : CP;
    pos += cp;
    idft(get_idft(FFT), (int16_t *)grid[0] + 2 * l * FFT, (int16_t *)time, 1);
    memcpy(&rxdata[pos], time, sizeof(c16_t) * FFT);
    memcpy(&rxdata[pos - cp], time + FFT - cp, sizeof(c16_t) * cp);
    pos += FFT;
  }

  nr_pdsch_passive_grant_t grant = {.check_sample_lifetime = false, .rnti = sc->rnti, .mcs = 5, .mcs_table = 0,
                                    .mcs_table_lbrm = -1};
  c16_t(*rxdataF)[fp->samples_per_slot_wCP] = aligned_alloc(64, sizeof(c16_t) * fp->samples_per_slot_wCP);
  memset(rxdataF, 0, sizeof(c16_t) * fp->samples_per_slot_wCP);
  nr_pdsch_passive_set_slot_share(0, 0, 0); // slot share off: the decoder FEPs PSS/SSS itself
  ldpc_calls = 0;
  ldpc_seen_G = 0;
  nr_pdsch_passive_decode_result_t out;
  d.status = nr_pdsch_passive_decode(ue, &proc, &d.cfg, &d.fa, &grant, rxdataF, &out);
  d.G = out.G;
  d.cw = out.cw;
  d.ldpc_G = ldpc_seen_G;
  d.ldpc_calls = ldpc_calls;
  free(rxdataF);
  free(time);
  free(rxdata);
  free(tx);
  free(grid);
  free(ue);
  return d;
}

static void expect_decoder(const char *name, const decode_result_t *d)
{
  CHECK(d->G == 2 * d->n_tx, "%s: decoder G %u, transmitted %u bits", name, d->G, 2 * d->n_tx);
  CHECK(d->status == NR_PDSCH_PASSIVE_DECODE_CRC_FAIL, "%s: decoder status %d (want CRC_FAIL %d: demodulated, G LLRs, LDPC ran)",
        name, d->status, NR_PDSCH_PASSIVE_DECODE_CRC_FAIL);
  CHECK(d->ldpc_calls > 0 && d->ldpc_G == d->G, "%s: LDPC handed G %u (%d calls), decoder G %u", name, d->ldpc_G,
        d->ldpc_calls, d->G);
}

/* ---- Detector sensitivity: nr_ssb_rm_observe() on PSS/SSS through a frequency-selective channel
 * plus AWGN at a per-RE SNR (mean |h|^2 = 1). Returns how many of `trials` slots produced an event;
 * `present` = false sends noise only. Channel per trial: two taps, the second 0.7 x the first with a
 * random phase and a delay of 8..40 samples (0.13..0.65 us at 61.44 Msps: 0.5..2.5 fading periods
 * across the 127 PSS/SSS subcarriers -- frequency-selective, never a flat fade of the whole band,
 * which is an SNR outage rather than a detector property), one random common phase, and a random
 * CFO-like phase step between the PSS and SSS symbols. Seeded: the count is deterministic. */
static int observe_trials(double snr_db, int trials, bool present, int pci_offset)
{
  NR_DL_FRAME_PARMS fp;
  make_fp(&fp, TX_PCI + pci_offset, SSB_SC, 1);
  c16_t(*rx)[fp.samples_per_slot_wCP] = aligned_alloc(64, sizeof(c16_t) * fp.samples_per_slot_wCP);
  int pss[127], sss[127];
  sync_seq(TX_PCI, pss, sss);
  const double S = 1000.0, sigma = S / sqrt(2.0 * pow(10.0, snr_db / 10.0));
  int hits = 0;
  for (int t = 0; t < trials; t++) {
    memset(rx, 0, sizeof(c16_t) * fp.samples_per_slot_wCP);
    const double d = 8 + floor(uniform01() * 33), ph1 = 2 * M_PI * uniform01(), ph0 = 2 * M_PI * uniform01();
    const double cfo = 2 * M_PI * uniform01(), norm = 1.0 / sqrt(1.0 + 0.49);
    for (int n = 0; n < 127; n++) {
      const int k = SSB_SC + 56 + n, bin = (fp.first_carrier_offset + k) % FFT;
      const double a = -2 * M_PI * k * d / FFT + ph1;
      const double hr = norm * (cos(ph0) + 0.7 * cos(a + ph0)), hi = norm * (sin(ph0) + 0.7 * sin(a + ph0));
      for (int q = 0; q < 2; q++) {
        const double sgn = present ? (q ? sss[n] : pss[n]) : 0.0, rot = q ? cfo : 0.0;
        const double yr = S * sgn * (hr * cos(rot) - hi * sin(rot)) + sigma * gauss();
        const double yi = S * sgn * (hr * sin(rot) + hi * cos(rot)) + sigma * gauss();
        rx[0][(2 + 2 * q) * FFT + bin] = (c16_t){(int16_t)lround(yr), (int16_t)lround(yi)};
      }
    }
    const nr_ssb_rm_event_t e = nr_ssb_rm_observe(&fp, 124, 0, 1u << 2, rx);
    hits += e.symbols != 0;
  }
  free(rx);
  return hits;
}

int main(void)
{
  // nr_rx_pdsch()'s LOG macros dereference global state that only logInit() sets up.
  char arg0[] = "test_nr_ssb_rate_match_prod";
  char *cfg_argv[] = {arg0, NULL};
  uniqCfg = load_configmodule(1, cfg_argv, CONFIG_ENABLECMDLINEONLY);
  logInit();
  set_glog(OAILOG_ERR);
  load_dftslib(); // the decoder's FEP and the test transmitter's IDFT

  // A: full-band grant over an observed SSB: 20 PRBs x 12 x 4 symbols leave PDSCH.
  scene_t a = contiguous(0, NRB, 0, NRB, 1, 13, 11);
  result_t ra = run(&a);
  CHECK(ra.event_symbols == 0x3c, "A: event 0x%x", ra.event_symbols);
  CHECK(ra.unav == 960, "A: unav %u", ra.unav);
  CHECK(ra.first == 1, "A: first %d", ra.first);
  expect_decodable("A", &ra);

  // B: same slot and grant, no SSB on air: nothing observed (the candidates at 2 and 8 hold data).
  scene_t b = a;
  b.ssb_tx = false;
  result_t rb = run(&b);
  CHECK(rb.event_symbols == 0, "B: event 0x%x", rb.event_symbols);
  CHECK(rb.unav == 0, "B: unav %u", rb.unav);
  expect_decodable("B", &rb);

  // C: grant entirely inside the SSB PRBs: symbols 2..5 carry no PDSCH and do not start the equaliser.
  scene_t c = contiguous(0, NRB, SSB_CRB0, SSB_CRB1 - SSB_CRB0 + 1, 2, 12, 11);
  result_t rc = run(&c);
  CHECK(rc.event_symbols == 0x3c, "C: event 0x%x", rc.event_symbols);
  CHECK(rc.first == 6, "C: first %d", rc.first);
  for (int m = 2; m <= 5; m++)
    CHECK(rc.dl_valid_re[m] == 0, "C: symbol %d has %u REs", m, rc.dl_valid_re[m]);
  CHECK(rc.dl_valid_re[6] == 240, "C: symbol 6 has %u REs", rc.dl_valid_re[6]);
  expect_decodable("C", &rc);

  // D: segmented, interleaved, in a BWP that starts at CRB 10: data order is not PRB order, and
  // CRBs 60..62 and 44..45 lie under the SSB while 63 and 10..13 do not.
  scene_t d = {.bwp_start = 10, .bwp_size = 96, .start = 1, .nsym = 13, .dmrs_sym = 11, .n_prb = 10,
               .prb = {50, 51, 52, 53, 0, 1, 2, 3, 34, 35}, .segmented = true, .ssb_tx = true, .rx_pci = TX_PCI,
               .rnti = 0x1234};
  result_t rd = run(&d);
  CHECK(rd.event_symbols == 0x3c, "D: event 0x%x", rd.event_symbols);
  CHECK(rd.unav == 5 * 12 * 4, "D: unav %u", rd.unav);
  expect_decodable("D", &rd);

  // E: DM-RS on an SSB symbol. Refused only where the grant really overlaps the SSB; a grant the
  // scheduler kept off the SSB PRBs is decoded normally although the SSB is observed in its slot.
  scene_t e1 = contiguous(0, NRB, 0, NRB, 1, 13, 2);
  result_t re1 = run(&e1);
  CHECK(!re1.planned, "E1: DM-RS on an SSB RE accepted");
  scene_t e2 = contiguous(0, NRB, 0, SSB_CRB0, 1, 13, 2);
  result_t re2 = run(&e2);
  CHECK(re2.event_symbols == 0x3c, "E2: event 0x%x", re2.event_symbols);
  CHECK(re2.unav == 0, "E2: unav %u", re2.unav);
  expect_decodable("E2", &re2);

  // F: SI-RNTI over the SSB is refused.
  scene_t f = a;
  f.rnti = 0xffff;
  CHECK(!run(&f).planned, "F: SI-RNTI over the SSB accepted");

  // G: another cell's SSB is not this cell's burst.
  scene_t g = a;
  g.rx_pci = TX_PCI + 1;
  result_t rg = run(&g);
  CHECK(rg.event_symbols == 0, "G: event 0x%x for PCI %d", rg.event_symbols, g.rx_pci);

  // M1: partial edge PRBs. The SSB starts mid-PRB (subcarrier 522 = CRB 43 + 6): CRBs 43..63, 21 PRBs.
  scene_t m1 = a;
  m1.ssb_sc = 522;
  result_t rm1 = run(&m1);
  CHECK(rm1.event_symbols == 0x3c, "M1: event 0x%x", rm1.event_symbols);
  CHECK(rm1.unav == 21 * 12 * 4, "M1: unav %u", rm1.unav);
  expect_decodable("M1", &rm1);

  // M2: two receive antennas; then antenna 0 dead, the SSB is still observed on antenna 1.
  scene_t m2 = a;
  m2.nb_rx = 2;
  result_t rm2 = run(&m2);
  CHECK(rm2.event_symbols == 0x3c, "M2: event 0x%x", rm2.event_symbols);
  CHECK(rm2.unav == 960, "M2: unav %u", rm2.unav);
  expect_decodable("M2", &rm2);
  m2.dead_ant0 = true;
  result_t rm2d = run(&m2);
  CHECK(rm2d.event_symbols == 0x3c, "M2 dead antenna 0: event 0x%x", rm2d.event_symbols);
  CHECK(rm2d.unav == 960, "M2 dead antenna 0: unav %u", rm2d.unav);
  expect_decodable("M2 dead antenna 0", &rm2d);

  // I1: the decoder's own wiring, from time-domain samples. With the SSB on air its G, the G it hands
  // the LDPC and its LLR count must all be the transmitter's; with none, likewise without a hole.
  decode_result_t da = run_decoder(&a);
  expect_decoder("decoder A", &da);
  decode_result_t db = run_decoder(&b);
  expect_decoder("decoder B", &db);
  decode_result_t dc = run_decoder(&c);
  expect_decoder("decoder C", &dc);
  /* CSI-RS rate matching on the decoder, no SSB: the LLR count must be the RE count G was computed
   * from whatever the BWP offset, the allocation shape, or a resource listed twice (NZP + ZP). */
  const struct {
    const char *name;
    int bwp_start, csi_sym, copies;
    bool segmented;
  } csi_scenes[] = {{"CSI BWP 0 last symbol", 0, 13, 1, false},
                    {"CSI BWP 10 last symbol", 10, 13, 1, false},
                    {"CSI BWP 10 symbol 12", 10, 12, 1, false},
                    {"CSI BWP 10 overlapping resources", 10, 12, 2, false},
                    {"CSI BWP 10 segmented", 10, 12, 1, true}};
  for (int i = 0; i < (int)(sizeof(csi_scenes) / sizeof(csi_scenes[0])); i++) {
    scene_t x = contiguous(csi_scenes[i].bwp_start, 96, 0, 96, 1, 13, 11);
    x.ssb_tx = false;
    x.csi_sym = csi_scenes[i].csi_sym;
    x.csi_copies = csi_scenes[i].copies;
    if (csi_scenes[i].segmented) {
      // two parity-preserving segments in data order: BWP PRBs 40..43 then 0..3
      const uint16_t prb[] = {40, 41, 42, 43, 0, 1, 2, 3};
      x.n_prb = 8;
      memcpy(x.prb, prb, sizeof(prb));
      x.segmented = true;
    }
    decode_result_t dx = run_decoder(&x);
    expect_decoder(csi_scenes[i].name, &dx);
  }

  decode_result_t de1 = run_decoder(&e1);
  CHECK(de1.status == NR_PDSCH_PASSIVE_DECODE_UNSUPPORTED && de1.ldpc_calls == 0,
        "decoder E1: DM-RS on an SSB RE not refused (status %d, %d LDPC calls)", de1.status, de1.ldpc_calls);

  // C1: the data-aided tap refuses a TB verified under a G its RE model (no SSB hole) cannot reproduce,
  // and lets through one whose G it does reproduce.
  {
    PHY_VARS_NR_UE *ue = calloc(1, sizeof(*ue));
    make_fp(&ue->frame_parms, TX_PCI, SSB_SC, 1);
    const UE_nr_rxtx_proc_t proc = {.frame_rx = 124, .nr_slot_rx = 0};
    c16_t(*rxdataF)[ue->frame_parms.samples_per_slot_wCP] = aligned_alloc(64, sizeof(c16_t) * ue->frame_parms.samples_per_slot_wCP);
    uint8_t tb[8] = {0};
    const uint64_t before = nr_isac_pdsch_data_aided_g_refused();
    // The transmitter's own bit counts, not the decoder's G: this guard is checked on its own.
    nr_isac_pdsch_data_aided_submit(ue, &proc, &da.cw, &da.cfg, &da.fa, 0x1234, tb, 7, rxdataF, 1.0, 2 * da.n_tx);
    CHECK(nr_isac_pdsch_data_aided_g_refused() == before + 1, "C1: tap accepted decode G %u over an observed SSB",
          2 * da.n_tx);
    nr_isac_pdsch_data_aided_submit(ue, &proc, &db.cw, &db.cfg, &db.fa, 0x1234, tb, 7, rxdataF, 1.0, 2 * db.n_tx);
    CHECK(nr_isac_pdsch_data_aided_g_refused() == before + 1, "C1: tap refused decode G %u with no hole", 2 * db.n_tx);
    free(rxdataF);
    free(ue);
  }

  // I2: detection rate at the stated operating point, and the false-alarm side through the same code.
  for (int snr = 0; snr <= 10; snr += 2)
    printf("SSB observe detection, 2-tap channel + AWGN, %2d dB: %d/200\n", snr, observe_trials(snr, 200, true, 0));
  const int det = observe_trials(8.0, 1000, true, 0);
  CHECK(det >= 990, "I2: %d/1000 detections at 8 dB per-RE SNR (want >= 99 %%)", det);
  const int fa_noise = observe_trials(8.0, 5000, false, 0);
  CHECK(fa_noise == 0, "I2: %d/5000 events on noise only", fa_noise);
  const int fa_pci = observe_trials(20.0, 1000, true, 1);
  CHECK(fa_pci == 0, "I2: %d/1000 events for the neighbouring PCI at 20 dB", fa_pci);

  printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
  return failures ? 1 : 0;
}
