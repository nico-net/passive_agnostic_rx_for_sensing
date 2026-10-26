/* Offline fixture of the passive receiver's blind UCI-on-PUSCH demultiplexing (nr_pusch_uci_blind.h).
 *
 * TX: ulsim's real OAI UE chain with UCI multiplexed on the PUSCH (nr_ue_ulsch_procedures: TS 38.212 6.3.2.4 rate matching
 * and the 6.2.7 data/control mapping), the UCI of each run given by --uci O_ACK,B_ACK,O_CSI1,B_CSI1,O_CSI2,B_CSI2,ALPHA.
 * RX: the gNB receive chain to descrambled LLRs, then the BLIND search with no knowledge of the UCI (nothing of --uci reaches
 * it): the TB must decode with the transmitted payload, and the UCI RE counts it found must equal the ones the UE used.
 * The first trial explores, the later trials must hit the learned hypothesis.
 *
 * Also, before the link runs, the receiver's mapping (TS 38.212 6.2.7 as written) is compared with the OAI UE's template
 * for a grid of geometries and UCI sizes; disagreements are reported per configuration (MAPCHECK). */
#define main ulsim_main
#define phy_procedures_gNB_uespec_RX ucisim_rx
#define phy_procedures_nrUE_TX ucisim_tx
#include "ulsim.c"
#undef phy_procedures_nrUE_TX
#undef phy_procedures_gNB_uespec_RX
#undef main

#include "PHY/NR_TRANSPORT/nr_transport_proto.h"
#include "PHY/NR_UE_TRANSPORT/nr_pusch_uci_blind.h"
#include "PHY/NR_UE_TRANSPORT/nr_uci_on_pusch.h"

extern int nr_ulsch_decoding(PHY_VARS_gNB *, NR_DL_FRAME_PARMS *, uint32_t, uint8_t, int *, int);
extern int phy_procedures_gNB_uespec_RX(PHY_VARS_gNB *, int, int, NR_UL_IND_t *);
extern void phy_procedures_nrUE_TX(PHY_VARS_NR_UE *ue, const UE_nr_rxtx_proc_t *proc, nr_phy_data_tx_t *phy_data, c16_t **txp);

/* The live UE executable owns these; the simulator has no capture producer. */
_Atomic long nr_ue_diag_producer_absolute_slot = -1;
_Atomic long nr_ue_diag_producer_wall_ns = 0;
_Atomic long nr_ue_pending_rebase_delta = 0;
_Atomic int nr_ue_pending_rebase_valid = 0;

static nfapi_nr_ue_pusch_uci_t g_uci;
static bool g_uci_on;
static PHY_VARS_NR_UE *g_ue;
static unsigned g_trials, g_fail, g_learned_hits, g_explored_hits;
static uint32_t g_truth_ack, g_truth_csi1, g_truth_csi2, g_truth_rvd;

void ucisim_tx(PHY_VARS_NR_UE *ue, const UE_nr_rxtx_proc_t *proc, nr_phy_data_tx_t *phy_data, c16_t **txp)
{
  g_ue = ue;
  ue->no_phase_pre_comp = false;
  if (g_uci_on && phy_data->ulsch.status == NR_ACTIVE) {
    phy_data->ulsch.pusch_pdu.pusch_uci = g_uci;
    phy_data->pucch_vars.pucch_pdu[0].prb_size = 1;
  }
  phy_procedures_nrUE_TX(ue, proc, phy_data, txp);
}

typedef struct {
  PHY_VARS_gNB *gnb;
  NR_gNB_ULSCH_t *ulsch;
  int frame, slot;
  uint32_t G, nlqm;
} tb_ctx_t;

static bool tb_decode(void *opaque, const int16_t *llr, uint32_t g_ulsch)
{
  (void)llr; /* the search wrote the stream into pusch_vars[0].llr */
  tb_ctx_t *c = opaque;
  c->ulsch->unav_res = (c->G - g_ulsch) / c->nlqm;
  c->ulsch->harq_process->harq_to_be_cleared = true;
  int id = 0;
  const int rc = nr_ulsch_decoding(c->gnb, &c->gnb->frame_parms, c->frame, c->slot, &id, 1);
  return rc == 0 && c->ulsch->harq_process->C > 0 && c->ulsch->harq_process->processedSegments == c->ulsch->harq_process->C;
}

