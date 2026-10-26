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

double cpuf; // normally defined by the executable
__thread uint32_t nr_dl_chest_nvar_ant[4]; // normally published by the passive channel estimator

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
enum { EST = 14 * FFT };

static NR_DL_FRAME_PARMS make_fp(int pci)
{
  NR_DL_FRAME_PARMS fp = {0};
  fp.numerology_index = 1;
  fp.ssb_type = nr_ssb_type_C;
  fp.Lmax = 8; // case C candidates: symbols 2 and 8 of slots 0..3 of each half frame
  fp.slots_per_frame = 20;
  fp.symbols_per_slot = 14;
  fp.N_RB_DL = NRB;
  fp.ofdm_symbol_size = FFT;
  fp.first_carrier_offset = FFT - NRB * 6;
  fp.ssb_start_subcarrier = SSB_SC;
  fp.Nid_cell = pci;
  fp.nb_antennas_rx = 1;
  fp.samples_per_slot_wCP = 14 * FFT;
  return fp;
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

typedef struct {
  int bwp_start, bwp_size;
  int start, nsym, dmrs_sym; // DM-RS type 1, one CDM group without data: even REs of dmrs_sym
  int n_prb;
  uint16_t prb[NR_PRB_SET_MAX]; // BWP-relative, DATA order
  bool segmented;
  bool ssb_tx; // SSB transmitted at symbols 2..5 of this slot
  int rx_pci;
  uint16_t rnti;
} scene_t;

typedef struct {
  bool planned;
  uint16_t event_symbols;
  uint32_t unav, G, valid_sum, n_tx;
  int first;
  uint32_t dl_valid_re[NR_SYMBOLS_PER_SLOT];
  int sign_errors;
} result_t;

static bool ssb_re(const scene_t *sc, int m, int crb)
{
  return sc->ssb_tx && m >= 2 && m <= 5 && crb >= SSB_CRB0 && crb <= SSB_CRB1;
}

static result_t run(const scene_t *sc)
{
  result_t r = {0};
  PHY_VARS_NR_UE *ue = calloc(1, sizeof(*ue));
  ue->frame_parms = make_fp(sc->rx_pci);
  NR_DL_FRAME_PARMS *fp = &ue->frame_parms;
  const UE_nr_rxtx_proc_t proc = {.frame_rx = 124, .nr_slot_rx = 0};

  fapi_nr_dl_config_dlsch_pdu_rel15_t cfg = {0};
  cfg.BWPStart = sc->bwp_start;
  cfg.BWPSize = sc->bwp_size;
  cfg.start_symbol = sc->start;
  cfg.number_symbols = sc->nsym;
  cfg.dlDmrsSymbPos = 1 << sc->dmrs_sym;
  cfg.dmrsConfigType = NFAPI_NR_DMRS_TYPE1;
  cfg.n_dmrs_cdm_groups = 1;

  freq_alloc_bitmap_t fa = {0};
  fa.n_prb_list = sc->n_prb;
  memcpy(fa.prb_list, sc->prb, sc->n_prb * sizeof(uint16_t));
  if (!nr_pdsch_passive_alloc_normalise(&fa, sc->bwp_size)) {
    CHECK(0, "bad allocation");
    return r;
  }
  if (!sc->segmented)
    fa.n_prb_list = 0; // contiguous: the legacy first_rb..last_rb form the decoder sees
  nr_prb_seg_t seg[NR_PRB_SET_MAX];
  const int nseg = nr_prb_segments(sc->prb, sc->n_prb, sc->bwp_start, 0, seg, NR_PRB_SET_MAX);

  // ---- transmitter: SSB (PSS/SSS + PBCH-like fill), DM-RS, and a known QPSK stream in data order
  c16_t(*rx)[fp->samples_per_slot_wCP] = aligned_alloc(64, sizeof(c16_t) * fp->samples_per_slot_wCP);
  memset(rx, 0, sizeof(c16_t) * fp->samples_per_slot_wCP);
  int pss[127], sss[127];
  sync_seq(TX_PCI, pss, sss);
  c16_t *tx = malloc(sizeof(c16_t) * NR_PRB_SET_MAX * 12 * 14);
  uint32_t ntx = 0;
  for (int m = sc->start; m < sc->start + sc->nsym; m++)
    for (int i = 0; i < sc->n_prb; i++) {
      const int crb = sc->bwp_start + sc->prb[i];
      for (int k = 0; k < 12; k++) {
        const int bin = (fp->first_carrier_offset + crb * 12 + k) % FFT;
        if (ssb_re(sc, m, crb))
          continue; // written below
        if (m == sc->dmrs_sym && !(k & 1)) {
          rx[0][m * FFT + bin] = (c16_t){A, A};
          continue;
        }
        const c16_t x = {bit() ? -A : A, bit() ? -A : A};
        rx[0][m * FFT + bin] = x;
        tx[ntx++] = x;
      }
    }
  if (sc->ssb_tx) {
    for (int m = 2; m <= 5; m++)
      for (int k = 0; k < 240; k++)
        rx[0][m * FFT + (fp->first_carrier_offset + SSB_SC + k) % FFT] = (c16_t){bit() ? -A : A, bit() ? -A : A};
    for (int n = 0; n < 127; n++) {
      const int bin = (fp->first_carrier_offset + SSB_SC + 56 + n) % FFT;
      rx[0][2 * FFT + bin] = (c16_t){pss[n] * A, 0};
      rx[0][4 * FFT + bin] = (c16_t){sss[n] * A, 0};
    }
  }
  r.n_tx = ntx;

  // ---- the production SSB path, in the order nr_pdsch_passive_decode() runs it
  const uint16_t cand = nr_ssb_rm_candidates(fp, proc.nr_slot_rx, cfg.start_symbol, cfg.number_symbols);
  const nr_ssb_rm_event_t ev = nr_ssb_rm_observe(fp, proc.frame_rx, proc.nr_slot_rx, cand, rx);
  r.event_symbols = ev.symbols;
  nr_ssb_rm_plan_t plan;
  r.planned = nr_ssb_rm_plan(&ev, proc.frame_rx, proc.nr_slot_rx, fp->Nid_cell, sc->rnti, &cfg, &fa,
                             sc->segmented ? seg : NULL, sc->segmented ? nseg : 0, &plan);
  if (!r.planned)
    goto out;
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
    memcpy(virt, rx, sizeof(c16_t) * fp->samples_per_slot_wCP);
    const int off0 = fp->first_carrier_offset + cfg.BWPStart * 12;
    for (int m = cfg.start_symbol; m < cfg.start_symbol + cfg.number_symbols; m++)
      for (int i = 0; i < nre; i++)
        virt[m * FFT + (off0 + i) % FFT] = rx[0][m * FFT + (off0 + gidx[i]) % FFT];
    memcpy(rx, virt, sizeof(c16_t) * fp->samples_per_slot_wCP);
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
  c16_t ptrs_phase[1][NR_SYMBOLS_PER_SLOT] = {0};
  int32_t ptrs_re[1][NR_SYMBOLS_PER_SLOT] = {0};
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
                      &log2_maxh, buf, 1, comp, mag, magb, magr, ptrs_phase, ptrs_re, 0, &scope, NULL,
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
out:
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

int main(void)
{
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

  printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
  return failures ? 1 : 0;
}
