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
#include "PHY/NR_UE_TRANSPORT/nr_initial_sync_budget.h"
#include "PHY/NR_REFSIG/sss_nr.h"
#include "PHY/NR_UE_ESTIMATION/nr_estimation.h"
#include "PHY/NR_REFSIG/nr_refsig.h"
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
  int fo_pss, fo_sss;
  int n_alt, alt_pci[3], alt_pos[3]; /* A3: every accepted candidate of a blind pass, strongest SSS metric first */
  int32_t alt_metric[3];
  int sel_pci, sel_pos; /* after nr_search_ssb_select(1) */
  bool sel_ok;
} lock_t;

static nr_ssb_search_params_t g_last_p; /* the parameters (with the PSS/SSS results) of the last search(), for the re-evaluation seam test */
static bool g_fo; /* A1 test: the estimators run (fo_flag) and the PSS estimate is compensated in place, as in nr_scan_ssb() */
static bool g_decim; /* the decimated float PSS path of the passive full search (production) or the stock int16 search */
static lock_t search(const c16_t *cap, int target)
{
  static NR_DL_FRAME_PARMS fp;
  make_fp(&fp);
  static c16_t rxdataF[NR_N_SYMBOLS_SSB][1][FFT] __attribute__((aligned(32)));
  static c16_t pssTime[NUMBER_PSS_SEQUENCE][FFT] __attribute__((aligned(32)));
  for (int n2 = 0; n2 < NUMBER_PSS_SEQUENCE; n2++)
    generate_pss_nr_time(FFT, fp.first_carrier_offset, n2, SSB_SC, pssTime[n2]);
  static c16_t *rx[1];
  rx[0] = (c16_t *)cap;
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
      .apply_freq_offset = g_fo,
      .fo_flag = g_fo,
      .rxdataF = rxdataF,
      .pssTime = pssTime,
      .pss_decim = g_decim,
  };
  lock_t l = {.found = nr_search_ssb_common(&p)};
  g_last_p = p;
  if (l.found) {
    l.pci = p.sss_res.nid_cell;
    l.pos = p.pss_res.pos;
    l.fo_pss = p.pss_res.freq_offset;
    l.fo_sss = p.sss_res.freq_offset;
    l.n_alt = p.n_alt;
    for (int a = 0; a < p.n_alt; a++) {
      l.alt_pci[a] = p.alt_sss[a].nid_cell;
      l.alt_pos[a] = p.alt_pss[a].pos;
      l.alt_metric[a] = p.alt_sss[a].metric;
    }
    if (p.n_alt > 1) {
      l.sel_ok = nr_search_ssb_select(&p, 1);
      l.sel_pci = p.sss_res.nid_cell;
      l.sel_pos = p.pss_res.pos;
    }
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
  CHECK(pw.n_alt == 1, "pinned search keeps its single best, n_alt=%d", pw.n_alt);
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

/* A1: a seed beyond the estimators' reach (> SCS) does not lock the pinned search; the escalation ladder around the seed does (a seed within reach locks at rung 0: clean synthetic). The capture carries cell `pci` rotated by `cfo_hz`; each rung compensates its hypothesis exactly as nr_scan_ssb() does. */
static void cfo_escalation(int pci, int cfo_hz)
{
  const int s0 = 60000;
  c16_t *cap = make_capture(pci, -1, 4.0, 0, s0, 0);
  NR_DL_FRAME_PARMS fp;
  make_fp(&fp);
  const int n = fp.samples_per_frame;
  const double fs = fp.samples_per_subframe * 1000.0;
  int h[NR_ACQ_LADDER_MAX];
  const int nh = nr_acq_cfo_ladder(0, 30000, (int)NR_ACQ_CFO_MAX_HZ, h, NR_ACQ_LADDER_MAX);
  int first = -1;
  c16_t *x = calloc(n + FFT, sizeof(c16_t));
  for (int k = 0; k < nh && first < 0; k++) {
    for (int i = 0; i < n; i++) {
      const double a = 2.0 * M_PI * (double)(cfo_hz - h[k]) * (double)i / fs; /* true CFO rotates the signal, the rung removes its hypothesis */
      x[i].r = (int16_t)lrint(cap[i].r * cos(a) - cap[i].i * sin(a));
      x[i].i = (int16_t)lrint(cap[i].r * sin(a) + cap[i].i * cos(a));
    }
    const lock_t l = search(x, pci);
    printf("  cfo %d rung %d (%d Hz): found=%d pci=%d fo_pss=%d fo_sss=%d\n", cfo_hz, k, h[k], l.found, l.pci, l.fo_pss, l.fo_sss);
    /* the scan accepts a rung by PBCH CRC, which needs the end-to-end CFO (hypothesis + PSS + SSS estimates) to be right to ~2 kHz */
    if (l.found && l.pci == pci && l.pos == s0 + CP && abs(cfo_hz - (h[k] + l.fo_pss + l.fo_sss)) <= 2000)
      first = k;
  }
  /* a seed within the estimators' reach (|offset| < SCS) locks at once (first == 0); beyond it only a ladder rung does */
  CHECK(first >= 0 && (abs(cfo_hz) < 30000 ? first == 0 : first > 0), "cfo %d: lock rung first=%d", cfo_hz, first);
  if (first > 0)
    printf("  cfo %d locked at rung %d (%d Hz)\n", cfo_hz, first, h[first]);
  free(x);
  free(cap);
}

/* RX_RECOVER re-sync CFO alias (2026-11-05 OTA, cell 923: -13802 / -14282 Hz for a true ~+200 Hz). The estimate is PSS (half-symbol phase) + SSS residual (phase over the PSS-SSS spacing,
 * wraps at +-scs/4.3); a PSS error beyond that wrap returns the truth minus one period (13953 Hz). The capture here is already at the radio's tuning (true residual `cfo_hz`, prior 0).
 * Checks, on the production estimators at several PSS strengths: whenever the raw estimate is a period off, nr_cfo_alias_k with the prior 0 returns it to the truth (+-400 Hz);
 * the alias occurs at least once (the test is sensitive); an estimate already near the prior gets k = 0; no prior leaves nothing to resolve (the caller only runs it with one). */
/* residual CFO of the SSB window in p->rxdata, from the cyclic prefix of its 4 symbols (the CP repeats the symbol tail: phase of sum conj(x[n]) x[n+FFT] = 2 pi f FFT / fs) */
static double window_cfo_hz(const nr_ssb_search_params_t *p)
{
  const int off = p->pss_res.pos - CP;
  double re = 0, im = 0;
  for (int sym = 1; sym < 4; sym++) { /* not the PSS symbol: the test's PSS-error emulation rotates it alone */
    const c16_t *x = p->rxdata[0] + off + sym * (CP + FFT);
    for (int n = 0; n < CP; n++) {
      re += (double)x[n].r * x[n + FFT].r + (double)x[n].i * x[n + FFT].i;
      im += (double)x[n].r * x[n + FFT].i - (double)x[n].i * x[n + FFT].r;
    }
  }
  return atan2(im, re) / (2.0 * M_PI) * 30000.0;
}
typedef struct {
  double tol_hz;
  int calls;
  double res_hz[2];
} fake_ctx_t;
/* fake PBCH: the CRC passes within tol_hz of residual CFO (the real one passes up to ~0.46 SCS), the DM-RS energy falls with the residual */
static bool fake_decode(void *vc, int hyp, double *m, double *mu0)
{
  fake_ctx_t *c = vc;
  c->calls++;
  c->res_hz[hyp] = window_cfo_hz(&g_last_p);
  *m = 1e6 / (1.0 + c->res_hz[hyp] * c->res_hz[hyp] / 1e6);
  *mu0 = 100;
  return fabs(c->res_hz[hyp]) <= c->tol_hz;
}

/* a capture of cell `pci` at +true_hz, the PSS stage forced pss_err_hz off (rotation of the PSS symbol only: a crude emulation, hence the residual of ~600 Hz) */
static c16_t *rotated_capture(int pci, int true_hz, int pss_err_hz)
{
  NR_DL_FRAME_PARMS fp;
  make_fp(&fp);
  const double fs = fp.samples_per_subframe * 1000.0;
  const int s0 = 60000;
  grng = 0x9e3779b97f4a7c15ull; /* the same noise every call */
  c16_t *cap = make_capture(pci, -1, 4.0, 0, s0, 0);
  for (int i = 0; i < fp.samples_per_frame; i++) {
    const double a = 2.0 * M_PI * (true_hz + (i >= s0 && i < s0 + CP + FFT ? pss_err_hz : 0)) * (double)i / fs;
    const c16_t c = cap[i];
    cap[i].r = (int16_t)lrint(c.r * cos(a) - c.i * sin(a));
    cap[i].i = (int16_t)lrint(c.r * sin(a) + c.i * cos(a));
  }
  return cap;
}

/* RX_RECOVER re-sync CFO alias (2026-11-05 OTA, cell 923: -13802 / -14282 Hz for a true ~+200 Hz). The estimate is PSS (half-symbol phase) + SSS residual (phase over the PSS-SSS spacing,
 * wraps at +-scs/4.3); a PSS error beyond that wrap returns the truth minus one period (13953 Hz). Prior and estimate are in the same units: the VERIFIED total lock CFO minus the radio tuning.
 * Pure helpers on the OTA values, the decision rule, then the production re-evaluation (nr_resync_cfo_reeval) through a fake PBCH decoder on synthetic captures. */
static void resync_alias(int pci)
{
  const int period = nr_sss_cfo_period_hz(30000);
  CHECK(period > 13900 && period < 14000, "period %d", period);
  /* E1B: verified total 248, radio at 248 -> prior in estimate units 0; the re-sync measured -13802 */
  CHECK(nr_cfo_alias_k(-13802, 248 - 248, period) == 1, "E1B: alias must be resolved");
  /* A1B epoch 2: verified 237, radio 237: -14282 -> alias */
  CHECK(nr_cfo_alias_k(-14282, 237 - 237, period) == 1, "A1B epoch 2: alias");
  /* A1B epoch 3: radio left at -13977 by the wrong lock, verified total 237 -> prior 237 + 13977; measured +14161 is the CORRECT residual: no alias, and the total 14161 - 13977 = 184 */
  CHECK(nr_cfo_alias_k(14161, 237 + 13977, period) == 0, "A1B epoch 3: a real tuning step must not be flipped");
  CHECK(nr_cfo_alias_k(14161, 0, period) == -1, "(documents the rejected prior-0 behaviour)");
  CHECK(nr_cfo_alias_k(151, 0, period) == 0 && nr_cfo_alias_k(-6900, 0, period) == 0, "near the prior: k must be 0");
  CHECK(nr_cfo_alias_k(-13802, 248, period) == 1 && nr_cfo_alias_k(-13802 + period, 248, period) == 0, "prior 248 picks +151");
  CHECK(nr_cfo_alias_k(5000, 0, 0) == 0, "degenerate period");

  /* decision rule */
  int ne;
  nr_resync_hyp_t a = {1, 1e6, 100}, b = {0, 1e6, 100}, c = {1, 1e4, 100}, d = {1, 1.001e6, 100};
  CHECK(nr_resync_cfo_decide(&a, &b, &ne) == 0 && !ne, "only h0 passes CRC -> keep");
  CHECK(nr_resync_cfo_decide(&b, &a, &ne) == 1 && !ne, "only h1 passes CRC -> alias");
  CHECK(nr_resync_cfo_decide(&b, &b, &ne) == 0 && ne == 1, "neither -> keep, counted");
  CHECK(nr_resync_cfo_decide(&a, &c, &ne) == 0 && !ne, "both pass, h0 energy decisively higher -> keep");
  CHECK(nr_resync_cfo_decide(&c, &a, &ne) == 1 && !ne, "both pass, h1 decisively higher -> alias");
  CHECK(nr_resync_cfo_decide(&a, &d, &ne) == 1, "both pass, within 3 sigma -> the hypothesis nearer the prior (the alias)");

  /* the production re-evaluation: truth +200 Hz, PSS stage -11000 Hz off, radio correctly tuned (prior 0) */
  NR_DL_FRAME_PARMS fp;
  make_fp(&fp);
  int n_flipped = 0;
  for (int mode = 0; mode < 4; mode++) {
    c16_t *cap = rotated_capture(pci, 200, -11000);
    const lock_t l = search(cap, pci);
    CHECK(l.found, "mode %d: no lock", mode);
    if (!l.found) {
      free(cap);
      continue;
    }
    const int est = l.fo_pss + l.fo_sss;
    CHECK(abs(est - (200 - period)) <= 900, "mode %d: est %d is not the E1B-like alias", mode, est);
    /* snapshot of the state a rejected alias must leave untouched */
    const int off = g_last_p.pss_res.pos - CP, win = 4 * (FFT + CP);
    c16_t *before = malloc(win * sizeof(c16_t));
    memcpy(before, cap + off, win * sizeof(c16_t));
    static c16_t F0[NR_N_SYMBOLS_SSB][1][FFT];
    memcpy(F0, g_last_p.rxdataF, sizeof(F0));
    const double rsrp0 = nr_ue_calculate_ssb_rsrp(&fp, F0[2], SSB_SC);
    uint64_t t0, k0, u0;
    nr_resync_cfo_stats(&t0, &k0, &u0);
    fake_ctx_t fc = {.tol_hz = mode == 0 ? 15000 : mode == 1 ? 3000 : mode == 2 ? 100 : 15000};
    const int prior_e = mode == 3 ? est : 0; /* mode 3: a prior consistent with the measurement */
    const int k = nr_resync_cfo_reeval(&g_last_p, est, prior_e, l.fo_sss, fake_decode, &fc);
    uint64_t t1, k1, u1;
    nr_resync_cfo_stats(&t1, &k1, &u1);
    const uint64_t unresolved = u1 - u0;
    printf("  reeval mode %d: est %d prior %d -> k %d, residual h0 %.0f h1 %.0f, unresolved %lu taken %lu\n", mode, est, prior_e, k, fc.res_hz[0], fc.res_hz[1], (unsigned long)unresolved, (unsigned long)(t1 - t0));
    if (mode == 0 || mode == 1) { /* both CRCs pass (energy decides) / only the alias passes */
      CHECK(k == 1 && fc.calls == 2, "mode %d: alias must be taken (k %d calls %d)", mode, k, fc.calls);
      CHECK(fabs(window_cfo_hz(&g_last_p)) <= 1500 && fabs(window_cfo_hz(&g_last_p)) < fabs(fc.res_hz[0]) / 8,  "mode %d: recompensation sign/size: residual %.0f Hz", mode, window_cfo_hz(&g_last_p));
      { /* exact sign and size: the window is the entry window rotated by -(sss + k*period) from phase 0 (the CP estimator above wraps at 2 periods and cannot see a sign flip) */
        const double w = -2.0 * M_PI * (double)(l.fo_sss + k * nr_sss_cfo_period_hz(30000)) / (fp.samples_per_subframe * 1000.0);
        int worst = 0;
        for (int n = 0; n < win; n++) {
          const int er = (int)lrint(before[n].r * cos(w * n) - before[n].i * sin(w * n)), ei = (int)lrint(before[n].r * sin(w * n) + before[n].i * cos(w * n));
          worst = max(worst, max(abs(er - cap[off + n].r), abs(ei - cap[off + n].i)));
        }
        CHECK(worst <= 3, "mode %d: window is not the entry window rotated by the alias compensation (worst diff %d)", mode, worst);
      }
      n_flipped++;
    } else if (mode == 2) { /* neither passes: measurement kept, state restored bit-exactly, counted */
      CHECK(k == 0 && unresolved == 1, "neither: k %d unresolved %lu", k, (unsigned long)unresolved);
      CHECK(memcmp(before, cap + off, win * sizeof(c16_t)) == 0, "rejected alias must restore the SSB window");
      CHECK(memcmp(F0, g_last_p.rxdataF, sizeof(F0)) == 0, "rejected alias must restore rxdataF");
      CHECK(nr_ue_calculate_ssb_rsrp(&fp, ((c16_t(*)[1][FFT])g_last_p.rxdataF)[2], SSB_SC) == rsrp0, "rsrp (rx gain) must not change after a rejected alias");
    } else { /* prior equal to the measurement: no re-evaluation at all */
      CHECK(k == 0 && fc.calls == 0 && memcmp(before, cap + off, win * sizeof(c16_t)) == 0, "estimate at the prior: nothing may run (calls %d)", fc.calls);
    }
    free(before);
    free(cap);
  }
  CHECK(n_flipped == 2, "flipped %d", n_flipped);

  /* A1B epoch 3: the radio is mistuned by -13977, the capture holds the cell at +14161; estimate +14161, verified prior 237 -> 14214 in estimate units: no alias, nothing runs */
  c16_t *cap = rotated_capture(pci, 14161, 0);
  const lock_t l = search(cap, pci);
  CHECK(l.found, "A1B epoch 3 lock");
  if (l.found) {
    const int est = l.fo_pss + l.fo_sss;
    fake_ctx_t fc = {.tol_hz = 15000};
    CHECK(abs(est - 14161) <= 900, "A1B epoch 3: est %d", est);
    CHECK(nr_resync_cfo_reeval(&g_last_p, est, 237 + 13977, l.fo_sss, fake_decode, &fc) == 0 && fc.calls == 0, "A1B epoch 3: must not be flipped (calls %d)", fc.calls);
  }
  free(cap);
}

/* The verification gate and its units (nr_cfo_verify_*, called by UE_synch), and the lock-evidence rule. */
static void verify_gate(void)
{
  const int P = nr_sss_cfo_period_hz(30000);
  /* only first-pass TBs whose slot is at/after the lock count; rescues and in-flight jobs from before the lock do not */
  CHECK(nr_cfo_crc_counts(1001, 1000, 0), "job after the lock counts");
  CHECK(!nr_cfo_crc_counts(1000, 1000, 0) && !nr_cfo_crc_counts(999, 1000, 0), "the last pre-loss slot and earlier in-flight jobs must not count (strict)");
  CHECK(nr_cfo_crc_counts(0, -1, 0) && nr_cfo_crc_counts(5000, -1, 0), "first lock of a cell (producer slot still -1): every later TB verifies it");
  CHECK(!nr_cfo_crc_counts(5000, 1000, 1), "a rescue (decodable at some FO) must not count");
  nr_cfo_verify_t v = {0};
  int dropped;
  /* first lock at total 248 (radio then at 248), nothing verified yet: no prior */
  nr_cfo_verify_record_lock(&v, 248, 923);
  dropped = nr_cfo_verify_select_prior(&v, 0, 0, 248);
  CHECK(!v.prior_valid && !dropped, "unverified first lock is not a prior");
  /* A,A,B,A: a lock of another PCI breaks the streak (3 unverified locks of the PRIOR's PCI in a row drop it) */
  {
    nr_cfo_verify_t w = {0};
    nr_cfo_verify_record_lock(&w, 100, 923);
    nr_cfo_verify_select_prior(&w, 4, 0, 100);
    const int pcis[4] = {923, 923, 415, 923};
    for (int i = 0; i < 4; i++) {
      nr_cfo_verify_record_lock(&w, 100, pcis[i]);
      nr_cfo_verify_select_prior(&w, 0, 0, 100);
    }
    CHECK(w.prior_valid, "a PCI change must reset the unverified streak (streak %d)", w.unverified_streak);
  }
  /* verified lock: promoted; prior in estimate units = total - radio tuning */
  nr_cfo_verify_record_lock(&v, 248, 923);
  nr_cfo_verify_select_prior(&v, 7, 0, 248);
  CHECK(v.prior_valid && v.prior_total == 248 && v.prior_pci == 923 && v.prior_est_hz == 0, "E1B: prior units (non-cont) %d", v.prior_est_hz);
  /* a wrong re-sync lock (total -13554, radio moved there) is unverified: the old prior stays; with the radio mistuned the prior in estimate units is 248 + 13554 */
  nr_cfo_verify_record_lock(&v, -13554, 923);
  nr_cfo_verify_select_prior(&v, 0, 0, -13554);
  CHECK(v.prior_valid && v.prior_total == 248 && v.prior_est_hz == 248 + 13554, "unverified lock replaced the prior (total %d est %d)", v.prior_total, v.prior_est_hz);
  CHECK(nr_cfo_alias_k(14161, v.prior_est_hz, P) == 0, "A1B epoch 3 through the gate: no flip");
  /* cont_fo_comp: the estimate is the total, the radio never moves: prior in estimate units = total */
  nr_cfo_verify_t c = {0};
  nr_cfo_verify_record_lock(&c, 4100, 415);
  nr_cfo_verify_select_prior(&c, 3, 1, 999 /* ignored */);
  CHECK(c.prior_est_hz == 4100, "cont_fo_comp units %d", c.prior_est_hz);
  /* the seed (initial_fo) is inside the estimate and cancels: the same prior for any seed */
  CHECK(nr_cfo_alias_k(-13802 + 5000, 5000, P) == nr_cfo_alias_k(-13802, 0, P), "seed cancels");
  /* clear (live tune) */
  nr_cfo_verify_clear(&c);
  nr_cfo_verify_select_prior(&c, 9, 1, 0);
  CHECK(!c.prior_valid, "cleared state must not promote a stale lock");
  /* prior exit: the true CFO jumps by more than half a period (reference re-lock): at most NR_CFO_UNVERIFIED_MAX wrong flips, then behaviour = no prior */
  nr_cfo_verify_t g = {0};
  int radio = 200, flips = 0, total = 200;
  nr_cfo_verify_record_lock(&g, 200, 923);
  nr_cfo_verify_select_prior(&g, 5, 0, radio);
  const int true_after = 200 + 9000;
  for (int it = 0; it < 8; it++) {
    nr_cfo_verify_select_prior(&g, 0, 0, radio); /* the previous lock decoded nothing */
    const int est = true_after - radio;
    const int k = g.prior_valid ? nr_cfo_alias_k(est, g.prior_est_hz, P) : 0;
    flips += k != 0;
    total = est + k * P + radio;
    radio = total; /* the radio follows the lock */
    nr_cfo_verify_record_lock(&g, total, 923);
  }
  CHECK(flips == NR_CFO_UNVERIFIED_MAX && !g.prior_valid && abs(total - true_after) <= 1, "GPSDO jump: flips %d prior_valid %d total %d (true %d)", flips, g.prior_valid, total, true_after);
}

/* nr_pbch_dmrs_metrics on a synthetic SSB: the PBCH DM-RS of cell 415, ssb index 3, amplitude A on its true RE positions, CFO f across the three PBCH symbols (symbol spacing N+CP samples) */
static void dmrs_metric(void)
{
  NR_DL_FRAME_PARMS fp;
  make_fp(&fp);
  fp.Lmax = 8;
  const int nid = 415, idx = 3;
  static c16_t F[NR_N_SYMBOLS_SSB][1][FFT];
  double e[2], mu[2];
  const double fs = fp.samples_per_subframe * 1000.0;
  for (int c = 0; c < 2; c++) {
    const double f = c == 0 ? 0.0 : 13953.0; /* the alias error of the OTA bug: 0.465 SCS */
    memset(F, 0, sizeof(F));
    grng = 0x1234567ull;
    for (int sym = 1; sym < 4; sym++) {
      const int dmrss = sym - 1;
      c16_t pilot[200];
      nr_pbch_dmrs_rx(dmrss, nr_gold_pbch(fp.Lmax, nid, 0, idx), pilot, false);
      unsigned off = fp.first_carrier_offset + SSB_SC;
      if (off >= FFT)
        off -= FFT;
      const int k = nid % 4;
      const double ph = 2.0 * M_PI * f * (double)(sym * (CP + FFT)) / fs;
      int re = off, n = 0;
      const int n_pil = dmrss == 1 ? 24 : 60;
      for (int j = 0; j < n_pil; j++) {
        if (dmrss == 1 && j == 12)
          re = (re + 144) % FFT;
        const double A = 1000.0; /* the correlation uses the conjugate pilot: transmit conj(conj(p)) */
        const double pr = pilot[n].r, pi = -pilot[n].i;
        F[sym][0][(re + k) % FFT].r = (int16_t)lrint((pr * cos(ph) - pi * sin(ph)) * A / 32768.0 * 1.0 + 30 * gauss());
        F[sym][0][(re + k) % FFT].i = (int16_t)lrint((pr * sin(ph) + pi * cos(ph)) * A / 32768.0 * 1.0 + 30 * gauss());
        re = (re + 4) % FFT;
        n++;
      }
    }
    int bi = -1;
    nr_pbch_dmrs_metrics(&fp, nid, 1, SSB_SC, F, &e[c], &mu[c], &bi);
    printf("  dmrs metric f=%.0f: best %.3g mu0 %.3g idx %d\n", f, e[c], mu[c], bi);
    if (c == 0)
      CHECK(bi == idx, "best index %d, expected %d", bi, idx);
  }
  CHECK(e[0] > 100 * mu[0], "correct CFO: best %.3g must dominate the wrong indices %.3g", e[0], mu[0]);
  /* measured: the coherent sum over the 3 PBCH symbols (60 + 24 + 60 DM-RS REs) flips the sign of the middle one at ~0.465 SCS: (60-24+60)^2 / 144^2 = -3.5 dB, not the ~30 dB of an exact cancellation */
  CHECK(e[1] < 0.6 * e[0] && e[1] > 0.3 * e[0], "0.465 SCS error must cost about 3.5 dB of energy: %.3g vs %.3g", e[1], e[0]);
  {
    int ne;
    const nr_resync_hyp_t aliased = {1, e[1], mu[1]}, tuned = {1, e[0], mu[0]};
    CHECK(nr_resync_cfo_decide(&aliased, &tuned, &ne) == 1 && nr_resync_cfo_decide(&tuned, &aliased, &ne) == 0,
          "decision on the measured energies: h0 %.3g vs h1 %.3g (3 sigma lead needed %.3g)", e[1], e[0], 3 * sqrt(2 * e[0] * mu[0] + 2 * e[1] * mu[1] + mu[0] * mu[0] + mu[1] * mu[1]));
  }
  CHECK(mu[1] < 3 * mu[0] && mu[0] < 3 * mu[1], "mu0 (wrong indices) must not depend on the CFO: %.3g vs %.3g", mu[0], mu[1]);
}

/* A3: two cells both accepted by the blind pass (weaker one 4 dB down): the pass keeps both, strongest SSS metric first, both PCIs reported, and the second can be selected */
static void inventory(int strong, int weak)
{
  const int s_strong = 40000, s_weak = 190000;
  c16_t *cap = make_capture(strong, weak, 4.0, 2.5, s_strong, s_weak);
  const lock_t b = search(cap, -1);
  printf("inventory strong=%d weak=%d: found=%d pci=%d n_alt=%d\n", strong, weak, b.found, b.pci, b.n_alt);
  CHECK(b.found && b.pci == strong, "blind must choose the stronger %d, got %d", strong, b.pci);
  CHECK(b.n_alt == 2, "both cells must be reported, n_alt=%d", b.n_alt);
  if (b.n_alt == 2) {
    CHECK(b.alt_pci[0] == strong && b.alt_pci[1] == weak, "inventory %d %d", b.alt_pci[0], b.alt_pci[1]);
    CHECK(b.alt_metric[0] > b.alt_metric[1], "SSS metric order %d %d", (int)b.alt_metric[0], (int)b.alt_metric[1]);
    CHECK(b.sel_ok && b.sel_pci == weak && b.sel_pos == s_weak + CP, "select(1): ok=%d pci=%d pos=%d", b.sel_ok, b.sel_pci, b.sel_pos);
  }
  const lock_t pw = search(cap, weak); /* pinned behaviour unchanged: the wanted cell, one result */
  CHECK(pw.found && pw.pci == weak && pw.n_alt == 1, "pinned: found=%d pci=%d n_alt=%d", pw.found, pw.pci, pw.n_alt);
  free(cap);
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
  g_decim = 1;
  inventory(380, 415);
  inventory(27, 403);
  inventory(415, 380); /* the weaker cell has the higher N_ID_2 order */
  g_fo = true;
  cfo_escalation(415, 40000);
  cfo_escalation(403, -47000);
  cfo_escalation(415, 9000);
  resync_alias(415);
  verify_gate();
  dmrs_metric();
  registry();
  if (failures) {
    printf("test_nr_sync_pin: %d FAILURES\n", failures);
    return 1;
  }
  printf("test_nr_sync_pin: all checks passed\n");
  return 0;
}