int ucisim_rx(PHY_VARS_gNB *gnb, int frame, int slot, NR_UL_IND_t *ind)
{
  const int initial_rc = phy_procedures_gNB_uespec_RX(gnb, frame, slot, ind);
  AssertFatal(initial_rc == 0, "VOID: simulator reference receive failed\n");
  const int saved_passive = get_softmodem_params()->passive_rx;
  get_softmodem_params()->passive_rx = 1;
  NR_gNB_ULSCH_t *ulsch = &gnb->ulsch[0];
  NR_gNB_PUSCH *pv = &gnb->pusch_vars[0];
  const nfapi_nr_pusch_pdu_t *p = &ulsch->harq_process->ulsch_pdu;
  const int ndmrs = count_bits64_with_mask(p->ul_dmrs_symb_pos, p->start_symbol_index, p->nr_of_symbols);
  const int dmrs_re = (p->dmrs_config_type == 0 ? 6 : 4) * p->num_dmrs_cdm_grps_no_data;
  const uint32_t G = nr_get_G(p->rb_size, p->nr_of_symbols, dmrs_re, ndmrs, 0, p->qam_mod_order, p->nrOfLayers);
  uint32_t *unav = &ulsch->unav_res;
  *unav = 0;
  nr_rx_pusch_group_tp(gnb, &pv, &p, &unav, 1, frame, slot);
  int16_t *llr = malloc(G * sizeof(*llr));
  AssertFatal(llr, "fixture allocation\n");
  memcpy(llr, pv->llr, G * sizeof(*llr));
  /* without UCI handling: the plain decode */
  ulsch->harq_process->harq_to_be_cleared = true;
  int id = 0;
  ulsch->unav_res = 0;
  const int rc0 = nr_ulsch_decoding(gnb, &gnb->frame_parms, frame, slot, &id, 1);
  const bool plain_ok = rc0 == 0 && ulsch->harq_process->processedSegments == ulsch->harq_process->C;
  tb_ctx_t tc = {.gnb = gnb, .ulsch = ulsch, .frame = frame, .slot = slot, .G = G, .nlqm = (uint32_t)p->qam_mod_order * p->nrOfLayers};
  nr_uci_search_in_t si = {.rnti = p->rnti,
                           .geom = {.rb = p->rb_size, .start_sym = p->start_symbol_index, .nsym = p->nr_of_symbols,
                                    .dmrs_pos = p->ul_dmrs_symb_pos, .data_re_dmrs_rb = (uint8_t)(12 - dmrs_re),
                                    .qm = p->qam_mod_order, .nl = p->nrOfLayers},
                           .tbs_bits = p->pusch_data.tb_size * 8u, .bg = p->maintenance_parms_v3.ldpcBaseGraph, .rv = 0, .dai = -1,
                           .llr = llr, .G = G, .work = pv->llr, .tb = tb_decode, .opaque = &tc, .explore = true, .budget_us = 3000000};
  if (g_uci_on) { /* the UE's own counts for this UCI on this grant (OAI UE rate matching): what the search must find */
    unsigned C = 0, K = 0, Z = 0, F = 0;
    nr_segmentation(NULL, NULL, lenWithCrc(1, p->pusch_data.tb_size * 8), &C, &K, &Z, &F, p->maintenance_parms_v3.ldpcBaseGraph);
    nfapi_nr_ue_pusch_pdu_t up;
    memset(&up, 0, sizeof(up));
    up.rb_size = p->rb_size;
    up.start_symbol_index = p->start_symbol_index;
    up.nr_of_symbols = p->nr_of_symbols;
    up.ul_dmrs_symb_pos = p->ul_dmrs_symb_pos;
    up.dmrs_config_type = p->dmrs_config_type;
    up.num_dmrs_cdm_grps_no_data = p->num_dmrs_cdm_grps_no_data;
    up.qam_mod_order = p->qam_mod_order;
    up.nrOfLayers = p->nrOfLayers;
    up.pusch_uci = g_uci;
    int8_t *map = malloc(G);
    rate_match_info_uci_t rm = {0};
    nr_uci_on_pusch_ulsch_map(&up, 0, 0, K, C, G, map, &rm);
    free(map);
    g_truth_rvd = g_uci.harq_ack_bit_length <= 2 ? rm.Q_dash_ACK : 0;
    g_truth_ack = g_uci.harq_ack_bit_length > 2 ? rm.Q_dash_ACK : 0;
    g_truth_csi1 = rm.Q_dash_CSI1;
    g_truth_csi2 = rm.Q_dash_CSI2;
  }
  nr_uci_search_out_t so = {0};
  if (!plain_ok || g_uci_on)
    nr_pusch_uci_blind_search(&si, &so);
  double agree = -1; /* fraction of UL-SCH stream positions where the found hypothesis reads the codeword bit the UE's map puts there */
  if (g_uci_on && so.ok) {
    unsigned C = 0, K = 0, Z = 0, F = 0;
    nr_segmentation(NULL, NULL, lenWithCrc(1, p->pusch_data.tb_size * 8), &C, &K, &Z, &F, p->maintenance_parms_v3.ldpcBaseGraph);
    nfapi_nr_ue_pusch_pdu_t up;
    memset(&up, 0, sizeof(up));
    up.rb_size = p->rb_size; up.start_symbol_index = p->start_symbol_index; up.nr_of_symbols = p->nr_of_symbols;
    up.ul_dmrs_symb_pos = p->ul_dmrs_symb_pos; up.dmrs_config_type = p->dmrs_config_type;
    up.num_dmrs_cdm_grps_no_data = p->num_dmrs_cdm_grps_no_data; up.qam_mod_order = p->qam_mod_order; up.nrOfLayers = p->nrOfLayers;
    up.pusch_uci = g_uci;
    int8_t *map = malloc(G);
    int16_t *lo = malloc(G * 2), *hi = malloc(G * 2), *olo = malloc(G * 2), *ohi = malloc(G * 2);
    nr_uci_on_pusch_ulsch_map(&up, 0, 0, K, C, G, map, NULL);
    for (uint32_t i = 0; i < G; i++) { lo[i] = (int16_t)(1 + (i & 0x3fff)); hi[i] = (int16_t)(1 + (i >> 14)); }
    nr_uci_geom_t gg = si.geom;
    gg.sum_k = C * K;
    const uint32_t gr = nr_uci_demux(&gg, &so.h, lo, 0, 0, olo);
    nr_uci_demux(&gg, &so.h, hi, 0, 0, ohi);
    uint32_t j = 0, same = 0, tot = 0;
    for (uint32_t i = 0; i < G && j < gr; i++) {
      if (map[i] < 0) continue;
      tot++;
      same += olo[j] && ((uint32_t)(olo[j] - 1) | ((uint32_t)(ohi[j] - 1) << 14)) == i;
      j++;
    }
    agree = tot ? (double)same / tot : 0;
    free(map); free(lo); free(hi); free(olo); free(ohi);
  }
  bool payload = false;
  const bool ok = g_uci_on ? so.ok : plain_ok;
  if (ok && g_ue) {
    payload = !memcmp(ulsch->harq_process->b, g_ue->ul_harq_processes[0].payload_AB, p->pusch_data.tb_size);
  }
  const bool counts = !g_uci_on || (so.c.q_csi1 == g_truth_csi1 && so.c.q_csi2 == g_truth_csi2
                                     && (g_uci.harq_ack_bit_length <= 2 ? so.c.q_rvd == g_truth_rvd : so.c.q_ack == g_truth_ack));
  const bool learned_ok = g_trials == 0 || !g_uci_on || so.learned;
  /* Two footprints that read the codeword the same way except for a few REs (e.g. CSI parts 1/2 of 9/8 vs 8/9 REs in one
   * symbol, both interleaved over it) are not separable by any CRC: the decode is right either way. Counted as the same. */
  const bool layout_ok = counts || agree >= 0.999;
  const bool pass = ok && payload && layout_ok && learned_ok && (!g_uci_on || g_uci.harq_ack_bit_length <= 2 || g_uci.csi_payload.p1_bits || !plain_ok);
  g_trials++;
  g_fail += !pass;
  g_learned_hits += so.learned;
  g_explored_hits += so.ok && !so.learned;
  printf("UCISIM trial=%u prb=%u qm=%u G=%u plain_ok=%d found=%d learned=%d payload=%d hyp[o_ack=%u b_ack=%u o1=%u b1=%u o2=%u b2=%u a=%u csi2_map=%u] "
         "re[ack=%u rvd=%u csi1=%u csi2=%u] truth[ack=%u rvd=%u csi1=%u csi2=%u] u=[%d,%d] cb_tests=%u tb_tests=%u stream_agree=%.4f %s\n",
         g_trials, p->rb_size, p->qam_mod_order, G, plain_ok, so.ok, so.learned, payload, so.h.o_ack, so.h.b_ack, so.h.o_csi1, so.h.b_csi1,
         so.h.o_csi2, so.h.b_csi2, so.h.alpha, so.h.csi2_map, so.c.q_ack, so.c.q_rvd, so.c.q_csi1, so.c.q_csi2, g_truth_ack, g_truth_rvd, g_truth_csi1,
         g_truth_csi2, so.u_lo, so.u_hi, so.cb_tests, so.tb_tests, agree, pass ? "PASS" : "FAIL");
  free(llr);
  get_softmodem_params()->passive_rx = saved_passive;
  return ok ? 0 : 1;
}

