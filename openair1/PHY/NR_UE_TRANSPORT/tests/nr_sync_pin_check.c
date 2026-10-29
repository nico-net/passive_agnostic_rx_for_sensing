/* A10 cell identity at sync: the PSS/SSS search of one cell must lock the PCI its descriptor names, even when a stronger co-channel cell sits on the same SSB.
 *
 * Production path: nr_search_ssb_common() (the PSS time search with the target N_ID_2 pin, then rx_sss_nr() with the passive N_ID_1 pin) on a synthetic capture that holds two
 * SSBs with distinct PCIs at distinct timings (two co-channel cells), the wanted one the WEAKER. The transmitter is independent (TS 38.211 7.4.2 sequences, IDFT + CP).
 * Checks: blind (-1) locks the stronger (the pre-A10 behaviour and the N = 1 behaviour without a descriptor PCI); pinned to the weaker PCI locks the weaker at its timing; pinned
 * to the stronger locks the stronger; a pin that is not on air never returns another PCI. Also the descriptor registry and its log rate limiter (ru_park.c).
 * Plain C. No environment, no option. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "PHY/defs_nr_UE.h"
#include "PHY/NR_UE_TRANSPORT/nr_transport_proto_ue.h"
#include "PHY/NR_REFSIG/sss_nr.h"
#include "executables/nr-uesoftmodem.h"
#include "common/config/config_load_configmodule.h"
#include "PHY/TOOLS/tools_defs.h"
#include "radio/COMMON/ru_park.h"


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

/* The sync search pulls the UE MAC library, whose RRC/RLC callbacks (never reached here: no PBCH is decoded, no MAC runs) live in the full receiver. */
void nr_mac_rrc_data_ind_ue() {}
void nr_mac_rrc_meas_ind_ue() {}
void nr_mac_rrc_inactivity_timer_ind() {}
void nr_mac_rrc_msg3_ind() {}
void nr_mac_rrc_ra_ind() {}
void nr_mac_rrc_verification_failed() {}
void nr_mac_rlc_data_ind() {}
void nr_mac_rlc_status_ind() {}
int nr_mac_rlc_data_req() { return 0; }
void *itti_alloc_new_message() { return NULL; }
int itti_send_msg_to_task() { return 0; }
int nr_rlc_module_init() { return 0; }
void nr_rlc_activate_srb0() {}
_Bool check_csi_report_consistency() { return 1; }

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

enum { FFT = 2048, NRB = 106, SSB_SC = 516, CP = FFT / 128 * 9, CP0 = FFT / 128 * 11 };

static void make_fp(NR_DL_FRAME_PARMS *fp)
{
  memset(fp, 0, sizeof(*fp));
  fp->numerology_index = 1;
  fp->ssb_type = nr_ssb_type_C;
  fp->Lmax = 8;
  fp->slots_per_frame = 20;
  fp->slots_per_subframe = 2;
  fp->symbols_per_slot = 14;
  fp->N_RB_DL = NRB;
  fp->ofdm_symbol_size = FFT;
  fp->first_carrier_offset = FFT - NRB * 6;
  fp->ssb_start_subcarrier = SSB_SC;
  fp->subcarrier_spacing = 30000;
  fp->nb_antennas_rx = 1;
  fp->Ncp = NR_NORMAL;
  fp->nb_prefix_samples = CP;
  fp->nb_prefix_samples0 = CP0;
  fp->samples_per_slot_wCP = 14 * FFT;
  fp->samples_per_slotN0 = (CP + FFT) * 14;
  fp->samples_per_slot0 = CP0 + 13 * CP + 14 * FFT;
  fp->samples_per_subframe = fp->samples_per_slot0 + fp->samples_per_slotN0;
  fp->samples_per_frame = 10 * fp->samples_per_subframe;
  fp->ofdm_offset_divisor = UINT32_MAX;
  fp->dl_CarrierFreq = 0; /* no carrier-phase rotation between symbols: the transmitter below is a plain IDFT + CP */
}

/* TS 38.211 7.4.2.2 / 7.4.2.3, independent of the receiver's generators. */
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

