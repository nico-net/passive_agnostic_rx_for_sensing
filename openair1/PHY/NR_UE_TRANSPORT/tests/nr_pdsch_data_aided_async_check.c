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


/* Async vs inline data-aided export, bit for bit, Nl = 1..4. The production nr_isac_pdsch_data_aided_submit (re-encode, scramble, modulate, RE sweep, windowed LS,
 * sub-slot grouping, da_emit) runs once inline on the test thread and once through nr_isac_pdsch_data_aided_submit_async (snapshot -> daBg worker) on identical
 * inputs; the test's own nr_isac_submit_cfr_multi_meta (linked ahead of the library's) hashes every exported frame -- slot, fraction, antennas, k/l, noise, layer
 * metadata and every float of every antenna row -- and the two hash sequences must be equal. The LDPC encoder is a deterministic stand-in (the TB bytes seed the
 * coded bits): the reconstruction chain around it is the production one. Also: a saturated worker sheds with a count and never runs the tap on the caller. */
#include <pthread.h>
#include <unistd.h>
#include "PHY/NR_UE_TRANSPORT/nr_sense_api.h"
#include "PHY/NR_UE_TRANSPORT/nr_sense_records.h"
#include "PHY/NR_UE_ISAC/nr_isac.h"
#include "PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_demod_host.h"
#include "PHY/NR_REFSIG/dmrs_nr.h"
#include "PHY/NR_REFSIG/nr_refsig.h"
#include "PHY/MODULATION/nr_modulation.h"

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

static pthread_mutex_t cap_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t cap_hash[4096];
static int cap_n;
static uint32_t cap_subslot_target; /* nr_isac_subslot_config answer */
static pthread_t cap_main;
static int cap_on_main_thread;

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
  const uint8_t *b = p;
  for (size_t i = 0; i < n; i++) {
    h ^= b[i];
    h *= 1099511628211ull;
  }
  return h;
}

/* strong definitions: the test executable links with --allow-multiple-definition and this object first */
static int cap_session_open; /* the receiver calls prepare/init BEFORE the cell's session opens: enabled is false then, configured is true */
int nr_isac_source_enabled(int source)
{
  return cap_session_open && source == NR_ISAC_SRC_PDSCH_DATA;
}
int nr_isac_source_configured(int source)
{
  return source == NR_ISAC_SRC_PDSCH_DATA;
}
uint32_t nr_isac_subslot_config(uint32_t *min_re, float *min_snr_db)
{
  *min_re = 40;
  *min_snr_db = -200.0f;
  return cap_subslot_target;
}
void nr_isac_submit_cfr_multi_meta(uint32_t slot_idx, float slot_frac, int source, const nr_isac_carrier_t *carrier, const float *h, uint32_t nof_ant,
                                   uint32_t ant_stride_re, const uint32_t *k_abs, const uint32_t *l_sym, uint32_t nof_re, float noise_var,
                                   const struct nr_sense_rs_meta_s *meta)
{
  uint64_t x = 1469598103934665603ull;
  x = fnv(x, &slot_idx, sizeof(slot_idx));
  x = fnv(x, &slot_frac, sizeof(slot_frac));
  x = fnv(x, &source, sizeof(source));
  x = fnv(x, &carrier->nof_prb, sizeof(carrier->nof_prb));
  x = fnv(x, &carrier->scs_hz, sizeof(carrier->scs_hz));
  x = fnv(x, &carrier->dl_center_hz, sizeof(carrier->dl_center_hz));
  x = fnv(x, &carrier->pci, sizeof(carrier->pci));
  x = fnv(x, &carrier->slots_per_frame, sizeof(carrier->slots_per_frame));
  x = fnv(x, &nof_ant, sizeof(nof_ant));
  x = fnv(x, &nof_re, sizeof(nof_re));
  x = fnv(x, &noise_var, sizeof(noise_var)); /* NaN payload included: both runs must produce the same bits */
  x = fnv(x, &meta->rnti, sizeof(meta->rnti));
  x = fnv(x, &meta->start_rb, sizeof(int));
  x = fnv(x, &meta->nb_rb, sizeof(int));
  x = fnv(x, &meta->ports, sizeof(int));
  x = fnv(x, &meta->port_mask, sizeof(int));
  x = fnv(x, &meta->layer_idx1, sizeof(int));
  x = fnv(x, &meta->n_layers, sizeof(int));
  x = fnv(x, &meta->dmrs_type, sizeof(int));
  x = fnv(x, &meta->dmrs_n_scid, sizeof(int));
  x = fnv(x, &meta->snr_db, sizeof(float));
  x = fnv(x, k_abs, sizeof(uint32_t) * nof_re);
  x = fnv(x, l_sym, sizeof(uint32_t) * nof_re);
  for (uint32_t a = 0; a < nof_ant; a++)
    x = fnv(x, &h[2 * (size_t)a * ant_stride_re], sizeof(float) * 2 * nof_re);
  pthread_mutex_lock(&cap_mu);
  if (cap_n < (int)(sizeof(cap_hash) / sizeof(cap_hash[0])))
    cap_hash[cap_n++] = x;
  if (pthread_equal(pthread_self(), cap_main))
    cap_on_main_thread++;
  pthread_mutex_unlock(&cap_mu);
}