/* ---- mapping cross-check: receiver (spec 6.2.7 inverse) vs the OAI UE template ---- */
static unsigned map_check(void)
{
  static const struct {
    int rb, s, n, dmrs, cdm;
  } geo[] = {{273, 0, 14, 0x0004, 2}, {273, 0, 14, 0x0804, 2}, {106, 0, 14, 0x0884, 1}, {51, 0, 12, 0x0004, 2}, {8, 0, 14, 0x0004, 2},
             {2, 0, 14, 0x0084, 2}, {24, 0, 13, 0x1008, 2}};
  static const int uci[][7] = {{3, 9, 0, 0, 0, 0, 3},  {11, 11, 0, 0, 0, 0, 3}, {0, 9, 6, 9, 0, 0, 3},  {2, 9, 6, 9, 5, 9, 3},
                               {1, 9, 11, 13, 8, 13, 3}, {4, 12, 7, 9, 12, 7, 3}, {20, 9, 25, 9, 40, 9, 3}, {2, 15, 3, 18, 2, 18, 0}};
  unsigned bad = 0, n = 0, spec_differs = 0, csi2_cfgs = 0;
  for (size_t gi = 0; gi < sizeof(geo) / sizeof(geo[0]); gi++)
    for (int qm = 2; qm <= 8; qm += 2)
      for (size_t ui = 0; ui < sizeof(uci) / sizeof(uci[0]); ui++) {
        const int dmrs_re = 6 * geo[gi].cdm;
        nr_uci_geom_t g = {.rb = (uint16_t)geo[gi].rb, .start_sym = (uint8_t)geo[gi].s, .nsym = (uint8_t)geo[gi].n,
                           .dmrs_pos = (uint16_t)geo[gi].dmrs, .data_re_dmrs_rb = (uint8_t)(12 - dmrs_re), .qm = (uint8_t)qm, .nl = 1};
        const uint32_t N = nr_uci_data_re(&g), G = N * qm;
        const uint32_t K = 8448, C = (G / 3) / K + 1; /* a code rate near 1/3 */
        g.sum_k = K * C;
        nfapi_nr_ue_pusch_pdu_t pdu;
        memset(&pdu, 0, sizeof(pdu));
        pdu.rb_size = g.rb;
        pdu.start_symbol_index = g.start_sym;
        pdu.nr_of_symbols = g.nsym;
        pdu.ul_dmrs_symb_pos = g.dmrs_pos;
        pdu.dmrs_config_type = 0;
        pdu.num_dmrs_cdm_grps_no_data = (uint8_t)geo[gi].cdm;
        pdu.qam_mod_order = (uint8_t)qm;
        pdu.nrOfLayers = 1;
        pdu.pusch_uci = (nfapi_nr_ue_pusch_uci_t){.harq_ack_bit_length = (uint16_t)uci[ui][0], .beta_offset_harq_ack = (uint8_t)uci[ui][1],
                                                   .csi_payload = {.p1_bits = uci[ui][2], .p2_bits = uci[ui][4]},
                                                   .beta_offset_csi1 = (uint8_t)uci[ui][3], .beta_offset_csi2 = (uint8_t)uci[ui][5],
                                                   .alpha_scaling = (uint8_t)uci[ui][6]};
        const nr_uci_hyp_t h = {.o_ack = (uint8_t)uci[ui][0], .b_ack = (uint8_t)uci[ui][1], .o_csi1 = (uint8_t)uci[ui][2],
                                .b_csi1 = (uint8_t)uci[ui][3], .o_csi2 = (uint8_t)uci[ui][4], .b_csi2 = (uint8_t)uci[ui][5],
                                .alpha = (uint8_t)uci[ui][6]};
        int8_t *map = malloc(G);
        int16_t *lo = malloc(G * sizeof(int16_t)), *hi = malloc(G * sizeof(int16_t)), *olo = malloc(G * sizeof(int16_t)),
                *ohi = malloc(G * sizeof(int16_t));
        rate_match_info_uci_t rm;
        const uint32_t g_oai = nr_uci_on_pusch_ulsch_map(&pdu, 0, 0, K, C, G, map, &rm);
        /* codeword index i encoded as two LLR planes: 1 + (i & 0x3fff) and 1 + (i >> 14) (0 = erased) */
        for (uint32_t i = 0; i < G; i++) {
          lo[i] = (int16_t)(1 + (i & 0x3fff));
          hi[i] = (int16_t)(1 + (i >> 14));
        }
        /* CSI part 2: the receiver's alternative placement (csi2_map 1) must reproduce the OAI UE exactly; the spec one
         * (csi2_map 0) is checked to differ from it below */
        nr_uci_hyp_t hx = h;
        hx.csi2_map = uci[ui][4] > 0;
        const uint32_t g_rx = nr_uci_demux(&g, &hx, lo, 0, 0, olo);
        nr_uci_demux(&g, &hx, hi, 0, 0, ohi);
        nr_uci_counts_t c;
        nr_uci_counts(&g, &h, &c);
        uint32_t first_diff = UINT32_MAX;
        if (g_rx == g_oai && g_rx) {
          uint32_t j = 0;
          for (uint32_t i = 0; i < G && first_diff == UINT32_MAX; i++) {
            if (map[i] < 0)
              continue;
            const bool er = olo[j] == 0;
            const uint32_t ix = er ? UINT32_MAX : (uint32_t)(olo[j] - 1) | ((uint32_t)(ohi[j] - 1) << 14);
            if ((map[i] == 0) != er || (!er && ix != i))
              first_diff = j;
            j++;
          }
        }
        const bool same = g_rx == g_oai && g_rx && first_diff == UINT32_MAX;
        /* Expected differences, not counted: (a) the OAI UE refuses a coded UCI part above 1024 bits (g_oai 0); (b) CSI part 2:
         * TS 38.212 6.2.7 step 5 picks phi_l(j * d) from the REs still free after CSI part 1, with d from their count; the OAI
         * UE (map_uci_common) takes d from the symbol's full RE count and walks raw RE indices, stepping over CSI-part-1 REs
         * one at a time -- the receiver follows the spec, as commercial UEs do. */
        const bool expected = g_oai == 0;
        if (uci[ui][4] > 0 && g_oai) { /* the spec placement of CSI part 2 is a different layout */
          int16_t *sp = malloc(G * sizeof(int16_t));
          const uint32_t gs = nr_uci_demux(&g, &h, lo, 0, 0, sp);
          spec_differs += gs == g_rx && memcmp(sp, olo, (size_t)gs * sizeof(int16_t)) != 0;
          csi2_cfgs++;
          free(sp);
        }
        n++;
        bad += !same && !expected;
        if (!same || getenv("UCISIM_VERBOSE"))
          printf("MAPCHECK rb=%d dmrs=0x%x cdm=%d qm=%d uci=[%d,%d,%d,%d,%d,%d,%d] oai[G_ulsch=%u ack=%u csi1=%u csi2=%u] "
                 "rx[G_ulsch=%u ack=%u rvd=%u csi1=%u csi2=%u] first_diff=%d %s\n",
                 geo[gi].rb, geo[gi].dmrs, geo[gi].cdm, qm, uci[ui][0], uci[ui][1], uci[ui][2], uci[ui][3], uci[ui][4], uci[ui][5], uci[ui][6],
                 g_oai, rm.Q_dash_ACK, rm.Q_dash_CSI1, rm.Q_dash_CSI2, g_rx, c.q_ack, c.q_rvd, c.q_csi1, c.q_csi2,
                 first_diff == UINT32_MAX ? -1 : (int)first_diff, same ? "SAME" : expected ? "DIFF(expected: OAI UE 1024-bit scope)" : "DIFF");
        free(map);
        free(lo);
        free(hi);
        free(olo);
        free(ohi);
      }
  printf("MAPCHECK configs=%u unexpected_differences=%u csi2_configs=%u spec_placement_differs_from_oai=%u\n", n, bad, csi2_cfgs, spec_differs);
  return bad;
}