/* adds one SSB (PSS, PBCH, SSS, PBCH symbols) of cell `pci` with per-RE amplitude `amp` starting at sample `s0` (the first CP sample of the PSS symbol) */
static void add_ssb(double *re, double *im, int pci, double amp, int s0)
{
  int pss[127], sss[127];
  sync_seq(pci, pss, sss);
  const int k0 = FFT - NRB * 6 + SSB_SC; /* SSB first subcarrier on the FFT grid */
  for (int sym = 0; sym < 4; sym++) {
    double xr[FFT] = {0}, xi[FFT] = {0}; /* the symbol in the time domain, FFT part */
    for (int c = 0; c < 240; c++) {
      const int k = (k0 + c) % FFT;
      double vr = 0, vi = 0;
      if (sym == 0 && c >= 56 && c < 183)
        vr = pss[c - 56];
      else if (sym == 2 && c >= 56 && c < 183)
        vr = sss[c - 56];
      else if (sym == 1 || sym == 3 || (sym == 2 && c < 48) || (sym == 2 && c >= 192)) { /* PBCH data: random QPSK */
        vr = (uniform01() < 0.5 ? -1 : 1) * M_SQRT1_2;
        vi = (uniform01() < 0.5 ? -1 : 1) * M_SQRT1_2;
      } else
        continue;
      for (int n = 0; n < FFT; n++) {
        const double ph = 2.0 * M_PI * (double)(((long)k * n) % FFT) / FFT;
        xr[n] += amp * (vr * cos(ph) - vi * sin(ph));
        xi[n] += amp * (vr * sin(ph) + vi * cos(ph));
      }
    }
    const int base = s0 + sym * (CP + FFT);
    for (int n = 0; n < CP + FFT; n++) {
      const int src = n < CP ? FFT - CP + n : n - CP;
      re[base + n] += xr[src];
      im[base + n] += xi[src];
    }
  }
}

typedef struct {
  bool found;
  int pci, pos;
} lock_t;

static bool g_decim; /* the decimated float PSS path of the passive full search (production) or the stock int16 search */
static lock_t search(const c16_t *cap, int target)
{
  static NR_DL_FRAME_PARMS fp;
  make_fp(&fp);
  static c16_t rxdataF[NR_N_SYMBOLS_SSB][1][FFT] __attribute__((aligned(32)));
  static c16_t pssTime[NUMBER_PSS_SEQUENCE][FFT] __attribute__((aligned(32)));
  for (int n2 = 0; n2 < NUMBER_PSS_SEQUENCE; n2++)
    generate_pss_nr_time(FFT, fp.first_carrier_offset, n2, SSB_SC, pssTime[n2]);
  c16_t *rx[1] = {(c16_t *)cap};
  nr_ssb_search_params_t p = {
      .dl_CarrierFreq = fp.dl_CarrierFreq,
      .sampling_rate = fp.samples_per_subframe * 1000,
      .slots_per_frame = fp.slots_per_frame,
      .slots_per_subframe = fp.slots_per_subframe,
      .numerology_index = fp.numerology_index,
      .ofdm_symbol_size = FFT,
      .ofdm_offset_divisor = fp.ofdm_offset_divisor,
      .nb_antennas_rx = 1,
      .symbols_per_slot = fp.symbols_per_slot,
      .first_carrier_offset = fp.first_carrier_offset,
      .N_RB_DL = NRB,
      .rxdata_size = fp.samples_per_frame,
      .rxdata = rx,
      .nb_prefix_samples = CP,
      .nb_prefix_samples0 = CP0,
      .ssb_start_subcarrier = SSB_SC,
      .subcarrier_spacing = fp.subcarrier_spacing,
      .samples_per_slot_wCP = fp.samples_per_slot_wCP,
      .target_nid_cell = target,
      .exclude_nid_cells = NULL,
      .num_exclude_nid_cells = 0,
      .apply_freq_offset = false,
      .fo_flag = false,
      .rxdataF = rxdataF,
      .pssTime = pssTime,
      .pss_decim = g_decim,
  };
  lock_t l = {.found = nr_search_ssb_common(&p)};
  if (l.found) {
    l.pci = p.sss_res.nid_cell;
    l.pos = p.pss_res.pos;
  }
  return l;
}

/* a capture with cell `strong` at sample s_strong and cell `weak` at sample s_weak (amp ratio weak/strong), plus noise */
static c16_t *make_capture(int strong, int weak, double a_strong, double a_weak, int s_strong, int s_weak)
{
  NR_DL_FRAME_PARMS fp;
  make_fp(&fp);
  const int n = fp.samples_per_frame;
  double *re = calloc(n, sizeof(double)), *im = calloc(n, sizeof(double));
  c16_t *cap = calloc(n + FFT, sizeof(c16_t));
  if (strong >= 0)
    add_ssb(re, im, strong, a_strong, s_strong);
  if (weak >= 0)
    add_ssb(re, im, weak, a_weak, s_weak);
  for (int i = 0; i < n; i++) {
    cap[i].r = (int16_t)lrint(re[i] + 6.0 * gauss());
    cap[i].i = (int16_t)lrint(im[i] + 6.0 * gauss());
  }
  free(re);
  free(im);
  return cap;
}