/* stand-in for the LDPC encoder: coded bits (packed) from a hash of the first segment, deterministic */
static int32_t fake_encoder(nrLDPC_slot_encoding_parameters_t *p)
{
  nrLDPC_TB_encoding_parameters_t *tb = p->TBs;
  uint64_t s = fnv(1469598103934665603ull, tb->segments[0].c, 64) ^ tb->G;
  for (uint32_t i = 0; i < (tb->G + 7) / 8; i++) {
    s = s * 6364136223846793005ull + 1442695040888963407ull;
    tb->output[i] = (uint8_t)(s >> 56);
  }
  return 0;
}

enum { FFT = 2048, NRB = 106, NANT = 4 };
static void make_fp2(NR_DL_FRAME_PARMS *fp)
{
  memset(fp, 0, sizeof(*fp));
  fp->numerology_index = 1;
  fp->subcarrier_spacing = 30000;
  fp->slots_per_frame = 20;
  fp->symbols_per_slot = 14;
  fp->N_RB_DL = NRB;
  fp->ofdm_symbol_size = FFT;
  fp->first_carrier_offset = FFT - NRB * 6;
  fp->Nid_cell = 415;
  fp->nb_antennas_rx = NANT;
  fp->dl_CarrierFreq = 3680010000ull;
  fp->samples_per_slot_wCP = 14 * FFT;
}

static uint32_t rnd_state = 12345;
static uint32_t rnd(void)
{
  rnd_state = rnd_state * 1664525u + 1013904223u;
  return rnd_state >> 8;
}

typedef struct {
  int nl, qm, nrb, first_rb, nsym, start_sym, bwp_start, n_prb_list, dmrs_type, cdm;
  uint32_t dmrs_pos;
} gcase_t;