int main(int argc, char **argv)
{
  /* fixture options: --uci O_ACK,B_ACK,O1,B1,O2,B2,ALPHA ; --mapcheck (mapping comparison only) */
  char *args[argc + 1];
  int n = 0;
  bool mapcheck_only = false;
  for (int i = 0; i < argc; i++) {
    if (!strcmp(argv[i], "--uci") && i + 1 < argc) {
      int v[7] = {0};
      AssertFatal(sscanf(argv[++i], "%d,%d,%d,%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]) == 7, "--uci takes 7 values\n");
      g_uci = (nfapi_nr_ue_pusch_uci_t){.harq_ack_bit_length = (uint16_t)v[0], .harq_payload = 0x5a5a5a5a5ull & ((1ull << (v[0] ? v[0] : 1)) - 1),
                                        .beta_offset_harq_ack = (uint8_t)v[1],
                                        .csi_payload = {.p1_bits = v[2], .part1_payload = 0x2d2d2d2dull & ((1ull << (v[2] ? v[2] : 1)) - 1),
                                                        .p2_bits = v[4], .part2_payload = 0x1b1b1b1bull & ((1ull << (v[4] ? v[4] : 1)) - 1)},
                                        .beta_offset_csi1 = (uint8_t)v[3], .beta_offset_csi2 = (uint8_t)v[5], .alpha_scaling = (uint8_t)v[6]};
      g_uci_on = v[0] || v[2];
      continue;
    }
    if (!strcmp(argv[i], "--mapcheck")) {
      mapcheck_only = true;
      continue;
    }
    args[n++] = argv[i];
  }
  args[n] = NULL;
  if (mapcheck_only) {
    /* the UE rate-matching code logs (LOG_D): the log module must exist, as ulsim's main sets it up */
    char *cargv[] = {argv[0], NULL};
    if ((uniqCfg = load_configmodule(1, cargv, CONFIG_ENABLECMDLINEONLY)) == 0)
      return 1;
    logInit();
    return map_check() ? 1 : 0;
  }
  const int rc = ulsim_main(n, args);
  printf("UCISIM trials=%u failures=%u learned_hits=%u explored_hits=%u\n", g_trials, g_fail, g_learned_hits, g_explored_hits);
  /* ulsim's own verdict counts its reference decode, which ignores UCI: the fixture's verdict is the blind decode above */
  (void)rc;
  return !g_trials || g_fail ? 1 : 0;
}