static void pair(int strong, int weak)
{
  const int s_strong = 40000, s_weak = 190000;
  c16_t *cap = make_capture(strong, weak, 4.0, 1.3, s_strong, s_weak); /* the wanted cell is 9.8 dB weaker */
  const lock_t blind = search(cap, -1);
  printf("pair strong=%d weak=%d: blind -> found=%d pci=%d pos=%d\n", strong, weak, blind.found, blind.pci, blind.pos);
  CHECK(blind.found && blind.pci == strong, "blind search must lock the stronger cell %d, got found=%d pci=%d (pre-A10 behaviour)", strong, blind.found, blind.pci);
  CHECK(blind.pos == s_strong + CP, "blind lock position %d, expected %d", blind.pos, s_strong + CP);

  const lock_t pw = search(cap, weak);
  printf("  pinned %d -> found=%d pci=%d pos=%d\n", weak, pw.found, pw.pci, pw.pos);
  CHECK(pw.found && pw.pci == weak, "pinned search must lock the weaker wanted cell %d, got found=%d pci=%d", weak, pw.found, pw.pci);
  CHECK(pw.pos == s_weak + CP, "pinned lock position %d, expected %d", pw.pos, s_weak + CP);

  const lock_t ps = search(cap, strong);
  CHECK(ps.found && ps.pci == strong && ps.pos == s_strong + CP, "pinned to the stronger cell: found=%d pci=%d pos=%d", ps.found, ps.pci, ps.pos);

  /* a pin that is not on air (other N_ID_2 and the same N_ID_2 as each cell) must never return another PCI */
  for (int d = 1; d <= 3; d++) {
    const int absent = (weak + d * 7) % 1008;
    if (absent == strong || absent == weak)
      continue;
    const lock_t pa = search(cap, absent);
    CHECK(!pa.found || pa.pci == absent, "pin %d (absent): returned pci=%d", absent, pa.pci);
  }
  free(cap);

  /* N = 1 without a descriptor PCI: one cell on air, blind search unchanged */
  c16_t *one = make_capture(weak, -1, 1.3, 0, s_weak, 0);
  const lock_t b1 = search(one, -1);
  CHECK(b1.found && b1.pci == weak && b1.pos == s_weak + CP, "single cell, blind: found=%d pci=%d pos=%d", b1.found, b1.pci, b1.pos);
  const lock_t p1 = search(one, weak);
  CHECK(p1.found && p1.pci == weak && p1.pos == s_weak + CP, "single cell, pinned: found=%d pci=%d pos=%d", p1.found, p1.pci, p1.pos);
  free(one);
}

static void registry(void)
{
  ru_cell_want_reset();
  for (int c = 0; c < 4; c++)
    CHECK(ru_cell_want_pci(c) == -1, "default want of cell %d is %d", c, ru_cell_want_pci(c));
  ru_cell_want_pci_set(1, 415);
  ru_cell_want_pci_set(0, 0); /* PCI 0 is a PCI, not 'unknown' */
  CHECK(ru_cell_want_pci(1) == 415 && ru_cell_want_pci(0) == 0 && ru_cell_want_pci(2) == -1, "want registry: %d %d %d", ru_cell_want_pci(0), ru_cell_want_pci(1), ru_cell_want_pci(2));
  ru_cell_want_pci_set(1, -1);
  CHECK(ru_cell_want_pci(1) == -1, "descriptor PCI cleared");
  /* rate limit: first call due, then not within the window, due again after it; cells and kinds are independent */
  const long long W = 10000000000LL;
  CHECK(ru_cell_sync_note_due(0, 0, 1000, W) == 1, "first note due");
  CHECK(ru_cell_sync_note_due(0, 0, 1000 + W - 1, W) == 0, "second note inside the window suppressed");
  CHECK(ru_cell_sync_note_due(0, 1, 1000 + 5, W) == 1, "other kind independent");
  CHECK(ru_cell_sync_note_due(1, 0, 1000 + 5, W) == 1, "other cell independent");
  CHECK(ru_cell_sync_note_due(0, 0, 1000 + W, W) == 1, "due again after the window");
  ru_cell_want_reset();
}

int main(void)
{
  char arg0[] = "test_nr_sync_pin";
  char *cfg_argv[] = {arg0, NULL};
  uniqCfg = load_configmodule(1, cfg_argv, CONFIG_ENABLECMDLINEONLY);
  logInit();
  set_glog(OAILOG_ERR);
  load_dftslib();
  softmodem_params.passive_rx = 1; /* the receiver is passive: the SSS search honours the PCI pin (N_ID_1) */

  for (int d = 0; d < 2; d++) {
    g_decim = d;
    printf("PSS path: %s\n", d ? "decimated float (passive production)" : "stock int16");
    pair(380, 415); /* 2026-10-28 sw28_P1 c0: the wanted 415 was weaker than the co-channel 380 */
    pair(27, 403);  /* mp28m_Q1 c1: the wanted 403 was weaker than the co-channel 27 */
  }
  registry();
  if (failures) {
    printf("test_nr_sync_pin: %d FAILURES\n", failures);
    return 1;
  }
  printf("test_nr_sync_pin: all checks passed\n");
  return 0;
}