static void run_case(PHY_VARS_NR_UE *ue, const gcase_t *g, uint32_t subslot, c16_t (*rx)[14 * FFT], c16_t (*rx_after)[14 * FFT])
{
  fapi_nr_dl_cw_info_t cw = {0};
  cw.Nl = g->nl;
  cw.qamModOrder = g->qm;
  cw.mcs = 10;
  cw.rv = 0;
  cw.ldpcBaseGraph = 1;
  cw.TBS = 1200;
  fapi_nr_dl_config_dlsch_pdu_rel15_t d = {0};
  d.BWPStart = g->bwp_start;
  d.start_symbol = g->start_sym;
  d.number_symbols = g->nsym;
  d.dlDmrsSymbPos = g->dmrs_pos;
  d.dmrsConfigType = g->dmrs_type;
  d.n_dmrs_cdm_groups = g->cdm;
  d.dmrs_ports = (1u << g->nl) - 1;
  d.nscid = 0;
  d.dlDataScramblingId = 415;
  d.tbslbrm = 100000;
  freq_alloc_bitmap_t fa = {0};
  fa.first_rb = g->first_rb;
  fa.num_rbs = g->nrb;
  fa.last_rb = g->first_rb + g->nrb - 1;
  if (g->n_prb_list) { /* data PRB i sits on a scattered PRB of the BWP */
    fa.n_prb_list = g->nrb;
    for (int i = 0; i < g->nrb; i++)
      fa.prb_list[i] = (uint16_t)((i * 7 + 3) % (NRB - g->bwp_start));
  }
  uint8_t tb[(1200 + 24 + 7) / 8];
  for (size_t i = 0; i < sizeof(tb); i++)
    tb[i] = (uint8_t)rnd();
  for (int a = 0; a < NANT; a++)
    for (size_t i = 0; i < 14 * FFT; i++)
      rx[a][i] = (c16_t){(int16_t)((int)(rnd() % 2001) - 1000), (int16_t)((int)(rnd() % 2001) - 1000)};
  const uint8_t nb_re_dmrs = get_num_dmrs_re_per_rb(d.dmrsConfigType, d.n_dmrs_cdm_groups);
  const uint32_t G = nr_get_G(fa.num_rbs, d.number_symbols, nb_re_dmrs, get_num_dmrs(d.dlDmrsSymbPos), 0, cw.qamModOrder, cw.Nl);
  const UE_nr_rxtx_proc_t proc = {.frame_rx = 77, .nr_slot_rx = 5};
  cap_subslot_target = subslot;

  pthread_mutex_lock(&cap_mu);
  cap_n = 0;
  pthread_mutex_unlock(&cap_mu);
  nr_isac_abs_slot_override = 123456;
  nr_isac_pdsch_data_aided_submit(ue, &proc, &cw, &d, &fa, 0x4601, tb, 9, (const c16_t(*)[14 * FFT])rx, 7.5, G, 12.25f);
  nr_isac_abs_slot_override = 0;
  const int n_inline = cap_n;
  uint64_t ref[4096];
  memcpy(ref, cap_hash, sizeof(uint64_t) * n_inline);
  pthread_mutex_lock(&cap_mu);
  cap_n = 0;
  pthread_mutex_unlock(&cap_mu);

  nr_isac_abs_slot_override = 123456;
  const int q = nr_isac_pdsch_data_aided_submit_async(ue, &proc, &cw, &d, &fa, 0x4601, tb, 9, (const c16_t(*)[14 * FFT])rx, 7.5, G, 12.25f);
  nr_isac_abs_slot_override = 0;
  memcpy(rx_after, rx, sizeof(c16_t) * NANT * 14 * FFT);
  /* the consumer reuses and overwrites its rxdataF straight away: the worker must have its own copy */
  for (int a = 0; a < NANT; a++)
    for (size_t i = 0; i < 14 * FFT; i++)
      rx[a][i] = (c16_t){0x5a5a, 0x5a5a};
  CHECK(q == 1, "Nl=%d nrb=%d: not queued", g->nl, g->nrb);
  for (int w = 0; w < 20000; w++) {
    uint64_t qd, dr, dn, sv;
    nr_isac_pdsch_data_aided_async_counts(&qd, &dr, &dn, &sv);
    if (dn >= qd)
      break;
    usleep(1000);
  }
  usleep(20000);
  CHECK(n_inline > 0, "Nl=%d nrb=%d sub=%u: the inline run exported nothing (case does not exercise the chain)", g->nl, g->nrb, subslot);
  CHECK(cap_n == n_inline, "Nl=%d nrb=%d sub=%u: %d frames inline, %d async", g->nl, g->nrb, subslot, n_inline, cap_n);
  for (int i = 0; i < n_inline && i < cap_n; i++)
    CHECK(cap_hash[i] == ref[i], "Nl=%d nrb=%d sub=%u frame %d differs", g->nl, g->nrb, subslot, i);
  CHECK(cap_on_main_thread == n_inline, "async frames were exported on the caller thread (%d on main, %d inline)", cap_on_main_thread, n_inline);
  cap_on_main_thread = 0;
  printf("Nl=%d nrb=%3d npl=%d sub=%u: %d frames identical\n", g->nl, g->nrb, g->n_prb_list, subslot, n_inline);
}

int main(void)
{
  char arg0[] = "test_nr_pdsch_data_aided_async";
  char *cfg_argv[] = {arg0, NULL};
  uniqCfg = load_configmodule(1, cfg_argv, CONFIG_ENABLECMDLINEONLY);
  logInit();
  set_glog(OAILOG_ERR);
  nr_generate_modulation_table();
  cap_main = pthread_self();
  PHY_VARS_NR_UE *ue = calloc(1, sizeof(*ue));
  make_fp2(&ue->frame_parms);
  ue->nrLDPC_coding_interface.nrLDPC_coding_encoder = fake_encoder;
  /* receiver start-up order (nr_pdsch_passive_queue.c): cell_prealloc -> _async_prepare, queue start -> _async_init, all before the session is open */
  nr_isac_pdsch_data_aided_async_prepare(0);
  nr_isac_pdsch_data_aided_async_init(ue);
  cap_session_open = 1;
  {
    uint64_t q, d, n, sv;
    nr_isac_pdsch_data_aided_async_counts(&q, &d, &n, &sv);
    CHECK(d == 0, "pool not ready after the receiver's init sequence");
  }
  c16_t(*rx)[14 * FFT] = aligned_alloc(64, sizeof(c16_t) * NANT * 14 * FFT);
  c16_t(*rx2)[14 * FFT] = aligned_alloc(64, sizeof(c16_t) * NANT * 14 * FFT);
  for (int nl = 1; nl <= 4; nl++) {
    const gcase_t cases[] = {
        {nl, 4, 24, 5, 12, 2, 0, 0, NFAPI_NR_DMRS_TYPE1, 2, 0x4},
        {nl, 6, 40, 0, 13, 1, 10, 0, NFAPI_NR_DMRS_TYPE1, 1, 0x84}, /* one CDM group: data REs share the DM-RS symbols */
        {nl, 2, 16, 3, 10, 2, 0, 1, NFAPI_NR_DMRS_TYPE2, 2, 0x4},   /* non-contiguous PRB list: carrier-band snapshot */
        {nl, 8, 90, 0, 14, 0, 0, 0, NFAPI_NR_DMRS_TYPE1, 2, 0x84},
    };
    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++)
      for (uint32_t sub = 0; sub <= 2; sub += 2)
        run_case(ue, &cases[c], sub, rx, rx2);
  }
  /* saturation: with the worker held up, the tap is shed and counted -- never run on this thread */
  {
    uint64_t q0, d0, n0, s0, q1, d1, n1, s1;
    nr_isac_pdsch_data_aided_async_counts(&q0, &d0, &n0, &s0);
    nr_sense_stats_t st0, st1;
    nr_sense_stats(&st0);
    cap_on_main_thread = 0;
    const gcase_t g = {4, 8, 90, 0, 14, 0, 0, 0, NFAPI_NR_DMRS_TYPE1, 2, 0x84};
    fapi_nr_dl_cw_info_t cw = {.Nl = 4, .qamModOrder = 8, .ldpcBaseGraph = 1, .TBS = 1200};
    fapi_nr_dl_config_dlsch_pdu_rel15_t d = {.number_symbols = 14, .dlDmrsSymbPos = 0x84, .dmrsConfigType = NFAPI_NR_DMRS_TYPE1, .n_dmrs_cdm_groups = 2, .dmrs_ports = 0xf};
    freq_alloc_bitmap_t fa = {.first_rb = 0, .num_rbs = g.nrb, .last_rb = g.nrb - 1};
    uint8_t tb[160] = {1};
    const uint32_t G = nr_get_G(fa.num_rbs, 14, get_num_dmrs_re_per_rb(d.dmrsConfigType, 2), get_num_dmrs(d.dlDmrsSymbPos), 0, 8, 4);
    const UE_nr_rxtx_proc_t proc = {.frame_rx = 1, .nr_slot_rx = 1};
    int queued = 0, shed = 0;
    for (int i = 0; i < 64; i++) { /* a burst far beyond the pool, back to back */
      const int r = nr_isac_pdsch_data_aided_submit_async(ue, &proc, &cw, &d, &fa, 0x4601, tb, 9, (const c16_t(*)[14 * FFT])rx2, 7.5, G, 12.25f);
      queued += r;
      shed += !r;
    }
    nr_isac_pdsch_data_aided_async_counts(&q1, &d1, &n1, &s1);
    nr_sense_stats(&st1);
    CHECK(shed > 0, "a 64-grant burst was not shed at all (queued %d)", queued);
    CHECK(d1 - d0 == (uint64_t)shed && q1 - q0 == (uint64_t)queued, "counters: dropped %lu vs %d, queued %lu vs %d", (unsigned long)(d1 - d0), shed,
          (unsigned long)(q1 - q0), queued);
    CHECK(st1.async_dropped[NR_SENSE_SRC_PDSCH_DATA] - st0.async_dropped[NR_SENSE_SRC_PDSCH_DATA] == (uint64_t)shed, "ISAC per-source drop counter");
    CHECK(st1.async_queued[NR_SENSE_SRC_PDSCH_DATA] - st0.async_queued[NR_SENSE_SRC_PDSCH_DATA] == (uint64_t)queued, "ISAC per-source queued counter");
    usleep(500000);
    CHECK(cap_on_main_thread == 0, "the tap ran on the caller thread under saturation");
    printf("burst of 64 x Nl4 QAM256 90 PRB: %d queued, %d shed, 0 inline\n", queued, shed);
  }
  {
    uint64_t q, d, n, sv;
    nr_isac_pdsch_data_aided_async_counts(&q, &d, &n, &sv);
    CHECK(q > 0 && n > 0, "queued %lu done %lu after the real init sequence", (unsigned long)q, (unsigned long)n);
  }
  if (failures) {
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  printf("PASS\n");
  return 0;
}
